#include "fluxen.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <filesystem>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using std::string_view;
using std::thread;

using namespace std::chrono_literals;

class FluxenConcurrentTest : public ::testing::Test {
protected:
  void SetUp() override {
    path_ = make_temp_path("concurrent");
    db_ = std::make_unique<fluxen::DB>(path_.string());
  }

  void TearDown() override {
    db_.reset();
    std::filesystem::remove(path_);
  }

  void reopen() {
    db_.reset();
    db_ = std::make_unique<fluxen::DB>(path_.string());
  }

  std::filesystem::path path_;
  std::unique_ptr<fluxen::DB> db_;
};

// concurrent reads

TEST_F(FluxenConcurrentTest, ConcurrentReadsReturnCorrectValue) {
  db_->put("key", int32_t{42});

  constexpr int kThreads = 16;
  std::latch start{kThreads};
  std::atomic<int> mismatches{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() -> void {
      start.arrive_and_wait();
      for (int j = 0; j < 200; ++j) {
        auto v = db_->get<int32_t>("key");
        if (!v || *v != 42) {
          ++mismatches;
        }
      }
    });
  }

  for (auto &t : threads)
    t.join();
  EXPECT_EQ(mismatches.load(), 0);
}

TEST_F(FluxenConcurrentTest, ConcurrentMixedReadOpsDoNotDeadlock) {
  db_->put("user:alice", "admin");
  db_->put("user:bob", "viewer");
  db_->put("cfg", "x");

  constexpr int kThreads = 12;
  std::latch start{kThreads};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() -> void {
      start.arrive_and_wait();
      for (int j = 0; j < 100; ++j) {
        switch ((i + j) % 4) {
        case 0:
          (void)db_->get("user:alice");
          break;
        case 1:
          (void)db_->has("cfg");
          break;
        case 2:
          db_->each([](auto, auto) -> auto {});
          break;
        case 3:
          db_->prefix("user:", [](auto, auto) -> auto {});
          break;
        }
      }
    });
  }

  for (auto &t : threads)
    t.join();
  SUCCEED();
}

TEST_F(FluxenConcurrentTest, LazyRemapUnderConcurrentReadersIsCorrect) {
  db_->put("x", int32_t{1});

  db_->put("x", int32_t{2});

  constexpr int kThreads = 32;
  std::latch start{kThreads};
  std::atomic<int> bad_reads{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() -> void {
      start.arrive_and_wait();
      auto v = db_->get<int32_t>("x");
      if (!v || *v != 2)
        ++bad_reads;
    });
  }

  for (auto &t : threads)
    t.join();
  EXPECT_EQ(bad_reads.load(), 0);
}

// concurrent writes

TEST_F(FluxenConcurrentTest, ConcurrentWritersDoNotCorruptData) {
  db_->put("n", int32_t{0});

  constexpr int kThreads = 8;
  constexpr int kWrites = 50;
  std::latch start{kThreads};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() -> void {
      start.arrive_and_wait();
      for (int j = 0; j < kWrites; ++j) {
        db_->put("n", int32_t{j});
      }
    });
  }

  for (auto &t : threads)
    t.join();

  auto v = db_->get<int32_t>("n");
  ASSERT_TRUE(v.has_value())
      << "key 'n' should still exist after concurrent writes";
  EXPECT_GE(*v, int32_t{0});
  EXPECT_LT(*v, int32_t{kWrites});
}

// concurrent reads and writes

TEST_F(FluxenConcurrentTest, ReadersObserveWriteAfterWriterFinishes) {
  db_->put("counter", int32_t{0});

  std::latch writer_done{1};

  thread writer([&]() -> void {
    db_->put("counter", int32_t{99});
    writer_done.count_down();
  });
  writer.join();

  writer_done.wait();

  constexpr int kThreads = 8;
  std::atomic<int> wrong_reads{0};
  std::vector<std::thread> readers;
  readers.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    readers.emplace_back([&]() -> void {
      auto v = db_->get<int32_t>("counter");
      if (!v || *v != 99)
        ++wrong_reads;
    });
  }

  for (auto &t : readers)
    t.join();
  EXPECT_EQ(wrong_reads.load(), 0);
}

TEST_F(FluxenConcurrentTest, WriterWaitsForActiveReader) {
  db_->put("val", std::string("before"));

  std::latch reader_inside{1};
  std::latch reader_done{1};
  std::atomic<bool> write_done{false};

  std::thread reader([&]() -> void {
    db_->each([&](string_view, fluxen::Bytes) -> void {
      reader_inside.count_down();
      std::this_thread::sleep_for(20ms);
    });
    reader_done.count_down();
  });

  std::thread writer([&]() -> void {
    reader_inside.wait();
    db_->put("val", std::string("after"));
    write_done.store(true);
  });

  reader.join();
  writer.join();

  EXPECT_TRUE(write_done.load());
  EXPECT_EQ(db_->get("val"), "after");
}

// transactions

TEST_F(FluxenConcurrentTest, TransactionAtomicUnderConcurrentReads) {
  db_->put("tx:a", std::string("old_a"));
  db_->put("tx:b", std::string("old_b"));

  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};

  std::thread reader([&]() -> void {
    while (!stop.load(std::memory_order_relaxed)) {
      bool has_a = db_->has("tx:a");
      bool has_b = db_->has("tx:b");
      if (!has_a || !has_b)
        ++torn;
    }
  });

  for (int i = 0; i < 200; ++i) {
    db_->transaction([&i](fluxen::Tx &tx) -> fluxen::TxResult {
      tx.put("tx:a", std::string("a") + std::to_string(i));
      tx.put("tx:b", std::string("b") + std::to_string(i));
      return fluxen::commit;
    });
  }

  stop.store(true);
  reader.join();

  EXPECT_EQ(torn.load(), 0) << "reader observed a torn transaction";
}

// compaction

TEST_F(FluxenConcurrentTest, CompactUnderConcurrentReadsCompletesCleanly) {
  for (int i = 0; i < 100; ++i) {
    db_->put("k" + std::to_string(i), int32_t{i});
  }
  for (int i = 0; i < 50; ++i) {
    db_->remove("k" + std::to_string(i));
  }

  std::atomic<bool> stop{false};
  std::atomic<int> read_errors{0};

  constexpr int kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int i = 0; i < kReaders; ++i) {
    readers.emplace_back([&]() -> void {
      while (!stop.load(std::memory_order_relaxed)) {
        auto v = db_->get<int32_t>("k75");
        if (v && *v != int32_t{75})
          ++read_errors;
      }
    });
  }

  std::this_thread::sleep_for(2ms);
  db_->compact();
  stop.store(true);

  for (auto &t : readers)
    t.join();

  EXPECT_EQ(read_errors.load(), 0);
  EXPECT_EQ(db_->key_count(), 50u);
  EXPECT_EQ(db_->get<int32_t>("k75"), int32_t{75});
}

TEST_F(FluxenConcurrentTest, ReadAfterCompactSeesCorrectData) {
  db_->put("live", std::string("yes"));
  db_->put("dead", std::string("no"));
  db_->remove("dead");
  db_->compact();

  constexpr int kThreads = 8;
  std::latch start{kThreads};
  std::atomic<int> errors{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() -> void {
      start.arrive_and_wait();
      auto live = db_->get("live");
      auto dead = db_->get("dead");
      if (!live || *live != "yes")
        ++errors;
      if (dead.has_value())
        ++errors;
    });
  }

  for (auto &t : threads)
    t.join();
  EXPECT_EQ(errors.load(), 0);
}
