#include "core/paths.hpp"
#include "core/unicode.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    bool checkValidation()
    {
        constexpr auto valid = std::array<std::string_view, 7>{
            "assets/example/file.json", "a", "COM0.txt", "COM10", "com1x", "пак/текст", "😀/café",
        };
        for (const auto path : valid)
        {
            if (!PackStacker::ValidatePortablePath(path))
            {
                std::cerr << "Valid path rejected: " << path << '\n';
                return false;
            }
        }
        constexpr auto invalid = std::array<std::string_view, 23>{
            "",    "/a",  "a/",      "a//b", ".",   "..",       "a/../b",   "a/./b",
            "a.",  "a ",  "a\\b",    "a:b",  "a*b", "a?b",      "a\"b",     "a<b",
            "a>b", "a|b", "con.txt", "PrN",  "aux", "nul.json", "CoM1.txt",
        };
        for (const auto path : invalid)
        {
            if (PackStacker::ValidatePortablePath(path))
            {
                std::cerr << "Unsafe path accepted: " << path << '\n';
                return false;
            }
        }
        constexpr auto reserved = std::array<std::string_view, 5>{
            "LPT9", "com¹.txt", "COM²", "LPT³.json", "a/\xc3",
        };
        for (const auto path : reserved)
        {
            if (PackStacker::ValidatePortablePath(path))
            {
                return false;
            }
        }
        auto deepest = std::string{"a"};
        for (auto index = std::uint32_t{1}; index < 128; ++index)
        {
            deepest += "/a";
        }
        return PackStacker::ValidatePortablePath(deepest).has_value() &&
               !PackStacker::ValidatePortablePath(deepest + "/a") &&
               !PackStacker::ValidatePortablePath(std::string_view{"a\0b", 3}) &&
               !PackStacker::ValidatePortablePath("a/\x1f");
    }

    bool checkCatalog()
    {
        constexpr auto collisions = std::array<std::pair<std::string_view, std::string_view>, 4>{
            std::pair{"assets/A", "assets/a"},
            std::pair{"café/a", "cafe\xcc\x81/b"},
            std::pair{"Straße/a", "STRASSE/b"},
            std::pair{"a", "a/b"},
        };
        for (const auto& [first, second] : collisions)
        {
            auto catalog = PackStacker::PortablePathCatalog{};
            if (!catalog.Add(first, false, 10) || catalog.Add(second, false, 10))
            {
                std::cerr << "Path collision was not rejected\n";
                return false;
            }
        }
        auto catalog = PackStacker::PortablePathCatalog{};
        if (!catalog.Add("a/b", false, 2) || !catalog.Add("a", true, 2) ||
            catalog.Add("a", true, 2))
        {
            return false;
        }
        const auto limited = catalog.Add("a/c", false, 2);
        auto reversed = PackStacker::PortablePathCatalog{};
        return !limited && limited.error().Code == PackStacker::PathErrorCode::LIMIT_EXCEEDED &&
               reversed.Add("a/b", false, 10).has_value() && !reversed.Add("a", false, 10);
    }

    bool checkOutput()
    {
        const auto paths = std::vector<std::string>{"a/b", "a/c"};
        if (!PackStacker::ValidateOutputPathSet(paths, 3) ||
            PackStacker::ValidateOutputPathSet(paths, 2))
        {
            return false;
        }
        const auto conflicts = std::array{
            std::vector<std::string>{"a", "a/b"},
            std::vector<std::string>{"a/b", "a"},
            std::vector<std::string>{"café/a", "cafe\xcc\x81/b"},
            std::vector<std::string>{"A/b", "a/c"},
        };
        for (const auto& conflict : conflicts)
        {
            if (PackStacker::ValidateOutputPathSet(conflict, 10))
            {
                return false;
            }
        }
        return PackStacker::ValidateOutputPathSet({}, 0).has_value();
    }

    bool checkDecoding()
    {
        const auto text = PackStacker::Utf16FromUtf8("a😀é");
        const auto null = PackStacker::Utf16FromUtf8(std::string_view{"a\0b", 3});
        return text && *text == u"a😀é" && null && *null == std::u16string{u"a\0b", 3} &&
               !PackStacker::Utf16FromUtf8("\xed\xa0\x80") &&
               !PackStacker::Utf16FromUtf8("\xf4\x90\x80\x80");
    }
}

int32_t main()
{
    return checkValidation() && checkCatalog() && checkOutput() && checkDecoding() ? EXIT_SUCCESS
                                                                                   : EXIT_FAILURE;
}
