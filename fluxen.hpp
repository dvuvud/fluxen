/**
 * @file fluxen.hpp
 * @mainpage fluxen
 * @brief Single-header embedded key-value store for C++20.
 *
 * Drop this file into any project and get a persistent key-value database
 * with zero dependencies, no build system changes, and no linking required.
 *
 * @par Quick start
 * @code
 *   #include "fluxen.hpp"
 *
 *   fluxen::DB db("myapp.db");
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
 * in the file. Iteration via `each()` and `prefix()` is zero-copy. Callbacks
 * receive a Bytes span pointing directly into the mapped region. `get()` copies
 * into the return type. Writes append a 6-byte header + key + value to the
 * file. Deletes append a tombstone. The index is rebuilt by scanning the log
 * once on open (last write wins).
 * `transaction()` stages any number of operations and flushes them as a
 * single write + fsync — the right tool for bulk writes.
 *
 * @par File & data layout
 * @code
 *   [magic: 8 bytes "FLUXEN01"]
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
 * All public methods are thread-safe. Concurrent reads are permitted via a
 * shared mutex: multiple threads may call @p get(),
 * @p has(), @p each(), @p prefix(), @p key_count(), and @p file_size()
 * simultaneously. Write operations (@p put(), @p remove(), @p transaction(),
 * @p compact()) acquire an exclusive lock and block until all active readers
 * have finished.
 *
 * The memory-mapped file is remapped lazily: writes leave the mapping stale
 * and the first reader to observe a dirty mapping performs the remap under a
 * secondary @p sync_mutex_ before proceeding. Subsequent concurrent readers
 * that also observed the dirty flag wait for the remap to complete and then
 * proceed without remapping again.
 *
 * @par Important limitations
 * - Maximum key length: 255 bytes.
 * - Maximum value length: ~4 GB (uint32_t).
 * - No range queries (keys are stored in hash order).
 * - The full key set lives in RAM (std::unordered_map).
 * - Windows-specific: The database file is locked while a DB object exists.
 *   You must destroy the DB object before attempting to delete or move the
 * file.
 *
 * @par Compiler requirements
 * C++20. Tested with GCC 12+, Clang 15+, MSVC 19.34+.
 * Runs on Linux, macOS, and Windows.
 *
 * @version 1.0.0
 * @par License
 * MIT License
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fluxen {

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

inline constexpr uint8_t MAGIC[8] = {'F', 'L', 'U', 'X', 'E', 'N', '0', '1'};
inline constexpr uint8_t FLAG_LIVE = 0x00;
inline constexpr uint8_t FLAG_TOMB = 0x01;
inline constexpr uint8_t MAX_KEY = 255;
inline constexpr size_t HEADER_SIZE = 6; // on-disk entry header size

/* On-disk entry header (6 bytes on disk) */
struct EntryHeader {
  uint8_t flags;
  uint8_t key_len;
  uint32_t val_len;
};

/* Serialize header into 6-byte buffer */
inline void encode_header(uint8_t out[HEADER_SIZE],
                          const EntryHeader &h) noexcept {
  out[0] = h.flags;
  out[1] = h.key_len;
  out[2] = static_cast<uint8_t>(h.val_len & 0xFFu);
  out[3] = static_cast<uint8_t>((h.val_len >> 8) & 0xFFu);
  out[4] = static_cast<uint8_t>((h.val_len >> 16) & 0xFFu);
  out[5] = static_cast<uint8_t>((h.val_len >> 24) & 0xFFu);
}

/* Deserialise header from a 6-byte buffer */
inline auto decode_header(const uint8_t in[HEADER_SIZE]) noexcept
    -> EntryHeader {
  return {
      .flags = in[0],
      .key_len = in[1],
      .val_len = static_cast<uint32_t>(in[2]) |
                 static_cast<uint32_t>(in[3]) << 8 |
                 static_cast<uint32_t>(in[4]) << 16 |
                 static_cast<uint32_t>(in[5]) << 24,
  };
}

/* In-memory index entry (points into the mmap'd file) */
struct IndexEntry {
  size_t val_offset; // byte offset of value data in file
  uint32_t val_len;
};

struct StringHash {
  using is_transparent = void;

  auto operator()(std::string_view sv) const noexcept -> size_t {
    return std::hash<std::string_view>{}(sv);
  }
  auto operator()(const std::string &s) const noexcept -> size_t {
    return std::hash<std::string_view>{}(s);
  }
};

using IndexMap =
    std::unordered_map<std::string, IndexEntry, StringHash, std::equal_to<>>;

/* Cross-platform mmap wrapper */
class MappedFile {
private:
#ifdef _WIN32
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE map_ = nullptr;
#else
  int fd_ = -1;
#endif
  uint8_t *ptr_ = nullptr;
  size_t size_ = 0;
  size_t file_size_ = 0;
  std::atomic<bool> dirty_{false};
  std::string path_;

public:
  MappedFile() = default;
  ~MappedFile() { close(); }

  MappedFile(const MappedFile &) = delete;
  auto operator=(const MappedFile &) -> MappedFile & = delete;

  auto open(std::string_view path) -> bool {
    path_ = path;
#ifdef _WIN32
    file_ = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      return false;
    }
    SetFilePointer(file_, 0, nullptr, FILE_END);
#else
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
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
    file_size_ = size_;
    if (size_ == 0) {
      dirty_.store(false, std::memory_order_release);
      return true;
    }

#ifdef _WIN32
    map_ = CreateFileMappingA(file_, nullptr, PAGE_READWRITE, 0, 0, nullptr);

    if (!map_) {
      dirty_.store(false, std::memory_order_release);
      return false;
    }

    ptr_ = static_cast<uint8_t *>(
        MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, size_));

    if (!ptr_) {
      CloseHandle(map_);
      map_ = nullptr;
      dirty_.store(false, std::memory_order_release);
      return false;
    }
#else
    ptr_ = static_cast<uint8_t *>(
        ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));

    if (ptr_ == MAP_FAILED) {
      ptr_ = nullptr;
      dirty_.store(false, std::memory_order_release);
      return false;
    }
#endif
    dirty_.store(false, std::memory_order_release);
    return true;
  }

  auto append(const void *data, size_t len) -> bool {
    if (len == 0) {
      return true;
    }
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(file_, data, static_cast<DWORD>(len), &written, nullptr) ||
        written != static_cast<DWORD>(len)) {
      return false;
    }
#else
    if (::write(fd_, data, len) != static_cast<ssize_t>(len)) {
      return false;
    }
#endif
    dirty_.store(true, std::memory_order_relaxed);
    file_size_ += len;
    return true;
  }

  [[nodiscard]] auto sync() -> bool {
#ifdef _WIN32
    return FlushFileBuffers(file_) != 0;
#else
    return ::fsync(fd_) == 0;
#endif
  }

  [[nodiscard]] auto truncate(size_t new_size) -> bool {
#ifdef _WIN32
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(new_size);
    if (!SetFilePointerEx(file_, li, nullptr, FILE_BEGIN)) {
      return false;
    }
    if (!SetEndOfFile(file_)) {
      SetFilePointer(file_, 0, nullptr, FILE_END);
      return false;
    }
    SetFilePointer(file_, 0, nullptr, FILE_END);
#else
    if (::ftruncate(fd_, static_cast<off_t>(new_size)) != 0) {
      return false;
    }
#endif
    file_size_ = new_size;
    dirty_.store(true, std::memory_order_release);
    return true;
  }

  /**
   * Atomically replaces the database file with @p data by writing to a
   * temporary file (@p path_ + ".tmp"), fsyncing it, then renaming it over
   * the original. On POSIX the rename is atomic. A crash at any point
   * leaves either the old file or the new file fully intact. On Windows
   * ReplaceFileA is used.
   *
   * The existing mapping and file handle are closed before the rename and
   * a fresh handle + mapping are opened afterwards.
   */
  auto rewrite(const std::vector<uint8_t> &data) -> bool {
    const std::string tmp_path = path_ + ".tmp";

#ifdef _WIN32
    HANDLE tmp = CreateFileA(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (tmp == INVALID_HANDLE_VALUE) {
      return false;
    }
    DWORD written = 0;
    const bool write_ok =
        WriteFile(tmp, data.data(), static_cast<DWORD>(data.size()), &written,
                  nullptr) &&
        written == static_cast<DWORD>(data.size());
    FlushFileBuffers(tmp);
    CloseHandle(tmp);

    if (!write_ok) {
      DeleteFileA(tmp_path.c_str());
      return false;
    }
#else
    const int tmp_fd =
        ::open(tmp_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) {
      return false;
    }
    const bool write_ok = ::write(tmp_fd, data.data(), data.size()) ==
                          static_cast<ssize_t>(data.size());
    ::fsync(tmp_fd);
    ::close(tmp_fd);

    if (!write_ok) {
      ::unlink(tmp_path.c_str());
      return false;
    }
#endif
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

#ifdef _WIN32
    if (!ReplaceFileA(path_.c_str(), tmp_path.c_str(), nullptr,
                      REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
      const DWORD err = GetLastError();

      if (err != ERROR_UNABLE_TO_MOVE_REPLACEMENT) {
        DeleteFileA(tmp_path.c_str());
      }

      file_ =
          CreateFileA(err == ERROR_UNABLE_TO_MOVE_REPLACEMENT ? tmp_path.c_str()
                                                              : path_.c_str(),
                      GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      SetFilePointer(file_, 0, nullptr, FILE_END);
      remap();
      return false;
    }
#else
    if (::rename(tmp_path.c_str(), path_.c_str()) != 0) {
      ::unlink(tmp_path.c_str());
      fd_ = ::open(path_.c_str(), O_RDWR | O_APPEND, 0644);
      remap();
      return false;
    }
#endif

#ifdef _WIN32
    file_ = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      return false;
    }
    SetFilePointer(file_, 0, nullptr, FILE_END);
#else
    fd_ = ::open(path_.c_str(), O_RDWR | O_APPEND, 0644);
    if (fd_ < 0) {
      return false;
    }
#endif
    return remap();
  }

  /**
   * Returns a pointer into the mapped region.
   * The caller must ensure the mapping is fresh by calling ensure_mapped()
   * (via DB) before dereferencing.
   */
  [[nodiscard]] auto ptr() const noexcept -> const uint8_t * { return ptr_; }

  /**
   * Returns true if the file has been written to since the last remap().
   * Safe to call from any thread holding at least a shared lock on mu_,
   * since only exclusive-lock holders can set this flag.
   */
  [[nodiscard]] auto is_dirty() const noexcept -> bool {
    return dirty_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto size() const noexcept -> size_t { return file_size_; }

private:
  void unmap() {
    if (!ptr_)
      return;
#ifdef _WIN32
    UnmapViewOfFile(ptr_);
    if (map_) {
      CloseHandle(map_);
      map_ = nullptr;
    }
#else
    ::munmap(ptr_, size_);
#endif
    ptr_ = nullptr;
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
};
} // namespace detail
/// @endcond

// --- transaction ---

class DB; // forward declaration

/**
 * @brief A staged batch of write operations, used inside DB::transaction().
 *
 * A @p Tx is constructed by DB::transaction() and passed to the callback.
 * Operations staged on it are only applied to the database if the
 * callback returns @p fluxen::commit. They are discarded on
 * @p fluxen::rollback or if the callback throws.
 *
 * @note Do not construct a @p Tx directly, use DB::transaction().
 *
 * @see DB::transaction()
 */
class Tx {
private:
  friend class DB;

  struct Op {
    std::string key;
    std::vector<uint8_t> val;
    bool is_delete;
  };
  std::vector<Op> ops_;

public:
  /**
   * @brief Stage a string value to be written.
   *
   * @param key   The key to write (max 255 bytes).
   * @param value The string value to store.
   */
  void put(std::string_view key, std::string_view value) {
    ops_.push_back({.key = std::string(key),
                    .val = std::vector<uint8_t>(value.begin(), value.end()),
                    .is_delete = false});
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
    requires(std::is_trivially_copyable_v<T> &&
             !std::is_convertible_v<T, std::string_view>)
  void put(std::string_view key, const T &value) {
    const auto *p = reinterpret_cast<const uint8_t *>(&value);
    ops_.push_back({.key = std::string(key),
                    .val = std::vector<uint8_t>(p, p + sizeof(T)),
                    .is_delete = false});
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
    ops_.push_back({.key = std::string(key), .val = {}, .is_delete = true});
  }
};

// --- DB ---

/**
 * @brief A persistent key-value database backed by a single file.
 *
 * Keys are UTF-8 strings up to 255 bytes. Values can be std::string,
 * string literals, or any trivially copyable type (int, float, struct, ...).
 *
 * The database is safe to use from multiple threads concurrently. Reads
 * proceed in parallel, and writes acquire an exclusive lock and are given
 * priority over waiting readers to prevent starvation. See the
 * @ref index "Thread safety" section in the main page for full details.
 *
 * @note @p DB is non-copyable. Create one instance per file.
 *
 * @par Example
 * @code
 *   fluxen::DB db("app.db");
 *
 *   db.put("hits", int64_t{0});
 *
 *   if (auto val = db.get<int64_t>("hits")) {
 *       std::cout << *val << std::endl;
 *   }
 * @endcode
 */
class DB {
private:
  detail::IndexMap index_;
  mutable detail::MappedFile file_;
  mutable std::shared_mutex mu_;
  mutable std::mutex sync_mutex_;

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
   *         header (i.e. was not created by fluxen).
   */
  explicit DB(std::string_view path) {
    if (!file_.open(path)) {
      throw std::runtime_error("fluxen: failed to open '" + std::string(path) +
                               "'");
    }

    if (file_.size() == 0) {
      init_file();
    } else {
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
  DB(const DB &) = delete;
  auto operator=(const DB &) -> DB & = delete;
  /// @endcond

  // --- WRITE ---

  /**
   * @brief Stores a string value under the given key.
   *
   * If the key already exists its value is overwritten. The write is
   * appended to the on-disk log immediately but is not explicitly fsynced.
   * Use transaction() if you require durability guarantees.
   *
   * Acquires an exclusive lock, blocking until all active readers have
   * finished.
   *
   * @param key   Key to write. Must be between 1 and 255 bytes.
   * @param value String value to store.
   */
  void put(std::string_view key, std::string_view value) {
    std::unique_lock lock(mu_);
    append_entry(key, reinterpret_cast<const uint8_t *>(value.data()),
                 static_cast<uint32_t>(value.size()), false);
  }

  /**
   * @brief Stores a trivially copyable value under the given key.
   *
   * The value is stored as its raw bytes via `memcpy`. Overwriting a key
   * with a value of a different size is valid. A subsequent get<T>() call
   * will return `nullopt` if the stored size does not match `sizeof(T)`.
   *
   * Acquires an exclusive lock, blocking until all active readers have
   * finished.
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
    requires(std::is_trivially_copyable_v<T> &&
             !std::is_convertible_v<T, std::string_view>)
  void put(std::string_view key, const T &value) {
    std::unique_lock lock(mu_);
    append_entry(key, reinterpret_cast<const uint8_t *>(&value),
                 static_cast<uint32_t>(sizeof(T)), false);
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
   * Acquires a shared lock, allowing concurrent calls from other readers.
   * If the memory-mapped file is stale from a recent write, this thread
   * will remap it before reading (see @p ensure_mapped()).
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
    std::shared_lock lock(mu_);
    ensure_mapped();

    auto it = index_.find(key);
    if (it == index_.end()) {
      return std::nullopt;
    }

    const auto &entry = it->second;

    if constexpr (std::is_same_v<T, std::string>) {
      const auto *ptr = file_.ptr() + entry.val_offset;
      return std::string(reinterpret_cast<const char *>(ptr), entry.val_len);
    } else {
      static_assert(std::is_trivially_copyable_v<T>,
                    "fluxen: T must be trivially copyable");
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
   * Acquires an exclusive lock, blocking until all active readers have
   * finished. Waiting writers take priority over new readers.
   *
   * @param key The key to delete.
   */
  void remove(std::string_view key) {
    std::unique_lock lock(mu_);
    append_entry(key, nullptr, 0, true);
  }

  // --- QUERY ---

  /**
   * @brief Returns true if the given key exists in the database.
   *
   * Acquires a shared lock, allowing concurrent calls from other readers.
   *
   * @param key The key to look up.
   * @return `true` if the key exists, `false` otherwise.
   */
  [[nodiscard]] auto has(std::string_view key) const -> bool {
    std::shared_lock lock(mu_);
    ensure_mapped();
    return index_.contains(key);
  }

  /**
   * @brief Iterates over all live key-value pairs.
   *
   * The callback is invoked once per key with a `string_view` of the key
   * and a `Bytes` span pointing directly into the memory-mapped file.
   * The iteration order is unspecified.
   *
   * Acquires a shared lock for the duration of iteration, allowing
   * concurrent readers. Write methods called from another thread will
   * block until iteration completes.
   *
   * @param fn Callback of the form `void(std::string_view key, Bytes val)`.
   *
   * @warning Do not call any write method from inside @p fn — the shared lock
   * is not reentrant with an exclusive lock and will deadlock. Copy keys out
   * first if you need to write during iteration. The @p Bytes span is only
   * valid for the duration of the callback; copy the data if you need it later.
   *
   * @par Example
   * @code
   *   db.each([](std::string_view key, fluxen::Bytes val) {
   *       std::string v(reinterpret_cast<const char*>(val.data()), val.size());
   *       std::cout << key << " = " << v << "\n";
   *   });
   * @endcode
   */
  void each(const std::function<void(std::string_view, Bytes)> &fn) const {
    std::shared_lock lock(mu_);
    ensure_mapped();
    for (const auto &[key, entry] : index_) {
      auto *ptr =
          reinterpret_cast<const std::byte *>(file_.ptr() + entry.val_offset);
      fn(key, Bytes{ptr, entry.val_len});
    }
  }

  /**
   * @brief Iterates over all keys that begin with the given prefix.
   *
   * A convenient way to implement namespaced key sets. For example, all
   * keys stored as `"user:1001"`, `"user:1002"`, etc. can be iterated with
   * `db.prefix("user:", fn)`.
   *
   * The callback and locking behaviour are identical to each(). The same
   * warnings about write methods and Bytes span lifetime apply.
   *
   * @param pfx The prefix string to filter by.
   * @param fn  Callback of the form `void(std::string_view key, Bytes val)`.
   *
   * @note Iteration is O(total keys), not O(matching keys), because the
   *       underlying index is a hash map with no sorted order.
   *
   * @par Example
   * @code
   *   db.put("user:james", "admin");
   *   db.put("user:bryce",   "viewer");
   *   db.put("config:x",   "value");
   *
   *   db.prefix("user:", [](std::string_view key, fluxen::Bytes val) {
   *       // called for user:james and user:bryce only
   *   });
   * @endcode
   */
  void prefix(std::string_view pfx,
              const std::function<void(std::string_view, Bytes)> &fn) const {
    std::shared_lock lock(mu_);
    ensure_mapped();
    for (const auto &[key, entry] : index_) {
      if (key.starts_with(pfx)) {
        auto *ptr =
            reinterpret_cast<const std::byte *>(file_.ptr() + entry.val_offset);
        fn(key, Bytes{ptr, entry.val_len});
      }
    }
  }

  // --- TRANSACTION ---

  /**
   * @brief Executes a batch of operations atomically.
   *
   * The callback receives a Tx object on which any number of put() and
   * remove() calls can be staged. Returning `fluxen::commit` applies all
   * operations atomically and flushes them to disk. Returning
   * `fluxen::rollback` discards all staged operations and the database is
   * left completely unchanged.
   *
   * If the callback throws, the exception propagates to the caller and all
   * staged operations are discarded (equivalent to rollback).
   *
   * @param fn A callable of the form `TxResult(Tx&)`.
   *
   * @note All staged operations are serialized into a single buffer and written
   * with one syscall and one fsync. This makes transaction() significantly
   * faster than the equivalent number of individual put() calls when writing
   * many keys at once. The callback runs without holding the database mutex,
   * so other threads may write between callback return and commit; the
   * transaction's own operations are always applied atomically.
   *
   * @par Example
   * @code
   *   db.transaction([](fluxen::Tx& tx) {
   *       tx.put("balance",  int32_t{500});
   *       tx.put("currency", std::string("USD"));
   *       tx.remove("old_session");
   *       return fluxen::commit;
   *   });
   * @endcode
   *
   * @par Rolling back
   * @code
   *   db.transaction([&](fluxen::Tx& tx) {
   *       tx.put("x", new_value);
   *       if (!validate(new_value)) {
   *           return fluxen::rollback;   // nothing is written
   *       }
   *       return fluxen::commit;
   *   });
   * @endcode
   */
  void transaction(const std::function<TxResult(Tx &)> &fn) {
    Tx tx;
    TxResult result = fn(tx);
    if (result == rollback) {
      return;
    }

    std::vector<uint8_t> batch;
    batch.reserve(tx.ops_.size() * 64);
    for (const auto& op : tx.ops_) {
        detail::EntryHeader hdr{
            .flags   = op.is_delete ? detail::FLAG_TOMB : detail::FLAG_LIVE,
            .key_len = static_cast<uint8_t>(op.key.size()),
            .val_len = op.is_delete ? 0u : static_cast<uint32_t>(op.val.size()),
        };
        uint8_t raw[detail::HEADER_SIZE];
        detail::encode_header(raw, hdr);
        batch.insert(batch.end(), raw, raw + detail::HEADER_SIZE);
        batch.insert(batch.end(), op.key.begin(), op.key.end());
        if (!op.is_delete) {
            batch.insert(batch.end(), op.val.begin(), op.val.end());
        }
    }

    std::unique_lock lock(mu_);
    file_.append(batch.data(), batch.size());
    file_.sync();

    size_t base = file_.size() - batch.size();
    size_t pos  = 0;
    for (const auto& op : tx.ops_) {
        pos += detail::HEADER_SIZE + op.key.size();
        if (op.is_delete) {
            index_.erase(op.key);
        } else {
            index_[op.key] = { .val_offset=base + pos, .val_len=static_cast<uint32_t>(op.val.size()) };
            pos += op.val.size();
        }
    }
  }

  // --- MAINTENANCE ---

  /**
   * @brief Rewrites the database file retaining only live entries.
   *
   * The append-only log accumulates dead entries over time as keys are
   * overwritten or deleted. `compact()` rewrites the file with only the
   * current live values, reclaiming that space.
   *
   * The rewrite is crash-safe. Compacted data is first written to a
   * temporary file (@p <path>.tmp), fsynced, and then atomically renamed
   * over the original. A crash at any point leaves either the original
   * or the fully written new file intact. That way data is never lost.
   *
   * Acquires an exclusive lock, blocking all reads and writes for the
   * duration of the rewrite. The database remains fully consistent. If
   * the process is interrupted mid-compact the old file is still intact.
   *
   * There is no automatic compaction. Call this periodically if your
   * workload involves many overwrites or deletions.
   *
   * @throws std::runtime_error If the file rewrite fails.
   *
   * @warning Invalidates all previously returned Bytes spans and pointers into
   * the mapped region. Reacquire any needed data after compaction.
   */
  void compact() {
    std::unique_lock lock(mu_);

    if (file_.is_dirty()) {
      file_.remap();
    }

    std::vector<uint8_t> buf;
    buf.reserve(file_.size());

    buf.insert(buf.end(), detail::MAGIC, detail::MAGIC + sizeof(detail::MAGIC));

    detail::IndexMap new_index;
    for (const auto &[key, entry] : index_) {
      detail::EntryHeader hdr{
          .flags = detail::FLAG_LIVE,
          .key_len = static_cast<uint8_t>(key.size()),
          .val_len = entry.val_len,
      };

      uint8_t raw[detail::HEADER_SIZE];
      detail::encode_header(raw, hdr);

      size_t val_off = buf.size() + detail::HEADER_SIZE + key.size();

      buf.insert(buf.end(), raw, raw + detail::HEADER_SIZE);
      buf.insert(buf.end(), key.begin(), key.end());

      const auto *val_ptr = file_.ptr() + entry.val_offset;
      buf.insert(buf.end(), val_ptr, val_ptr + entry.val_len);

      new_index[key] = {.val_offset = val_off, .val_len = entry.val_len};
    }

    if (!file_.rewrite(buf)) {
      throw std::runtime_error("fluxen: compact failed during rewrite");
    }

    index_ = std::move(new_index);
  }

  // --- DIAGNOSTICS ---

  /**
   * @brief Returns the number of live keys currently stored.
   *
   * Acquires a shared lock, allowing concurrent calls from other readers.
   *
   * @return Number of keys in the index.
   */
  [[nodiscard]] auto key_count() const -> size_t {
    std::shared_lock lock(mu_);
    return index_.size();
  }

  /**
   * @brief Returns the current size of the database file in bytes.
   *
   * This includes space used by dead entries (overwritten or deleted values)
   * that have not yet been reclaimed by compact(). It is an upper bound on
   * the live data size.
   *
   * Acquires a shared lock, allowing concurrent calls from other readers.
   *
   * @return File size in bytes.
   */
  [[nodiscard]] auto file_size() const -> size_t {
    std::shared_lock lock(mu_);
    return file_.size();
  }

private:
  /// Writes the magic header to a newly created file.
  void init_file() { file_.append(detail::MAGIC, sizeof(detail::MAGIC)); }

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
      throw std::runtime_error("fluxen: file too small to be valid");
    }

    if (std::memcmp(file_.ptr(), detail::MAGIC, sizeof(detail::MAGIC)) != 0) {
      throw std::runtime_error(
          "fluxen: bad magic. File was not created by fluxen");
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

      std::string key(reinterpret_cast<const char *>(file_.ptr() + pos),
                      hdr.key_len);
      pos += hdr.key_len;

      if (hdr.flags == detail::FLAG_TOMB) {
        index_.erase(key);
      } else {
        index_[key] = {.val_offset = pos, .val_len = hdr.val_len};
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
  void append_entry(std::string_view key, const uint8_t *val, uint32_t val_len,
                    bool tombstone) {
    assert(key.size() <= detail::MAX_KEY &&
           "fluxen: key exceeds 255-byte limit");

    detail::EntryHeader hdr{
        .flags = tombstone ? detail::FLAG_TOMB : detail::FLAG_LIVE,
        .key_len = static_cast<uint8_t>(key.size()),
        .val_len = val_len,
    };

    std::vector<uint8_t> buf;
    buf.resize(detail::HEADER_SIZE + key.size() + val_len);
    detail::encode_header(buf.data(), hdr);
    std::memcpy(buf.data() + detail::HEADER_SIZE, key.data(), key.size());
    if (val && val_len) {
      std::memcpy(buf.data() + detail::HEADER_SIZE + key.size(), val, val_len);
    }

    file_.append(buf.data(), buf.size());

    if (tombstone) {
      if (auto it = index_.find(key); it != index_.end()) {
        index_.erase(it);
      }
    } else {
      index_[std::string(key)] = {.val_offset = file_.size() - val_len,
                                  .val_len = val_len};
    }
  }

  /**
   * @brief Ensures the memory-mapped file reflects the latest on-disk data.
   *
   * Must be called at the top of every read method, after acquiring a
   * shared lock on @p mu_.
   *
   * Fast path: if the mapping is not dirty, returns immediately with no
   * locking overhead.
   *
   * Slow path: acquires @p sync_mutex_ so that exactly one reader among
   * all concurrent readers performs the remap. The rest block on
   * @p sync_mutex_ and skip the remap when they acquire it, since the
   * first thread will have already cleared the dirty flag.
   *
   * This is safe to call under a shared lock because @p dirty_ is only
   * ever set by writers, who hold an exclusive lock, and no writer
   * can be active while any reader holds a shared lock.
   */
  void ensure_mapped() const {
    if (!file_.is_dirty()) {
      return;
    }

    std::unique_lock sync_lock(sync_mutex_);
    if (file_.is_dirty()) {
      file_.remap();
    }
  }
};

} // namespace fluxen
