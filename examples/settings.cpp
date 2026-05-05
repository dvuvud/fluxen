// A minimal build-tool configuration store built with tinydb.
//
// Demonstrates using tinydb as a persistent settings backend for a
// CLI tool. Settings are written once and remembered across runs.
//
// Usage:
//   settings set compiler clang++
//   settings set standard 20
//   settings set warnings all
//   settings set outdir build
//   settings get compiler
//   settings list
//   settings reset

#include "../tinydb.hpp"

#include <iostream>
#include <string>
#include <string_view>

static constexpr std::string_view DB_PATH = ".cppbuild.db";

static const std::string VALID_KEYS[] = {
    "compiler", "standard", "warnings", "outdir"
};

static auto is_valid_key(std::string_view key) -> bool {
    for (const auto& k : VALID_KEYS) {
        if (k == key) {
            return true;
        }
    }
    return false;
}

static void print_usage() {
    std::cout <<
        "usage:\n"
        "  settings set <key> <value>   save a setting\n"
        "  settings get <key>           read a setting\n"
        "  settings list                show all settings\n"
        "  settings reset               clear all settings\n"
        "\n"
        "keys: compiler, standard, warnings, outdir\n";
}

auto main(int argc, char* argv[]) -> int {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    tinydb::DB db(DB_PATH);
    std::string_view cmd = argv[1];

    if (cmd == "set") {
        if (argc != 4) {
            std::cerr << "usage: settings set <key> <value>\n";
            return 1;
        }

        std::string_view key   = argv[2];
        std::string_view value = argv[3];

        if (!is_valid_key(key)) {
            std::cerr << "unknown key '" << key << "'. "
                      << "valid keys: compiler, standard, warnings, outdir\n";
            return 1;
        }

        db.put(key, value);
        std::cout << key << " = " << value << "\n";
        return 0;
    }

    if (cmd == "get") {
        if (argc != 3) {
            std::cerr << "usage: settings get <key>\n";
            return 1;
        }

        std::string_view key = argv[2];

        if (!is_valid_key(key)) {
            std::cerr << "unknown key '" << key << "'\n";
            return 1;
        }

        if (auto val = db.get(key)) {
            std::cout << *val << "\n";
        } else {
            std::cout << "(not set)\n";
        }
        return 0;
    }

    if (cmd == "list") {
        bool any = false;
        for (const auto& key : VALID_KEYS) {
            if (auto val = db.get(key)) {
                std::cout << key << " = " << *val << "\n";
                any = true;
            }
        }
        if (!any) {
            std::cout << "no settings saved. run 'settings set' to configure.\n";
        }
        return 0;
    }

    if (cmd == "reset") {
        for (const auto& key : VALID_KEYS) {
            db.remove(key);
        }
        std::cout << "all settings cleared.\n";
        return 0;
    }

    std::cerr << "unknown command '" << cmd << "'\n\n";
    print_usage();
    return 1;
}
