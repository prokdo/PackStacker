#include "core/filesystem.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    PackStacker::StagingDirectory create(const std::filesystem::path& parent)
    {
        auto result = PackStacker::StagingDirectory::Create(parent);
        require(result.has_value(), result ? "" : result.error());
        return std::move(*result);
    }

    void testLifetime()
    {
        const auto parent = create(std::filesystem::temp_directory_path());
        auto path = std::filesystem::path{};
        {
            auto original = create(parent.Path());
            path = original.Path();
            const auto second = create(parent.Path());
            require(second.Path() != path, "Staging directories are not unique");
            require(std::filesystem::is_directory(path), "Staging directory was not created");
            {
                const auto moved = std::move(original);
                require(moved.Path() == path, "Moving changed the directory path");
                std::filesystem::create_directories(path / "nested");
                auto file = std::ofstream(path / "nested" / "content", std::ios::binary);
                file << "data";
                file.close();
                require(!file.fail(), "Cannot write staging contents");
            }
            require(!std::filesystem::exists(path), "Moved directory contents were not cleaned");
        }
        require(!std::filesystem::exists(path), "Moved-from owner recreated the directory");
        require(std::filesystem::is_directory(parent.Path()), "Cleanup removed the parent");
        require(std::filesystem::is_empty(parent.Path()), "Cleanup left staging contents");
    }

    void testUnicodeParentAndErrors()
    {
        const auto parent = create(std::filesystem::temp_directory_path());
        const auto unicode = parent.Path() / std::filesystem::path(u8"экспорт-😀");
        std::filesystem::create_directory(unicode);
        const auto staging = create(unicode);
        require(staging.Path().parent_path() == unicode, "Unicode parent path changed");
#ifndef _WIN32
        const auto permissions = std::filesystem::status(staging.Path()).permissions();
        require((permissions &
                 (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
                    std::filesystem::perms::none,
                "Staging directory is accessible to other users");
#endif
        const auto missing = parent.Path() / "missing";
        require(!PackStacker::StagingDirectory::Create(missing), "Missing parent was created");
        require(!std::filesystem::exists(missing), "Missing parent exists after failure");
        const auto filePath = parent.Path() / "file";
        auto file = std::ofstream(filePath);
        file.close();
        require(!file.fail(), "Cannot create parent-file fixture");
        require(!PackStacker::StagingDirectory::Create(filePath), "File accepted as parent");
    }
}

int32_t main()
{
    try
    {
        testLifetime();
        testUnicodeParentAndErrors();
        std::cout << "Filesystem tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
