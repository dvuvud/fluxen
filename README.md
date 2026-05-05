<p align="center">
  <img width="680" alt="tinydb_logo_no_tagline-2" src="https://github.com/user-attachments/assets/87ec195d-bf13-45b3-b3f5-bc00981a55bd"/>
</p>

<p align="center">
  A single-header, embedded key-value store for C++20.<br/>
  Drop one file into your project and you have a persistent database.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue?style=flat-square"/>
  <img src="https://img.shields.io/badge/header--only-single%20file-4A7FD6?style=flat-square"/>
  <img src="https://img.shields.io/badge/license-MIT-green?style=flat-square"/>
</p>

---

```cpp
#include "tinydb.hpp"

tinydb::DB db("myapp.db");
db.put("username", "run");

if (auto name = db.get("username")) {
    std::cout << *name << "\n";   // run
}
```

No CMake. No dependencies. No linking. One `#include`.

---

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

## Library

### `DB(path)`: open or create

```cpp
tinydb::DB db("myapp.db");
```

Opens the database at `path`, creating it if it doesn't exist. Throws `std::runtime_error` if the file can't be opened or has an invalid magic header.

---

### `put`: write a value

```cpp
db.put("key", "string");      // std::string / string literal
db.put("key", int32_t{42});   // any numeric type
db.put("key", myStruct);      // any trivially copyable struct
```

Overwrites any existing value for the key. Thread-safe.
Key length is limited to 255 bytes.

---

### `get<T>`: read a value

```cpp
auto s = db.get("key");            // std::optional<std::string>  (default)
auto n = db.get<int32_t>("key");   // std::optional<int32_t>
auto c = db.get<Config>("key");    // std::optional<Config>
```

Returns `std::nullopt` if the key doesn't exist, or if the stored size doesn't match `sizeof(T)`.

---

### `remove`: delete a key

```cpp
db.remove("key");   // no-op if key doesn't exist
```

---

### `has`: check existence

```cpp
if (db.has("key")) { ... }
```

---

### `each`: iterate all keys

```cpp
db.each([](std::string_view key, tinydb::Bytes val) {
    std::string v(reinterpret_cast<const char*>(val.data()), val.size());
    std::cout << key << " = " << v << "\n";
});
```

> [!Warning]
> The database mutex is held for the entire iteration. Do not call any `DB` method from inside the callback, as it will deadlock. Copy keys out first if you need to modify the database during iteration.

---

### `prefix`: scan by key prefix

```cpp
db.put("user:rob", "admin");
db.put("user:nick",   "viewer");

db.prefix("user:", [](std::string_view key, tinydb::Bytes val) {
    // called for user:rob and user:nick only
});
```

Same mutex warning as `each`.

---

### `transaction`: atomic batch

```cpp
db.transaction([](tinydb::Tx& tx) {
    tx.put("balance",  int32_t{500});
    tx.put("currency", std::string("USD"));
    tx.remove("old_key");
    return tinydb::commit;   // or tinydb::rollback to discard everything
});
```

All operations in a committed transaction are written atomically and flushed to disk. If the callback throws, all staged operations are discarded (equivalent to `rollback`).

---

### `compact`: reclaim disk space

```cpp
db.compact();
```

Rewrites the file keeping only live entries. The append-only log accumulates dead entries over time as keys are overwritten or deleted. Call this periodically if your workload involves many mutations.

---

### `key_count` / `file_size`

```cpp
std::cout << db.key_count() << " keys\n";
std::cout << db.file_size() << " bytes\n";
```

---

## Namespaced keys

tinydb has no concept of tables, but prefixed keys give you the same thing:

```cpp
db.put("user:1001", "rob");
db.put("user:1002", "nick");
db.put("session:abc123", "user:1001");
db.put("config:theme", "dark");

db.prefix("user:",    [](auto key, auto val) { /* all users   */ });
db.prefix("session:", [](auto key, auto val) { /* all sessions */ });
```

---

## How it works

tinydb is an **append-only log** on disk with an **in-memory hash index**.

Writes append a 6-byte header + key + value to the file. Reads look up the key in the index and return a view directly into the memory-mapped file. Deletes append a tombstone entry. On open, the log is scanned once to rebuild the index (last write wins). `compact()` rewrites the file with only live entries.

The header is serialised in little-endian bytes with no alignment assumptions and is portable across every compiler and platform.

### File & data layout

```
[magic: 8 bytes "TINYDB01"]
[entry][entry]...

entry:
  flags   : 1 byte   — 0x00 live, 0x01 tombstone
  key_len : 1 byte   — length of key (max 255)
  val_len : 4 bytes  — length of value, little-endian
  key     : key_len bytes
  value   : val_len bytes  (absent in tombstone entries)
```

---

## Thread safety

All public methods are thread-safe via a single `std::mutex`.

---

## Limitations

- **In-memory index:** The full key set lives in a `std::unordered_map` in RAM.
- **Single writer:** All operations share one mutex. It is not designed for high-throughput concurrent workloads.
- **No range queries:** Keys are stored in hash order. `prefix()` is O(n keys).
- **Key length:** Maximum 255 bytes.

---

## Building

```bash
cmake -B build
cmake --build build
./tests/tinydb-tests
```
