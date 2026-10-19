#ifndef LSM_WAL_HPP
#define LSM_WAL_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <cstring>
#include <zlib.h> // For CRC32 calculation

namespace lsm {

enum class WALRecordType : uint8_t {
    kZero = 0,
    kFullType = 1,
    kFirstType = 2,
    kMiddleType = 3,
    kLastType = 4
};

struct WALHeader {
    uint32_t crc;
    uint32_t length;
    uint8_t type;
} __attribute__((packed));

class WALError : public std::system_error {
public:
    explicit WALError(std::error_code ec) : std::system_error(ec) {}
    explicit WALError(std::error_code ec, const std::string &what) : std::system_error(ec, what) {}
};

class WALWriter {
public:
    static constexpr size_t kBlockSize = 32768; // 32KB fixed-size blocks

    explicit WALWriter(std::string filepath)
        : filepath_(std::move(filepath)), block_offset_(0) {
        file_.open(filepath_, std::ios::out | std::ios::binary | std::ios::app);
        if (!file_.is_open()) {
            throw WALError(std::make_error_code(std::errc::io_error),
                           "Failed to open WAL file for writing: " + filepath_);
        }
    }

    ~WALWriter() {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

    WALWriter(const WALWriter&) = delete;
    WALWriter& operator=(const WALWriter&) = delete;
    WALWriter(WALWriter&&) noexcept = default;
    WALWriter& operator=(WALWriter&&) noexcept = default;

    void Append(const char* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        size_t left = size;
        const char* ptr = data;
        bool first = true;

        while (left > 0) {
            size_t leftover = kBlockSize - block_offset_;
            
            // If the remaining space in the current block is less than the header size,
            // pad the rest of the block with zeros and start a new block.
            if (leftover < sizeof(WALHeader)) {
                if (leftover > 0) {
                    std::vector<char> padding(leftover, 0);
                    file_.write(padding.data(), leftover);
                }
                block_offset_ = 0;
                leftover = kBlockSize;
            }

            size_t avail = leftover - sizeof(WALHeader);
            size_t fragment_length = (left < avail) ? left : avail;
            
            WALRecordType type;
            if (first && (left <= avail)) {
                type = WALRecordType::kFullType;
            } else if (first) {
                type = WALRecordType::kFirstType;
            } else if (left <= avail) {
                type = WALRecordType::kLastType;
            } else {
                type = WALRecordType::kMiddleType;
            }

            // Compute CRC32 over the record type and the payload fragment
            uLong crc = crc32(0L, Z_NULL, 0);
            uint8_t t_byte = static_cast<uint8_t>(type);
            crc = crc32(crc, &t_byte, 1);
            crc = crc32(crc, reinterpret_cast<const Bytef*>(ptr), fragment_length);

            WALHeader header;
            header.crc = static_cast<uint32_t>(crc);
            header.length = static_cast<uint32_t>(fragment_length);
            header.type = t_byte;

            file_.write(reinterpret_cast<const char*>(&header), sizeof(header));
            file_.write(ptr, fragment_length);

            block_offset_ += sizeof(WALHeader) + fragment_length;
            ptr += fragment_length;
            left -= fragment_length;
            first = false;
        }

        file_.flush();
    }

    void Sync() {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.flush();
        if (!file_.good()) {
            throw WALError(std::make_error_code(std::errc::io_error),
                           "Failed to flush/sync WAL file: " + filepath_);
        }
        int fd = fileno(file_);
        if (fd != -1) {
            fsync(fd);
        }
    }

private:
    std::string filepath_;
    std::ofstream file_;
    size_t block_offset_;
    std::mutex mutex_;
};

class WALReader {
public:
    explicit WALReader(std::string filepath)
        : filepath_(std::move(filepath)) {
        file_.open(filepath_, std::ios::in | std::ios::binary);
        if (!file_.is_open()) {
            throw WALError(std::make_error_code(std::errc::no_such_file_or_directory),
                           "Failed to open WAL file for reading: " + filepath_);
        }
    }

    ~WALReader() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    WALReader(const WALReader&) = delete;
    WALReader& operator=(const WALReader&) = delete;

    bool ReadRecord(std::vector<char>& scratch, WALRecordType* type) {
        char header_buf[sizeof(WALHeader)];
        
        // Check current position and handle block boundaries/padding
        size_t file_pos = file_.tellg();
        size_t block_offset = file_pos % WALWriter::kBlockSize;
        
        if (WALWriter::kBlockSize - block_offset < sizeof(WALHeader)) {
            // Skip remaining padding bytes in the block
            file_.seekg(WALWriter::kBlockSize - block_offset, std::ios::cur);
            if (!file_.read(header_buf, sizeof(WALHeader))) {
                return false;
            }
        } else {
            if (!file_.read(header_buf, sizeof(WALHeader))) {
                return false;
            }
            // Check if it's all zeros (padding)
            bool all_zeros = true;
            for (size_t i = 0; i < sizeof(WALHeader); ++i) {
                if (header_buf[i] != 0) {
                    all_zeros = false;
                    break;
                }
            }
            if (all_zeros) {
                // Skip to the next block boundary
                size_t current_pos = file_.tellg();
                size_t cur_offset = (current_pos - sizeof(WALHeader)) % WALWriter::kBlockSize;
                size_t remaining_in_block = WALWriter::kBlockSize - cur_offset;
                file_.seekg(remaining_in_block, std::ios::cur);
                return ReadRecord(scratch, type);
            }
        }

        WALHeader header;
        std::memcpy(&header, header_buf, sizeof(WALHeader));

        scratch.resize(header.length);
        if (!file_.read(scratch.data(), header.length)) {
            // Truncated record at tail, corruption encountered
            return false;
        }

        // Validate CRC32
        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, &header.type, 1);
        crc = crc32(crc, reinterpret_cast<const Bytef*>(scratch.data()), header.length);

        if (header.crc != static_cast<uint32_t>(crc)) {
            // Checksum mismatch, corruption detected
            return false;
        }

        *type = static_cast<WALRecordType>(header.type);
        return true;
    }

private:
    std::string filepath_;
    std::ifstream file_;
};

} // namespace lsm

#endif // LSM_WAL_HPP