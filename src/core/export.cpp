#include "core/filesystem.hpp"
#include "core/filters.hpp"
#include "core/io.hpp"
#include "core/metadata.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/pack_source.hpp"
#include "core/streams.hpp"
#include "core/unicode.hpp"
#include "core/zip_archive.hpp"

#include <zip.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <zipconf.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cwchar>

#include <minwindef.h>
#include <stringapiset.h>
#include <winnls.h>
#else
#include <unistd.h>
#endif
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        bool writableDirectory(const std::filesystem::path& path)
        {
            auto error = std::error_code{};
            if (!std::filesystem::is_directory(path, error) || error)
            {
                return false;
            }
#ifdef _WIN32
            return _waccess(path.c_str(), 2) == 0;
#else
            return access(path.c_str(), W_OK) == 0;
#endif
        }

        Result<Json> outputLanguages(const ResolvedPack& pack, const Limits& limits)
        {
            if (pack.Type != PackType::RESOURCE)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::UNSUPPORTED_FEATURE,
                    .Message = "Language definitions are supported for resource packs only",
                    .Pack = {},
                    .Path = "pack.mcmeta",
                });
            }
            if (std::cmp_greater(pack.Languages.size(), limits.MaxEntries))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::LIMIT_EXCEEDED,
                    .Message = "Language definition limit exceeded",
                    .Pack = {},
                    .Path = "pack.mcmeta",
                });
            }
            if (auto valid = ValidateLanguages(pack.Languages, {}); !valid)
            {
                return std::unexpected(valid.error());
            }
            auto languages = Json::object();
            for (const auto& [code, language] : pack.Languages)
            {
                languages.emplace(code, Json{
                                            {"name", language.Name},
                                            {"region", language.Region},
                                            {"bidirectional", language.Bidirectional},
                                        });
            }
            return languages;
        }

        Result<std::string> outputMetadata(const ResolvedPack& pack, const ExportRequest& request)
        {
            if ((pack.Type != PackType::RESOURCE && pack.Type != PackType::DATA) ||
                pack.Target.Major < 4 || pack.Target.Minor < 0 ||
                !Utf16FromUtf8(request.Description))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INVALID_METADATA,
                    .Message = "Invalid output metadata",
                    .Pack = {},
                    .Path = "pack.mcmeta",
                });
            }
            auto metadata = Json{{"description", request.Description}};
            const auto modernThreshold = pack.Type == PackType::RESOURCE ? 65 : 82;
            if (pack.Target.Major < modernThreshold)
            {
                if (pack.Target.Minor != 0)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::UNSUPPORTED_FEATURE,
                        .Message = "Legacy metadata cannot declare a minor format",
                        .Pack = {},
                        .Path = "pack.mcmeta",
                    });
                }
                metadata.emplace("pack_format", pack.Target.Major);
            }
            else
            {
                const auto version = Json::array({pack.Target.Major, pack.Target.Minor});
                metadata.emplace("min_format", version);
                metadata.emplace("max_format", version);
            }
            auto root = Json{{"pack", std::move(metadata)}};
            if (!pack.Languages.empty())
            {
                auto languages = outputLanguages(pack, request.Limits);
                if (!languages)
                {
                    return std::unexpected(languages.error());
                }
                root.emplace("language", std::move(*languages));
            }
            if (!pack.Filters.empty())
            {
                if (pack.Target.Major < (pack.Type == PackType::RESOURCE ? 9 : 10))
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::UNSUPPORTED_FEATURE,
                        .Message = "The target format predates pack filters",
                        .Pack = {},
                        .Path = "pack.mcmeta",
                    });
                }
                if (auto valid = CompileFilters(pack.Filters, request.Limits, {}); !valid)
                {
                    return std::unexpected(valid.error());
                }
                auto blocks = Json::array();
                for (const auto& block : pack.Filters)
                {
                    blocks.push_back({
                        {"namespace", block.NamespacePattern},
                        {"path", block.PathPattern},
                    });
                }
                root.emplace("filter", Json{{"block", std::move(blocks)}});
            }
            auto bytes = root.dump();
            if (auto valid = ParseJson(bytes, {}, "pack.mcmeta", request.Limits,
                                       ErrorCode::INVALID_METADATA);
                !valid)
            {
                return std::unexpected(valid.error());
            }
            return bytes;
        }

        std::expected<bool, std::string> within(std::string_view candidate, std::string_view root)
        {
#ifdef _WIN32
            auto candidatePath = PathFromUtf8(candidate);
            auto rootPath = PathFromUtf8(root);
            candidatePath.make_preferred();
            rootPath.make_preferred();
            const auto& native = candidatePath.native();
            const auto& prefix = rootPath.native();
            if (native.size() < prefix.size())
            {
                return false;
            }
            if (!std::in_range<int32_t>(prefix.size()))
            {
                return std::unexpected("Path comparison limit exceeded");
            }
            const auto length = static_cast<int32_t>(prefix.size());
            const auto compared =
                CompareStringOrdinal(native.data(), length, prefix.data(), length, TRUE);
            if (compared == 0)
            {
                return std::unexpected("Cannot compare Windows paths");
            }
            return compared == CSTR_EQUAL &&
                   (native.size() == prefix.size() || prefix.ends_with(L'\\') ||
                    native.at(prefix.size()) == L'\\');
#else
            return candidate == root ||
                   candidate.starts_with(root.ends_with('/') ? std::string(root)
                                                             : std::string(root) + '/');
#endif
        }

        Result<bool> inputOverlaps(std::string_view source, std::string_view output)
        {
            auto error = std::error_code{};
            const auto physical = PathFromUtf8(source);
            auto input = std::filesystem::canonical(physical, error);
            if (error)
            {
                input = std::filesystem::absolute(physical, error);
            }
            if (error)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = error.message(),
                    .Pack = std::string(source),
                    .Path = std::string(output),
                });
            }
            const auto directory = std::filesystem::is_directory(physical, error);
            if (error && error != std::errc::no_such_file_or_directory)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = error.message(),
                    .Pack = std::string(source),
                    .Path = std::string(output),
                });
            }
            const auto logical = PathToUtf8(input.lexically_normal());
            const auto nested = within(logical, output);
            const auto contains =
                nested && !*nested && directory ? within(output, logical) : nested;
            if (!contains)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = contains.error(),
                    .Pack = std::string(source),
                    .Path = std::string(output),
                });
            }
            return *contains;
        }

        Result<std::string> destinationPath(const ResolvedPack& pack, const ExportRequest& request)
        {
            if (request.Path.empty() || request.Path.contains('\0') ||
                !Utf16FromUtf8(request.Path) ||
                (request.Format != OutputFormat::DIRECTORY && request.Format != OutputFormat::ZIP))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INVALID_PATH,
                    .Message = "Invalid output path or format",
                    .Pack = {},
                    .Path = request.Path,
                });
            }
            auto error = std::error_code{};
            const auto destination = std::filesystem::absolute(PathFromUtf8(request.Path), error);
            if (error)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = error.message(),
                    .Pack = {},
                    .Path = request.Path,
                });
            }
            if (auto valid = ValidateLogicalPath(PathToUtf8(destination.filename()), {}); !valid)
            {
                return std::unexpected(valid.error());
            }
            const auto parent = destination.parent_path();
            if (auto valid = CheckPhysicalPath(PathToUtf8(destination)); !valid)
            {
                return std::unexpected(valid.error());
            }
            if (!writableDirectory(parent))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = "Output parent must be an existing writable directory",
                    .Pack = {},
                    .Path = PathToUtf8(parent),
                });
            }
            const auto canonicalParent = std::filesystem::canonical(parent, error);
            if (error)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = "Cannot resolve output parent",
                    .Pack = {},
                    .Path = PathToUtf8(parent),
                });
            }
            auto path = PathToUtf8(canonicalParent / destination.filename());
            auto sources =
                std::set<std::string>(pack.SourcePaths.cbegin(), pack.SourcePaths.cend());
            for (const auto& entry : pack.Files)
            {
                for (const auto& source : entry.second.SourcePacks())
                {
                    sources.insert(source);
                }
            }
            for (const auto& source : sources)
            {
                const auto decoded = Utf16FromUtf8(source);
                if (!decoded || source.contains('\0'))
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::INVALID_PATH,
                        .Message = "Invalid source pack path",
                        .Pack = source,
                        .Path = path,
                    });
                }
                const auto overlaps = inputOverlaps(source, path);
                if (!overlaps)
                {
                    return std::unexpected(overlaps.error());
                }
                if (*overlaps)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::INVALID_PATH,
                        .Message = "Output overlaps an input pack",
                        .Pack = source,
                        .Path = path,
                    });
                }
            }
            const auto exists = std::filesystem::exists(destination, error);
            if (error)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = error.message(),
                    .Pack = {},
                    .Path = path,
                });
            }
            if (exists)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::OUTPUT_EXISTS,
                    .Message = "Output already exists; replacement is disabled",
                    .Pack = {},
                    .Path = path,
                });
            }
            return path;
        }

        Result<void> validateContents(const ResolvedPack& pack, const ExportRequest& request,
                                      const std::string& metadata)
        {
            if (request.Limits.MaxEntries == 0)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::LIMIT_EXCEEDED,
                    .Message = "Output entry limit exceeded",
                    .Pack = {},
                    .Path = request.Path,
                });
            }
            if (auto valid = ValidateOutputPaths(pack.Files, request.Limits.MaxEntries - 1); !valid)
            {
                return valid;
            }
            auto total = static_cast<std::uint64_t>(metadata.size());
            if (total > request.Limits.MaxFileBytes || total > request.Limits.MaxTotalBytes)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::LIMIT_EXCEEDED,
                    .Message = "Output metadata size limit exceeded",
                    .Pack = {},
                    .Path = "pack.mcmeta",
                });
            }
            for (const auto& entry : pack.Files)
            {
                const auto firstComponent =
                    std::string_view(entry.first).substr(0, entry.first.find('/'));
                const auto folded = PathCollisionKey(firstComponent);
                if (!folded)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::INVALID_PATH,
                        .Message = folded.error(),
                        .Pack = {},
                        .Path = entry.first,
                    });
                }
                if (*folded == "pack.mcmeta")
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::INVALID_PATH,
                        .Message = "pack.mcmeta is reserved for generated metadata",
                        .Pack = {},
                        .Path = entry.first,
                    });
                }
                const auto opposite = pack.Type == PackType::RESOURCE
                                          ? entry.first.starts_with("data/")
                                          : entry.first.starts_with("assets/");
                if (opposite)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::UNSUPPORTED_FEATURE,
                        .Message = "Output contains the wrong pack type",
                        .Pack = {},
                        .Path = entry.first,
                    });
                }
                if (entry.second.Size() > request.Limits.MaxFileBytes ||
                    entry.second.Size() > request.Limits.MaxTotalBytes - total)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::LIMIT_EXCEEDED,
                        .Message = "Output size limit exceeded",
                        .Pack = {},
                        .Path = entry.first,
                    });
                }
                total += entry.second.Size();
            }
            return {};
        }

        std::unexpected<Diagnostic> outputError(const std::string& message, std::string_view path)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::IO,
                .Message = message,
                .Pack = {},
                .Path = std::string(path),
            });
        }

        Result<FileWriter> openOutputFile(const std::filesystem::path& physical,
                                          std::string_view logical)
        {
            auto file = FileWriter::Open(physical);
            if (!file)
            {
                return outputError(file.error(), logical);
            }
            return std::move(*file);
        }

        Result<void> finishOutputFile(FileWriter& file, std::string_view path)
        {
            if (auto flushed = file.Flush(); !flushed)
            {
                return outputError(flushed.error(), path);
            }
            if (auto closed = file.Close(); !closed)
            {
                return outputError(closed.error(), path);
            }
            return {};
        }

        Result<void> writeMetadata(const std::filesystem::path& root, const std::string& bytes,
                                   const std::stop_token& stop)
        {
            auto file = openOutputFile(root / "pack.mcmeta", "pack.mcmeta");
            if (!file)
            {
                return std::unexpected(file.error());
            }
            if (auto written = WriteAll(*file, bytes, {}, "pack.mcmeta", stop); !written)
            {
                return written;
            }
            return finishOutputFile(*file, "pack.mcmeta");
        }

        Result<void> writeContents(const ResolvedPack& pack, const ExportRequest& request,
                                   const std::filesystem::path& root, const std::stop_token& stop)
        {
            for (const auto& entry : pack.Files)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::CANCELLED,
                        .Message = "Cancelled",
                        .Pack = {},
                        .Path = entry.first,
                    });
                }
                const auto& logical = entry.first;
                const auto physical = root / PathFromUtf8(logical);
                auto directoryError = std::error_code{};
                std::filesystem::create_directories(physical.parent_path(), directoryError);
                if (directoryError)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::IO,
                        .Message = "Cannot create output directory",
                        .Pack = {},
                        .Path = entry.first,
                    });
                }
                auto file = openOutputFile(physical, logical);
                if (!file)
                {
                    return std::unexpected(file.error());
                }
                if (auto copied = entry.second.CopyTo(*file, request.Limits.MaxFileBytes, stop);
                    !copied)
                {
                    auto error = copied.error();
                    if (error.Path.empty())
                    {
                        error.Path = entry.first;
                    }
                    return std::unexpected(std::move(error));
                }
                if (auto finished = finishOutputFile(*file, logical); !finished)
                {
                    return finished;
                }
                if (file->Size() != entry.second.Size())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::SOURCE_CHANGED,
                        .Message = "Copied entry size does not match the plan",
                        .Pack = {},
                        .Path = entry.first,
                    });
                }
            }
            return {};
        }

        Result<void> writeZip(const ResolvedPack& pack, const std::filesystem::path& root,
                              const std::filesystem::path& path, std::stop_token stop)
        {
            auto opened = OpenZip(PathToUtf8(path), ZIP_CREATE | ZIP_EXCL);
            if (!opened)
            {
                return std::unexpected(opened.error());
            }
            auto& archive = *opened;
            const auto failed = [&]
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = zip_strerror(archive.get()),
                    .Pack = {},
                    .Path = PathToUtf8(path),
                });
            };
            const auto cancelled = [](zip_t*, void* state)
            { return static_cast<std::stop_token*>(state)->stop_requested() ? 1 : 0; };
            if (zip_register_cancel_callback_with_state(archive.get(), cancelled, nullptr, &stop) !=
                0)
            {
                return failed();
            }
            auto paths = std::vector<std::string>{};
            paths.reserve(pack.Files.size() + 1);
            for (const auto& entry : pack.Files)
            {
                paths.push_back(entry.first);
            }
            paths.emplace_back("pack.mcmeta");
            std::ranges::sort(paths);
            for (const auto& logical : paths)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::CANCELLED,
                        .Message = "Cancelled",
                        .Pack = {},
                        .Path = logical,
                    });
                }
                const auto physical = root / PathFromUtf8(logical);
#ifdef _WIN32
                auto* source =
                    zip_source_win32w(archive.get(), physical.c_str(), 0, ZIP_LENGTH_TO_END);
#else
                auto* source =
                    zip_source_file(archive.get(), physical.c_str(), 0, ZIP_LENGTH_TO_END);
#endif
                if (source == nullptr)
                {
                    return failed();
                }
                const auto index =
                    zip_file_add(archive.get(), logical.c_str(), source, ZIP_FL_ENC_UTF_8);
                if (index < 0)
                {
                    zip_source_free(source);
                    return failed();
                }
                const auto entry = static_cast<zip_uint64_t>(index);
                if (zip_set_file_compression(archive.get(), entry, ZIP_CM_DEFLATE, 6) != 0 ||
                    zip_file_set_mtime(archive.get(), entry, 315532800, 0) != 0 ||
                    zip_file_set_external_attributes(archive.get(), entry, 0, ZIP_OPSYS_UNIX,
                                                     0100644U << 16U) != 0)
                {
                    return failed();
                }
            }
            if (auto* closing = archive.release(); zip_close(closing) != 0)
            {
                archive.reset(closing);
                if (stop.stop_requested())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::CANCELLED,
                        .Message = "Cancelled",
                        .Pack = {},
                        .Path = PathToUtf8(path),
                    });
                }
                return failed();
            }
            return {};
        }

    }

    Result<void> ExportPack(const ResolvedPack& pack, const ExportRequest& request,
                            const std::stop_token& stop)
    {
        if (stop.stop_requested())
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::CANCELLED,
                .Message = "Cancelled",
                .Pack = {},
                .Path = request.Path,
            });
        }
        auto metadata = outputMetadata(pack, request);
        if (!metadata)
        {
            return std::unexpected(metadata.error());
        }
        auto destination = destinationPath(pack, request);
        if (!destination)
        {
            return std::unexpected(destination.error());
        }
        if (auto valid = validateContents(pack, request, *metadata); !valid)
        {
            return valid;
        }
        const auto parent = PathFromUtf8(*destination).parent_path();
        const auto staging = StagingDirectory::Create(parent);
        if (!staging)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::IO,
                .Message = staging.error(),
                .Pack = {},
                .Path = request.Path,
            });
        }
        const auto physicalRoot = staging->Path() / "pack";
        const auto& root = physicalRoot;
        auto directoryError = std::error_code{};
        if (!std::filesystem::create_directory(physicalRoot, directoryError))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::IO,
                .Message = "Cannot create staging directory",
                .Pack = {},
                .Path = PathToUtf8(root),
            });
        }
        if (auto written = writeMetadata(root, *metadata, stop); !written)
        {
            return written;
        }
        if (auto written = writeContents(pack, request, root, stop); !written)
        {
            return written;
        }
        auto prepared = root;
        if (request.Format == OutputFormat::ZIP)
        {
            prepared = staging->Path() / "result.zip";
            if (auto written = writeZip(pack, root, prepared, stop); !written)
            {
                return written;
            }
        }
        if (stop.stop_requested())
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::CANCELLED,
                .Message = "Cancelled",
                .Pack = {},
                .Path = request.Path,
            });
        }
        if (auto valid = CheckPhysicalPath(PathToUtf8(parent)); !valid)
        {
            return valid;
        }
        return PublishNoReplace(prepared, PathFromUtf8(*destination));
    }
}
