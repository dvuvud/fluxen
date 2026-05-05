/**
 * @file tinydb.hpp
 * @mainpage tinydb
 * @brief Single-header embedded key-value store for C++20.
 *
 * Drop this file into any project and get a persistent key-value database
 * with zero dependencies, no build system changes, and no linking required.
 *
 * @par Quick start
 * @code
 *   #include "tinydb.hpp"
 *
 *   tinydb::DB db("myapp.db");
 *   db.put("username", "rick");
 *
 *   if (auto name = db.get("username")) {
 *       std::cout << *name << std::endl;   // "rick"
 *   }
 * @endcode
 *
 * @par How does it work?
 * The database is an append-only log on disk backed by a memory-mapped file.
 * An in-memory hash index maps each key to the offset and length of its value
 * in the file. Iteration via `each()` and `prefix()` is zero-copy. Callbacks receive
 * a Bytes span pointing directly into the mapped region. `get()` copies into the
 * return type. Writes append a 6-byte header + key + value to the file.
 * Deletes append a tombstone. The index is rebuilt by scanning the log once
 * on open (last write wins).
 *
 * @par File & data layout
 * @code
 *   [magic: 8 bytes "TINYDB01"]
 *   [entry] [entry] ...
 *
 *   entry layout:
 *     flags   : 1 byte   — 0x00 is live and 0x01 is tombstone
 *     key_len : 1 byte   — length of key in bytes (max 255)
 *     val_len : 4 bytes  — length of value in bytes (little-endian)
 *     key     : key_len bytes
 *     value   : val_len bytes  (omitted in tombstone entries)
 * @endcode
 *
 * @par Thread safety
 * All public methods are thread-safe. A single std::mutex serialises
 * concurrent access.
 *
 * @par Important limitations
 * - Maximum key length: 255 bytes.
 * - Maximum value length: ~4 GB (uint32_t).
 * - No range queries (keys are stored in hash order).
 * - The full key set lives in RAM (std::unordered_map).
 * - Windows-specific: The database file is locked while a DB object exists. 
 *   You must destroy the DB object before attempting to delete or move the file.
 *
 * @par Compiler requirements
 * C++20. Tested with GCC 12+, Clang 15+, MSVC 19.34+.
 * Runs on Linux, macOS, and Windows.
 *
 * @version 1.1.0
 * @par License
 * MIT License
 */

#pragma once

#include <cassert>
#include <stdexcept>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <functional>
#include <string_view>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <type_traits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tinydb {

// --- public types ---

/**
 * @brief A non-owning view of raw bytes in the memory-mapped file.
 *
 * Returned by the @p each() and @p prefix() iteration callbacks.
 * The span remains valid until the next write operation or @p compact() call,
 * so copy the data out if you need to keep it longer.
 */
using Bytes = std::span<const std::byte>;

/**
 * @brief Controls whether a transaction's operations are applied or discarded.
 *
 * Returned by the callback passed to DB::transaction().
 *
 * @see DB::transaction()
 */
enum TxResult : uint8_t { commit, rollback };

// --- internal ---

/// @cond INTERNAL
namespace detail {

inline constexpr uint8_t MAGIC[8]  = { 'T','I','N','Y','D','B','0','1' };
inline constexpr uint8_t FLAG_LIVE = 0x00;
inline constexpr uint8_t FLAG_TOMB = 0x01;
inline constexpr uint8_t MAX_KEY   = 255;
inline constexpr size_t  HEADER_SIZE = 6;   // on-disk entry header size

/* On-disk entry header (6 bytes on disk) */
struct EntryHeader {
    uint8_t  flags;
    uint8_t  key_len;
    uint32_t val_len;
};

/* Serialize header into 6-byte buffer */
inline void encode_header(uint8_t out[HEADER_SIZE], const EntryHeader& h) noexcept {
    out[0] = h.flags;
    out[1] = h.key_len;
    out[2] = static_cast<uint8_t>( h.val_len        & 0xFFu);
    out[3] = static_cast<uint8_t>((h.val_len >>  8) & 0xFFu);
    out[4] = static_cast<uint8_t>((h.val_len >> 16) & 0xFFu);
    out[5] = static_cast<uint8_t>((h.val_len >> 24) & 0xFFu);
}

/* Deserialise header from a 6-byte buffer */
inline auto decode_header(const uint8_t in[HEADER_SIZE]) noexcept -> EntryHeader {
    return {
        .flags   = in[0],
        .key_len = in[1],
        .val_len = static_cast<uint32_t>(in[2])
                 | static_cast<uint32_t>(in[3]) <<  8
                 | static_cast<uint32_t>(in[4]) << 16
                 | static_cast<uint32_t>(in[5]) << 24,
    };
}

/* In-memory index entry (points into the mmap'd file) */
struct IndexEntry {
    size_t val_offset;  // byte offset of value data in file
    uint32_t val_len;
};

/* Cross-platform mmap wrapper */
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { close(); }

    MappedFile(const MappedFile&)            = delete;
    auto operator=(const MappedFile&) -> MappedFile& = delete;

    auto open(std::string_view path) -> bool {
        path_ = path;
#ifdef _WIN32
        file_ = CreateFileA(path_.c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            return false;
        }
#else
        fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd_ < 0) {
            return false;
        }
#endif
        return remap();
    }

    void close() {
        unmap();
#ifdef _WIN32
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

    auto remap() -> bool {
        unmap();
        size_ = file_size();
        dirty_ = false;
        file_size_ = size_;
        if (size_ == 0) {
            return true;
        }

#ifdef _WIN32
        map_ = CreateFileMappingA(file_, nullptr, PAGE_READWRITE, 0, 0, nullptr);

        if (!map_) {
            return false;
        }

        ptr_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, size_));

        if (!ptr_) {
            CloseHandle(map_);
            map_ = nullptr;
            return false;
        }
#else
        ptr_ = static_cast<uint8_t*>(::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));

        if (ptr_ == MAP_FAILED) {
            ptr_ = nullptr;
            return false;
        }
#endif
        return true;
    }

    auto append(const void* data, size_t len) -> bool {
        if (len == 0) {
            return true;
        }
#ifdef _WIN32
        DWORD written = 0;
        SetFilePointer(file_, 0, nullptr, FILE_END);
        if (!WriteFile(file_, data, static_cast<DWORD>(len), &written, nullptr)
            || written != static_cast<DWORD>(len)) {
            return false;
        }
#else
        ::lseek(fd_, 0, SEEK_END);
        if (::write(fd_, data, len) != static_cast<ssize_t>(len)) {
            return false;
        }
#endif
        dirty_ = true;
        file_size_ += len;
        return true;
    }

    void sync() {
#ifdef _WIN32
        FlushFileBuffers(file_);
#else
        ::fsync(fd_);
#endif
    }

    auto rewrite(const std::vector<uint8_t>& data) -> bool {
        unmap();
#ifdef _WIN32
        SetFilePointer(file_, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(file_);
        DWORD written = 0;
        if (!WriteFile(file_, data.data(), static_cast<DWORD>(data.size()),
                       &written, nullptr)) {
            return false;
        }
        FlushFileBuffers(file_);
#else
        if (::ftruncate(fd_, 0) < 0) return false;
        ::lseek(fd_, 0, SEEK_SET);
        if (::write(fd_, data.data(), data.size())
            != static_cast<ssize_t>(data.size()))
            return false;
        ::fsync(fd_);
#endif
        return remap();
    }

    [[nodiscard]] auto ptr() const noexcept -> const uint8_t* {
        if (dirty_) {
            const_cast<MappedFile*>(this)->remap();
        }
        return ptr_;
    }
    [[nodiscard]] auto size() const noexcept -> size_t { return file_size_; }

private:
    void unmap() {
        if (!ptr_) return;
#ifdef _WIN32
        UnmapViewOfFile(ptr_);
        if (map_) { CloseHandle(map_); map_ = nullptr; }
#else
        ::munmap(ptr_, size_);
#endif
        ptr_  = nullptr;
        size_ = 0;
    }

    [[nodiscard]] auto file_size() const noexcept -> size_t {
#ifdef _WIN32
        LARGE_INTEGER sz{};
        GetFileSizeEx(file_, &sz);
        return static_cast<size_t>(sz.QuadPart);
#else
        struct stat st{};
        ::fstat(fd_, &st);
        return static_cast<size_t>(st.st_size);
#endif
    }

#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE map_  = nullptr;
#else
    int fd_ = -1;
#endif
    uint8_t* ptr_  = nullptr;
    size_t size_ = 0;
    bool dirty_ = false;
    size_t file_size_ = 0;
    std::string path_;
};
}   // namespace detail
/// @endcond

// --- transaction ---

class DB;   // forward declaration

/**
 * @brief A staged batch of write operations, used inside DB::transaction().
 *
 * A @p Tx is constructed by DB::transaction() and passed to the callback.
 * Operations staged on it are only applied to the database if the
 * callback returns @p tinydb::commit. They are discarded on
 * @p tinydb::rollback or if the callback throws.
 *
 * @note Do not construct a @p Tx directly, use DB::transaction().
 *
 * @see DB::transaction()
 */
class Tx {
public:
    /**
     * @brief Stage a string value to be written.
     *
     * @param key   The key to write (max 255 bytes).
     * @param value The string value to store.
     */
    void put(std::string_view key, std::string_view value) {
        ops_.push_back({ .key=std::string(key),
                         .val=std::vector<uint8_t>(value.begin(), value.end()),
                         .is_delete=false });
    }

     /**
     * @brief Stage a trivially copyable value to be written.
     *
     * Accepts any type that satisfies `std::is_trivially_copyable`, except
     * types convertible to `std::string_view` (those go through the string
     * overload above).
     *
     * @tparam T    Any trivially copyable type: int, float, struct, etc.
     * @param key   The key to write (max 255 bytes).
     * @param value The value to store. Stored as raw bytes via memcpy.
     *
     * @par Example
     * @code
     *   struct Point { float x, y; };
     *   tx.put("origin", Point{0.0f, 0.0f});
     *   tx.put("count",  int32_t{42});
     * @endcode
     */
    template <typename T>
        requires (std::is_trivially_copyable_v<T>
               && !std::is_convertible_v<T, std::string_view>)
    void put(std::string_view key, const T& value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        ops_.push_back({ .key=std::string(key),
                         .val=std::vector<uint8_t>(p, p + sizeof(T)),
                         .is_delete=false });
    }

    /**
     * @brief Stage a key deletion.
     *
     * If the key does not exist in the database, this is a no-op when the
     * transaction is committed.
     *
     * @param key The key to delete.
     */
    void remove(std::string_view key) {
        ops_.push_back({ .key=std::string(key), .val={}, .is_delete=true });
    }

private:
    friend class DB;

    struct Op {
        std::string key;
        std::vector<uint8_t> val;
        bool is_delete;
    };
    std::vector<Op> ops_;
};

// --- DB ---

/**
 * @brief A persistent key-value database backed by a single file.
 *
 * Keys are UTF-8 strings up to 255 bytes. Values can be std::string,
 * string literals, or any trivially copyable type (int, float, struct, ...).
 *
 * The database is safe to use from multiple threads concurrently since all public
 * methods acquire an internal mutex.
 *
 * @note @p DB is non-copyable. Create one instance per file.
 *
 * @par Example
 * @code
 *   tinydb::DB db("app.db");
 *
 *   db.put("hits", int64_t{0});
 *
 *   if (auto val = db.get<int64_t>("hits")) {
 *       std::cout << *val << std::endl;
 *   }
 * @endcode
 */
class DB {
public:
    /**
     * @brief Opens or creates a database at the given path.
     *
     * If the file does not exist, it is created and initialised with the
     * magic header. If it exists, it is scanned to rebuild the
     * in-memory index.
     *
     * @param path Filesystem path to the database file.
     *
     * @throws std::runtime_error If the file cannot be opened or created.
     * @throws std::runtime_error If the file exists but has an invalid magic
     *         header (i.e. was not created by tinydb).
     */
    explicit DB(std::string_view path) {
        if (!file_.open(path)) {
            throw std::runtime_error("tinydb: failed to open '" + std::string(path) + "'");
        }

        if (file_.size() == 0) {
            init_file();
        }
        else {
            load_index();
        }
    }

    /**
     * @brief Closes the database and releases all file locks.
     * 
     * @note On Windows, the underlying file is locked as long as this object 
     * exists. Ensure this object is destroyed before calling 
     * `std::filesystem::remove()` on the database file.
     */
    ~DB() = default;
    /// @cond
    DB(const DB&)            = delete;
    auto operator=(const DB&) -> DB& = delete;
    /// @endcond

    // --- WRITE ---

    /**
     * @brief Stores a string value under the given key.
     *
     * If the key already exists its value is overwritten. The write is
     * appended to the on-disk log immediately but is not explicitly fsynced.
     * Use transaction() if you require durability guarantees.
     *
     * @param key   Key to write. Must be between 1 and 255 bytes.
     * @param value String value to store.
     */
    void put(std::string_view key, std::string_view value) {
        std::scoped_lock lock(mu_);
        append_entry(
            key,
            reinterpret_cast<const uint8_t*>(value.data()),
            static_cast<uint32_t>(value.size()),
            false
        );
    }

    /**
     * @brief Stores a trivially copyable value under the given key.
     *
     * The value is stored as its raw bytes via `memcpy`. Overwriting a key
     * with a value of a different size is valid. A subsequent get<T>() call
     * will return `nullopt` if the stored size does not match `sizeof(T)`.
     *
     * @tparam T    Any trivially copyable type: int, float, bool, struct, etc.
     *              Must not be implicitly convertible to std::string_view.
     * @param key   Key to write. Must be between 1 and 255 bytes.
     * @param value Value to store.
     *
     * @par Example
     * @code
     *   struct Config { int port; bool debug; };
     *   db.put("cfg", Config{8080, false});
     *   db.put("score", int32_t{100});
     * @endcode
     */
    template <typename T>
        requires (std::is_trivially_copyable_v<T>
               && !std::is_convertible_v<T, std::string_view>)
    void put(std::string_view key, const T& value) {
        std::scoped_lock lock(mu_);
        append_entry(
            key,
            reinterpret_cast<const uint8_t*>(&value),
            static_cast<uint32_t>(sizeof(T)),
            false
        );
    }

    // --- READ ---

    /**
     * @brief Retrieves the value stored under the given key.
     *
     * Returns `std::nullopt` in two cases:
     * - The key does not exist.
     * - For non-string @p T: the stored byte size differs from `sizeof(T)`.
     *
     * The default template parameter is `std::string`, so `db.get("key")` and
     * `db.get<std::string>("key")` are equivalent.
     *
     * @tparam T The type to deserialise into. Defaults to `std::string`.
     *           Must be `std::string` or a trivially copyable type.
     * @param key The key to look up.
     * @return `std::optional<T>` containing the value, or `std::nullopt`.
     *
     * @par Example
     * @code
     *   auto name  = db.get("username");            // std::optional<std::string>
     *   auto score = db.get<int32_t>("score");      // std::optional<int32_t>
     *   auto cfg   = db.get<Config>("cfg");         // std::optional<Config>
     *
     *   // Idiomatic usage
     *   if (auto v = db.get<int32_t>("score")) {
     *       process(*v);
     *   }
     * @endcode
     */
    template <typename T = std::string>
    auto get(std::string_view key) const -> std::optional<T> {
        std::scoped_lock lock(mu_);
        auto it = index_.find(std::string(key));
        if (it == index_.end()) {
            return std::nullopt;
        }

        const auto& entry = it->second;

        if constexpr (std::is_same_v<T, std::string>) {
            const auto* ptr = file_.ptr() + entry.val_offset;
            return std::string(reinterpret_cast<const char*>(ptr), entry.val_len);
        } else {
            static_assert(std::is_trivially_copyable_v<T>, "tinydb: T must be trivially copyable");
            if (entry.val_len != static_cast<uint32_t>(sizeof(T))) {
                return std::nullopt;
            }
            T result;
            std::memcpy(&result, file_.ptr() + entry.val_offset, sizeof(T));
            return result;
        }
    }

    // --- DELETE ---

    /**
     * @brief Deletes the value stored under the given key.
     *
     * Appends a tombstone entry to the log and removes the key from the index.
     * If the key does not exist this is a no-op. Space used by the old value
     * is reclaimed the next time compact() is called.
     *
     * @param key The key to delete.
     */
    void remove(std::string_view key) {
        std::scoped_lock lock(mu_);
        if (!index_.contains(std::string(key))) {
            return;
        }
        append_entry(key, nullptr, 0, true);
    }

    // --- QUERY ---

    /**
     * @brief Returns true if the given key exists in the database.
     *
     * @param key The key to look up.
     * @return `true` if the key exists, `false` otherwise.
     */
    [[nodiscard]] auto has(std::string_view key) const -> bool {
        std::scoped_lock lock(mu_);
        return index_.contains(std::string(key));
    }

    /**
     * @brief Iterates over all live key-value pairs.
     *
     * The callback is invoked once per key with a `string_view` of the key
     * and a `Bytes` span pointing directly into the memory-mapped file.
     * The iteration order is unspecified.
     *
     * @param fn Callback of the form `void(std::string_view key, Bytes val)`.
     *
     * @warning The database mutex is held for the entire duration of
     *          iteration. Do not call any DB method from inside @p fn,
     *          as it will deadlock. If you need to modify the database during
     *          iteration, copy the keys out first and operate on them after.
     *
     * @warning The `Bytes` span is only valid for the duration of the
     *          callback. Do not store it. Instead, copy the data if you need it later.
     *
     * @par Example
     * @code
     *   db.each([](std::string_view key, tinydb::Bytes val) {
     *       std::string v(reinterpret_cast<const char*>(val.data()), val.size());
     *       std::cout << key << " = " << v << "\n";
     *   });
     * @endcode
     */
    void each(const std::function<void(std::string_view, Bytes)>& fn) {
        std::scoped_lock lock(mu_);
        for (const auto& [key, entry] : index_) {
            auto* ptr = reinterpret_cast<const std::byte*>(file_.ptr() + entry.val_offset);
            fn(key, Bytes{ ptr, entry.val_len });
        }
    }

    /**
     * @brief Iterates over all keys that begin with the given prefix.
     *
     * A convenient way to implement namespaced key sets. For example, all
     * keys stored as `"user:1001"`, `"user:1002"`, etc. can be iterated with
     * `db.prefix("user:", fn)`.
     *
     * The callback and iteration order behave identically to each().
     *
     * @param pfx The prefix string to filter by.
     * @param fn  Callback of the form `void(std::string_view key, Bytes val)`.
     *
     * @note Iteration is O(total keys), not O(matching keys), because the
     *       underlying index is a hash map with no sorted order.
     *
     * @warning The database mutex is held for the entire duration of
     *          iteration. Do not call any DB method from inside @p fn,
     *          as it will deadlock.
     *
     * @par Example
     * @code
     *   db.put("user:james", "admin");
     *   db.put("user:bryce",   "viewer");
     *   db.put("config:x",   "value");
     *
     *   db.prefix("user:", [](std::string_view key, tinydb::Bytes val) {
     *       // called for user:james and user:bryce only
     *   });
     * @endcode
     */
    void prefix(std::string_view pfx, const std::function<void(std::string_view, Bytes)>& fn) {
        std::scoped_lock lock(mu_);
        for (const auto& [key, entry] : index_) {
            if (key.starts_with(pfx)) {
                auto* ptr = reinterpret_cast<const std::byte*>(file_.ptr() + entry.val_offset);
                fn(key, Bytes{ ptr, entry.val_len });
            }
        }
    }

// --- TRANSACTION ---

    /**
     * @brief Executes a batch of operations atomically.
     *
     * The callback receives a Tx object on which any number of put() and
     * remove() calls can be staged. Returning `tinydb::commit` applies all
     * operations atomically and flushes them to disk. Returning
     * `tinydb::rollback` discards all staged operations and the database is
     * left completely unchanged.
     *
     * If the callback throws, the exception propagates to the caller and all
     * staged operations are discarded (equivalent to rollback).
     *
     * @param fn A callable of the form `TxResult(Tx&)`.
     *
     * @note The callback is invoked without holding the database mutex,
     * so other threads may commit writes between the time the callback
     * returns and the time the transaction is applied. The transaction's
     * own operations are applied atomically, but they are not isolated
     * from concurrent writers during the callback's execution.
     *
     * @par Example
     * @code
     *   db.transaction([](tinydb::Tx& tx) {
     *       tx.put("balance",  int32_t{500});
     *       tx.put("currency", std::string("USD"));
     *       tx.remove("old_session");
     *       return tinydb::commit;
     *   });
     * @endcode
     *
     * @par Rolling back
     * @code
     *   db.transaction([&](tinydb::Tx& tx) {
     *       tx.put("x", new_value);
     *       if (!validate(new_value)) {
     *           return tinydb::rollback;   // nothing is written
     *       }
     *       return tinydb::commit;
     *   });
     * @endcode
     */
    void transaction(const std::function<TxResult(Tx&)>& fn) {
        Tx tx;
        TxResult result = fn(tx);
        if (result == rollback) return;

        std::scoped_lock lock(mu_);
        for (const auto& op : tx.ops_) {
            append_entry(
                op.key,
                op.is_delete ? nullptr : op.val.data(),
                op.is_delete ? 0       : op.val.size(),
                op.is_delete
            );
        }
        file_.sync();
    }

// --- MAINTENANCE ---

    /**
     * @brief Rewrites the database file retaining only live entries.
     *
     * The append-only log accumulates dead entries over time as keys are
     * overwritten or deleted. `compact()` rewrites the file with only the
     * current live values, reclaiming that space.
     *
     * Blocks all reads and writes for the duration of the rewrite. The
     * database remains fully consistent. If the process is interrupted
     * mid-compact the old file is still intact.
     *
     * There is no automatic compaction. Call this periodically if your
     * workload involves many overwrites or deletions.
     *
     * @throws std::runtime_error If the file rewrite fails.
     *
     * @warning This function invalidates all previously returned spans, views,
     * and pointers to database-backed data. After `compact()` returns, any
     * attempt to access such data is undefined behavior. Callers must reacquire
     * any needed data after compaction.
     *
     * @par Example
     * @code
     *   // After a batch of deletions
     *   for (auto& key : expired_keys) {
     *       db.remove(key);
     *   }
     *   db.compact();
     * @endcode
     */
    void compact() {
        std::scoped_lock lock(mu_);

        std::vector<uint8_t> buf;
        buf.reserve(file_.size());

        buf.insert(
            buf.end(),
            detail::MAGIC,
            detail::MAGIC + sizeof(detail::MAGIC)
        );

        std::unordered_map<std::string, detail::IndexEntry> new_index;
        for (const auto& [key, entry] : index_) {
            detail::EntryHeader hdr{
                .flags   = detail::FLAG_LIVE,
                .key_len = static_cast<uint8_t>(key.size()),
                .val_len = entry.val_len,
            };

            uint8_t raw[detail::HEADER_SIZE];
            detail::encode_header(raw, hdr);

            size_t val_off = buf.size() + detail::HEADER_SIZE + key.size();

            buf.insert(buf.end(), raw, raw + detail::HEADER_SIZE);
            buf.insert(buf.end(), key.begin(), key.end());

            const auto* val_ptr = file_.ptr() + entry.val_offset;
            buf.insert(buf.end(), val_ptr, val_ptr + entry.val_len);

            new_index[key] = { .val_offset=val_off, .val_len=entry.val_len };
        }

        if (!file_.rewrite(buf)) {
            throw std::runtime_error("tinydb: compact failed during rewrite");
        }

        index_ = std::move(new_index);
    }

// --- DIAGNOSTICS ---

    /**
     * @brief Returns the number of live keys currently stored.
     *
     * @return Number of keys in the index.
     */
    [[nodiscard]] auto key_count() const -> size_t {
        std::scoped_lock lock(mu_);
        return index_.size();
    }

    /**
     * @brief Returns the current size of the database file in bytes.
     *
     * This includes space used by dead entries (overwritten or deleted values)
     * that have not yet been reclaimed by compact(). It is an upper bound on
     * the live data size.
     *
     * @return File size in bytes.
     */
    [[nodiscard]] auto file_size() const -> size_t {
        std::scoped_lock lock(mu_);
        return file_.size();
    }

private:
    /// Writes the magic header to a newly created file.
    void init_file() {
        file_.append(detail::MAGIC, sizeof(detail::MAGIC));
    }

    /**
     * @brief Scans the on-disk log to rebuild the in-memory index.
     *
     * Called once during construction for existing files. Iterates every
     * entry in the log; the last write for any given key wins. Tombstone
     * entries remove the key from the index. A truncated entry at the tail
     * of the file (e.g. from a crash mid-write) is silently skipped.
     */
    void load_index() {
        if (file_.size() < sizeof(detail::MAGIC)) {
            throw std::runtime_error("tinydb: file too small to be valid");
        }

        if (std::memcmp(file_.ptr(), detail::MAGIC, sizeof(detail::MAGIC)) != 0) {
            throw std::runtime_error("tinydb: bad magic. File was not created by tinydb");
        }

        size_t pos = sizeof(detail::MAGIC);

        while (pos + detail::HEADER_SIZE <= file_.size()) {
            uint8_t raw[detail::HEADER_SIZE];
            std::memcpy(raw, file_.ptr() + pos, detail::HEADER_SIZE);
            detail::EntryHeader hdr = detail::decode_header(raw);
            pos += detail::HEADER_SIZE;

            if (pos + hdr.key_len + hdr.val_len > file_.size()) {
                break;
            }

            std::string key(
                reinterpret_cast<const char*>(file_.ptr() + pos),
                hdr.key_len
            );
            pos += hdr.key_len;

            if (hdr.flags == detail::FLAG_TOMB) {
                index_.erase(key);
            } else {
                index_[key] = { .val_offset=pos, .val_len=hdr.val_len };
            }

            pos += hdr.val_len;
        }
    }

    /**
     * @brief Appends one entry to the file and updates the in-memory index.
     *
     * For live entries the index is updated to point at the newly written
     * value. For tombstone entries the key is erased from the index.
     *
     * @pre The caller must hold @p mu_.
     * @pre `key.size() <= detail::MAX_KEY`
     */
    void append_entry(std::string_view key,
                      const uint8_t* val,
                      uint32_t val_len,
                      bool tombstone) {
        assert(key.size() <= detail::MAX_KEY && "tinydb: key exceeds 255-byte limit");

        detail::EntryHeader hdr{
            .flags   = tombstone ? detail::FLAG_TOMB : detail::FLAG_LIVE,
            .key_len = static_cast<uint8_t>(key.size()),
            .val_len = val_len,
        };

        uint8_t raw[detail::HEADER_SIZE];
        detail::encode_header(raw, hdr);
 
        file_.append(raw, detail::HEADER_SIZE);
        file_.append(key.data(), key.size());
        if (val && val_len) {
            file_.append(val, val_len);
        }

        if (tombstone) {
            index_.erase(std::string(key));
        } else {
            size_t val_off = file_.size() - val_len;
            index_[std::string(key)] = { .val_offset=val_off, .val_len=val_len };
        }
    }

    detail::MappedFile file_;
    std::unordered_map<std::string, detail::IndexEntry> index_;
    mutable std::mutex mu_;
};

}   // namespace tinydb
