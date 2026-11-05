#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rocksdb { class DB; class Env; }

namespace nexora::security {

// Enrollment, adoption and emergency recovery are deliberately offline-only.
// Opening this store takes the
// RocksDB process lock. No database handle or credential verifier is exported.
struct BootstrapReceipt {
    std::string domain_id;
    std::string root_id;
    std::vector<std::string> recovery_codes; // Display once; never persisted.
};

struct BootstrapStatus {
    bool initialized = false;
    std::string domain_id;
    std::string root_id;
    std::string username;
};

class OfflineIdentityStore final {
public:
    // Non-owning Env injection is for native storage fault tests only; the
    // default OS environment is used by the CLI. It must outlive this store.
    explicit OfflineIdentityStore(const std::string& path, rocksdb::Env* environment_for_testing = nullptr);
    ~OfflineIdentityStore();
    OfflineIdentityStore(const OfflineIdentityStore&) = delete;
    OfflineIdentityStore& operator=(const OfflineIdentityStore&) = delete;

    BootstrapStatus status() const;
    BootstrapReceipt initialize(std::string_view username, std::string_view password);
    void recoverRoot(std::string_view recovery_code, std::string_view new_password);
    BootstrapReceipt adoptExisting(std::string_view username, std::string_view password,
        std::string_view confirmation, std::string_view previous_owner_password = "");

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BootstrapReceipt initializeImpl(std::string_view,std::string_view,bool,std::string_view);
};

// Called before legacy engine initialization, migrations or background work.
// Enrolled stores are inaccessible through legacy C++/pybind11 construction;
// authenticated online access must use SecureDatabase.
void requireLegacyAccessAllowed(rocksdb::DB& db);
void requireEnrolledStore(rocksdb::DB& db);

// Native Argon2id implementation; verifier checks are bounded to the exact
// versioned profile emitted by this release. Not exposed through pybind11.
std::string hashPassword(std::string_view password);
bool verifyPassword(std::string_view verifier, std::string_view password);
void wipe(std::string& secret) noexcept;

} // namespace nexora::security
