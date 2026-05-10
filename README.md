<p align="center">
  <img width="680" alt="fluxen_logo" src="https://github.com/user-attachments/assets/156ce053-e5d4-4198-a567-ad739019a123"/>
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
- [Docs](#docs)
- [Building tests](#building-tests)
- [License](#mit-license)

---

## Why fluxen exists
fluxen is built for cases where you want persistent storage without adopting a full database system.

It is not intended to replace general databases, which provide a much broader set of features such as SQL queries, indexing, and multi-purpose storage capabilities.

---

## When to use fluxen

fluxen is a good fit when:
- You need a simple persistent store without a query language
- Your workload is read-heavy
- You want to store typed values (structs, ints, floats) without serialization boilerplate
- You want zero build friction with no CMake targets, no vcpkg packages, no linking
- You need atomic bulk writes: `transaction()` commits many operations in a single write

fluxen is **not** a good fit when:
- You need range queries, secondary indexes, or SQL
- You need multi-process access to the same database file

---

## How it works

fluxen uses a [Bitcask](https://riak.com/assets/bitcask-intro.pdf)-style design:

- The database is an **append-only log** on disk backed by a memory-mapped file.
- An **in-memory hash index** maps each key to the byte offset of its value in the file.
- **Reads** look up the offset in the hash map and read directly from the mapped region.
- **Writes** append a small header + key + value to the end of the file.
- **Transactions** stage any number of operations and flush them in a single write + fsync.
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

// Bulk writes: one write, one fsync, fully atomic
db.transaction([](fluxen::Tx& tx) {
  tx.put("balance",  int32_t{500});
  tx.put("currency", std::string("USD"));
  tx.remove("old_session");
  return fluxen::commit;
});
```

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
