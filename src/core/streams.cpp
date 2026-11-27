#include "core/streams.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

namespace PackStacker
{
    bool MemoryWriter::IsWritable() const
    {
        return true;
    }

    std::expected<std::size_t, std::string> MemoryWriter::Write(std::span<const char> value)
    {
        if (value.empty())
        {
            return 0;
        }
        bytes.append(value.begin(), value.end());
        return value.size();
    }

    const std::string& MemoryWriter::Bytes() const&
    {
        return bytes;
    }

    std::string MemoryWriter::Bytes() &&
    {
        return std::move(bytes);
    }

    FileWriter::FileWriter(std::ofstream stream) : stream(std::move(stream))
    {
    }

    std::expected<FileWriter, std::string> FileWriter::Open(const std::filesystem::path& path)
    {
        auto file = std::ofstream(path, std::ios::out | std::ios::binary | std::ios::noreplace);
        if (!file.is_open() || !file.good())
        {
            return std::unexpected("Cannot create output file");
        }
        return FileWriter(std::move(file));
    }

    bool FileWriter::IsWritable() const
    {
        return stream.is_open() && stream.good();
    }

    std::expected<std::size_t, std::string> FileWriter::Write(std::span<const char> bytes)
    {
        if (!IsWritable())
        {
            return std::unexpected("Output file is not writable");
        }
        if (bytes.empty())
        {
            return 0;
        }
        const auto count = std::min(
            bytes.size(), static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()));
        stream.write(bytes.data(), static_cast<std::streamsize>(count));
        if (!stream.good())
        {
            return std::unexpected("Cannot write output file");
        }
        size += count;
        return count;
    }

    std::expected<void, std::string> FileWriter::Flush()
    {
        if (!IsWritable())
        {
            return std::unexpected("Output file is not writable");
        }
        stream.flush();
        if (!stream.good())
        {
            return std::unexpected("Cannot flush output file");
        }
        return {};
    }

    std::expected<void, std::string> FileWriter::Close()
    {
        if (!stream.is_open())
        {
            return std::unexpected("Output file is not open");
        }
        stream.close();
        if (stream.fail())
        {
            return std::unexpected("Cannot close output file");
        }
        return {};
    }

    std::uint64_t FileWriter::Size() const
    {
        return size;
    }

    std::expected<void, StreamError> WriteBytes(ByteWriter& output, std::span<const char> bytes,
                                                const std::stop_token& stop)
    {
        auto offset = std::size_t{0};
        while (offset < bytes.size())
        {
            if (stop.stop_requested())
            {
                return std::unexpected(StreamError{
                    .Code = StreamErrorCode::CANCELLED,
                    .Message = "Cancelled",
                });
            }
            const auto chunk =
                bytes.subspan(offset, std::min<std::size_t>(65536, bytes.size() - offset));
            const auto written = output.Write(chunk);
            if (!written || *written == 0 || *written > chunk.size())
            {
                return std::unexpected(StreamError{
                    .Code = StreamErrorCode::IO,
                    .Message = written ? "Invalid write count" : "Write failed: " + written.error(),
                });
            }
            offset += *written;
        }
        return {};
    }
}
