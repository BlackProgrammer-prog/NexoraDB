#include "core/security/NativeIam.h"
#include "core/DocEngine.h"
#include "TestTempDir.h"
#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>
#include <set>
#include <functional>

namespace {
using namespace nexora::security;
using Json = nlohmann::json;
constexpr auto password = "correct horse battery staple";

class NativeIam : public testing::Test {
protected:
    TestTempDir directory{"native_iam"};
    std::string path = (directory.path()/"db").string();

    // Simulates an operator modifying physical metadata offline. Never exposed
    // as a production API or pybind11 fixture.
    void mutate(const std::function<void(rocksdb::DB&)>& fn) {
        rocksdb::DB* raw = nullptr;
        rocksdb::Options options;
        ASSERT_TRUE(rocksdb::DB::Open(options,path,&raw).ok());
        std::unique_ptr<rocksdb::DB> db(raw);
        fn(*db);
    }
    void enroll(const std::string& username = "chosen.owner") {
        OfflineIdentityStore store(path);
        store.initialize(username,password);
    }
};

TEST_F(NativeIam, OwnerChosenNamePersistentIdentityAndRecoverySecrecy) {
    BootstrapReceipt receipt;
    {
        OfflineIdentityStore store(path);
        EXPECT_FALSE(store.status().initialized);
        receipt = store.initialize("not_a_reserved_root",password);
        EXPECT_EQ(receipt.recovery_codes.size(),8);
        EXPECT_EQ(std::set<std::string>(receipt.recovery_codes.begin(),receipt.recovery_codes.end()).size(),8);
        EXPECT_NE(receipt.domain_id,receipt.root_id);
        for (const auto& code : receipt.recovery_codes) EXPECT_EQ(code.size(),64);
    }
    {
        OfflineIdentityStore store(path);
        const auto status = store.status();
        EXPECT_TRUE(status.initialized);
        EXPECT_EQ(status.username,"not_a_reserved_root");
        EXPECT_EQ(status.root_id,receipt.root_id);
        EXPECT_EQ(status.domain_id,receipt.domain_id);
        EXPECT_THROW(store.initialize("attacker",password),std::runtime_error);
    }
    mutate([&](rocksdb::DB& db) {
        std::string bytes;
        ASSERT_TRUE(db.Get({},"sec:state:v1",&bytes).ok());
        EXPECT_EQ(bytes.find(password),std::string::npos);
        for (const auto& code : receipt.recovery_codes) EXPECT_EQ(bytes.find(code),std::string::npos);
        const auto state = Json::parse(bytes);
        EXPECT_TRUE(verifyPassword(state["root"]["password_hash"].get<std::string>(),password));
        EXPECT_EQ(state["audit"]["action"],"owner.bootstrap");
    });
}
TEST_F(NativeIam, RootIsAllowedAsOwnerChosenName) {
    enroll("root");
    OfflineIdentityStore store(path);
    EXPECT_EQ(store.status().username,"root");
}
TEST_F(NativeIam, ConcurrentBootstrapHasExactlyOneWinner) {
    OfflineIdentityStore store(path);
    std::atomic<int> winners{0}, rejected{0};
    std::vector<std::thread> workers;
    for (int i=0;i<8;++i) workers.emplace_back([&,i] {
        try { store.initialize("owner"+std::to_string(i),password); ++winners; }
        catch (const std::runtime_error&) { ++rejected; }
    });
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(winners,1); EXPECT_EQ(rejected,7);
}
TEST_F(NativeIam, RunningEngineBlocksOfflineInitialization) {
    nexora::core::DocEngine engine(path);
    EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
}
TEST_F(NativeIam, OfflineStoreBlocksSecondProcessHandle) {
    OfflineIdentityStore store(path);
    EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
}
TEST_F(NativeIam, EnrolledDatabaseDeniesLegacyNativeAndBindingEngine) {
    enroll();
    EXPECT_THROW(nexora::core::DocEngine{path},std::runtime_error);
}
TEST_F(NativeIam, MissingIdentityNeverReopensBootstrap) {
    enroll();
    mutate([](rocksdb::DB& db) { ASSERT_TRUE(db.Delete({},"sec:state:v1").ok()); });
    EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
    EXPECT_THROW(nexora::core::DocEngine{path},std::runtime_error);
}
TEST_F(NativeIam, DeletingBothSecurityRecordsStillLeavesReaderFence) {
    enroll();
    mutate([](rocksdb::DB& db) {
        ASSERT_TRUE(db.Delete({},"sec:state:v1").ok());
        ASSERT_TRUE(db.Delete({},"meta:iam:required").ok());
    });
    EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
    EXPECT_THROW(nexora::core::DocEngine{path},std::runtime_error);
}
TEST_F(NativeIam, CorruptAndEmptyIdentityFailClosed) {
    enroll();
    for (const auto* bytes : {"", "{broken"}) {
        mutate([&](rocksdb::DB& db) { ASSERT_TRUE(db.Put({},"sec:state:v1",bytes).ok()); });
        EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
        EXPECT_THROW(nexora::core::DocEngine{path},std::runtime_error);
    }
}
TEST_F(NativeIam, UnsupportedSchemaAndIdentityMismatchFailClosed) {
    enroll();
    mutate([](rocksdb::DB& db) {
        std::string bytes; ASSERT_TRUE(db.Get({},"sec:state:v1",&bytes).ok());
        auto state = Json::parse(bytes); state["schema"] = 999;
        ASSERT_TRUE(db.Put({},"sec:state:v1",state.dump()).ok());
    });
    EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
}
TEST_F(NativeIam, MalformedCredentialVerifierFailsClosed) {
    enroll();
    mutate([](rocksdb::DB& db) {
        std::string bytes; ASSERT_TRUE(db.Get({},"sec:state:v1",&bytes).ok());
        auto state = Json::parse(bytes);
        state["root"]["password_hash"] = "$argon2id$v=19$m=65536,t=2,p=1$" + std::string(69,'!');
        ASSERT_TRUE(db.Put({},"sec:state:v1",state.dump()).ok());
    });
    EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
    EXPECT_THROW(nexora::core::DocEngine{path},std::runtime_error);
}
TEST_F(NativeIam, PartialStateAndStraySecurityKeysNeverBootstrap) {
    { OfflineIdentityStore store(path); }
    mutate([](rocksdb::DB& db) { ASSERT_TRUE(db.Put({},"sec:orphan","{}").ok()); });
    EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
    EXPECT_THROW(nexora::core::DocEngine{path},std::runtime_error);
}
TEST_F(NativeIam, LegacyUsersAndDataRequireExplicitMigration) {
    { nexora::core::DocEngine engine(path); }
    mutate([](rocksdb::DB& db) {
        ASSERT_TRUE(db.Put({},"data:__nexora_internal_users:root","legacy record").ok());
    });
    OfflineIdentityStore store(path);
    EXPECT_THROW(store.initialize("new_owner",password),std::runtime_error);
    EXPECT_FALSE(store.status().initialized);
}
TEST_F(NativeIam, ExistingCollectionsRequireExplicitMigration) {
    { nexora::core::DocEngine engine(path); }
    mutate([](rocksdb::DB& db) { ASSERT_TRUE(db.Put({},"meta:col:payroll","{}").ok()); });
    OfflineIdentityStore store(path);
    EXPECT_THROW(store.initialize("new_owner",password),std::runtime_error);
}
TEST_F(NativeIam, InvalidInputDoesNotConsumeBootstrap) {
    OfflineIdentityStore store(path);
    for (const auto* username : {"", "name:with:delimiters", "name with spaces", "نام"})
        EXPECT_THROW(store.initialize(username,password),std::invalid_argument);
    EXPECT_THROW(store.initialize("owner","short"),std::invalid_argument);
    EXPECT_THROW(store.initialize("owner",std::string(1025,'x')),std::invalid_argument);
    EXPECT_THROW(store.initialize("owner",std::string("valid password\0hidden",21)),std::invalid_argument);
    EXPECT_FALSE(store.status().initialized);
    EXPECT_NO_THROW(store.initialize("valid.owner",password));
}
TEST(PasswordProfile, SaltedArgon2idAndBoundedVerification) {
    const auto first = hashPassword(password), second = hashPassword(password);
    EXPECT_NE(first,second);
    EXPECT_TRUE(first.starts_with("$argon2id$v=19$m=65536,t=2,p=1$"));
    EXPECT_TRUE(verifyPassword(first,password));
    EXPECT_FALSE(verifyPassword(first,"wrong password"));
    EXPECT_FALSE(verifyPassword("$argon2id$v=19$m=999999999,t=99999,p=1$bad",password));
    EXPECT_FALSE(verifyPassword("not-a-verifier",password));
    EXPECT_FALSE(verifyPassword(first,std::string(1025,'a')));
    std::string secret = "clear me"; wipe(secret); EXPECT_TRUE(secret.empty());
}
} // namespace
