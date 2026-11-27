#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace PackStacker
{
    std::expected<std::string, std::string> PathCollisionKey(std::string_view text);
    std::expected<std::u16string, std::string> Utf16FromUtf8(std::string_view text);
}
