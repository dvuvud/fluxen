#include "fluxen.hpp"

#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include "test_helpers.hpp"

#include <gtest/gtest.h>

class FluxenTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = make_temp_path("test");
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

// strings

TEST_F(FluxenTest, PutAndGetString) {
    db_->put("hello", "world");
    ASSERT_EQ(db_->get("hello"), "world");
}

TEST_F(FluxenTest, GetMissingKeyReturnsNullopt) {
    EXPECT_EQ(db_->get("missing"), std::nullopt);
}

TEST_F(FluxenTest, OverwriteUpdatesValue) {
    db_->put("key", "first");
    db_->put("key", "second");
    EXPECT_EQ(db_->get("key"), "second");
}

TEST_F(FluxenTest, OverwriteDoesNotIncreaseKeyCount) {
    db_->put("key", "first");
    db_->put("key", "second");
    EXPECT_EQ(db_->key_count(), 1u);
}

TEST_F(FluxenTest, EmptyStringValueIsValid) {
    db_->put("empty", "");
    auto v = db_->get("empty");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->empty());
}

TEST_F(FluxenTest, BinaryDataRoundTrips) {
    std::string blob = { '\x00', '\x01', '\xFF', '\xFE' };
    db_->put("blob", blob);
    EXPECT_EQ(db_->get("blob"), blob);
}

// numeric types

TEST_F(FluxenTest, PutAndGetInt32) {
    db_->put("n", int32_t{-42});
    EXPECT_EQ(db_->get<int32_t>("n"), int32_t{-42});
}

TEST_F(FluxenTest, PutAndGetUInt64) {
    db_->put("n", uint64_t{0xDEADBEEFCAFEBABEULL});
    EXPECT_EQ(db_->get<uint64_t>("n"), uint64_t{0xDEADBEEFCAFEBABEULL});
}

TEST_F(FluxenTest, PutAndGetFloat) {
    db_->put("f", 3.14f);
    EXPECT_EQ(db_->get<float>("f"), 3.14f);
}

TEST_F(FluxenTest, PutAndGetDouble) {
    db_->put("d", 2.718281828);
    EXPECT_EQ(db_->get<double>("d"), 2.718281828);
}

TEST_F(FluxenTest, PutAndGetBool) {
    db_->put("b", true);
    EXPECT_EQ(db_->get<bool>("b"), true);
}

// structs

TEST_F(FluxenTest, PutAndGetTrivialStruct) {
    struct Vec3 { float x, y, z; };
    db_->put("pos", Vec3{1.0f, 2.0f, 3.0f});
    auto v = db_->get<Vec3>("pos");
    ASSERT_TRUE(v.has_value());
    EXPECT_FLOAT_EQ(v->x, 1.0f);
    EXPECT_FLOAT_EQ(v->y, 2.0f);
    EXPECT_FLOAT_EQ(v->z, 3.0f);
}

TEST_F(FluxenTest, GetWrongTypeSizeReturnsNullopt) {
    db_->put("small", uint8_t{7});
    EXPECT_EQ(db_->get<uint32_t>("small"), std::nullopt);
}

// remove and has

TEST_F(FluxenTest, HasReturnsTrueForExistingKey) {
    db_->put("x", "1");
    EXPECT_TRUE(db_->has("x"));
}

TEST_F(FluxenTest, HasReturnsFalseForMissingKey) {
    EXPECT_FALSE(db_->has("nope"));
}

TEST_F(FluxenTest, RemoveDeletesKey) {
    db_->put("x", "1");
    db_->remove("x");
    EXPECT_FALSE(db_->has("x"));
    EXPECT_EQ(db_->get("x"), std::nullopt);
}

TEST_F(FluxenTest, RemoveNonExistentKeyIsNoop) {
    EXPECT_NO_THROW(db_->remove("ghost"));
    EXPECT_EQ(db_->key_count(), 0u);
}

TEST_F(FluxenTest, ReinsertAfterRemoveWorks) {
    db_->put("x", "original");
    db_->remove("x");
    db_->put("x", "back");
    EXPECT_EQ(db_->get("x"), "back");
}

// key_count

TEST_F(FluxenTest, KeyCountStartsAtZero) {
    EXPECT_EQ(db_->key_count(), 0u);
}

TEST_F(FluxenTest, KeyCountTracksInsertions) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->put("c", "3");
    EXPECT_EQ(db_->key_count(), 3u);
}

TEST_F(FluxenTest, KeyCountTracksRemovals) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->remove("a");
    EXPECT_EQ(db_->key_count(), 1u);
}

// each

TEST_F(FluxenTest, EachVisitsAllLiveKeys) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->put("c", "3");
    db_->remove("b");

    std::vector<std::string> keys;
    db_->each([&](std::string_view k, fluxen::Bytes) {
        keys.push_back(std::string(k));
    });

    EXPECT_EQ(keys.size(), 2u);
    EXPECT_NE(std::find(keys.begin(), keys.end(), "a"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "c"), keys.end());
    EXPECT_EQ(std::find(keys.begin(), keys.end(), "b"), keys.end());
}

TEST_F(FluxenTest, EachOnEmptyDatabaseCallsCallbackZeroTimes) {
    int calls = 0;
    db_->each([&](std::string_view, fluxen::Bytes) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST_F(FluxenTest, EachValueBytesMatchStoredString) {
    db_->put("msg", "hello");
    db_->each([](std::string_view key, fluxen::Bytes val) {
        if (key == "msg") {
            std::string v(reinterpret_cast<const char*>(val.data()), val.size());
            EXPECT_EQ(v, "hello");
        }
    });
}

// prefix

TEST_F(FluxenTest, PrefixFiltersCorrectly) {
    db_->put("user:james",   "admin");
    db_->put("user:rick",    "viewer");
    db_->put("config:theme", "dark");

    int users = 0;
    db_->prefix("user:", [&](std::string_view, fluxen::Bytes) { ++users; });
    EXPECT_EQ(users, 2);
}

TEST_F(FluxenTest, PrefixWithNoMatchesCallsCallbackZeroTimes) {
    db_->put("user:james", "admin");
    int calls = 0;
    db_->prefix("session:", [&](std::string_view, fluxen::Bytes) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST_F(FluxenTest, PrefixEmptyStringMatchesAll) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->put("c", "3");
    int calls = 0;
    db_->prefix("", [&](std::string_view, fluxen::Bytes) { ++calls; });
    EXPECT_EQ(calls, 3);
}

// transactions

TEST_F(FluxenTest, TransactionCommitAppliesAllOps) {
    db_->transaction([](fluxen::Tx& tx) {
        tx.put("balance",  int32_t{1000});
        tx.put("currency", std::string("USD"));
        return fluxen::commit;
    });
    EXPECT_EQ(db_->get<int32_t>("balance"), int32_t{1000});
    EXPECT_EQ(db_->get("currency"), "USD");
}

TEST_F(FluxenTest, TransactionRollbackAppliesNothing) {
    db_->put("x", "original");
    db_->transaction([](fluxen::Tx& tx) {
        tx.put("x", std::string("changed"));
        tx.put("y", std::string("new"));
        return fluxen::rollback;
    });
    EXPECT_EQ(db_->get("x"), "original");
    EXPECT_FALSE(db_->has("y"));
}

TEST_F(FluxenTest, TransactionCanDelete) {
    db_->put("temp", "bye");
    db_->transaction([](fluxen::Tx& tx) {
        tx.remove("temp");
        return fluxen::commit;
    });
    EXPECT_FALSE(db_->has("temp"));
}

TEST_F(FluxenTest, TransactionThrowIsEquivalentToRollback) {
    db_->put("x", "safe");
    try {
        db_->transaction([](fluxen::Tx& tx) -> fluxen::TxResult {
            tx.put("x", std::string("danger"));
            throw std::runtime_error("abort");
        });
    } catch (const std::runtime_error&) {}
    EXPECT_EQ(db_->get("x"), "safe");
}

TEST_F(FluxenTest, TransactionMixedPutAndDelete) {
    db_->put("keep", "yes");
    db_->put("drop", "no");
    db_->transaction([](fluxen::Tx& tx) {
        tx.remove("drop");
        tx.put("new", std::string("hello"));
        return fluxen::commit;
    });
    EXPECT_TRUE(db_->has("keep"));
    EXPECT_FALSE(db_->has("drop"));
    EXPECT_EQ(db_->get("new"), "hello");
}

// persistence

TEST_F(FluxenTest, DataPersistsAcrossReopen) {
    db_->put("name",    "fluxen");
    db_->put("version", int32_t{1});
    db_->remove("name");

    reopen();

    EXPECT_FALSE(db_->has("name"));
    EXPECT_EQ(db_->get<int32_t>("version"), int32_t{1});
}

TEST_F(FluxenTest, StructPersistsAcrossReopen) {
    struct Config { int port; float timeout; bool debug; char pad[3]; };
    db_->put("cfg", Config{8080, 30.0f, true, {}});

    reopen();

    auto cfg = db_->get<Config>("cfg");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->port, 8080);
    EXPECT_FLOAT_EQ(cfg->timeout, 30.0f);
    EXPECT_TRUE(cfg->debug);
}

TEST_F(FluxenTest, KeyCountCorrectAfterReopen) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->put("c", "3");
    db_->remove("b");

    reopen();

    EXPECT_EQ(db_->key_count(), 2u);
}

TEST_F(FluxenTest, TombstonesHonouredAfterReopen) {
    db_->put("gone", "bye");
    db_->remove("gone");

    reopen();

    EXPECT_FALSE(db_->has("gone"));
}

TEST_F(FluxenTest, OverwritePersistsAcrossReopen) {
    db_->put("key", "first");
    reopen();
    db_->put("key", "second");
    reopen();
    EXPECT_EQ(db_->get("key"), "second");
}

// compaction

TEST_F(FluxenTest, CompactReducesFileSize) {
    for (int i = 0; i < 100; ++i) {
        db_->put("k" + std::to_string(i), int32_t{i});
    }
    for (int i = 0; i < 50; ++i) {
        db_->remove("k" + std::to_string(i));
    }

    size_t before = db_->file_size();
    db_->compact();
    EXPECT_LT(db_->file_size(), before);
}

TEST_F(FluxenTest, CompactPreservesAllLiveKeys) {
    for (int i = 0; i < 100; ++i) {
        db_->put("k" + std::to_string(i), int32_t{i});
    }
    for (int i = 0; i < 50; ++i) {
        db_->remove("k" + std::to_string(i));
    }

    db_->compact();

    EXPECT_EQ(db_->key_count(), 50u);
    for (int i = 50; i < 100; ++i) {
        EXPECT_EQ(db_->get<int32_t>("k" + std::to_string(i)), int32_t{i});
    }
}

TEST_F(FluxenTest, CompactRemovesDeletedKeys) {
    for (int i = 0; i < 50; ++i) {
        db_->remove("k" + std::to_string(i));
    }

    db_->compact();

    for (int i = 0; i < 50; ++i) {
        EXPECT_FALSE(db_->has("k" + std::to_string(i)));
    }
}

TEST_F(FluxenTest, CompactedDataPersistsAcrossReopen) {
    db_->put("a", "keep");
    db_->put("b", "drop");
    db_->put("c", "keep");
    db_->remove("b");
    db_->compact();

    reopen();

    EXPECT_EQ(db_->get("a"), "keep");
    EXPECT_FALSE(db_->has("b"));
    EXPECT_EQ(db_->get("c"), "keep");
    EXPECT_EQ(db_->key_count(), 2u);
}

TEST_F(FluxenTest, CompactWithOverwrittenKeys) {
    for (int i = 0; i < 50; ++i) {
        db_->put("k" + std::to_string(i), int32_t{i});
    }
    for (int i = 0; i < 50; ++i) {
        db_->put("k" + std::to_string(i), int32_t{i * 10});
    }

    size_t before = db_->file_size();
    db_->compact();

    EXPECT_LT(db_->file_size(), before);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(db_->get<int32_t>("k" + std::to_string(i)), int32_t{i * 10});
    }
}

// file format

TEST_F(FluxenTest, BadMagicThrowsOnOpen) {
    auto bad_path = std::filesystem::temp_directory_path() / "fluxen_bad_magic.db";
    {
        std::FILE* f = std::fopen(bad_path.string().c_str(), "wb");
        std::fwrite("NOTVALID", 1, 8, f);
        std::fclose(f);
    }
    EXPECT_THROW(fluxen::DB{bad_path.string()}, std::runtime_error);
    std::filesystem::remove(bad_path);
}

TEST_F(FluxenTest, EmptyFileInitialisesCleanly) {
    EXPECT_EQ(db_->key_count(), 0u);
    EXPECT_GT(db_->file_size(), 0u);
}

// stress testing

TEST_F(FluxenTest, ManyKeysAllReadBack) {
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        db_->put("k" + std::to_string(i), int32_t{i});
    }

    ASSERT_EQ(db_->key_count(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(db_->get<int32_t>("k" + std::to_string(i)), int32_t{i});
    }
}

TEST_F(FluxenTest, LargeValueRoundTrips) {
    std::string big(1024 * 1024, 'x');
    db_->put("big", big);
    EXPECT_EQ(db_->get("big"), big);
}

TEST_F(FluxenTest, LargeValuePersistsAcrossReopen) {
    std::string big(1024 * 1024, 'z');
    db_->put("big", big);

    reopen();

    EXPECT_EQ(db_->get("big"), big);
}
