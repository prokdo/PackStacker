#pragma once

#include "core/pack.hpp"
#include "core/pattern.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace PackStacker
{
    class FilterPattern
    {
    public:
        static Result<FilterPattern> Compile(std::string_view pattern, const Limits& limits,
                                             std::string_view pack);
        Result<bool> Matches(std::string_view value, std::uint64_t& budget, std::string_view pack,
                             const std::stop_token& stop) const;

    private:
        PortablePattern pattern;
    };

    struct CompiledFilter
    {
        ResourceFilterBlock Block;
        FilterPattern NamespacePattern;
        FilterPattern PathPattern;
    };

    Result<void> ValidateFilterSet(const std::vector<ResourceFilterBlock>& blocks,
                                   const Limits& limits, std::string_view pack);
    Result<std::vector<CompiledFilter>> CompileFilters(
        const std::vector<ResourceFilterBlock>& blocks, const Limits& limits,
        std::string_view pack);
    Result<void> ApplyFilters(std::map<std::string, ResolvedFile>& files,
                              const std::vector<CompiledFilter>& filters, PackType type,
                              std::uint64_t& budget, std::string_view pack,
                              const std::stop_token& stop);
}
