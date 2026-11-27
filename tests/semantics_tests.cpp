#include "core/filesystem.hpp"
#include "core/filters.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "testing/fixtures.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    using PackStacker::ErrorCode;
    using PackStacker::ExportPack;
    using PackStacker::FilterPattern;
    using PackStacker::InspectPack;
    using PackStacker::Limits;
    using PackStacker::OutputFormat;
    using PackStacker::PackType;
    using PackStacker::ResolvedPack;
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
            throw std::runtime_error(result.error().Message + " [" + result.error().Path + "]");
        }
        return std::move(*result);
    }

    void take(Result<void> result)
    {
        if (!result)
        {
            throw std::runtime_error(result.error().Message + " [" + result.error().Path + "]");
        }
    }

    template <typename T>
    void expectError(const Result<T>& result, ErrorCode code)
    {
        require(!result, "Expected an error");
        if (result.error().Code != code)
        {
            throw std::runtime_error("Unexpected error: " + result.error().Message);
        }
    }

    std::string encode(const Json& value)
    {
        return std::string(value.dump());
    }

    using PackStacker::Testing::Read;
    using PackStacker::Testing::Write;

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
                         Json extra = Json::object(), int32_t format = 34) const
        {
            const auto root = Path(name);
            extra.update(Json{{"pack", {{"pack_format", format}, {"description", "Test pack"}}}});
            Write(root + "/pack.mcmeta", encode(extra));
            for (const auto& [relative, bytes] : files)
            {
                Write((root + '/').append(relative), bytes);
            }
            return root;
        }
    };

    ResolvedPack resolve(const std::vector<std::string>& paths, PackType type = PackType::RESOURCE,
                         int32_t format = 34, Limits limits = {})
    {
        auto corePaths = std::vector<std::string>{};
        for (const auto& path : paths)
        {
            corePaths.push_back(path);
        }
        return take(ResolveStack({
            .PacksHighToLow = std::move(corePaths),
            .Type = type,
            .Target = {.Major = format, .Minor = 0},
            .Limits = limits,
            .Rules = {},
        }));
    }

    Json content(const ResolvedPack& pack, const std::string& path)
    {
        const auto found = pack.Files.find(path);
        require(found != pack.Files.cend(), "Missing resource");
        return Json::parse(take(found->second.Read(std::uint64_t{16} * 1024 * 1024)));
    }

    Json filter(const Json& blocks)
    {
        return Json{{"filter", {{"block", blocks}}}};
    }

    Json oneFilter(const std::string& path, const std::string& name = "^example$")
    {
        return filter(Json::array({{{"namespace", name}, {"path", path}}}));
    }

    Json registerSound(Json state, const Json& registration)
    {
        if (state.is_null() || registration.value("replace", false))
        {
            state = {
                {
                    "subtitle",
                    registration.contains("subtitle") ? registration.at("subtitle") : Json(nullptr),
                },
                {"sounds", Json::array()},
            };
        }
        if (registration.contains("sounds"))
        {
            for (const auto& sound : registration.at("sounds"))
            {
                state.at("sounds").push_back(sound);
            }
        }
        return state;
    }

    constexpr auto SOUNDS = std::string_view("assets/example/sounds.json");
    constexpr auto LANGUAGE = std::string_view("assets/example/lang/en_us.json");
    constexpr auto TEXTURE = std::string_view("assets/example/textures/block/test.png");

    void testSoundsAppendAndPreserveWeightsReferencesAndDuplicates()
    {
        const auto fixture = Fixture{};
        const auto sound = Json{
            {"name", "example:reference"},
            {"type", "event"},
            {"weight", 7},
            {"volume", 0.5},
            {"pitch", 1.2},
            {"stream", true},
            {"preload", true},
            {"attenuation_distance", 0},
        };
        const auto low = fixture.Pack(
            "low", {
                       {
                           std::string(SOUNDS),
                           encode(Json{
                               {"ambient", {{"subtitle", "sub.low"}, {"sounds", {"example:a"}}}},
                               {"only.low", Json::object()},
                           }),
                       },
                   });
        const auto high =
            fixture.Pack("high", {
                                     {
                                         std::string(SOUNDS),
                                         encode(Json{
                                             {
                                                 "ambient",
                                                 {
                                                     {"subtitle", "sub.ignored"},
                                                     {"sounds", Json::array({sound, "example:a"})},
                                                 },
                                             },
                                             {"only.high", Json::object()},
                                         }),
                                     },
                                 });
        const auto merged = content(resolve({high, low}), std::string(SOUNDS));
        require(merged.at("ambient").at("sounds") == Json::array({"example:a", sound, "example:a"}),
                "Sound list changed");
        require(merged.at("ambient").at("subtitle") == "sub.low", "Appending replaced subtitle");
        require(merged.contains("only.low") && merged.contains("only.high"), "Unique event lost");
    }

    void testSoundResetIsStickyAndChoosesTheResetSubtitle()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack(
            "low", {
                       {
                           std::string(SOUNDS),
                           R"({"ambient":{"sounds":["example:discarded"],"subtitle":"sub.old"}})",
                       },
                   });
        const auto reset = fixture.Pack(
            "reset",
            {
                {
                    std::string(SOUNDS),
                    R"({"ambient":{"replace":true,"sounds":["example:reset"],"subtitle":"sub.reset"}})",
                },
            });
        const auto high = fixture.Pack(
            "high",
            {
                {
                    std::string(SOUNDS),
                    R"({"ambient":{"replace":false,"sounds":["example:top"],"subtitle":"sub.ignored"}})",
                },
            });
        const auto event = content(resolve({high, reset, low}), std::string(SOUNDS)).at("ambient");
        require(event.at("replace") == true && event.at("subtitle") == "sub.reset",
                "Reset state lost");
        require(event.at("sounds") == Json::array({"example:reset", "example:top"}),
                "Reset retained lower sounds");
    }

    void testSoundCompositionAgreesWithRegistrationModelForExternalBases()
    {
        const auto fixture = Fixture{};
        const auto registrations = std::vector<Json>{
            Json{{"sounds", {"example:a"}}, {"subtitle", "sub.first"}},
            Json{{"replace", false}, {"sounds", {"example:b"}}, {"subtitle", "sub.second"}},
            Json{{"replace", true}},
            Json{{"replace", true}, {"sounds", {"example:d"}}, {"subtitle", "sub.reset"}},
            Json::object(),
        };
        const auto bases = std::vector<Json>{
            Json(nullptr),
            Json{{"sounds", {"external:base"}}, {"subtitle", "sub.external"}},
        };
        for (const auto& first : registrations)
        {
            for (const auto& second : registrations)
            {
                for (const auto& third : registrations)
                {
                    const auto low = fixture.Pack(
                        "low", {{std::string(SOUNDS), encode(Json{{"ambient", first}})}});
                    const auto middle = fixture.Pack(
                        "middle", {{std::string(SOUNDS), encode(Json{{"ambient", second}})}});
                    const auto high = fixture.Pack(
                        "high", {{std::string(SOUNDS), encode(Json{{"ambient", third}})}});
                    const auto combined =
                        content(resolve({high, middle, low}), std::string(SOUNDS)).at("ambient");
                    for (const auto& base : bases)
                    {
                        const auto stacked =
                            registerSound(registerSound(registerSound(base, first), second), third);
                        require(stacked == registerSound(base, combined),
                                "Sound composition changed external-base semantics");
                    }
                }
            }
        }
    }

    void testSoundExportPreservesUnfilteredExternalBaseRegistration()
    {
        const auto fixture = Fixture{};
        const auto external = fixture.Pack(
            "external",
            {
                {
                    std::string(SOUNDS),
                    R"({"ambient":{"sounds":["external:base"],"subtitle":"sub.external"}})",
                },
            });
        const auto low = fixture.Pack(
            "low", {
                       {
                           std::string(SOUNDS),
                           R"({"ambient":{"sounds":["example:low"],"subtitle":"sub.low"}})",
                       },
                   });
        const auto high = fixture.Pack(
            "high", {
                        {
                            std::string(SOUNDS),
                            R"({"ambient":{"sounds":["example:high"],"subtitle":"sub.ignored"}})",
                        },
                    });
        const auto combined = resolve({high, low});
        const auto output = fixture.Path("combined.zip");
        take(ExportPack(combined, {
                                      .Path = output,
                                      .Format = OutputFormat::ZIP,
                                      .Description = "test",
                                      .Limits = {},
                                  }));
        require(content(resolve({high, low, external}), std::string(SOUNDS)) ==
                    content(resolve({output, external}), std::string(SOUNDS)),
                "Sound export changed unfiltered external-base registration");
    }

    void testSoundOverlayReplacesTheWholeBaseFile()
    {
        const auto fixture = Fixture{};
        const auto extra = Json{
            {
                "overlays",
                {{"entries", Json::array({{{"directory", "overlay"}, {"formats", 34}}})}},
            },
        };
        const auto path = fixture.Pack(
            "pack",
            {
                {std::string(SOUNDS), R"({"base":{"sounds":["example:base"]}})"},
                {"overlay/" + std::string(SOUNDS), R"({"overlay":{"sounds":["example:overlay"]}})"},
            },
            extra);
        const auto sounds = content(resolve({path}), std::string(SOUNDS));
        require(!sounds.contains("base") && sounds.contains("overlay"),
                "Overlay merged sound files within one pack");
    }

    void testInvalidSoundShapesAndNumericBounds()
    {
        const auto fixture = Fixture{};
        const auto values = std::vector<Json>{
            Json::array(),
            Json{{"ambient", Json::array()}},
            Json{{"ambient", {{"replace", "true"}}}},
            Json{{"ambient", {{"subtitle", nullptr}}}},
            Json{{"ambient", {{"sounds", "not-array"}}}},
            Json{{"Bad:Event", Json::object()}},
            Json{{"ambient", {{"sounds", Json::array({42})}}}},
        };
        for (const auto& value : values)
        {
            const auto path = fixture.Pack("invalid", {{std::string(SOUNDS), encode(value)}});
            expectError(ResolveStack({
                            .PacksHighToLow = {path},
                            .Type = PackType::RESOURCE,
                            .Target = {.Major = 34, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        }),
                        ErrorCode::INVALID_RESOURCE);
        }
        const auto entries = std::vector<Json>{
            Json{{"name", "Bad:Sound"}},
            Json{{"name", "example:a"}, {"volume", 0}},
            Json{{"name", "example:a"}, {"pitch", -1}},
            Json{{"name", "example:a"}, {"weight", 0}},
            Json{{"name", "example:a"}, {"weight", 1.5}},
            Json{{"name", "example:a"}, {"weight", 2147483648ULL}},
            Json{{"name", "example:a"}, {"stream", "true"}},
            Json{{"name", "example:a"}, {"preload", 1}},
            Json{{"name", "example:a"}, {"attenuation_distance", -2147483649LL}},
            Json{{"name", "example:a"}, {"type", false}},
        };
        for (const auto& entry : entries)
        {
            const auto path = fixture.Pack(
                "invalid", {
                               {
                                   std::string(SOUNDS),
                                   encode(Json{{"ambient", {{"sounds", Json::array({entry})}}}}),
                               },
                           });
            expectError(ResolveStack({
                            .PacksHighToLow = {path},
                            .Type = PackType::RESOURCE,
                            .Target = {.Major = 34, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        }),
                        ErrorCode::INVALID_RESOURCE);
        }
    }

    void testSoundDiagnosticsPreserveValidationOrder()
    {
        const auto fixture = Fixture{};
        const auto cases = std::vector<std::tuple<const char*, ErrorCode, const char*>>{
            {
                R"({"ambient":{"replace":0,"custom":true}})",
                ErrorCode::INVALID_RESOURCE,
                "Invalid sound event: ambient",
            },
            {
                R"({"ambient":{"custom":true,"sounds":[42]}})",
                ErrorCode::UNSUPPORTED_FEATURE,
                "Unknown sound event field",
            },
            {
                R"({"ambient":{"sounds":[{"name":false,"custom":true}]}})",
                ErrorCode::INVALID_RESOURCE,
                "Invalid sound entry",
            },
            {
                R"({"ambient":{"sounds":[{"name":"example:a","custom":true,"type":false}]}})",
                ErrorCode::UNSUPPORTED_FEATURE,
                "Unknown sound entry field",
            },
            {
                R"({"ambient":{"sounds":[{"name":"example:a","type":"custom","volume":0}]}})",
                ErrorCode::UNSUPPORTED_FEATURE,
                "Non-vanilla sound entry type",
            },
            {
                R"({"ambient":{"sounds":[{"name":"example:a","volume":0,"weight":0}]}})",
                ErrorCode::INVALID_RESOURCE,
                "Invalid positive sound parameter",
            },
            {
                R"({"ambient":{"sounds":[{"name":"example:a","weight":0,"stream":0}]}})",
                ErrorCode::INVALID_RESOURCE,
                "Invalid integer sound parameter",
            },
        };
        for (const auto& [source, code, message] : cases)
        {
            const auto path = fixture.Pack("invalid", {{std::string(SOUNDS), std::string(source)}});
            const auto result = ResolveStack({
                .PacksHighToLow = {path},
                .Type = PackType::RESOURCE,
                .Target = {.Major = 34, .Minor = 0},
                .Limits = {},
                .Rules = {},
            });
            expectError(result, code);
            require(result.error().Message == message, "Unexpected sound diagnostic");
            require(result.error().Pack == path &&
                        result.error().Path == std::string_view(std::string(SOUNDS).data(),
                                                                std::string(SOUNDS).size()),
                    "Unexpected sound diagnostic location");
        }
    }

    void testUnknownSoundExtensionsAreExplicitErrors()
    {
        const auto fixture = Fixture{};
        for (const auto& value : {
                 Json{{"ambient", {{"custom", true}}}},
                 Json{
                     {
                         "ambient",
                         {
                             {
                                 "sounds",
                                 Json::array({{{"name", "example:a"}, {"custom", true}}}),
                             },
                         },
                     },
                 },
                 Json{
                     {
                         "ambient",
                         {
                             {
                                 "sounds",
                                 Json::array({{{"name", "example:a"}, {"type", "custom"}}}),
                             },
                         },
                     },
                 },
             })
        {
            const auto path = fixture.Pack("pack", {{std::string(SOUNDS), encode(value)}});
            expectError(ResolveStack({
                            .PacksHighToLow = {path},
                            .Type = PackType::RESOURCE,
                            .Target = {.Major = 34, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        }),
                        ErrorCode::UNSUPPORTED_FEATURE);
        }
    }

    void testLanguageDefinitionsAreFirstWinsWhileTranslationsAreLastWins()
    {
        const auto fixture = Fixture{};
        const auto lowLanguage = Json{{"name", "Low name"}, {"region", "Low region"}};
        const auto highLanguage =
            Json{{"name", "High name"}, {"region", "High region"}, {"bidirectional", true}};
        const auto low = fixture.Pack("low", {{std::string(LANGUAGE), R"({"a":"low"})"}},
                                      Json{{"language", {{"en_us", lowLanguage}}}});
        const auto high =
            fixture.Pack("high", {{std::string(LANGUAGE), R"({"a":"high"})"}},
                         Json{{"language", {{"en_us", highLanguage}, {"custom", highLanguage}}}});
        const auto merged = resolve({high, low});
        require(merged.Languages.at("en_us").Name == "Low name" &&
                    !merged.Languages.at("en_us").Bidirectional,
                "Language metadata priority changed");
        require(merged.Languages.at("custom").Bidirectional, "Unique RTL language lost");
        require(content(merged, std::string(LANGUAGE)).at("a") == "high",
                "Metadata priority leaked into translation priority");
    }

    void testLanguageMetadataRoundTripsThroughFolderAndZIP()
    {
        const auto fixture = Fixture{};
        const auto language = Json{{"name", "Язык"}, {"region", "Регион"}, {"bidirectional", true}};
        const auto source = fixture.Pack("source", {}, Json{{"language", {{"custom", language}}}});
        const auto merged = resolve({source});
        for (const auto format : {OutputFormat::DIRECTORY, OutputFormat::ZIP})
        {
            const auto output = fixture.Path(format == OutputFormat::ZIP ? "result.zip" : "result");
            take(ExportPack(merged, {
                                        .Path = output,
                                        .Format = format,
                                        .Description = "test",
                                        .Limits = {},
                                    }));
            const auto roundTrip = resolve({output});
            require(roundTrip.Languages.at("custom").Name == "Язык", "Language name changed");
            require(roundTrip.Languages.at("custom").Bidirectional, "RTL flag lost");
        }
    }

    void testLanguageMetadataValidationAndDataPackBoundary()
    {
        const auto fixture = Fixture{};
        for (const auto& languages : {
                 Json::array(),
                 Json{{"custom", Json::object()}},
                 Json{{"custom", {{"name", ""}, {"region", "Region"}}}},
                 Json{
                     {
                         "custom",
                         {
                             {"name", "Name"},
                             {"region", "Region"},
                             {"bidirectional", "true"},
                         },
                     },
                 },
                 Json{{"UPPER", {{"name", "Name"}, {"region", "Region"}}}},
                 Json{{"too_long_language_code", {{"name", "Name"}, {"region", "Region"}}}},
             })
        {
            const auto path = fixture.Pack("pack", {}, Json{{"language", languages}});
            expectError(InspectPack(path), ErrorCode::INVALID_METADATA);
        }
        const auto language = Json{{"custom", {{"name", "Name"}, {"region", "Region"}}}};
        const auto data = fixture.Pack("data", {}, Json{{"language", language}}, 48);
        expectError(ResolveStack({
                        .PacksHighToLow = {data},
                        .Type = PackType::DATA,
                        .Target = {.Major = 48, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::UNSUPPORTED_FEATURE);
        const auto extension = fixture.Pack(
            "extension", {},
            Json{
                {
                    "language",
                    {
                        {
                            "custom",
                            {{"name", "Name"}, {"region", "Region"}, {"fallback", "en_us"}},
                        },
                    },
                },
            });
        expectError(InspectPack(extension), ErrorCode::UNSUPPORTED_FEATURE);
    }

    void testMetadataAndSoundMergeLimitsAreEnforced()
    {
        const auto fixture = Fixture{};
        auto languages = Json(Json::object());
        for (const auto* code : {"one", "two", "three"})
        {
            languages.emplace(code, Json{{"name", "Name"}, {"region", "Region"}});
        }
        const auto languagePack = fixture.Pack("languages", {}, Json{{"language", languages}});
        auto limits = Limits{};
        limits.MaxEntries = 2;
        expectError(InspectPack(languagePack, limits), ErrorCode::LIMIT_EXCEEDED);
        const auto name = std::string("example:" + std::string(100, 'a'));
        const auto low = fixture.Pack(
            "low", {{std::string(SOUNDS), encode(Json{{"ambient", {{"sounds", {name}}}}})}});
        const auto high = fixture.Pack(
            "high", {{std::string(SOUNDS), encode(Json{{"ambient", {{"sounds", {name}}}}})}});
        limits = {};
        limits.MaxJsonBytes = 200;
        expectError(ResolveStack({
                        .PacksHighToLow = {high, low},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 34, .Minor = 0},
                        .Limits = limits,
                        .Rules = {},
                    }),
                    ErrorCode::LIMIT_EXCEEDED);
    }

    void testOverlayRootMetadataIsIgnored()
    {
        const auto fixture = Fixture{};
        const auto extra = Json{
            {"language", {{"custom", {{"name", "Root"}, {"region", "Region"}}}}},
            {
                "overlays",
                {{"entries", Json::array({{{"directory", "layer"}, {"formats", 34}}})}},
            },
        };
        const auto source = fixture.Pack(
            "pack",
            {
                {std::string(TEXTURE), "png"},
                {
                    "layer/pack.mcmeta",
                    R"({"filter":{"block":[{}]},"language":{"custom":{"name":"Ignored","region":"Ignored"}}})",
                },
            },
            extra);
        const auto merged = resolve({source});
        require(merged.Languages.at("custom").Name == "Root" &&
                    merged.Files.contains(std::string(TEXTURE)),
                "Overlay metadata affected root metadata or filters");
    }

    void testExportRevalidatesPublicLanguageAndFilterMetadata()
    {
        const auto fixture = Fixture{};
        auto merged = resolve({fixture.Pack("pack", {}, oneFilter(".*"))});
        const auto output = fixture.Path("invalid-output.zip");
        require(!merged.Filters.empty(), "Missing root filter");
        merged.Filters.front().PathPattern = "[a-z]";
        expectError(ExportPack(merged,
                               {
                                   .Path = output,
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::UNSUPPORTED_FEATURE);
        merged.Filters.clear();
        merged.Languages.insert_or_assign("custom", PackStacker::LanguageDefinition{
                                                        .Name = "",
                                                        .Region = "Region",
                                                        .Bidirectional = false,
                                                    });
        expectError(ExportPack(merged,
                               {
                                   .Path = output,
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::INVALID_METADATA);
        require(!exists(output), "Invalid metadata published output");
    }

    void testExportRejectsInvalidUtf8Metadata()
    {
        const auto fixture = Fixture{};
        auto merged = resolve({fixture.Pack("pack", {})});
        const auto output = fixture.Path("invalid-utf8.zip");
        const auto request = PackStacker::ExportRequest{
            .Path = output,
            .Format = OutputFormat::ZIP,
            .Description = "test",
            .Limits = {},
        };
        const auto definitions = std::vector<PackStacker::LanguageDefinition>{
            {.Name = "\xed\xa0\x80", .Region = "Region", .Bidirectional = false},
            {.Name = "Name", .Region = "\xc0\xaf", .Bidirectional = false},
        };
        for (const auto& definition : definitions)
        {
            merged.Languages.insert_or_assign("custom", definition);
            expectError(ExportPack(merged, request), ErrorCode::INVALID_METADATA);
        }
        merged.Languages.clear();
        merged.Filters.push_back({
            .NamespacePattern = "\xed\xa0\x80",
            .PathPattern = ".*",
        });
        expectError(ExportPack(merged, request), ErrorCode::UNSUPPORTED_FEATURE);
        require(!exists(output), "Invalid UTF-8 metadata published output");
    }

    void testPortableFilterRegexFixturesAgreeWithJavaGoldenResults()
    {
        auto fixture = std::ifstream(
            PackStacker::PathFromUtf8(std::string(PACKSTACKER_TEST_DATA_DIR) + "/filter_regex.tsv"),
            std::ios::binary);
        require(fixture.is_open(), "Cannot open regex fixtures");
        auto line = std::string{};
        while (std::getline(fixture, line))
        {
            if (line.empty() || line.starts_with("pattern\t"))
            {
                continue;
            }
            const auto first = line.find('\t');
            const auto second = line.find('\t', first + 1);
            require(first != std::string::npos && second != std::string::npos,
                    "Invalid regex fixture");
            const auto pattern = take(FilterPattern::Compile(line.substr(0, first), {}, {}));
            auto budget = std::uint64_t{1000000};
            require(take(pattern.Matches(line.substr(first + 1, second - first - 1), budget, {},
                                         {})) == (line.substr(second + 1) == "true"),
                    "Regex fixture mismatch");
        }
        require(fixture.eof() && !fixture.bad(), "Cannot read regex fixtures");
        auto budget = std::uint64_t{1000000};
        const auto supplementary = std::string("😀");
        const auto oneUnit = take(FilterPattern::Compile("^.$", {}, {}));
        const auto twoUnits = take(FilterPattern::Compile("^..$", {}, {}));
        require(!take(oneUnit.Matches(supplementary, budget, {}, {})) &&
                    take(twoUnits.Matches(supplementary, budget, {}, {})),
                "Filter wildcard UTF-16 semantics changed");
        const auto emptyBranch = take(FilterPattern::Compile("^absent$|", {}, {}));
        require(take(emptyBranch.Matches("other", budget, {}, {})),
                "Trailing empty filter alternative was lost");
    }

    void testFilterRemovesLowerResourcesAndStaleMetadataButNotRootFiles()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack("low", {
                                                 {std::string(TEXTURE), "png"},
                                                 {std::string(TEXTURE) + ".mcmeta", "meta"},
                                                 {"assets/other/textures/block/test.png", "other"},
                                                 {"NOTICE", "notice"},
                                                 {"pack.png", "icon"},
                                             });
        const auto high = fixture.Pack("high", {}, oneFilter("^textures/block/test\\.png$"));
        const auto merged = resolve({high, low});
        require(!merged.Files.contains(std::string(TEXTURE)) &&
                    !merged.Files.contains((std::string(TEXTURE) + ".mcmeta")),
                "Filtered texture or stale metadata survived");
        require(merged.Files.contains("assets/other/textures/block/test.png") &&
                    merged.Files.contains("NOTICE") && merged.Files.contains("pack.png"),
                "Filter removed unrelated resources");
        require(merged.Filters.size() == 1, "External filter effect was discarded");
    }

    void testFilterDoesNotApplyToItsOwnResourcesOrMetadata()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack(
            "low", {{std::string(TEXTURE), "low"}, {std::string(TEXTURE) + ".mcmeta", "old"}});
        const auto high = fixture.Pack(
            "high", {{std::string(TEXTURE), "high"}, {std::string(TEXTURE) + ".mcmeta", "new"}},
            oneFilter("^textures/"));
        const auto merged = resolve({high, low});
        require(merged.Files.contains(std::string(TEXTURE)) &&
                    merged.Files.contains((std::string(TEXTURE) + ".mcmeta")),
                "Self-filtered resource lost");
        require(take(merged.Files.at(std::string(TEXTURE)).Read(1024)) == "high",
                "Lower texture survived");
    }

    void testMetadataOnlyFilterKeepsTheMainResource()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack(
            "low", {{std::string(TEXTURE), "png"}, {std::string(TEXTURE) + ".mcmeta", "old"}});
        const auto high = fixture.Pack("high", {}, oneFilter("\\.mcmeta$"));
        const auto merged = resolve({high, low});
        require(merged.Files.contains(std::string(TEXTURE)) &&
                    !merged.Files.contains((std::string(TEXTURE) + ".mcmeta")),
                "Metadata filter changed main resource");
    }

    void testFiltersUseFindSemanticsAndResourceRelativePaths()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack("low", {{std::string(TEXTURE), "png"}});
        const auto substring = fixture.Pack("substring", {}, oneFilter("block/", "ample"));
        require(!resolve({substring, low}).Files.contains(std::string(TEXTURE)),
                "Regex incorrectly anchored to the full resource");
        const auto wrongRoot = fixture.Pack("wrong-root", {}, oneFilter("^assets/"));
        require(resolve({wrongRoot, low}).Files.contains(std::string(TEXTURE)),
                "Filter path included assets root");
    }

    void testOmittedNamespaceAndPathFieldsDefaultToAll()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack("low", {{std::string(TEXTURE), "png"}, {"NOTICE", "notice"}});
        const auto high = fixture.Pack("high", {}, filter(Json::array({Json::object()})));
        const auto merged = resolve({high, low});
        require(!merged.Files.contains(std::string(TEXTURE)) && merged.Files.contains("NOTICE"),
                "Default filter did not match all resources");
    }

    void testSharedNamespaceAndSharedPathFilterSetsAreSupported()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack("low", {
                                                 {std::string(TEXTURE), "png"},
                                                 {std::string(LANGUAGE), R"({"a":"value"})"},
                                                 {"assets/other/textures/block/test.png", "other"},
                                             });
        const auto commonNamespace =
            fixture.Pack("same-name", {},
                         filter(Json::array({
                             {{"namespace", "^example$"}, {"path", "^textures/"}},
                             {{"namespace", "^example$"}, {"path", "^lang/"}},
                         })));
        const auto first = resolve({commonNamespace, low});
        require(!first.Files.contains(std::string(TEXTURE)) &&
                    !first.Files.contains(std::string(LANGUAGE)) &&
                    first.Files.contains("assets/other/textures/block/test.png"),
                "Shared namespace filter mismatch");
        const auto commonPath =
            fixture.Pack("same-path", {},
                         filter(Json::array({
                             {{"namespace", "^example$"}, {"path", "^textures/"}},
                             {{"namespace", "^other$"}, {"path", "^textures/"}},
                         })));
        const auto second = resolve({commonPath, low});
        require(!second.Files.contains(std::string(TEXTURE)) &&
                    !second.Files.contains("assets/other/textures/block/test.png") &&
                    second.Files.contains(std::string(LANGUAGE)),
                "Shared path filter mismatch");
    }

    void testAmbiguousFilterSetsRejectBothPerPackAndCrossPackCombinations()
    {
        const auto fixture = Fixture{};
        const auto ambiguous =
            fixture.Pack("ambiguous", {},
                         filter(Json::array({
                             {{"namespace", "^example$"}, {"path", "^textures/"}},
                             {{"namespace", "^other$"}, {"path", "^lang/"}},
                         })));
        expectError(InspectPack(ambiguous), ErrorCode::UNSUPPORTED_FEATURE);
        const auto low = fixture.Pack("low", {}, oneFilter("^textures/", "^example$"));
        const auto high = fixture.Pack("high", {}, oneFilter("^lang/", "^other$"));
        expectError(ResolveStack({
                        .PacksHighToLow = {high, low},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 34, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::UNSUPPORTED_FEATURE);
    }

    void testFilterSemanticsSurviveExportOverAnExternalLowerPack()
    {
        const auto fixture = Fixture{};
        struct Case
        {
            std::string Path;
            std::string External;
            std::string Lower;
            std::string Upper;
            PackType Type;
            int32_t Format;
        };
        const auto cases = std::vector<Case>{
            {
                .Path = std::string(LANGUAGE),
                .External = R"({"external":"old"})",
                .Lower = R"({"lower":"old"})",
                .Upper = R"({"upper":"new"})",
                .Type = PackType::RESOURCE,
                .Format = 34,
            },
            {
                .Path = std::string(SOUNDS),
                .External = R"({"ambient":{"sounds":["external:old"],"subtitle":"sub.external"}})",
                .Lower = R"({"ambient":{"sounds":["example:low"]}})",
                .Upper = R"({"ambient":{"sounds":["example:upper"],"subtitle":"sub.upper"}})",
                .Type = PackType::RESOURCE,
                .Format = 34,
            },
            {
                .Path = "data/example/tags/function/load.json",
                .External = R"({"values":["external:old"]})",
                .Lower = R"({"replace":true,"values":["example:low"]})",
                .Upper = R"({"values":["example:upper"]})",
                .Type = PackType::DATA,
                .Format = 48,
            },
            {
                .Path = "assets/example/font/default.json",
                .External = R"({"providers":[{"type":"reference","id":"external:old"}]})",
                .Lower = R"({"providers":[{"type":"reference","id":"example:low"}]})",
                .Upper = R"({"providers":[{"type":"reference","id":"example:upper"}]})",
                .Type = PackType::RESOURCE,
                .Format = 34,
            },
            {
                .Path = "assets/example/atlases/blocks.json",
                .External =
                    R"({"sources":[{"type":"minecraft:single","resource":"external:old"}]})",
                .Lower = R"({"sources":[{"type":"minecraft:single","resource":"example:low"}]})",
                .Upper = R"({"sources":[{"type":"minecraft:single","resource":"example:upper"}]})",
                .Type = PackType::RESOURCE,
                .Format = 34,
            },
        };
        auto index = std::size_t{0};
        for (const auto& item : cases)
        {
            const auto prefix = std::to_string(index++);
            const auto external = fixture.Pack(prefix + "-external", {{item.Path, item.External}},
                                               Json::object(), item.Format);
            const auto low = fixture.Pack(prefix + "-low", {{item.Path, item.Lower}},
                                          Json::object(), item.Format);
            const auto barrier =
                fixture.Pack(prefix + "-barrier", {}, oneFilter(".*"), item.Format);
            const auto high = fixture.Pack(prefix + "-high", {{item.Path, item.Upper}},
                                           Json::object(), item.Format);
            const auto combined = resolve({high, barrier, low}, item.Type, item.Format);
            const auto original = resolve({high, barrier, low, external}, item.Type, item.Format);
            const auto output = fixture.Path(prefix + "-combined.zip");
            take(ExportPack(combined, {
                                          .Path = output,
                                          .Format = OutputFormat::ZIP,
                                          .Description = "test",
                                          .Limits = {},
                                      }));
            const auto roundTrip = resolve({output, external}, item.Type, item.Format);
            require(content(original, item.Path) == content(roundTrip, item.Path),
                    "External lower contribution reappeared after export");
        }
    }

    void testFilterOnlyOutputHidesResourcesSuppliedOnlyByExternalPacks()
    {
        const auto fixture = Fixture{};
        const auto external = fixture.Pack("external", {{std::string(TEXTURE), "external"}});
        const auto barrier = fixture.Pack("barrier", {}, oneFilter("^textures/"));
        const auto combined = resolve({barrier});
        const auto output = fixture.Path("combined");
        take(ExportPack(combined, {
                                      .Path = output,
                                      .Format = OutputFormat::DIRECTORY,
                                      .Description = "test",
                                      .Limits = {},
                                  }));
        require(!resolve({output, external}).Files.contains(std::string(TEXTURE)),
                "Filter-only output restored external texture");
        const auto metadata = Json::parse(Read(output + "/pack.mcmeta"));
        require(metadata.contains("filter") && metadata.at("filter").at("block").size() == 1,
                "Filter metadata missing");
    }

    void testInvalidFilterShapesAndUnsupportedRegexFailExplicitly()
    {
        const auto fixture = Fixture{};
        for (const auto& value : {
                 Json::object(),
                 Json{{"block", 1}},
                 Json{{"block", Json::array({1})}},
                 Json{{"block", Json::array({{{"path", false}}})}},
             })
        {
            const auto path = fixture.Pack("invalid", {}, Json{{"filter", value}});
            expectError(InspectPack(path), ErrorCode::INVALID_METADATA);
        }
        for (const auto& pattern : {"[a-z]", "(?=textures)", "\\d", "a+", "a?", "*", "(a|b)"})
        {
            const auto path = fixture.Pack("unsupported", {}, oneFilter(pattern));
            expectError(InspectPack(path), ErrorCode::UNSUPPORTED_FEATURE);
        }
        const auto extension = fixture.Pack(
            "extension", {}, filter(Json::array({{{"path", ".*"}, {"custom", true}}})));
        expectError(InspectPack(extension), ErrorCode::UNSUPPORTED_FEATURE);
    }

    void testFilterLimitsCoverPatternSizeAggregateBlocksAndMatchingWork()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack("low", {{std::string(TEXTURE), "png"}});
        const auto high = fixture.Pack("high", {}, oneFilter("^textures/"));
        auto limits = Limits{};
        limits.MaxFilterPatternBytes = 2;
        expectError(InspectPack(high, limits), ErrorCode::LIMIT_EXCEEDED);
        limits = {};
        limits.MaxFilterBlocks = 0;
        expectError(InspectPack(high, limits), ErrorCode::LIMIT_EXCEEDED);
        limits = {};
        limits.MaxFilterMatchSteps = 1;
        expectError(ResolveStack({
                        .PacksHighToLow = {high, low},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 34, .Minor = 0},
                        .Limits = limits,
                        .Rules = {},
                    }),
                    ErrorCode::LIMIT_EXCEEDED);
        const auto other = fixture.Pack("other", {}, oneFilter("^lang/"));
        limits = {};
        limits.MaxFilterBlocks = 1;
        expectError(ResolveStack({
                        .PacksHighToLow = {other, high},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 34, .Minor = 0},
                        .Limits = limits,
                        .Rules = {},
                    }),
                    ErrorCode::LIMIT_EXCEEDED);
    }

    void testEmptyFiltersAreNoOpsAndOldFormatsRejectActiveFilters()
    {
        const auto fixture = Fixture{};
        const auto empty =
            fixture.Pack("empty", {{std::string(TEXTURE), "png"}}, filter(Json::array()), 4);
        require(resolve({empty}, PackType::RESOURCE, 4).Files.contains(std::string(TEXTURE)),
                "Empty filter removed a resource");
        const auto old = fixture.Pack("old", {}, oneFilter(".*"), 4);
        expectError(ResolveStack({
                        .PacksHighToLow = {old},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::UNSUPPORTED_FEATURE);
        const auto merged = resolve({fixture.Pack("valid", {}, oneFilter(".*"))});
        auto altered = merged;
        altered.Target = {.Major = 4, .Minor = 0};
        expectError(ExportPack(altered,
                               {
                                   .Path = fixture.Path("invalid-output"),
                                   .Format = OutputFormat::DIRECTORY,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::UNSUPPORTED_FEATURE);
    }

}

int32_t main()
{
    const auto tests = std::vector<std::pair<const char*, std::function<void()>>>{
        {
            "sounds append and preserve weights, references and duplicates",
            testSoundsAppendAndPreserveWeightsReferencesAndDuplicates,
        },
        {
            "sound reset is sticky and chooses the reset subtitle",
            testSoundResetIsStickyAndChoosesTheResetSubtitle,
        },
        {
            "sound composition agrees with registration model for external bases",
            testSoundCompositionAgreesWithRegistrationModelForExternalBases,
        },
        {
            "sound export preserves unfiltered external-base registration",
            testSoundExportPreservesUnfilteredExternalBaseRegistration,
        },
        {
            "sound overlay replaces the whole base file",
            testSoundOverlayReplacesTheWholeBaseFile,
        },
        {
            "invalid sound shapes and numeric bounds",
            testInvalidSoundShapesAndNumericBounds,
        },
        {
            "sound diagnostics preserve validation order",
            testSoundDiagnosticsPreserveValidationOrder,
        },
        {
            "unknown sound extensions are explicit errors",
            testUnknownSoundExtensionsAreExplicitErrors,
        },
        {
            "language definitions are first-wins while translations are last-wins",
            testLanguageDefinitionsAreFirstWinsWhileTranslationsAreLastWins,
        },
        {
            "language metadata round trips through folder and ZIP",
            testLanguageMetadataRoundTripsThroughFolderAndZIP,
        },
        {
            "language metadata validation and data-pack boundary",
            testLanguageMetadataValidationAndDataPackBoundary,
        },
        {
            "metadata and sound merge limits are enforced",
            testMetadataAndSoundMergeLimitsAreEnforced,
        },
        {
            "overlay root metadata is ignored",
            testOverlayRootMetadataIsIgnored,
        },
        {
            "export revalidates public language and filter metadata",
            testExportRevalidatesPublicLanguageAndFilterMetadata,
        },
        {"export rejects invalid UTF-8 metadata", testExportRejectsInvalidUtf8Metadata},
        {
            "portable filter regex fixtures agree with Java golden results",
            testPortableFilterRegexFixturesAgreeWithJavaGoldenResults,
        },
        {
            "filter removes lower resources and stale metadata but not root files",
            testFilterRemovesLowerResourcesAndStaleMetadataButNotRootFiles,
        },
        {
            "filter does not apply to its own resources or metadata",
            testFilterDoesNotApplyToItsOwnResourcesOrMetadata,
        },
        {
            "metadata-only filter keeps the main resource",
            testMetadataOnlyFilterKeepsTheMainResource,
        },
        {
            "filters use find semantics and resource-relative paths",
            testFiltersUseFindSemanticsAndResourceRelativePaths,
        },
        {
            "omitted namespace and path fields default to all",
            testOmittedNamespaceAndPathFieldsDefaultToAll,
        },
        {
            "shared-namespace and shared-path filter sets are supported",
            testSharedNamespaceAndSharedPathFilterSetsAreSupported,
        },
        {
            "ambiguous filter sets reject both per-pack and cross-pack combinations",
            testAmbiguousFilterSetsRejectBothPerPackAndCrossPackCombinations,
        },
        {
            "filter semantics survive export over an external lower pack",
            testFilterSemanticsSurviveExportOverAnExternalLowerPack,
        },
        {
            "filter-only output hides resources supplied only by external packs",
            testFilterOnlyOutputHidesResourcesSuppliedOnlyByExternalPacks,
        },
        {
            "invalid filter shapes and unsupported regex fail explicitly",
            testInvalidFilterShapesAndUnsupportedRegexFailExplicitly,
        },
        {
            "filter limits cover pattern size, aggregate blocks and matching work",
            testFilterLimitsCoverPatternSizeAggregateBlocksAndMatchingWork,
        },
        {
            "empty filters are no-ops and old formats reject active filters",
            testEmptyFiltersAreNoOpsAndOldFormatsRejectActiveFilters,
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
