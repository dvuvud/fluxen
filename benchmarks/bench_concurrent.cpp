#include "fluxen.hpp"
#include "tests/test_helpers.hpp"
#include <benchmark/benchmark.h>
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>

namespace {

auto make_keys(int n) -> std::vector<std::string> {
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) v.push_back("key:" + std::to_string(i));
    return v;
}

auto make_values(int n) -> std::vector<std::string> {
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) v.push_back("value:" + std::to_string(i));
    return v;
}

auto open_sqlite_wal(const std::string& path) -> sqlite3* {
    sqlite3* db = nullptr;
    sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    return db;
}

void sqlite_populate(const std::string& path, const std::vector<std::string>& keys, const std::vector<std::string>& vals) {
    sqlite3* db = open_sqlite_wal(path);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT); BEGIN;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv VALUES (?, ?);", -1, &stmt, nullptr);
    for (size_t i = 0; i < keys.size(); ++i) {
        sqlite3_bind_text(stmt, 1, keys[i].c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, vals[i].c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

static std::unique_ptr<fluxen::DB> g_fluxen;
static std::string g_db_path;

} // namespace

// concurrent reads

static void BM_fluxen_ConcurrentReads(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);

    if (state.thread_index() == 0) {
        g_db_path = make_temp_path("cr_tdb");
        auto vals = make_values(n);
        {
            fluxen::DB setup(g_db_path);
            for (int i = 0; i < n; ++i) setup.put(keys[i], vals[i]);
        }
        g_fluxen = std::make_unique<fluxen::DB>(g_db_path);
    }

    for (auto _ : state) {
        for (int i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(g_fluxen->get(keys[i]));
        }
    }

    if (state.thread_index() == 0) {
        g_fluxen.reset();
        std::filesystem::remove(g_db_path);
    }
    state.SetItemsProcessed(state.iterations() * n);
}

static void BM_SQLite_ConcurrentReads(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);

    if (state.thread_index() == 0) {
        g_db_path = make_temp_path("cr_sqlite");
        sqlite_populate(g_db_path, keys, make_values(n));
    }

    sqlite3* db = open_sqlite_wal(g_db_path);
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?;", -1, &sel, nullptr);

    for (auto _ : state) {
        for (int i = 0; i < n; ++i) {
            sqlite3_bind_text(sel, 1, keys[i].c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(sel) == SQLITE_ROW) {
                benchmark::DoNotOptimize(sqlite3_column_text(sel, 0));
            }
            sqlite3_reset(sel);
        }
    }

    sqlite3_finalize(sel);
    sqlite3_close(db);

    if (state.thread_index() == 0) {
        std::filesystem::remove(g_db_path);
    }
    state.SetItemsProcessed(state.iterations() * n);
}

// read/write contention

static void BM_fluxen_ReadWriteContention(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    if (state.thread_index() == 0) {
        g_db_path = make_temp_path("rwc_tdb");
        g_fluxen = std::make_unique<fluxen::DB>(g_db_path);
    }

    long local_reads = 0;
    for (auto _ : state) {
        if (state.thread_index() == 0) {
            for (int i = 0; i < n; ++i) {
                g_fluxen->put(keys[i], vals[i]);
            }
        } else {
            for (int i = 0; i < n; ++i) {
                benchmark::DoNotOptimize(g_fluxen->get(keys[i]));
                local_reads++;
            }
        }
    }

    if (state.thread_index() == 0) {
        g_fluxen.reset();
        std::filesystem::remove(g_db_path);
    }
    state.counters["reads"] = benchmark::Counter(local_reads, benchmark::Counter::kIsRate);
}

static void BM_SQLite_ReadWriteContention(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    if (state.thread_index() == 0) {
        g_db_path = make_temp_path("rwc_sqlite");
        sqlite_populate(g_db_path, keys, vals);
    }

    sqlite3* db = open_sqlite_wal(g_db_path);
    long local_reads = 0;

    if (state.thread_index() == 0) {
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv VALUES (?, ?);", -1, &ins, nullptr);
        for (auto _ : state) {
            for (int i = 0; i < n; ++i) {
                sqlite3_bind_text(ins, 1, keys[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 2, vals[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_step(ins);
                sqlite3_reset(ins);
            }
        }
        sqlite3_finalize(ins);
    } else {
        sqlite3_stmt* sel = nullptr;
        sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?;", -1, &sel, nullptr);
        for (auto _ : state) {
            for (int i = 0; i < n; ++i) {
                sqlite3_bind_text(sel, 1, keys[i].c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(sel) == SQLITE_ROW) {
                    benchmark::DoNotOptimize(sqlite3_column_text(sel, 0));
                }
                sqlite3_reset(sel);
                local_reads++;
            }
        }
        sqlite3_finalize(sel);
    }

    sqlite3_close(db);
    if (state.thread_index() == 0) {
        std::filesystem::remove(g_db_path);
    }
    state.counters["reads"] = benchmark::Counter(local_reads, benchmark::Counter::kIsRate);
}

// writer contention

static void BM_fluxen_WriterContention(benchmark::State& state) {
    const int n = state.range(0);
    const int keys_per_thread = n / state.threads();
    auto keys = make_keys(n);
    auto vals = make_values(n);

    if (state.thread_index() == 0) {
        g_db_path = make_temp_path("wc_tdb");
        g_fluxen = std::make_unique<fluxen::DB>(g_db_path);
    }

    const int start = state.thread_index() * keys_per_thread;
    for (auto _ : state) {
        for (int i = start; i < start + keys_per_thread; ++i) {
            g_fluxen->put(keys[i], vals[i]);
        }
    }

    if (state.thread_index() == 0) {
        g_fluxen.reset();
        std::filesystem::remove(g_db_path);
    }
    state.SetItemsProcessed(state.iterations() * keys_per_thread);
}

static void BM_SQLite_WriterContention(benchmark::State& state) {
    const int n = state.range(0);
    const int keys_per_thread = n / state.threads();
    auto keys = make_keys(n);
    auto vals = make_values(n);

    if (state.thread_index() == 0) {
        g_db_path = make_temp_path("wc_sqlite");
        sqlite3* init = open_sqlite_wal(g_db_path);
        sqlite3_exec(init, "CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT);", nullptr, nullptr, nullptr);
        sqlite3_close(init);
    }

    sqlite3* db = open_sqlite_wal(g_db_path);
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv VALUES (?, ?);", -1, &ins, nullptr);

    const int start = state.thread_index() * keys_per_thread;
    for (auto _ : state) {
        for (int i = start; i < start + keys_per_thread; ++i) {
            sqlite3_bind_text(ins, 1, keys[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 2, vals[i].c_str(), -1, SQLITE_STATIC);
            int rc;
            do {
                rc = sqlite3_step(ins);
            } while (rc == SQLITE_BUSY);
            sqlite3_reset(ins);
        }
    }

    sqlite3_finalize(ins);
    sqlite3_close(db);
    if (state.thread_index() == 0) std::filesystem::remove(g_db_path);
    state.SetItemsProcessed(state.iterations() * keys_per_thread);
}

// registration

#define COMMON_ARGS Arg(1000)->Arg(10000)->ThreadRange(2, 16)

BENCHMARK(BM_fluxen_ConcurrentReads)->COMMON_ARGS;
BENCHMARK(BM_SQLite_ConcurrentReads)->COMMON_ARGS;
BENCHMARK(BM_fluxen_ReadWriteContention)->COMMON_ARGS;
BENCHMARK(BM_SQLite_ReadWriteContention)->COMMON_ARGS;
BENCHMARK(BM_fluxen_WriterContention)->COMMON_ARGS;
BENCHMARK(BM_SQLite_WriterContention)->COMMON_ARGS;
