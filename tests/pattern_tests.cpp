#include "core/filters.hpp"
#include "core/model.hpp"
#include "core/pattern.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    bool checkMatches()
    {
        struct Example
        {
            std::string_view Pattern;
            std::string_view Value;
            bool Expected;
        };
        constexpr auto examples = std::array{
            Example{.Pattern = "", .Value = "", .Expected = true},
            Example{.Pattern = "", .Value = "a", .Expected = true},
            Example{.Pattern = "^$", .Value = "", .Expected = true},
            Example{.Pattern = "^$", .Value = "a", .Expected = false},
            Example{.Pattern = "abc", .Value = "xabcx", .Expected = true},
            Example{.Pattern = "^abc$", .Value = "xabcx", .Expected = false},
            Example{.Pattern = "^abc", .Value = "abcx", .Expected = true},
            Example{.Pattern = "abc$", .Value = "xabc", .Expected = true},
            Example{.Pattern = "^a.*b$", .Value = "ab", .Expected = true},
            Example{.Pattern = "^a.*b$", .Value = "ax/yb", .Expected = true},
            Example{.Pattern = "^a.*b$", .Value = "ax", .Expected = false},
            Example{.Pattern = "^a\\.b$", .Value = "a.b", .Expected = true},
            Example{.Pattern = "^a\\.b$", .Value = "axb", .Expected = false},
            Example{.Pattern = "^a$|^b$", .Value = "b", .Expected = true},
            Example{.Pattern = "^absent$|", .Value = "other", .Expected = true},
            Example{.Pattern = "^.$", .Value = "😀", .Expected = false},
            Example{.Pattern = "^..$", .Value = "😀", .Expected = true},
            Example{.Pattern = "^.$", .Value = "é", .Expected = true},
            Example{.Pattern = "^.$", .Value = "é", .Expected = false},
            Example{.Pattern = "^..$", .Value = "é", .Expected = true},
            Example{.Pattern = "^.*$", .Value = "текст/😀", .Expected = true},
        };
        for (const auto& example : examples)
        {
            const auto pattern = PackStacker::PortablePattern::Compile(example.Pattern, 1024);
            if (!pattern)
            {
                return false;
            }
            auto budget = std::uint64_t{100000};
            const auto matched = pattern->Matches(example.Value, budget);
            if (!matched || *matched != example.Expected)
            {
                std::cerr << "Unexpected match: " << example.Pattern << '\n';
                return false;
            }
        }
        return true;
    }

    bool checkCompilation()
    {
        constexpr auto unsupported = std::array<std::string_view, 12>{
            "[a]", "a+",  "a?", "(a)", "a{2}", "\\d",
            "a$b", "a^b", "é",  "😀",  "\xc3", "\xed\xa0\x80",
        };
        for (const auto text : unsupported)
        {
            const auto pattern = PackStacker::PortablePattern::Compile(text, 1024);
            if (pattern ||
                pattern.error().Code != PackStacker::PatternErrorCode::UNSUPPORTED_PATTERN)
            {
                return false;
            }
        }
        const auto limited = PackStacker::PortablePattern::Compile("[a]", 2);
        return !limited && limited.error().Code == PackStacker::PatternErrorCode::SIZE_LIMIT &&
               PackStacker::PortablePattern::Compile("^a$", 3).has_value();
    }

    bool checkBudget()
    {
        const auto pattern = PackStacker::PortablePattern::Compile("^z$|^a$", 1024);
        if (!pattern)
        {
            return false;
        }
        auto budget = std::uint64_t{8};
        const auto matched = pattern->Matches("a", budget);
        if (!matched || !*matched || budget != 0)
        {
            return false;
        }
        budget = 7;
        const auto limited = pattern->Matches("a", budget);
        if (limited || limited.error().Code != PackStacker::PatternErrorCode::WORK_LIMIT ||
            budget != 3)
        {
            return false;
        }
        budget = 4;
        const auto early = pattern->Matches("z", budget);
        return early && *early && budget == 0;
    }

    bool checkErrors()
    {
        const auto pattern = PackStacker::PortablePattern::Compile("^.*$", 1024);
        if (!pattern)
        {
            return false;
        }
        auto budget = std::uint64_t{100};
        const auto cancellation = std::stop_source{};
        cancellation.request_stop();
        const auto cancelled = pattern->Matches("abc", budget, cancellation.get_token());
        if (cancelled || cancelled.error().Code != PackStacker::PatternErrorCode::CANCELLED ||
            budget != 100)
        {
            return false;
        }
        const auto invalid = pattern->Matches("\xf0\x9f", budget);
        if (invalid || invalid.error().Code != PackStacker::PatternErrorCode::INVALID_TEXT ||
            budget != 100)
        {
            return false;
        }
        const auto embedded = pattern->Matches(std::string_view{"a\0b", 3}, budget);
        return embedded && *embedded;
    }

    bool checkFilterDiagnostics()
    {
        const auto pack = std::string("паки/😀");
        const auto unsupported = PackStacker::FilterPattern::Compile("[a]", {}, pack);
        if (unsupported ||
            unsupported.error().Code != PackStacker::ErrorCode::UNSUPPORTED_FEATURE ||
            unsupported.error().Pack != pack || unsupported.error().Path != "pack.mcmeta")
        {
            return false;
        }
        const auto pattern = PackStacker::FilterPattern::Compile("^.$", {}, pack);
        if (!pattern)
        {
            return false;
        }
        auto budget = std::uint64_t{100};
        const auto invalid = pattern->Matches("\xed\xa0\x80", budget, pack, {});
        if (invalid || invalid.error().Code != PackStacker::ErrorCode::INVALID_PATH ||
            invalid.error().Pack != pack || budget != 100)
        {
            return false;
        }
        const auto matched = pattern->Matches("é", budget, pack, {});
        return matched && *matched;
    }

    bool checkFilterSet()
    {
        const auto valid = std::vector<PackStacker::ResourceFilterBlock>{
            {.NamespacePattern = "example", .PathPattern = "first"},
            {.NamespacePattern = "example", .PathPattern = "second"},
        };
        const auto compiled = PackStacker::CompileFilters(valid, {}, "пак");
        if (!compiled || compiled->size() != 2)
        {
            return false;
        }
        const auto invalid = std::vector<PackStacker::ResourceFilterBlock>{
            {.NamespacePattern = "first", .PathPattern = "first"},
            {.NamespacePattern = "second", .PathPattern = "second"},
        };
        const auto rejected = PackStacker::CompileFilters(invalid, {}, "пак");
        return !rejected && rejected.error().Code == PackStacker::ErrorCode::UNSUPPORTED_FEATURE &&
               rejected.error().Pack == "пак" && rejected.error().Path == "pack.mcmeta";
    }
}

int32_t main()
{
    return checkMatches() && checkCompilation() && checkBudget() && checkErrors() &&
                   checkFilterDiagnostics() && checkFilterSet()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
