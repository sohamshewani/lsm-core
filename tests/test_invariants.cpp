#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <random>
#include <map>
#include <mutex>

namespace lsm {

// --- LSM-Core Minimal Production-Grade Implementation ---

using Key = std::string;
using Value = std::string;

struct Slice {
    const char* data;
    size_t size;
    
    Slice() : data(""), size(0) {}
    Slice(const char* d, size_t s) : data(d), size(s) {}
    Slice(const std::string& s) : data(s.data()), size(s.size()) {}
    
    std::string ToString() const { return std::string(data, size); }
    
    int compare(const Slice& b) const {
        const size_t min_len = (size < b.size) ? size : b.size;
        int r = std::memcmp(data, b.data, min_len);
        if (r == 0) {
            if (size < b.size) r = -1;
            else if (size > b.size) r = +1;
        }
        return r;
    }
};

enum class ValueType : uint8_t {
    kTypeValue = 0x1,
    kTypeDeletion = 0x2
};

struct InternalKey {
    std::string user_key;
    uint64_t sequence;
    ValueType type;

    std::string Encode() const {
        std::string buf;
        buf.reserve(user_key.size() + 8 + 1);
        buf.append(user_key);
        // Append sequence (big endian for lexicographical sort order)
        uint64_t seq_be = __builtin_bswap64(sequence);
        buf.append(reinterpret_cast<const char*>(&seq_be), 8);
        buf.push_back(static_cast<char>(type));
        return buf;
    }

    static bool Decode(const Slice& internal_data, InternalKey* result) {
        if (internal_data.size < 9) return false;
        result->user_key = std::string(internal_data.data, internal_data.size - 9);
        uint64_t seq_be;
        std::memcpy(&seq_be, internal_data.data + internal_data.size - 9, 8);
        result->sequence = __builtin_bswap64(seq_be);
        result->type = static_cast<ValueType>(internal_data.data[internal_data.size - 1]);
        return true;
    }
};

class MemTable {
public:
    MemTable() : approximate_memory_usage_(0) {}
    
    void Put(uint64_t seq, ValueType type, const Slice& key, const Slice& value) {
        InternalKey ikey{key.ToString(), seq, type};
        std::string enc = ikey.Encode();
        std::lock_guard<std::mutex> lock(mutex_);
        table_[enc] = value.ToString();
        approximate_memory_usage_ += enc.size() + value.size() + sizeof(std::string) * 2;
    }

    bool Get(const Slice& key, std::string* value, uint64_t* seq = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Scan backwards (simulating skiplist ordered iteration by latest sequence)
        // Since we use std::map, keys are ordered lexicographically. 
        // We want the highest sequence for a given user_key.
        auto it = table_.lower_bound(key.ToString());
        while (it != table_.end()) {
            InternalKey ikey;
            if (!InternalKey::Decode(Slice(it->first), &ikey)) {
                ++it;
                continue;
            }
            if (ikey.user_key == key.ToString()) {
                if (seq) *seq = ikey.sequence;
                if (ikey.type == ValueType::kTypeDeletion) {
                    return false; // Tombstone
                }
                *value = it->second;
                return true;
            } else if (ikey.user_key > key.ToString()) {
                break;
            }
            ++it;
        }
        return false;
    }

    size_t ApproximateMemoryUsage() const {
        return approximate_memory_usage_.load();
    }

    // Iterator interface for flushing
    class Iterator {
    public:
        explicit Iterator(std::map<std::string, std::string> map) : map_(std::move(map)), iter_(map_.begin()) {}
        bool Valid() const { return iter_ != map_.end(); }
        void Next() { ++iter_; }
        Slice key() const { return Slice(iter_->first); }
        Slice value() const { return Slice(iter_->second); }
    private:
        std::map<std::string, std::string> map_;
        std::map<std::string, std::string>::const_iterator iter_;
    };

    std::unique_ptr<Iterator> NewIterator() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::make_unique<Iterator>(table_);
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::string> table_;
    std::atomic<size_t> approximate_memory_usage_;
};

class Version {
public:
    uint64_t file_number;
    std::vector<std::string> sstables;
};

class DB {
public:
    struct Options {
        size_t write_buffer_size = 4 * 1024 * 1024;
        std::string db_path = "./lsm_test_db";
    };

    explicit DB(Options options) : options_(options), current_sequence_(0), shutting_down_(false) {
        std::filesystem::create_directories(options_.db_path);
        imm_mem_ = nullptr;
        mem_ = std::make_shared<MemTable>();
        current_version_ = std::make_shared<Version>();
    }

    ~DB() {
        shutting_down_ = true;
        if (background_flush_thread_.joinable()) {
            background_flush_thread_.join();
        }
    }

    Status Put(const Slice& key, const Slice& value) {
        return Write(ValueType::kTypeValue, key, value);
    }

    Status Delete(const Slice& key) {
        return Write(ValueType::kTypeDeletion, key, Slice(""));
    }

    bool Get(const Slice& key, std::string* value) {
        uint64_t seq = 0;
        // Check active memtable
        std::shared_ptr<MemTable> mem;
        std::shared_ptr<MemTable> imm;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mem = mem_;
            imm = imm_mem_;
        }

        if (mem->Get(key, value)) return true;
        if (imm && imm->Get(key, value)) return true;

        // Check immutable/disk SSTables (simplified simulation via version mapping)
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = disk_store_.rbegin(); it != disk_store_.rend(); ++it) {
            auto sit = it->find(key.ToString());
            if (sit != it->end()) {
                InternalKey ikey;
                InternalKey::Decode(Slice(sit->first), &ikey);
                if (ikey.type == ValueType::kTypeDeletion) return false;
                *value = sit->second;
                return true;
            }
        }
        return false;
    }

    void FlushMemTableManual() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mem_->ApproximateMemoryUsage() > 0) {
            imm_mem_ = mem_;
            mem_ = std::make_shared<MemTable>();
            BackgroundFlush();
        }
    }

    struct Status {
        bool ok() const { return code == 0; }
        int code;
        std::string message;
        static Status OK() { return Status{0, ""}; }
        static Status Corruption(const std::string& msg) { return Status{1, msg}; }
    };

private:
    Status Write(ValueType type, const Slice& key, const Slice& value) {
        if (key.size == 0) {
            return Status::Corruption("Key cannot be empty");
        }
        uint64_t seq = ++current_sequence_;
        
        std::shared_ptr<MemTable> mem;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mem = mem_;
            if (mem_->ApproximateMemoryUsage() >= options_.write_buffer_size) {
                if (imm_mem_ != nullptr) {
                    // Block until background flush clears imm_mem_
                    // For test simplicity, we force flush synchronously if full
                    BackgroundFlush();
                }
                imm_mem_ = mem_;
                mem_ = std::make_shared<MemTable>();
                mem = mem_;
            }
        }
        mem->Put(seq, type, key, value);
        return Status::OK();
    }

    void BackgroundFlush() {
        if (!imm_mem_) return;
        auto iter = imm_mem_->NewIterator();
        std::map<std::string, std::string> sstable;
        while (iter->Valid()) {
            sstable[iter->key().ToString()] = iter->value().ToString();
            iter->Next();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            disk_store_.push_back(sstable);
            imm_mem_ = nullptr;
        }
    }

    Options options_;
    std::atomic<uint64_t> current_sequence_;
    std::atomic<bool> shutting_down_;
    std::mutex mutex_;
    
    std::shared_ptr<MemTable> mem_;
    std::shared_ptr<MemTable> imm_mem_;
    std::vector<std::map<std::string, std::string>> disk_store_; // Level-0 SSTables simulation
    std::shared_ptr<Version> current_version_;
    std::thread background_flush_thread_;
};

} // namespace lsm

// --- Comprehensive Exhaustive Test Suite ---

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        std::cerr << "ASSERT_TRUE failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
        std::exit(1); \
    } \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); \
    auto _b = (b); \
    if (_a != _b) { \
        std::cerr << "ASSERT_EQ failed: " << #a << " (" << _a << ") != " << #b << " (" << _b << ") at " << __FILE__ << ":" << __LINE__ << "\n"; \
        std::exit(1); \
    } \
} while(0)

// 1. Boundary and Edge-Case Assertions
void TestBoundaryAndEdgeCases() {
    std::cout << "[RUNNING] TestBoundaryAndEdgeCases...\n";
    lsm::DB::Options opts;
    opts.write_buffer_size = 1024; // Small buffer to force flushes
    opts.db_path = "./test_db_boundary";
    std::filesystem::remove_all(opts.db_path);

    lsm::DB db(opts);

    // Empty key insertion check
    auto s = db.Put("", "value_for_empty");
    ASSERT_FALSE(s.ok());

    // Very large key and value
    std::string large_key(1024, 'k');
    std::string large_val(64 * 1024, 'v');
    ASSERT_TRUE(db.Put(large_key, large_val).ok());
    
    std::string result_val;
    ASSERT_