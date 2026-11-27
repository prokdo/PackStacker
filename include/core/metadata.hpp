#pragma once

#include "core/filters.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PackStacker
{
    class PackSource;

    using Json = nlohmann::json;

    struct Overlay
    {
        std::string Directory;
        FormatRange Formats;
    };

    struct Metadata
    {
        FormatRange Formats;
        std::vector<Overlay> Overlays;
        std::map<std::string, LanguageDefinition> Languages;
        std::vector<CompiledFilter> Filters;
    };

    Result<Json> ParseJson(std::string_view bytes, std::string_view pack, std::string_view path,
                           const Limits& limits, ErrorCode code);
    Result<Metadata> ParseMetadata(std::string_view bytes, std::string_view sourcePath,
                                   const Limits& limits);
    Result<Metadata> ReadMetadata(const PackSource& source, const Limits& limits,
                                  const std::stop_token& stop);
    Result<void> ValidateLanguages(const std::map<std::string, LanguageDefinition>& languages,
                                   std::string_view pack);
}
