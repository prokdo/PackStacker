#pragma once

#include "core/model.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PackStacker::Cli
{
    struct Options
    {
        bool Help = false;
        bool VersionRequested = false;
        PackType Type = PackType::RESOURCE;
        PackFormat Target;
        OutputFormat Format = OutputFormat::DIRECTORY;
        std::string Output;
        std::string Description = "Combined with PackStacker";
        std::string Rules;
        std::vector<std::string> Packs;
    };

    std::expected<Options, std::string> ParseOptions(std::span<const std::string_view> arguments);
    std::string HelpText(std::string_view executable);
    std::string SingleLine(std::string_view text);
}
