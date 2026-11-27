#include "core/unicode.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    bool checkKeys()
    {
        constexpr auto equivalent = std::array<std::pair<std::string_view, std::string_view>, 5>{
            std::pair{"Readme", "README"},
            std::pair{"caf\xc3\xa9", "cafe\xcc\x81"},
            std::pair{"\xce\xa3", "\xcf\x82"},
            std::pair{"Stra\xc3\x9f"
                      "e",
                      "STRASSE"},
            std::pair{"\xd0\x9f\xd0\xb0\xd0\xba", "\xd0\xbf\xd0\xb0\xd0\xba"},
        };
        for (const auto& [first, second] : equivalent)
        {
            const auto firstKey = PackStacker::PathCollisionKey(first);
            const auto secondKey = PackStacker::PathCollisionKey(second);
            if (!firstKey || !secondKey || *firstKey != *secondKey)
            {
                std::cerr << "Equivalent Unicode paths have different keys\n";
                return false;
            }
        }
        return true;
    }

    bool checkInvalidUtf8()
    {
        constexpr auto invalid = std::array<std::string_view, 5>{
            "\xc0\xaf", "\xc3", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80",
        };
        for (const auto text : invalid)
        {
            if (PackStacker::PathCollisionKey(text) || PackStacker::Utf16FromUtf8(text))
            {
                std::cerr << "Invalid UTF-8 was accepted\n";
                return false;
            }
        }
        return true;
    }
}

int32_t main()
{
    const auto empty = PackStacker::PathCollisionKey({});
    const auto embeddedNull = PackStacker::PathCollisionKey(std::string_view{"A\0B", 3});
    const auto distinct = PackStacker::PathCollisionKey("different");
    if (!checkKeys() || !checkInvalidUtf8() || !empty || !empty->empty() || !embeddedNull ||
        *embeddedNull != std::string("a\0b", 3) || !distinct || *distinct != "different")
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
