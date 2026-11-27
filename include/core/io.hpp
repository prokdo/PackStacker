#pragma once

#include "core/model.hpp"
#include "core/streams.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace PackStacker
{
    inline Result<void> WriteAll(ByteWriter& output, std::span<const char> bytes,
                                 std::string_view pack, std::string_view path,
                                 const std::stop_token& stop)
    {
        if (auto written = WriteBytes(output, bytes, stop); !written)
        {
            return std::unexpected(Diagnostic{
                .Code = written.error().Code == StreamErrorCode::CANCELLED ? ErrorCode::CANCELLED
                                                                           : ErrorCode::IO,
                .Message = written.error().Message,
                .Pack = std::string(pack),
                .Path = std::string(path),
            });
        }
        return {};
    }
}
