#ifndef NEXORADB_COMPILED_UPDATE_H
#define NEXORADB_COMPILED_UPDATE_H

#pragma once

#include "DocumentView.h"
#include "UpdateSpec.h"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace nexora::query {

class CompiledUpdate final {
public:
    struct Result {
        bool success = false;
        std::string document;
        std::string error;
    };

    explicit CompiledUpdate(const UpdateSpec& spec);
    ~CompiledUpdate();

    [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] Result ApplyJson(std::string_view json_document) const;

private:
    struct Operation;

    std::vector<Operation> operations_;
    std::string error_;
};

} // namespace nexora::query

#endif // NEXORADB_COMPILED_UPDATE_H
