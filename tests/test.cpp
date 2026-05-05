#include "tinydb.hpp"

#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include "test_helpers.hpp"

#include <gtest/gtest.h>

class TinyDBTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = make_temp_path("test");
        db_ = std::make_unique<tinydb::DB>(path_.string());
    }

    void TearDown() override {
        db_.reset();
        std::filesystem::remove(path_);
    }

    void reopen() {
        db_.reset();
        db_ = std::make_unique<tinydb::DB>(path_.string());
    }

    std::filesystem::path path_;
    std::unique_ptr<tinydb::DB> db_;
};

// strings

TEST_F(TinyDBTest, PutAndGetString) {
    db_->put("hello", "world");
    ASSERT_EQ(db_->get("hello"), "world");
}

TEST_F(TinyDBTest, GetMissingKeyReturnsNullopt) {
    EXPECT_EQ(db_->get("missing"), std::nullopt);
}

TEST_F(TinyDBTest, OverwriteUpdatesValue) {
    db_->put("key", "first");
    db_->put("key", "second");
    EXPECT_EQ(db_->get("key"), "second");
}

TEST_F(TinyDBTest, OverwriteDoesNotIncreaseKeyCount) {
    db_->put("key", "first");
    db_->put("key", "second");
    EXPECT_EQ(db_->key_count(), 1u);
}

TEST_F(TinyDBTest, EmptyStringValueIsValid) {
    db_->put("empty", "");
    auto v = db_->get("empty");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->empty());
}

TEST_F(TinyDBTest, BinaryDataRoundTrips) {
    std::string blob = { '\x00', '\x01', '\xFF', '\xFE' };
    db_->put("blob", blob);
    EXPECT_EQ(db_->get("blob"), blob);
}

// numeric types

TEST_F(TinyDBTest, PutAndGetInt32) {
    db_->put("n", int32_t{-42});
    EXPECT_EQ(db_->get<int32_t>("n"), int32_t{-42});
}

TEST_F(TinyDBTest, PutAndGetUInt64) {
    db_->put("n", uint64_t{0xDEADBEEFCAFEBABEULL});
    EXPECT_EQ(db_->get<uint64_t>("n"), uint64_t{0xDEADBEEFCAFEBABEULL});
}

TEST_F(TinyDBTest, PutAndGetFloat) {
    db_->put("f", 3.14f);
    EXPECT_EQ(db_->get<float>("f"), 3.14f);
}

TEST_F(TinyDBTest, PutAndGetDouble) {
    db_->put("d", 2.718281828);
    EXPECT_EQ(db_->get<double>("d"), 2.718281828);
}

TEST_F(TinyDBTest, PutAndGetBool) {
    db_->put("b", true);
    EXPECT_EQ(db_->get<bool>("b"), true);
}

// structs

TEST_F(TinyDBTest, PutAndGetTrivialStruct) {
    struct Vec3 { float x, y, z; };
    db_->put("pos", Vec3{1.0f, 2.0f, 3.0f});
    auto v = db_->get<Vec3>("pos");
    ASSERT_TRUE(v.has_value());
    EXPECT_FLOAT_EQ(v->x, 1.0f);
    EXPECT_FLOAT_EQ(v->y, 2.0f);
    EXPECT_FLOAT_EQ(v->z, 3.0f);
}

TEST_F(TinyDBTest, GetWrongTypeSizeReturnsNullopt) {
    db_->put("small", uint8_t{7});
    EXPECT_EQ(db_->get<uint32_t>("small"), std::nullopt);
}

// remove and has

TEST_F(TinyDBTest, HasReturnsTrueForExistingKey) {
    db_->put("x", "1");
    EXPECT_TRUE(db_->has("x"));
}

TEST_F(TinyDBTest, HasReturnsFalseForMissingKey) {
    EXPECT_FALSE(db_->has("nope"));
}

TEST_F(TinyDBTest, RemoveDeletesKey) {
    db_->put("x", "1");
    db_->remove("x");
    EXPECT_FALSE(db_->has("x"));
    EXPECT_EQ(db_->get("x"), std::nullopt);
}

TEST_F(TinyDBTest, RemoveNonExistentKeyIsNoop) {
    EXPECT_NO_THROW(db_->remove("ghost"));
    EXPECT_EQ(db_->key_count(), 0u);
}

TEST_F(TinyDBTest, ReinsertAfterRemoveWorks) {
    db_->put("x", "original");
    db_->remove("x");
    db_->put("x", "back");
    EXPECT_EQ(db_->get("x"), "back");
}

// key_count

TEST_F(TinyDBTest, KeyCountStartsAtZero) {
    EXPECT_EQ(db_->key_count(), 0u);
}

TEST_F(TinyDBTest, KeyCountTracksInsertions) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->put("c", "3");
    EXPECT_EQ(db_->key_count(), 3u);
}

TEST_F(TinyDBTest, KeyCountTracksRemovals) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->remove("a");
    EXPECT_EQ(db_->key_count(), 1u);
}

// each

TEST_F(TinyDBTest, EachVisitsAllLiveKeys) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->put("c", "3");
    db_->remove("b");

    std::vector<std::string> keys;
    db_->each([&](std::string_view k, tinydb::Bytes) {
        keys.push_back(std::string(k));
    });

    EXPECT_EQ(keys.size(), 2u);
    EXPECT_NE(std::find(keys.begin(), keys.end(), "a"), keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "c"), keys.end());
    EXPECT_EQ(std::find(keys.begin(), keys.end(), "b"), keys.end());
}

TEST_F(TinyDBTest, EachOnEmptyDatabaseCallsCallbackZeroTimes) {
    int calls = 0;
    db_->each([&](std::string_view, tinydb::Bytes) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST_F(TinyDBTest, EachValueBytesMatchStoredString) {
    db_->put("msg", "hello");
    db_->each([](std::string_view key, tinydb::Bytes val) {
        if (key == "msg") {
            std::string v(reinterpret_cast<const char*>(val.data()), val.size());
            EXPECT_EQ(v, "hello");
        }
    });
}

// prefix

TEST_F(TinyDBTest, PrefixFiltersCorrectly) {
    db_->put("user:james",   "admin");
    db_->put("user:rick",    "viewer");
    db_->put("config:theme", "dark");

    int users = 0;
    db_->prefix("user:", [&](std::string_view, tinydb::Bytes) { ++users; });
    EXPECT_EQ(users, 2);
}

TEST_F(TinyDBTest, PrefixWithNoMatchesCallsCallbackZeroTimes) {
    db_->put("user:james", "admin");
    int calls = 0;
    db_->prefix("session:", [&](std::string_view, tinydb::Bytes) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST_F(TinyDBTest, PrefixEmptyStringMatchesAll) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->put("c", "3");
    int calls = 0;
    db_->prefix("", [&](std::string_view, tinydb::Bytes) { ++calls; });
    EXPECT_EQ(calls, 3);
}

// transactions

TEST_F(TinyDBTest, TransactionCommitAppliesAllOps) {
    db_->transaction([](tinydb::Tx& tx) {
        tx.put("balance",  int32_t{1000});
        tx.put("currency", std::string("USD"));
        return tinydb::commit;
    });
    EXPECT_EQ(db_->get<int32_t>("balance"), int32_t{1000});
    EXPECT_EQ(db_->get("currency"), "USD");
}

TEST_F(TinyDBTest, TransactionRollbackAppliesNothing) {
    db_->put("x", "original");
    db_->transaction([](tinydb::Tx& tx) {
        tx.put("x", std::string("changed"));
        tx.put("y", std::string("new"));
        return tinydb::rollback;
    });
    EXPECT_EQ(db_->get("x"), "original");
    EXPECT_FALSE(db_->has("y"));
}

TEST_F(TinyDBTest, TransactionCanDelete) {
    db_->put("temp", "bye");
    db_->transaction([](tinydb::Tx& tx) {
        tx.remove("temp");
        return tinydb::commit;
    });
    EXPECT_FALSE(db_->has("temp"));
}

TEST_F(TinyDBTest, TransactionThrowIsEquivalentToRollback) {
    db_->put("x", "safe");
    try {
        db_->transaction([](tinydb::Tx& tx) -> tinydb::TxResult {
            tx.put("x", std::string("danger"));
            throw std::runtime_error("abort");
        });
    } catch (const std::runtime_error&) {}
    EXPECT_EQ(db_->get("x"), "safe");
}

TEST_F(TinyDBTest, TransactionMixedPutAndDelete) {
    db_->put("keep", "yes");
    db_->put("drop", "no");
    db_->transaction([](tinydb::Tx& tx) {
        tx.remove("drop");
        tx.put("new", std::string("hello"));
        return tinydb::commit;
    });
    EXPECT_TRUE(db_->has("keep"));
    EXPECT_FALSE(db_->has("drop"));
    EXPECT_EQ(db_->get("new"), "hello");
}

// persistence

TEST_F(TinyDBTest, DataPersistsAcrossReopen) {
    db_->put("name",    "tinydb");
    db_->put("version", int32_t{1});
    db_->remove("name");

    reopen();

    EXPECT_FALSE(db_->has("name"));
    EXPECT_EQ(db_->get<int32_t>("version"), int32_t{1});
}

TEST_F(TinyDBTest, StructPersistsAcrossReopen) {
    struct Config { int port; float timeout; bool debug; char pad[3]; };
    db_->put("cfg", Config{8080, 30.0f, true, {}});

    reopen();

    auto cfg = db_->get<Config>("cfg");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->port, 8080);
    EXPECT_FLOAT_EQ(cfg->timeout, 30.0f);
    EXPECT_TRUE(cfg->debug);
}

TEST_F(TinyDBTest, KeyCountCorrectAfterReopen) {
    db_->put("a", "1");
    db_->put("b", "2");
    db_->put("c", "3");
    db_->remove("b");

    reopen();

    EXPECT_EQ(db_->key_count(), 2u);
}

TEST_F(TinyDBTest, TombstonesHonouredAfterReopen) {
    db_->put("gone", "bye");
    db_->remove("gone");

    reopen();

    EXPECT_FALSE(db_->has("gone"));
}

TEST_F(TinyDBTest, OverwritePersistsAcrossReopen) {
    db_->put("key", "first");
    reopen();
    db_->put("key", "second");
    reopen();
    EXPECT_EQ(db_->get("key"), "second");
}

// compaction

TEST_F(TinyDBTest, CompactReducesFileSize) {
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

TEST_F(TinyDBTest, CompactPreservesAllLiveKeys) {
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

TEST_F(TinyDBTest, CompactRemovesDeletedKeys) {
    for (int i = 0; i < 50; ++i) {
        db_->remove("k" + std::to_string(i));
    }

    db_->compact();

    for (int i = 0; i < 50; ++i) {
        EXPECT_FALSE(db_->has("k" + std::to_string(i)));
    }
}

TEST_F(TinyDBTest, CompactedDataPersistsAcrossReopen) {
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

TEST_F(TinyDBTest, CompactWithOverwrittenKeys) {
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

TEST_F(TinyDBTest, BadMagicThrowsOnOpen) {
    auto bad_path = std::filesystem::temp_directory_path() / "tinydb_bad_magic.db";
    {
        std::FILE* f = std::fopen(bad_path.string().c_str(), "wb");
        std::fwrite("NOTVALID", 1, 8, f);
        std::fclose(f);
    }
    EXPECT_THROW(tinydb::DB{bad_path.string()}, std::runtime_error);
    std::filesystem::remove(bad_path);
}

TEST_F(TinyDBTest, EmptyFileInitialisesCleanly) {
    EXPECT_EQ(db_->key_count(), 0u);
    EXPECT_GT(db_->file_size(), 0u);
}

// stress testing

TEST_F(TinyDBTest, ManyKeysAllReadBack) {
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        db_->put("k" + std::to_string(i), int32_t{i});
    }

    ASSERT_EQ(db_->key_count(), static_cast<size_t>(N));

    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(db_->get<int32_t>("k" + std::to_string(i)), int32_t{i});
    }
}

TEST_F(TinyDBTest, LargeValueRoundTrips) {
    std::string big(1024 * 1024, 'x');
    db_->put("big", big);
    EXPECT_EQ(db_->get("big"), big);
}

TEST_F(TinyDBTest, LargeValuePersistsAcrossReopen) {
    std::string big(1024 * 1024, 'z');
    db_->put("big", big);

    reopen();

    EXPECT_EQ(db_->get("big"), big);
}
