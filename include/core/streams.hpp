#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>

namespace PackStacker
{
    class ByteWriter
    {
    public:
        ByteWriter() = default;
        virtual ~ByteWriter() = default;
        ByteWriter(const ByteWriter&) = delete;
        ByteWriter& operator=(const ByteWriter&) = delete;
        ByteWriter(ByteWriter&&) noexcept = default;
        ByteWriter& operator=(ByteWriter&&) noexcept = default;

        virtual bool IsWritable() const = 0;
        virtual std::expected<std::size_t, std::string> Write(std::span<const char> bytes) = 0;
    };

    class MemoryWriter final : public ByteWriter
    {
    public:
        bool IsWritable() const override;
        std::expected<std::size_t, std::string> Write(std::span<const char> value) override;
        const std::string& Bytes() const&;
        std::string Bytes() &&;

    private:
        std::string bytes;
    };

    class FileWriter final : public ByteWriter
    {
    public:
        static std::expected<FileWriter, std::string> Open(const std::filesystem::path& path);
        bool IsWritable() const override;
        std::expected<std::size_t, std::string> Write(std::span<const char> bytes) override;
        std::expected<void, std::string> Flush();
        std::expected<void, std::string> Close();
        std::uint64_t Size() const;

    private:
        using FileStream = std::unique_ptr<std::FILE, int32_t (*)(std::FILE*)>;

        explicit FileWriter(FileStream file);
        FileStream stream;
        std::uint64_t size = 0;
    };

    enum class StreamErrorCode : std::uint8_t
    {
        IO,
        CANCELLED,
    };

    struct StreamError
    {
        StreamErrorCode Code;
        std::string Message;
    };

    std::expected<void, StreamError> WriteBytes(ByteWriter& output, std::span<const char> bytes,
                                                const std::stop_token& stop = {});
}
