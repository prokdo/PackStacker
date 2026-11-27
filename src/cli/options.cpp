#include "cli/options.hpp"

#include "core/model.hpp"

#include <CLI/CLI.hpp>

#include <charconv>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    std::optional<PackStacker::PackFormat> parseFormat(std::string_view text)
    {
        auto result = PackStacker::PackFormat{};
        const auto separator = text.find('.');
        const auto major = text.substr(0, separator);
        const auto parseComponent = [](std::string_view component, int32_t& value)
        {
            if (component.empty() || component.front() < '0' || component.front() > '9')
            {
                return false;
            }
            const auto [end, error] = std::from_chars(std::to_address(component.begin()),
                                                      std::to_address(component.end()), value);
            return error == std::errc{} && end == std::to_address(component.end());
        };
        if (!parseComponent(major, result.Major) || result.Major < 4)
        {
            return std::nullopt;
        }
        if (separator != std::string_view::npos &&
            !parseComponent(text.substr(separator + 1), result.Minor))
        {
            return std::nullopt;
        }
        return result;
    }

    struct Arguments
    {
        PackStacker::Cli::Options Values;
        std::string Type;
        std::string Format;
    };

    void configure(CLI::App& app, Arguments& arguments)
    {
        app.description(
            "Combine Minecraft Java packs without version conversion.\n"
            "Inputs are ordered HIGH to LOW priority: the first pack wins ordinary file "
            "conflicts.\n"
            "The output must not exist. Core compatibility restrictions still apply.");
        app.get_help_ptr()->disable_flag_override();
        app.set_version_flag("--version", "")->disable_flag_override();
        app.add_option("--type", arguments.Type, "Pack type: resource or data")
            ->required()
            ->check(CLI::IsMember({"resource", "data"}));
        app.add_option_function<std::string>(
               "--target",
               [&](const std::string& value)
               {
                   const auto target = parseFormat(value);
                   if (!target)
                   {
                       throw CLI::ValidationError(
                           "--target",
                           "must be MAJOR or MAJOR.MINOR with int32 components and MAJOR >= 4");
                   }
                   arguments.Values.Target = *target;
               },
               "Exact pack format, not a game version")
            ->required()
            ->type_name("MAJOR[.MINOR]");
        app.add_option("-o,--output", arguments.Values.Output, "New output path")
            ->required()
            ->check([](const std::string& value) -> std::string
                    { return value.empty() ? "must not be empty" : ""; });
        app.add_option("--output-format", arguments.Format, "Output format: directory or zip")
            ->required()
            ->check(CLI::IsMember({"directory", "zip"}));
        app.add_option("--description", arguments.Values.Description,
                       "Output pack description (plain text)");
        app.add_option("--rules", arguments.Values.Rules, "Merge rules JSON file")
            ->check([](const std::string& value) -> std::string
                    { return value.empty() ? "must not be empty" : ""; });
        app.add_option("packs", arguments.Values.Packs,
                       "Input folders or ZIPs, highest priority first. Use -- before paths "
                       "starting with '-'.")
            ->required();
    }
}

namespace PackStacker::Cli
{
    std::expected<Options, std::string> ParseOptions(std::span<const std::string_view> arguments)
    {
        auto parsed = Arguments{};
        auto app = CLI::App{};
        configure(app, parsed);
        try
        {
            app.parse(std::vector<std::string>(arguments.rbegin(), arguments.rend()));
        }
        catch (const CLI::CallForHelp&)
        {
            auto result = Options{};
            result.Help = true;
            return result;
        }
        catch (const CLI::CallForVersion&)
        {
            auto result = Options{};
            result.VersionRequested = true;
            return result;
        }
        catch (const CLI::ParseError& error)
        {
            return std::unexpected(std::string(error.what()));
        }
        parsed.Values.Type = parsed.Type == "resource" ? PackType::RESOURCE : PackType::DATA;
        parsed.Values.Format = parsed.Format == "zip" ? OutputFormat::ZIP : OutputFormat::DIRECTORY;
        return std::move(parsed.Values);
    }

    std::string HelpText(std::string_view executable)
    {
        auto arguments = Arguments{};
        auto app = CLI::App{"", SingleLine(executable)};
        configure(app, arguments);
        return app.help();
    }

    std::string SingleLine(std::string_view text)
    {
        auto result = std::string{};
        for (const auto character : text)
        {
            switch (character)
            {
                case '\\':
                    result += "\\\\";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    result += character;
            }
        }
        return result;
    }
}
