#include "core/filesystem.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/rules.hpp"
#include "testing/fixtures.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    using PackStacker::MergeRule;
    using PackStacker::MergeStrategy;

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename T>
    T take(PackStacker::Result<T> value)
    {
        if (!value)
        {
            throw std::runtime_error(value.error().Message);
        }
        return std::move(*value);
    }

    void checkDocuments()
    {
        const auto rules = std::vector<MergeRule>{
            {.Match = "extra/текст-😀/**", .Strategy = MergeStrategy::HIGH_PRIORITY},
            {.Match = "assets/*/lang/*.json", .Strategy = MergeStrategy::LANGUAGE},
        };
        const auto parsed = take(PackStacker::ParseRules(take(PackStacker::SerializeRules(rules))));
        require(parsed.size() == 2 && parsed.front().Match == rules.front().Match &&
                    parsed.back().Strategy == MergeStrategy::LANGUAGE,
                "Rules document changed during round trip");
        for (const auto* invalid : {
                 "{}",
                 R"({"version":2,"rules":[]})",
                 R"({"version":1,"rules":[],"unknown":true})",
                 R"({"version":1,"rules":[{"match":"assets/**","strategy":"unknown"}]})",
                 R"({"version":1,"rules":[{"match":"../**","strategy":"standard"}]})",
                 R"({"version":1,"rules":[{"match":"assets/?","strategy":"standard"}]})",
                 R"({"version":1,"rules":[{"match":"assets/**","strategy":"standard","extra":1}]})",
                 R"({"version":1,"version":1,"rules":[]})",
             })
        {
            require(!PackStacker::ParseRules(invalid), "Invalid rules document accepted");
        }
        auto limits = PackStacker::Limits{};
        limits.MaxFilterBlocks = 1;
        require(!PackStacker::ValidateRules(rules, limits), "Rule count limit ignored");
        limits = {};
        limits.MaxFilterPatternBytes = 1;
        require(!PackStacker::ValidateRules(rules, limits), "Pattern size limit ignored");
        const auto directory = PackStacker::Testing::TemporaryDirectory();
        const auto path = PackStacker::PathToUtf8(directory.Path() / "правила-😀.json");
        require(PackStacker::SaveRules(path, rules).has_value(), "Cannot export rules");
        require(take(PackStacker::LoadRules(path)).size() == rules.size(), "Cannot import rules");
        const auto original = PackStacker::Testing::Read(path);
        const auto duplicate = PackStacker::SaveRules(path, {});
        require(!duplicate && duplicate.error().Code == PackStacker::ErrorCode::OUTPUT_EXISTS,
                "Existing file was not rejected during publication");
        require(PackStacker::Testing::Read(path) == original, "Existing rule bytes were modified");
        const auto missingParent =
            PackStacker::PathToUtf8(directory.Path() / "missing" / "rules.json");
        const auto missing = PackStacker::SaveRules(missingParent, rules);
        require(!missing && missing.error().Code == PackStacker::ErrorCode::IO &&
                    missing.error().Path == missingParent,
                "Rules write error did not identify the destination");
        for (const auto& entry : std::filesystem::directory_iterator(directory.Path()))
        {
            require(entry.path() == PackStacker::PathFromUtf8(path),
                    "Rules export left an unexpected file or staging directory");
        }
        require(!PackStacker::LoadRules(path + ".missing"), "Missing rules file accepted");
        limits.MaxJsonBytes = 1;
        require(!PackStacker::LoadRules(path, limits), "Rules file byte limit ignored");
    }

    void checkGlobs()
    {
        for (const auto& [pattern, path, expected] :
             std::vector<std::tuple<std::string_view, std::string_view, bool>>{
                 {"assets/*/lang/*.json", "assets/test/lang/en_us.json", true},
                 {"assets/*/lang/*.json", "assets/test/nested/lang/en_us.json", false},
                 {"assets/**/lang/*.json", "assets/lang/en_us.json", true},
                 {"assets/**/lang/*.json", "assets/test/nested/lang/en_us.json", true},
                 {"**", "extra/текст-😀/data", true},
                 {"extra/*", "extra/текст-😀", true},
                 {"extra/*", "extra/sub/data", false},
                 {"extra/*.json", "extra/file.json.bak", false},
                 {"extra/a", "extra/A", false},
             })
        {
            const auto rules = std::vector<MergeRule>{
                {.Match = std::string(pattern), .Strategy = MergeStrategy::LOW_PRIORITY},
            };
            auto budget = std::uint64_t{100000};
            require(take(PackStacker::MatchRules(rules, path, budget)) ==
                        (expected ? MergeStrategy::LOW_PRIORITY : MergeStrategy::STANDARD),
                    "Glob matching changed");
        }
        const auto rules = std::vector<MergeRule>{
            {.Match = "**", .Strategy = MergeStrategy::LOW_PRIORITY},
            {.Match = "assets/**", .Strategy = MergeStrategy::STANDARD},
        };
        auto budget = std::uint64_t{1000};
        require(
            take(PackStacker::MatchRules(rules, "assets/test", budget)) == MergeStrategy::STANDARD,
            "Last matching standard rule did not reset custom strategy");
        budget = 0;
        require(!PackStacker::MatchRules(rules, "assets/test", budget), "Match budget ignored");
        const auto cancellation = std::stop_source{};
        cancellation.request_stop();
        budget = 1000;
        const auto cancelled =
            PackStacker::MatchRules(rules, "assets/test", budget, cancellation.get_token());
        require(!cancelled && cancelled.error().Code == PackStacker::ErrorCode::CANCELLED,
                "Rule cancellation ignored");
    }

    void checkMergers()
    {
        const auto directory = PackStacker::Testing::TemporaryDirectory();
        const auto low = PackStacker::PathToUtf8(directory.Path() / "low");
        const auto high = PackStacker::PathToUtf8(directory.Path() / "high");
        for (const auto& path : {low, high})
        {
            PackStacker::Testing::Write(path + "/pack.mcmeta",
                                        R"({"pack":{"pack_format":34,"description":"test"}})");
        }
        auto request = PackStacker::ResolveRequest{
            .PacksHighToLow = {high, low},
            .Type = PackStacker::PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        };
        for (const auto& [strategy, lower, higher, expected] :
             std::vector<std::tuple<MergeStrategy, std::string, std::string, std::string>>{
                 {MergeStrategy::HIGH_PRIORITY, "low", "high", "high"},
                 {MergeStrategy::LOW_PRIORITY, "low", "high", "low"},
                 {
                     MergeStrategy::LANGUAGE,
                     R"({"a":"low","b":"kept"})",
                     R"({"a":"high"})",
                     R"({"a":"high","b":"kept"})",
                 },
                 {
                     MergeStrategy::TAG,
                     R"({"values":["low"]})",
                     R"({"values":["high"]})",
                     R"({"replace":false,"values":["low","high"]})",
                 },
                 {
                     MergeStrategy::TAG,
                     R"({"values":["low"]})",
                     R"({"replace":true,"values":["high"]})",
                     R"({"replace":true,"values":["high"]})",
                 },
                 {
                     MergeStrategy::FONT,
                     R"({"providers":[{"type":"low"}]})",
                     R"({"providers":[{"type":"high"}]})",
                     R"({"providers":[{"type":"high"},{"type":"low"}]})",
                 },
                 {
                     MergeStrategy::ATLAS,
                     R"({"sources":[{"type":"low"}]})",
                     R"({"sources":[{"type":"high"}]})",
                     R"({"sources":[{"type":"low"},{"type":"high"}]})",
                 },
                 {
                     MergeStrategy::SOUNDS,
                     R"({"event":{"sounds":["low"]}})",
                     R"({"event":{"sounds":["high"]}})",
                     R"({"event":{"sounds":["low","high"]}})",
                 },
             })
        {
            PackStacker::Testing::Write(low + "/extra/data", lower);
            PackStacker::Testing::Write(high + "/extra/data", higher);
            request.Rules = {{.Match = "extra/data", .Strategy = strategy}};
            const auto result = take(PackStacker::ResolveStack(request));
            require(take(result.Files.at("extra/data").Read(4096)) == expected,
                    "Custom merger produced wrong content");
        }
        PackStacker::Testing::Write(high + "/extra/data", R"({"wrong":true})");
        request.Rules = {{.Match = "extra/data", .Strategy = MergeStrategy::FONT}};
        const auto invalid = PackStacker::ResolveStack(request);
        require(!invalid && invalid.error().Code == PackStacker::ErrorCode::INVALID_RESOURCE,
                "Invalid custom JSON silently fell back to copying");
        request.Rules = {{.Match = "pack.mcmeta", .Strategy = MergeStrategy::FONT}};
        require(PackStacker::ResolveStack(request).has_value(), "pack.mcmeta was user-merged");
    }

    void checkPriorityAndCompanions()
    {
        const auto directory = PackStacker::Testing::TemporaryDirectory();
        const auto low = PackStacker::PathToUtf8(directory.Path() / "low");
        const auto high = PackStacker::PathToUtf8(directory.Path() / "high");
        for (const auto& path : {low, high})
        {
            PackStacker::Testing::Write(path + "/pack.mcmeta",
                                        R"({"pack":{"pack_format":34,"description":"test"}})");
        }
        PackStacker::Testing::Write(low + "/extra/data", "low");
        PackStacker::Testing::Write(low + "/extra/data.mcmeta", "low-meta");
        PackStacker::Testing::Write(high + "/extra/data", "high");
        PackStacker::Testing::Write(high + "/extra/data.mcmeta", "high-meta");
        PackStacker::Testing::Write(low + "/extra/текст-😀", "low");
        PackStacker::Testing::Write(low + "/extra/текст-😀.mcmeta", "low-meta");
        PackStacker::Testing::Write(high + "/extra/текст-😀.mcmeta", "high-meta");
        PackStacker::Testing::Write(low + "/extra/bare", "bare");
        PackStacker::Testing::Write(high + "/extra/bare.mcmeta", "high-meta");
        PackStacker::Testing::Write(high + "/extra/new", "new");
        PackStacker::Testing::Write(high + "/extra/new.mcmeta", "new-meta");
        PackStacker::Testing::Write(low + "/extra/default", "default");
        PackStacker::Testing::Write(high + "/extra/default.mcmeta", "high-meta");
        const auto result = take(PackStacker::ResolveStack({
            .PacksHighToLow = {high, low},
            .Type = PackStacker::PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules =
                {
                    {.Match = "**", .Strategy = MergeStrategy::LOW_PRIORITY},
                    {.Match = "extra/default", .Strategy = MergeStrategy::STANDARD},
                },
        }));
        require(take(result.Files.at("extra/data").Read(1024)) == "low" &&
                    take(result.Files.at("extra/data.mcmeta").Read(1024)) == "low-meta",
                "Low-priority companion metadata was replaced");
        require(take(result.Files.at("extra/текст-😀").Read(1024)) == "low" &&
                    take(result.Files.at("extra/текст-😀.mcmeta").Read(1024)) == "low-meta",
                "Metadata-only higher pack replaced the chosen file's companion");
        require(!result.Files.contains("extra/bare.mcmeta"),
                "Metadata-only higher pack attached a companion to a chosen file");
        require(take(result.Files.at("extra/new.mcmeta").Read(1024)) == "new-meta",
                "Companion of a newly chosen file was dropped");
        require(take(result.Files.at("extra/default.mcmeta").Read(1024)) == "high-meta",
                "Standard standalone metadata override changed");
    }
}

int32_t main()
{
    try
    {
        checkDocuments();
        checkGlobs();
        checkMergers();
        checkPriorityAndCompanions();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
