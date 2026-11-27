#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace PackStacker
{
    enum class PathErrorCode : std::uint8_t
    {
        INVALID_PATH,
        LIMIT_EXCEEDED,
    };

    struct PathError
    {
        PathErrorCode Code;
        std::string Message;
        std::string Path;
    };

    std::expected<void, PathError> ValidatePortablePath(std::string_view path);
    std::expected<void, PathError> ValidateOutputPathSet(std::span<const std::string> paths,
                                                         std::uint64_t maxEntries);

    class PortablePathCatalog
    {
    public:
        std::expected<void, PathError> Add(std::string_view path, bool directory,
                                           std::uint64_t limit);

    private:
        std::set<std::string, std::less<>> explicitPaths;
        std::map<std::string, bool, std::less<>> nodes;
        std::map<std::string, std::string> portablePaths;
    };
}
