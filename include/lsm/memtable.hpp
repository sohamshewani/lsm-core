#ifndef LSI_LSM_MEMTABLE_HPP
#define LSI_LSM_MEMTABLE_HPP

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <optional>
#include <memory>
#include <vector>

namespace lsm {

// Unique sequence number for MVCC and sorting order.
using SequenceNumber = uint64_t;

// Record operation type within the LSM tree.
enum class ValueType : uint8_t {
    kTypeValue = 0x1,
    kTypeDeletion = 0x2
};

// Internal key structure containing user key, sequence number, and value type.
struct InternalKey {
    std::string_view user_key;
    SequenceNumber seq_num;
    ValueType type;

    // Compares two internal keys.
    // Ordering: strictly sorted lexicographically by user key ascending, 
    // then descending by sequence number (higher/newer sequence numbers first).
    int Compare(const InternalKey& other) const {
        int cmp = user_key.compare(other.user_key);
        if (cmp != 0) {
            return cmp;
        }
        if (seq_num > other.seq_num) {
            return -1; // Descending order for sequence numbers
        } else if (seq_num < other.seq_num) {
            return 1;
        }
        if (type == other.type) {
            return 0;
        }
        return (type < other.type) ? 1 : -1;
    }

    bool operator<(const InternalKey& other) const { return Compare(other) < 0; }
    bool operator==(const InternalKey& other) const { return Compare(other) == 0; }
};

// Slice representing binary data payload for keys and values.
class Slice {
public:
    Slice() : data_(nullptr), size_(0) {}
    Slice(const char* d, size_t n) : data_(d), size_(n) {}
    Slice(std::string_view sv) : data_(sv.data()), size_(sv.size()) {}

    const char* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    std::string_view ToStringView() const { return std::string_view(data_, size_); }

private:
    const char* data_;
    size_t size_;
};

// Arena allocator designed to eliminate malloc overhead during high-throughput writes.
// Thread-safe chunk allocation using atomic pointer bumps for allocation.
class Arena {
public:
    explicit Arena(size_t block_size = 4194304) // 4MB default block size
        : block_size_(block_size),
          bytes_allocated_(0) {
        // Allocate initial block
        AllocBlock(block_size_);
    }

    ~Arena() {
        for (char* block : blocks_) {
            std::free(block);
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) noexcept = delete;
    Arena& operator=(Arena&&) noexcept = delete;

    // Allocates aligned memory from the arena. Thread-safe lock-free bump allocation.
    char* Allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
        if (bytes == 0) return nullptr;

        while (true) {
            char* current_alloc = alloc_ptr_.load(std::memory_order_relaxed);
            char* current_limit = alloc_limit_.load(std::memory_order_relaxed);

            // Calculate current offset and alignment padding
            uintptr_t current_address = reinterpret_cast<uintptr_t>(current_alloc);
            size_t padding = (alignment - (current_address % alignment)) % alignment;
            size_t required = bytes + padding;

            if (current_alloc + required <= current_limit) {
                // Try to claim space atomically
                if (alloc_ptr_.compare_exchange_weak(
                        current_alloc, 
                        current_alloc + required, 
                        std::memory_order_relaxed, 
                        std::memory_order_relaxed)) {
                    bytes_allocated_.fetch_add(required, std::memory_order_relaxed);
                    return current_alloc + padding;
                }
                // CAS failed, retry loop
            } else {
                // Current block exhausted, allocate new block thread-safely via mutex block list
                std::lock_guard<std::mutex> lock(mutex_);
                // Re-read under lock
                if (alloc_ptr_.load(std::memory_order_relaxed) == current_alloc) {
                    size_t new_block_size = (bytes_schl(bytes, block_size_));
                    AllocBlock(new_block_size);
                }
            }
        }
    }

    // Returns total memory allocated in bytes by this arena.
    size_t MemoryUsage() const {
        return bytes_allocated_.load(std::memory_order_relaxed);
    }

private:
    struct BlockDeleter {
        void operator()(char* p) const { std::free(p); }
    };

    size_t block_size_;
    std::atomic<size_t> bytes_allocated_;
    std::mutex mutex_;
    std::vector<char*> blocks_;

    std::atomic<char*> alloc_ptr_{nullptr};
    std::atomic<char*> alloc_limit_{nullptr};

    size_t bsz(size_t a, size_t b) { return (a > b) ? a : b; }
    size_t block_schl(size_t requested, size_t base) {
        return (requested > base) ? requested : base;
    }

    void AllocBlock(size_t block_bytes) {
        char* new_block = static_cast<char*>(std::aligned_alloc(64, block_bytes));
        if (!new_block) {
            throw std::bad_alloc();
        }
        blocks_.push_back(new_block);
        alloc_ptr_.store(new_block, std::memory_order_relaxed);
        alloc_limit_.store(new_block + block_bytes, std::memory_order_relaxed);
    }
};

// Lock-free multi-producer, single-consumer SkipList node aligned to 64-byte cache lines.
struct alignas(64) SkipListNode {
    InternalKey key;
    Slice value;
    std::atomic<uint32_t> height{1};
    
    // Flexible array member representing tower pointers. Allocated dynamically based on height.
    std::atomic<SkipListNode*> forward[1];

    static void* operator new(size_t size, Arena* arena, uint32_t height) {
        // Calculate extra size required for height-1 forward pointers
        // The struct already includes 1 pointer in `forward`.
        size_t extra = (height > 1) ? (height - 1) * sizeof(std::atomic<SkipListNode*>) : 0;
        // Ensure 64-byte alignment
        size_t total_size = ((size + extra + 63) / 64) * 64;
        return arena->Allocate(total_size, 64);
    }

    // Placement delete required because of custom operator new
    static void operator delete(void* ptr, Arena* arena, uint32_t height) noexcept {
        // Arena managed memory; no-op.
    }

    static void operator delete(void* ptr) noexcept {
        // Arena managed memory; no-op.
    }
};

// Lock-free SkipList implementation optimized for high-throughput concurrent writes
// and atomic single-consumer flushes.
class SkipList {
public:
    explicit SkipList(Arena* arena)
        : arena_(arena),
          max_height_(1),
          head_(CreateNode(InternalKey{"", 0, ValueType::kTypeValue}, Slice(), kMaxHeight)) {
        for (int i = 0; i < kMaxHeight; ++i) {
            head_->forward[i].store(nullptr, std::memory_order_relaxed);
        }
    }

    ~SkipList() = default;

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;
    SkipList(SkipList&&) noexcept = delete;
    SkipList& operator=(SkipList&&) noexcept = delete;

    // Inserts a key-value pair into the skip list lock-free using CAS loops.
    void Insert(const InternalKey& key, const Slice& value) {
        SkipListNode* update[kMaxHeight];
        SkipListNode* x = FindGreaterOrEqual(key, update);

        // If exact key match exists, overwrite or insert new version depending on semantics.
        // For LSM memtable, we insert multiple sequence versions for the same user key.
        
        int height = RandomHeight();
        int cur_max_height = max_height_.load(std::memory_order_relaxed);

        while (height > cur_max_height) {
            if (max_height_.compare_exchange_weak(cur_max_height, height,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                cur_max_height = height;
                break;
            }
        }

        SkipListNode* new_node = CreateNode(key, value, height);

        for (int i = 0; i < height; ++i) {
            while (true) {
                SkipListNode* next = update[i]->forward[i].load(std::memory_order_relaxed);
                new_node->forward[i].store(next, std::memory_order_relaxed);
                if (update[i]->forward[i].compare_exchange_weak(
                        next, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    break;
                }
                // If CAS fails, re-evaluate predecessors for this level
                FindSplice(key, i, update);
            }
        }
    }

    // Searches for a key in the SkipList. Returns true and populates value if found.
    bool Lookup(const InternalKey& target, Slice* value) const {
        SkipListNode* x = head_;
        int level = max_height_.load(std::memory_order_relaxed) - 1;
        while (true) {
            SkipListNode* next = x->forward[level].load(std::memory_order_acquire);
            if (next != nullptr && next->key.Compare(target) < 0) {
                x = next;
            } else {
                if (next != nullptr && next->key.Compare(target) == 0) {
                    *value = next->value;
                    return true;
                }
                if (level == 0) {
                    // Check base level exact match
                    if (next != nullptr && next->key.Compare(target) == 0) {
                        *value = next->value;
                        return true;
                    }
                    return false;
                }
                level--;
            }
        }
    }

    // Iterator to scan through the SkipList sequentially (Single Consumer thread safe).
    class Iterator {
    public:
        explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

        void SeekToFirst() {
            node_ = list_->head_->forward[0].load(std::memory_order_acquire);
        }

        void Seek(const InternalKey& target) {
            SkipListNode* update[kMaxHeight];
            node_ = list_->FindGreaterOrEqual(target, update);
        }

        bool Valid() const { return node_ != nullptr; }

        void Next() {
            assert(Valid());
            node_ = node_->forward[0].load(std::memory_order_acquire);
        }

        InternalKey GetKey() const {
            assert(Valid());
            return node_->key;
        }

        Slice GetValue() const {
            assert(Valid());
            return node_->value;
        }

    private:
        const SkipList* list_;
        SkipListNode* node_;
    };

private:
    static constexpr int kMaxHeight = 32;
    static constexpr unsigned int kBranching = 4;

    Arena* arena_;
    std::atomic<int> max_height_;
    SkipListNode* head_;

    int RandomHeight() {
        // Thread-safe pseudo-random height generation
        static thread_local uint32_t seed = 0x12345678;
        seed = seed * 1103515245 + 12345;
        int height = 1;
        while (height < kMaxHeight && (seed % kBranching == 0)) {
            height++;
            seed = seed * 1103515245 + 12345;
        }
        return height < kMaxHeight ? height : kMaxHeight;
    }

    SkipListNode* CreateNode(const InternalKey& key, const Slice& value, int height) {
        void* mem = SkipListNode::operator new(sizeof(SkipListNode), arena_, height);
        SkipListNode* node = new (mem) SkipListNode();
        node->key = key;
        
        // Copy value payload into Arena memory to ensure persistence and lifetime
        char* value_copy = arena_->Allocate(value.size());
        std::memcpy(value_copy, value.data(), value.size());
        node->value = Slice(value_copy, value.size());
        
        node->height.store(height, std::memory_order_relaxed);
        for (int i = 0; i < height; ++i) {
            node->forward[i].store(nullptr, std::memory_order_relaxed);
        }
        return node;
    }

    SkipListNode* FindGreaterOrEqual(const InternalKey& key, SkipListNode** update) const {
        SkipListNode* x = head_;
        int level = max_height_.load(std::memory_order_relaxed) - 1;
        for (int i = level; i >= 0; --i) {
            SkipListNode* next = x->forward[i].load(std::memory_order_acquire);
            while (next != nullptr && next->key.Compare(key) < 0) {
                x = next;
                next = x->forward[i].load(std::memory_order_acquire);
            }
            update[i] = x;
        }
        return x->forward[0].load(std::memory_order_acquire);
    }

    void FindSplice(const InternalKey& key, int level, SkipListNode** update) {
        SkipListNode* x = head_;
        for (int i = level; i >= 0; --i) {
            SkipListNode* next = x->forward[i].load(std::memory_order_acquire);
            while (next != nullptr && next->key.Compare(key) < 0) {
                x = next;
                next = x->forward[i].load(std::memory_order_acquire);
            }
            update[i] = x;
        }
    }
};

// MemTable encapsulating Arena, lock-free SkipList, and memory usage accounting.
class MemTable {
public:
    explicit MemTable(size_t arena_block_size = 4194304)
        : arena_(std::make_unique<Arena>(arena_block_size)),
          skip_list_(arena_.get()),
          approximate_memory_usage_(0),
          ref_count_(0) {}

    ~MemTable() = default;

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;
    MemTable(MemTable&&) noexcept = delete;
    MemTable& operator=(MemTable&&) noexcept = delete;

    // Adds a write entry (Put or Delete) to the MemTable.
    void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
        InternalKey internal_key{key.ToStringView(), seq, type};
        
        // Calculate approximate memory overhead for tracking
        size_t estimated_node_size = sizeof(SkipListNode) + key.size() + value.size() + 64;
        approximate_memory_usage_.fetch_add(estimated_node_size, std::memory_order_relaxed);

        skip_list_.Insert(internal_key, value);
    }

    // Looks up a user key at or below a given sequence number.
    bool Get(const Slice& user_key, SequenceNumber max_seq, std::string& value, ValueType* type_out) {
        // Construct lookup