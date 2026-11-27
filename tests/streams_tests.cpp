#include "core/io.hpp"
#include "core/model.hpp"
#include "core/streams.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class PartialWriter final : public PackStacker::ByteWriter
    {
    public:
        std::string Bytes;
        bool Writable = true;
        bool Fail = false;
        bool Zero = false;
        bool Oversized = false;
        std::stop_source* Cancellation = nullptr;

        bool IsWritable() const override
        {
            return Writable;
        }

        std::expected<std::size_t, std::string> Write(std::span<const char> bytes) override
        {
            if (Fail || !Writable)
            {
                return std::unexpected("Injected write failure");
            }
            if (Zero)
            {
                return 0;
            }
            if (Oversized)
            {
                return bytes.size() + 1;
            }
            const auto chunk = bytes.first(std::min<std::size_t>(7, bytes.size()));
            Bytes.append(chunk.begin(), chunk.end());
            if (Cancellation != nullptr)
            {
                Cancellation->request_stop();
            }
            return chunk.size();
        }
    };

    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
        {
            const auto base = std::filesystem::temp_directory_path();
            const auto stamp =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            for (auto index = std::size_t{0}; index < 128; ++index)
            {
                root = base / ("packstacker-streams-" + stamp + "-" + std::to_string(index));
                auto error = std::error_code{};
                if (std::filesystem::create_directory(root, error))
                {
                    return;
                }
                if (error)
                {
                    throw std::runtime_error(error.message());
                }
            }
            throw std::runtime_error("Cannot create test directory");
        }

        ~TemporaryDirectory()
        {
            auto error = std::error_code{};
            std::filesystem::remove_all(root, error);
            if (error)
            {
                std::cerr << "Cannot remove test directory: " << error.message() << '\n';
            }
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
        TemporaryDirectory(TemporaryDirectory&&) = delete;
        TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

        const std::filesystem::path& Path() const
        {
            return root;
        }

    private:
        std::filesystem::path root;
    };

    void testMemoryAndPartialWrites()
    {
        auto payload = std::string(200000, 'x');
        payload.replace(0, 6, std::string_view("\0\r\n\x1a\xffz", 6));
        auto memory = PackStacker::MemoryWriter{};
        require(PackStacker::WriteBytes(memory, payload).has_value(), "Memory write failed");
        require(memory.Bytes() == payload, "Memory writer changed binary bytes");
        require(memory.Write({}) == 0 && PackStacker::WriteBytes(memory, {}).has_value(),
                "Empty write failed");
        const auto extracted = std::move(memory).Bytes();
        require(extracted == payload, "Extracted bytes changed");
        auto partial = PartialWriter{};
        require(PackStacker::WriteBytes(partial, payload).has_value(), "Partial write failed");
        require(partial.Bytes == payload, "Partial writes lost bytes");
    }

    void testInvalidAndFailedWrites()
    {
        auto failed = PartialWriter{};
        failed.Fail = true;
        const auto result = PackStacker::WriteBytes(failed, std::string_view("data"));
        require(!result && result.error().Code == PackStacker::StreamErrorCode::IO &&
                    result.error().Message == "Write failed: Injected write failure",
                "Write error changed");
        auto zero = PartialWriter{};
        zero.Zero = true;
        const auto stopped = PackStacker::WriteBytes(zero, std::string_view("data"));
        require(!stopped && stopped.error().Code == PackStacker::StreamErrorCode::IO,
                "Zero progress accepted");
        auto oversized = PartialWriter{};
        oversized.Oversized = true;
        const auto invalid = PackStacker::WriteBytes(oversized, std::string_view("data"));
        require(!invalid && invalid.error().Code == PackStacker::StreamErrorCode::IO,
                "Invalid write count accepted");
        require(failed.Bytes.empty() && zero.Bytes.empty() && oversized.Bytes.empty(),
                "Invalid writers changed bytes");
    }

    void testCancellation()
    {
        const auto stopped = std::stop_source{};
        stopped.request_stop();
        auto untouched = PartialWriter{};
        const auto before =
            PackStacker::WriteBytes(untouched, std::string_view("data"), stopped.get_token());
        require(!before && before.error().Code == PackStacker::StreamErrorCode::CANCELLED &&
                    untouched.Bytes.empty(),
                "Cancelled write had side effects");
        auto stop = std::stop_source{};
        auto partial = PartialWriter{};
        partial.Cancellation = &stop;
        const auto during =
            PackStacker::WriteBytes(partial, std::string(100000, 'x'), stop.get_token());
        require(!during && during.error().Code == PackStacker::StreamErrorCode::CANCELLED &&
                    partial.Bytes.size() == 7,
                "Cancellation did not stop partial writes");
    }

    void testFileWritesNeverReplaceExistingFiles()
    {
        const auto directory = TemporaryDirectory{};
        const auto path = directory.Path() / u8"текст.bin";
        const auto payload = std::string(
            "\0\r\n\x1a\xff"
            "binary",
            11);
        auto file = PackStacker::FileWriter::Open(path);
        require(file.has_value() && file->IsWritable(), "Cannot open output");
        require(PackStacker::WriteBytes(*file, payload).has_value(), "Cannot write output");
        require(file->Size() == payload.size(), "Incorrect output size");
        require(file->Flush().has_value(), "Cannot flush output");
        require(!PackStacker::FileWriter::Open(path), "Open output was overwritten");
        require(file->Close().has_value() && !file->IsWritable(), "Cannot close output");
        require(!PackStacker::FileWriter::Open(path), "Closed output was overwritten");
        require(!file->Write(std::string_view("bad")), "Closed file accepted data");
        require(!file->Flush() && !file->Close(), "Closed file accepted finalization");
        auto input = std::ifstream(path, std::ios::binary);
        const auto bytes = std::string(std::istreambuf_iterator<char>(input), {});
        require(input.is_open() && !input.bad() && bytes == payload, "File bytes changed");
        require(!PackStacker::FileWriter::Open(directory.Path()), "Directory accepted as file");
        require(!PackStacker::FileWriter::Open(directory.Path() / "missing" / "data"),
                "Missing parent accepted");
    }

    void testDiagnosticContext()
    {
        const auto pack = std::string("пакет-😀") + '\0' + "suffix";
        const auto path = std::string("extra/текст-😀.bin");
        const auto payload = std::string_view("data");
        auto failed = PartialWriter{};
        failed.Fail = true;
        const auto written = PackStacker::WriteAll(failed, payload, pack, path, {});
        require(!written && written.error().Code == PackStacker::ErrorCode::IO &&
                    written.error().Message == "Write failed: Injected write failure" &&
                    written.error().Pack == pack && written.error().Path == path,
                "Write failure lost UTF-8 or embedded-null diagnostic context");
        const auto cancellation = std::stop_source{};
        cancellation.request_stop();
        auto untouched = PartialWriter{};
        const auto cancelled =
            PackStacker::WriteAll(untouched, payload, pack, path, cancellation.get_token());
        require(!cancelled && cancelled.error().Code == PackStacker::ErrorCode::CANCELLED &&
                    cancelled.error().Message == "Cancelled" && cancelled.error().Pack == pack &&
                    cancelled.error().Path == path && untouched.Bytes.empty(),
                "Cancellation lost diagnostic context or changed output");
        auto output = PackStacker::MemoryWriter{};
        require(PackStacker::WriteAll(output, payload, pack, path, {}).has_value() &&
                    output.Bytes() == payload,
                "Diagnostic wrapper changed successful writes");
    }

    void testFileWriterMovePreservesOwnership()
    {
        const auto directory = TemporaryDirectory{};
        auto file = PackStacker::FileWriter::Open(directory.Path() / "moved.bin");
        require(file.has_value(), "Cannot open output");
        require(PackStacker::WriteBytes(*file, std::string_view("before")).has_value(),
                "Initial write failed");
        auto moved = std::move(*file);
        require(!file->IsWritable() && !file->Write(std::string_view("invalid")),
                "Moved-from writer retained ownership");
        require(PackStacker::WriteBytes(moved, std::string_view("after")).has_value(),
                "Moved writer lost ownership");
        require(moved.Size() == 11 && moved.Flush().has_value() && moved.Close().has_value(),
                "Moved writer could not finish output");
        auto input = std::ifstream(directory.Path() / "moved.bin", std::ios::binary);
        const auto bytes = std::string(std::istreambuf_iterator<char>(input), {});
        require(input.is_open() && !input.bad() && bytes == "beforeafter", "Moved output changed");
    }
}

int32_t main()
{
    const auto tests = std::vector<std::pair<const char*, std::function<void()>>>{
        {"memory and partial writes", testMemoryAndPartialWrites},
        {"failed and invalid writes", testInvalidAndFailedWrites},
        {"cancellation", testCancellation},
        {"exclusive binary files", testFileWritesNeverReplaceExistingFiles},
        {"standard diagnostic context", testDiagnosticContext},
        {"file writer move", testFileWriterMovePreservesOwnership},
    };
    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "PASS: " << name << '\n';
        }
        catch (const std::exception& error)
        {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
