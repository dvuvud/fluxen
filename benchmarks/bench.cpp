#include "tests/test_helpers.hpp"
#include "tinydb.hpp"

#include <benchmark/benchmark.h>
#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>

static auto make_keys(int n) -> std::vector<std::string> {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (int i = 0; i < n; ++i) {
        keys.push_back("key:" + std::to_string(i));
    }
    return keys;
}

static auto make_values(int n) -> std::vector<std::string> {
    std::vector<std::string> vals;
    vals.reserve(n);
    for (int i = 0; i < n; ++i) {
        vals.push_back("value:" + std::to_string(i));
    }
    return vals;
}

/**
 * SQLite in WAL mode with synchronous=OFF. No fsyncs on individual writes
 * which matches tinydb's bare put().
 */
static auto open_sqlite_no_sync(const std::string& path) -> sqlite3* {
    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_exec(
        db,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=OFF;",
        nullptr, nullptr, nullptr
    );
    return db;
}

/**
 * SQLite in WAL mode with synchronous=NORMAL. Checkpoints sync but individual
 * commits don't. Matches tinydb's transaction(), which fsyncs once at the end.
 */
static auto open_sqlite_normal_sync(const std::string& path) -> sqlite3* {
    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_exec(
        db,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;",
        nullptr, nullptr, nullptr
    );
    return db;
}

/**
 * Sequential Write: individual writes, no explicit transaction
 *
 * One write at a time, no batching on either side. tinydb appends to the log
 * and SQLite auto-commits each INSERT. Neither side fsyncs between writes.
 * Baseline cost of a single put in each system.
 */

static void BM_tinydb_SequentialWrite(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("write");
        {
            tinydb::DB db(path);
            for (int i = 0; i < n; ++i) {
                db.put(keys[i], vals[i]);
            }
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_tinydb_SequentialWrite)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_SQLite_SequentialWrite(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("sqlite_write.db");
        {
            sqlite3* db = open_sqlite_no_sync(path);
            sqlite3_exec(
                db,
                "CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);",
                nullptr, nullptr, nullptr
            );

            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(
                db,
                "INSERT INTO kv (k, v) VALUES (?, ?);",
                -1, &stmt, nullptr
            );

            for (int i = 0; i < n; ++i) {
                sqlite3_bind_text(stmt, 1, keys[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, vals[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
            }

            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SQLite_SequentialWrite)->Arg(1000)->Arg(10000)->Arg(100000);

/**
 * Bulk Write: all keys in one transaction
 *
 * Both sides batch all writes into a single atomic transaction.  This is
 * SQLite's strong suit: it amortises B-tree and WAL overhead over many rows.
 * tinydb uses its transaction() API which fsyncs at the end and SQLite uses
 * synchronous=NORMAL which also syncs at checkpoint but not per-commit.
 */

static void BM_tinydb_BulkWrite(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("bulk");
        {
            tinydb::DB db(path);
            db.transaction([&](tinydb::Tx& tx) -> tinydb::TxResult {
                for (int i = 0; i < n; ++i) {
                    tx.put(keys[i], vals[i]);
                }
                return tinydb::commit;
            });
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_tinydb_BulkWrite)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_SQLite_BulkWrite(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("sqlite_bulk.db");
        {
            sqlite3* db = open_sqlite_normal_sync(path);
            sqlite3_exec(
                db,
                "CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);",
                nullptr, nullptr, nullptr
            );

            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(
                db,
                "INSERT INTO kv (k, v) VALUES (?, ?);",
                -1, &stmt, nullptr
            );

            sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
            for (int i = 0; i < n; ++i) {
                sqlite3_bind_text(stmt, 1, keys[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, vals[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
            }
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);

            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SQLite_BulkWrite)->Arg(1000)->Arg(10000)->Arg(100000);

/**
 * Sequential Read
 *
 * DB is pre-loaded and the connection is open before the clock starts.
 * Pure read throughput with no open cost and no index rebuild paid during timing.
 */

static void BM_tinydb_SequentialRead(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    auto path = make_temp_path("read");
    {
        tinydb::DB setup(path);
        for (int i = 0; i < n; ++i) {
            setup.put(keys[i], vals[i]);
        }
    }

    tinydb::DB db(path);
    for (auto _ : state) {
        for (int i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(db.get(keys[i]));
        }
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_tinydb_SequentialRead)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_SQLite_SequentialRead(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);
    auto path = make_temp_path("sqlite_read.db");

    {
        sqlite3* db = open_sqlite_no_sync(path);
        sqlite3_exec(
            db,
            "CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);",
            nullptr, nullptr, nullptr
        );
        sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(
            db,
            "INSERT INTO kv (k, v) VALUES (?, ?);",
            -1, &ins, nullptr
        );
        for (int i = 0; i < n; ++i) {
            sqlite3_bind_text(ins, 1, keys[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 2, vals[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        sqlite3_finalize(ins);
        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    sqlite3* db = open_sqlite_normal_sync(path);
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
    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SQLite_SequentialRead)->Arg(1000)->Arg(10000)->Arg(100000);

/**
 * Random Read
 *
 * Same as sequential read, but keys are visited in a shuffled order to stress
 * cache locality. The shuffle is fixed-seed so results are reproducible.
 */

static void BM_tinydb_RandomRead(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(order.begin(), order.end(), rng);

    auto path = make_temp_path("rread");
    {
        tinydb::DB setup(path);
        for (int i = 0; i < n; ++i) {
            setup.put(keys[i], vals[i]);
        }
    }

    tinydb::DB db(path);
    for (auto _ : state) {
        for (int idx : order) {
            benchmark::DoNotOptimize(db.get(keys[idx]));
        }
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_tinydb_RandomRead)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_SQLite_RandomRead(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(order.begin(), order.end(), rng);

    auto path = make_temp_path("sqlite_rread.db");
    {
        sqlite3* tmp = open_sqlite_no_sync(path);
        sqlite3_exec(
            tmp,
            "CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);",
            nullptr, nullptr, nullptr
        );
        sqlite3_exec(tmp, "BEGIN;", nullptr, nullptr, nullptr);
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(tmp, "INSERT INTO kv VALUES (?, ?);", -1, &ins, nullptr);
        for (int i = 0; i < n; ++i) {
            sqlite3_bind_text(ins, 1, keys[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 2, vals[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        sqlite3_finalize(ins);
        sqlite3_exec(tmp, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(tmp);
    }

    sqlite3* db = open_sqlite_normal_sync(path);
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?;", -1, &sel, nullptr);

    for (auto _ : state) {
        for (int idx : order) {
            sqlite3_bind_text(sel, 1, keys[idx].c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(sel) == SQLITE_ROW) {
                benchmark::DoNotOptimize(sqlite3_column_text(sel, 0));
            }
            sqlite3_reset(sel);
        }
    }

    sqlite3_finalize(sel);
    sqlite3_close(db);
    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SQLite_RandomRead)->Arg(1000)->Arg(10000)->Arg(100000);

/**
 * Overwrite: write N keys, then write them all again
 *
 * No transaction wrapper. Every write auto-commits. tinydb's put() naturally
 * handles overwrites and SQLite uses INSERT OR REPLACE.
 */

static void BM_tinydb_Overwrite(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("overwrite");
        {
            tinydb::DB db(path);
            for (int i = 0; i < n; ++i) {
                db.put(keys[i], vals[i]);
            }
            for (int i = 0; i < n; ++i) {
                db.put(keys[i], vals[i]);
            }
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_tinydb_Overwrite)->Arg(1000)->Arg(10000);

static void BM_SQLite_Overwrite(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("sqlite_ovw.db");
        {
            sqlite3* db = open_sqlite_no_sync(path);
            sqlite3_exec(
                db,
                "CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);",
                nullptr, nullptr, nullptr
            );

            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(
                db,
                "INSERT OR REPLACE INTO kv VALUES (?, ?);",
                -1, &stmt, nullptr
            );

            for (int i = 0; i < n; ++i) {
                sqlite3_bind_text(stmt, 1, keys[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, vals[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
            }
            for (int i = 0; i < n; ++i) {
                sqlite3_bind_text(stmt, 1, keys[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, vals[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
            }

            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_SQLite_Overwrite)->Arg(1000)->Arg(10000);

/**
 * Open: how long does it take to open an existing database?
 *
 * tinydb scans the whole log and rebuilds its hash index from scratch on every
 * open. SQLite opens the file and does a full SELECT k, v scan to give both
 * sides comparable startup work.
 */

static void BM_tinydb_Open(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    auto path = make_temp_path("open");
    {
        tinydb::DB setup(path);
        for (int i = 0; i < n; ++i) {
            setup.put(keys[i], vals[i]);
        }
    }

    for (auto _ : state) {
        tinydb::DB db(path);
        benchmark::DoNotOptimize(db.key_count());
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_tinydb_Open)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_SQLite_Open(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);
    auto path = make_temp_path("sqlite_open.db");

    {
        sqlite3* tmp = open_sqlite_no_sync(path);
        sqlite3_exec(
            tmp,
            "CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);",
            nullptr, nullptr, nullptr
        );
        sqlite3_exec(tmp, "BEGIN;", nullptr, nullptr, nullptr);
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(tmp, "INSERT INTO kv VALUES (?, ?);", -1, &stmt, nullptr);
        for (int i = 0; i < n; ++i) {
            sqlite3_bind_text(stmt, 1, keys[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, vals[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_exec(tmp, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(tmp);
    }

    for (auto _ : state) {
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT k, v FROM kv;", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            benchmark::DoNotOptimize(sqlite3_column_text(stmt, 0));
            benchmark::DoNotOptimize(sqlite3_column_text(stmt, 1));
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SQLite_Open)->Arg(1000)->Arg(10000)->Arg(100000);

// Compaction

static void BM_tinydb_Compact(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("compact");

        state.PauseTiming();
        {
            tinydb::DB db(path);
            for (int i = 0; i < n; ++i) {
                db.put(keys[i], vals[i]);
            }
            for (int i = 0; i < n / 2; ++i) {
                db.remove(keys[i]);
            }
        }
        {
            tinydb::DB db(path);
            state.ResumeTiming();
            db.compact();
        }

        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
}
// BENCHMARK(BM_tinydb_Compact)->Arg(1000)->Arg(10000);

/**
 * Struct Write: storing a fixed-size struct as raw bytes, one write at a time.
 */

static void BM_tinydb_StructWrite(benchmark::State& state) {
    struct Player { int32_t score; float x, y; bool active; char pad[3]; };
    const int n = state.range(0);
    auto keys = make_keys(n);

    for (auto _ : state) {
        auto path = make_temp_path("struct");
        {
            tinydb::DB db(path);
            for (int i = 0; i < n; ++i) {
                db.put(keys[i], Player{.score=i, .x=float(i), .y=float(i), .active=true, .pad={}});
            }
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_tinydb_StructWrite)->Arg(1000)->Arg(10000);

struct Player { int32_t score; float x, y; bool active; char pad[3]; };

static void BM_SQLite_StructWrite(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);

    for (auto _ : state) {
        auto path = make_temp_path("sqlite_struct.db");
        {
            sqlite3* db = open_sqlite_no_sync(path);
            sqlite3_exec(
                db,
                "CREATE TABLE kv (k TEXT PRIMARY KEY, v BLOB);",
                nullptr, nullptr, nullptr
            );

            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(
                db,
                "INSERT INTO kv VALUES (?, ?);",
                -1, &stmt, nullptr
            );

            for (int i = 0; i < n; ++i) {
                Player p{.score=i, .x=float(i), .y=float(i), .active=true, .pad={}};
                sqlite3_bind_text(stmt, 1, keys[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_blob(stmt, 2, &p, sizeof(Player), SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
            }

            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SQLite_StructWrite)->Arg(1000)->Arg(10000);

/**
 * Struct Read: reading those structs back out. Connection open before the
 * clock starts on both sides.
 */

static void BM_tinydb_StructRead(benchmark::State& state) {
    struct Player { int32_t score; float x, y; bool active; char pad[3]; };
    const int n = state.range(0);
    auto keys = make_keys(n);

    auto path = make_temp_path("sread");
    {
        tinydb::DB setup(path);
        for (int i = 0; i < n; ++i) {
            setup.put(keys[i], Player{.score=i, .x=float(i), .y=float(i), .active=true, .pad={}});
        }
    }

    tinydb::DB db(path);
    for (auto _ : state) {
        for (int i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(db.get<Player>(keys[i]));
        }
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_tinydb_StructRead)->Arg(1000)->Arg(10000);

static void BM_SQLite_StructRead(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto path = make_temp_path("sqlite_sread.db");

    {
        sqlite3* tmp = open_sqlite_no_sync(path);
        sqlite3_exec(
            tmp,
            "CREATE TABLE kv (k TEXT PRIMARY KEY, v BLOB);",
            nullptr, nullptr, nullptr
        );
        sqlite3_exec(tmp, "BEGIN;", nullptr, nullptr, nullptr);
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(tmp, "INSERT INTO kv VALUES (?, ?);", -1, &ins, nullptr);
        for (int i = 0; i < n; ++i) {
            Player p{.score=i, .x=float(i), .y=float(i), .active=true, .pad={}};
            sqlite3_bind_text(ins, 1, keys[i].c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_blob(ins, 2, &p, sizeof(Player), SQLITE_STATIC);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        sqlite3_finalize(ins);
        sqlite3_exec(tmp, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(tmp);
    }

    sqlite3* db = open_sqlite_normal_sync(path);
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?;", -1, &sel, nullptr);

    for (auto _ : state) {
        for (int i = 0; i < n; ++i) {
            sqlite3_bind_text(sel, 1, keys[i].c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(sel) == SQLITE_ROW) {
                const void* blob = sqlite3_column_blob(sel, 0);
                Player p{};
                std::memcpy(&p, blob, sizeof(Player));
                benchmark::DoNotOptimize(p);
            }
            sqlite3_reset(sel);
        }
    }

    sqlite3_finalize(sel);
    sqlite3_close(db);
    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SQLite_StructRead)->Arg(1000)->Arg(10000);

/**
 * Transaction: one committed batch per iteration, with durability.
 *
 * tinydb fsyncs at the end of every commit. SQLite uses synchronous=NORMAL
 * with WAL, which syncs at checkpoint rather than per commit. This has a
 * closely matched durability level.
 */

static void BM_tinydb_Transaction(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("tx");
        {
            tinydb::DB db(path);
            db.transaction([&](tinydb::Tx& tx) -> tinydb::TxResult {
                for (int i = 0; i < n; ++i) {
                    tx.put(keys[i], vals[i]);
                }
                return tinydb::commit;
            });
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_tinydb_Transaction)->Arg(100)->Arg(1000)->Arg(10000);

static void BM_SQLite_Transaction(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("sqlite_tx.db");
        {
            sqlite3* db = open_sqlite_normal_sync(path);
            sqlite3_exec(
                db,
                "CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT);",
                nullptr, nullptr, nullptr
            );

            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(
                db,
                "INSERT INTO kv VALUES (?, ?);",
                -1, &stmt, nullptr
            );

            sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
            for (int i = 0; i < n; ++i) {
                sqlite3_bind_text(stmt, 1, keys[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, vals[i].c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
            }
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);

            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SQLite_Transaction)->Arg(100)->Arg(1000)->Arg(10000);
