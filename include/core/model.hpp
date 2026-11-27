#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace PackStacker
{
    enum class PackType : std::uint8_t
    {
        RESOURCE,
        DATA,
    };

    struct PackFormat
    {
        int32_t Major = 0;
        int32_t Minor = 0;
        auto operator<=>(const PackFormat&) const = default;
    };

    struct FormatRange
    {
        PackFormat Minimum;
        PackFormat Maximum;

        constexpr bool Contains(PackFormat value) const
        {
            return value >= Minimum && value <= Maximum;
        }
    };

    enum class ErrorCode : std::uint8_t
    {
        IO,
        INVALID_PATH,
        INVALID_ARCHIVE,
        LIMIT_EXCEEDED,
        INVALID_METADATA,
        INCOMPATIBLE_FORMAT,
        UNSUPPORTED_FEATURE,
        INVALID_RESOURCE,
        SOURCE_CHANGED,
        CANCELLED,
        OUTPUT_EXISTS,
    };

    struct Diagnostic
    {
        ErrorCode Code;
        std::string Message;
        std::string Pack;
        std::string Path;
    };

    template <typename T>
    using Result = std::expected<T, Diagnostic>;

    struct Limits
    {
        std::uint64_t MaxPacks = 256;
        std::uint64_t MaxEntries = 100000;
        std::uint64_t MaxFileBytes = 512ULL * 1024 * 1024;
        std::uint64_t MaxTotalBytes = 4ULL * 1024 * 1024 * 1024;
        std::uint64_t MaxJsonBytes = 16ULL * 1024 * 1024;
        int32_t MaxJsonDepth = 128;
        std::uint64_t MaxFilterBlocks = 256;
        std::uint64_t MaxFilterPatternBytes = 1024;
        std::uint64_t MaxFilterMatchSteps = 1000000000;
    };

    struct LanguageDefinition
    {
        std::string Name;
        std::string Region;
        bool Bidirectional = false;
    };

    struct ResourceFilterBlock
    {
        std::string NamespacePattern = ".*";
        std::string PathPattern = ".*";
    };

    struct PackInfo
    {
        std::string Path;
        FormatRange Formats;
        std::uint64_t FileCount = 0;
        std::uint64_t TotalBytes = 0;
    };

    enum class MergeStrategy : std::uint8_t
    {
        STANDARD,
        HIGH_PRIORITY,
        LOW_PRIORITY,
        LANGUAGE,
        TAG,
        FONT,
        ATLAS,
        SOUNDS,
    };

    struct MergeRule
    {
        std::string Match;
        MergeStrategy Strategy = MergeStrategy::STANDARD;
    };

    struct ResolveRequest
    {
        std::vector<std::string> PacksHighToLow;
        PackType Type = PackType::RESOURCE;
        PackFormat Target;
        PackStacker::Limits Limits;
        std::vector<MergeRule> Rules;
    };

    enum class OutputFormat : std::uint8_t
    {
        DIRECTORY,
        ZIP,
    };

    struct ExportRequest
    {
        std::string Path;
        OutputFormat Format = OutputFormat::ZIP;
        std::string Description = "Combined with PackStacker";
        PackStacker::Limits Limits;
    };
}
