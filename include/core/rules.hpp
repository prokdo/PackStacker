#pragma once

#include "core/model.hpp"

#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace PackStacker
{
    std::string_view StrategyName(MergeStrategy strategy);
    std::optional<MergeStrategy> ParseStrategyName(std::string_view name);
    Result<void> ValidateRules(std::span<const MergeRule> rules, const Limits& limits = {});
    Result<std::vector<MergeRule>> ParseRules(std::string_view bytes, const Limits& limits = {});
    Result<std::string> SerializeRules(std::span<const MergeRule> rules, const Limits& limits = {});
    Result<std::vector<MergeRule>> LoadRules(std::string_view path, const Limits& limits = {});
    Result<void> SaveRules(std::string_view path, std::span<const MergeRule> rules,
                           const Limits& limits = {});
    Result<MergeStrategy> MatchRules(std::span<const MergeRule> rules, std::string_view path,
                                     std::uint64_t& budget, const std::stop_token& stop = {});
}
