#include "core/filters.hpp"

#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/pattern.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <iterator>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        Diagnostic patternFailure(const PatternError& error, std::string_view pack)
        {
            auto code = ErrorCode::UNSUPPORTED_FEATURE;
            switch (error.Code)
            {
                case PatternErrorCode::SIZE_LIMIT:
                case PatternErrorCode::WORK_LIMIT:
                    code = ErrorCode::LIMIT_EXCEEDED;
                    break;
                case PatternErrorCode::CANCELLED:
                    code = ErrorCode::CANCELLED;
                    break;
                case PatternErrorCode::INVALID_TEXT:
                    code = ErrorCode::INVALID_PATH;
                    break;
                case PatternErrorCode::UNSUPPORTED_PATTERN:
                    break;
            }
            return Diagnostic{
                .Code = code,
                .Message = error.Message,
                .Pack = std::string(pack),
                .Path = "pack.mcmeta",
            };
        }

        Result<bool> blockedByFilters(std::string_view logical, std::string_view root,
                                      const std::vector<CompiledFilter>& filters,
                                      std::uint64_t& budget, std::string_view pack,
                                      const std::stop_token& stop)
        {
            if (!logical.starts_with(root))
            {
                return false;
            }
            const auto resource = logical.substr(root.size());
            const auto separator = resource.find('/');
            const auto name = resource.substr(0, separator);
            const auto path = separator == std::string_view::npos ? std::string_view{}
                                                                  : resource.substr(separator + 1);
            for (const auto& filter : filters)
            {
                auto matchedName = filter.NamespacePattern.Matches(name, budget, pack, stop);
                if (!matchedName)
                {
                    return std::unexpected(matchedName.error());
                }
                if (!*matchedName)
                {
                    continue;
                }
                auto matchedPath = filter.PathPattern.Matches(path, budget, pack, stop);
                if (!matchedPath)
                {
                    return std::unexpected(matchedPath.error());
                }
                if (*matchedPath)
                {
                    return true;
                }
            }
            return false;
        }
    }

    Result<FilterPattern> FilterPattern::Compile(std::string_view pattern, const Limits& limits,
                                                 std::string_view pack)
    {
        auto compiled = PortablePattern::Compile(pattern, limits.MaxFilterPatternBytes);
        if (!compiled)
        {
            return std::unexpected(patternFailure(compiled.error(), pack));
        }
        auto result = FilterPattern{};
        result.pattern = std::move(*compiled);
        return result;
    }

    Result<bool> FilterPattern::Matches(std::string_view value, std::uint64_t& budget,
                                        std::string_view pack, const std::stop_token& stop) const
    {
        const auto matched = pattern.Matches(value, budget, stop);
        if (!matched)
        {
            return std::unexpected(patternFailure(matched.error(), pack));
        }
        return *matched;
    }

    Result<void> ValidateFilterSet(const std::vector<ResourceFilterBlock>& blocks,
                                   const Limits& limits, std::string_view pack)
    {
        if (std::cmp_greater(blocks.size(), limits.MaxFilterBlocks))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::LIMIT_EXCEEDED,
                .Message = "Filter block limit exceeded",
                .Pack = std::string(pack),
                .Path = "pack.mcmeta",
            });
        }
        if (blocks.empty())
        {
            return {};
        }
        const auto sameNamespace = std::ranges::all_of(
            blocks, [&](const auto& block)
            { return block.NamespacePattern == blocks.front().NamespacePattern; });
        const auto samePath =
            std::ranges::all_of(blocks, [&](const auto& block)
                                { return block.PathPattern == blocks.front().PathPattern; });
        if (!sameNamespace && !samePath)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::UNSUPPORTED_FEATURE,
                .Message =
                    "Filter blocks must share a namespace or path pattern to avoid MC-271761",
                .Pack = std::string(pack),
                .Path = "pack.mcmeta",
            });
        }
        return {};
    }

    Result<std::vector<CompiledFilter>> CompileFilters(
        const std::vector<ResourceFilterBlock>& blocks, const Limits& limits, std::string_view pack)
    {
        if (auto valid = ValidateFilterSet(blocks, limits, pack); !valid)
        {
            return std::unexpected(valid.error());
        }
        auto result = std::vector<CompiledFilter>{};
        for (const auto& block : blocks)
        {
            auto name = FilterPattern::Compile(block.NamespacePattern, limits, pack);
            if (!name)
            {
                return std::unexpected(name.error());
            }
            auto path = FilterPattern::Compile(block.PathPattern, limits, pack);
            if (!path)
            {
                return std::unexpected(path.error());
            }
            result.push_back({
                .Block = block,
                .NamespacePattern = std::move(*name),
                .PathPattern = std::move(*path),
            });
        }
        return result;
    }

    Result<void> ApplyFilters(std::map<std::string, ResolvedFile>& files,
                              const std::vector<CompiledFilter>& filters, PackType type,
                              std::uint64_t& budget, std::string_view pack,
                              const std::stop_token& stop)
    {
        if (filters.empty())
        {
            return {};
        }
        const auto root =
            type == PackType::RESOURCE ? std::string_view{"assets/"} : std::string_view{"data/"};
        auto entry = files.begin();
        while (entry != files.end())
        {
            if (stop.stop_requested())
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::CANCELLED,
                    .Message = "Cancelled",
                    .Pack = std::string(pack),
                    .Path = entry->first,
                });
            }
            const auto path = std::string_view(entry->first);
            auto remove = blockedByFilters(path, root, filters, budget, pack, stop);
            if (!remove)
            {
                return std::unexpected(remove.error());
            }
            if (!*remove && path.ends_with(".mcmeta"))
            {
                remove = blockedByFilters(path.substr(0, path.size() - 7), root, filters, budget,
                                          pack, stop);
                if (!remove)
                {
                    return std::unexpected(remove.error());
                }
            }
            entry = *remove ? files.erase(entry) : std::next(entry);
        }
        return {};
    }
}
