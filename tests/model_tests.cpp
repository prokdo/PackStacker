#include "core/model.hpp"
#include "core/pack.hpp"

#include <cstdint>
#include <expected>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    static_assert(std::is_same_v<std::underlying_type_t<PackStacker::PackType>, std::uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<PackStacker::ErrorCode>, std::uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<PackStacker::OutputFormat>, std::uint8_t>);
    static_assert(std::is_same_v<decltype(PackStacker::Diagnostic::Message), std::string>);
    static_assert(std::is_same_v<decltype(PackStacker::Diagnostic::Pack), std::string>);
    static_assert(std::is_same_v<decltype(PackStacker::Diagnostic::Path), std::string>);
    static_assert(std::is_same_v<decltype(PackStacker::ExportRequest::Path), std::string>);
    static_assert(std::is_same_v<decltype(PackStacker::ExportRequest::Description), std::string>);
    static_assert(std::is_same_v<decltype(PackStacker::PackInfo::Path), std::string>);
    static_assert(std::is_same_v<decltype(PackStacker::ResolveRequest::PacksHighToLow),
                                 std::vector<std::string>>);
    static_assert(std::is_same_v<decltype(PackStacker::ResolvedPack::Files),
                                 std::map<std::string, PackStacker::ResolvedFile>>);
    static_assert(
        std::is_same_v<decltype(PackStacker::ResolvedPack::SourcePaths), std::vector<std::string>>);
    static_assert(
        std::is_same_v<PackStacker::Result<void>, std::expected<void, PackStacker::Diagnostic>>);
    static_assert(PackStacker::Result<int32_t>{42}.value() == 42);
    constexpr auto Range = PackStacker::FormatRange{
        .Minimum = {.Major = 34, .Minor = 1},
        .Maximum = {.Major = 35, .Minor = 2},
    };
    static_assert(!Range.Contains({.Major = 34, .Minor = 0}));
    static_assert(Range.Contains(Range.Minimum));
    static_assert(Range.Contains({.Major = 35, .Minor = 0}));
    static_assert(Range.Contains(Range.Maximum));
    static_assert(!Range.Contains({.Major = 35, .Minor = 3}));
    constexpr auto Limits = PackStacker::Limits{};
    static_assert(Limits.MaxTotalBytes == std::uint64_t{4294967296});
    static_assert(Limits.MaxFilterMatchSteps == std::uint64_t{1000000000});
}

int32_t main()
{
    const auto language = PackStacker::LanguageDefinition{
        .Name = "Язык 😀",
        .Region = "Регион",
        .Bidirectional = true,
    };
    const auto filter = PackStacker::ResourceFilterBlock{};
    const auto request = PackStacker::ExportRequest{};
    const auto resolveRequest = PackStacker::ResolveRequest{};
    const auto packInfo = PackStacker::PackInfo{};
    if (language.Name != "Язык 😀" || language.Region != "Регион" || !language.Bidirectional ||
        filter.NamespacePattern != ".*" || filter.PathPattern != ".*" || !request.Path.empty() ||
        request.Format != PackStacker::OutputFormat::ZIP ||
        request.Description != "Combined with PackStacker" ||
        request.Limits.MaxTotalBytes != Limits.MaxTotalBytes || !packInfo.Path.empty() ||
        packInfo.FileCount != 0 || packInfo.TotalBytes != 0 ||
        !resolveRequest.PacksHighToLow.empty() ||
        resolveRequest.Type != PackStacker::PackType::RESOURCE ||
        resolveRequest.Target != PackStacker::PackFormat{} ||
        resolveRequest.Limits.MaxTotalBytes != Limits.MaxTotalBytes)
    {
        std::cerr << "Standard metadata model defaults or UTF-8 text changed\n";
        return 1;
    }
    return 0;
}
