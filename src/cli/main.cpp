#include "cli/options.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/rules.hpp"
#include "core/version.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <stringapiset.h>
#include <winnls.h>
#endif

namespace
{
    bool print(FILE* stream, std::string_view text)
    {
        while (!text.empty())
        {
            const auto written = std::fwrite(text.data(), 1, text.size(), stream);
            if (written == 0)
            {
                return false;
            }
            text.remove_prefix(written);
        }
        return std::fflush(stream) == 0;
    }

    int32_t success(std::string_view message)
    {
        if (print(stdout, message))
        {
            return 0;
        }
        print(stderr, "packstacker-cli: IO: Cannot write to stdout\n");
        return 5;
    }

    std::string_view errorName(PackStacker::ErrorCode code)
    {
        switch (code)
        {
            case PackStacker::ErrorCode::IO:
                return "IO";
            case PackStacker::ErrorCode::INVALID_PATH:
                return "INVALID_PATH";
            case PackStacker::ErrorCode::INVALID_ARCHIVE:
                return "INVALID_ARCHIVE";
            case PackStacker::ErrorCode::LIMIT_EXCEEDED:
                return "LIMIT_EXCEEDED";
            case PackStacker::ErrorCode::INVALID_METADATA:
                return "INVALID_METADATA";
            case PackStacker::ErrorCode::INCOMPATIBLE_FORMAT:
                return "INCOMPATIBLE_FORMAT";
            case PackStacker::ErrorCode::UNSUPPORTED_FEATURE:
                return "UNSUPPORTED_FEATURE";
            case PackStacker::ErrorCode::INVALID_RESOURCE:
                return "INVALID_RESOURCE";
            case PackStacker::ErrorCode::SOURCE_CHANGED:
                return "SOURCE_CHANGED";
            case PackStacker::ErrorCode::CANCELLED:
                return "CANCELLED";
            case PackStacker::ErrorCode::OUTPUT_EXISTS:
                return "OUTPUT_EXISTS";
        }
        return "UNKNOWN";
    }

    int32_t failure(const PackStacker::Diagnostic& diagnostic, int32_t exitCode)
    {
        auto message = std::string("packstacker-cli: ") + std::string(errorName(diagnostic.Code)) +
                       ": " + PackStacker::Cli::SingleLine(diagnostic.Message);
        if (!diagnostic.Pack.empty())
        {
            message += " | pack: " + PackStacker::Cli::SingleLine(diagnostic.Pack);
        }
        if (!diagnostic.Path.empty())
        {
            message += " | path: " + PackStacker::Cli::SingleLine(diagnostic.Path);
        }
        print(stderr, message + '\n');
        return exitCode;
    }

    int32_t run(const std::vector<std::string>& arguments)
    {
        const auto argumentError = [](std::string_view message)
        {
            print(stderr, "packstacker-cli: ARGUMENT: " + PackStacker::Cli::SingleLine(message) +
                              "\nUse --help for usage.\n");
            return 2;
        };
        auto views = std::vector<std::string_view>{};
        views.reserve(arguments.size());
        for (const auto& argument : arguments)
        {
            views.emplace_back(argument);
        }
        const auto options = PackStacker::Cli::ParseOptions(std::span(views).subspan(1));
        if (!options)
        {
            return argumentError(options.error());
        }
        if (options->Help)
        {
            return success(PackStacker::Cli::HelpText(arguments.front()));
        }
        if (options->VersionRequested)
        {
            return success(std::string("PackStacker ") + PackStacker::BuildVersion + '\n');
        }
        auto request = PackStacker::ResolveRequest{
            .PacksHighToLow = options->Packs,
            .Type = options->Type,
            .Target = options->Target,
            .Limits = {},
            .Rules = {},
        };
        if (!options->Rules.empty())
        {
            auto rules = PackStacker::LoadRules(options->Rules, request.Limits);
            if (!rules)
            {
                return failure(rules.error(), 2);
            }
            request.Rules = std::move(*rules);
        }
        auto resolved = PackStacker::ResolveStack(request);
        if (!resolved)
        {
            return failure(resolved.error(), 3);
        }
        const auto exportRequest = PackStacker::ExportRequest{
            .Path = options->Output,
            .Format = options->Format,
            .Description = options->Description,
            .Limits = {},
        };
        if (auto result = PackStacker::ExportPack(*resolved, exportRequest); !result)
        {
            return failure(result.error(), 4);
        }
        return success("Exported " + std::to_string(resolved->Files.size()) +
                       " files plus pack.mcmeta to " +
                       PackStacker::Cli::SingleLine(options->Output) + '\n');
    }

#ifdef _WIN32
    std::expected<std::string, std::string> utf8Argument(const wchar_t* argument)
    {
        const auto value = std::wstring_view(argument);
        if (value.empty())
        {
            return std::string{};
        }
        if (!std::in_range<int32_t>(value.size()))
        {
            return std::unexpected("Command-line argument is too long");
        }
        const auto size = static_cast<int32_t>(value.size());
        const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, size,
                                                  nullptr, 0, nullptr, nullptr);
        if (required == 0)
        {
            return std::unexpected("Invalid Unicode command-line argument");
        }
        auto result = std::string(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, size, result.data(),
                                required, nullptr, nullptr) != required)
        {
            return std::unexpected("Cannot convert command-line argument to UTF-8");
        }
        return result;
    }
#endif
}

#ifdef _WIN32
extern "C" int32_t wmain(int32_t argc, wchar_t* argv[])
{
    auto arguments = std::vector<std::string>{};
    for (const auto* argument : std::span(argv, static_cast<std::size_t>(std::max(argc, 0))))
    {
        auto converted = utf8Argument(argument);
        if (!converted)
        {
            print(stderr, "packstacker-cli: ARGUMENT: " + converted.error() + '\n');
            return 2;
        }
        arguments.push_back(std::move(*converted));
    }
    return arguments.empty() ? 2 : run(arguments);
}
#else
int32_t main(int32_t argc, char* argv[])
{
    auto arguments = std::vector<std::string>{};
    for (const auto* argument : std::span(argv, static_cast<std::size_t>(std::max(argc, 0))))
    {
        arguments.emplace_back(argument);
    }
    return arguments.empty() ? 2 : run(arguments);
}
#endif
