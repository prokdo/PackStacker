#include "core/filesystem.hpp"

#include "core/model.hpp"

#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <cstdint>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <memory>
#include <sddl.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <linux/fs.h>
#endif

namespace PackStacker
{
    std::filesystem::path PathFromUtf8(std::string_view text)
    {
        return {std::u8string(text.begin(), text.end())};
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const auto text = path.generic_u8string();
        return {text.begin(), text.end()};
    }

    namespace
    {
#ifdef _WIN32
        std::wstring extendedPath(std::filesystem::path path)
        {
            path.make_preferred();
            auto native = path.native();
            if (!native.starts_with(L"\\\\?\\"))
            {
                native = native.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + native.substr(2)
                                                     : L"\\\\?\\" + native;
            }
            return native;
        }
#endif

        std::error_code createPrivateDirectory(const std::filesystem::path& path)
        {
#ifdef _WIN32
            auto* descriptor = PSECURITY_DESCRIPTOR{nullptr};
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
            {
                return {static_cast<int32_t>(GetLastError()), std::system_category()};
            }
            const auto security =
                std::unique_ptr<void, decltype(&LocalFree)>(descriptor, LocalFree);
            auto attributes = SECURITY_ATTRIBUTES{
                .nLength = sizeof(SECURITY_ATTRIBUTES),
                .lpSecurityDescriptor = security.get(),
                .bInheritHandle = FALSE,
            };
            const auto native = extendedPath(path);
            if (CreateDirectoryW(native.c_str(), &attributes))
            {
                return {};
            }
            const auto error = GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
            {
                return std::make_error_code(std::errc::file_exists);
            }
            return {static_cast<int32_t>(error), std::system_category()};
#else
            if (mkdir(path.c_str(), 0700) == 0)
            {
                return {};
            }
            return {errno, std::generic_category()};
#endif
        }
    }

    Result<void> PublishNoReplace(const std::filesystem::path& source,
                                  const std::filesystem::path& destination)
    {
#ifdef _WIN32
        const auto nativeSource = extendedPath(source);
        const auto nativeDestination = extendedPath(destination);
        if (MoveFileExW(nativeSource.c_str(), nativeDestination.c_str(), 0))
        {
            return {};
        }
        const auto error = GetLastError();
        auto statusError = std::error_code{};
        const auto exists = std::filesystem::exists(destination, statusError);
        const auto code = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS || exists
                              ? ErrorCode::OUTPUT_EXISTS
                              : ErrorCode::IO;
        const auto message = "Cannot publish output (Windows error " + std::to_string(error) + ")";
#elifdef __linux__
        if (renameat2(AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) ==
            0)
        {
            return {};
        }
        const auto error = errno;
        auto code = ErrorCode::IO;
        if (error == EEXIST)
        {
            code = ErrorCode::OUTPUT_EXISTS;
        }
        else if (error == ENOSYS || error == EINVAL || error == EOPNOTSUPP)
        {
            code = ErrorCode::UNSUPPORTED_FEATURE;
        }
        const auto message = "Cannot publish output (Linux error " + std::to_string(error) + ")";
#else
        static_cast<void>(source);
        const auto code = ErrorCode::UNSUPPORTED_FEATURE;
        const auto message = "Publishing is supported on Windows and Linux only";
#endif
        return std::unexpected(Diagnostic{
            .Code = code,
            .Message = message,
            .Pack = {},
            .Path = PathToUtf8(destination),
        });
    }

    StagingDirectory::StagingDirectory(std::filesystem::path path) : path(std::move(path))
    {
    }

    StagingDirectory::StagingDirectory(StagingDirectory&& other) noexcept
        : path(std::exchange(other.path, {}))
    {
    }

    StagingDirectory::~StagingDirectory()
    {
        if (!path.empty())
        {
            auto error = std::error_code{};
            std::filesystem::remove_all(path, error);
            if (error)
            {
                std::cerr << "Cannot remove staging directory: " << error.message() << '\n';
            }
        }
    }

    const std::filesystem::path& StagingDirectory::Path() const
    {
        return path;
    }

    std::expected<StagingDirectory, std::string> StagingDirectory::Create(
        const std::filesystem::path& parent)
    {
        auto error = std::error_code{};
        const auto absolute = std::filesystem::absolute(parent, error);
        if (error)
        {
            return std::unexpected(error.message());
        }
        try
        {
            auto random = std::random_device{};
            for (auto attempt = std::size_t{0}; attempt < 128; ++attempt)
            {
                const auto suffix = std::to_string(random()) + "-" + std::to_string(random()) +
                                    "-" + std::to_string(random()) + "-" + std::to_string(random());
                auto candidate = absolute / (".packstacker-" + suffix);
                error = createPrivateDirectory(candidate);
                if (!error)
                {
                    return StagingDirectory(std::move(candidate));
                }
                if (error != std::errc::file_exists)
                {
                    return std::unexpected(error.message());
                }
            }
            return std::unexpected("Cannot allocate a unique staging directory");
        }
        catch (const std::runtime_error& exception)
        {
            return std::unexpected(exception.what());
        }
    }
}
