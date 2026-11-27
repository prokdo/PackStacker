#pragma once

#include "core/model.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace PackStacker
{
    std::filesystem::path PathFromUtf8(std::string_view text);
    std::string PathToUtf8(const std::filesystem::path& path);
    Result<void> PublishNoReplace(const std::filesystem::path& source,
                                  const std::filesystem::path& destination);

    class StagingDirectory
    {
    public:
        static std::expected<StagingDirectory, std::string> Create(
            const std::filesystem::path& parent);
        ~StagingDirectory();
        StagingDirectory(const StagingDirectory&) = delete;
        StagingDirectory& operator=(const StagingDirectory&) = delete;
        StagingDirectory(StagingDirectory&& other) noexcept;
        StagingDirectory& operator=(StagingDirectory&&) = delete;

        const std::filesystem::path& Path() const;

    private:
        explicit StagingDirectory(std::filesystem::path path);
        std::filesystem::path path;
    };
}
