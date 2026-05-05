#include "tinydb.hpp"
#include "tests/test_helpers.hpp"

#include <benchmark/benchmark.h>

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

// seq writes

static void BM_SequentialWrite(benchmark::State& state) {
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
BENCHMARK(BM_SequentialWrite)->Arg(1000)->Arg(10000)->Arg(100000);

// seq reads

static void BM_SequentialRead(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    auto path = make_temp_path("read");
    {
        tinydb::DB db(path);
        for (int i = 0; i < n; ++i) {
            db.put(keys[i], vals[i]);
        }
    }

    for (auto _ : state) {
        tinydb::DB db(path);
        for (int i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(db.get(keys[i]));
        }
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SequentialRead)->Arg(1000)->Arg(10000)->Arg(100000);

// rand read

static void BM_RandomRead(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(42);
    std::shuffle(order.begin(), order.end(), rng);

    auto path = make_temp_path("rread");
    {
        tinydb::DB db(path);
        for (int i = 0; i < n; ++i) {
            db.put(keys[i], vals[i]);
        }
    }

    for (auto _ : state) {
        tinydb::DB db(path);
        for (int idx : order) {
            benchmark::DoNotOptimize(db.get(keys[idx]));
        }
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_RandomRead)->Arg(1000)->Arg(10000)->Arg(100000);

// overwrites

static void BM_Overwrite(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("overwrite");
        {
            tinydb::DB db(path);
            for (int i = 0; i < n; ++i) db.put(keys[i], vals[i]);
            for (int i = 1; i < n; ++i) db.put(keys[i], vals[i]);
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * n * 2);
}
BENCHMARK(BM_Overwrite)->Arg(1000)->Arg(10000);

// open / rebuild

static void BM_Open(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    auto path = make_temp_path("open");
    {
        tinydb::DB db(path);
        for (int i = 0; i < n; ++i) {
            db.put(keys[i], vals[i]);
        }
    }

    for (auto _ : state) {
        tinydb::DB db(path);
        benchmark::DoNotOptimize(db.key_count());
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Open)->Arg(1000)->Arg(10000)->Arg(100000);

// compaction

static void BM_Compact(benchmark::State& state) {
    const int n = state.range(0);
    auto keys = make_keys(n);
    auto vals = make_values(n);

    for (auto _ : state) {
        auto path = make_temp_path("compact");

        state.PauseTiming();
        {
            tinydb::DB db(path);
            for (int i = 0; i < n; ++i) db.put(keys[i], vals[i]);
            for (int i = 0; i < n / 2; ++i) db.remove(keys[i]);
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
BENCHMARK(BM_Compact)->Arg(1000)->Arg(10000);

// struct write/read

static void BM_StructWrite(benchmark::State& state) {
    struct Player { int32_t score; float x, y; bool active; char pad[3]; };
    const int n = state.range(0);
    auto keys = make_keys(n);

    for (auto _ : state) {
        auto path = make_temp_path("struct");
        {
            tinydb::DB db(path);
            for (int i = 0; i < n; ++i) {
                db.put(keys[i], Player{ .score=i, .x=float(i), .y=float(i), .active=true, .pad={} });
            }
        }
        state.PauseTiming();
        std::filesystem::remove(path);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_StructWrite)->Arg(1000)->Arg(10000);

static void BM_StructRead(benchmark::State& state) {
    struct Player { int32_t score; float x, y; bool active; char pad[3]; };
    const int n = state.range(0);
    auto keys = make_keys(n);

    auto path = make_temp_path("sread");
    {
        tinydb::DB db(path);
        for (int i = 0; i < n; ++i) {
            db.put(keys[i], Player{ .score=i, .x=float(i), .y=float(i), .active=true, .pad={} });
        }
    }

    for (auto _ : state) {
        tinydb::DB db(path);
        for (int i = 0; i < n; ++i) {
            benchmark::DoNotOptimize(db.get<Player>(keys[i]));
        }
    }

    std::filesystem::remove(path);
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_StructRead)->Arg(1000)->Arg(10000);

// transaction

static void BM_Transaction(benchmark::State& state) {
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
BENCHMARK(BM_Transaction)->Arg(100)->Arg(1000)->Arg(10000);
