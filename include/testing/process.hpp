#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PackStacker::Testing
{
    struct Invocation
    {
        int32_t Code;
        std::string Output;
        std::string Error;
    };

    Invocation Run(std::string_view executable, const std::vector<std::string>& arguments,
                   std::string_view directory);
}
