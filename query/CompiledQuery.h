#ifndef NEXORADB_COMPILED_QUERY_H
#define NEXORADB_COMPILED_QUERY_H

#pragma once

#include "Condition.h"
#include "DocumentView.h"

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace nexora::query {

class CompiledQuery final {
public:
    explicit CompiledQuery(const Condition& condition);

    [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] bool Match(const DocumentView& document) const;

private:
    struct TransparentStringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        std::size_t operator()(const std::string& value) const noexcept {
            return (*this)(std::string_view(value));
        }
    };

    using Constant = std::variant<std::monostate, std::string,
                                  std::int64_t, double, bool>;

    struct Node {
        bool empty = false;
        bool leaf = false;
        LogicOp logic = LogicOp::AND;
        Op op = Op::EQ;
        ValueType value_type = ValueType::String;
        CompiledFieldPath path{"invalid"};
        Constant constant;
        std::vector<Constant> list;
        std::unordered_set<std::string, TransparentStringHash,
                           std::equal_to<>> string_set;
        std::unordered_set<std::int64_t> integer_set;
        std::unordered_set<double> floating_set;
        std::optional<std::regex> regex;
        std::vector<Node> children;
        std::uint8_t cost = 1;
        std::uint8_t estimated_selectivity = 50;
    };

    Node Compile(const Condition& condition, std::size_t depth = 0);
    static Constant ParseConstant(std::string_view value, ValueType type,
                                  bool& valid);
    static bool MatchNode(const Node& node,
                          const DocumentView& document);
    static bool MatchLeaf(const Node& node,
                          const DocumentValueView& value);
    static bool Equal(const DocumentValueView& value,
                      const Constant& constant) noexcept;

    Node root_;
    std::string error_;
    std::size_t compiled_nodes_ = 0;
};

} // namespace nexora::query

#endif // NEXORADB_COMPILED_QUERY_H
