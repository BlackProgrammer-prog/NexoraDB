#include "core/security/SecureDatabase.h"
#include "TestTempDir.h"
#include "RecoveryTestSupport.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>
namespace {
using namespace nexora::security;
namespace core=nexora::core;
using Json=nlohmann::json;
constexpr auto password="correct horse battery staple";
constexpr auto replacement="new correct horse battery staple";
class SecureIam:public testing::Test {
protected:
    TestTempDir directory{"secure_iam"};
    std::string path=(directory.path()/"db").string();
    BootstrapReceipt receipt;
    std::shared_ptr<SecureDatabase> db;
    std::string owner;
    void SetUp() override {
        {OfflineIdentityStore store(path);receipt=store.initialize("chosen.owner",password);}
        db=std::make_shared<SecureDatabase>(path);owner=db->login("chosen.owner",password,"test");
    }
    Json admin(const std::string& op,const Json& args=Json::object()) {return Json::parse(db->administer(owner,op,args.dump()));}
    std::string user(const std::string& name="reader") {return admin("create_user",{{"username",name},{"password",password}}).at("_id");}
    void grant(const std::string& id,const Json& grants) {admin("set_user",{{"id",id},{"grants",grants}});}
    core::DBResult data(const std::string& token,const std::string& op,const std::string& collection="items",const std::string& payload="") {
        DataRequest request;request.operation=op;request.collection=collection;request.payload=payload;
        return std::get<core::DBResult>(db->execute(token,request));
    }
};
TEST_F(SecureIam, NativeLoginAndDefaultDenyAcrossRawAndSecureInterfaces) {
    EXPECT_THROW(data("forged","find_many"),AuthenticationError);
    EXPECT_THROW(nexora::core::DocEngine{path},std::runtime_error);
    EXPECT_THROW(db->login("missing",password,"test"),AuthenticationError);
    EXPECT_THROW(db->login("chosen.owner","wrong","test"),AuthenticationError);
    const auto id=user();const auto token=db->login("reader",password,"test");
    EXPECT_THROW(data(token,"find_many"),AuthorizationError);
    EXPECT_THROW(db->administer(token,"list_users","{}"),AuthorizationError);
    grant(id,{{"items",Json::array({"read"})}});
    EXPECT_THROW(data(token,"find_many"),AuthenticationError);
    const auto active=db->login("reader",password,"test");
    ASSERT_TRUE(data(owner,"create_collection").success);
    ASSERT_TRUE(data(owner,"insert_one","items",R"({"_id":"a","value":1})").success);
    EXPECT_TRUE(data(active,"find_many").success);
    EXPECT_THROW(data(active,"insert_one","items",R"({"value":2})"),AuthorizationError);
    grant(id,Json::object());
    EXPECT_THROW(data(active,"find_many"),AuthenticationError);
}
TEST_F(SecureIam, PasswordChangeRevokesEverySessionAndNeverTrimsSecret) {
    const auto second=db->login("chosen.owner",password,"test");
    db->changePassword(owner,password,"  a password with deliberate spaces  ");
    EXPECT_THROW(db->claims(owner),AuthenticationError);
    EXPECT_THROW(db->claims(second),AuthenticationError);
    EXPECT_THROW(db->login("chosen.owner",password,"test"),AuthenticationError);
    EXPECT_THROW(db->login("chosen.owner","a password with deliberate spaces","test"),AuthenticationError);
    EXPECT_NO_THROW(db->login("chosen.owner","  a password with deliberate spaces  ","test"));
}
TEST_F(SecureIam, SingleUseAdministrativeResetAndProtectedOwner) {
    const auto id=user();const auto old=db->login("reader",password,"test");
    const auto reset=admin("issue_reset",{{"id",id}}).at("resetToken").get<std::string>();
    EXPECT_THROW(db->claims(old),AuthenticationError);
    EXPECT_THROW(db->login("reader",password,"test"),AuthenticationError);
    EXPECT_THROW(admin("issue_reset",{{"id",receipt.root_id}}),AuthorizationError);
    db->completeReset(reset,replacement);
    EXPECT_THROW(db->completeReset(reset,password),AuthenticationError);
    EXPECT_NO_THROW(db->login("reader",replacement,"test"));
}
TEST_F(SecureIam, DisableLogoutRootProtectionAndRenameUseImmutableIds) {
    const auto id=user();const auto token=db->login("reader",password,"test");
    admin("set_user",{{"id",id},{"status","disabled"}});EXPECT_THROW(db->claims(token),AuthenticationError);
    EXPECT_THROW(admin("set_user",{{"id",receipt.root_id},{"status","disabled"}}),AuthorizationError);
    EXPECT_THROW(admin("set_user",{{"id",receipt.root_id},{"roles",Json::array()}}),AuthorizationError);
    admin("set_user",{{"id",receipt.root_id},{"username","renamed.owner"}});
    EXPECT_THROW(db->claims(owner),AuthenticationError);
    owner=db->login("renamed.owner",password,"test");
    EXPECT_EQ(Json::parse(db->claims(owner)).at("sub"),receipt.root_id);
    db->logout(owner);EXPECT_THROW(db->claims(owner),AuthenticationError);
}
TEST_F(SecureIam, RoleInheritanceCyclesAndLiveRevocation) {
    ASSERT_TRUE(data(owner,"create_collection").success);
    admin("set_role",{{"name","base"},{"grants",{{"items",Json::array({"read"})}}}});
    admin("set_role",{{"name","child"},{"parents",Json::array({"base"})},{"grants",Json::object()}});
    const auto id=user();admin("set_user",{{"id",id},{"roles",Json::array({"child"})}});
    const auto token=db->login("reader",password,"test");EXPECT_TRUE(data(token,"find_many").success);
    EXPECT_THROW(admin("set_role",{{"name","base"},{"parents",Json::array({"child"})},{"grants",Json::object()}}),std::invalid_argument);
    admin("set_role",{{"name","base"},{"grants",Json::object()}});
    EXPECT_THROW(db->claims(token),AuthenticationError);
    const auto current=db->login("reader",password,"test");EXPECT_THROW(data(current,"find_many"),AuthorizationError);
    db.reset();db=std::make_shared<SecureDatabase>(path);owner=db->login("chosen.owner",password,"test");
    EXPECT_EQ(admin("list_roles").at("child").at("parents")[0],"base");
}
TEST_F(SecureIam, NativeServiceScopesCollectionIsolationAndDurableRevocation) {
    ASSERT_TRUE(data(owner,"create_collection").success);
    const auto service=admin("issue_token",{{"appId","application"},{"scopes",Json::array({"documents:read"})},
        {"collections",{{"items",Json::array({"read","write"})}}}});
    const auto token=service.at("token").get<std::string>();
    EXPECT_TRUE(data(token,"find_many").success);
    EXPECT_THROW(data(token,"insert_one","items","{}"),AuthorizationError);
    EXPECT_THROW(data(token,"find_many","other"),AuthorizationError);
    EXPECT_THROW(db->administer(token,"list_users","{}"),AuthorizationError);
    db.reset();db=std::make_shared<SecureDatabase>(path);
    EXPECT_TRUE(data(token,"find_many").success);
    EXPECT_THROW(db->claims(owner),AuthenticationError);
    owner=db->login("chosen.owner",password,"test");
    admin("revoke_token",{{"id",service.at("tokenId")}});EXPECT_THROW(db->claims(token),AuthenticationError);
}
TEST_F(SecureIam, CursorOwnershipAndRevocationBeforeNextPage) {
    ASSERT_TRUE(data(owner,"create_collection").success);
    for(const auto* id:{"a","b","c"}) ASSERT_TRUE(data(owner,"insert_one","items",Json({{"_id",id}}).dump()).success);
    DataRequest request;request.operation="find_page";request.collection="items";request.limit=1;
    const auto page=std::get<core::PageResult>(db->execute(owner,request));ASSERT_TRUE(page.success);ASSERT_FALSE(page.continuation_token.empty());
    request.page_token=page.continuation_token;
    const auto second=db->login("chosen.owner",password,"test");
    EXPECT_THROW(db->execute(second,request),AuthorizationError);
    db->logout(owner);EXPECT_THROW(db->execute(owner,request),AuthenticationError);
}
TEST_F(SecureIam, RootRecoveryRequiresOfflineFactorConsumesItAndPreservesUsers) {
    const auto id=user();(void)id;
    EXPECT_THROW(OfflineIdentityStore{path},std::runtime_error);
    db.reset();
    {OfflineIdentityStore store(path);
        EXPECT_THROW(store.recoverRoot(std::string(64,'0'),replacement),std::runtime_error);
        store.recoverRoot(receipt.recovery_codes[0],replacement);
        EXPECT_THROW(store.recoverRoot(receipt.recovery_codes[0],password),std::runtime_error);
        EXPECT_TRUE(store.status().initialized);
    }
    db=std::make_shared<SecureDatabase>(path);
    EXPECT_THROW(db->login("chosen.owner",password,"test"),AuthenticationError);
    owner=db->login("chosen.owner",replacement,"test");
    EXPECT_EQ(admin("list_users").size(),2);
    bool found=false;for(const auto& entry:admin("audit")) found |= entry.at("action")=="owner.recover.offline";
    EXPECT_TRUE(found);
}
TEST_F(SecureIam, AccountRateLimitBoundsRepeatedPasswordVerification) {
    for(int i=0;i<5;++i) EXPECT_THROW(db->login("chosen.owner","wrong","same-source"),AuthenticationError);
    EXPECT_THROW(db->login("chosen.owner",password,"another-source"),RateLimitError);
}
TEST_F(SecureIam, TransactionCannotCrossSessionsOrCommitAfterPolicyChange) {
    ASSERT_TRUE(data(owner,"create_collection").success);
    DataRequest request;request.operation="begin_transaction";
    auto transaction=std::get<std::shared_ptr<SecureTransaction>>(db->execute(owner,request));
    ASSERT_TRUE(transaction->isValid());
    request.transaction=transaction;request.operation="insert_one_tx";request.collection="items";request.payload=R"({"_id":"uncommitted"})";
    EXPECT_TRUE(std::get<core::DBResult>(db->execute(owner,request)).success);
    const auto other=db->login("chosen.owner",password,"other");
    request.operation="commit_transaction";
    EXPECT_THROW(db->execute(other,request),AuthorizationError);
    user("policy_change");
    EXPECT_THROW(db->execute(owner,request),AuthorizationError);
    EXPECT_FALSE(transaction->isValid());
    DataRequest read;read.operation="find_by_id";read.collection="items";read.id="uncommitted";
    EXPECT_FALSE(std::get<core::DBResult>(db->execute(owner,read)).success);
}
TEST_F(SecureIam, IdleAndAbsoluteExpiryUseNativeClock) {
    db.reset();SessionConfig config;config.idle_seconds=1;config.absolute_seconds=2;
    db=std::make_shared<SecureDatabase>(path,core::RocksSettings{},config);
    const auto idle=db->login("chosen.owner",password,"test");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_THROW(db->claims(idle),AuthenticationError);
    const auto active=db->login("chosen.owner",password,"test");
    for(int i=0;i<3;++i) {std::this_thread::sleep_for(std::chrono::milliseconds(600));EXPECT_NO_THROW(db->claims(active));}
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_THROW(db->claims(active),AuthenticationError);
}
TEST_F(SecureIam, RecoveryRotationRequiresPasswordAndRetiresOldFactors) {
    EXPECT_THROW(admin("rotate_recovery",{{"currentPassword","wrong"}}),AuthenticationError);
    const auto codes=admin("rotate_recovery",{{"currentPassword",password}}).at("recoveryCodes");
    ASSERT_EQ(codes.size(),8);db.reset();
    OfflineIdentityStore store(path);
    EXPECT_THROW(store.recoverRoot(receipt.recovery_codes[0],replacement),std::runtime_error);
    EXPECT_NO_THROW(store.recoverRoot(codes[0].get<std::string>(),replacement));
}
TEST_F(SecureIam, SecurityWalFailureNeverLeavesAnActivePermissiveAuthority) {
    db.reset();recovery_test::FaultEnv env;
    auto settings=recovery_test::settings(&env);
    db=std::make_shared<SecureDatabase>(path,settings);owner=db->login("chosen.owner",password,"test");
    env.failure=1;
    EXPECT_THROW(admin("set_role",{{"name","failing"},{"grants",Json::object()}}),std::runtime_error);
    EXPECT_FALSE(db->ready());
    EXPECT_THROW(db->claims(owner),std::runtime_error);
    EXPECT_FALSE(db->prepareShutdown().success);
    db.reset(); // Must not attempt a fresh SyncWAL after the known append error.
}
TEST(SecureIamMigration, ExistingDataIsAdoptedOnlyExplicitlyAndLegacyOwnerMustAuthenticate) {
    TestTempDir directory("iam_adopt");const auto path=(directory.path()/"db").string();
    {core::DocEngine engine(path);ASSERT_TRUE(engine.CreateCollection("items").success);
        ASSERT_TRUE(engine.InsertOne("items",R"({"_id":"original"})").success);
        Json old={{"username","root"},{"role","admin"},{"status","active"},{"password_hash",hashPassword(password)},
            {"email","root@example.org"},{"first_name","Old"},{"last_name","Owner"}};
        ASSERT_TRUE(engine.CreateInternalUser(old.dump()).success);
    }
    {OfflineIdentityStore store(path);EXPECT_THROW(store.initialize("new.owner",replacement),std::runtime_error);
        EXPECT_THROW(store.adoptExisting("new.owner",replacement,"ADOPT EXISTING DATA","wrong"),std::runtime_error);
        EXPECT_NO_THROW(store.adoptExisting("new.owner",replacement,"ADOPT EXISTING DATA",password));}
    SecureDatabase db(path);const auto token=db.login("new.owner",replacement,"test");
    DataRequest request;request.operation="find_by_id";request.collection="items";request.id="original";
    EXPECT_TRUE(std::get<core::DBResult>(db.execute(token,request)).success);
}
}
