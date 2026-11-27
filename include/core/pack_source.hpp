#pragma once

#include "core/pack.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace PackStacker
{
    struct SourceEntry
    {
        std::uint64_t Size = 0;
        std::uint64_t ZipIndex = 0;
        std::filesystem::file_time_type Modified;
    };

    class PackSource
    {
    public:
        static Result<std::shared_ptr<const PackSource>> Open(std::string_view path,
                                                              const Limits& limits,
                                                              const std::stop_token& stop);

        const std::string& Path() const;
        const std::map<std::string, SourceEntry, std::less<>>& Entries() const;
        std::uint64_t TotalBytes() const;
        Result<std::string> Read(std::string_view path, std::uint64_t maxBytes,
                                 const std::stop_token& stop = {}) const;
        Result<void> CopyTo(std::string_view path, ByteWriter& output, std::uint64_t maxBytes,
                            const std::stop_token& stop = {}) const;

    private:
        Result<void> indexDirectory(const Limits& limits, const std::stop_token& stop);
        Result<void> indexArchive(const Limits& limits, const std::stop_token& stop);

        std::string sourcePath;
        bool directory = false;
        std::uint64_t archiveSize = 0;
        std::filesystem::file_time_type archiveModified;
        std::map<std::string, SourceEntry, std::less<>> entries;
        std::uint64_t totalBytes = 0;
    };

    Result<void> ValidateLogicalPath(std::string_view path, std::string_view pack);
    Result<void> CheckPhysicalPath(std::string_view path);
    Result<void> ValidateOutputPaths(const std::map<std::string, ResolvedFile>& files,
                                     std::uint64_t maxEntries);
}
