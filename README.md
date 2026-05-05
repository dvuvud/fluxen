<p align="center">
  <img width="680" alt="tinydb_logo_no_tagline-2" src="https://github.com/user-attachments/assets/87ec195d-bf13-45b3-b3f5-bc00981a55bd"/>
</p>

<p align="center">
  A single-header, embedded key-value store for C++20.<br/>
  Drop tinydb.hpp into your project and get a persistent key-value database.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?style=flat-square" alt="C++20"/>
  <img src="https://img.shields.io/badge/header--only-single%20file-4A7FD6?style=flat-square" alt="header-only"/>
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square" alt="MIT"/>
  <img src="https://github.com/dvuvud/tinydb/actions/workflows/ci.yml/badge.svg" alt="CI"/>
</p>

---

```cpp
#include "tinydb.hpp"

tinydb::DB db("myapp.db");
db.put("username", "jim");

if (auto name = db.get("username")) {
    std::cout << *name << "\n";   // jim
}
```

No CMake. No dependencies. No linking. One `#include`.

---

## When to use tinydb

tinydb is a good fit when:
- You need a simple persistent store without a query language
- Your workload is read-heavy
- You want to store typed values (structs, ints, floats) without serialization boilerplate
- You want zero build friction with no CMake targets, no vcpkg packages, no linking

tinydb is **not** a good fit when:
- You need range queries, secondary indexes, or SQL
- Your workload is dominated by large batched writes
- You need multi-process access to the same database file

## How it works

tinydb uses a [Bitcask](https://riak.com/assets/bitcask-intro.pdf)-style design:

The database is an **append-only log** on disk backed by a memory-mapped file. An **in-memory hash index** maps each key to the byte offset of its value in the file. **Reads** look up the offset in the hash map and read directly from the mapped region. **Writes** append a small header + key + value to the end of the file. **Deletes** append a tombstone. Dead space is reclaimed by `compact()`. On open, the log is scanned once to rebuild the index (last write wins).

## Performance

Benchmarks were run on an Apple M2 with 8 GB of RAM running macOS Tahoe. All tests were performed on the internal SSD under minimal background load.

SQLite was configured in WAL mode with durability settings roughly comparable to tinydb. Unless otherwise noted, all benchmarks are **single-threaded**.

### Read performance
In this setup, tinydb shows substantially higher read throughput. Each lookup is effectively a hash map probe followed by a direct memory access into an mmap’d file. By contrast, SQLite performs a B-tree lookup and goes through its query and storage layers, which adds overhead even when data is cached in memory.

| Benchmark | N | tinydb | SQLite | Ratio |
|---|---|---|---|---|
| Sequential read | 1,000   | 38.1 M items/s | 476 k items/s | ~80x faster    |
| Sequential read | 100,000 | 25.6 M items/s | 308 k items/s | ~83x faster    |
| Random read | 1,000   | 35.1 M items/s | 426 k items/s | ~82x faster    |
| Random read | 100,000 | 15.1 M items/s | 303 k items/s | ~50x faster    |

These results reflect a best-case scenario for tinydb’s access pattern (direct key lookups with data already memory-resident). SQLite’s performance here includes the cost of its more general-purpose storage engine and abstractions.

### Individual writes
For individual writes, tinydb is faster in this benchmark because it appends to a flat log, while SQLite must maintain its B-tree structure even with relaxed durability settings.

| Benchmark | N | tinydb | SQLite | Ratio |
|---|---|---|---|---|
| Sequential write | 100,000 | 213 k items/s | 89 k items/s | ~2.4x faster |

### Bulk transactional writes
For batched writes within a transaction, SQLite performs significantly better. It amortizes B-tree updates and journaling overhead across the entire transaction, whereas tinydb still performs per-entry appends.

| Benchmark | N | tinydb | SQLite | Ratio |
|---|---|---|---|---|
| Bulk write (tx) | 10,000 | 218 k items/s | 1.25 M items/s | ~5.7x slower |
| Bulk write (tx) | 100,000 | 217 k items/s | 1.38 M items/s | ~6.3x slower |

### Notes on concurrency
These benchmarks are single-threaded. SQLite in WAL mode supports multiple concurrent readers and a writer, and can scale read throughput across threads. In multi-threaded, read-heavy workloads, this can reduce the gap observed here, depending on contention and access patterns.

## Install

Copy [`tinydb.hpp`](tinydb.hpp) into your project:

```bash
curl -O https://raw.githubusercontent.com/dvuvud/tinydb/main/tinydb.hpp
```

**Requirements:** C++20, GCC 12+, Clang 15+, or MSVC 19.34+. Linux, macOS, and Windows.

---

## Quick start

```cpp
#include "tinydb.hpp"

// Open or create a database
tinydb::DB db("app.db");

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

## Docs

Full API reference is available at [dvuvud.github.io/tinydb](https://dvuvud.github.io/tinydb).

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
