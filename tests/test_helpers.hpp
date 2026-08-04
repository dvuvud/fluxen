#pragma once

#include <filesystem>
#include <format>
#include <random>
#include <string>

inline std::string make_temp_path(std::string_view tag = "tmp") {
  static thread_local std::mt19937 gen{ [] { std::random_device rd; return rd(); }() };
  const auto res = gen();
  // if the temp directory path includes any non-ascii characters, the
  // conversion from std::filesystem::path to std::string may mangle the path
  return (std::filesystem::temp_directory_path() /
          std::format("fluxen_{}_{:04X}-{:04X}", tag, res & 0xFFFF, (res >> 16) & 0xFFFF)).string();
}
