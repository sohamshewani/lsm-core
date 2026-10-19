#ifndef INCLUDE_LSM_TYPES_HPP
#define INCLUDE_LSM_TYPES_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <chrono>
#include <array>
#include <system_error>
#include <algorithm>
#include <stdexcept>

namespace lsm {

// -----------------------------------------------------------------------------
// Constants and Magic Numbers
// -----------------------------------------------------------------------------
constexpr uint64_t kSSTableMagicNumber = 0x4C534D5353545256ULL; // "LSMSSTRV"
constexpr uint32_t kWALBlockSize      = 32768;                 // 32 KB fixed blocks
constexpr size_t   kCacheLineSize     = 64;                    // Cache line alignment

// -----------------------------------------------------------------------------
// Enumerations
// -----------------------------------------------------------------------------
enum class ValueType : uint8_t {
    kDeletion = 0x0,
    kValue    = 0x1
};

enum class RecordType : uint8_t {
    kZeroType     = 0,
    kFullType     = 1,
    kFirstType    = 2,
    kMiddleType   = 3,
    kLastType     = 4
};

enum class StatusCode : int {
    kOk = 0,
    kNotFound,
    kCorruption,
    kNotSupported,
    kInvalidArgument,
    kIOError,
    kFull,
    kBusy
};

// -----------------------------------------------------------------------------
// Error Handling Category and Status
// -----------------------------------------------------------------------------
class LSMErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "lsm-core";
    }

    std::string message(int ev) const override {
        switch (static_cast<StatusCode>(ev)) {
            case StatusCode::kOk:             return "Success";
            case StatusCode::kNotFound:       return "Not Found";
            case StatusCode::kCorruption:     return "Data Corruption Detected";
            case StatusCode::kNotSupported:   return "Operation Not Supported";
            case StatusCode::kInvalidArgument:return "Invalid Argument";
            case StatusCode::kIOError:        return "I/O Error";
            case StatusCode::kFull:           return "Resource Full / Arena Exhausted";
            case StatusCode::kBusy:           return "Resource Busy / Latch Contention";
            default:                          return "Unknown LSM Error";
        }
    }
};

inline const std::error_category& lsm_error_category() noexcept {
    static LSMErrorCategory instance;
    return instance;
}

inline std::error_code make_error_code(StatusCode sc) noexcept {
    return std::error_code(static_cast<int>(sc), lsm_error_category());
}

} // namespace lsm

namespace std {
template <>
struct is_error_code_enum<lsm::StatusCode> : std::true_type {};
} // namespace std

namespace lsm {

// -----------------------------------------------------------------------------
// Internal Key (User Key + Sequence Number + Value Type)
// Ordering: Strictly sorted lexicographically by user key (ascending), 
// then descending by sequence number.
// -----------------------------------------------------------------------------
class InternalKey {
public:
    InternalKey() : seq_num_(0), type_(ValueType::kDeletion) {}

    InternalKey(std::string_view user_key, uint64_t seq_num, ValueType type)
        : user_key_(user_key), seq_num_(seq_num), type_(type) {}

    ~InternalKey() = default;

    const std::string& user_key() const noexcept { return user_key_; }
    uint64_t sequence_number() const noexcept { return seq_num_; }
    ValueType type() const noexcept { return type_; }

    void set_user_key(std::string_view key) { user_key_.assign(key.data(), key.size()); }
    void set_sequence_number(uint64_t seq) noexcept { seq_num_ = seq; }
    void set_type(ValueType type) noexcept { type_ = type; }

    size_t serialized_size() const noexcept {
        return user_key_.size() + sizeof(uint64_t) + sizeof(uint8_t);
    }

    void encode(std::vector<uint8_t>& dst) const {
        uint32_t key_len = static_cast<uint32_t>(user_key_.size());
        dst.resize(dst.size() + sizeof(uint32_t) + key_len + sizeof(uint64_t) + sizeof(uint8_t));
        uint8_t* ptr = dst.data() + dst.size() - (sizeof(uint32_t) + key_len + sizeof(uint64_t) + sizeof(uint8_t));
        
        std::memcpy(ptr, &key_len, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        std::memcpy(ptr, user_key_.data(), key_len);
        ptr += key_len;
        
        // Pack sequence number (56 bits) and type (8 bits) into a single 64-bit word
        uint64_t packed = (seq_num_ << 8) | (static_cast<uint64_t>(type_) & 0xFF);
        std::memcpy(ptr, &packed, sizeof(uint64_t));
    }

    bool decode(const char* data, size_t size, size_t& bytes_consumed) {
        if (size < sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint8_t)) {
            return false;
        }
        uint32_t key_len = 0;
        std::memcpy(&key_len, data, sizeof(uint32_t));
        
        if (size < sizeof(uint32_t) + key_len + sizeof(uint64_t)) {
            return false;
        }
        
        user_key_.assign(data + sizeof(uint32_t), key_len);
        
        uint64_t packed = 0;
        std::memcpy(&packed, data + sizeof(uint32_t) + key_len, sizeof(uint64_t));
        
        seq_num_ = packed >> 8;
        type_ = static_cast<ValueType>(packed & 0xFF);
        
        bytes_consumed = sizeof(uint32_t) + key_len + sizeof(uint64_t);
        return true;
    }

    int compare(const InternalKey& other) const noexcept {
        int cmp = user_key_.compare(other.user_key_);
        if (cmp != 0) {
            return cmp;
        }
        // Descending order by sequence number
        if (seq_num_ > other.seq_num_) {
            return -1;
        } else if (seq_num_ < other.seq_num_) {
            return 1;
        }
        // If user key and sequence number match, compare types
        if (type_ == other.type_) {
            return 0;
        }
        return (static_cast<uint8_t>(type_) > static_cast<uint8_t>(other.type_)) ? -1 : 1;
    }

    bool operator<(const InternalKey& other) const noexcept {
        return compare(other) < 0;
    }

    bool operator==(const InternalKey& other) const noexcept {
        return user_key_ == other.user_key_ && seq_num_ == other.seq_num_ && type_ == other.type_;
    }

private:
    std::string user_key_;
    uint64_t    seq_num_;
    ValueType   type_;
};

// -----------------------------------------------------------------------------
// Slice Abstraction for Zero-Copy Data Handling
// -----------------------------------------------------------------------------
class Slice {
public:
    Slice() noexcept : data_(""), size_(0) {}
    Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}
    Slice(std::string_view sv) noexcept : data_(sv.data()), size_(sv.size()) {}
    Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}

    ~Slice() = default;

    const char* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    char operator[](size_t n) const {
        return data_[n];
    }

    void clear() noexcept {
        data_ = "";
        size_ = 0;
    }

    void remove_prefix(size_t n) {
        if (n > size_) {
            n = size_;
        }
        data_ += n;
        size_ -= n;
    }

    std::string toString() const {
        return std::string(data_, size_);
    }

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

    bool operator!=(const Slice& y) const noexcept {
        return !(*this == y);
    }

    bool operator<(const Slice& y) const noexcept {
        return compare(y) < 0;
    }

private:
    const char* data_;
    size_t      size_;
};

// -----------------------------------------------------------------------------
// Read/Write Options & Statistics
// -----------------------------------------------------------------------------
struct ReadOptions {
    bool verify_checksums = false;
    bool fill_cache = true;
};

struct WriteOptions {
    bool sync = true; // Durability guarantee: fsync to non-volatile storage
};

struct LSMStats {
    uint64_t num_keys = 0;
    uint64_t memtable_bytes_used = 0;
    uint64_t wal_bytes_written = 0;
    uint64_t sstable_reads = 0;
    uint64_t compactions_completed = 0;
};

// -----------------------------------------------------------------------------
// Block Handle for SSTable Meta Indexing
// -----------------------------------------------------------------------------
struct BlockHandle {
    uint64_t offset = 0;
    uint64_t size = 0;

    size_t serialized_size() const noexcept {
        return sizeof(uint64_t) * 2;
    }

    void encode(std::vector<uint8_t>& dst) const {
        size_t old_size = dst.size();
        dst.resize(old_size + sizeof(uint64_t) * 2);
        std::memcpy(dst.data() + old_size, &offset, sizeof(uint64_t));
        std::memcpy(dst.data() + old_size + sizeof(uint64_t), &size, sizeof(uint64_t));
    }

    bool decode(const char* data, size_t size_bytes, size_t& bytes_consumed) {
        if (size_bytes < sizeof(uint64_t) * 2) {
            return false;
        }
        std::memcpy(&offset, data, sizeof(uint64_t));
        std::memcpy(&size, data + sizeof(uint64_t), sizeof(uint64_t));
        bytes_consumed = sizeof(uint64_t) * 2;
        return true;
    }
};

// -----------------------------------------------------------------------------
// Footer Structure for SSTables
// -----------------------------------------------------------------------------
struct Footer {
    BlockHandle metaindex_handle;
    BlockHandle index_handle;
    uint64_t    magic_number = kSSTableMagicNumber;

    size_t serialized_size() const noexcept {
        return metaindex_handle.serialized_size() + index_handle.serialized_size() + sizeof(uint64_t);
    }

    void encode(std::vector<uint8_t>& dst) const {
        metaindex_handle.encode(dst);
        index_handle.encode(dst);
        size_t old_size = dst.size();
        dst.resize(old_size + sizeof(uint64_t));
        std::memcpy(dst.data() + old_size, &magic_number, sizeof(uint64_t));
    }

    bool decode(const char* data, size_t size_bytes, size_t& bytes_consumed) {
        size_t consumed = 0;
        size_t total_consumed = 0;
        
        if (!metaindex_handle.decode(data, size_bytes, consumed)) {
            return false;
        }
        total_consumed += consumed;
        
        if (!index_handle.decode(data + total_consumed, size_bytes - total_consumed, consumed)) {
            return false;
        }
        total_consumed += consumed;
        
        if (size_bytes - total_consumed < sizeof(uint64_t)) {
            return false;
        }
        std::memcpy(&magic_number, data + total_consumed, sizeof(uint64_t));
        total_consumed += sizeof(uint64_t);
        
        bytes_consumed = total_consumed;
        return true;
    }
};

} // namespace lsm

#endif // INCLUDE_LSM_TYPES_HPP