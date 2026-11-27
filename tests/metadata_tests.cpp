#include "core/metadata.hpp"
#include "core/model.hpp"
#include "core/sounds.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    constexpr auto SourcePath = std::string_view{"паки/😀"};

    bool checkMetadata()
    {
        const auto parsed = PackStacker::ParseMetadata(
            R"({"pack":{"description":"Описание 😀","min_format":[34,1],"max_format":[35,2],"pack_format":34},"language":{"custom":{"name":"Язык 😀","region":"Регион","bidirectional":true}},"filter":{"block":[{"namespace":"example","path":"textures/.*"}]},"overlays":{"entries":[{"directory":"extra","formats":34}]}})",
            SourcePath, {});
        if (!parsed)
        {
            std::cerr << parsed.error().Message << '\n';
            return false;
        }
        return parsed->Formats.Minimum == PackStacker::PackFormat{.Major = 34, .Minor = 1} &&
               parsed->Formats.Maximum == PackStacker::PackFormat{.Major = 35, .Minor = 2} &&
               parsed->Languages.at("custom").Name == "Язык 😀" &&
               parsed->Languages.at("custom").Region == "Регион" &&
               parsed->Languages.at("custom").Bidirectional && parsed->Filters.size() == 1 &&
               parsed->Overlays.size() == 1 && parsed->Overlays.front().Directory == "extra" &&
               parsed->Overlays.front().Formats.Minimum.Major == 34;
    }

    bool checkMetadataErrors()
    {
        struct Example
        {
            std::string_view Text;
            PackStacker::ErrorCode Code;
            std::string_view Message;
        };
        constexpr auto examples = std::array{
            Example{
                .Text = R"({"pack":{"pack_format":34,"description":"x","pack_format":35}})",
                .Code = PackStacker::ErrorCode::INVALID_METADATA,
                .Message = "Duplicate JSON key",
            },
            Example{
                .Text = R"({"pack":{"pack_format":2147483648,"description":"x"}})",
                .Code = PackStacker::ErrorCode::INVALID_METADATA,
                .Message = "Invalid or inconsistent pack format range",
            },
            Example{
                .Text =
                    R"({"pack":{"pack_format":34,"description":"x"},"language":[],"filter":false})",
                .Code = PackStacker::ErrorCode::INVALID_METADATA,
                .Message = "Invalid language metadata object",
            },
            Example{
                .Text =
                    R"({"pack":{"pack_format":34,"description":"x"},"overlays":{"entries":[{"directory":"NUL","formats":false}]}})",
                .Code = PackStacker::ErrorCode::INVALID_METADATA,
                .Message = "Invalid overlay directory",
            },
            Example{
                .Text =
                    R"({"pack":{"pack_format":34,"description":"x"},"filter":{"block":[{"path":"[a]"}]}})",
                .Code = PackStacker::ErrorCode::UNSUPPORTED_FEATURE,
                .Message = "Filter regex is outside the supported portable subset",
            },
        };
        for (const auto& example : examples)
        {
            const auto parsed = PackStacker::ParseMetadata(example.Text, SourcePath, {});
            if (parsed || parsed.error().Code != example.Code ||
                parsed.error().Message != example.Message || parsed.error().Pack != SourcePath ||
                parsed.error().Path != "pack.mcmeta")
            {
                std::cerr << "Unexpected metadata diagnostic: " << example.Text << '\n';
                return false;
            }
        }
        return true;
    }

    bool checkJsonLimits()
    {
        auto limits = PackStacker::Limits{};
        limits.MaxJsonBytes = 1;
        const auto oversized = PackStacker::ParseJson("{}", SourcePath, "данные.json", limits,
                                                      PackStacker::ErrorCode::INVALID_RESOURCE);
        if (oversized || oversized.error().Code != PackStacker::ErrorCode::LIMIT_EXCEEDED ||
            oversized.error().Message != "JSON size limit exceeded" ||
            oversized.error().Pack != SourcePath || oversized.error().Path != "данные.json")
        {
            return false;
        }
        limits = {};
        limits.MaxJsonDepth = 1;
        const auto nested = PackStacker::ParseMetadata(R"({"pack":{}})", SourcePath, limits);
        if (nested || nested.error().Code != PackStacker::ErrorCode::LIMIT_EXCEEDED ||
            nested.error().Message != "JSON nesting limit exceeded")
        {
            return false;
        }
        const auto malformed = PackStacker::ParseJson("\"\xed\xa0\x80\"", SourcePath, "test.json",
                                                      {}, PackStacker::ErrorCode::INVALID_RESOURCE);
        return !malformed && malformed.error().Code == PackStacker::ErrorCode::INVALID_RESOURCE &&
               malformed.error().Message == "Invalid JSON";
    }

    bool checkSounds()
    {
        const auto lower =
            PackStacker::Json::parse(R"({"event":{"subtitle":"low","sounds":["first"]}})");
        const auto higher = PackStacker::Json::parse(
            R"({"event":{"subtitle":"high","sounds":[{"name":"second","volume":0.5,"weight":2}]}})");
        if (!PackStacker::ValidateSounds(lower, SourcePath, "sounds.json") ||
            !PackStacker::ValidateSounds(higher, SourcePath, "sounds.json"))
        {
            return false;
        }
        const auto merged = PackStacker::MergeSounds(lower, higher);
        if (merged.at("event").at("subtitle") != "low" ||
            merged.at("event").at("sounds").size() != 2)
        {
            return false;
        }
        const auto invalid = PackStacker::ValidateSounds(
            PackStacker::Json::parse(
                R"({"event":{"sounds":[{"name":"valid","weight":2147483648}]}})"),
            SourcePath, "звуки.json");
        return !invalid && invalid.error().Code == PackStacker::ErrorCode::INVALID_RESOURCE &&
               invalid.error().Message == "Invalid integer sound parameter" &&
               invalid.error().Pack == SourcePath && invalid.error().Path == "звуки.json";
    }
}

int32_t main()
{
    return checkMetadata() && checkMetadataErrors() && checkJsonLimits() && checkSounds()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
