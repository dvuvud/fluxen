#pragma once
#include <filesystem>
#include <format>
#include <atomic>
#include <string>

inline auto make_temp_path(std::string_view tag = "tmp") -> std::string {
    static std::atomic<uint32_t> counter{0};
    return (std::filesystem::temp_directory_path()
            / std::format("tinydb_{}_{}", tag, counter++)).string();
}

