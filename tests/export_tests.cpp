#include "core/filesystem.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/streams.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using PackStacker::Diagnostic;
    using PackStacker::ErrorCode;
    using PackStacker::ExportPack;
    using PackStacker::ExportRequest;
    using PackStacker::InspectPack;
    using PackStacker::OutputFormat;
    using PackStacker::PackFormat;
    using PackStacker::PackType;
    using PackStacker::ResolvedFile;
    using PackStacker::ResolvedPack;
    using PackStacker::ResolveStack;
    using PackStacker::Result;
    using Json = nlohmann::json;
    using Files = std::vector<std::pair<std::string, std::string>>;

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename T>
    T take(Result<T> result)
    {
        if (!result)
        {
            throw std::runtime_error(result.error().Message + " [" + result.error().Path + "]");
        }
        return std::move(*result);
    }

    void take(Result<void> result)
    {
        if (!result)
        {
            throw std::runtime_error(result.error().Message + " [" + result.error().Path + "]");
        }
    }

    template <typename T>
    void expectError(const Result<T>& result, ErrorCode code)
    {
        require(!result, "Expected an error");
        if (result.error().Code != code)
        {
            throw std::runtime_error("Unexpected error: " + result.error().Message);
        }
    }

    void write(std::string_view path, std::string_view bytes)
    {
        const auto physical = PackStacker::PathFromUtf8(path);
        std::filesystem::create_directories(physical.parent_path());
        auto file = std::ofstream(physical, std::ios::binary | std::ios::trunc);
        require(file.is_open(), "Cannot open fixture file");
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        require(!file.fail(), "Cannot write fixture file");
    }

    std::string read(std::string_view path)
    {
        auto file = std::ifstream(PackStacker::PathFromUtf8(path), std::ios::binary);
        require(file.is_open(), "Cannot read fixture file");
        auto bytes = std::string{};
        auto buffer = std::array<char, 65536>{};
        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            bytes.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
        }
        require(file.eof() && !file.bad(), "Fixture read failed");
        return bytes;
    }

    bool exists(std::string_view path)
    {
        return std::filesystem::exists(PackStacker::PathFromUtf8(path));
    }

    bool makeDirectory(std::string_view path)
    {
        return std::filesystem::create_directory(PackStacker::PathFromUtf8(path));
    }

    bool emptyDirectory(std::string_view path)
    {
        return std::filesystem::is_empty(PackStacker::PathFromUtf8(path));
    }

    PackStacker::StagingDirectory temporaryDirectory()
    {
        auto directory =
            PackStacker::StagingDirectory::Create(std::filesystem::temp_directory_path());
        if (!directory)
        {
            throw std::runtime_error(directory.error());
        }
        return std::move(*directory);
    }

    ResolvedFile generated(const std::string& bytes)
    {
        return {{},
                static_cast<std::uint64_t>(bytes.size()),
                [bytes](std::uint64_t, const std::stop_token&) -> Result<std::string>
                { return bytes; }};
    }

    struct Fixture
    {
        PackStacker::StagingDirectory Temporary;

        Fixture() : Temporary(temporaryDirectory())
        {
        }

        std::string Path(std::string_view name) const
        {
            return PackStacker::PathToUtf8(Temporary.Path() / PackStacker::PathFromUtf8(name));
        }

        ResolvedPack Resolve(const Files& files = {}) const
        {
            const auto root = Path("input");
            write(root + "/pack.mcmeta",
                  R"({"pack":{"pack_format":34,"description":"Test pack"}})");
            for (const auto& [name, bytes] : files)
            {
                auto path = root;
                path.append("/").append(name);
                write(path, bytes);
            }
            return take(ResolveStack({
                .PacksHighToLow = {root},
                .Type = PackType::RESOURCE,
                .Target = {.Major = 34, .Minor = 0},
                .Limits = {},
                .Rules = {},
            }));
        }

        void Clean() const
        {
            for (const auto& entry : std::filesystem::directory_iterator(Temporary.Path()))
            {
                require(!entry.is_directory() || !PackStacker::PathToUtf8(entry.path().filename())
                                                      .starts_with(".packstacker-"),
                        "Staging directory leaked");
            }
        }
    };

    constexpr auto LanguagePath = "assets/example/lang/en_us.json";
    constexpr auto TexturePath = "assets/example/textures/block/test.png";

    std::string content(const ResolvedPack& pack, std::string_view path)
    {
        const auto entry = pack.Files.find(std::string(path));
        require(entry != pack.Files.cend(), "Missing exported resource");
        return take(entry->second.Read(std::uint64_t{4} * 1024 * 1024));
    }

    void testDirectoryZIPExportParityAndRoundTrip()
    {
        const auto fixture = Fixture{};
        const auto binary = std::string("\0\1\xff\n", 4);
        const auto pack = fixture.Resolve({
            {LanguagePath, R"({"a":"value"})"},
            {TexturePath, binary},
            {"NOTICE.txt", "notice"},
            {"pack.png", "icon"},
            {"extra/текст-😀.bin", binary},
        });
        const auto parent = fixture.Path("экспорт-😀");
        require(makeDirectory(parent), "Cannot create Unicode output parent");
        const auto folder = parent + std::string("/готовый пак");
        const auto zip = parent + std::string("/готовый пак.zip");
        const auto description = std::string("Описание \"пака\"\nНовая строка");
        take(ExportPack(pack, {
                                  .Path = folder,
                                  .Format = OutputFormat::DIRECTORY,
                                  .Description = description,
                                  .Limits = {},
                              }));
        take(ExportPack(pack, {
                                  .Path = zip,
                                  .Format = OutputFormat::ZIP,
                                  .Description = description,
                                  .Limits = {},
                              }));
        const auto directoryPack = take(ResolveStack({
            .PacksHighToLow = {folder},
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        }));
        const auto zipPack = take(ResolveStack({
            .PacksHighToLow = {zip},
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        }));
        require(std::ranges::equal(pack.Files, directoryPack.Files, {},
                                   &decltype(pack.Files)::value_type::first,
                                   &decltype(directoryPack.Files)::value_type::first),
                "Directory paths differ");
        require(std::ranges::equal(pack.Files, zipPack.Files, {},
                                   &decltype(pack.Files)::value_type::first,
                                   &decltype(zipPack.Files)::value_type::first),
                "ZIP has an extra wrapper directory");
        for (auto entry = pack.Files.cbegin(); entry != pack.Files.cend(); ++entry)
        {
            const auto path = entry->first;
            require(content(pack, path) == content(directoryPack, path), "Directory bytes differ");
            require(content(pack, path) == content(zipPack, path), "ZIP bytes differ");
        }
        const auto meta = Json::parse(read(folder + "/pack.mcmeta"));
        require(meta.at("pack").at("description") == description, "Description changed");
        require(read(folder + '/' + TexturePath) == binary, "Binary texture changed");
        fixture.Clean();
    }

    void testDeterministicZIPBytes()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve({{TexturePath, std::string(100000, 'x')}});
        const auto first = fixture.Path("first.zip");
        const auto second = fixture.Path("second.zip");
        take(ExportPack(pack, {
                                  .Path = first,
                                  .Format = OutputFormat::ZIP,
                                  .Description = "same",
                                  .Limits = {},
                              }));
        take(ExportPack(pack, {
                                  .Path = second,
                                  .Format = OutputFormat::ZIP,
                                  .Description = "same",
                                  .Limits = {},
                              }));
        require(read(first) == read(second), "Repeated ZIP export is not deterministic");
        fixture.Clean();
    }

    void testMetadataSchemasAndExactModernTarget()
    {
        const auto fixture = Fixture{};
        const auto targets = std::vector<std::pair<PackType, PackFormat>>{
            {PackType::RESOURCE, {.Major = 64, .Minor = 0}},
            {PackType::RESOURCE, {.Major = 65, .Minor = 3}},
            {PackType::DATA, {.Major = 81, .Minor = 0}},
            {PackType::DATA, {.Major = 82, .Minor = 1}},
        };
        auto index = std::size_t{0};
        for (const auto& [type, target] : targets)
        {
            const auto pack = ResolvedPack{
                .Type = type,
                .Target = target,
                .Files = {},
                .SourcePaths = {},
                .Languages = {},
                .Filters = {},
            };
            const auto output = fixture.Path(std::to_string(index++));
            take(ExportPack(pack, {
                                      .Path = output,
                                      .Format = OutputFormat::DIRECTORY,
                                      .Description = "test",
                                      .Limits = {},
                                  }));
            const auto meta = Json::parse(read(output + "/pack.mcmeta")).at("pack");
            const auto modern =
                (type == PackType::RESOURCE ? target.Major >= 65 : target.Major >= 82);
            if (modern)
            {
                require(!meta.contains("pack_format"), "Legacy format leaked into modern metadata");
                require(meta.at("min_format") == Json::array({target.Major, target.Minor}),
                        "Wrong minimum format");
                require(meta.at("min_format") == meta.at("max_format"),
                        "Output claims untested format range");
            }
            else
            {
                require(meta.at("pack_format") == target.Major && !meta.contains("min_format"),
                        "Wrong legacy schema");
            }
            const auto inspected = take(InspectPack(output));
            require(inspected.Formats.Contains(target), "Exported format does not round trip");
        }
        fixture.Clean();
    }

    void testExportedDataPackRetainsReplacementFlag()
    {
        const auto fixture = Fixture{};
        const auto tag = std::string("data/example/tags/function/load.json");
        auto pack = ResolvedPack{
            .Type = PackType::DATA,
            .Target = {.Major = 48, .Minor = 0},
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        pack.Files.insert_or_assign(tag,
                                    generated(R"({"replace":true,"values":["example:load"]})"));
        const auto output = fixture.Path("data.zip");
        take(ExportPack(pack, {
                                  .Path = output,
                                  .Format = OutputFormat::ZIP,
                                  .Description = "data",
                                  .Limits = {},
                              }));
        const auto roundTrip = take(ResolveStack({
            .PacksHighToLow = {output},
            .Type = PackType::DATA,
            .Target = {.Major = 48, .Minor = 0},
            .Limits = {},
            .Rules = {},
        }));
        require(Json::parse(content(roundTrip, tag)).at("replace") == true, "Tag reset lost");
        fixture.Clean();
    }

    void testExistingFilesAndDirectoriesAreNeverReplaced()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve();
        const auto existingFile = fixture.Path("existing.zip");
        write(existingFile, "sentinel");
        const auto existingDirectory = fixture.Path("existing-directory");
        require(makeDirectory(existingDirectory), "Cannot create existing directory");
        for (const auto format : {OutputFormat::DIRECTORY, OutputFormat::ZIP})
        {
            expectError(ExportPack(pack,
                                   {
                                       .Path = existingFile,
                                       .Format = format,
                                       .Description = "test",
                                       .Limits = {},
                                   }),
                        ErrorCode::OUTPUT_EXISTS);
            expectError(ExportPack(pack,
                                   {
                                       .Path = existingDirectory,
                                       .Format = format,
                                       .Description = "test",
                                       .Limits = {},
                                   }),
                        ErrorCode::OUTPUT_EXISTS);
        }
        require(read(existingFile) == "sentinel", "Existing file changed");
        require(emptyDirectory(existingDirectory), "Existing directory changed");
        fixture.Clean();
    }

    void testInputOverlapIncludesEmptyPacksAndDroppedContributors()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve();
        for (const auto& output : {
                 fixture.Path("input"),
                 fixture.Path("input/nested.zip"),
                 PackStacker::PathToUtf8(fixture.Temporary.Path()),
             })
        {
            expectError(ExportPack(pack,
                                   {
                                       .Path = output,
                                       .Format = OutputFormat::ZIP,
                                       .Description = "test",
                                       .Limits = {},
                                   }),
                        ErrorCode::INVALID_PATH);
        }
        require(!exists(fixture.Path("input/nested.zip")), "Output created inside input");
        const auto sibling = fixture.Path("input-output");
        take(ExportPack(pack, {
                                  .Path = sibling,
                                  .Format = OutputFormat::DIRECTORY,
                                  .Description = "test",
                                  .Limits = {},
                              }));
        fixture.Clean();
    }

    void testFullyShadowedInputsRemainProtected()
    {
        const auto fixture = Fixture{};
        fixture.Resolve({{TexturePath, "high"}});
        const auto lower = fixture.Path("lower");
        write(lower + "/pack.mcmeta", R"({"pack":{"pack_format":34,"description":"lower"}})");
        write(lower + '/' + TexturePath, "low");
        const auto pack = take(ResolveStack({
            .PacksHighToLow = {fixture.Path("input"), lower},
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        }));
        const auto entry = pack.Files.find(TexturePath);
        require(
            entry != pack.Files.cend() && std::ranges::find(entry->second.SourcePacks(), lower) ==
                                              entry->second.SourcePacks().end(),
            "Lower file was not shadowed");
        expectError(ExportPack(pack,
                               {
                                   .Path = (lower + "/result.zip"),
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::INVALID_PATH);
        require(read(lower + '/' + TexturePath) == "low", "Shadowed source changed");
        fixture.Clean();
    }

    void testZIPInputCannotBeItsOwnOutput()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve({{TexturePath, "png"}});
        const auto zip = fixture.Path("input.zip");
        take(ExportPack(pack, {
                                  .Path = zip,
                                  .Format = OutputFormat::ZIP,
                                  .Description = "test",
                                  .Limits = {},
                              }));
        const auto before = read(zip);
        const auto fromZip = take(ResolveStack({
            .PacksHighToLow = {zip},
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        }));
        expectError(ExportPack(fromZip,
                               {
                                   .Path = zip,
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::INVALID_PATH);
        require(read(zip) == before, "ZIP input changed");
        fixture.Clean();
    }

    void testPreflightLimitsDoNotCreateOutputs()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve({{TexturePath, "png"}});
        const auto output = fixture.Path("result.zip");
        auto request = ExportRequest{
            .Path = output,
            .Format = OutputFormat::ZIP,
            .Description = "test",
            .Limits = {},
        };
        request.Limits.MaxEntries = 1;
        expectError(ExportPack(pack, request), ErrorCode::LIMIT_EXCEEDED);
        request.Limits = {};
        request.Limits.MaxFileBytes = 1;
        expectError(ExportPack(pack, request), ErrorCode::LIMIT_EXCEEDED);
        request.Limits = {};
        request.Limits.MaxTotalBytes = 1;
        expectError(ExportPack(pack, request), ErrorCode::LIMIT_EXCEEDED);
        request.Limits = {};
        request.Limits.MaxJsonBytes = 2;
        expectError(ExportPack(pack, request), ErrorCode::LIMIT_EXCEEDED);
        require(!exists(output), "Failed preflight created output");
        fixture.Clean();
    }

    void testEntryLimitIncludesImplicitDirectoriesAndGeneratedMetadata()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve({{TexturePath, "png"}});
        auto request = ExportRequest{
            .Path = fixture.Path("result"),
            .Format = OutputFormat::DIRECTORY,
            .Description = "test",
            .Limits = {},
        };
        request.Limits.MaxEntries = 2;
        expectError(ExportPack(pack, request), ErrorCode::LIMIT_EXCEEDED);
        request.Limits.MaxEntries = 6;
        take(ExportPack(pack, request));
        take(InspectPack(request.Path, request.Limits));
        fixture.Clean();
    }

    void testInvalidUnicodePathsAndDescriptionsAreRejected()
    {
        const auto fixture = Fixture{};
        const auto output = fixture.Path("result");
        auto pack = ResolvedPack{
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        pack.Files.insert_or_assign("invalid-\xed\xa0\x80", generated("test"));
        expectError(ExportPack(pack,
                               {
                                   .Path = output,
                                   .Format = OutputFormat::DIRECTORY,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::INVALID_PATH);
        pack.Files.clear();
        expectError(ExportPack(pack,
                               {
                                   .Path = output,
                                   .Format = OutputFormat::DIRECTORY,
                                   .Description = "invalid-\xed\xa0\x80",
                                   .Limits = {},
                               }),
                    ErrorCode::INVALID_METADATA);
        require(!exists(output), "Invalid Unicode created output");
        for (const auto& invalidPath : {
                 std::string("invalid-\xed\xa0\x80"),
                 std::string("invalid-\xc0\xaf"),
                 std::string("invalid-\xf4\x90\x80\x80"),
                 std::string("invalid\0path", 12),
             })
        {
            const auto result = ExportPack(pack, {
                                                     .Path = invalidPath,
                                                     .Format = OutputFormat::DIRECTORY,
                                                     .Description = "test",
                                                     .Limits = {},
                                                 });
            expectError(result, ErrorCode::INVALID_PATH);
            require(result.error().Path == invalidPath, "Invalid UTF-8 path context was changed");
            pack.SourcePaths = {invalidPath};
            const auto invalidSource = ExportPack(pack, {
                                                            .Path = output,
                                                            .Format = OutputFormat::DIRECTORY,
                                                            .Description = "test",
                                                            .Limits = {},
                                                        });
            expectError(invalidSource, ErrorCode::INVALID_PATH);
            require(invalidSource.error().Pack == invalidPath,
                    "Invalid UTF-8 source path context was changed");
            pack.SourcePaths.clear();
            pack.Files.insert_or_assign(invalidPath, generated("test"));
            const auto invalidEntry = ExportPack(pack, {
                                                           .Path = output,
                                                           .Format = OutputFormat::DIRECTORY,
                                                           .Description = "test",
                                                           .Limits = {},
                                                       });
            expectError(invalidEntry, ErrorCode::INVALID_PATH);
            require(invalidEntry.error().Path == invalidPath,
                    "Invalid UTF-8 entry path context was changed");
            pack.Files.clear();
        }
        const auto description = std::string("before\0after", 12) + "Описание 😀";
        take(ExportPack(pack, {
                                  .Path = output,
                                  .Format = OutputFormat::DIRECTORY,
                                  .Description = description,
                                  .Limits = {},
                              }));
        const auto metadata = Json::parse(read(output + "/pack.mcmeta"));
        require(metadata.at("pack").at("description") == description,
                "UTF-8 description or embedded NUL was changed");
        fixture.Clean();
    }

    void testUnsafeAndReservedOutputEntriesAreRevalidated()
    {
        const auto fixture = Fixture{};
        const auto output = fixture.Path("result");
        for (const auto& name :
             {"../escape", "pack.mcmeta", "PACK.MCMETA", "pack.mcmeta/child", "NUL.txt"})
        {
            auto pack = ResolvedPack{
                .Type = PackType::RESOURCE,
                .Target = {.Major = 34, .Minor = 0},
                .Files = {},
                .SourcePaths = {},
                .Languages = {},
                .Filters = {},
            };
            pack.Files.insert_or_assign(name, generated("data"));
            expectError(ExportPack(pack,
                                   {
                                       .Path = output,
                                       .Format = OutputFormat::DIRECTORY,
                                       .Description = "test",
                                       .Limits = {},
                                   }),
                        ErrorCode::INVALID_PATH);
        }
        auto colliding = ResolvedPack{
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        colliding.Files.insert_or_assign("Readme", generated("one"));
        colliding.Files.insert_or_assign("README", generated("two"));
        expectError(ExportPack(colliding,
                               {
                                   .Path = output,
                                   .Format = OutputFormat::DIRECTORY,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::INVALID_PATH);
        require(!exists(output), "Unsafe plan created output");
        fixture.Clean();
    }

    void testInvalidDestinationAndMissingParent()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve();
        expectError(ExportPack(pack,
                               {
                                   .Path = "",
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::INVALID_PATH);
        expectError(ExportPack(pack,
                               {
                                   .Path = fixture.Path("NUL.zip"),
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::INVALID_PATH);
        expectError(ExportPack(pack,
                               {
                                   .Path = fixture.Path("missing/result.zip"),
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::IO);
        require(!exists(fixture.Path("missing")), "Missing parent created implicitly");
        fixture.Clean();
    }

    void testChangedSourceLeavesNoPartialOutput()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve({{TexturePath, "before"}});
        write(fixture.Path("input/") + TexturePath, "changed-size");
        for (const auto format : {OutputFormat::DIRECTORY, OutputFormat::ZIP})
        {
            const auto output = fixture.Path(format == OutputFormat::ZIP ? "result.zip" : "result");
            expectError(ExportPack(pack,
                                   {
                                       .Path = output,
                                       .Format = format,
                                       .Description = "test",
                                       .Limits = {},
                                   }),
                        ErrorCode::SOURCE_CHANGED);
            require(!exists(output), "Changed source published partial output");
            fixture.Clean();
        }
    }

    void testExportUsesTheStreamingCopierRatherThanMaterializingTheReader()
    {
        const auto fixture = Fixture{};
        auto pack = ResolvedPack{
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        pack.Files.insert_or_assign(
            "NOTICE", ResolvedFile(
                          {}, 3,
                          [](std::uint64_t, const std::stop_token&) -> Result<std::string>
                          {
                              return std::unexpected(Diagnostic{
                                  .Code = ErrorCode::IO,
                                  .Message = "Reader must not be used",
                                  .Pack = {},
                                  .Path = {},
                              });
                          },
                          [](PackStacker::ByteWriter& output, std::uint64_t,
                             const std::stop_token&) -> Result<void>
                          {
                              if (output.Write(std::span("abc", 3)) != 3)
                              {
                                  return std::unexpected(Diagnostic{
                                      .Code = ErrorCode::IO,
                                      .Message = "Write failed",
                                      .Pack = {},
                                      .Path = {},
                                  });
                              }
                              return {};
                          }));
        const auto output = fixture.Path("streamed");
        take(ExportPack(pack, {
                                  .Path = output,
                                  .Format = OutputFormat::DIRECTORY,
                                  .Description = "test",
                                  .Limits = {},
                              }));
        require(read(output + "/NOTICE") == "abc", "Streamed data changed");
        fixture.Clean();
    }

    void testCopyFailureCleansStagingWithoutPublishing()
    {
        const auto fixture = Fixture{};
        auto pack = ResolvedPack{
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        pack.Files.insert_or_assign("NOTICE",
                                    ResolvedFile({}, 3, {},
                                                 [](PackStacker::ByteWriter& output, std::uint64_t,
                                                    const std::stop_token&) -> Result<void>
                                                 {
                                                     if (output.Write(std::span("a", 1)) != 1)
                                                     {
                                                         return std::unexpected(Diagnostic{
                                                             .Code = ErrorCode::IO,
                                                             .Message = "Write failed",
                                                             .Pack = {},
                                                             .Path = {},
                                                         });
                                                     }
                                                     return std::unexpected(Diagnostic{
                                                         .Code = ErrorCode::IO,
                                                         .Message = "Injected copy failure",
                                                         .Pack = {},
                                                         .Path = {},
                                                     });
                                                 }));
        const auto output = fixture.Path("result.zip");
        expectError(ExportPack(pack,
                               {
                                   .Path = output,
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::IO);
        require(!exists(output), "Failed copy published output");
        fixture.Clean();
    }

    void testCopierSizeMismatchIsNotPublished()
    {
        const auto fixture = Fixture{};
        auto pack = ResolvedPack{
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        pack.Files.insert_or_assign(
            "NOTICE", ResolvedFile({}, 3, {},
                                   [](PackStacker::ByteWriter& output, std::uint64_t,
                                      const std::stop_token&) -> Result<void>
                                   {
                                       if (output.Write(std::span("wrong-size", 10)) != 10)
                                       {
                                           return std::unexpected(Diagnostic{
                                               .Code = ErrorCode::IO,
                                               .Message = "Write failed",
                                               .Pack = {},
                                               .Path = {},
                                           });
                                       }
                                       return {};
                                   }));
        const auto output = fixture.Path("result");
        expectError(ExportPack(pack,
                               {
                                   .Path = output,
                                   .Format = OutputFormat::DIRECTORY,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::SOURCE_CHANGED);
        require(!exists(output), "Wrong-size content published");
        fixture.Clean();
    }

    void testCancellationAfterCopyingIsNotPublished()
    {
        const auto fixture = Fixture{};
        for (const auto format : {OutputFormat::DIRECTORY, OutputFormat::ZIP})
        {
            auto stop = std::stop_source{};
            auto pack = ResolvedPack{
                .Type = PackType::RESOURCE,
                .Target = {.Major = 34, .Minor = 0},
                .Files = {},
                .SourcePaths = {},
                .Languages = {},
                .Filters = {},
            };
            pack.Files.insert_or_assign(
                "NOTICE", ResolvedFile({}, 3, {},
                                       [&stop](PackStacker::ByteWriter& output, std::uint64_t,
                                               const std::stop_token&) -> Result<void>
                                       {
                                           if (output.Write(std::span("abc", 3)) != 3)
                                           {
                                               return std::unexpected(Diagnostic{
                                                   .Code = ErrorCode::IO,
                                                   .Message = "Write failed",
                                                   .Pack = {},
                                                   .Path = {},
                                               });
                                           }
                                           stop.request_stop();
                                           return {};
                                       }));
            const auto output = fixture.Path(format == OutputFormat::ZIP ? "result.zip" : "result");
            expectError(ExportPack(pack,
                                   {
                                       .Path = output,
                                       .Format = format,
                                       .Description = "test",
                                       .Limits = {},
                                   },
                                   stop.get_token()),
                        ErrorCode::CANCELLED);
            require(!exists(output), "Cancelled operation published output");
            fixture.Clean();
        }
    }

    void testCancelledRequestHasNoSideEffects()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Resolve();
        const auto stop = std::stop_source{};
        stop.request_stop();
        const auto output = fixture.Path("result.zip");
        expectError(ExportPack(pack,
                               {
                                   .Path = output,
                                   .Format = OutputFormat::ZIP,
                                   .Description = "test",
                                   .Limits = {},
                               },
                               stop.get_token()),
                    ErrorCode::CANCELLED);
        require(!exists(output), "Cancelled request created output");
        fixture.Clean();
    }

    void testDestinationAppearingAfterPreflightIsNotOverwritten()
    {
        const auto fixture = Fixture{};
        for (const auto format : {OutputFormat::DIRECTORY, OutputFormat::ZIP})
        {
            const auto destination =
                fixture.Path(format == OutputFormat::ZIP ? "competitor.zip" : "competitor");
            auto pack = ResolvedPack{
                .Type = PackType::RESOURCE,
                .Target = {.Major = 34, .Minor = 0},
                .Files = {},
                .SourcePaths = {},
                .Languages = {},
                .Filters = {},
            };
            pack.Files.insert_or_assign(
                "NOTICE",
                ResolvedFile({}, 3, {},
                             [destination, format](PackStacker::ByteWriter& output, std::uint64_t,
                                                   const std::stop_token&) -> Result<void>
                             {
                                 if (output.Write(std::span("abc", 3)) != 3)
                                 {
                                     return std::unexpected(Diagnostic{
                                         .Code = ErrorCode::IO,
                                         .Message = "Write failed",
                                         .Pack = {},
                                         .Path = {},
                                     });
                                 }
                                 if (format == OutputFormat::ZIP)
                                 {
                                     write(destination, "competitor");
                                 }
                                 else
                                 {
                                     require(makeDirectory(destination),
                                             "Cannot create competing directory");
                                 }
                                 return {};
                             }));
            expectError(ExportPack(pack,
                                   {
                                       .Path = destination,
                                       .Format = format,
                                       .Description = "test",
                                       .Limits = {},
                                   }),
                        ErrorCode::OUTPUT_EXISTS);
            if (format == OutputFormat::ZIP)
            {
                require(read(destination) == "competitor", "Competing file was overwritten");
            }
            else
            {
                require(emptyDirectory(destination), "Competing empty directory was replaced");
            }
            fixture.Clean();
        }
    }

    void testLegacyMinorTargetIsRejected()
    {
        const auto fixture = Fixture{};
        const auto pack = ResolvedPack{
            .Type = PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 1},
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        expectError(ExportPack(pack,
                               {
                                   .Path = fixture.Path("result"),
                                   .Format = OutputFormat::DIRECTORY,
                                   .Description = "test",
                                   .Limits = {},
                               }),
                    ErrorCode::UNSUPPORTED_FEATURE);
        fixture.Clean();
    }

}

int32_t main()
{
    const auto tests = std::vector<std::pair<const char*, std::function<void()>>>{
        {
            "directory/ZIP export parity and round trip",
            testDirectoryZIPExportParityAndRoundTrip,
        },
        {
            "deterministic ZIP bytes",
            testDeterministicZIPBytes,
        },
        {
            "metadata schemas and exact modern target",
            testMetadataSchemasAndExactModernTarget,
        },
        {
            "exported data pack retains replacement flag",
            testExportedDataPackRetainsReplacementFlag,
        },
        {
            "existing files and directories are never replaced",
            testExistingFilesAndDirectoriesAreNeverReplaced,
        },
        {
            "input overlap includes empty packs and dropped contributors",
            testInputOverlapIncludesEmptyPacksAndDroppedContributors,
        },
        {
            "fully shadowed inputs remain protected",
            testFullyShadowedInputsRemainProtected,
        },
        {
            "ZIP input cannot be its own output",
            testZIPInputCannotBeItsOwnOutput,
        },
        {
            "preflight limits do not create outputs",
            testPreflightLimitsDoNotCreateOutputs,
        },
        {
            "entry limit includes implicit directories and generated metadata",
            testEntryLimitIncludesImplicitDirectoriesAndGeneratedMetadata,
        },
        {
            "invalid Unicode paths and descriptions are rejected",
            testInvalidUnicodePathsAndDescriptionsAreRejected,
        },
        {
            "unsafe and reserved output entries are revalidated",
            testUnsafeAndReservedOutputEntriesAreRevalidated,
        },
        {
            "invalid destination and missing parent",
            testInvalidDestinationAndMissingParent,
        },
        {
            "changed source leaves no partial output",
            testChangedSourceLeavesNoPartialOutput,
        },
        {
            "export uses the streaming copier rather than materializing the reader",
            testExportUsesTheStreamingCopierRatherThanMaterializingTheReader,
        },
        {
            "copy failure cleans staging without publishing",
            testCopyFailureCleansStagingWithoutPublishing,
        },
        {
            "copier size mismatch is not published",
            testCopierSizeMismatchIsNotPublished,
        },
        {
            "cancellation after copying is not published",
            testCancellationAfterCopyingIsNotPublished,
        },
        {
            "cancelled request has no side effects",
            testCancelledRequestHasNoSideEffects,
        },
        {
            "destination appearing after preflight is not overwritten",
            testDestinationAppearingAfterPreflightIsNotOverwritten,
        },
        {
            "legacy minor target is rejected",
            testLegacyMinorTargetIsRejected,
        },
    };
    auto failures = std::size_t{0};
    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
