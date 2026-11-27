#include "core/filesystem.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/version.hpp"
#include "testing/fixtures.hpp"
#include "testing/process.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using PackStacker::PackType;
    using PackStacker::ResolveStack;
    using PackStacker::Result;
    using Json = nlohmann::json;
    bool exists(std::string_view path)
    {
        return std::filesystem::exists(PackStacker::PathFromUtf8(path));
    }

    using Files = std::vector<std::pair<std::string, std::string>>;

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename T>
    T take(Result<T> result)
    {
        if (!result)
        {
            throw std::runtime_error(result.error().Message);
        }
        return std::move(*result);
    }

    using PackStacker::Testing::Read;
    using PackStacker::Testing::Write;

    using PackStacker::Testing::Invocation;

    void expect(const Invocation& invocation, int32_t code, const std::string& error = {})
    {
        if (invocation.Code != code || (!error.empty() && !invocation.Error.contains(error)))
        {
            throw std::runtime_error("Unexpected CLI result " + std::to_string(invocation.Code) +
                                     ": " + invocation.Error);
        }
        if (code == 0)
        {
            require(invocation.Error.empty(), "Success wrote to stderr");
            require(!invocation.Output.empty(), "Success did not write to stdout");
        }
        else
        {
            require(invocation.Output.empty(), "Failure wrote success output");
        }
    }

    struct Fixture
    {
        PackStacker::StagingDirectory Temporary;

        Fixture() : Temporary(PackStacker::Testing::TemporaryDirectory())
        {
        }
        std::string Path(const std::string& name) const
        {
            return PackStacker::PathToUtf8(Temporary.Path() / PackStacker::PathFromUtf8(name));
        }
        std::string Pack(const std::string& name, const Files& files = {},
                         int32_t format = 34) const
        {
            const auto root = Path(name);
            Write(root + "/pack.mcmeta",
                  std::string(
                      Json{{"pack", {{"pack_format", format}, {"description", "Test"}}}}.dump()));
            for (const auto& [relative, bytes] : files)
            {
                Write((root + '/').append(relative), bytes);
            }
            return root;
        }
        Invocation Invoke(std::string_view executable,
                          const std::vector<std::string>& arguments) const
        {
            return PackStacker::Testing::Run(executable, arguments,
                                             PackStacker::PathToUtf8(Temporary.Path()));
        }
        void Clean() const
        {
            for (const auto& entry : std::filesystem::directory_iterator(Temporary.Path()))
            {
                require(
                    !PackStacker::PathToUtf8(entry.path().filename()).starts_with(".packstacker-"),
                    "Staging leaked");
            }
        }
    };

    std::vector<std::string> operator+(std::vector<std::string> first,
                                       const std::vector<std::string>& second)
    {
        first.insert(first.end(), second.begin(), second.end());
        return first;
    }

    std::vector<std::string> options(const std::string& output = "result",
                                     const std::string& format = "directory")
    {
        return {
            "--type", "resource", "--target", "34", "--output-format", format, "--output", output,
        };
    }

    constexpr auto LANGUAGE = std::string_view("assets/example/lang/en_us.json");
    constexpr auto OPAQUE = std::string_view("assets/custom/path/file.bin");
}

#ifdef _WIN32
extern "C" int32_t wmain(int32_t argc, wchar_t* argv[])
#else
int32_t main(int32_t argc, char* argv[])
#endif
{
    require(argc == 2, "Expected CLI executable path");
    const auto* supplied = std::span(argv, static_cast<std::size_t>(argc)).subspan(1).front();
#ifdef _WIN32
    const auto executable = PackStacker::PathToUtf8(std::filesystem::path(supplied));
#else
    const auto executable = std::string(supplied);
#endif
    const auto tests = std::vector<std::pair<const char*, std::function<void()>>>{
        {
            "version prints the embedded build version without merge arguments",
            [&]
            {
                const auto fixture = Fixture{};
                const auto invocation = fixture.Invoke(executable, {"--version"});
                expect(invocation, 0);
                const auto version = std::string("PackStacker ") + PackStacker::BuildVersion;
                require(
                    invocation.Output == version + "\n" || invocation.Output == version + "\r\n",
                    "CLI reports the wrong embedded version");
            },
        },
        {
            "JSON merge rules override defaults without Qt",
            [&]
            {
                const auto fixture = Fixture{};
                const auto lower =
                    fixture.Pack("lower", {{std::string(LANGUAGE), R"({"low":"keep"})"}});
                const auto higher =
                    fixture.Pack("higher", {{std::string(LANGUAGE), R"({"high":"keep"})"}});
                const auto rules = fixture.Path("правила-😀.json");
                Write(
                    rules,
                    R"({"version":1,"rules":[{"match":"assets/**/lang/*.json","strategy":"low_priority"}]})");
                expect(fixture.Invoke(
                           executable,
                           options() + std::vector<std::string>{"--rules", rules, higher, lower}),
                       0);
                require(Json::parse(Read(fixture.Path("result/") + std::string(LANGUAGE))) ==
                            Json::parse(R"({"low":"keep"})"),
                        "CLI ignored custom rules");
                Write(rules, R"({"version":1,"rules":[{"match":"**","strategy":"unknown"}]})");
                expect(fixture.Invoke(executable, options("invalid") +
                                                      std::vector<std::string>{
                                                          "--rules",
                                                          rules,
                                                          higher,
                                                          lower,
                                                      }),
                       2);
                require(!exists(fixture.Path("invalid")), "Invalid rules published output");
                expect(fixture.Invoke(executable, options("missing") +
                                                      std::vector<std::string>{
                                                          "--rules",
                                                          fixture.Path("missing.json"),
                                                          higher,
                                                      }),
                       2);
            },
        },
        {
            "help works headlessly without required arguments",
            [executable]
            {
                const auto fixture = Fixture{};
                for (const auto& help : {"--help", "-h"})
                {
                    const auto result = fixture.Invoke(executable, {help});
                    expect(result, 0);
                    require(
                        result.Output.contains("HIGH to LOW") && result.Output.contains("--target"),
                        "Help omits contract");
                }
                expect(fixture.Invoke(executable, {"--help", "--unknown"}), 0);
                expect(fixture.Invoke(executable, {"--help", "--output"}), 2, "--output");
            },
        },
        {
            "missing required options and inputs",
            [executable]
            {
                const auto fixture = Fixture{};
                expect(fixture.Invoke(executable, {}), 2, "ARGUMENT");
                const auto valid = options();
                for (auto index = std::size_t{0}; index < valid.size(); index += 2)
                {
                    auto arguments = valid;
                    arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index),
                                    arguments.begin() + static_cast<std::ptrdiff_t>(index + 2));
                    arguments.emplace_back("input");
                    expect(fixture.Invoke(executable, arguments), 2, "ARGUMENT");
                }
                expect(fixture.Invoke(executable, valid), 2, "packs");
                require(!exists(fixture.Path("result")), "Invalid arguments created output");
            },
        },
        {
            "unknown, missing-value and repeated options",
            [executable]
            {
                const auto fixture = Fixture{};
                expect(fixture.Invoke(executable, {"--unknown"}), 2, "ARGUMENT");
                expect(fixture.Invoke(executable, {"--output"}), 2, "ARGUMENT");
                for (const auto& repeated : {
                         std::vector<std::string>{"--type", "data"},
                         std::vector<std::string>{"--target", "34"},
                         std::vector<std::string>{"-o", "other"},
                         std::vector<std::string>{"--output-format", "zip"},
                         std::vector<std::string>{"--description", "one", "--description", "two"},
                     })
                {
                    expect(fixture.Invoke(executable,
                                          options() + repeated + std::vector<std::string>{"input"}),
                           2, "ARGUMENT");
                }
            },
        },
        {
            "invalid option values and format boundaries",
            [executable]
            {
                const auto fixture = Fixture{};
                for (const auto& target : {
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
                     })
                {
                    auto arguments = options();
                    arguments.at(3) = target;
                    expect(
                        fixture.Invoke(executable, arguments + std::vector<std::string>{"input"}),
                        2, "ARGUMENT");
                }
                auto arguments = options();
                arguments.at(1) = "mixed";
                expect(fixture.Invoke(executable, arguments + std::vector<std::string>{"input"}), 2,
                       "--type");
                arguments = options();
                arguments.at(5) = "auto";
                expect(fixture.Invoke(executable, arguments + std::vector<std::string>{"input"}), 2,
                       "--output-format");
                arguments = options();
                arguments.at(7) = "";
                expect(fixture.Invoke(executable, arguments + std::vector<std::string>{"input"}), 2,
                       "--output");
            },
        },
        {
            "directory export combines keys and copies opaque bytes",
            [executable]
            {
                const auto fixture = Fixture{};
                const auto bytes = std::string("\0\xff\r\n", 4);
                fixture.Pack("low",
                             {
                                 {std::string(LANGUAGE), R"({"shared":"low","unique":"low"})"},
                                 {std::string(OPAQUE), "low"},
                             });
                fixture.Pack("high", {
                                         {std::string(LANGUAGE), R"({"shared":"high"})"},
                                         {std::string(OPAQUE), bytes},
                                     });
                expect(
                    fixture.Invoke(executable, options() + std::vector<std::string>{"high", "low"}),
                    0);
                const auto language =
                    Json::parse(Read(fixture.Path("result/" + std::string(LANGUAGE))));
                require(language == Json{{"shared", "high"}, {"unique", "low"}},
                        "Wrong translation merge");
                require(Read(fixture.Path("result/" + std::string(OPAQUE))) == bytes,
                        "Opaque bytes changed");
                const auto metadata = Json::parse(Read(fixture.Path("result/pack.mcmeta")));
                require(metadata.at("pack").at("description") == "Combined with PackStacker",
                        "Default description missing");
                fixture.Clean();
            },
        },
        {
            "reversing inputs reverses file priority",
            [executable]
            {
                const auto fixture = Fixture{};
                fixture.Pack("low", {{std::string(OPAQUE), "low"}});
                fixture.Pack("high", {{std::string(OPAQUE), "high"}});
                expect(fixture.Invoke(executable,
                                      options("first") + std::vector<std::string>{"high", "low"}),
                       0);
                expect(fixture.Invoke(executable,
                                      options("second") + std::vector<std::string>{"low", "high"}),
                       0);
                require(Read(fixture.Path("first/" + std::string(OPAQUE))) == "high" &&
                            Read(fixture.Path("second/" + std::string(OPAQUE))) == "low",
                        "Priority does not follow input order");
            },
        },
        {
            "ZIP output and ZIP input round trip independent of extension",
            [executable]
            {
                const auto fixture = Fixture{};
                fixture.Pack("input", {{std::string(OPAQUE), "bytes"}});
                expect(fixture.Invoke(executable, options("archive.bin", "zip") +
                                                      std::vector<std::string>{"input"}),
                       0);
                expect(fixture.Invoke(executable, options("folder.zip") +
                                                      std::vector<std::string>{"archive.bin"}),
                       0);
                require(Read(fixture.Path("folder.zip/" + std::string(OPAQUE))) == "bytes",
                        "ZIP round trip failed");
                fixture.Clean();
            },
        },
        {
            "attached and equal-sign options preserve the export contract",
            [executable]
            {
                const auto fixture = Fixture{};
                fixture.Pack("input", {{std::string(OPAQUE), "bytes"}});
                for (const auto& output : {
                         std::vector<std::string>{"-ofirst"},
                         std::vector<std::string>{"-o", "second"},
                         std::vector<std::string>{"--output=third"},
                     })
                {
                    expect(fixture.Invoke(executable,
                                          std::vector<std::string>{
                                              "--type=resource",
                                              "--target=34",
                                              "--output-format=directory",
                                              "--description",
                                              "",
                                          } + output +
                                              std::vector<std::string>{"input"}),
                           0);
                }
                for (const auto* output : {"first", "second", "third"})
                {
                    require(Read(fixture.Path(std::string(output) + '/' + std::string(OPAQUE))) ==
                                "bytes",
                            "Attached output path changed");
                    require(Json::parse(Read(fixture.Path(std::string(output) + "/pack.mcmeta")))
                                .at("pack")
                                .at("description")
                                .get_ref<const std::string&>()
                                .empty(),
                            "Empty description changed");
                }
            },
        },
        {
            "Unicode paths and plain text descriptions survive process boundary",
            [executable]
            {
                const auto fixture = Fixture{};
                const auto input = std::string("Вход с пробелами 世界 😀");
                const auto output = std::string("Результат 世界 😀.zip");
                const auto description = std::string("Пак 世界 \"text\"\nновая строка");
                fixture.Pack(input, {{std::string(OPAQUE), "unicode"}});
                expect(fixture.Invoke(executable, options(output, "zip") +
                                                      std::vector<std::string>{
                                                          "--description",
                                                          description,
                                                          input,
                                                      }),
                       0);
                const auto pack = take(ResolveStack({
                    .PacksHighToLow = {(fixture.Path(output))},
                    .Type = PackType::RESOURCE,
                    .Target = {.Major = 34, .Minor = 0},
                    .Limits = {},
                    .Rules = {},
                }));
                require(take(pack.Files.at(std::string(OPAQUE)).Read(100)) == "unicode",
                        "Unicode input lost");
                expect(fixture.Invoke(executable,
                                      options("unpacked") + std::vector<std::string>{output}),
                       0);
                require(Json::parse(Read(fixture.Path("unpacked/pack.mcmeta")))
                                .at("pack")
                                .at("pack_format") == 34,
                        "Unicode ZIP failed to reopen");
                expect(fixture.Invoke(executable, options("described") +
                                                      std::vector<std::string>{
                                                          "--description",
                                                          description,
                                                          input,
                                                      }),
                       0);
                require(Json::parse(Read(fixture.Path("described/pack.mcmeta")))
                                .at("pack")
                                .at("description") == description,
                        "Description bytes changed");
            },
        },
        {
            "process arguments preserve quotes and trailing backslashes",
            [executable]
            {
                const auto fixture = Fixture{};
                fixture.Pack("input");
                auto index = std::size_t{0};
                for (const auto* description :
                     {"", "\\", "text\\\\", "slash\\\"quote", "spaces\tand\n\"quotes\"\\"})
                {
                    const auto output = "quoted-" + std::to_string(index++);
                    expect(fixture.Invoke(executable, options(output) +
                                                          std::vector<std::string>{
                                                              "--description",
                                                              description,
                                                              "input",
                                                          }),
                           0);
                    require(Json::parse(Read(fixture.Path(output + "/pack.mcmeta")))
                                    .at("pack")
                                    .at("description") == description,
                            "Process argument bytes changed");
                }
            },
        },
        {
            "double dash permits input names starting with dash",
            [executable]
            {
                const auto fixture = Fixture{};
                fixture.Pack("-input", {{std::string(OPAQUE), "dash"}});
                expect(fixture.Invoke(executable,
                                      options() + std::vector<std::string>{"--", "-input"}),
                       0);
                require(Read(fixture.Path("result/" + std::string(OPAQUE))) == "dash",
                        "Dash path failed");
            },
        },
        {
            "data pack mode uses tag merging",
            [executable]
            {
                const auto fixture = Fixture{};
                const auto tag = std::string("data/example/tags/function/load.json");
                fixture.Pack("low", {{tag, R"({"values":["example:low"]})"}}, 48);
                fixture.Pack("high", {{tag, R"({"values":["example:high"]})"}}, 48);
                auto arguments = options();
                arguments.at(1) = "data";
                arguments.at(3) = "48";
                expect(
                    fixture.Invoke(executable, arguments + std::vector<std::string>{"high", "low"}),
                    0);
                require(Json::parse(Read(fixture.Path("result/" + tag))).at("values") ==
                            Json::array({"example:low", "example:high"}),
                        "Data tag ordering changed");
            },
        },
        {
            "modern minor format exports exact metadata",
            [executable]
            {
                const auto fixture = Fixture{};
                const auto input = fixture.Pack("input");
                Write(input + "/pack.mcmeta",
                      R"({"pack":{"min_format":[65,2],"max_format":[65,2],"description":"Test"}})");
                auto arguments = options();
                arguments.at(3) = "65.2";
                expect(fixture.Invoke(executable, arguments + std::vector<std::string>{"input"}),
                       0);
                const auto metadata = Json::parse(Read(fixture.Path("result/pack.mcmeta")));
                require(metadata.at("pack").at("min_format") == Json::array({65, 2}) &&
                            metadata.at("pack").at("max_format") == Json::array({65, 2}),
                        "Minor target lost");
            },
        },
        {
            "resolve diagnostics contain symbolic error and source context",
            [executable]
            {
                const auto fixture = Fixture{};
                expect(fixture.Invoke(executable, options() + std::vector<std::string>{"missing"}),
                       3, "IO");
                const auto input = fixture.Pack("invalid");
                Write(input + "/pack.mcmeta", "{");
                const auto result =
                    fixture.Invoke(executable, options() + std::vector<std::string>{"invalid"});
                expect(result, 3, "INVALID_METADATA");
                require(result.Error.contains("pack.mcmeta") && result.Error.contains("invalid"),
                        "Diagnostic lost context");
                require(!exists(fixture.Path("result")), "Resolve error created output");
            },
        },
        {
            "incompatible format and unsupported metadata remain explicit errors",
            [executable]
            {
                const auto fixture = Fixture{};
                fixture.Pack("old", {}, 4);
                expect(fixture.Invoke(executable, options() + std::vector<std::string>{"old"}), 3,
                       "INCOMPATIBLE_FORMAT");
                const auto source = fixture.Pack("unsupported");
                Write(source + "/pack.mcmeta",
                      R"({"pack":{"pack_format":34,"description":"Test"},"features":{}})");
                expect(
                    fixture.Invoke(executable, options() + std::vector<std::string>{"unsupported"}),
                    3, "UNSUPPORTED_FEATURE");
            },
        },
        {
            "existing file and directory are never overwritten",
            [executable]
            {
                const auto fixture = Fixture{};
                fixture.Pack("input");
                Write(fixture.Path("existing.zip"), "keep-file");
                Write(fixture.Path("existing/marker"), "keep-directory");
                expect(fixture.Invoke(executable, options("existing.zip", "zip") +
                                                      std::vector<std::string>{"input"}),
                       4, "OUTPUT_EXISTS");
                expect(fixture.Invoke(executable,
                                      options("existing") + std::vector<std::string>{"input"}),
                       4, "OUTPUT_EXISTS");
                require(Read(fixture.Path("existing.zip")) == "keep-file" &&
                            Read(fixture.Path("existing/marker")) == "keep-directory",
                        "Existing output changed");
                fixture.Clean();
            },
        },
        {
            "output inside source is rejected without modifying input",
            [executable]
            {
                const auto fixture = Fixture{};
                fixture.Pack("input", {{std::string(OPAQUE), "keep"}});
                expect(fixture.Invoke(executable,
                                      options("input/output") + std::vector<std::string>{"input"}),
                       4, "INVALID_PATH");
                require(!exists(fixture.Path("input/output")) &&
                            Read(fixture.Path("input/" + std::string(OPAQUE))) == "keep",
                        "Source modified");
                fixture.Clean();
            },
        },
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
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
