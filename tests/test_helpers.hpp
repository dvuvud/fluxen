#pragma once

#include <atomic>
#include <filesystem>
#include <string>

inline auto make_temp_path(std::string_view tag = "tmp") -> std::string {
  static std::atomic<uint32_t> counter{0};
  return (std::filesystem::temp_directory_path() /
          ("fluxen_" + std::string(tag) + "_" + std::to_string(counter++)))
      .string();
}
