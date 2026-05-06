<p align="center">
  <img width="680" alt="fluxen_logo" src="https://github.com/user-attachments/assets/87ec195d-bf13-45b3-b3f5-bc00981a55bd"/>
</p>

<p align="center">
  A single-header, embedded key-value store for C++20.<br/>
  Drop fluxen.hpp into your project and get a persistent key-value database.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?style=flat-square" alt="C++20"/>
  <img src="https://img.shields.io/badge/header--only-single%20file-4A7FD6?style=flat-square" alt="header-only"/>
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square" alt="MIT"/>
  <img src="https://github.com/dvuvud/fluxen/actions/workflows/ci.yml/badge.svg" alt="CI"/>
</p>

---

```cpp
#include "fluxen.hpp"

fluxen::DB db("myapp.db");
db.put("username", "jim");

if (auto name = db.get("username")) {
    std::cout << *name << "\n";   // jim
}
```

No CMake. No dependencies. No linking. One `#include`.

---

### Table of Contents

- [Why fluxen exists](#why-fluxen-exists)
- [When to use fluxen](#when-to-use-fluxen)
- [How it works](#how-it-works)
- [Install](#install)
- [Quick start](#quick-start)
- [Benchmarks (early results)](#benchmarks-early-results)
    - [Read performance](#read-performance)
    - [Individual writes](#individual-writes)
    - [Bulk transactional writes](#bulk-transactional-writes)
- [Concurrency behavior](#concurrency-behavior)
- [Docs](#docs)
- [Building tests](#building-tests)
- [License](#mit-license)

---

## Why fluxen exists
fluxen is built for cases where you want persistent storage without adopting a full database system.

It is not intended to replace general databases, which provide a much broader set of features such as SQL queries, indexing, and multi-purpose storage capabilities.

To give a sense of where this design sits in practice, [the benchmarks section](#benchmarks-early-results) includes benchmarks against SQLite for a few specific, narrow workloads (primarily direct key-value access patterns).

---

## When to use fluxen

fluxen is a good fit when:
- You need a simple persistent store without a query language
- Your workload is read-heavy
- You want to store typed values (structs, ints, floats) without serialization boilerplate
- You want zero build friction with no CMake targets, no vcpkg packages, no linking

fluxen is **not** a good fit when:
- You need range queries, secondary indexes, or SQL
- Your workload is dominated by large batched writes
- You need multi-process access to the same database file

---

## How it works

fluxen uses a [Bitcask](https://riak.com/assets/bitcask-intro.pdf)-style design:

- The database is an **append-only log** on disk backed by a memory-mapped file.
- An **in-memory hash index** maps each key to the byte offset of its value in the file.
- **Reads** look up the offset in the hash map and read directly from the mapped region.
- **Writes** append a small header + key + value to the end of the file.
- **Deletes** append a tombstone. Dead space is reclaimed by `compact()`.
- On open, the log is scanned once to rebuild the index (last write wins).

---

## Install

Copy [`fluxen.hpp`](fluxen.hpp) into your project:

```bash
curl -O https://raw.githubusercontent.com/dvuvud/fluxen/main/fluxen.hpp
```

**Requirements:** C++20, GCC 12+, Clang 15+, or MSVC 19.34+. Linux, macOS, and Windows.

---

## Quick start

```cpp
#include "fluxen.hpp"

// Open or create a database
fluxen::DB db("app.db");

// Strings
db.put("city", "Stockholm");
auto city = db.get("city");    // std::optional<std::string>

// Numbers and any trivially copyable types
db.put("score", int32_t{100});
db.put("ratio", 0.95f);

auto score = db.get<int32_t>("score");
auto ratio = db.get<float>("ratio");

// Structs
struct Config { int port; bool debug; };
db.put("cfg", Config{8080, false});
auto cfg = db.get<Config>("cfg");

// Check and delete
if (db.has("city")) {
    db.remove("city");
}
```

---

## Benchmarks (early results)

Early benchmarks comparing fluxen against SQLite in key-value-style workloads.

Tests were run on an Apple M2 (8 GB RAM) on macOS. SQLite was configured in WAL mode with durability settings chosen to approximate fluxen’s persistence behavior.

The following benchmarks are single-threaded.

#### Read performance
fluxen benefits from direct hash lookups and memory-mapped reads.

| Benchmark | N | fluxen | SQLite | Ratio |
|---|---|---|---|---|
| Sequential read | 1,000   | 38.1 M items/s | 476 k items/s | ~80x faster    |
| Sequential read | 100,000 | 25.6 M items/s | 308 k items/s | ~83x faster    |
| Random read | 1,000   | 35.1 M items/s | 426 k items/s | ~82x faster    |
| Random read | 100,000 | 15.1 M items/s | 303 k items/s | ~50x faster    |

These results reflect an ideal cache-resident workload where keys are already in memory.

#### Individual writes
For individual writes, fluxen benefits from its append-only log structure.

| Benchmark | N | fluxen | SQLite | Ratio |
|---|---|---|---|---|
| Sequential write | 100,000 | 213 k items/s | 89 k items/s | ~2.4x faster |

#### Bulk transactional writes
SQLite performs significantly better in batched transactions due to amortized B-tree and WAL overhead.

| Benchmark | N | fluxen | SQLite | Ratio |
|---|---|---|---|---|
| Bulk write (tx) | 10,000 | 218 k items/s | 1.25 M items/s | ~5.7x slower |
| Bulk write (tx) | 100,000 | 217 k items/s | 1.38 M items/s | ~6.3x slower |

---

## Concurrency behavior
These benchmarks evaluate fluxen under multi-threaded workloads using Google Benchmark’s threading model on an 8-core Apple M2. They measure contention effects rather than raw per-operation throughput.

<details>
<summary><b>Concurrent reads</b></summary>
fluxen shows high baseline throughput due to its hash-based index and memory-mapped storage.

Performance is not strictly linear: throughput decreases as thread count increases from 2 to 8, with a partial recovery at 16 threads. This V-shaped scaling is an artifact of **cache-coherency pressure** and core saturation. On the 8-core M2 test hardware, contention peaks when all physical cores simultaneously compete for the same memory bus and internal cache lines to access the shared index.

| Benchmark | N | Threads | fluxen | SQLite | Ratio |
|---|---|---|---|---|---|
| Multi-threaded read | 1,000 | 2 threads | 20.89 M/s | 1.53 M/s | ~13.7x faster |
| Multi-threaded read | 1,000 | 4 threads | 13.11 M/s | 2.64 M/s | ~5.0x faster |
| Multi-threaded read | 1,000 | 8 threads | 11.93 M/s | 1.93 M/s | ~6.2x faster |
| Multi-threaded read | 1,000 | 16 threads | 17.61 M/s | 2.69 M/s | ~6.5x faster |
| Multi-threaded read | 10,000 | 2 threads | 17.42 M/s | 1.16 M/s | ~15.1x faster |
| Multi-threaded read | 10,000 | 4 threads | 11.56 M/s | 2.30 M/s | ~5.0x faster |
| Multi-threaded read | 10,000 | 8 threads | 9.52 M/s | 1.89 M/s | ~5.0x faster |
| Multi-threaded read | 10,000 | 16 threads | 17.33 M/s | 2.75 M/s | ~6.3x faster |

</details>

<details>
<summary><b>Read/write contention</b></summary>
fluxen coordinates thread safety via a `std::shared_mutex` (Readers-Writer lock).

In mixed workloads, write operations (`put`, `remove`) acquire an exclusive lock, which briefly pauses active readers. However, because the design is **append-only**, the exclusive window is extremely small—limited only to a file append and a hash-map update. Reads remain highly performant because they primarily involve a hash-map lookup and a direct pointer dereference into the mapped memory.

| Benchmark | N | Threads | fluxen (reads/s) | SQLite (reads/s) | Ratio |
|---|---|---|---|---|---|
| Read/write contention | 1,000 | 2 threads | 235.9k/s | 164.2k/s | ~1.44x faster |
| Read/write contention | 1,000 | 4 threads | 615.8k/s | 785.0k/s | ~1.27x slower |
| Read/write contention | 1,000 | 8 threads | 1.15M/s | 1.40M/s | ~1.22x slower |
| Read/write contention | 1,000 | 16 threads | 3.35M/s | 2.84M/s | ~1.18x faster |
| Read/write contention | 10,000 | 2 threads | 237.0k/s | 152.7k/s | ~1.55x faster |
| Read/write contention | 10,000 | 4 threads | 614.4k/s | 735.6k/s | ~1.20x slower |
| Read/write contention | 10,000 | 8 threads | 1.11M/s | 1.24M/s | ~1.12x slower |
| Read/write contention | 10,000 | 16 threads | 4.18M/s | 2.49M/s | ~1.68x faster |

</details>

<details>
<summary><b>Writer contention</b></summary>
Under multiple concurrent writers, SQLite scales significantly with thread count, whereas fluxen maintains flat, stable throughput.

This is due to the difference in coordination models. SQLite allows **work coalescing**. When a thread holds the write lock, it may perform multiple queued operations before yielding, amortizing the cost of the WAL write. fluxen serializes writes with a simple exclusive lock and no cross-thread batching. Each write incurs its full latency independently, resulting in predictable but non-scaling write performance under heavy contention.

| Benchmark | N | Threads | fluxen (items/s) | SQLite (items/s) | Ratio |
|---|---|---|---|---|---|
| Writer contention | 1,000 | 2 threads | 242.1k/s | 313.6k/s | ~1.30x slower |
| Writer contention | 1,000 | 4 threads | 274.6k/s | 919.6k/s | ~3.35x slower |
| Writer contention | 1,000 | 8 threads | 200.8k/s | 1.07M/s | ~5.30x slower |
| Writer contention | 1,000 | 16 threads | 238.9k/s | 1.84M/s | ~7.69x slower |
| Writer contention | 10,000 | 2 threads | 245.3k/s | 307.0k/s | ~1.25x slower |
| Writer contention | 10,000 | 4 threads | 271.0k/s | 863.5k/s | ~3.19x slower |
| Writer contention | 10,000 | 8 threads | 199.8k/s | 1.12M/s | ~5.60x slower |
| Writer contention | 10,000 | 16 threads | 219.9k/s | 1.94M/s | ~8.83x slower |

</details>

---

## Docs

Full API reference is available at [dvuvud.github.io/fluxen](https://dvuvud.github.io/fluxen).

---

## Building tests

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## MIT License

See [LICENSE](LICENSE).
