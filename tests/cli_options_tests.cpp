#include "cli/options.hpp"
#include "core/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <initializer_list>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::expected<PackStacker::Cli::Options, std::string> parse(
        std::initializer_list<std::string_view> arguments)
    {
        return PackStacker::Cli::ParseOptions(std::span(arguments.begin(), arguments.size()));
    }

    std::vector<std::string_view> arguments()
    {
        return {
            "--type", "resource",        "--target",  "34",    "--output",
            "result", "--output-format", "directory", "input",
        };
    }

    void testValidOptions()
    {
        const auto result = PackStacker::Cli::ParseOptions(arguments());
        require(result.has_value(), "Valid options rejected");
        require(!result->Help && result->Type == PackStacker::PackType::RESOURCE &&
                    result->Target == PackStacker::PackFormat{.Major = 34, .Minor = 0} &&
                    result->Format == PackStacker::OutputFormat::DIRECTORY &&
                    result->Output == "result" &&
                    result->Description == "Combined with PackStacker" &&
                    result->Packs == std::vector<std::string>{"input"},
                "Parsed values changed");
        const auto modern = parse({
            "--type=data",
            "--target=65.2147483647",
            "-oРезультат 😀.zip",
            "--output-format=zip",
            "--description=Пак\n世界",
            "high",
            "--",
            "-low",
        });
        require(modern.has_value(), "Attached Unicode options rejected");
        require(modern->Target.Minor == 2147483647 && modern->Type == PackStacker::PackType::DATA &&
                    modern->Format == PackStacker::OutputFormat::ZIP &&
                    modern->Output == "Результат 😀.zip" && modern->Description == "Пак\n世界" &&
                    modern->Packs == std::vector<std::string>{"high", "-low"},
                "Unicode, minor format or priority changed");
        const auto emptyDescription = parse({
            "--type",
            "resource",
            "--target",
            "34",
            "-oresult",
            "--output-format",
            "directory",
            "--description",
            "",
            "-",
        });
        require(emptyDescription && emptyDescription->Description.empty() &&
                    emptyDescription->Packs.front() == "-",
                "Empty description or single-dash input rejected");
    }

    void testHelpAndParseOrder()
    {
        for (const auto* help : {"-h", "--help", "-hh", "-horesult"})
        {
            const auto result = parse({help});
            require(result && result->Help, "Help requires ordinary options");
        }
        const auto repeated = parse({"--type", "resource", "--type", "data", "--help"});
        require(repeated && repeated->Help, "Help must precede value validation");
        const auto unknownWithHelp = parse({"--help", "--unknown"});
        require(unknownWithHelp && unknownWithHelp->Help, "Help did not bypass unknown options");
        require(!parse({"--help", "--output"}), "Missing option value accepted");
        require(!parse({"--help=value"}), "Help accepted attached value");
        require(!parse({"--help-all"}), "Legacy help-all unexpectedly bypassed required options");
        const auto consumed = parse({"--type", "--help"});
        require(!consumed && consumed.error().contains("--target"),
                "Value beginning with dash was parsed as option");
        const auto help = PackStacker::Cli::HelpText("packstacker-cli");
        require(help.contains("packstacker-cli [OPTIONS]") && help.contains("HIGH to LOW") &&
                    help.contains("--target") && help.contains("Use -- before"),
                "Help lost command contract");
    }

    void testRequiredAndRepeatedOptions()
    {
        for (auto index = std::size_t{0}; index < 8; index += 2)
        {
            auto missing = arguments();
            missing.at(index + 1) = "";
            require(!PackStacker::Cli::ParseOptions(missing), "Empty required value accepted");
        }
        for (const auto& [name, value] : std::array{
                 std::pair{"--type", "resource"},
                 std::pair{"--target", "34"},
                 std::pair{"-o", "result"},
                 std::pair{"--output-format", "directory"},
                 std::pair{"--description", "description"},
             })
        {
            auto repeated = arguments();
            repeated.emplace_back(name);
            repeated.emplace_back(value);
            if (name == std::string_view("--description"))
            {
                repeated.emplace_back(name);
                repeated.emplace_back(value);
            }
            const auto result = PackStacker::Cli::ParseOptions(repeated);
            require(!result, "Repeated option accepted");
        }
        auto noInput = arguments();
        noInput.pop_back();
        const auto result = PackStacker::Cli::ParseOptions(noInput);
        require(!result && result.error().contains("packs"), "Missing input accepted");
    }

    void testFormatBounds()
    {
        for (const auto* format : {
                 "",
                 "3",
                 "-1",
                 "+34",
                 "34.",
                 ".1",
                 "34.1.0",
                 "34.-1",
                 "2147483648",
                 "65.2147483648",
                 "1.21.5",
                 " 34",
                 "34x",
                 "34. 1",
                 "34.+1",
                 "٣٤",
             })
        {
            auto invalid = arguments();
            invalid.at(3) = format;
            require(!PackStacker::Cli::ParseOptions(invalid), "Invalid target accepted");
        }
        for (const auto* format : {"4", "0004", "4.0", "2147483647.2147483647"})
        {
            auto valid = arguments();
            valid.at(3) = format;
            require(PackStacker::Cli::ParseOptions(valid).has_value(), "Valid target rejected");
        }
    }

    void testSingleLine()
    {
        require(PackStacker::Cli::SingleLine("Пак\\x\r\n\t世界") == "Пак\\\\x\\r\\n\\t世界",
                "Diagnostic escaping changed");
        const auto input = std::string("a\0b", 3);
        require(PackStacker::Cli::SingleLine(input) == input, "Embedded NUL truncated");
    }
}

int32_t main()
{
    const auto tests = std::array{
        std::pair{"valid options", testValidOptions},
        std::pair{"help and parse order", testHelpAndParseOrder},
        std::pair{"required and repeated options", testRequiredAndRepeatedOptions},
        std::pair{"format bounds", testFormatBounds},
        std::pair{"single-line diagnostics", testSingleLine},
    };
    auto failures = std::size_t{0};
    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
