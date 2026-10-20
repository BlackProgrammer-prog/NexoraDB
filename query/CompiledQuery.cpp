#include "CompiledQuery.h"
#include "NumericCompare.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace nexora::query {
namespace {

template <typename Number>
bool ParseNumber(std::string_view text, Number& output) noexcept {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, output);
    return result.ec == std::errc{} && result.ptr == end;
}

} // namespace

CompiledQuery::CompiledQuery(const Condition& condition) {
    root_ = Compile(condition);
}

CompiledQuery::Constant CompiledQuery::ParseConstant(
        std::string_view value, ValueType type, bool& valid) {
    valid = true;
    switch (type) {
        case ValueType::String:
            return std::string(value);
        case ValueType::Int64: {
            std::int64_t parsed = 0;
            valid = ParseNumber(value, parsed);
            return parsed;
        }
        case ValueType::Float64: {
            double parsed = 0.0;
            valid = ParseNumber(value, parsed) && std::isfinite(parsed);
            return parsed;
        }
        case ValueType::Bool:
            if (value == "1" || value == "true") return true;
            if (value == "0" || value == "false") return false;
            valid = false;
            return false;
        case ValueType::Null:
            return std::monostate{};
    }
    valid = false;
    return std::monostate{};
}

CompiledQuery::Node CompiledQuery::Compile(const Condition& condition, std::size_t depth) {
    Node node;
    if (depth > 64 || ++compiled_nodes_ > 4096) {
        error_ = "Query complexity limit exceeded";
        return node;
    }
    if (condition.IsEmpty()) {
        node.empty = true;
        return node;
    }
    if (condition.IsComposite()) {
        node.logic = condition.logic;
        node.children.reserve(condition.sub_conditions.size());
        for (const auto& child : condition.sub_conditions) {
            node.children.push_back(Compile(child, depth + 1));
            if (!error_.empty()) return node;
        }
        if (node.logic == LogicOp::AND) {
            std::stable_sort(node.children.begin(), node.children.end(),
                             [](const Node& left, const Node& right) {
                                 const auto left_score =
                                         left.estimated_selectivity + left.cost * 4;
                                 const auto right_score =
                                         right.estimated_selectivity + right.cost * 4;
                                 return left_score < right_score;
                             });
        }
        return node;
    }

    node.leaf = true;
    node.op = condition.op;
    node.value_type = condition.value_type;
    node.path = CompiledFieldPath(condition.field);
    if (!node.path.valid()) {
        error_ = node.path.error();
        return node;
    }

    bool constant_valid = true;
    node.constant = ParseConstant(
            condition.value, condition.value_type, constant_valid);
    if (!constant_valid && condition.op != Op::IN &&
        condition.op != Op::NIN) {
        error_ = "invalid typed query constant for field '" +
                 condition.field + "'";
    }

    if (condition.op == Op::IN || condition.op == Op::NIN) {
        node.cost = 2;
        node.estimated_selectivity = 20;
        std::vector<std::string> legacy_values;
        const std::vector<std::string>* values = &condition.values;
        if (values->empty() && !condition.value.empty()) {
            std::size_t start = 0;
            while (start < condition.value.size()) {
                const std::size_t end = condition.value.find('\0', start);
                legacy_values.emplace_back(condition.value.substr(
                        start, end == std::string::npos
                                ? std::string::npos : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
            }
            values = &legacy_values;
        }
        node.list.reserve(values->size());
        for (std::size_t index = 0; index < values->size(); ++index) {
            const auto& item = (*values)[index];
            const ValueType item_type =
                    condition.value_types.size() == values->size()
                    ? condition.value_types[index]
                    : condition.value_type;
            bool item_valid = true;
            auto parsed = ParseConstant(item, item_type, item_valid);
            if (!item_valid) {
                error_ = "invalid typed IN constant for field '" +
                         condition.field + "'";
                break;
            }
            if (const auto* string = std::get_if<std::string>(&parsed))
                node.string_set.insert(*string);
            else if (const auto* integer = std::get_if<std::int64_t>(&parsed))
                node.integer_set.insert(*integer);
            else if (const auto* floating = std::get_if<double>(&parsed))
                node.floating_set.insert(*floating);
            node.list.push_back(std::move(parsed));
        }
    } else if (condition.op == Op::REGEX) {
        node.cost = 8;
        node.estimated_selectivity = 50;
        try {
            node.regex.emplace(condition.value,
                               std::regex::ECMAScript |
                               std::regex::optimize);
        } catch (const std::regex_error& exception) {
            error_ = "invalid regex: " + std::string(exception.what());
        }
    } else if (condition.op == Op::CONTAINS) {
        node.cost = 5;
        node.estimated_selectivity = 55;
    } else if (condition.op == Op::STARTS) {
        node.cost = 3;
        node.estimated_selectivity = 35;
    } else if (condition.op == Op::EXISTS) {
        node.cost = 0;
        node.estimated_selectivity = 90;
    } else if (condition.op == Op::EQ) {
        node.estimated_selectivity = 10;
    } else if (condition.op == Op::NEQ) {
        node.estimated_selectivity = 80;
    } else {
        node.estimated_selectivity = 30;
    }
    return node;
}

bool CompiledQuery::Equal(const DocumentValueView& value,
                          const Constant& constant) noexcept {
    if (std::holds_alternative<std::monostate>(constant))
        return value.type == DocumentValueType::Null;
    if (const auto* string = std::get_if<std::string>(&constant))
        return value.type == DocumentValueType::String &&
               value.string_value == *string;
    if (const auto* boolean = std::get_if<bool>(&constant))
        return value.type == DocumentValueType::Bool &&
               value.bool_value == *boolean;
    if (std::holds_alternative<std::int64_t>(constant) ||
        std::holds_alternative<double>(constant)) {
        if (!value.is_number()) return false;
        if (const auto* integer = std::get_if<std::int64_t>(&constant)) {
            if (value.type == DocumentValueType::Int64)
                return value.int_value == *integer;
            return std::isfinite(value.double_value) &&
                   CompareIntDouble(*integer, value.double_value) == 0;
        }
        if (value.type == DocumentValueType::Int64)
            return CompareIntDouble(value.int_value, std::get<double>(constant)) == 0;
        return value.double_value == std::get<double>(constant);
    }
    return false;
}

bool CompiledQuery::MatchLeaf(const Node& node,
                              const DocumentValueView& value) {
    if (node.op == Op::EXISTS) {
        const bool expected = std::get_if<bool>(&node.constant)
                ? std::get<bool>(node.constant) : false;
        return value.found() == expected;
    }
    if (!value.found()) return false;

    if (node.op == Op::IN || node.op == Op::NIN) {
        bool contained = false;
        if (value.type == DocumentValueType::String &&
            !node.string_set.empty()) {
            contained = node.string_set.contains(value.string_value);
        } else if (value.type == DocumentValueType::Int64 &&
                   (!node.integer_set.empty() || !node.floating_set.empty())) {
            contained = node.integer_set.contains(value.int_value);
            const double rounded = static_cast<double>(value.int_value);
            if (!contained && CompareIntDouble(value.int_value, rounded) == 0)
                contained = node.floating_set.contains(rounded);
        } else if (value.type == DocumentValueType::Float64 &&
                   (!node.floating_set.empty() || !node.integer_set.empty())) {
            contained = node.floating_set.contains(value.double_value);
            if (!contained && std::isfinite(value.double_value) &&
                std::trunc(value.double_value) == value.double_value &&
                value.double_value >= static_cast<double>(
                        std::numeric_limits<std::int64_t>::min()) &&
                value.double_value < static_cast<double>(
                        std::numeric_limits<std::int64_t>::max())) {
                contained = node.integer_set.contains(
                        static_cast<std::int64_t>(value.double_value));
            }
        } else {
            contained = std::any_of(
                    node.list.begin(), node.list.end(),
                    [&](const Constant& item) { return Equal(value, item); });
        }
        return node.op == Op::IN ? contained : !contained;
    }

    if (node.op == Op::REGEX) {
        return value.type == DocumentValueType::String && node.regex &&
               std::regex_search(value.string_value.begin(),
                                 value.string_value.end(), *node.regex);
    }
    if (node.op == Op::STARTS) {
        const auto* expected = std::get_if<std::string>(&node.constant);
        return value.type == DocumentValueType::String && expected &&
               value.string_value.starts_with(*expected);
    }
    if (node.op == Op::CONTAINS) {
        const auto* expected = std::get_if<std::string>(&node.constant);
        return value.type == DocumentValueType::String && expected &&
               value.string_value.find(*expected) != std::string_view::npos;
    }

    if (node.op == Op::EQ) return Equal(value, node.constant);
    if (node.op == Op::NEQ) return !Equal(value, node.constant);

    int ordering = 0;
    if (value.is_number() &&
        (std::holds_alternative<std::int64_t>(node.constant) ||
         std::holds_alternative<double>(node.constant))) {
        if (value.type == DocumentValueType::Int64) {
            if (const auto* right = std::get_if<std::int64_t>(&node.constant))
                ordering = value.int_value < *right ? -1 : (value.int_value > *right ? 1 : 0);
            else
                ordering = CompareIntDouble(value.int_value, std::get<double>(node.constant));
        } else {
            if (!std::isfinite(value.double_value)) return false;
            if (const auto* right = std::get_if<std::int64_t>(&node.constant))
                ordering = -CompareIntDouble(*right, value.double_value);
            else {
                const double right_float = std::get<double>(node.constant);
                ordering = value.double_value < right_float ? -1 :
                           (value.double_value > right_float ? 1 : 0);
            }
        }
    } else if (value.type == DocumentValueType::String) {
        const auto* right = std::get_if<std::string>(&node.constant);
        if (!right) return false;
        ordering = value.string_value < *right
                ? -1 : (value.string_value > *right ? 1 : 0);
    } else {
        return false;
    }

    switch (node.op) {
        case Op::GT: return ordering > 0;
        case Op::GTE: return ordering >= 0;
        case Op::LT: return ordering < 0;
        case Op::LTE: return ordering <= 0;
        default: return false;
    }
}

bool CompiledQuery::MatchNode(const Node& node,
                              const DocumentView& document) {
    if (node.empty) return true;
    if (node.leaf) return MatchLeaf(node, document.Get(node.path));
    switch (node.logic) {
        case LogicOp::AND:
            return std::all_of(node.children.begin(), node.children.end(),
                               [&](const Node& child) {
                                   return MatchNode(child, document);
                               });
        case LogicOp::OR:
            return std::any_of(node.children.begin(), node.children.end(),
                               [&](const Node& child) {
                                   return MatchNode(child, document);
                               });
        case LogicOp::NOR:
            return std::none_of(node.children.begin(), node.children.end(),
                                [&](const Node& child) {
                                    return MatchNode(child, document);
                                });
        case LogicOp::NOT:
            return node.children.empty() ||
                   !MatchNode(node.children.front(), document);
    }
    return false;
}

bool CompiledQuery::Match(const DocumentView& document) const {
    return valid() && document.valid() && MatchNode(root_, document);
}

} // namespace nexora::query
