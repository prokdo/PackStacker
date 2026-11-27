#include "core/filesystem.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/streams.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void write(const std::filesystem::path& path, std::string_view bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        auto output = PackStacker::FileWriter::Open(path);
        require(output.has_value(), "Cannot open fixture");
        require(PackStacker::WriteBytes(*output, bytes).has_value(), "Cannot write fixture");
        require(output->Close().has_value(), "Cannot close fixture");
    }

    void checkPipeline(const std::filesystem::path& root)
    {
        const auto source = root / std::filesystem::path(u8"пак-😀");
        write(source / "pack.mcmeta", R"({"pack":{"pack_format":34,"description":"input"}})");
        const auto content = std::string{"binary\0\r\n\x1a\xff", 11};
        write(source / std::filesystem::path(u8"extra/текст-😀.bin"), content);
        const auto inspected = PackStacker::InspectPack(PackStacker::PathToUtf8(source));
        require(inspected && inspected->FileCount == 2, "Source inspection failed");
        const auto resolved = PackStacker::ResolveStack({
            .PacksHighToLow = {PackStacker::PathToUtf8(source)},
            .Type = PackStacker::PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        });
        require(resolved.has_value(), "Source resolution failed");
        for (const auto format :
             std::array{PackStacker::OutputFormat::DIRECTORY, PackStacker::OutputFormat::ZIP})
        {
            const auto output = root / (format == PackStacker::OutputFormat::DIRECTORY
                                            ? std::filesystem::path(u8"пак-😀-export")
                                            : std::filesystem::path(u8"архив-😀.zip"));
            const auto request = PackStacker::ExportRequest{
                .Path = PackStacker::PathToUtf8(output),
                .Format = format,
                .Description = "Описание 😀",
                .Limits = {},
            };
            require(PackStacker::ExportPack(*resolved, request).has_value(), "Export failed");
            const auto repeated = PackStacker::ExportPack(*resolved, request);
            require(!repeated && repeated.error().Code == PackStacker::ErrorCode::OUTPUT_EXISTS,
                    "Existing output was not protected");
            const auto roundTrip = PackStacker::ResolveStack({
                .PacksHighToLow = {request.Path},
                .Type = PackStacker::PackType::RESOURCE,
                .Target = {.Major = 34, .Minor = 0},
                .Limits = {},
                .Rules = {},
            });
            require(roundTrip.has_value(), "Exported pack cannot be resolved");
            const auto bytes = roundTrip->Files.at("extra/текст-😀.bin").Read(1024);
            require(bytes && *bytes == content, "Export changed binary content");
        }
        const auto overlap = PackStacker::ExportPack(
            *resolved, {
                           .Path = PackStacker::PathToUtf8(source / "output"),
                           .Format = PackStacker::OutputFormat::DIRECTORY,
                           .Description = "overlap",
                           .Limits = {},
                       });
        require(!overlap && overlap.error().Code == PackStacker::ErrorCode::INVALID_PATH,
                "Output inside input was not rejected");
    }

    void checkReservedMetadata(const std::filesystem::path& root)
    {
        auto resolved = PackStacker::ResolvedPack{
            .Type = PackStacker::PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        resolved.Files.emplace(
            "pacK.mcmeta",
            PackStacker::ResolvedFile(
                {}, 1, [](std::uint64_t, const std::stop_token&) -> PackStacker::Result<std::string>
                { return "x"; }));
        const auto rejected = PackStacker::ExportPack(
            resolved, {
                          .Path = PackStacker::PathToUtf8(root / "reserved"),
                          .Format = PackStacker::OutputFormat::DIRECTORY,
                          .Description = "reserved",
                          .Limits = {},
                      });
        require(!rejected && rejected.error().Code == PackStacker::ErrorCode::INVALID_PATH &&
                    rejected.error().Path == "pacK.mcmeta",
                "Unicode alias bypassed reserved metadata protection");
    }
}

int32_t main()
{
    try
    {
        auto directory =
            PackStacker::StagingDirectory::Create(std::filesystem::temp_directory_path());
        require(directory.has_value(), "Cannot create temporary directory");
        checkPipeline(directory->Path());
        checkReservedMetadata(directory->Path());
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
