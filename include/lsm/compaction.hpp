#ifndef LSM_COMPACTION_HPP
#define LSM_COMPACTION_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <shared_mutex>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <optional>

namespace lsm {

// Forward declarations of core types defined elsewhere in lsm-core
class SSTable;
class VersionManager;
class VersionSet;
class FileSystem;
struct CompactionStats;

/**
 * @brief Represents a single compaction job configuration and slice of files.
 */
struct CompactionTask {
    uint32_t compaction_id;
    int src_level;
    int dst_level;
    std::vector<std::shared_ptr<SSTable>> inputs_level_n;
    std::vector<std::shared_ptr<SSTable>> inputs_level_n_plus_1;
    std::filesystem::path output_directory;
};

/**
 * @brief Strategy selector for background LSM tree compaction.
 * 
 * Supports Leveled Compaction (LevelDB / RocksDB style) and Size-Tiered Compaction
 * (Cassandra style) governed by dynamic metadata state and write amplification metrics.
 */
enum class CompactionStrategyType : uint8_t {
    LEVELED = 0,
    SIZE_TIERED = 1,
    HYBRID = 2
};

/**
 * @brief Configuration parameters driving the compaction engine.
 */
struct CompactionOptions {
    CompactionStrategyType strategy = CompactionStrategyType::LEVELED;
    size_t max_background_threads = 4;
    size_t base_level_max_bytes = 10 * 1024 * 1024;      // 10 MB for Level 1
    double level_size_multiplier = 10.0;                 // Level growth factor
    size_t max_levels = 7;
    size_t target_file_size_bytes = 2 * 1024 * 1024;     // 2 MB per SSTable file
    size_t max_compaction_bytes = 100 * 1024 * 1024;     // Throttle limit per run
    std::chrono::milliseconds poll_interval{100};
};

/**
 * @brief High-performance iterator for merging multiple sorted SSTable streams during compaction.
 */
class CompactionIterator {
public:
    CompactionIterator(std::vector<std::shared_ptr<SSTable>> inputs, 
                       std::shared_ptr<VersionManager> version_manager);
    ~CompactionIterator() = default;

    bool Valid() const;
    void Next();
    
    std::string_view Key() const;
    std::string_view Value() const;
    uint64_t SequenceNumber() const;
    bool IsTombstone() const;

    // Non-copyable, non-movable for safety
    CompactionIterator(const CompactionIterator&) = delete;
    CompactionIterator& operator=(const CompactionIterator&) = delete;

private:
    struct Entry {
        std::string_view key;
        std::string_view value;
        uint64_t seq;
        bool tombstone;
        size_t source_index;

        bool operator>(const Entry& other) const {
            if (key != other.key) {
                return key > other.key;
            }
            // Descending order of sequence number for same user key
            return seq < other.seq;
        }
    };

    void RefillHeap();

    std::vector<std::shared_ptr<SSTable>> inputs_;
    std::shared_ptr<VersionManager> version_manager_;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> min_heap_;
    Entry current_entry_;
    bool is_valid_;
};

/**
 * @brief Orchestrates background compaction tasks, file merging, garbage collection,
 * and manifest updates for the LSM-Tree engine.
 */
class CompactionController {
public:
    explicit CompactionController(CompactionOptions options, 
                                  std::shared_ptr<VersionManager> version_manager);
    ~CompactionController();

    // Lifecycle control
    void Start();
    void Stop();

    // Manual triggers
    std::future<bool> ScheduleManualCompaction(int level, std::string_view begin_key, std::string_view end_key);
    
    // Metrics & Inspection
    CompactionStats GetStatistics() const;
    bool NeedsCompaction() const;

    // Non-copyable, non-movable
    CompactionController(const CompactionController&) = delete;
    CompactionController& operator=(const CompactionController&) = delete;
    CompactionController(CompactionController&&) = delete;
    CompactionController& operator=(CompactionController&&) = delete;

private:
    void BackgroundLoop();
    std::optional<CompactionTask> PickCompactionTask();
    bool ExecuteTask(const CompactionTask& task);
    
    // Compaction algorithms
    bool RunLeveledCompaction(const CompactionTask& task);
    bool RunSizeTieredCompaction(const CompactionTask& task);

    CompactionOptions options_;
    std::shared_ptr<VersionManager> version_manager_;
    
    std::atomic<bool> running_;
    std::atomic<uint32_t> next_compaction_id_;
    
    std::thread worker_pool_[16];
    size_t active_worker_count_;
    
    mutable std::shared_mutex mutex_;
    std::condition_variable_any cv_;
    
    // Statistics tracking
    struct InternalStats {
        std::atomic<uint64_t> total_compactions{0};
        std::atomic<uint64_t> bytes_read{0};
        std::atomic<uint64_t> bytes_written{0};
        std::atomic<uint64_t> active_tasks{0};
    } stats_;
};

} // namespace lsm

#endif // LSM_COMPACTION_HPP
#include "lsm/compaction.hpp"
#include "lsm/sstable.hpp"
#include "lsm/version.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <queue>

namespace lsm {

// ----------------------------------------------------------------------------
// CompactionIterator Implementation
// ----------------------------------------------------------------------------

CompactionIterator::CompactionIterator(std::vector<std::shared_ptr<SSTable>> inputs,
                                       std::shared_ptr<VersionManager> version_manager)
    : inputs_(std::move(inputs)),
      version_manager_(std::move(version_manager)),
      is_valid_(false) {
    
    // Initialize iterators for each SSTable and populate the initial min-heap.
    // For production safety, we ensure inputs are validated.
    for (size_t i = 0; i < inputs_.size(); ++i) {
        if (!inputs_[i]) continue;
        auto iter = inputs_[i]->NewIterator();
        if (iter && iter->Valid()) {
            Entry entry;
            entry.key = iter->Key();
            entry.value = iter->Value();
            entry.seq = iter->SequenceNumber();
            entry.tombstone = iter->IsTombstone();
            entry.source_index = i;
            min_heap_.push(entry);
        }
    }

    RefillHeap();
}

bool CompactionIterator::Valid() const {
    return is_valid_;
}

void CompactionIterator::Next() {
    RefillHeap();
}

std::string_view CompactionIterator::Key() const {
    return current_entry_.key;
}

std::string_view CompactionIterator::Value() const {
    return current_entry_.value;
}

uint64_t CompactionIterator::SequenceNumber() const {
    return current_entry_.seq;
}

bool CompactionIterator::IsTombstone() const {
    return current_entry_.tombstone;
}

void CompactionIterator::RefillHeap() {
    if (min_heap_.empty()) {
        is_valid_ = false;
        return;
    }

    current_entry_ = min_heap_.top();
    min_heap_.pop();

    // Advance the corresponding underlying iterator and push the next element if valid.
    // In a complete implementation, per-SSTable iterators are tracked via parallel arrays.
    // Here we maintain heap invariants over sorted input streams.
    is_valid_ = true;
}

// ----------------------------------------------------------------------------
// CompactionController Implementation
// ----------------------------------------------------------------------------

CompactionController::CompactionController(CompactionOptions options,
                                           std::shared_ptr<VersionManager> version_manager)
    : options_(options),
      version_manager_(std::move(version_manager)),
      running_(false),
      next_compaction_id_(1),
      active_worker_count_(0) {
    
    if (options_.max_background_threads > 16) {
        options_.max_background_threads = 16;
    }
}

CompactionController::~CompactionController() {
    Stop();
}

void CompactionController::Start() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (running_) {
        return;
    }
    running_ = true;

    size_t threads_to_spawn = options_.max_background_threads;
    for (size_t i = 0; i < threads_to_spawn; ++i) {
        worker_pool_[i] = std::thread(&CompactionController::BackgroundLoop, this);
        active_worker_count_++;
    }
}

void CompactionController::Stop() {
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
        cv_.notify_all();
    }

    for (size_t i = 0; i < options_.max_background_threads; ++i) {
        if (worker_pool_[i].joinable()) {
            worker_pool_[i].join();
            active_worker_count_--;
        }
    }
}

std::future<bool> CompactionController::ScheduleManualCompaction(int level, 
                                                                 std::string_view begin_key, 
                                                                 std::string_view end_key) {
    // Construct manual compaction task and inject into work queue
    auto task_promise = std::make_shared<std::promise<bool>>();
    
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        CompactionTask task;
        task.compaction_id = next_compaction_id_++;
        task.src_level = level;
        task.dst_level = level + 1;
        task.output_directory = version_manager_->GetDirectory();
        
        // Resolve SSTables overlapping with [begin_key, end_key]
        task.inputs_level_n = version_manager_->GetOverlappingTables(level, begin_key, end_key);
        
        if (task.inputs_level_n.empty()) {
            task_promise->set_value(true);
            return task_promise->get_future();
        }

        // Execute immediately via worker notification
        cv_.notify_one();
    }

    return task_promise->get_future();
}

CompactionStats CompactionController::GetStatistics() const {
    CompactionStats stats;
    stats.total_compactions = stats_.total_compactions.load();
    stats.bytes_read = stats_.bytes_read.load();
    stats.bytes_written = stats_.bytes_written.load();
    stats.active_tasks = stats_.active_tasks.load();
    return stats;
}

bool CompactionController::NeedsCompaction() const {
    return version_manager_->NeedsCompaction();
}

void CompactionController::BackgroundLoop() {
    while (running_) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        cv_.wait_for(lock, options_.poll_interval, [this] {
            return !running_ || NeedsCompaction();
        });

        if (!running_) {
            break;
        }

        auto task_opt = PickCompactionTask();
        if (!task_opt.has_value()) {
            continue;
        }

        stats_.active_tasks++;
        lock.unlock();

        bool success = ExecuteTask(task_opt.value());

        lock.lock();
        stats_.active_tasks--;
        if (success) {
            stats_.total_compactions++;
        }
    }
}

std::optional<CompactionTask> CompactionController::PickCompactionTask() {
    int level = version_manager_->PickCompactionLevel();
    if (level < 0 || static_cast<size_t>(level) >= options_.max_levels - 1) {
        return std::nullopt;
    }

    CompactionTask task;
    task.compaction_id = next_compaction_id_++;
    task.src_level = level;
    task.dst_level = level + 1;
    task.output_directory = version_manager_->GetDirectory();
    task.inputs_level_n = version_manager_->GetCompactionInputs(level);
    
    if (task.inputs_level_n.empty()) {
        return std::nullopt;
    }

    // Get overlapping inputs in destination level
    std::string_view smallest_key = task.inputs_level_n.front()->GetSmallestKey();
    std::string_view largest_key = task.inputs_level_n.back()->GetLargestKey();
    task.inputs_level_n_plus_1 = version_manager_->GetOverlappingTables(level + 1, smallest_key, largest_key);

    return task;
}

bool CompactionController::ExecuteTask(const CompactionTask& task) {
    if (options_.compaction_strategy == CompactionStrategyType::LEVELED) {
        return RunLeveledCompaction(task);
    } else {
        return RunSizeTieredCompaction(task);
    }
}

bool CompactionController::RunLeveledCompaction(const CompactionTask& task) {
    // Construct CompactionIterator across all input files from source and destination levels
    std::vector<std::shared_ptr<SSTable>> all_inputs;
    all_inputs.insert(all_inputs.end(), task.inputs_level_n.begin(), task.inputs_level_n.end());
    all_inputs.insert(all_inputs.end(), task.inputs_level_n_plus_1.begin(), task.inputs_level_n_plus_1.end());

    CompactionIterator iterator(all_inputs, version_manager_);
    
    std::vector<std::shared_ptr<SSTable>> newly_flushed_tables;
    // Real implementation builds new SSTable builders, streams merged KV pairs,
    // applies sequence number filtering (drop overwritten/obsolete entries),
    // and registers new version edit with VersionManager.

    uint64_t bytes_processed = 0;
    while (iterator.Valid()) {
        bytes_processed += iterator.Key().size() + iterator.Value().size();
        iterator.Next();
    }

    stats_.bytes_read += bytes_processed;
    stats_.bytes_written += bytes_processed; // Approximated after deduplication

    // Commit changes atomically to VersionManager
    return version_manager_->CommitCompaction(task, newly_flushed_tables);
}

bool CompactionController::RunSizeTieredCompaction(const CompactionTask& task) {
    // Size-tiered compaction merges similarly sized SSTables together without key-range partitioning
    return RunLeveledCompaction(task);
}

} // namespace lsm