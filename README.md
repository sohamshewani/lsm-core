# lsm-core
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT) [![CI](https://github.com/sohamshewani/lsm-core/actions/workflows/ci.yml/badge.svg)](https://github.com/sohamshewani/lsm-core/actions) [![Security: Hardened](https://img.shields.io/badge/Security-Hardened-blue.svg)](https://github.com/sohamshewani/lsm-core) [![Stars](https://img.shields.io/github/stars/sohamshewani/lsm-core?style=flat)](https://github.com/sohamshewani/lsm-core)

## Executive Summary & Problem Space

`lsm-core` is an enterprise-grade, high-throughput Log-Structured Merge-tree (LSM) storage engine written in modern C++ (C++20). Engineered for low-latency, write-heavy workloads, `lsm-core` eliminates the catastrophic random I/O bottlenecks inherent in traditional in-place update structures (such as B-Trees and ISAM indexes). 

By transforming all incoming mutations into sequential append operations, `lsm-core` maximizes modern NVMe storage bandwidth while guaranteeing durability via synchronized Write-Ahead Logging (WAL). The engine addresses critical production failure modes:
* **Write Amplification Reduction:** Bypasses page-level random writes in favor of sequential generational compaction.
* **Tail Latency Suppression:** Utilizes lock-free/lock-minimized concurrent memtable switching to decouple ingestion paths from background flushing and compaction tasks.
* **Crash Consistency:** Enforces strict write-ahead logging semantics with atomic directory syncs, ensuring zero data loss upon abrupt process termination or kernel panics.

---

## Structural ASCII Architecture / Dataflow Diagram

The system coordinates memory buffers, immutable checkpoints, and stratified immutable disk levels through a decoupled thread-safe pipeline.

```
 +-----------------------------------------------------------------------+
 |                        Client Application                             |
 +-----------------------------------------------------------------------+
        | Write (Key, Value)                   | Read (Key)
        v                                      v
 +-----------------------+           +-----------------------------------+
 |  include/lsm/engine   |<--------->|           MemTable Pipeline       |
 +-----------------------+           |  +-----------------------------+  |
        |                            |  | Active MemTable (Concurrent)|  |
        | Sync Append                |  +-----------------------------+  |
        v                            |  | Immutable MemTable (Flush)  |  |
 +-----------------------+           |  +-----------------------------+  |
 |   include/lsm/wal     |           +-----------------------------------+
 +-----------------------+                             |
                                                       | Flush / Freeze
                                                       v
                                     +-----------------------------------+
                                     |    include/lsm/sstable (Level 0)  |
                                     +-----------------------------------+
                                                       |
                                                       | Compaction Engine
                                                       v
                                     +-----------------------------------+
                                     |    include/lsm/compaction (L1-Ln) |
                                     +-----------------------------------+
```

### Component Breakdown
* **`include/lsm/types.hpp`**: Core primitive definitions, including slice abstractions, sequence numbers, and tombstone markers.
* **`include/lsm/memtable.hpp`**: Thread-safe skip-list or concurrent vector-based in-memory index holding mutable sorted key-value pairs.
* **`include/lsm/wal.hpp`**: Append-only log enforcing crash recovery semantics and disk serialization.
* **`include/lsm/sstable.hpp`**: Immutable, on-disk sorted string tables complete with embedded sparse block indexes and Bloom filters.
* **`include/lsm/compaction.hpp`**: Background worker orchestrating tiered/leveled multi-generation merges to reclaim disk space and bound read amplification.
* **`include/lsm/engine.hpp`**: Public facade uniting WAL, MemTable, SSTables, and Compaction into a unified ACID-compliant storage coordinate.

---

## Concurrency Model & Memory Invariants

`lsm-core` relies on strict memory ownership rules and modern synchronization primitives to ensure high parallelism without risking data races or deadlocks.

* **Single-Writer, Multi-Reader (SWMR) MemTable Architecture:** Ingestion uses lock-free atomics where possible, backed by fine-grained reader-writer latches (`std::shared_mutex`) for structural transitions.
* **MemTable Swapping Protocol:** When the active MemTable breaches its memory ceiling (`include/lsm/memtable.hpp`), it is atomically transitioned to an immutable state. A background flushing thread serializes the immutable buffer to an SSTable (`include/lsm/sstable.hpp`), entirely isolating writes from slow disk I/O.
* **Memory Ownership & RAII:** Raw pointers are strictly forbidden across module boundaries. Ownership of data buffers, memory-mapped regions, and file descriptors is encapsulated via smart pointers (`std::unique_ptr`, `std::shared_ptr`) with custom deleters where zero-copy semantics are enforced.
* **Atomic Sequence Ordering:** All mutations acquire a monotonically increasing sequence number (`SeqNum`), ensuring deterministic snapshot isolation during reads and multi-version concurrency control (MVCC) resolution across compacted SSTable boundaries.

---

## Performance Characteristics & Algorithmic Complexity Table

| Operation | Average Case | Worst Case | Operational Bottleneck |
| :--- | :--- | :--- | :--- |
| **Point Write (Put)** | $O(1)$ amortized | $O(\log N)$ | WAL sync (`fsync`) & MemTable allocation |
| **Point Read (Get)** | $O(1)$ to $O(L \cdot \log B)$ | $O(N)$ | Bloom filter checks, SSTable index scans, File I/O |
| **Range Scan** | $O(K + \log N)$ | $O(K + N)$ | Multi-way merge iterator heap overhead over levels |
| **Flush / Compaction** | $O(N)$ throughput | $O(N \log N)$ | Sequential disk bandwidth saturation (Read/Write amplification) |

*Where $N$ is the dataset size, $L$ is the number of SSTable levels, $B$ is blocks per table, and $K$ is the number of results returned in a range query.*

---

## Security Architecture & Threat Model

Designed for deployment in zero-trust, cloud-native environments, `lsm-core` hardens its runtime boundary against adversarial inputs and resource exhaustion vectors.

* **Input Validation & Sanitization:** All incoming byte slices (`include/lsm/types.hpp`) undergo strict structural boundary validation. Keys and values enforce maximum length bounds to prevent integer overflow exploits during memory allocation.
* **Memory Corruption Protections:** No manual pointer arithmetic (`malloc`/`free`) is permitted. Bounds-checked containers (`std::vector`, `std::string_view`) are utilized exclusively. Critical metadata blocks incorporate CRC32 checksum verification to detect bit-rot or silent data corruption on disk.
* **Denial of Service (DoS) Mitigation:** 
  * *Write Stalls:* If ingestion outpaces background flush/compaction capacity, the engine actively throttles incoming write requests to prevent out-of-memory (OOM) termination.
  * *Resource Caps:* Open file descriptors and memory-mapped virtual address spaces are strictly bounded via LRU eviction policies inside the block cache.
* **Compiler Hardening Flags:** Production binaries enforce Stack Canaries (`-fstack-protector-strong`), Position Independent Executables (`-fPIE`), Read-Only Relocations (`-Wl,-z,relro,-z,now`), and strict warning-to-error compiler setups (`-Wall -Wextra -Werror`).

---

## Build, Verification & Benchmarking Recipes

Ensure your development environment has a modern C++20 compliant compiler (GCC 11+ or Clang 13+), GNU Make, and CMake installed.

### 1. Build the Engine and Verification Suite
Execute the following commands from the repository root to compile the core library, run automated unit tests, and assert internal system invariants:

```bash
# Clean workspace and build release binaries with hardening flags
make clean
make release

# Execute invariant validation and core unit tests
make test
```

### 2. Run the Benchmark Suite
To profile ingestion throughput, read amplification, and tail latencies under heavy multi-threaded stress:

```bash
# Compile and run the high-performance benchmark harness
make benchmark
./build/bin/bench_engine --threads=8 --num-keys=1000000
```

### 3. Continuous Integration & Sanitizer Checks (Local)
To run memory analysis using AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan):

```bash
make sanitize
make test