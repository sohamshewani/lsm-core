#ifndef LSM_SSTABLE_HPP
#define LSM_SSTABLE_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <fstream>
#include <shared_mutex>
#include <filesystem>
#include <span>
#include <array>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace lsm {

// Magic number for SSTable footer validation: "LSMSTBL1"
constexpr uint64_t kSSTableMagic = 0x4C534D5354424C31ULL;
constexpr size_t kBlockSize = 4096;
constexpr size_t kFooterSize = 48; // Fixed size for serialization

struct Slice {
    const char* data_{nullptr};
    size_t size_{0};

    Slice() = default;
    Slice(const char* data, size_t size) : data_(data), size_(size) {}
    Slice(std::string_view sv) : data_(sv.data()), size_(sv.size()) {}

    [[nodiscard]] const char* data() const { return data_; }
    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }

    [[nodiscard]] std::string_view toStringView() const {
        return std::string_view(data_, size_);
    }

    [[nodiscard]] int compare(const Slice& b) const {
        const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
        int r = std::memcmp(data_, b.data_, min_len);
        if (r != 0) return r;
        if (size_ < b.size_) return -1;
        if (size_ > b.size_) return 1;
        return 0;
    }

    [[nodiscard]] bool operator==(const Slice& b) const {
        return size_ == b.size_ && std::memcmp(data_, b.data_, size_) == 0;
    }
    [[nodiscard]] bool operator<(const Slice& b) const {
        return compare(b) < 0;
    }
};

struct InternalKey {
    std::string user_key;
    uint64_t sequence_number{0};
    uint8_t value_type{0}; // 0 = deletion, 1 = value

    [[nodiscard]] std::string encode() const {
        std::string buf;
        buf.reserve(user_key.size() + 9);
        buf.append(user_key);
        
        // Append sequence number (8 bytes, little-endian) + value type (1 byte)
        uint64_t seq_type = (sequence_number << 8) | static_cast<uint64_t>(value_type & 0xFF);
        for (int i = 0; i < 8; ++i) {
            buf.push_back(static_cast<char>((seq_type >> (i * 8)) & 0xFF));
        }
        return buf;
    }

    static [[nodiscard]] InternalKey decode(std::string_view encoded) {
        if (encoded.size() < 8) {
            throw std::runtime_error("Malformed InternalKey: too short");
        }
        std::string_view user_k = encoded.substr(0, encoded.size() - 8);
        
        uint64_t seq_type = 0;
        const char* ptr = encoded.data() + encoded.size() - 8;
        for (int i = 0; i < 8; ++i) {
            uint64_t b = static_cast<uint8_t>(ptr[i]);
            seq_type |= (b << (i * 8));
        }
        
        InternalKey key;
        key.user_key = std::string(user_k);
        key.sequence_number = seq_type >> 8;
        key.value_type = static_cast<uint8_t>(seq_type & 0xFF);
        return key;
    }

    // Ordering: strictly sorted lexicographically by user key, then descending by sequence number.
    [[nodiscard]] bool operator<(const InternalKey& other) const {
        int cmp = user_key.compare(other.user_key);
        if (cmp != 0) {
            return cmp < 0;
        }
        return sequence_number > other.sequence_number;
    }
};

class BlockHandle {
private:
    uint64_t offset_{0};
    uint64_t size_{0};

public:
    BlockHandle() = default;
    BlockHandle(uint64_t offset, uint64_t size) : offset_(offset), size_(size) {}

    [[nodiscard]] uint64_t offset() const { return offset_; }
    [[nodiscard]] uint64_t size() const { return size_; }

    void encodeTo(std::string& dst) const {
        // Simple fixed-width serialization: 8 bytes offset, 8 bytes size
        char buf[16];
        std::memcpy(buf, &offset_, 8);
        std::memcpy(buf + 8, &size_, 8);
        dst.append(buf, 16);
    }

    static BlockHandle decodeFrom(std::string_view src, size_t& index) {
        if (index + 16 > src.size()) {
            throw std::runtime_error("Malformed BlockHandle: buffer overrun");
        }
        uint64_t off, sz;
        std::memcpy(&off, src.data() + index, 8);
        std::memcpy(&sz, src.data() + index + 8, 8);
        index += 16;
        return BlockHandle(off, sz);
    }
};

class BloomFilter {
private:
    std::vector<uint8_t> bits_;
    size_t num_hashes_{3};

    // MurmurHash3 32-bit variant
    static uint32_t murmurHash3(const char* data, size_t len, uint32_t seed) {
        const uint32_t c1 = 0xcc9e2d51;
        const uint32_t c2 = 0x1b873593;
        const int nblocks = static_cast<int>(len / 4);
        
        uint32_t h1 = seed;
        const uint32_t* blocks = reinterpret_cast<const uint32_t*>(data);
        for (int i = 0; i < nblocks; ++i) {
            uint32_t k1 = blocks[i];
            k1 *= c1;
            k1 = (k1 << 15) | (k1 >> 17);
            k1 *= c2;
            
            h1 ^= k1;
            h1 = (h1 << 13) | (h1 >> 19);
            h1 = h1 * 5 + 0xe6546b64;
        }

        const uint8_t* tail = reinterpret_cast<const uint8_t*>(data + nblocks * 4);
        uint32_t k1 = 0;
        switch (len & 3) {
            case 3: k1 ^= static_cast<uint32_t>(tail[2]) << 16; [[fallthrough]];
            case 2: k1 ^= static_cast<uint32_t>(tail[1]) << 8; [[fallthrough]];
            case 1: k1 ^= static_cast<uint32_t>(tail[0]);
                    k1 *= c1;
                    k1 = (k1 << 15) | (k1 >> 17);
                    k1 *= c2;
                    h1 ^= k1;
        }

        h1 ^= static_cast<uint32_t>(len);
        h1 ^= h1 >> 16;
        h1 *= 0x85ebca6b;
        h1 ^= h1 >> 13;
        h1 *= 0xc2b2ae35;
        h1 ^= h1 >> 16;
        return h1;
    }

public:
    BloomFilter() = default;
    explicit BloomFilter(size_t total_keys, double false_positive_rate = 0.01) {
        if (total_keys == 0) total_keys = 1;
        // m = - (n * ln(p)) / (ln(2)^2)
        double m = -static_cast<double>(total_keys) * std::log(false_positive_rate) / (0.69314718056 * 0.69314718056);
        size_t bits_size = static_cast<size_t>(std::ceil(m));
        bits_size = (bits_size + 7) & ~7UL; // align to bytes
        bits_.resize(bits_size / 8, 0);
        num_hashes_ = static_cast<size_t>(std::ceil((static_cast<double>(bits_size) / total_keys) * 0.69314718056));
        if (num_hashes_ < 1) num_hashes_ = 1;
        if (num_hashes_ > 30) num_hashes_ = 30;
    }

    void add(std::string_view key) {
        if (bits_.empty()) return;
        uint32_t h1 = murmurHash3(key.data(), key.size(), 0xbc9f1d34);
        uint32_t h2 = murmurHash3(key.data(), key.size(), h1);
        size_t total_bits = bits_.size() * 8;
        for (size_t i = 0; i < num_hashes_; ++i) {
            uint32_t combined = h1 + i * h2;
            size_t bit_pos = combined % total_bits;
            bits_[bit_pos / 8] |= (1 << (bit_pos % 8));
        }
    }

    [[nodiscard]] bool mayContain(std::string_view key) const {
        if (bits_.empty()) return true;
        uint32_t h1 = murmurHash3(key.data(), key.size(), 0xbc9f1d34);
        uint32_t h2 = murmurHash3(key.data(), key.size(), h1);
        size_t total_bits = bits_.size() * 8;
        for (size_t i = 0; i < num_hashes_; ++i) {
            uint32_t combined = h1 + i * h2;
            size_t bit_pos = combined % total_bits;
            if ((bits_[bit_pos / 8] & (1 << (bit_pos % 8))) == 0) {
                return false;
            }
        }
        return true;
    }

    void encodeTo(std::string& dst) const {
        uint32_t num_bytes = static_cast<uint32_t>(bits_.size());
        uint32_t num_h = static_cast<uint32_t>(num_hashes_);
        
        char header[8];
        std::memcpy(header, &num_bytes, 4);
        std::memcpy(header + 4, &num_h, 4);
        dst.append(header, 8);
        if (num_bytes > 0) {
            dst.append(reinterpret_cast<const char*>(bits_.data()), num_bytes);
        }
    }

    static BloomFilter decodeFrom(std::string_view src, size_t& index) {
        if (index + 8 > src.size()) {
            throw std::runtime_error("Malformed BloomFilter header");
        }
        uint32_t num_bytes = 0;
        uint32_t num_h = 0;
        std::memcpy(&num_bytes, src.data() + index, 4);
        std::memcpy(&num_h, src.data() + index + 4, 4);
        index += 8;

        if (index + num_bytes > src.size()) {
            throw std::runtime_error("Malformed BloomFilter data payload");
        }
        BloomFilter bf;
        bf.num_hashes_ = num_h;
        bf.bits_.resize(num_bytes);
        if (num_bytes > 0) {
            std::memcpy(bf.bits_.data(), src.data() + index, num_bytes);
            index += num_bytes;
        }
        return bf;
    }
};

class SSTableBuilder {
private:
    std::string file_path_;
    std::ofstream file_;
    uint64_t current_offset_{0};
    
    std::string current_block_data_;
    std::vector<std::pair<std::string, BlockHandle>> index_entries_;
    std::string last_key_;
    
    std::vector<std::string> all_keys_for_bloom_;
    BlockHandle metaindex_handle_;
    BlockHandle bloom_handle_;
    BlockHandle index_handle_;

    void writeRaw(const char* data, size_t size) {
        file_.write(data, size);
        if (!file_) {
            throw std::runtime_error("Failed to write to SSTable file: " + file_path_);
        }
        current_offset_ += size;
    }

    void flushCurrentBlock() {
        if (current_block_data_.empty()) return;

        uint64_t block_offset = current_offset_;
        uint64_t block_size = current_block_data_.size();
        
        // Append simple CRC32 or checksum placeholder if needed, here raw block data
        writeRaw(current_block_data_.data(), block_size);
        
        BlockHandle handle(block_offset, block_size);
        if (!index_entries_.empty()) {
            index_entries_.emplace_back(last_key_, handle);
        } else {
            // First block
            index_entries_.emplace_back(last_key_, handle);
        }
        current_block_data_.clear();
    }

public:
    explicit SSTableBuilder(std::string file_path) : file_path_(std::move(file_path)) {
        file_.open(file_path_, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file_.is_open()) {
            throw std::runtime_error("Failed to open SSTable for writing: " + file_path_);
        }
    }

    ~SSTableBuilder() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    void add(const InternalKey& key, std::string_view value) {
        std::string encoded_key = key.encode();
        last_key_ = encoded_key;
        all_keys_for_bloom_.push_back(encoded_key);

        // Format in block: [key_len(4)][val_len(4)][key][value]
        uint32_t k_len = static_cast<uint32_t>(encoded_key.size());
        uint32_t v_len = static_cast<uint32_t>(value.size());

        char header[8];
        std::memcpy(header, &k_len, 4);
        std::memcpy(header + 4, &v_len, 4);

        current_block_data_.append(header, 8);
        current_block_data_.append(encoded_key);
        current_block_data_.append(value.data(), value.size());

        if (current_block_data_.size() >= kBlockSize) {
            flushCurrentBlock();
        }
    }

    void finish() {
        // Flush remaining data block
        if (!current_block_data_.empty()) {
            flushCurrentBlock();
        }

        // Build and write Bloom Filter Block
        BloomFilter bf(all_keys_for_bloom_.size());
        for (const auto& k : all_keys_for_bloom_) {
            bf.add(k);
        }
        std::string bf_encoded;
        bf.encodeTo(bf_encoded);