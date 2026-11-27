#include "core/rules.hpp"

#include "core/filesystem.hpp"
#include "core/metadata.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/streams.hpp"
#include "core/unicode.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        constexpr auto Names = std::array{
            "standard", "high_priority", "low_priority", "language",
            "tag",      "font",          "atlas",        "sounds",
        };

        std::unexpected<Diagnostic> invalid(std::string message)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::INVALID_METADATA,
                .Message = std::move(message),
                .Pack = {},
                .Path = {},
            });
        }

        void advanceMatch(const std::vector<std::uint8_t>& previous,
                          std::vector<std::uint8_t>& current, std::string_view token,
                          std::string_view path)
        {
            const auto star = token.front() == '*';
            const auto directory = token == "**/";
            current.front() = star ? previous.front() : std::uint8_t{0};
            auto reachable = previous.front() != 0;
            for (auto index = std::size_t{1}; index < current.size(); ++index)
            {
                const auto character = path.at(index - 1);
                if (directory)
                {
                    current.at(index) = static_cast<std::uint8_t>(previous.at(index) != 0 ||
                                                                  (character == '/' && reachable));
                    reachable = reachable || previous.at(index) != 0;
                }
                else if (star)
                {
                    current.at(index) = static_cast<std::uint8_t>(
                        previous.at(index) != 0 ||
                        (current.at(index - 1) != 0 && (token == "**" || character != '/')));
                }
                else
                {
                    current.at(index) = static_cast<std::uint8_t>(previous.at(index - 1) != 0 &&
                                                                  character == token.front());
                }
            }
        }

        Result<bool> matches(std::string_view pattern, std::string_view path, std::uint64_t& budget,
                             const std::stop_token& stop)
        {
            auto previous = std::vector<std::uint8_t>(path.size() + 1, 0);
            auto current = std::vector<std::uint8_t>(previous.size(), 0);
            previous.front() = 1;
            while (!pattern.empty())
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::CANCELLED,
                        .Message = "Cancelled",
                        .Pack = {},
                        .Path = {},
                    });
                }
                if (previous.size() > budget)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::LIMIT_EXCEEDED,
                        .Message = "Merge rule match budget exceeded",
                        .Pack = {},
                        .Path = std::string(path),
                    });
                }
                budget -= previous.size();
                auto length = std::size_t{1};
                if (pattern.starts_with("**"))
                {
                    length = 2;
                    if (pattern.starts_with("**/"))
                    {
                        length = 3;
                    }
                }
                advanceMatch(previous, current, pattern.substr(0, length), path);
                previous.swap(current);
                pattern.remove_prefix(length);
            }
            return previous.back() != 0;
        }
    }

    std::string_view StrategyName(MergeStrategy strategy)
    {
        const auto index = static_cast<std::size_t>(strategy);
        return index < Names.size() ? Names.at(index) : std::string_view{};
    }

    std::optional<MergeStrategy> ParseStrategyName(std::string_view name)
    {
        const auto* const found = std::ranges::find(Names, name);
        if (found == Names.end())
        {
            return std::nullopt;
        }
        return static_cast<MergeStrategy>(found - Names.begin());
    }

    Result<void> ValidateRules(std::span<const MergeRule> rules, const Limits& limits)
    {
        if (rules.size() > limits.MaxFilterBlocks)
        {
            return invalid("Too many merge rules");
        }
        for (const auto& rule : rules)
        {
            if (rule.Match.empty() || rule.Match.size() > limits.MaxFilterPatternBytes ||
                StrategyName(rule.Strategy).empty())
            {
                return invalid("Invalid merge rule");
            }
            auto path = rule.Match;
            std::ranges::replace(path, '*', 'x');
            if (!ValidatePortablePath(path))
            {
                return invalid("Invalid merge rule match: " + rule.Match);
            }
        }
        return {};
    }

    Result<std::vector<MergeRule>> ParseRules(std::string_view bytes, const Limits& limits)
    {
        const auto json = ParseJson(bytes, {}, {}, limits, ErrorCode::INVALID_METADATA);
        if (!json)
        {
            return std::unexpected(json.error());
        }
        if (!json->is_object() || json->size() != 2 || !json->contains("version") ||
            !json->at("version").is_number_integer() || json->at("version") != 1 ||
            !json->contains("rules") || !json->at("rules").is_array())
        {
            return invalid("Expected merge rules version 1 and a rules array");
        }
        const auto& entries = json->at("rules");
        if (entries.size() > limits.MaxFilterBlocks)
        {
            return invalid("Too many merge rules");
        }
        auto result = std::vector<MergeRule>{};
        result.reserve(entries.size());
        for (const auto& item : entries)
        {
            if (!item.is_object() || item.size() != 2 || !item.contains("match") ||
                !item.at("match").is_string() || !item.contains("strategy") ||
                !item.at("strategy").is_string())
            {
                return invalid("Expected match and strategy strings");
            }
            const auto& name = item.at("strategy").get_ref<const std::string&>();
            const auto strategy = ParseStrategyName(name);
            if (!strategy)
            {
                return invalid("Unknown merge strategy: " + name);
            }
            result.push_back({
                .Match = item.at("match").get<std::string>(),
                .Strategy = *strategy,
            });
        }
        if (auto valid = ValidateRules(result, limits); !valid)
        {
            return std::unexpected(valid.error());
        }
        return result;
    }

    Result<std::string> SerializeRules(std::span<const MergeRule> rules, const Limits& limits)
    {
        if (auto valid = ValidateRules(rules, limits); !valid)
        {
            return std::unexpected(valid.error());
        }
        auto entries = Json::array();
        for (const auto& rule : rules)
        {
            entries.push_back({{"match", rule.Match}, {"strategy", StrategyName(rule.Strategy)}});
        }
        auto bytes = Json{{"version", 1}, {"rules", std::move(entries)}}.dump(2) + '\n';
        if (bytes.size() > limits.MaxJsonBytes)
        {
            return invalid("Merge rules file is too large");
        }
        return bytes;
    }

    Result<std::vector<MergeRule>> LoadRules(std::string_view path, const Limits& limits)
    {
        if (path.empty() || path.contains('\0') || !Utf16FromUtf8(path))
        {
            return invalid("Invalid merge rules file path");
        }
        auto file = std::ifstream(PathFromUtf8(path), std::ios::binary);
        if (!file)
        {
            return invalid("Cannot open merge rules file: " + std::string(path));
        }
        auto bytes = std::string{};
        auto buffer = std::array<char, 65536>{};
        while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0)
        {
            const auto count = static_cast<std::uint64_t>(file.gcount());
            if (bytes.size() > limits.MaxJsonBytes || count > limits.MaxJsonBytes - bytes.size())
            {
                return invalid("Merge rules file is too large");
            }
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
        if (file.bad() || !file.eof())
        {
            return invalid("Cannot read merge rules file: " + std::string(path));
        }
        auto result = ParseRules(bytes, limits);
        if (!result)
        {
            result.error().Path = std::string(path);
        }
        return result;
    }

    Result<void> SaveRules(std::string_view path, std::span<const MergeRule> rules,
                           const Limits& limits)
    {
        auto bytes = SerializeRules(rules, limits);
        if (!bytes)
        {
            return std::unexpected(bytes.error());
        }
        if (path.empty() || path.contains('\0') || !Utf16FromUtf8(path))
        {
            return invalid("Invalid merge rules file path");
        }
        const auto destination = PathFromUtf8(path);
        if (!destination.has_filename())
        {
            return invalid("Invalid merge rules file path");
        }
        const auto writeError = [path](std::string message)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::IO,
                .Message = "Cannot write merge rules file: " + std::move(message),
                .Pack = {},
                .Path = std::string(path),
            });
        };
        auto error = std::error_code{};
        const auto absolute = std::filesystem::absolute(destination, error);
        if (error)
        {
            return writeError(error.message());
        }
        const auto parent = std::filesystem::canonical(absolute.parent_path(), error);
        if (error)
        {
            return writeError(error.message());
        }
        const auto staging = StagingDirectory::Create(parent);
        if (!staging)
        {
            return writeError(staging.error());
        }
        const auto prepared = staging->Path() / "rules.json";
        auto output = FileWriter::Open(prepared);
        if (!output)
        {
            return writeError(output.error());
        }
        if (auto written = WriteBytes(*output, *bytes); !written)
        {
            return writeError(written.error().Message);
        }
        if (auto flushed = output->Flush(); !flushed)
        {
            return writeError(flushed.error());
        }
        if (auto closed = output->Close(); !closed)
        {
            return writeError(closed.error());
        }
        return PublishNoReplace(prepared, parent / absolute.filename());
    }

    Result<MergeStrategy> MatchRules(std::span<const MergeRule> rules, std::string_view path,
                                     std::uint64_t& budget, const std::stop_token& stop)
    {
        for (const auto& rule : rules | std::views::reverse)
        {
            const auto matched = matches(rule.Match, path, budget, stop);
            if (!matched)
            {
                return std::unexpected(matched.error());
            }
            if (*matched)
            {
                return rule.Strategy;
            }
        }
        return MergeStrategy::STANDARD;
    }
}
