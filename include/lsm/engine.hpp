#ifndef LSM_ENGINE_HPP
#define LSM_ENGINE_HPP

#include <string>
#include <vector>
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <system_error>
#include <optional>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <thread>
#include <condition_variable>
#include <queue>
#include <array>

namespace lsm {

// ============================================================================
// ERROR HANDLING & STATUS
// ============================================================================

enum class StatusCode : uint8_t {
    Ok = 0,
    NotFound = 1,
    Corruption = 2,
    IOError = 3,
    InvalidArgument = 4,
    Busy = 5,
    NotSupported = 6
};

class Status {
public:
    Status() noexcept : code_(StatusCode::Ok), msg_() {}
    Status(StatusCode code, std::string msg) : code_(code), msg_(std::move(msg)) {}

    static Status Ok() { return Status(StatusCode::Ok, ""); }
    static Status NotFound(std::string m) { return Status(StatusCode::NotFound, std::move(m)); }
    static Status Corruption(std::string m) { return Status(StatusCode::Corruption, std::move(m)); }
    static Status IOError(std::string m) { return Status(StatusCode::IOError, std::move(m)); }
    static Status InvalidArgument(std::string m) { return Status(StatusCode::InvalidArgument, std::move(m)); }
    static Status Busy(std::string m) { return Status(StatusCode::Busy, std::move(m)); }

    bool ok() const noexcept { return code_ == StatusCode::Ok; }
    bool is_not_found() const noexcept { return code_ == StatusCode::NotFound; }
    bool is_corruption() const noexcept { return code_ == StatusCode::Corruption; }
    bool is_io_error() const noexcept { return code_ == StatusCode::IOError; }
    
    StatusCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return msg_; }

private:
    StatusCode code_;
    std::string msg_;
};

// ============================================================================
// SLICE & KEY DEFINITIONS
// ============================================================================

class Slice {
public:
    Slice() noexcept : data_(""), size_(0) {}
    Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}
    Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}
    Slice(const char* s) noexcept : data_(s), size_(std::strlen(s)) {}

    const char* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    char operator[](size_t n) const noexcept { return data_[n]; }

    int compare(const Slice& b) const noexcept {
        const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
        int r = std::memcmp(data_, b.data_, min_len);
        if (r == 0) {
            if (size_ < b.size_) r = -1;
            else if (size_ > b.size_) r = +1;
        }
        return r;
    }

    bool operator==(const Slice& y) const noexcept {
        return ((size_ == y.size_) && (std::memcmp(data_, y.data_, size_) == 0));
    }
    bool operator!=(const Slice& y) const noexcept { return !(*this == y); }
    bool operator<(const Slice& y) const noexcept { return compare(y) < 0; }

    std::string to_string() const { return std::string(data_, size_); }

private:
    const char* data_;
    size_t size_;
};

// Internal key structure containing user key, sequence number, and value type
struct InternalKey {
    std::string user_key;
    uint64_t sequence_number;
    uint8_t value_type; // 0 = deletion/tombstone, 1 = value

    std::string encode() const {
        std::string buf;
        buf.reserve(user_key.size() + 8 + 1);
        buf.append(user_key);
        
        // Append sequence number (64-bit big endian or little endian - using little endian native)
        uint64_t seq_type = (sequence_number << 8) | (static_cast<uint64_t>(value_type) & 0xFF);
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<char>((seq_type >> (i * 8)) & 0xFF));
        }
        return buf;
    }

    static bool decode(const Slice& internal_data, InternalKey& out_key) {
        if (internal_data.size() < 9) return false;
        size_t user_len = internal_data.size() - 9;
        out_key.user_key = std::string(internal_data.data(), user_len);
        
        uint64_t seq_type = 0;
        const char* ptr = internal_data.data() + user_len;
        for (int i = 0; i < 8; ++i) {
            uint64_t b = static_cast<uint64_t>(static_cast<unsigned char>(ptr[i]));
            seq_type |= (b << (i * 8));
        }
        out_key.value_type = static_cast<uint8_t>(seq_type & 0xFF);
        out_key.sequence_number = seq_type >> 8;
        return true;
    }
};

// Comparator implementing: sorted lexicographically by user key, then descending by sequence number.
struct InternalKeyComparator {
    int operator()(const Slice& a, const Slice& b) const noexcept {
        InternalKey ka, kb;
        if (!InternalKey::decode(a, ka) || !InternalKey::decode(b, kb)) {
            return a.compare(b);
        }
        int r = ka.user_key.compare(kb.user_key);
        if (r != 0) return r;
        
        // Descending by sequence number
        if (ka.sequence_number > kb.sequence_number) return -1;
        if (ka.sequence_number < kb.sequence_number) return 1;
        
        // Value type descending
        if (ka.value_type > kb.value_type) return -1;
        if (ka.value_type < kb.value_type) return 1;
        return 0;
    }
};

// ============================================================================
// MEMORY MANAGEMENT: ARENA ALLOCATOR & CACHE ALIGNED MEMTABLE
// ============================================================================

class alignas(64) Arena {
public:
    explicit Arena(size_t block_size = 65536) 
        : block_size_(block_size), alloc_bytes_remaining_(0), total_allocated_(0) {}

    ~Arena() {
        for (auto block : blocks_) {
            ::operator delete[](block);
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    char* allocate(size_t bytes) {
        if (bytes == nullptr_t_size) return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        if (bytes > alloc_bytes_remaining_) {
            return allocate_fallback(bytes);
        }
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        total_allocated_ += bytes;
        return result;
    }

    char* allocate_aligned(size_t bytes, size_t alignment = 64) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Simple aligned allocation within current block or fallback
        size_t current_addr = reinterpret_cast<size_t>(alloc_ptr_);
        size_t aligned_addr = (current_addr + (alignment - 1)) & ~(alignment - 1);
        size_t adjustment = aligned_addr - current_addr;
        size_t total_needed = bytes + adjustment;

        if (total_needed > alloc_bytes_remaining_) {
            // Allocate new block with extra room for alignment
            char* raw = allocate_new_block(bytes + alignment);
            size_t raw_addr = reinterpret_cast<size_t>(raw);
            size_t aligned_raw = (raw_addr + (alignment - 1)) & ~(alignment - 1);
            total_allocated_ += bytes;
            return reinterpret_cast<char*>(aligned_raw);
        }

        char* result = reinterpret_cast<char*>(aligned_addr);
        alloc_ptr_ = result + bytes;
        alloc_bytes_remaining_ -= total_needed;
        total_allocated_ += bytes;
        return result;
    }

    size_t memory_usage() const noexcept {
        return total_allocated_.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t nullptr_t_size = 0;

    char* allocate_fallback(size_t bytes) {
        if (bytes > block_size_ / 4) {
            // Large object gets its own block
            char* result = allocate_new_block(bytes);
            total_allocated_ += bytes;
            return result;
        }
        alloc_ptr_ = allocate_new_block(block_size_);
        alloc_bytes_remaining_ = block_size_;
        
        char* result = alloc_ptr_;
        alloc_ptr_ += bytes;
        alloc_bytes_remaining_ -= bytes;
        total_allocated_ += bytes;
        return result;
    }

    char* allocate_new_block(size_t block_bytes) {
        char* block = new char[block_bytes];
        blocks_.push_back(block);
        return block;
    }

    size_t block_size_;
    char* alloc_ptr_{nullptr};
    size_t alloc_bytes_remaining_{0};
    std::atomic<size_t> total_allocated_;
    std::vector<char*> blocks_;
    std::mutex mutex_;
};

// Lock-free MPSC / Concurrent SkipList Node aligned to 64 bytes
struct alignas(64) SkipListNode {
    Slice key;
    Slice value;
    std::atomic<SkipListNode*> next[1]; // Flexible array member emulation

    static SkipListNode* create(Arena& arena, const Slice& k, const Slice& v, int height) {
        size_t bytes = sizeof(SkipListNode) + sizeof(std::atomic<SkipListNode*>)*(height - 1);
        char* mem = arena.allocate_aligned(bytes, 64);
        auto node = new (mem) SkipListNode();
        node->key = k;
        node->value = v;
        for (int i = 0; i < height; ++i) {
            new (&node->next[i]) std::atomic<SkipListNode*>(nullptr);
        }
        return node;
    }
private:
    SkipListNode() = default;
};

class MemTable {
public:
    explicit MemTable(size_t arena_block_size = 65536) 
        : arena_(arena_block_size), head_(SkipListNode::create(arena_, Slice(), Slice(), kMaxHeight)),
          max_height_(1), approx_memory_usage_(0) {
    }

    ~MemTable() = default;

    bool insert(const Slice& key, const Slice& value) {
        // Simple thread-safe randomized skip-list insertion using CAS loops
        int height = random_height();
        update_max_height(height);

        std::vector<SkipListNode*> update_nodes(kMaxHeight, nullptr);
        SkipListNode* x = head_.load(std::memory_order_relaxed);

        for (int i = max_height_.load(std::memory_order_relaxed) - 1; i >= 0; --i) {
            SkipListNode* next = x->next[i].load(std::memory_order_relaxed);
            while (next != nullptr && InternalKeyComparator()(next->key, key) < 0) {
                x = next;
                next = x->next[i].load(std::memory_order_relaxed);
            }
            update_nodes[i] = x;
        }

        // Allocate new node
        // Copy key/value data into arena
        char* key_mem = arena_.allocate(key.size());
        std::memcpy(key_mem, key.data(), key.size());
        Slice key_copy(key_mem, key.size());

        char* val_mem = arena_.allocate(value.size());
        std::memcpy(val_mem, value.data(), value.size());
        Slice val_copy(val_mem, value.size());

        SkipListNode* new_node = SkipListNode::create(arena_, key_copy, val_copy, height);

        while (true) {
            for (int i = 0; i < height; ++i) {
                new_node->next[i].store(update_nodes[i]->next[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            
            // CAS on bottom level
            if (update_nodes[0]->next[0].compare_exchange_weak(
                new_node->next[0], new_node,
                std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
            // If CAS failed, re-traverse locally for conflict resolution
            x = head_.load(std::memory_order_relaxed);
            for (int i = max_height_.load(std::memory_order_relaxed) - 1; i >= 0; --i) {
                SkipListNode* next = x->next[i].load(std::memory_order_relaxed);
                while (next != nullptr && InternalKeyComparator()(next->key, key) < 0) {
                    x = next;
                    next = x->next[i].load(std::memory_order_relaxed);
                }
                update_nodes[i] = x;
            }
        }

        approx_memory_usage_.fetch_add(key.size() + value.size() + sizeof(SkipListNode), std::memory_order_relaxed);
        return true;
    }

    bool get(const Slice& key, std::string& val_out) const {
        SkipListNode* x = head_.load(std::memory_order_relaxed);
        for (int i = max_height_.load(std::memory_order_relaxed) - 1; i >= 0; --i) {
            SkipListNode* next = x->next[i].load(std::memory_order_relaxed);
            while (next != nullptr && InternalKeyComparator()(next->key, key) < 0) {
                x = next;
                next = x->next[i].load(std::memory_order_relaxed);
            }
        }
        x = x->next[0].load(std::memory_order_relaxed);
        if (x != nullptr && InternalKeyComparator()(x->key, key) == 0) {
            val_out = x->value.to_string();
            return true;
        }
        return false;
    }

    size_t memory_usage() const noexcept {
        return approx_memory_usage_.load(std::memory_order_relaxed) + arena_.memory_usage();
    }

private:
    static constexpr int kMaxHeight = 12;
    static constexpr int kBranching = 4;

    int random_height() {
        static thread_local uint32_t seed = 123456789;
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        int height = 1;
        while (height < kMaxHeight && (seed % kBranching == 0)) {
            height++;
            seed ^= seed << 13;
        }
        return height;
    }

    void update_max_height(int height) {
        int cur = max_height_.load(std::memory_order_relaxed);
        while