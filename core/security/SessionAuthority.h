#pragma once
#include "NativeIam.h"
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

namespace nexora::security {
class AuthenticationError : public std::runtime_error { using std::runtime_error::runtime_error; };
class AuthorizationError : public std::runtime_error { using std::runtime_error::runtime_error; };
class RateLimitError : public std::runtime_error { using std::runtime_error::runtime_error; };

struct SessionConfig {
    unsigned idle_seconds = 900;
    unsigned absolute_seconds = 28800;
    std::size_t max_sessions = 4096;
};

// Contains only a policy read lease, never a raw engine/DB reference. Security
// mutations wait for admitted operations to finish; subsequent operations must
// validate the current credential generation and current policy.
class AccessLease final {
public:
    ~AccessLease();
    AccessLease(AccessLease&&) noexcept;
    AccessLease& operator=(AccessLease&&) noexcept;
    std::string principal_id;
    bool superuser = false;
    uint64_t policy_epoch = 0;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit AccessLease(std::unique_ptr<Impl>);
    friend class SessionAuthority;
};

class SessionAuthority final {
public:
    explicit SessionAuthority(rocksdb::DB&, SessionConfig config = {});
    ~SessionAuthority();
    bool commitUncertain() const noexcept;
    std::string login(const std::string& username, const std::string& password,
        const std::string& source);
    std::string claims(const std::string& token);
    void logout(const std::string& token);
    void changePassword(const std::string& token, const std::string& current,
        const std::string& replacement);
    void completeReset(const std::string& token, const std::string& replacement);
    std::string administer(const std::string& token, const std::string& operation,
        const std::string& request);
    AccessLease authorize(const std::string& token, const std::string& action,
        const std::string& collection = "", const std::string& scope = "");
    AccessLease authorizeMany(const std::string& token,
        const std::vector<std::pair<std::string,std::string>>& resources, const std::string& scope);
    std::vector<std::string> visibleCollections(const std::string& token,
        const std::vector<std::string>& collections);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace nexora::security
