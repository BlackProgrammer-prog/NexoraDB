#include "NativeIam.h"
#include "core/RocksTuning.h"
#include "core/DocumentCodec.h"

#include <sodium.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/write_batch.h>
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <stdexcept>

namespace nexora::security {
namespace {
using Json = nlohmann::json;
constexpr auto stateKey = "sec:state:v1";
constexpr auto fenceKey = "meta:iam:required";
constexpr auto manifestKey = "meta:format:components";
constexpr std::size_t recoveryCount = 8;
// One expensive operation per process, including verification; no unbounded KDF
// memory fan-out. SessionAuthority additionally rejects excess authentication work.
std::mutex kdfMutex;

void sodiumReady() {
    static const bool ready = sodium_init() >= 0;
    if (!ready) throw std::runtime_error("IAM cryptography initialization failed");
}
void checked(const rocksdb::Status& status) {
    if (!status.ok()) throw std::runtime_error("IAM storage operation failed: " + status.ToString());
}
rocksdb::ReadOptions readOptions() {
    rocksdb::ReadOptions options;
    options.verify_checksums = true;
    options.total_order_seek = true;
    return options;
}
std::string read(rocksdb::DB& db, const char* key) {
    std::string value;
    const auto result = db.Get(readOptions(), key, &value);
    if (result.IsNotFound()) return {};
    checked(result);
    // A present empty security key is corruption, never an absent key.
    if (value.empty()) throw std::runtime_error("IAM metadata is empty; maintenance required");
    return value;
}
bool hasPrefix(rocksdb::DB& db, const char* prefix) {
    std::unique_ptr<rocksdb::Iterator> it(db.NewIterator(readOptions()));
    it->Seek(prefix);
    const bool found = it->Valid() && it->key().starts_with(prefix);
    checked(it->status());
    return found;
}
Json legacyManifest() {
    return {{"manifest",1},{"min_reader",3},{"document_keys",1},
        {"document_envelope",1},{"indexes",3},{"graph_storage",2},{"graph_wal",2}};
}
Json iamManifest() { auto value = legacyManifest(); value["iam"] = 1; return value; }
bool hexId(const std::string& value, std::size_t size = 64) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string randomId() {
    std::array<unsigned char,32> bytes{};
    std::array<char,65> hex{};
    randombytes_buf(bytes.data(), bytes.size());
    sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}
bool supportedVerifier(std::string_view verifier) {
    // libsodium interactive Argon2id 1.3: 64 MiB, two passes, one lane.
    // Prevent attacker-controlled PHC parameters from requesting arbitrary RAM.
    if (verifier.size() != 97 || !verifier.starts_with("$argon2id$v=19$m=65536,t=2,p=1$") ||
        verifier.find('\0') != std::string_view::npos) return false;
    const std::string encoded(verifier);
    // This parses PHC metadata without running the KDF. Reject malformed salt
    // and hash encodings as well as unsupported profiles during store opening.
    return crypto_pwhash_str_needs_rehash(encoded.c_str(),crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0;
}
void validateUsername(std::string_view username) {
    if (username.empty() || username.size() > 64)
        throw std::invalid_argument("Username must contain 1..64 ASCII characters");
    for (const unsigned char c : username) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            throw std::invalid_argument("Username allows ASCII letters, digits, underscore, dash and dot");
    }
}
void validatePassword(std::string_view password) {
    if (password.size() < 15 || password.size() > 1024 || password.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Password must contain 15..1024 UTF-8 bytes without NUL");
}
BootstrapStatus inspect(rocksdb::DB& db) {
    const auto fence = read(db, fenceKey), state = read(db, stateKey);
    const auto manifest = read(db, manifestKey);
    const auto parsedManifest = manifest.empty() ? Json() : Json::parse(manifest);
    if (fence.empty() && state.empty()) {
        if (hasPrefix(db, "sec:") || (!parsedManifest.is_null() && parsedManifest.contains("iam")))
            throw std::runtime_error("IAM metadata missing or incomplete; maintenance required");
        return {};
    }
    try {
        if (fence.empty() || state.empty() || parsedManifest != iamManifest() ||
            read(db,"meta:format:documents") != "3") throw std::runtime_error("fence mismatch");
        const auto f = Json::parse(fence), s = Json::parse(state);
        const auto domain = s.at("domain_id").get<std::string>();
        const auto root = s.at("root_id").get<std::string>();
        const auto& user = s.at("root");
        if (f != Json({{"schema",1},{"domain_id",domain},{"root_id",root}}) ||
            s.at("schema") != 1 || s.at("bootstrap") != "INITIALIZED" ||
            !s.at("policy_epoch").is_number_unsigned() || s.at("policy_epoch").get<uint64_t>() < 1 || !hexId(domain) || !hexId(root) ||
            user.at("id") != root || user.at("protected_root") != true ||
            user.at("status") != "active" || !user.at("credential_generation").is_number_unsigned() || user.at("credential_generation").get<uint64_t>() < 1 ||
            !user.at("roles").is_array() || std::find(user.at("roles").begin(),user.at("roles").end(),"superuser")==user.at("roles").end() ||
            !s.at("roles").is_object() || s.at("roles").at("superuser") != Json({{"builtin",true}}) ||
            !supportedVerifier(user.at("password_hash").get<std::string>()) ||
            s.at("recovery").size() != recoveryCount ||
            s.at("audit").at("action") != "owner.bootstrap" ||
            s.at("audit").at("principal_id") != root)
            throw std::runtime_error("invalid identity metadata");
        const auto username = user.at("username").get<std::string>();
        validateUsername(username);
        std::vector<std::string> seen;
        for (const auto& entry : s.at("recovery")) {
            const auto digest = entry.at("digest").get<std::string>();
            if (!hexId(digest) || !entry.at("used").is_boolean() ||
                std::find(seen.begin(), seen.end(), digest) != seen.end())
                throw std::runtime_error("invalid recovery metadata");
            seen.push_back(digest);
        }
        return {true,domain,root,username};
    } catch (const std::exception&) {
        // Never include serialized metadata (credential verifiers) in errors.
        throw std::runtime_error("IAM metadata is corrupt or unsupported; maintenance required");
    }
}
void requireFreshStore(rocksdb::DB& db) {
    // Importing existing identities/data is an explicit future offline migration.
    if (hasPrefix(db,"data:") || hasPrefix(db,"idx:") || hasPrefix(db,"idx2") || hasPrefix(db,"idx3"))
        throw std::runtime_error("Existing data requires an audited IAM migration; bootstrap refused");
    std::unique_ptr<rocksdb::Iterator> it(db.NewIterator(readOptions()));
    for (it->Seek("meta:col:"); it->Valid() && it->key().starts_with("meta:col:"); it->Next()) {
        const auto key = it->key().ToString();
        if (key != "meta:col:__nexora_internal_users" && key != "meta:col:__nexora_internal_app_tokens")
            throw std::runtime_error("Existing collections require an audited IAM migration; bootstrap refused");
    }
    checked(it->status());
    const auto manifest = read(db,manifestKey);
    const auto generation = read(db,"meta:format:documents");
    if ((!manifest.empty() && Json::parse(manifest) != legacyManifest()) ||
        (!generation.empty() && generation != "1" && generation != "2" && generation != "3"))
        throw std::runtime_error("Unsupported storage format; bootstrap refused");
    if ((generation == "2" || generation == "3") && manifest.empty())
        throw std::runtime_error("Missing storage manifest; bootstrap refused");
}
} // namespace

void wipe(std::string& secret) noexcept {
    if (!secret.empty()) sodium_memzero(secret.data(),secret.size());
    secret.clear();
}
std::string hashPassword(std::string_view password) {
    sodiumReady(); validatePassword(password);
    std::lock_guard lock(kdfMutex);
    std::array<char,crypto_pwhash_STRBYTES> encoded{};
    if (crypto_pwhash_str_alg(encoded.data(),password.data(),password.size(),
            crypto_pwhash_OPSLIMIT_INTERACTIVE,crypto_pwhash_MEMLIMIT_INTERACTIVE,
            crypto_pwhash_ALG_ARGON2ID13) != 0)
        throw std::runtime_error("IAM password hashing resource allocation failed");
    return encoded.data();
}
bool verifyPassword(std::string_view verifier, std::string_view password) {
    sodiumReady();
    if (!supportedVerifier(verifier) || password.size() > 1024 || password.find('\0') != std::string_view::npos)
        return false;
    std::lock_guard lock(kdfMutex);
    const std::string encoded(verifier);
    return crypto_pwhash_str_verify(encoded.c_str(),password.data(),password.size()) == 0;
}

struct OfflineIdentityStore::Impl {
    std::unique_ptr<rocksdb::TransactionDB> db;
    mutable std::mutex mutex;
    bool commit_uncertain = false;
};
OfflineIdentityStore::OfflineIdentityStore(const std::string& path, rocksdb::Env* environment_for_testing)
    :impl_(std::make_unique<Impl>()) {
    sodiumReady();
    if (path.empty()) throw std::invalid_argument("Explicit database directory required");
    if (std::filesystem::create_directories(path))
        std::filesystem::permissions(path,std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
    // Match engine comparators/table format; no worker threads or legacy user
    // creation. RocksDB's exclusive LOCK prevents online bootstrap/recovery.
    core::RocksSettings settings;
    settings.env_for_testing = environment_for_testing;
    auto resources = std::make_shared<core::RocksResources>(settings.memory_budget_bytes,settings.block_cache_bytes);
    auto options = core::MakeRocksOptions(settings,resources);
    rocksdb::TransactionDB* raw = nullptr;
    checked(rocksdb::TransactionDB::Open(options,{},path,&raw));
    impl_->db.reset(raw);
    (void)inspect(*impl_->db);
}
OfflineIdentityStore::~OfflineIdentityStore() = default;
BootstrapStatus OfflineIdentityStore::status() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->commit_uncertain) throw std::runtime_error("IAM commit outcome uncertain; close and inspect offline");
    return inspect(*impl_->db);
}
BootstrapReceipt OfflineIdentityStore::initialize(std::string_view username, std::string_view password) {
    return initializeImpl(username,password,false,"");
}
BootstrapReceipt OfflineIdentityStore::adoptExisting(std::string_view username,std::string_view password,
        std::string_view confirmation,std::string_view previous_owner_password) {
    if(confirmation!="ADOPT EXISTING DATA") throw std::invalid_argument("Explicit migration confirmation required");
    return initializeImpl(username,password,true,previous_owner_password);
}
BootstrapReceipt OfflineIdentityStore::initializeImpl(std::string_view username,std::string_view password,
        bool adopting,std::string_view previous_owner_password) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->commit_uncertain) throw std::runtime_error("IAM commit outcome uncertain; close and inspect offline");
    if (inspect(*impl_->db).initialized) throw std::runtime_error("IAM is already initialized; bootstrap permanently closed");
    if(!adopting) requireFreshStore(*impl_->db);
    else {
        const auto manifest=read(*impl_->db,manifestKey);
        const auto generation=read(*impl_->db,"meta:format:documents");
        if((!manifest.empty() && Json::parse(manifest)!=legacyManifest()) ||
            (!generation.empty() && generation!="1" && generation!="3") ||
            (generation=="3" && manifest.empty())) throw std::runtime_error("Migrate the storage format before IAM adoption");
        std::string old;
        const auto result=impl_->db->Get(readOptions(),"data:__nexora_internal_users:root",&old);
        if(result.ok()) {
            const auto decoded=core::DocumentCodec::DecodeToJson(old);
            if(!decoded.success) throw std::runtime_error("Legacy owner metadata is corrupt");
            const auto previous=Json::parse(decoded.value);
            if(previous.at("role")!="admin" || previous.at("status")!="active")
                throw std::runtime_error("Legacy owner is not active; migration requires security review");
            const auto verifier=previous.at("password_hash").get<std::string>();
            const bool bounded=verifier.size()==97 && (verifier.starts_with("$argon2id$v=19$m=65536,t=3,p=4$") || supportedVerifier(verifier));
            std::lock_guard kdf(kdfMutex);
            if(!bounded || previous_owner_password.size()>1024 ||
                crypto_pwhash_str_verify(verifier.c_str(),previous_owner_password.data(),previous_owner_password.size())!=0)
                throw std::runtime_error("Legacy owner authentication failed; migration refused");
        } else {
            checked(result.IsNotFound()?rocksdb::Status::OK():result);
            if(hasPrefix(*impl_->db,"data:__nexora_internal_users:") || hasPrefix(*impl_->db,"data:__nexora_internal_app_tokens:"))
                throw std::runtime_error("Legacy identity store has no owner; migration requires review");
        }
    }
    validateUsername(username);
    const auto verifier = hashPassword(password);
    BootstrapReceipt receipt{randomId(),randomId(),{}};
    Json recovery = Json::array();
    for (std::size_t i=0;i<recoveryCount;++i) {
        auto code = randomId(); // 256-bit offline bearer factor.
        auto input = std::string("nexoradb-recovery-v1:") + receipt.domain_id + ":" + code;
        std::array<unsigned char,32> digest{};
        std::array<char,65> encoded{};
        crypto_generichash(digest.data(),digest.size(),
            reinterpret_cast<const unsigned char*>(input.data()),input.size(),nullptr,0);
        wipe(input);
        sodium_bin2hex(encoded.data(),encoded.size(),digest.data(),digest.size());
        recovery.push_back({{"digest",encoded.data()},{"used",false}});
        receipt.recovery_codes.push_back(std::move(code));
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Json state = {{"schema",1},{"bootstrap","INITIALIZED"},{"domain_id",receipt.domain_id},
        {"root_id",receipt.root_id},{"policy_epoch",uint64_t(1)},
        {"root",{{"id",receipt.root_id},{"username",username},{"protected_root",true},
            {"status","active"},{"roles",Json::array({"superuser"})},{"credential_generation",1},
            {"password_hash",verifier},{"created_at",timestamp}}},
        {"roles",{{"superuser",{{"builtin",true}}}}},{"recovery",recovery},
        {"audit",{{"event_id",randomId()},{"action","owner.bootstrap"},
            {"principal_id",receipt.root_id},{"timestamp",timestamp},{"policy_epoch",1},{"outcome","success"}}}};
    Json fence = {{"schema",1},{"domain_id",receipt.domain_id},{"root_id",receipt.root_id}};
    rocksdb::WriteBatch batch;
    batch.Put(stateKey,state.dump());
    batch.Put(fenceKey,fence.dump());
    // Old binaries reject the changed component manifest. This is committed
    // with credentials, roles, recovery enrollment and the bootstrap audit.
    batch.Put(manifestKey,iamManifest().dump());
    batch.Put("meta:format:documents","3");
    if(adopting) {
        for(const auto* prefix:{"data:__nexora_internal_users:","data:__nexora_internal_app_tokens:"}) {
            std::unique_ptr<rocksdb::Iterator> it(impl_->db->NewIterator(readOptions()));
            for(it->Seek(prefix);it->Valid() && it->key().starts_with(prefix);it->Next()) batch.Delete(it->key());
            checked(it->status());
        }
        batch.Put("seq:__nexora_internal_users","0");batch.Put("seq:__nexora_internal_app_tokens","0");
        batch.Put("sec:migration:v1",Json({{"adopted_existing_data",true},{"timestamp",timestamp},
            {"legacy_credentials_revoked",true},{"root_id",receipt.root_id}}).dump());
    }
    rocksdb::WriteOptions options;
    options.sync = true;
    options.disableWAL = false;
    impl_->commit_uncertain = true;
    checked(impl_->db->Write(options,&batch));
    impl_->commit_uncertain = false;
    return receipt;
}
void OfflineIdentityStore::recoverRoot(std::string_view recovery_code,std::string_view new_password) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->commit_uncertain) throw std::runtime_error("IAM commit outcome uncertain; inspect offline");
    const auto status=inspect(*impl_->db);
    if(!status.initialized || recovery_code.size()!=64) throw std::runtime_error("Invalid recovery factor");
    auto state=Json::parse(read(*impl_->db,stateKey));
    auto input=std::string("nexoradb-recovery-v1:")+status.domain_id+":"+std::string(recovery_code);
    unsigned char bytes[32];char hex[65];crypto_generichash(bytes,sizeof bytes,
        reinterpret_cast<const unsigned char*>(input.data()),input.size(),nullptr,0);
    wipe(input);sodium_bin2hex(hex,sizeof hex,bytes,sizeof bytes);
    size_t selected=recoveryCount;
    for(size_t i=0;i<recoveryCount;++i) {
        const auto verifier=state.at("recovery").at(i).at("digest").get<std::string>();
        if(sodium_memcmp(verifier.data(),hex,64)==0 && state.at("recovery").at(i).at("used")==false) selected=i;
    }
    if(selected==recoveryCount) throw std::runtime_error("Invalid or consumed recovery factor");
    const auto replacement=hashPassword(new_password);
    state["recovery"][selected]["used"]=true;auto& root=state["root"];
    root["password_hash"]=replacement;root["credential_generation"]=root.at("credential_generation").get<uint64_t>()+1;
    // Emergency owner recovery revokes every persistent delegated credential,
    // including credentials issued by other superusers, and pending resets.
    state["tokens"]=Json::object();
    if(state.contains("users")) for(auto& user:state["users"]) {
        user["credential_generation"]=user.at("credential_generation").get<uint64_t>()+1;
        user.erase("reset");
    }
    state["policy_epoch"]=state.at("policy_epoch").get<uint64_t>()+1;
    const auto sequence=state.value("audit_sequence",uint64_t(0))+1;
    const auto hash=[](const std::string& value) {unsigned char digest[32];char out[65];
        crypto_generichash(digest,sizeof digest,reinterpret_cast<const unsigned char*>(value.data()),value.size(),nullptr,0);
        sodium_bin2hex(out,sizeof out,digest,sizeof digest);return std::string(out);};
    const auto previous=state.value("audit_head",hash(state.at("audit").dump()));
    Json event={{"sequence",sequence},{"event_id",randomId()},{"timestamp",std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()},{"principal_id",status.root_id},
        {"action","owner.recover.offline"},{"target",status.root_id},{"outcome","success"},{"previous",previous},
        {"policy_epoch",state.at("policy_epoch")}};
    const auto head=hash(event.dump());event["digest"]=head;state["audit_sequence"]=sequence;state["audit_head"]=head;
    auto suffix=std::to_string(sequence);suffix=std::string(20-suffix.size(),'0')+suffix;
    rocksdb::WriteBatch batch;batch.Put(stateKey,state.dump());batch.Put("sec:audit:"+suffix,event.dump());
    rocksdb::WriteOptions options;options.sync=true;options.disableWAL=false;
    impl_->commit_uncertain=true;checked(impl_->db->Write(options,&batch));impl_->commit_uncertain=false;
}
void requireLegacyAccessAllowed(rocksdb::DB& db) {
    // Do not parse/expose credentials to the legacy engine. Any trace of IAM
    // enrollment locks it, including partially deleted/corrupted metadata.
    const auto manifest = read(db,manifestKey);
    if (!read(db,fenceKey).empty() || hasPrefix(db,"sec:") ||
        (!manifest.empty() && Json::parse(manifest).contains("iam")))
        throw std::runtime_error("Native IAM store requires SecureDatabase and an authenticated session; legacy access is disabled");
}
void requireEnrolledStore(rocksdb::DB& db) {
    sodiumReady();
    if (!inspect(db).initialized) throw std::runtime_error("Native IAM is not initialized; run local nexora_iam init/adopt first");
}
} // namespace nexora::security
