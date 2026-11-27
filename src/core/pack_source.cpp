#include "core/pack_source.hpp"

#include "core/filesystem.hpp"
#include "core/io.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/paths.hpp"
#include "core/streams.hpp"
#include "core/unicode.hpp"
#include "core/zip_archive.hpp"

#include <zip.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <zipconf.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <errhandlingapi.h>
#include <fileapi.h>
#include <handleapi.h>
#include <minwinbase.h>
#include <winnt.h>
#endif

namespace PackStacker
{
    namespace
    {
        Diagnostic failure(ErrorCode code, std::string_view message, std::string_view pack,
                           std::string_view path = {})
        {
            return {
                .Code = code,
                .Message = std::string(message),
                .Pack = std::string(pack),
                .Path = std::string(path),
            };
        }

        struct SourceSnapshot
        {
            std::filesystem::file_type Type;
            std::uint64_t Size = 0;
            std::filesystem::file_time_type Modified;
        };

        Result<SourceSnapshot> sourceSnapshot(const std::filesystem::path& path,
                                              std::string_view pack, std::string_view logical)
        {
            auto error = std::error_code{};
            const auto status = std::filesystem::symlink_status(path, error);
            if (error)
            {
                return std::unexpected(
                    failure(ErrorCode::IO, "Cannot read source metadata", pack, logical));
            }
            auto snapshot = SourceSnapshot{.Type = status.type(), .Size = 0, .Modified = {}};
            if (!std::filesystem::is_regular_file(status))
            {
                return snapshot;
            }
            snapshot.Size = std::filesystem::file_size(path, error);
            if (!error)
            {
                snapshot.Modified = std::filesystem::last_write_time(path, error);
            }
            if (error)
            {
                return std::unexpected(
                    failure(ErrorCode::IO, "Cannot read source metadata", pack, logical));
            }
            return snapshot;
        }

        std::expected<bool, std::error_code> isPhysicalLink(
            [[maybe_unused]] const std::filesystem::path& path,
            const std::filesystem::file_status& status)
        {
#ifdef _WIN32
            if (std::filesystem::is_symlink(status))
            {
                return true;
            }
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                return std::unexpected(
                    std::error_code{static_cast<int32_t>(GetLastError()), std::system_category()});
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U)
            {
                return false;
            }
            auto data = WIN32_FIND_DATAW{};
            auto* const handle = FindFirstFileW(path.c_str(), &data);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return std::unexpected(
                    std::error_code{static_cast<int32_t>(GetLastError()), std::system_category()});
            }
            const auto link = data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT ||
                              data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
            if (FindClose(handle) == 0)
            {
                return std::unexpected(
                    std::error_code{static_cast<int32_t>(GetLastError()), std::system_category()});
            }
            return link;
#else
            return std::filesystem::is_symlink(status);
#endif
        }

        Result<std::vector<std::string>> directoryChildren(const std::filesystem::path& path,
                                                           std::string_view pack,
                                                           std::string_view logical,
                                                           const std::stop_token& stop)
        {
            auto error = std::error_code{};
            auto iterator = std::filesystem::directory_iterator(path, error);
            auto children = std::vector<std::pair<std::u16string, std::string>>{};
            const auto end = std::filesystem::directory_iterator{};
            while (!error && iterator != end)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(failure(ErrorCode::CANCELLED, "Cancelled", pack));
                }
                try
                {
                    auto name = PathToUtf8(iterator->path().filename());
                    auto decoded = Utf16FromUtf8(name);
                    if (!decoded)
                    {
                        auto invalidPath = std::string(logical);
                        if (!invalidPath.empty())
                        {
                            invalidPath += '/';
                        }
                        invalidPath += name;
                        return std::unexpected(failure(ErrorCode::INVALID_PATH,
                                                       "Invalid entry path", pack, invalidPath));
                    }
                    children.emplace_back(std::move(*decoded), std::move(name));
                }
                catch (const std::filesystem::filesystem_error&)
                {
                    return std::unexpected(
                        failure(ErrorCode::INVALID_PATH, "Invalid entry path", pack, logical));
                }
                iterator.increment(error);
            }
            if (error)
            {
                return std::unexpected(
                    failure(ErrorCode::IO, "Directory is not readable", pack, logical));
            }
            std::ranges::sort(children);
            auto result = std::vector<std::string>{};
            result.reserve(children.size());
            for (auto& child : children)
            {
                result.push_back(std::move(child.second));
            }
            return result;
        }

        Result<SourceSnapshot> regularSnapshot(const std::filesystem::path& path,
                                               std::string_view pack, std::string_view logical)
        {
            const auto snapshot = sourceSnapshot(path, pack, logical);
            if (!snapshot)
            {
                return std::unexpected(snapshot.error());
            }
            const auto status = std::filesystem::file_status{snapshot->Type};
            const auto link = isPhysicalLink(path, status);
            if (!link)
            {
                return std::unexpected(
                    failure(ErrorCode::IO, "Cannot inspect physical path", pack, logical));
            }
            if (*link || (snapshot->Type != std::filesystem::file_type::regular &&
                          snapshot->Type != std::filesystem::file_type::directory))
            {
                return std::unexpected(failure(ErrorCode::INVALID_PATH,
                                               "Only regular files and directories are supported",
                                               pack, logical));
            }
            return *snapshot;
        }

        Diagnostic pathFailure(const PathError& error, std::string_view pack)
        {
            return Diagnostic{
                .Code = error.Code == PathErrorCode::LIMIT_EXCEEDED ? ErrorCode::LIMIT_EXCEEDED
                                                                    : ErrorCode::INVALID_PATH,
                .Message = error.Message,
                .Pack = std::string(pack),
                .Path = error.Path,
            };
        }

        Result<void> addCatalogPath(PortablePathCatalog& catalog, std::string_view path,
                                    bool directory, std::string_view pack, std::uint64_t limit)
        {
            if (auto added = catalog.Add(path, directory, limit); !added)
            {
                return std::unexpected(pathFailure(added.error(), pack));
            }
            return {};
        }

        Result<void> checkSize(std::uint64_t size, std::uint64_t total, const Limits& limits,
                               std::string_view pack, std::string_view path)
        {
            if (size > limits.MaxFileBytes || total > limits.MaxTotalBytes ||
                size > limits.MaxTotalBytes - total)
            {
                return std::unexpected(
                    failure(ErrorCode::LIMIT_EXCEEDED, "Pack size limit exceeded", pack, path));
            }
            return {};
        }

        Result<void> validateZipAttributes(zip_t* archive, zip_uint64_t index,
                                           const zip_stat_t& stat, bool directory,
                                           std::string_view pack, std::string_view path)
        {
            auto system = zip_uint8_t{0};
            auto attributes = zip_uint32_t{0};
            if (zip_file_get_external_attributes(archive, index, 0, &system, &attributes) != 0)
            {
                return std::unexpected(failure(ErrorCode::INVALID_ARCHIVE,
                                               "Cannot read ZIP entry attributes", pack, path));
            }
            const auto mode = (attributes >> 16U) & 0170000U;
            if ((system == ZIP_OPSYS_UNIX && mode != 0 &&
                 mode != (directory ? 0040000U : 0100000U)) ||
                ((system == ZIP_OPSYS_DOS || system == ZIP_OPSYS_WINDOWS_NTFS) &&
                 (attributes & 0x400U) != 0) ||
                stat.encryption_method != ZIP_EM_NONE || (directory && stat.size != 0))
            {
                return std::unexpected(failure(ErrorCode::INVALID_ARCHIVE,
                                               "Unsupported ZIP entry type or encryption", pack,
                                               path));
            }
            return {};
        }

        Result<void> checkSourceSnapshot(std::string_view physical, std::uint64_t size,
                                         std::filesystem::file_time_type modified,
                                         std::string_view pack, std::string_view path)
        {
            if (auto valid = CheckPhysicalPath(physical); !valid)
            {
                return std::unexpected(valid.error());
            }
            const auto current = sourceSnapshot(PathFromUtf8(physical), pack, path);
            if (!current || current->Type != std::filesystem::file_type::regular ||
                current->Size != size || current->Modified != modified)
            {
                return std::unexpected(failure(ErrorCode::SOURCE_CHANGED,
                                               "Source changed after indexing", pack, path));
            }
            return {};
        }

        template <typename Reader>
        Result<std::uint64_t> copyContents(Reader read, ByteWriter& output, std::uint64_t size,
                                           std::string_view pack, std::string_view path,
                                           const std::stop_token& stop)
        {
            auto processed = std::uint64_t{0};
            auto buffer = std::array<char, 65536>{};
            while (true)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(failure(ErrorCode::CANCELLED, "Cancelled", pack, path));
                }
                const auto count = read(buffer);
                if (count < 0)
                {
                    return std::unexpected(failure(ErrorCode::IO, "Read failed", pack, path));
                }
                if (static_cast<std::uint64_t>(count) > size - processed)
                {
                    return std::unexpected(
                        failure(ErrorCode::SOURCE_CHANGED, "Entry size changed", pack, path));
                }
                if (auto written =
                        WriteAll(output, std::span(buffer.data(), static_cast<std::size_t>(count)),
                                 pack, path, stop);
                    !written)
                {
                    return std::unexpected(written.error());
                }
                processed += static_cast<std::uint64_t>(count);
                if (count == 0)
                {
                    break;
                }
            }
            return processed;
        }

        Result<std::uint64_t> copyFileContents(const std::filesystem::path& physical,
                                               ByteWriter& output, std::uint64_t size,
                                               std::string_view pack, std::string_view path,
                                               const std::stop_token& stop)
        {
            auto file = std::ifstream(physical, std::ios::binary);
            if (!file.is_open())
            {
                return std::unexpected(
                    failure(ErrorCode::IO, "Cannot open source file", pack, path));
            }
            return copyContents(
                [&](auto& buffer)
                {
                    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    return file.bad() || (file.fail() && !file.eof()) ? std::streamsize{-1}
                                                                      : file.gcount();
                },
                output, size, pack, path, stop);
        }
    }

    Result<void> ValidateLogicalPath(std::string_view path, std::string_view pack)
    {
        if (auto valid = ValidatePortablePath(path); !valid)
        {
            return std::unexpected(pathFailure(valid.error(), pack));
        }
        return {};
    }

    Result<void> ValidateOutputPaths(const std::map<std::string, ResolvedFile>& files,
                                     std::uint64_t maxEntries)
    {
        auto paths = std::vector<std::string>{};
        paths.reserve(files.size());
        for (const auto& entry : files)
        {
            paths.push_back(entry.first);
        }
        if (auto valid = ValidateOutputPathSet(paths, maxEntries); !valid)
        {
            return std::unexpected(pathFailure(valid.error(), {}));
        }
        return {};
    }

    Result<void> CheckPhysicalPath(std::string_view path)
    {
        if (path.contains('\0') || !Utf16FromUtf8(path))
        {
            return std::unexpected(failure(ErrorCode::INVALID_PATH, "Invalid physical path", path));
        }
        auto error = std::error_code{};
        const auto absolute = std::filesystem::absolute(PathFromUtf8(path), error);
        if (error)
        {
            return std::unexpected(failure(ErrorCode::IO, "Cannot resolve physical path", path));
        }
        auto current = absolute.root_path();
        for (const auto& component : absolute.relative_path())
        {
            current /= component;
            const auto status = std::filesystem::symlink_status(current, error);
            if (error == std::errc::no_such_file_or_directory ||
                error == std::errc::not_a_directory)
            {
                error.clear();
                continue;
            }
            if (error)
            {
                return std::unexpected(failure(ErrorCode::IO, "Cannot inspect physical path", path,
                                               PathToUtf8(current)));
            }
            const auto link = isPhysicalLink(current, status);
            if (!link)
            {
                return std::unexpected(failure(ErrorCode::IO, "Cannot inspect physical path", path,
                                               PathToUtf8(current)));
            }
            if (*link)
            {
                return std::unexpected(failure(ErrorCode::INVALID_PATH,
                                               "Symbolic links and junctions are not supported",
                                               path, PathToUtf8(current)));
            }
        }
        return {};
    }

    Result<std::shared_ptr<const PackSource>> PackSource::Open(std::string_view pathText,
                                                               const Limits& limits,
                                                               const std::stop_token& stop)
    {
        const auto decoded = Utf16FromUtf8(pathText);
        if (!decoded || pathText.empty() || pathText.contains('\0'))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::INVALID_PATH,
                .Message = "Invalid pack path",
                .Pack = std::string(pathText),
                .Path = {},
            });
        }
        auto error = std::error_code{};
        const auto physical = std::filesystem::absolute(PathFromUtf8(pathText), error);
        if (error)
        {
            return std::unexpected(failure(ErrorCode::IO, "Pack is not readable", pathText));
        }
        if (auto valid = CheckPhysicalPath(PathToUtf8(physical)); !valid)
        {
            return std::unexpected(valid.error());
        }
        const auto absolute = PathToUtf8(physical.lexically_normal());
        const auto root = sourceSnapshot(physical, absolute, {});
        if (!root || (root->Type != std::filesystem::file_type::directory &&
                      root->Type != std::filesystem::file_type::regular))
        {
            return std::unexpected(failure(ErrorCode::IO, "Pack is not readable", absolute));
        }
        if (root->Type == std::filesystem::file_type::regular &&
            !std::ifstream(physical, std::ios::binary).is_open())
        {
            return std::unexpected(failure(ErrorCode::IO, "Pack is not readable", absolute));
        }
        auto source = std::make_shared<PackSource>();
        source->sourcePath = absolute;
        source->directory = root->Type == std::filesystem::file_type::directory;
        source->archiveSize = root->Size;
        source->archiveModified = root->Modified;
        auto indexed = source->directory ? source->indexDirectory(limits, stop)
                                         : source->indexArchive(limits, stop);
        if (!indexed)
        {
            return std::unexpected(indexed.error());
        }
        if (stop.stop_requested())
        {
            return std::unexpected(failure(ErrorCode::CANCELLED, "Cancelled", absolute));
        }
        return std::shared_ptr<const PackSource>(std::move(source));
    }

    Result<void> PackSource::indexDirectory(const Limits& limits, const std::stop_token& stop)
    {
        const auto& absolute = sourcePath;
        auto catalog = PortablePathCatalog{};
        auto pending = std::vector<std::string>{std::string()};
        while (!pending.empty())
        {
            const auto relative = pending.back();
            pending.pop_back();
            const auto physical = PathFromUtf8(absolute) / PathFromUtf8(relative);
            const auto children = directoryChildren(physical, absolute, relative, stop);
            if (!children)
            {
                return std::unexpected(children.error());
            }
            for (const auto& child : *children)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(failure(ErrorCode::CANCELLED, "Cancelled", absolute));
                }
                auto name = relative;
                if (!name.empty())
                {
                    name += '/';
                }
                name += child;
                const auto childPath = physical / PathFromUtf8(child);
                const auto snapshot = regularSnapshot(childPath, absolute, name);
                if (!snapshot)
                {
                    return std::unexpected(snapshot.error());
                }
                if (auto added = addCatalogPath(
                        catalog, name, snapshot->Type == std::filesystem::file_type::directory,
                        absolute, limits.MaxEntries);
                    !added)
                {
                    return added;
                }
                if (snapshot->Type == std::filesystem::file_type::directory)
                {
                    pending.push_back(name);
                    continue;
                }
                const auto size = snapshot->Size;
                if (auto valid = checkSize(size, totalBytes, limits, absolute, name); !valid)
                {
                    return std::unexpected(valid.error());
                }
                entries.emplace(
                    name, SourceEntry{.Size = size, .ZipIndex = 0, .Modified = snapshot->Modified});
                totalBytes += size;
            }
        }
        return {};
    }

    Result<void> PackSource::indexArchive(const Limits& limits, const std::stop_token& stop)
    {
        const auto& absolute = sourcePath;
        auto catalog = PortablePathCatalog{};
        auto opened = OpenZip(absolute);
        if (!opened)
        {
            return std::unexpected(opened.error());
        }
        const auto& archive = *opened;
        const auto count = zip_get_num_entries(archive.get(), 0);
        if (count < 0 || std::cmp_greater(count, limits.MaxEntries))
        {
            return std::unexpected(
                failure(ErrorCode::LIMIT_EXCEEDED, "Too many ZIP entries", absolute));
        }
        for (auto index = zip_uint64_t{0}; std::cmp_less(index, count); ++index)
        {
            if (stop.stop_requested())
            {
                return std::unexpected(failure(ErrorCode::CANCELLED, "Cancelled", absolute));
            }
            auto stat = zip_stat_t{};
            zip_stat_init(&stat);
            if (zip_stat_index(archive.get(), index, ZIP_FL_ENC_STRICT, &stat) != 0 ||
                (stat.valid & (ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_ENCRYPTION_METHOD)) !=
                    (ZIP_STAT_NAME | ZIP_STAT_SIZE | ZIP_STAT_ENCRYPTION_METHOD))
            {
                return std::unexpected(failure(ErrorCode::INVALID_ARCHIVE,
                                               "Cannot read ZIP entry metadata", absolute));
            }
            const auto decoded = Utf16FromUtf8(stat.name);
            if (!decoded)
            {
                return std::unexpected(
                    failure(ErrorCode::INVALID_PATH, "Invalid UTF-8 entry name", absolute));
            }
            auto name = std::string(stat.name);
            const auto directory = name.ends_with('/');
            if (directory)
            {
                name.pop_back();
            }
            if (auto added = catalog.Add(name, directory, limits.MaxEntries); !added)
            {
                return std::unexpected(pathFailure(added.error(), absolute));
            }
            if (auto valid =
                    validateZipAttributes(archive.get(), index, stat, directory, absolute, name);
                !valid)
            {
                return std::unexpected(valid.error());
            }
            if (directory)
            {
                continue;
            }
            if (auto valid = checkSize(stat.size, totalBytes, limits, absolute, name); !valid)
            {
                return std::unexpected(valid.error());
            }
            entries.emplace(name,
                            SourceEntry{.Size = stat.size, .ZipIndex = index, .Modified = {}});
            totalBytes += stat.size;
        }
        return {};
    }

    const std::string& PackSource::Path() const
    {
        return sourcePath;
    }

    const std::map<std::string, SourceEntry, std::less<>>& PackSource::Entries() const
    {
        return entries;
    }

    std::uint64_t PackSource::TotalBytes() const
    {
        return totalBytes;
    }

    Result<std::string> PackSource::Read(std::string_view path, std::uint64_t maxBytes,
                                         const std::stop_token& stop) const
    {
        const auto entry = entries.find(path);
        if (entry == entries.cend())
        {
            return std::unexpected(failure(ErrorCode::IO, "Entry not found", sourcePath, path));
        }
        if (entry->second.Size > std::string{}.max_size())
        {
            return std::unexpected(
                failure(ErrorCode::LIMIT_EXCEEDED, "Read size limit exceeded", sourcePath, path));
        }
        auto output = MemoryWriter{};
        if (auto copied = CopyTo(path, output, maxBytes, stop); !copied)
        {
            return std::unexpected(copied.error());
        }
        return std::move(output).Bytes();
    }

    Result<void> PackSource::CopyTo(std::string_view path, ByteWriter& output,
                                    std::uint64_t maxBytes, const std::stop_token& stop) const
    {
        const auto entry = entries.find(path);
        if (entry == entries.cend())
        {
            return std::unexpected(failure(ErrorCode::IO, "Entry not found", sourcePath, path));
        }
        if (entry->second.Size > maxBytes)
        {
            return std::unexpected(
                failure(ErrorCode::LIMIT_EXCEEDED, "Read size limit exceeded", sourcePath, path));
        }
        if (stop.stop_requested())
        {
            return std::unexpected(failure(ErrorCode::CANCELLED, "Cancelled", sourcePath, path));
        }
        if (!output.IsWritable())
        {
            return std::unexpected(failure(
                ErrorCode::IO, "Output device must be writable in binary mode", sourcePath, path));
        }
        const auto physical =
            directory ? PathToUtf8(PathFromUtf8(sourcePath) / PathFromUtf8(path)) : sourcePath;
        const auto expectedSize = directory ? entry->second.Size : archiveSize;
        const auto expectedModified = directory ? entry->second.Modified : archiveModified;
        if (auto valid =
                checkSourceSnapshot(physical, expectedSize, expectedModified, sourcePath, path);
            !valid)
        {
            return std::unexpected(valid.error());
        }
        auto processed = std::uint64_t{0};
        if (directory)
        {
            auto copied = copyFileContents(PathFromUtf8(physical), output, entry->second.Size,
                                           sourcePath, path, stop);
            if (!copied)
            {
                return std::unexpected(copied.error());
            }
            processed = *copied;
        }
        else
        {
            auto opened = OpenZip(sourcePath);
            if (!opened)
            {
                return std::unexpected(opened.error());
            }
            auto* handle = zip_fopen_index(opened->get(), entry->second.ZipIndex, 0);
            if (handle == nullptr)
            {
                return std::unexpected(failure(ErrorCode::INVALID_ARCHIVE,
                                               zip_strerror(opened->get()), sourcePath, path));
            }
            auto file = std::unique_ptr<zip_file_t, decltype(&zip_fclose)>(handle, zip_fclose);
            auto copied = copyContents(
                [&](auto& buffer) { return zip_fread(file.get(), buffer.data(), buffer.size()); },
                output, entry->second.Size, sourcePath, path, stop);
            if (!copied)
            {
                return std::unexpected(copied.error());
            }
            if (zip_fclose(file.release()) != 0)
            {
                return std::unexpected(failure(ErrorCode::INVALID_ARCHIVE,
                                               "ZIP entry integrity check failed", sourcePath,
                                               path));
            }
            processed = *copied;
        }
        if (processed != entry->second.Size)
        {
            return std::unexpected(
                failure(ErrorCode::SOURCE_CHANGED, "Entry size changed", sourcePath, path));
        }
        return {};
    }
}
