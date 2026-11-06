#pragma once
#include "core/DocEngine.h"
#include "SessionAuthority.h"
#include <variant>

namespace nexora::security {
class GraphGateway;
class SecureTransaction;
struct DataRequest {
    std::string operation,collection,id,payload,page_token,target_collection,source_field,target_field;
    query::Condition condition;
    query::UpdateSpec update;
    std::optional<core::SchemaDefinition> schema;
    core::IndexDefinition index;
    core::ForeignKeyDefinition foreign_key;
    std::vector<std::string> documents;
    std::vector<std::array<std::string,3>> joins;
    uint32_t limit=0,skip=0;
    size_t max_bytes=core::pagination::kDefaultBytes;
    bool flag=false,second_flag=false;
    std::shared_ptr<SecureTransaction> transaction;
};
using DataReply=std::variant<core::DBResult,core::PageResult,bool,int64_t,
    std::vector<std::string>,std::optional<core::SchemaDefinition>,
    std::vector<core::IndexDefinition>,std::vector<core::ForeignKeyDefinition>,core::RocksMetrics,
    std::shared_ptr<SecureTransaction>>;

// The only online entry point for enrolled stores. Owns the engine and native
// authority; no API returns the raw DocEngine, RocksDB handle or maintenance key.
class SecureDatabase final : public std::enable_shared_from_this<SecureDatabase> {
public:
    explicit SecureDatabase(const std::string& path,const core::RocksSettings& settings={},SessionConfig sessions={});
    ~SecureDatabase();
    std::string login(const std::string& username,const std::string& password,const std::string& source);
    std::string claims(const std::string& token);
    void logout(const std::string& token);
    void changePassword(const std::string& token,const std::string& current,const std::string& replacement);
    void completeReset(const std::string& token,const std::string& replacement);
    std::string administer(const std::string& token,const std::string& operation,const std::string& request);
    DataReply execute(const std::string& token,const DataRequest& request);
    // Returns operational readiness only, never data, metrics or configuration.
    bool ready() const noexcept;
    core::DBResult prepareShutdown();
private:
    friend class GraphGateway;
    std::unique_ptr<core::DocEngine> engine_;
    std::unique_ptr<SessionAuthority> authority_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex projection_mutex_;
    std::function<void(const std::string&)> refresh_projection_;
    void refreshProjection(const std::string& collection);
};
class SecureTransaction final {
public:
    ~SecureTransaction();
    bool isValid() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit SecureTransaction(std::unique_ptr<Impl> value);
    friend class SecureDatabase;
};
} // namespace nexora::security
