#include "core/streams.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#endif

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

    FileWriter::FileWriter(FileStream file) : stream(std::move(file))
    {
    }

    std::expected<FileWriter, std::string> FileWriter::Open(const std::filesystem::path& path)
    {
#ifdef _WIN32
        auto descriptor = int32_t{-1};
        if (_wsopen_s(&descriptor, path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                      _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
        {
            return std::unexpected("Cannot create output file");
        }
        auto file = FileStream(_fdopen(descriptor, "wb"), std::fclose);
        if (file == nullptr)
        {
            if (_close(descriptor) != 0)
            {
                return std::unexpected("Cannot close output descriptor");
            }
            return std::unexpected("Cannot open output stream");
        }
#else
        auto file = FileStream(std::fopen(path.c_str(), "wbx"), std::fclose);
        if (file == nullptr)
        {
            return std::unexpected("Cannot create output file");
        }
#endif
        return FileWriter(std::move(file));
    }

    bool FileWriter::IsWritable() const
    {
        return stream != nullptr && std::ferror(stream.get()) == 0;
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
        const auto count = std::fwrite(bytes.data(), 1, bytes.size(), stream.get());
        if (count != bytes.size())
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
        if (std::fflush(stream.get()) != 0)
        {
            return std::unexpected("Cannot flush output file");
        }
        return {};
    }

    std::expected<void, std::string> FileWriter::Close()
    {
        if (stream == nullptr)
        {
            return std::unexpected("Output file is not open");
        }
        if (std::fclose(stream.release()) != 0)
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
