#include "SessionAuthority.h"
#include <nlohmann/json.hpp>
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>
#include <sodium.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace nexora::security {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string randomSecret() {
    unsigned char bytes[32]; char hex[65]; randombytes_buf(bytes,sizeof bytes);
    sodium_bin2hex(hex,sizeof hex,bytes,sizeof bytes); return hex;
}
std::string digest(std::string_view input) {
    unsigned char bytes[32]; char hex[65];
    crypto_generichash(bytes,sizeof bytes,reinterpret_cast<const unsigned char*>(input.data()),input.size(),nullptr,0);
    sodium_bin2hex(hex,sizeof hex,bytes,sizeof bytes); return hex;
}
bool secretEqual(const std::string& a,const std::string& b) {
    return a.size()==b.size() && sodium_memcmp(a.data(),b.data(),a.size())==0;
}
void storage(const rocksdb::Status& value) {
    if(!value.ok()) throw std::runtime_error("IAM durable storage failed; maintenance required");
}
bool nameValid(const std::string& name,std::size_t maximum=64) {
    return !name.empty() && name.size()<=maximum &&
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")==std::string::npos;
}
void username(const std::string& name) {
    if(!nameValid(name)) throw std::invalid_argument("Invalid username (1..64 case-sensitive ASCII letters, digits, dot, dash, underscore)");
}
void collection(const std::string& name) {
    if(!nameValid(name,80) || name.starts_with("__nexora_") || name.find('.')!=std::string::npos || name.find('-')!=std::string::npos)
        throw AuthorizationError("Invalid or protected collection resource");
}
void grantsValid(const Json& grants) {
    if(!grants.is_object() || grants.size()>128) throw std::invalid_argument("Collection grants must be an object with at most 128 resources");
    for(auto it=grants.begin();it!=grants.end();++it) {
        collection(it.key());
        if(!it.value().is_array() || it.value().size()>3) throw std::invalid_argument("Invalid collection grants");
        for(const auto& action:it.value()) if(action!="read" && action!="write" && action!="manage")
            throw std::invalid_argument("Supported collection permissions are read, write, manage");
    }
}
void exactFields(const Json& value,std::initializer_list<const char*> fields) {
    if(!value.is_object()) throw std::invalid_argument("Expected an object");
    for(auto it=value.begin();it!=value.end();++it) {
        bool known=false;for(const auto* key:fields) known |= it.key()==key;
        if(!known) throw std::invalid_argument("Unsupported security request field");
    }
}
const Json& userById(const Json& state,const std::string& id) {
    if(state.at("root_id")==id) return state.at("root");
    if(state.at("users").contains(id)) return state.at("users").at(id);
    throw AuthenticationError("Session is invalid or expired");
}
Json& mutableUser(Json& state,const std::string& id) {
    if(state.at("root_id")==id) return state.at("root");
    if(!state.at("users").contains(id)) throw std::invalid_argument("Unknown identity");
    return state.at("users").at(id);
}
std::string findUser(const Json& state,const std::string& name) {
    if(state.at("root").at("username")==name) return state.at("root_id");
    for(auto it=state.at("users").begin();it!=state.at("users").end();++it)
        if(it.value().at("username")==name) return it.key();
    return {};
}
bool super(const Json& user) {
    const auto& roles=user.at("roles");return std::find(roles.begin(),roles.end(),"superuser")!=roles.end();
}
Json publicUser(const Json& user) {
    const auto elevated=super(user);
    return {{"_id",user.at("id")},{"username",user.at("username")},{"role",elevated?"admin":"application"},
        {"roles",user.at("roles")},{"grants",user.value("grants",Json::object())},{"protectedRoot",user.value("protected_root",false)},
        {"status",user.at("status")},{"createdAt",user.at("created_at")},
        {"updatedAt",user.value("updated_at",user.at("created_at").get<int64_t>())},
        {"lastLoginAt",user.value("last_login_at",Json())},{"email",nullptr},{"firstName",nullptr},
        {"lastName",nullptr},{"displayName",user.at("username")}};
}
bool hasGrant(const Json& grants,const std::string& resource,const std::string& action) {
    if(!grants.contains(resource)) return false;
    const auto& actions=grants.at(resource);return std::find(actions.begin(),actions.end(),action)!=actions.end();
}
bool roleAllows(const Json& state,const std::string& role,const std::string& resource,
    const std::string& action,std::unordered_set<std::string>& visited) {
    if(!visited.insert(role).second) return false;
    if(!state.at("roles").contains(role) || role=="superuser") return false;
    const auto& record=state.at("roles").at(role);
    if(hasGrant(record.at("grants"),resource,action)) return true;
    for(const auto& parent:record.at("parents"))
        if(roleAllows(state,parent.get<std::string>(),resource,action,visited)) return true;
    return false;
}
bool allows(const Json& state,const Json& user,const std::string& resource,const std::string& action) {
    if(super(user)) return true;
    if(hasGrant(user.value("grants",Json::object()),resource,action)) return true;
    std::unordered_set<std::string> visited;
    for(const auto& role:user.at("roles"))
        if(roleAllows(state,role.get<std::string>(),resource,action,visited)) return true;
    return false;
}
void validateRoleGraph(const Json& roles) {
    if(roles.size()>128) throw std::invalid_argument("Too many roles");
    std::unordered_set<std::string> visiting,done;
    const auto visit=[&](const auto& self,const std::string& role)->void {
        if(role=="superuser" || !roles.contains(role)) throw std::invalid_argument("Unknown or protected parent role");
        if(done.contains(role)) return;
        if(!visiting.insert(role).second) throw std::invalid_argument("Role inheritance cycle");
        for(const auto& parent:roles.at(role).at("parents")) self(self,parent.get<std::string>());
        visiting.erase(role);done.insert(role);
    };
    for(auto it=roles.begin();it!=roles.end();++it) if(it.key()!="superuser") {
        username(it.key());grantsValid(it.value().at("grants"));
        if(!it.value().at("parents").is_array() || it.value().at("parents").size()>16)
            throw std::invalid_argument("Invalid role inheritance");
        visit(visit,it.key());
    }
}
struct AuthAdmission {
    std::atomic_flag& flag;
    explicit AuthAdmission(std::atomic_flag& value):flag(value) {
        if(flag.test_and_set()) throw RateLimitError("Authentication capacity exceeded; retry later");
    }
    ~AuthAdmission(){flag.clear();}
};
// Stop admitting new reads while a security writer waits. A stream of fresh
// requests must not starve credential revocation behind a reader-preferred OS
// rwlock. Already admitted operations finish under their bounded execution.
struct PolicyRead {
    std::shared_lock<std::shared_mutex> lock;
    PolicyRead(std::shared_mutex& mutex,std::atomic<unsigned>& writers):lock(mutex,std::defer_lock) {
        if(writers.load()!=0) throw RateLimitError("Security update in progress; retry");
        lock.lock();
        if(writers.load()!=0) {lock.unlock();throw RateLimitError("Security update in progress; retry");}
    }
};
struct PolicyWrite {
    std::atomic<unsigned>& writers;
    std::unique_lock<std::shared_mutex> lock;
    PolicyWrite(std::shared_mutex& mutex,std::atomic<unsigned>& waiting):writers(waiting),lock(mutex,std::defer_lock) {
        ++writers;try {lock.lock();}catch(...){--writers;throw;}
    }
    ~PolicyWrite(){lock.unlock();--writers;}
};
}

struct AccessLease::Impl {
    std::shared_ptr<void> owner;
    PolicyRead lock;
    Impl(std::shared_ptr<void> value,std::shared_mutex& mutex,std::atomic<unsigned>& writers)
        :owner(std::move(value)),lock(mutex,writers) {}
};
AccessLease::AccessLease(std::unique_ptr<Impl> value):impl_(std::move(value)) {}
AccessLease::~AccessLease()=default;
AccessLease::AccessLease(AccessLease&&) noexcept=default;
AccessLease& AccessLease::operator=(AccessLease&&) noexcept=default;

struct SessionAuthority::Impl {
    rocksdb::DB& db;
    SessionConfig config;
    mutable std::shared_mutex policy;
    std::atomic<unsigned> waitingWriters{0};
    Json state;
    std::atomic<bool> uncertain{false};
    struct Session {std::string user;uint64_t generation;Clock::time_point created,last;int64_t expires;};
    struct Attempt {unsigned failures=0;Clock::time_point blocked{},touched=Clock::now();};
    std::mutex sessionsMutex,attemptMutex;
    std::unordered_map<std::string,Session> sessions;
    std::unordered_map<std::string,Attempt> attempts;
    std::atomic_flag authBusy=ATOMIC_FLAG_INIT;
    std::string dummy;
    Impl(rocksdb::DB& value,SessionConfig settings):db(value),config(settings) {
        requireEnrolledStore(db);
        if(!config.idle_seconds || !config.absolute_seconds || config.absolute_seconds>28800 ||
            config.idle_seconds>config.absolute_seconds || !config.max_sessions || config.max_sessions>4096)
            throw std::invalid_argument("Invalid native session policy");
        std::string bytes;storage(db.Get({},"sec:state:v1",&bytes));state=Json::parse(bytes);
        if(!state.contains("users")) state["users"]=Json::object();
        if(!state.contains("tokens")) state["tokens"]=Json::object();
        validateRoleGraph(state.at("roles"));
        std::unordered_set<std::string> names{state.at("root").at("username").get<std::string>()};
        if(!state.at("users").is_object() || state.at("users").size()>1024 ||
            !state.at("tokens").is_object() || state.at("tokens").size()>4096)
            throw std::runtime_error("Invalid IAM identity registry");
        for(auto it=state.at("users").begin();it!=state.at("users").end();++it) {
            const auto& user=it.value();const auto name=user.at("username").get<std::string>();username(name);
            if(user.at("id")!=it.key() || !nameValid(it.key()) || user.at("protected_root")!=false ||
                !names.insert(name).second || !user.at("credential_generation").is_number_unsigned() ||
                user.at("credential_generation").get<uint64_t>()<1 ||
                (user.at("status")!="active" && user.at("status")!="disabled"))
                throw std::runtime_error("Invalid IAM user record");
            grantsValid(user.at("grants"));
            for(const auto& role:user.at("roles")) if(!state.at("roles").contains(role.get<std::string>()))
                throw std::runtime_error("Unknown IAM role assignment");
            if(crypto_pwhash_str_needs_rehash(user.at("password_hash").get<std::string>().c_str(),
                crypto_pwhash_OPSLIMIT_INTERACTIVE,crypto_pwhash_MEMLIMIT_INTERACTIVE)!=0)
                throw std::runtime_error("Invalid IAM credential profile");
        }
        dummy=hashPassword(randomSecret());
    }
    void healthy() const {if(uncertain) throw std::runtime_error("IAM commit outcome uncertain; restart in maintenance mode");}
    // Called under policy lock. Token claims are always reconstructed here;
    // the client never supplies authoritative subjects, roles or permissions.
    Json resolve(const std::string& token,bool touch=true) {
        healthy();
        if(token.size()>128) throw AuthenticationError("Session is invalid or expired");
        const auto key=digest(token);
        if(token.starts_with("nxservice_")) {
            if(!state.at("tokens").contains(key)) throw AuthenticationError("Session is invalid or expired");
            auto record=state.at("tokens").at(key);
            const auto& user=userById(state,record.at("principal_id"));
            if(user.at("status")!="active" || user.at("credential_generation")!=record.at("generation") ||
                record.at("expires_at").get<int64_t>()<=nowMs()) throw AuthenticationError("Session is invalid or expired");
            record["user"]=user;record["service"]=true;return record;
        }
        std::lock_guard guard(sessionsMutex);
        const auto found=sessions.find(key);
        if(!token.starts_with("nxsession_") || found==sessions.end()) throw AuthenticationError("Session is invalid or expired");
        auto& session=found->second;const auto now=Clock::now();
        const auto& user=userById(state,session.user);
        if(now-session.created>=std::chrono::seconds(config.absolute_seconds) ||
            now-session.last>=std::chrono::seconds(config.idle_seconds) ||
            user.at("status")!="active" || user.at("credential_generation")!=session.generation ||
            user.value("must_change_password",false)) {
            sessions.erase(found);throw AuthenticationError("Session is invalid or expired");
        }
        if(touch) session.last=now;
        return {{"user",user},{"service",false},{"expires_at",session.expires},{"principal_id",session.user}};
    }
    void requireAdmin(const Json& identity) {
        if(identity.at("service")==true || !super(identity.at("user"))) throw AuthorizationError("Superuser session required");
    }
    void save(Json next,const std::string& principal,const std::string& action,const std::string& target,
        const std::string& outcome="success") {
        healthy();const auto sequence=next.value("audit_sequence",uint64_t(0))+1;
        const auto previous=next.value("audit_head",digest(next.at("audit").dump()));
        Json event={{"sequence",sequence},{"event_id",randomSecret()},{"timestamp",nowMs()},
            {"principal_id",principal},{"action",action},{"target",target},{"outcome",outcome},{"previous",previous},
            {"policy_epoch",next.at("policy_epoch")}};
        const auto hash=digest(event.dump());event["digest"]=hash;
        next["audit_sequence"]=sequence;next["audit_head"]=hash;
        std::ostringstream key;key<<"sec:audit:"<<std::setw(20)<<std::setfill('0')<<sequence;
        rocksdb::WriteBatch batch;batch.Put("sec:state:v1",next.dump());batch.Put(key.str(),event.dump());
        rocksdb::WriteOptions options;options.sync=true;options.disableWAL=false;
        uncertain=true;storage(db.Write(options,&batch));state=std::move(next);uncertain=false;
    }
    void failedLogin(const std::string& key) {
        std::lock_guard guard(attemptMutex);
        if(attempts.size()>=4096 && !attempts.contains(key)) throw RateLimitError("Authentication throttled");
        auto& attempt=attempts[key];attempt.touched=Clock::now();
        if(++attempt.failures>=5) attempt.blocked=Clock::now()+std::chrono::seconds(60);
    }
    void checkAttempts(const std::string& key) {
        std::lock_guard guard(attemptMutex);const auto now=Clock::now();
        for(auto it=attempts.begin();it!=attempts.end();) {
            if(now-it->second.touched>std::chrono::minutes(10)) it=attempts.erase(it);else ++it;
        }
        if(attempts.contains(key) && attempts.at(key).blocked>now) throw RateLimitError("Authentication throttled; retry later");
    }
};
SessionAuthority::SessionAuthority(rocksdb::DB& db,SessionConfig config):impl_(std::make_shared<Impl>(db,config)) {}
SessionAuthority::~SessionAuthority()=default;
bool SessionAuthority::commitUncertain() const noexcept {return impl_->uncertain.load();}

std::string SessionAuthority::login(const std::string& name,const std::string& password,const std::string& source) {
    auto& p=*impl_;AuthAdmission admission(p.authBusy);
    if(name.size()>64 || password.size()>1024 || source.size()>128) throw AuthenticationError("Invalid username or password");
    const auto accountKey="account:"+digest(name),sourceKey="source:"+digest(source);
    p.checkAttempts(accountKey);p.checkAttempts(sourceKey);
    std::string id,verifier;uint64_t generation=0;
    {PolicyRead lock(p.policy,p.waitingWriters);p.healthy();id=findUser(p.state,name);
        if(!id.empty()) {const auto& user=userById(p.state,id);verifier=user.at("password_hash");generation=user.at("credential_generation");}
        else verifier=p.dummy;}
    const bool verified=verifyPassword(verifier,password);
    PolicyWrite lock(p.policy,p.waitingWriters);p.healthy();
    const bool valid=verified && !id.empty() && userById(p.state,id).at("status")=="active" &&
        userById(p.state,id).at("credential_generation")==generation &&
        !userById(p.state,id).value("must_change_password",false);
    if(!valid) {
        p.failedLogin(accountKey);p.failedLogin(sourceKey);
        p.save(p.state,id,"authentication.login",name,"failure");
        throw AuthenticationError("Invalid username or password");
    }
    auto next=p.state;mutableUser(next,id)["last_login_at"]=nowMs();
    p.save(std::move(next),id,"authentication.login",id);
    const auto token="nxsession_"+randomSecret();
    {std::lock_guard guard(p.sessionsMutex);const auto now=Clock::now();
        for(auto it=p.sessions.begin();it!=p.sessions.end();) {
            if(now-it->second.created>=std::chrono::seconds(p.config.absolute_seconds) ||
                now-it->second.last>=std::chrono::seconds(p.config.idle_seconds)) it=p.sessions.erase(it);else ++it;
        }
        if(p.sessions.size()>=p.config.max_sessions) throw RateLimitError("Session capacity exceeded");
        p.sessions.emplace(digest(token),Impl::Session{id,generation,now,now,nowMs()+1000LL*p.config.absolute_seconds});
    }
    {std::lock_guard guard(p.attemptMutex);p.attempts.erase(accountKey);}
    return token;
}
std::string SessionAuthority::claims(const std::string& token) {
    auto& p=*impl_;PolicyRead lock(p.policy,p.waitingWriters);const auto identity=p.resolve(token);
    const auto& user=identity.at("user");
    Json grants=Json::object();
    // Adapter-side checks are hints only; every data call independently checks
    // the native authority. Enumerate the effective native collection grants.
    std::unordered_set<std::string> resources;
    const auto direct=user.value("grants",Json::object());
    for(auto it=direct.begin();it!=direct.end();++it) resources.insert(it.key());
    for(auto it=p.state.at("roles").begin();it!=p.state.at("roles").end();++it)
        if(it.key()!="superuser") for(auto grant=it.value().at("grants").begin();grant!=it.value().at("grants").end();++grant) resources.insert(grant.key());
    const bool service=identity.at("service");
    if(service) resources.clear();
    if(service) for(auto it=identity.at("collections").begin();it!=identity.at("collections").end();++it) resources.insert(it.key());
    for(const auto& resource:resources) for(const auto* action:{"read","write","manage"})
        if(allows(p.state,user,resource,action) && (!service || hasGrant(identity.at("collections"),resource,action))) {
            if(!grants.contains(resource)) grants[resource]=Json::array();
            grants[resource].push_back(action);
        }
    Json scopes=service?identity.at("scopes"):Json::array({"query:execute","collections:read","collections:write",
        "documents:read","documents:write","graphs:read","graphs:write","monitoring:read"});
    return Json({{"sub",user.at("id")},{"username",user.at("username")},{"role",super(user)?"admin":"application"},
        {"exp",identity.at("expires_at").get<int64_t>()/1000},{"user",publicUser(user)},
        {"service",service},{"scopes",scopes},{"collections",!service && super(user)?Json():grants},
        {"app_id",service?identity.at("app_id"):user.at("id")},
        {"app_name",service?identity.at("app_name"):user.at("username")},
        {"token_id",service?identity.at("id"):Json(digest(token))},{"policy_epoch",p.state.at("policy_epoch")}}).dump();
}
AccessLease SessionAuthority::authorize(const std::string& token,const std::string& action,const std::string& resource,const std::string& scope) {
    return authorizeMany(token,{{action,resource}},scope);
}
AccessLease SessionAuthority::authorizeMany(const std::string& token,
    const std::vector<std::pair<std::string,std::string>>& resources,const std::string& scope) {
    AccessLease lease(std::make_unique<AccessLease::Impl>(impl_,impl_->policy,impl_->waitingWriters));
    const auto identity=impl_->resolve(token);const auto& user=identity.at("user");
    lease.principal_id=user.at("id");lease.superuser=super(user) && identity.at("service")==false;
    lease.policy_epoch=impl_->state.at("policy_epoch");
    if(identity.at("service")==true && !scope.empty()) {
        const auto& scopes=identity.at("scopes");
        if(std::find(scopes.begin(),scopes.end(),scope)==scopes.end()) throw AuthorizationError("Service credential is missing the required scope");
    }
    for(const auto& [action,resource]:resources) {
    if(action=="session") continue;
    if(resource.empty()) {
        if(!lease.superuser) throw AuthorizationError("Instance administration requires a superuser session");
        continue;
    }
    collection(resource);
    if(action!="read" && action!="write" && action!="manage") throw AuthorizationError("Unsupported protected action");
    if(!allows(impl_->state,user,resource,action) ||
        (identity.at("service")==true && !hasGrant(identity.at("collections"),resource,action)))
        throw AuthorizationError("Permission denied for collection operation");
    }
    return lease;
}
std::vector<std::string> SessionAuthority::visibleCollections(const std::string& token,const std::vector<std::string>& values) {
    auto& p=*impl_;PolicyRead lock(p.policy,p.waitingWriters);const auto identity=p.resolve(token);std::vector<std::string> result;
    for(const auto& resource:values) {
        if(resource.starts_with("__nexora_")) continue;
        for(const auto* action:{"read","write","manage"})
            if(allows(p.state,identity.at("user"),resource,action) &&
                (identity.at("service")==false || hasGrant(identity.at("collections"),resource,action))) {result.push_back(resource);break;}
    }
    return result;
}
void SessionAuthority::logout(const std::string& token) {
    auto& p=*impl_;PolicyWrite lock(p.policy,p.waitingWriters);const auto identity=p.resolve(token);
    if(identity.at("service")==true) throw AuthorizationError("Revoke service credentials through administration");
    p.save(p.state,identity.at("user").at("id"),"authentication.logout",identity.at("user").at("id"));
    std::lock_guard guard(p.sessionsMutex);p.sessions.erase(digest(token));
}
void SessionAuthority::changePassword(const std::string& token,const std::string& current,const std::string& replacement) {
    auto& p=*impl_;AuthAdmission admission(p.authBusy);
    std::string id,verifier;uint64_t generation;
    {PolicyRead lock(p.policy,p.waitingWriters);const auto identity=p.resolve(token);
        if(identity.at("service")==true) throw AuthorizationError("Interactive session required");
        const auto& user=identity.at("user");id=user.at("id");verifier=user.at("password_hash");generation=user.at("credential_generation");}
    if(!verifyPassword(verifier,current)) throw AuthenticationError("Current password is invalid");
    const auto hash=hashPassword(replacement);
    PolicyWrite lock(p.policy,p.waitingWriters);const auto identity=p.resolve(token);
    if(identity.at("user").at("credential_generation")!=generation) throw AuthenticationError("Credentials changed; sign in again");
    auto next=p.state;auto& user=mutableUser(next,id);user["password_hash"]=hash;user["updated_at"]=nowMs();
    user["credential_generation"]=generation+1;user.erase("reset");next["policy_epoch"]=next.at("policy_epoch").get<uint64_t>()+1;
    p.save(std::move(next),id,"credential.change",id);
}
void SessionAuthority::completeReset(const std::string& token,const std::string& replacement) {
    auto& p=*impl_;AuthAdmission admission(p.authBusy);
    if(!token.starts_with("nxreset_") || token.size()!=72) throw AuthenticationError("Invalid or expired reset token");
    const auto key=digest(token);std::string id;uint64_t generation=0;
    {PolicyRead lock(p.policy,p.waitingWriters);p.healthy();for(auto it=p.state.at("users").begin();it!=p.state.at("users").end();++it) {
        const auto& user=it.value();if(user.contains("reset") && secretEqual(user.at("reset").at("digest"),key) &&
            user.at("reset").at("expires_at").get<int64_t>()>nowMs() && user.at("status")=="active") {
            id=it.key();generation=user.at("credential_generation");break;
        }
    }}
    if(id.empty()) throw AuthenticationError("Invalid or expired reset token");
    const auto hash=hashPassword(replacement);
    PolicyWrite lock(p.policy,p.waitingWriters);p.healthy();const auto& existing=userById(p.state,id);
    if(existing.at("status")!="active" || existing.at("credential_generation")!=generation ||
        !existing.contains("reset") || !secretEqual(existing.at("reset").at("digest"),key) ||
        existing.at("reset").at("expires_at").get<int64_t>()<=nowMs()) throw AuthenticationError("Invalid or expired reset token");
    auto next=p.state;auto& user=mutableUser(next,id);user["password_hash"]=hash;user["must_change_password"]=false;
    user["credential_generation"]=generation+1;user["updated_at"]=nowMs();user.erase("reset");
    next["policy_epoch"]=next.at("policy_epoch").get<uint64_t>()+1;
    p.save(std::move(next),id,"credential.reset.complete",id);
}

std::string SessionAuthority::administer(const std::string& token,const std::string& operation,const std::string& request) {
    auto& p=*impl_;
    if(request.size()>32768) throw std::invalid_argument("Security request exceeds 32KiB");
    {PolicyRead lock(p.policy,p.waitingWriters);p.requireAdmin(p.resolve(token));}
    Json args=Json::parse(request);
    // Password hashing takes place outside the policy write lock. Admission is
    // bounded and permissions are checked again before durable mutation.
    std::string newHash;
    std::string recoveryCredential;
    if(operation=="create_user") {
        {PolicyRead lock(p.policy,p.waitingWriters);p.requireAdmin(p.resolve(token));}
        AuthAdmission admission(p.authBusy);newHash=hashPassword(args.at("password").get<std::string>());
    }
    if(operation=="rotate_recovery") {
        std::string verifier;
        {PolicyRead lock(p.policy,p.waitingWriters);const auto identity=p.resolve(token);p.requireAdmin(identity);
            if(identity.at("user").at("id")!=p.state.at("root_id")) throw AuthorizationError("Protected owner session required");
            verifier=identity.at("user").at("password_hash");}
        AuthAdmission admission(p.authBusy);
        if(!verifyPassword(verifier,args.at("currentPassword").get<std::string>())) throw AuthenticationError("Current password is invalid");
        recoveryCredential=verifier;
    }
    PolicyWrite lock(p.policy,p.waitingWriters);const auto identity=p.resolve(token);p.requireAdmin(identity);
    const std::string actor=identity.at("user").at("id");
    if(operation=="list_users") {
        exactFields(args,{});Json result=Json::array({publicUser(p.state.at("root"))});
        for(const auto& user:p.state.at("users")) result.push_back(publicUser(user));
        return result.dump();
    }
    if(operation=="list_roles") {exactFields(args,{});return p.state.at("roles").dump();}
    if(operation=="audit") {
        exactFields(args,{});Json events=Json::array({p.state.at("audit")});
        rocksdb::ReadOptions options;options.total_order_seek=true;options.verify_checksums=true;
        std::unique_ptr<rocksdb::Iterator> it(p.db.NewIterator(options));
        for(it->SeekForPrev("sec:audit;" );it->Valid() && it->key().starts_with("sec:audit:") && events.size()<=1000;it->Prev())
            events.push_back(Json::parse(it->value().ToString()));
        storage(it->status());return events.dump();
    }
    if(operation=="list_tokens") {
        exactFields(args,{});Json result=Json::array();for(const auto& record:p.state.at("tokens"))
            result.push_back({{"id",record.at("id")},{"tokenId",record.at("id")},{"appId",record.at("app_id")},
                {"appName",record.at("app_name")},{"createdAt",record.at("created_at").get<int64_t>()/1000},
                {"createdBy",record.at("principal_id")},{"status",record.at("expires_at").get<int64_t>()<=nowMs()?"expired":"active"},
                {"expiresAt",record.at("expires_at").get<int64_t>()/1000},{"scopes",record.at("scopes")},
                {"collections",record.at("collections")},{"token",""},{"tokenType","bearer"}});
        return result.dump();
    }
    auto next=p.state;Json response={{"message","ok"}};std::string target;
    if(operation=="rotate_recovery") {
        exactFields(args,{"currentPassword"});
        if(actor!=next.at("root_id") || next.at("root").at("password_hash")!=recoveryCredential)
            throw AuthenticationError("Owner credentials changed; sign in again");
        Json codes=Json::array(),records=Json::array();
        for(size_t i=0;i<8;++i) {
            auto code=randomSecret();auto input=std::string("nexoradb-recovery-v1:")+next.at("domain_id").get<std::string>()+":"+code;
            records.push_back({{"digest",digest(input)},{"used",false}});wipe(input);codes.push_back(std::move(code));
        }
        next["recovery"]=std::move(records);response={{"recoveryCodes",std::move(codes)}};target=actor;
    } else if(operation=="create_user") {
        exactFields(args,{"username","password","roles","grants"});const auto name=args.at("username").get<std::string>();username(name);
        const auto roles=args.value("roles",Json::array());const auto grants=args.value("grants",Json::object());
        if(!roles.is_array() || roles.size()>16) throw std::invalid_argument("Invalid role assignments");
        for(const auto& role:roles) {
            if(!next.at("roles").contains(role.get<std::string>())) throw std::invalid_argument("Unknown role");
            if(role=="superuser" && actor!=next.at("root_id")) throw AuthorizationError("Only the protected owner can grant superuser");
        }
        grantsValid(grants);
        if(!findUser(next,name).empty()) throw std::invalid_argument("Username already exists");
        if(next.at("users").size()>=1024) throw std::invalid_argument("Identity capacity exceeded");
        target=randomSecret();const auto timestamp=nowMs();
        next["users"][target]={{"id",target},{"username",name},{"protected_root",false},{"status","active"},
            {"roles",roles},{"grants",grants},{"credential_generation",uint64_t(1)},
            {"password_hash",newHash},{"created_at",timestamp},{"updated_at",timestamp},{"must_change_password",false}};
        response=publicUser(next.at("users").at(target));
    } else if(operation=="set_user" || operation=="issue_reset") {
        exactFields(args,operation=="set_user"?std::initializer_list<const char*>{"id","username","status","roles","grants"}:
            std::initializer_list<const char*>{"id"});
        target=args.at("id");auto& user=mutableUser(next,target);
        const bool root=next.at("root_id")==target;
        if(operation=="issue_reset") {
            if(root) throw AuthorizationError("Protected owner recovery is offline only");
            if(user.at("status")!="active") throw std::invalid_argument("Enable the account before issuing a reset");
            const auto reset="nxreset_"+randomSecret();const auto expires=nowMs()+15*60*1000;
            user["reset"]={{"digest",digest(reset)},{"expires_at",expires}};user["must_change_password"]=true;
            user["credential_generation"]=user.at("credential_generation").get<uint64_t>()+1;
            response={{"resetToken",reset},{"expiresAt",expires},{"username",user.at("username")}};
        } else {
            if(args.contains("username")) {const auto name=args.at("username").get<std::string>();username(name);
                const auto found=findUser(next,name);if(!found.empty() && found!=target) throw std::invalid_argument("Username already exists");user["username"]=name;}
            if(args.contains("status")) {
                if(args.at("status")!="active" && args.at("status")!="disabled") throw std::invalid_argument("Invalid account status");
                if(root && args.at("status")!="active") throw AuthorizationError("Protected owner cannot be disabled");
                user["status"]=args.at("status");
            }
            if(args.contains("roles")) {
                if(!args.at("roles").is_array() || args.at("roles").size()>16) throw std::invalid_argument("Invalid role assignments");
                for(const auto& role:args.at("roles")) {
                    if(!next.at("roles").contains(role.get<std::string>())) throw std::invalid_argument("Unknown role");
                    if(role=="superuser" && actor!=next.at("root_id") && !super(user)) throw AuthorizationError("Only the protected owner can grant superuser");
                }
                user["roles"]=args.at("roles");if(root && !super(user)) throw AuthorizationError("Protected owner must retain superuser");
            }
            if(args.contains("grants")) {grantsValid(args.at("grants"));user["grants"]=args.at("grants");}
            user["credential_generation"]=user.at("credential_generation").get<uint64_t>()+1;
            response=publicUser(user);
        }
        user["updated_at"]=nowMs();
    } else if(operation=="set_role") {
        exactFields(args,{"name","parents","grants"});target=args.at("name");username(target);
        if(target=="superuser") throw AuthorizationError("Built-in superuser role is immutable");
        grantsValid(args.at("grants"));next["roles"][target]={{"grants",args.at("grants")},{"parents",args.value("parents",Json::array())}};
        validateRoleGraph(next.at("roles"));response=next.at("roles").at(target);
        // Stored cursor/idempotency results must not outlive a role revocation.
        // Invalidate ordinary-user sessions and their delegated credentials;
        // superuser effective privileges are unchanged by custom role edits.
        for(auto& user:next["users"]) if(!super(user))
            user["credential_generation"]=user.at("credential_generation").get<uint64_t>()+1;
    } else if(operation=="issue_token") {
        exactFields(args,{"appId","appName","scopes","collections","expiresInSeconds"});
        const auto app=args.at("appId").get<std::string>();username(app);
        const auto label=args.value("appName",app);if(label.size()>120) throw std::invalid_argument("Application label too long");
        grantsValid(args.at("collections"));const auto ttl=args.value("expiresInSeconds",int64_t(86400));
        if(ttl<300 || ttl>31536000 || next.at("tokens").size()>=4096) throw std::invalid_argument("Invalid service credential lifetime or capacity");
        const auto scopes=args.at("scopes");const auto known=Json::array({"query:execute","collections:read","collections:write","documents:read","documents:write"});
        if(!scopes.is_array() || scopes.size()>8) throw std::invalid_argument("Invalid service scopes");
        for(const auto& scope:scopes) if(std::find(known.begin(),known.end(),scope)==known.end()) throw std::invalid_argument("Invalid service scope");
        const auto secret="nxservice_"+randomSecret();target=randomSecret();const auto created=nowMs(),expires=created+ttl*1000;
        next["tokens"][digest(secret)]={{"id",target},{"principal_id",actor},{"generation",identity.at("user").at("credential_generation")},
            {"app_id",app},{"app_name",label},{"scopes",scopes},{"collections",args.at("collections")},{"created_at",created},{"expires_at",expires}};
        response={{"tokenId",target},{"id",target},{"token",secret},{"tokenType","bearer"},{"appId",app},{"appName",label},
            {"scopes",scopes},{"collections",args.at("collections")},{"createdAt",created/1000},{"expiresAt",expires/1000}};
    } else if(operation=="revoke_token") {
        exactFields(args,{"id"});target=args.at("id");bool found=false;
        for(auto it=next.at("tokens").begin();it!=next.at("tokens").end();++it)
            if(it.value().at("id")==target) {next["tokens"].erase(it.key());found=true;break;}
        if(!found) throw std::invalid_argument("Service credential not found");
    } else throw AuthorizationError("Unsupported security administration operation");
    next["policy_epoch"]=next.at("policy_epoch").get<uint64_t>()+1;
    p.save(std::move(next),actor,"security."+operation,target);
    return response.dump();
}
} // namespace nexora::security
