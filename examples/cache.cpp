// A persistent shell command output cache built with tinydb.
//
// Demonstrates usage of tinydb to memoize expensive or slow commands.
// The output of any shell command is cached by its
// command string and returned instantly on subsequent runs.
//
// Usage:
//   cache run <command>          run command, print output (cached after first run)
//   cache invalidate <command>   bust the cache for a specific command
//   cache clear                  wipe all cached output
//   cache list                   show all cached commands

#include "tinydb.hpp"

#include <array>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
  #define popen _popen
  #define pclose _pclose
#endif

static constexpr std::string_view DB_PATH = ".cmdcache.db";

static auto run_command(const std::string& cmd) -> std::string {
    std::array<char, 256> buf{};
    std::string output;

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("cache: failed to run command");
    }

    while (std::fgets(buf.data(), buf.size(), pipe)) {
        output += buf.data();
    }

    int exit_code = ::pclose(pipe);
    if (exit_code != 0) {
        throw std::runtime_error(
            "cache: command exited with code " + std::to_string(exit_code)
        );
    }

    return output;
}

static void print_usage() {
    std::cout <<
        "usage:\n"
        "  cache run <command>          run and cache a command's output\n"
        "  cache invalidate <command>   remove a specific entry from the cache\n"
        "  cache list                   list all cached commands\n"
        "  cache clear                  wipe the entire cache\n";
}

auto main(int argc, char* argv[]) -> int {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    tinydb::DB db(DB_PATH);
    std::string_view cmd = argv[1];

    if (cmd == "run") {
        if (argc < 3) {
            std::cerr << "usage: cache run <command>\n";
            return 1;
        }

        std::string command = argv[2];
        for (int i = 3; i < argc; ++i) {
            command += ' ';
            command += argv[i];
        }

        if (auto cached = db.get(command)) {
            std::cout << *cached;
            return 0;
        }

        try {
            std::string output = run_command(command);
            db.put(command, output);
            std::cout << output;
        } catch (const std::runtime_error& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }

        return 0;
    }

    if (cmd == "invalidate") {
        if (argc < 3) {
            std::cerr << "usage: cache invalidate <command>\n";
            return 1;
        }

        std::string command = argv[2];
        for (int i = 3; i < argc; ++i) {
            command += ' ';
            command += argv[i];
        }

        if (!db.has(command)) {
            std::cout << "not cached: " << command << "\n";
            return 0;
        }

        db.remove(command);
        std::cout << "invalidated: " << command << "\n";
        return 0;
    }

    if (cmd == "list") {
        if (db.key_count() == 0) {
            std::cout << "cache is empty.\n";
            return 0;
        }

        db.each([](std::string_view key, tinydb::Bytes) -> void {
            std::cout << key << "\n";
        });

        return 0;
    }

    if (cmd == "clear") {
        std::vector<std::string> keys;
        db.each([&](std::string_view key, tinydb::Bytes) -> void {
            keys.emplace_back(key);
        });
        for (const auto& key : keys) {
            db.remove(key);
        }
        db.compact();
        std::cout << "cache cleared.\n";
        return 0;
    }

    std::cerr << "unknown command '" << cmd << "'\n\n";
    print_usage();
    return 1;
}
