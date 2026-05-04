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

/* Raw byte view into mmap'd file */
using Bytes = std::span<const std::byte>;

// --- internal ---

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
inline EntryHeader decode_header(const uint8_t in[HEADER_SIZE]) noexcept {
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
    MappedFile& operator=(const MappedFile&) = delete;

    bool open(std::string_view path) {
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

    bool remap() {
        unmap();
        size_ = file_size();
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

    bool append(const void* data, size_t len) {
        if (len == 0) {
            return true;
        }
#ifdef _WIN32
        DWORD written = 0;
        SetFilePointer(file_, 0, nullptr, FILE_END);
        return WriteFile(file_, data, static_cast<DWORD>(len), &written, nullptr)
        && written == static_cast<DWORD>(len);
#else
        return ::write(fd_, data, len) == static_cast<ssize_t>(len);
#endif
    }

    void sync() {
#ifdef _WIN32
        FlushFileBuffers(file_);
#else
        ::fsync(fd_);
#endif
    }

    bool rewrite(const std::vector<uint8_t>& data) {
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

    const uint8_t* ptr()  const noexcept { return ptr_;  }
    size_t         size() const noexcept { return size_; }

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

    size_t file_size() const noexcept {
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
    uint8_t*    ptr_  = nullptr;
    size_t      size_ = 0;
    std::string path_;
};
}   // namespace detail

// --- transaction ---

class DB;

class Tx {
public:
    /* Store a string value */
    void put(std::string_view key, std::string_view value) {
        ops_.push_back({ std::string(key),
                         std::vector<uint8_t>(value.begin(), value.end()),
                         false });
    }

    /* Store any trivially copyable, non-string type */
    template <typename T>
        requires (std::is_trivially_copyable_v<T>
               && !std::is_convertible_v<T, std::string_view>)
    void put(std::string_view key, const T& value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        ops_.push_back({ std::string(key),
                         std::vector<uint8_t>(p, p + sizeof(T)),
                         false });
    }

    /* Mark a key for deletion */
    void remove(std::string_view key) {
        ops_.push_back({ std::string(key), {}, true });
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

class DB {
public:
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

    ~DB() = default;
    DB(const DB&)            = delete;
    DB& operator=(const DB&) = delete;

    /* Store a string value */
    void put(std::string_view key, std::string_view value) {
        std::lock_guard lock(mu_);
        append_entry(
            key,
            reinterpret_cast<const uint8_t*>(value.data()),
            static_cast<uint32_t>(value.size()),
            false
        );
    }

    /* Store any trivially copyable, non-string type (int, float, struct, ...) */
    template <typename T>
        requires (std::is_trivially_copyable_v<T>
               && !std::is_convertible_v<T, std::string_view>)
    void put(std::string_view key, const T& value) {
        std::lock_guard lock(mu_);
        append_entry(
            key,
            reinterpret_cast<const uint8_t*>(&value),
            static_cast<uint32_t>(sizeof(T)),
            false
        );
    }

    /**
     * Retrieve a value.
     * Returns std::nullopt if the key doesn't exist
     */
    template <typename T = std::string>
    std::optional<T> get(std::string_view key) const {
        std::lock_guard lock(mu_);
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

    /* Remove a key. Does nothing if the key doesn't exist */
    void remove(std::string_view key) {
        std::lock_guard lock(mu_);
        if (!index_.contains(std::string(key))) {
            return;
        }
        append_entry(key, nullptr, 0, true);
    }

    /* Returns true if the key exists */
    [[nodiscard]] bool has(std::string_view key) const {
        std::lock_guard lock(mu_);
        return index_.contains(std::string(key));
    }

    /**
     * Iterate all live keys. Callback receives (key, raw_bytes)
     * Use Bytes::data() and Bytes::size() to access the value bytes.
     */
    void each(std::function<void(std::string_view, Bytes)> fn) {
        std::lock_guard lock(mu_);
        for (const auto& [key, entry] : index_) {
            auto* ptr = reinterpret_cast<const std::byte*>(file_.ptr() + entry.val_offset);
            fn(key, Bytes{ ptr, entry.val_len });
        }
    }

    /**
     * Iterate only keys that start with the given prefix.
     *
     *  db.put("user:rick", ...);
     *  db.put("user:john",   ...);
     *  db.prefix("user:", [](auto key, auto val) { ... });
     */
    void prefix(std::string_view pfx, std::function<void(std::string_view, Bytes)> fn) {
        std::lock_guard lock(mu_);
        for (const auto& [key, entry] : index_) {
            if (key.starts_with(pfx)) {
                auto* ptr = reinterpret_cast<const std::byte*>(file_.ptr() + entry.val_offset);
                fn(key, Bytes{ ptr, entry.val_len });
            }
        }
    }

    /**
     * Rewrite the file by retaining only live entries and reclaiming space from
     * deleted and overwritten ones. Blocks all reads and writes while running.
     * Safe to call at any time.
     */
    void compact() {
        std::lock_guard lock(mu_);

        std::vector<uint8_t> buf;
        buf.reserve(file_.size());

        buf.insert(
            buf.end(),
            detail::MAGIC,
            detail::MAGIC + sizeof(detail::MAGIC)
        );

        // Serialise all live entries and track their new offsets
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

            new_index[key] = { val_off, entry.val_len };
        }

        if (!file_.rewrite(buf)) {
            throw std::runtime_error("tinydb: compact failed during rewrite");
        }

        index_ = std::move(new_index);
    }

    /* Number of live keys currently stored */
    [[nodiscard]] size_t key_count() const {
        std::lock_guard lock(mu_);
        return index_.size();
    }

    /* Estimated file size in bytes (includes dead entries until compact()) */
    [[nodiscard]] size_t file_size() const {
        std::lock_guard lock(mu_);
        return file_.size();
    }

private:

    /* Write the magic header to a brand-new file */
    void init_file() {
        file_.append(detail::MAGIC, sizeof(detail::MAGIC));
        file_.remap();
    }

    /**
     * Scan the on-disk log and rebuild the in-memory index
     * Called once on open. Last write for a given key wins
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
                index_[key] = { pos, hdr.val_len };
            }

            pos += hdr.val_len;
        }
    }

    /**
     * Append one entry to the file and update the in-memory index_
     * @note Caller must hold mu_
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
        file_.remap();

        if (tombstone) {
            index_.erase(std::string(key));
        } else {
            // Bytes were appended last so offset is file size - value length
            size_t val_off = file_.size() - val_len;
            index_[std::string(key)] = { val_off, val_len };
        }
    }

    detail::MappedFile file_;
    std::unordered_map<std::string, detail::IndexEntry> index_;
    mutable std::mutex mu_;
};

}   // namespace tinydb
