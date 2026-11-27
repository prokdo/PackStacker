#pragma once

#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace PackStacker
{
    enum class PatternErrorCode : std::uint8_t
    {
        UNSUPPORTED_PATTERN,
        SIZE_LIMIT,
        WORK_LIMIT,
        CANCELLED,
        INVALID_TEXT,
    };

    struct PatternError
    {
        PatternErrorCode Code;
        std::string Message;
    };

    class PortablePattern
    {
    public:
        static std::expected<PortablePattern, PatternError> Compile(std::string_view pattern,
                                                                    std::uint64_t maxBytes);
        std::expected<bool, PatternError> Matches(std::string_view value, std::uint64_t& budget,
                                                  const std::stop_token& stop = {}) const;

    private:
        enum class TokenKind : std::uint8_t
        {
            LITERAL,
            ANY,
            STAR,
        };

        struct Token
        {
            TokenKind Kind = TokenKind::LITERAL;
            char16_t Literal = 0;
        };

        struct Alternative
        {
            bool Start = false;
            bool End = false;
            std::vector<Token> Tokens;
        };

        static std::expected<Alternative, PatternError> compileAlternative(
            std::u16string_view branch);
        static std::expected<bool, PatternError> matchesAlternative(const Alternative& alternative,
                                                                    std::u16string_view value,
                                                                    std::uint64_t& budget,
                                                                    const std::stop_token& stop);

        std::vector<Alternative> alternatives;
    };
}
