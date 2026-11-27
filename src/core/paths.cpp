#include "core/paths.hpp"

#include "core/unicode.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace PackStacker
{
    namespace
    {
        std::unexpected<PathError> pathError(PathErrorCode code, std::string message,
                                             std::string_view path)
        {
            return std::unexpected(PathError{
                .Code = code,
                .Message = std::move(message),
                .Path = std::string{path},
            });
        }

        bool reservedDevice(std::u16string_view part)
        {
            auto base = std::u16string{part.substr(0, part.find(u'.'))};
            std::ranges::transform(base, base.begin(),
                                   [](char16_t character)
                                   {
                                       return character >= u'a' && character <= u'z'
                                                  ? static_cast<char16_t>(character - u'a' + u'A')
                                                  : character;
                                   });
            const auto numbered = base.size() == 4 &&
                                  (base.starts_with(u"COM") || base.starts_with(u"LPT")) &&
                                  std::u16string_view{u"123456789¹²³"}.contains(base.back());
            return base == u"CON" || base == u"PRN" || base == u"AUX" || base == u"NUL" || numbered;
        }

        std::expected<void, PathError> validateComponent(std::u16string_view part,
                                                         std::string_view path)
        {
            if (part.empty() || part == u"." || part == u".." || part.ends_with(u'.') ||
                part.ends_with(u' '))
            {
                return pathError(PathErrorCode::INVALID_PATH, "Unsafe path component", path);
            }
            if (std::ranges::any_of(part,
                                    [](char16_t character)
                                    {
                                        return character < 32 ||
                                               std::u16string_view{u"\\:*?\"<>|"}.contains(
                                                   character);
                                    }))
            {
                return pathError(PathErrorCode::INVALID_PATH, "Non-portable path component", path);
            }
            if (reservedDevice(part))
            {
                return pathError(PathErrorCode::INVALID_PATH, "Reserved device name", path);
            }
            return {};
        }
    }

    std::expected<void, PathError> ValidatePortablePath(std::string_view path)
    {
        const auto decoded = Utf16FromUtf8(path);
        if (path.empty() || !decoded || std::ranges::count(path, '/') >= 128)
        {
            return pathError(PathErrorCode::INVALID_PATH, "Invalid entry path", path);
        }
        auto remaining = std::u16string_view{*decoded};
        while (true)
        {
            const auto separator = remaining.find(u'/');
            if (auto valid = validateComponent(remaining.substr(0, separator), path); !valid)
            {
                return valid;
            }
            if (separator == std::u16string_view::npos)
            {
                return {};
            }
            remaining.remove_prefix(separator + 1);
        }
    }

    std::expected<void, PathError> PortablePathCatalog::Add(std::string_view path, bool directory,
                                                            std::uint64_t limit)
    {
        if (auto valid = ValidatePortablePath(path); !valid)
        {
            return valid;
        }
        if (explicitPaths.contains(path))
        {
            return pathError(PathErrorCode::INVALID_PATH, "Duplicate entry", path);
        }
        explicitPaths.emplace(path);
        auto end = std::size_t{0};
        while (true)
        {
            end = path.find('/', end);
            const auto prefix = path.substr(0, end);
            const auto isDirectory = end != std::string_view::npos || directory;
            const auto portable = PathCollisionKey(prefix);
            if (!portable)
            {
                return pathError(PathErrorCode::INVALID_PATH, portable.error(), prefix);
            }
            if (const auto found = portablePaths.find(*portable);
                found != portablePaths.end() && found->second != prefix)
            {
                return pathError(PathErrorCode::INVALID_PATH, "Non-portable name collision",
                                 prefix);
            }
            if (const auto found = nodes.find(prefix); found != nodes.end())
            {
                if (!found->second || !isDirectory)
                {
                    return pathError(PathErrorCode::INVALID_PATH, "File/directory collision",
                                     prefix);
                }
            }
            else
            {
                if (std::cmp_greater_equal(nodes.size(), limit))
                {
                    return pathError(PathErrorCode::LIMIT_EXCEEDED, "Too many entries", path);
                }
                nodes.emplace(prefix, isDirectory);
                portablePaths.insert_or_assign(*portable, std::string{prefix});
            }
            if (end == std::string_view::npos)
            {
                return {};
            }
            ++end;
        }
    }

    std::expected<void, PathError> ValidateOutputPathSet(std::span<const std::string> paths,
                                                         std::uint64_t maxEntries)
    {
        auto portable = std::map<std::string, std::string>{};
        const auto files = std::set<std::string, std::less<>>(paths.begin(), paths.end());
        for (const auto& path : paths)
        {
            if (auto valid = ValidatePortablePath(path); !valid)
            {
                return valid;
            }
            auto end = std::size_t{0};
            while (true)
            {
                end = path.find('/', end);
                const auto prefix = std::string_view{path}.substr(0, end);
                const auto folded = PathCollisionKey(prefix);
                if (!folded)
                {
                    return pathError(PathErrorCode::INVALID_PATH, folded.error(), path);
                }
                if (const auto found = portable.find(*folded);
                    (found != portable.end() && found->second != prefix) ||
                    (end != std::string::npos && files.contains(prefix)))
                {
                    return pathError(PathErrorCode::INVALID_PATH, "Output paths collide", path);
                }
                portable.insert_or_assign(*folded, std::string{prefix});
                if (std::cmp_greater(portable.size(), maxEntries))
                {
                    return pathError(PathErrorCode::LIMIT_EXCEEDED, "Output entry limit exceeded",
                                     path);
                }
                if (end == std::string::npos)
                {
                    break;
                }
                ++end;
            }
        }
        return {};
    }
}
