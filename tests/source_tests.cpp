#include "core/filesystem.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/pack_source.hpp"
#include "core/streams.hpp"
#include "core/zip_archive.hpp"

#include <zip.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void write(const std::filesystem::path& path, const std::string& bytes)
    {
        auto output = PackStacker::FileWriter::Open(path);
        require(output.has_value(), "Cannot open fixture file");
        require(PackStacker::WriteBytes(*output, bytes).has_value(), "Cannot write fixture");
        require(output->Close().has_value(), "Cannot close fixture");
    }

    void checkSource()
    {
        auto temporary =
            PackStacker::StagingDirectory::Create(std::filesystem::temp_directory_path());
        require(temporary.has_value(), "Cannot create temporary directory");
        const auto folder = temporary->Path() / std::filesystem::path(u8"пак-😀");
        std::filesystem::create_directories(folder / "extra");
        const auto metadata = std::string{R"({"pack":{"pack_format":34,"description":"Тест 😀"}})"};
        auto content = std::string(140000, '\0');
        content.replace(0, 5, "\r\n\x1a\xffx", 5);
        const auto logical = std::string{"extra/текст-😀.bin"};
        const auto files = std::map<std::string, std::string>{
            {"pack.mcmeta", metadata},
            {logical, content},
            {".hidden", "hidden"},
        };
        for (const auto& [path, bytes] : files)
        {
            write(folder / PackStacker::PathFromUtf8(path), bytes);
        }
        const auto archivePath =
            PackStacker::PathToUtf8(temporary->Path() / std::filesystem::path(u8"пак-😀.zip"));
        auto archive = PackStacker::OpenZip(archivePath, ZIP_CREATE | ZIP_EXCL);
        require(archive.has_value(), "Cannot create archive");
        for (const auto& [path, bytes] : files)
        {
            auto* source = zip_source_buffer(archive->get(), bytes.data(), bytes.size(), 0);
            require(source != nullptr, "Cannot create ZIP source");
            if (zip_file_add(archive->get(), path.c_str(), source, ZIP_FL_ENC_UTF_8) < 0)
            {
                zip_source_free(source);
                throw std::runtime_error("Cannot add ZIP entry");
            }
        }
        if (auto* closing = archive->release(); zip_close(closing) != 0)
        {
            archive->reset(closing);
            throw std::runtime_error("Cannot close archive");
        }
        for (const auto& path : {PackStacker::PathToUtf8(folder), archivePath})
        {
            const auto source = PackStacker::PackSource::Open(path, {}, {});
            require(source.has_value(), "Cannot open source");
            require((*source)->Entries().size() == files.size(), "Entry count changed");
            for (const auto& [name, bytes] : files)
            {
                const auto read = (*source)->Read(name, bytes.size());
                require(read && *read == bytes, "Source bytes changed");
                auto copied = PackStacker::MemoryWriter{};
                require((*source)->CopyTo(name, copied, bytes.size()).has_value(), "Copy failed");
                require(copied.Bytes() == bytes, "Copied bytes changed");
            }
            const auto inspected = PackStacker::InspectPack(path);
            require(inspected && inspected->Path == (*source)->Path() &&
                        inspected->FileCount == files.size() &&
                        inspected->Formats.Minimum.Major == 34,
                    "Inspection changed");
            const auto cancellation = std::stop_source{};
            cancellation.request_stop();
            const auto cancelled =
                (*source)->Read(logical, content.size(), cancellation.get_token());
            require(!cancelled && cancelled.error().Code == PackStacker::ErrorCode::CANCELLED,
                    "Cancellation ignored");
            const auto limited = (*source)->Read(logical, content.size() - 1);
            require(!limited && limited.error().Code == PackStacker::ErrorCode::LIMIT_EXCEEDED,
                    "Read limit ignored");
        }
        const auto source = PackStacker::PackSource::Open(PackStacker::PathToUtf8(folder), {}, {});
        require(source.has_value(), "Cannot reopen folder");
        const auto physical = folder / PackStacker::PathFromUtf8(logical);
        const auto modified = std::filesystem::last_write_time(physical);
        std::filesystem::last_write_time(physical, modified + std::chrono::seconds{2});
        const auto changed = (*source)->Read(logical, content.size());
        require(!changed && changed.error().Code == PackStacker::ErrorCode::SOURCE_CHANGED,
                "Source timestamp change ignored");
    }

    void checkDirectoryOrder()
    {
        auto temporary =
            PackStacker::StagingDirectory::Create(std::filesystem::temp_directory_path());
        require(temporary.has_value(), "Cannot create temporary directory");
        write(temporary->Path() / std::filesystem::path(u8"\ue000"), "x");
        write(temporary->Path() / std::filesystem::path(u8"😀"), "x");
        auto limits = PackStacker::Limits{};
        limits.MaxFileBytes = 0;
        const auto source =
            PackStacker::PackSource::Open(PackStacker::PathToUtf8(temporary->Path()), limits, {});
        require(!source && source.error().Code == PackStacker::ErrorCode::LIMIT_EXCEEDED &&
                    source.error().Path == "😀",
                "Directory diagnostic order changed from UTF-16 ordering");
    }
}

int32_t main()
{
    try
    {
        checkSource();
        checkDirectoryOrder();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
