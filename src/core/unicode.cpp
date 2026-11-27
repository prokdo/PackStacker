#include "core/unicode.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        std::expected<std::string, std::string> mapText(std::string_view text,
                                                        std::uint32_t options)
        {
            if (text.empty())
            {
                return std::string{};
            }
            if (!std::in_range<utf8proc_ssize_t>(text.size()))
            {
                return std::unexpected(std::string{"Unicode input is too long"});
            }
            const auto input = std::vector<utf8proc_uint8_t>(text.begin(), text.end());
            auto* mapped = static_cast<utf8proc_uint8_t*>(nullptr);
            const auto length = utf8proc_map(
                input.data(), static_cast<utf8proc_ssize_t>(text.size()), &mapped, options);
            const auto buffer =
                std::unique_ptr<utf8proc_uint8_t, decltype(&utf8proc_free)>{mapped, utf8proc_free};
            if (length < 0)
            {
                return std::unexpected(std::string{utf8proc_errmsg(length)});
            }
            const auto bytes = std::span{buffer.get(), static_cast<std::size_t>(length)};
            return std::string{bytes.begin(), bytes.end()};
        }
    }

    std::expected<std::string, std::string> PathCollisionKey(std::string_view text)
    {
        const auto normalized = mapText(text, static_cast<std::uint32_t>(UTF8PROC_STABLE) |
                                                  static_cast<std::uint32_t>(UTF8PROC_COMPOSE));
        if (!normalized)
        {
            return std::unexpected(normalized.error());
        }
        return mapText(*normalized, static_cast<std::uint32_t>(UTF8PROC_STABLE) |
                                        static_cast<std::uint32_t>(UTF8PROC_CASEFOLD));
    }

    std::expected<std::u16string, std::string> Utf16FromUtf8(std::string_view text)
    {
        auto result = std::u16string{};
        while (!text.empty())
        {
            auto bytes = std::array<utf8proc_uint8_t, 4>{};
            const auto count = std::min(text.size(), bytes.size());
            std::ranges::copy(text.substr(0, count), bytes.begin());
            auto codepoint = utf8proc_int32_t{0};
            const auto consumed =
                utf8proc_iterate(bytes.data(), static_cast<utf8proc_ssize_t>(count), &codepoint);
            if (consumed < 0)
            {
                return std::unexpected(std::string{utf8proc_errmsg(consumed)});
            }
            if (codepoint <= 0xffff)
            {
                result.push_back(static_cast<char16_t>(codepoint));
            }
            else
            {
                const auto supplementary = codepoint - 0x10000;
                result.push_back(static_cast<char16_t>(0xd800 + (supplementary / 0x400)));
                result.push_back(static_cast<char16_t>(0xdc00 + (supplementary % 0x400)));
            }
            text.remove_prefix(static_cast<std::size_t>(consumed));
        }
        return result;
    }
}
