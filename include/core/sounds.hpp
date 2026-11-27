#pragma once

#include "core/metadata.hpp"

namespace PackStacker
{
    Result<void> ValidateSounds(const Json& sounds, std::string_view pack, std::string_view path);
    Json MergeSounds(Json lower, Json higher);
}
