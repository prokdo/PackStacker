#include "core/pattern.hpp"

#include "core/unicode.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        auto unsupported()
        {
            return std::unexpected(PatternError{
                .Code = PatternErrorCode::UNSUPPORTED_PATTERN,
                .Message = "Filter regex is outside the supported portable subset",
            });
        }

        auto cancelled()
        {
            return std::unexpected(PatternError{
                .Code = PatternErrorCode::CANCELLED,
                .Message = "Cancelled",
            });
        }
    }

    std::expected<PortablePattern, PatternError> PortablePattern::Compile(std::string_view pattern,
                                                                          std::uint64_t maxBytes)
    {
        if (pattern.size() > maxBytes)
        {
            return std::unexpected(PatternError{
                .Code = PatternErrorCode::SIZE_LIMIT,
                .Message = "Filter pattern size limit exceeded",
            });
        }
        const auto text = Utf16FromUtf8(pattern);
        if (!text)
        {
            return unsupported();
        }
        auto result = PortablePattern{};
        auto remaining = std::u16string_view{*text};
        while (true)
        {
            const auto separator = remaining.find(u'|');
            auto alternative = compileAlternative(remaining.substr(0, separator));
            if (!alternative)
            {
                return std::unexpected(alternative.error());
            }
            result.alternatives.push_back(std::move(*alternative));
            if (separator == std::u16string_view::npos)
            {
                break;
            }
            remaining.remove_prefix(separator + 1);
        }
        return result;
    }

    std::expected<PortablePattern::Alternative, PatternError> PortablePattern::compileAlternative(
        std::u16string_view branch)
    {
        auto alternative = Alternative{};
        if (branch.starts_with(u'^'))
        {
            alternative.Start = true;
            branch.remove_prefix(1);
        }
        if (branch.ends_with(u'$') && !branch.ends_with(u"\\$"))
        {
            alternative.End = true;
            branch.remove_suffix(1);
        }
        for (auto index = std::size_t{0}; index < branch.size(); ++index)
        {
            const auto character = branch.at(index);
            if ((character >= u'a' && character <= u'z') ||
                (character >= u'A' && character <= u'Z') ||
                (character >= u'0' && character <= u'9') || character == u'/' ||
                character == u'_' || character == u'-')
            {
                alternative.Tokens.push_back({.Kind = TokenKind::LITERAL, .Literal = character});
            }
            else if (character == u'.')
            {
                if (index + 1 < branch.size() && branch.at(index + 1) == u'*')
                {
                    alternative.Tokens.push_back({.Kind = TokenKind::STAR, .Literal = {}});
                    ++index;
                }
                else
                {
                    alternative.Tokens.push_back({.Kind = TokenKind::ANY, .Literal = {}});
                }
            }
            else if (character == u'\\' && index + 1 < branch.size() &&
                     branch.at(index + 1) == u'.')
            {
                alternative.Tokens.push_back({.Kind = TokenKind::LITERAL, .Literal = u'.'});
                ++index;
            }
            else
            {
                return unsupported();
            }
        }
        return alternative;
    }

    std::expected<bool, PatternError> PortablePattern::Matches(std::string_view value,
                                                               std::uint64_t& budget,
                                                               const std::stop_token& stop) const
    {
        const auto text = Utf16FromUtf8(value);
        if (!text)
        {
            return std::unexpected(PatternError{
                .Code = PatternErrorCode::INVALID_TEXT,
                .Message = text.error(),
            });
        }
        for (const auto& alternative : alternatives)
        {
            const auto matched = matchesAlternative(alternative, *text, budget, stop);
            if (!matched)
            {
                return std::unexpected(matched.error());
            }
            if (*matched)
            {
                return true;
            }
        }
        return false;
    }

    std::expected<bool, PatternError> PortablePattern::matchesAlternative(
        const Alternative& alternative, std::u16string_view value, std::uint64_t& budget,
        const std::stop_token& stop)
    {
        if (stop.stop_requested())
        {
            return cancelled();
        }
        const auto positions = static_cast<std::uint64_t>(value.size()) + 1;
        const auto passes = static_cast<std::uint64_t>(alternative.Tokens.size()) + 1;
        if (positions > budget / passes)
        {
            return std::unexpected(PatternError{
                .Code = PatternErrorCode::WORK_LIMIT,
                .Message = "Filter matching work limit exceeded",
            });
        }
        budget -= positions * passes;
        auto previous = std::vector<std::uint8_t>(static_cast<std::size_t>(positions),
                                                  alternative.Start ? 0 : 1);
        auto current = std::vector<std::uint8_t>(static_cast<std::size_t>(positions));
        previous.front() = 1;
        for (const auto& token : alternative.Tokens)
        {
            if (stop.stop_requested())
            {
                return cancelled();
            }
            std::ranges::fill(current, 0);
            for (auto position = std::size_t{0}; position < current.size(); ++position)
            {
                if (token.Kind == TokenKind::STAR)
                {
                    current.at(position) =
                        static_cast<std::uint8_t>(previous.at(position) != 0 ||
                                                  (position > 0 && current.at(position - 1) != 0));
                }
                else if (position > 0)
                {
                    current.at(position) = static_cast<std::uint8_t>(
                        previous.at(position - 1) != 0 &&
                        (token.Kind == TokenKind::ANY || value.at(position - 1) == token.Literal));
                }
            }
            previous.swap(current);
        }
        return alternative.End
                   ? previous.back() != 0
                   : std::ranges::any_of(previous, [](std::uint8_t value) { return value != 0; });
    }
}
