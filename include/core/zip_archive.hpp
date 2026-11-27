#pragma once

#include "core/filesystem.hpp"
#include "core/pack.hpp"
#include "core/unicode.hpp"

#include <zip.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace PackStacker
{
    using ZipArchive = std::unique_ptr<zip_t, decltype(&zip_discard)>;

    inline Result<ZipArchive> OpenZip(std::string_view path,
                                      int32_t flags = ZIP_RDONLY | ZIP_CHECKCONS)
    {
        if (path.empty() || path.contains('\0') || !Utf16FromUtf8(path))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::INVALID_PATH,
                .Message = "Invalid pack path",
                .Pack = std::string(path),
                .Path = {},
            });
        }
        auto error = zip_error_t{};
        zip_error_init(&error);
#ifdef _WIN32
        const auto name = PathFromUtf8(path);
        auto* source = zip_source_win32w_create(name.c_str(), 0, ZIP_LENGTH_TO_END, &error);
#else
        const auto name = std::string(path);
        auto* source = zip_source_file_create(name.c_str(), 0, ZIP_LENGTH_TO_END, &error);
#endif
        auto* archive = (source != nullptr) ? zip_open_from_source(source, flags, &error) : nullptr;
        if (archive == nullptr)
        {
            const auto message = std::string(zip_error_strerror(&error));
            if (source != nullptr)
            {
                zip_source_free(source);
            }
            zip_error_fini(&error);
            return std::unexpected(Diagnostic{
                .Code = ((static_cast<std::uint32_t>(flags) & ZIP_CREATE) != 0U)
                            ? ErrorCode::IO
                            : ErrorCode::INVALID_ARCHIVE,
                .Message = message,
                .Pack = std::string(path),
                .Path = {},
            });
        }
        zip_error_fini(&error);
        return ZipArchive(archive, zip_discard);
    }
}
