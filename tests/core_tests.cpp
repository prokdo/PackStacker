#include "core/filesystem.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/streams.hpp"
#include "testing/fixtures.hpp"

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <zip.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <zipconf.h>

namespace
{
    using PackStacker::ErrorCode;
    using PackStacker::ExportPack;
    using PackStacker::InspectPack;
    using PackStacker::Limits;
    using PackStacker::OutputFormat;
    using PackStacker::PackFormat;
    using PackStacker::PackType;
    using PackStacker::ResolvedFile;
    using PackStacker::ResolvedPack;
    using PackStacker::ResolveRequest;
    using PackStacker::ResolveStack;
    using PackStacker::Result;
    using Json = nlohmann::json;
    bool exists(std::string_view path)
    {
        return std::filesystem::exists(PackStacker::PathFromUtf8(path));
    }

    using Files = std::vector<std::pair<std::string, std::string>>;

    void require(bool condition, const std::string& message)
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

    template <typename T>
    void expectError(const Result<T>& result, ErrorCode code)
    {
        require(!result, "Expected an error");
        require(result.error().Code == code, "Unexpected error: " + result.error().Message);
    }

    using PackStacker::Testing::Read;
    using PackStacker::Testing::Write;

    std::string metadata(int32_t format = 4)
    {
        return std::string(
            Json{{"pack", {{"pack_format", format}, {"description", "Test pack"}}}}.dump());
    }

    std::string fixtureName(std::string path)
    {
        std::ranges::replace(path, '/', '_');
        return path;
    }

    void replaceBytes(std::string& bytes, std::string_view original, std::string_view replacement)
    {
        auto offset = std::size_t{0};
        while ((offset = bytes.find(original, offset)) != std::string::npos)
        {
            bytes.replace(offset, original.size(), replacement);
            offset += replacement.size();
        }
    }

    void makeZip(const std::string& path, const Files& entries, bool symbolicLink = false)
    {
        auto error = zip_error_t{};
        zip_error_init(&error);
#ifdef _WIN32
        const auto filename = PackStacker::PathFromUtf8(path).wstring();
        auto* source = zip_source_win32w_create(filename.c_str(), 0, ZIP_LENGTH_TO_END, &error);
#else
        const auto& filename = path;
        auto* source = zip_source_file_create(filename.c_str(), 0, ZIP_LENGTH_TO_END, &error);
#endif
        auto* opened = (source != nullptr)
                           ? zip_open_from_source(source, ZIP_CREATE | ZIP_TRUNCATE, &error)
                           : nullptr;
        if (opened == nullptr)
        {
            const auto message = std::string(zip_error_strerror(&error));
            if (source != nullptr)
            {
                zip_source_free(source);
            }
            zip_error_fini(&error);
            throw std::runtime_error(message);
        }
        zip_error_fini(&error);
        std::unique_ptr<zip_t, decltype(&zip_discard)> archive(opened, zip_discard);
        for (const auto& [name, bytes] : entries)
        {
            auto* content = zip_source_buffer(archive.get(), bytes.c_str(),
                                              static_cast<zip_uint64_t>(bytes.size()), 0);
            require(content != nullptr, "Cannot create ZIP fixture source");
            const auto encoded = name;
            const auto index =
                zip_file_add(archive.get(), encoded.c_str(), content, ZIP_FL_ENC_UTF_8);
            if (index < 0)
            {
                zip_source_free(content);
                throw std::runtime_error("Cannot add ZIP fixture entry");
            }
            require(zip_set_file_compression(archive.get(), static_cast<zip_uint64_t>(index),
                                             ZIP_CM_STORE, 0) == 0,
                    "Cannot configure ZIP fixture compression");
            if (symbolicLink && name != "pack.mcmeta")
            {
                require(zip_file_set_external_attributes(archive.get(),
                                                         static_cast<zip_uint64_t>(index), 0,
                                                         ZIP_OPSYS_UNIX, 0120777U << 16U) == 0,
                        "Cannot set ZIP fixture attributes");
            }
        }
        if (auto* closing = archive.release(); zip_close(closing) != 0)
        {
            archive.reset(closing);
            throw std::runtime_error("Cannot close ZIP fixture");
        }
    }

    struct Fixture
    {
        PackStacker::StagingDirectory Temporary;

        Fixture() : Temporary(PackStacker::Testing::TemporaryDirectory())
        {
        }

        std::string Pack(const std::string& name, const Files& files = {},
                         const std::string& meta = metadata()) const
        {
            const auto path =
                PackStacker::PathToUtf8(Temporary.Path() / PackStacker::PathFromUtf8(name));
            Write(path + "/pack.mcmeta", meta);
            for (const auto& [relative, bytes] : files)
            {
                Write((path + '/').append(relative), bytes);
            }
            return path;
        }

        std::string Zip(const std::string& name, Files files = {},
                        const std::string& meta = metadata()) const
        {
            files.emplace_back("pack.mcmeta", meta);
            const auto path =
                PackStacker::PathToUtf8(Temporary.Path() / PackStacker::PathFromUtf8(name));
            makeZip(path, files);
            return path;
        }
    };

    ResolvedPack resolve(const std::vector<std::string>& packs, PackType type = PackType::RESOURCE,
                         PackFormat target = {.Major = 4, .Minor = 0})
    {
        auto paths = std::vector<std::string>{};
        for (const auto& pack : packs)
        {
            paths.push_back(pack);
        }
        return take(ResolveStack({
            .PacksHighToLow = std::move(paths),
            .Type = type,
            .Target = target,
            .Limits = {},
            .Rules = {},
        }));
    }

    std::string content(const ResolvedPack& pack, const std::string& path)
    {
        const auto found = pack.Files.find(path);
        require(found != pack.Files.cend(), "Missing resolved file: " + path);
        return std::string(take(found->second.Read(std::uint64_t{1024} * 1024)));
    }

    const ResolvedFile& file(const ResolvedPack& pack, const std::string& path)
    {
        const auto found = pack.Files.find(path);
        require(found != pack.Files.cend(), "Missing resolved file: " + path);
        return found->second;
    }

    Json jsonContent(const ResolvedPack& pack, const std::string& path)
    {
        return Json::parse(content(pack, path));
    }

    constexpr auto LANGUAGE = std::string_view("assets/example/lang/en_us.json");
    constexpr auto TAG = std::string_view("data/example/tags/function/load.json");
    constexpr auto TEXTURE = std::string_view("assets/example/textures/block/test.png");

    class PartialOutput : public PackStacker::ByteWriter
    {
    public:
        std::string Bytes;
        std::int64_t FailAfter = -1;
        std::stop_source* Cancellation = nullptr;
        bool Writable = true;

        bool IsWritable() const override
        {
            return Writable;
        }

        std::expected<std::size_t, std::string> Write(std::span<const char> bytes) override
        {
            if (FailAfter >= 0 && std::cmp_greater_equal(Bytes.size(), FailAfter))
            {
                return std::unexpected("Injected write failure");
            }
            const auto count = std::min<std::size_t>(7, bytes.size());
            Bytes.append(bytes.data(), static_cast<std::size_t>(count));
            if (Cancellation != nullptr)
            {
                Cancellation->request_stop();
            }
            return count;
        }
    };

    void testPriorityAndOrderReversal()
    {
        const auto fixture = Fixture{};
        const auto low =
            fixture.Pack("low", {{std::string(TEXTURE), "low"}, {"LICENSE", "license"}});
        const auto high = fixture.Pack("high", {{std::string(TEXTURE), "high"}});
        require(content(resolve({high, low}), std::string(TEXTURE)) == "high", "Top pack must win");
        require(content(resolve({low, high}), std::string(TEXTURE)) == "low", "Order must matter");
        require(content(resolve({high, low}), "LICENSE") == "license", "Unique file lost");
    }

    void testLanguageKeysAndProvenance()
    {
        const auto fixture = Fixture{};
        const auto low =
            fixture.Pack("low", {{std::string(LANGUAGE), R"({"a":"old","b":"kept"})"}});
        const auto high =
            fixture.Pack("high", {{std::string(LANGUAGE), R"({"a":"new","c":"added"})"}});
        const auto merged = resolve({high, low});
        require(jsonContent(merged, std::string(LANGUAGE)) ==
                    Json{{"a", "new"}, {"b", "kept"}, {"c", "added"}},
                "Incorrect language merge");
        require(file(merged, std::string(LANGUAGE)).SourcePacks() ==
                    std::vector<std::string>{high, low},
                "Incorrect contributor order");
    }

    void testLanguageNamespacesRemainSeparate()
    {
        const auto fixture = Fixture{};
        const auto other = std::string("assets/other/lang/en_us.json");
        const auto pack = fixture.Pack(
            "pack", {{std::string(LANGUAGE), R"({"a":"one"})"}, {other, R"({"a":"two"})"}});
        const auto merged = resolve({pack});
        require(jsonContent(merged, std::string(LANGUAGE)).at("a") == "one", "Namespace collision");
        require(jsonContent(merged, other).at("a") == "two", "Namespace collision");
    }

    void testOpaqueJSONAndBinaryBytesAreUnchanged()
    {
        const auto fixture = Fixture{};
        const auto binary = std::string("\0\1\2\xff", 4);
        const auto model = std::string("assets/example/models/item/test.json");
        const auto bytes = std::string("{ \"parent\" : \"example:item/base\" }\n");
        const auto pack = fixture.Pack("pack", {{model, bytes}, {std::string(TEXTURE), binary}});
        const auto merged = resolve({pack});
        require(content(merged, model) == bytes, "Opaque JSON reformatted");
        require(content(merged, std::string(TEXTURE)) == binary, "Binary changed");
    }

    void testTagsAppendWithoutDestroyingEntryTypesOrDuplicates()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack(
            "low", {
                       {
                           std::string(TAG),
                           R"({"values":["example:a",{"id":"#example:group","required":false}]})",
                       },
                   });
        const auto high =
            fixture.Pack("high", {{std::string(TAG), R"({"values":["example:a","example:b"]})"}});
        const auto value = jsonContent(resolve({high, low}, PackType::DATA), std::string(TAG));
        require(value.at("replace") == false, "Unexpected replacement");
        require(value.at("values") == Json::array({
                                          "example:a",
                                          {{"id", "#example:group"}, {"required", false}},
                                          "example:a",
                                          "example:b",
                                      }),
                "Tag entry order/types changed");
    }

    void testTagReplacementRemainsEffectiveAgainstExternalLowerPacks()
    {
        const auto fixture = Fixture{};
        const auto low =
            fixture.Pack("low", {{std::string(TAG), R"({"values":["example:discarded"]})"}});
        const auto middle = fixture.Pack(
            "middle", {{std::string(TAG), R"({"replace":true,"values":["example:reset"]})"}});
        const auto high = fixture.Pack(
            "high", {{std::string(TAG), R"({"replace":false,"values":["example:top"]})"}});
        const auto value =
            jsonContent(resolve({high, middle, low}, PackType::DATA), std::string(TAG));
        require(value == Json{{"replace", true}, {"values", {"example:reset", "example:top"}}},
                "Replacement flag or values lost");
    }

    void testEmptyReplacingTag()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack("low", {{std::string(TAG), R"({"values":["example:a"]})"}});
        const auto high =
            fixture.Pack("high", {{std::string(TAG), R"({"replace":true,"values":[]})"}});
        const auto value = jsonContent(resolve({high, low}, PackType::DATA), std::string(TAG));
        require(value.at("replace") == true && value.at("values").empty(), "Empty reset failed");
    }

    void testNonVanillaTagFieldsAreRejected()
    {
        const auto fixture = Fixture{};
        const auto pack =
            fixture.Pack("pack", {{std::string(TAG), R"({"values":[],"remove":["example:a"]})"}});
        expectError(ResolveStack({
                        .PacksHighToLow = {pack},
                        .Type = PackType::DATA,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::UNSUPPORTED_FEATURE);
    }

    void testInvalidTagShapes()
    {
        const auto fixture = Fixture{};
        for (const auto& bytes : {
                 std::string("[]"),
                 std::string("{}"),
                 std::string(R"({"values":"not-array"})"),
                 std::string(R"({"replace":"true","values":[]})"),
                 std::string(R"({"values":[42]})"),
                 std::string(R"({"values":[{"id":"example:a","required":"false"}]})"),
             })
        {
            const auto pack = fixture.Pack("pack", {{std::string(TAG), bytes}});
            expectError(ResolveStack({
                            .PacksHighToLow = {pack},
                            .Type = PackType::DATA,
                            .Target = {.Major = 4, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        }),
                        ErrorCode::INVALID_RESOURCE);
        }
    }

    void testInvalidLanguageShape()
    {
        const auto fixture = Fixture{};
        for (const auto& bytes : {
                 std::string("[]"),
                 std::string(R"({"nested":{"x":"value"}})"),
                 std::string(R"({"number":4})"),
                 std::string("{"),
             })
        {
            const auto pack = fixture.Pack("pack", {{std::string(LANGUAGE), bytes}});
            expectError(ResolveStack({
                            .PacksHighToLow = {pack},
                            .Type = PackType::RESOURCE,
                            .Target = {.Major = 4, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        }),
                        ErrorCode::INVALID_RESOURCE);
        }
    }

    void testDuplicateJSONKeysAreRejected()
    {
        const auto fixture = Fixture{};
        const auto pack =
            fixture.Pack("pack", {{std::string(LANGUAGE), R"({"a":"one","a":"two"})"}});
        expectError(ResolveStack({
                        .PacksHighToLow = {pack},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INVALID_RESOURCE);
    }

    void testDirectoryAndZIPParityWithUnicodeHostPaths()
    {
        const auto fixture = Fixture{};
        const auto binary = std::string("\0\r\n\x1a\xff", 5) + std::string(140000, 'x') +
                            std::string("\x1a\0\r\n", 4);
        const auto files = Files{
            {std::string(LANGUAGE), R"({"a":"value"})"},
            {std::string(TEXTURE), "png"},
            {"NOTICE.txt", "notice"},
            {std::string("extra/текст.bin"), binary},
            {".hidden", binary},
        };
        const auto directoryPath = fixture.Pack(std::string("папка с пробелом 😀"), files);
        const auto archivePath = fixture.Zip(std::string("архив с пробелом 😀.zip"), files);
        const auto directory = resolve({directoryPath});
        const auto archive = resolve({archivePath});
        require(take(InspectPack(directoryPath)).Path == directoryPath,
                "Inspected UTF-8 pack path changed");
        require(directory.SourcePaths == std::vector<std::string>{directoryPath} &&
                    archive.SourcePaths == std::vector<std::string>{archivePath},
                "UTF-8 source paths changed");
        require(file(directory, std::string(TEXTURE)).SourcePacks() ==
                        std::vector<std::string>{directoryPath} &&
                    file(archive, std::string(TEXTURE)).SourcePacks() ==
                        std::vector<std::string>{archivePath},
                "UTF-8 file provenance changed");
        require(std::ranges::equal(directory.Files, archive.Files, {},
                                   &decltype(directory.Files)::value_type::first,
                                   &decltype(archive.Files)::value_type::first),
                "Directory/ZIP paths differ");
        require(content(directory, std::string("extra/текст.bin")) == binary,
                "Binary source data changed during streaming");
        for (const auto& entry : directory.Files)
        {
            const auto path = std::string(entry.first);
            require(content(directory, path) == content(archive, path),
                    "Directory/ZIP bytes differ");
        }
    }

    void testMixedZIPDirectoryStack()
    {
        const auto fixture = Fixture{};
        const auto low =
            fixture.Zip("low.zip", {{std::string(LANGUAGE), R"({"a":"low","b":"kept"})"}});
        const auto high = fixture.Pack("high", {{std::string(LANGUAGE), R"({"a":"high"})"}});
        require(jsonContent(resolve({high, low}), std::string(LANGUAGE)) ==
                    Json{{"a", "high"}, {"b", "kept"}},
                "Mixed source merge failed");
    }

    void testOverlaysReplaceWithinAPackBeforeLanguageMerging()
    {
        const auto fixture = Fixture{};
        auto meta = Json::parse(metadata());
        meta.at("pack").emplace("supported_formats", Json::array({4, 6}));
        meta.emplace("overlays", Json{
                                     {
                                         "entries",
                                         Json::array({
                                             {{"directory", "first"}, {"formats", {4, 6}}},
                                             {{"directory", "second"}, {"formats", 5}},
                                         }),
                                     },
                                 });
        const auto pack =
            fixture.Pack("pack",
                         {
                             {std::string(LANGUAGE), R"({"base":"discarded"})"},
                             {"first/" + std::string(LANGUAGE), R"({"first":"value"})"},
                             {"second/" + std::string(LANGUAGE), R"({"second":"value"})"},
                             {"second/pack.png", "ignored"},
                         },
                         std::string(meta.dump()));
        require(jsonContent(resolve({pack}, PackType::RESOURCE, {.Major = 4, .Minor = 0}),
                            std::string(LANGUAGE)) == Json{{"first", "value"}},
                "Overlay incorrectly deep-merged with base");
        require(jsonContent(resolve({pack}, PackType::RESOURCE, {.Major = 5, .Minor = 0}),
                            std::string(LANGUAGE)) == Json{{"second", "value"}},
                "Overlay order incorrect");
        require(!resolve({pack}, PackType::RESOURCE, {.Major = 5, .Minor = 0})
                     .Files.contains("second/pack.png"),
                "Inactive overlay contents leaked");
    }

    void testModernFormatMajorMinorBoundaries()
    {
        const auto fixture = Fixture{};
        const auto meta = std::string(
            R"({"pack":{"description":"test","min_format":[88,1],"max_format":[88,3]}})");
        const auto pack = fixture.Pack("pack", {}, meta);
        const auto info = take(InspectPack(pack));
        require(info.Formats.Contains({.Major = 88, .Minor = 1}) &&
                    info.Formats.Contains({.Major = 88, .Minor = 3}),
                "Inclusive boundary failed");
        expectError(ResolveStack({
                        .PacksHighToLow = {pack},
                        .Type = PackType::DATA,
                        .Target = {.Major = 88, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INCOMPATIBLE_FORMAT);
        resolve({pack}, PackType::DATA, {.Major = 88, .Minor = 2});
        expectError(ResolveStack({
                        .PacksHighToLow = {pack},
                        .Type = PackType::DATA,
                        .Target = {.Major = 88, .Minor = 4},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INCOMPATIBLE_FORMAT);
    }

    void testModernIntegerMaximumIncludesAllMinorVersions()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack(
            "pack", {}, R"({"pack":{"description":"test","min_format":88,"max_format":88}})");
        require(take(InspectPack(pack)).Formats.Contains({.Major = 88, .Minor = 999}),
                "Integer maximum mishandled");
    }

    void testLegacyRangeObject()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack(
            "pack", {},
            R"({"pack":{"description":{"text":"test"},"pack_format":4,"supported_formats":{"min_inclusive":4,"max_inclusive":6}}})");
        resolve({pack}, PackType::RESOURCE, {.Major = 6, .Minor = 0});
    }

    void testMalformedFormatMetadata()
    {
        const auto fixture = Fixture{};
        for (
            const auto& meta : {
                std::string(R"({})"),
                std::string(R"({"pack":{}})"),
                std::string(R"({"pack":{"pack_format":4}})"),
                std::string(R"({"pack":{"description":"test","min_format":[],"max_format":4}})"),
                std::string(
                    R"({"pack":{"description":"test","min_format":[4,0,1],"max_format":4}})"),
                std::string(
                    R"({"pack":{"description":"test","pack_format":4,"supported_formats":{"min_inclusive":4}}})"),
                std::string(
                    R"({"pack":{"description":"test","pack_format":4},"language":{"en_us":{"name":"English"}}})"),
                std::string(R"({"pack":{"description":"test","pack_format":4},"filter":{}})"),
                std::string(
                    R"({"pack":{"description":"test","pack_format":4},"overlays":{"entries":[{}]}})"),
                std::string(R"({"pack":{"description":"test","pack_format":256.5}})"),
                std::string(R"({"pack":{"description":"test","pack_format":-1}})"),
                std::string(
                    R"({"pack":{"description":"test","pack_format":18446744073709551615}})"),
                std::string(R"({"pack":{"description":"test","min_format":88}})"),
                std::string(
                    R"({"pack":{"description":"test","pack_format":4,"supported_formats":[5,6]}})"),
                std::string(R"({"pack":{"description":"test","min_format":6,"max_format":4}})"),
                std::string(R"({"pack":{"description":"test","pack_format":4,"pack_format":5}})"),
            })
        {
            expectError(InspectPack((fixture.Pack("pack", {}, meta))), ErrorCode::INVALID_METADATA);
        }
    }

    void testMetadataDiagnosticsPreserveSectionValidationOrder()
    {
        const auto fixture = Fixture{};
        const auto cases = std::vector<std::tuple<const char*, ErrorCode, const char*>>{
            {
                R"({"language":[],"filter":false,"overlays":false})",
                ErrorCode::INVALID_METADATA,
                "Invalid language metadata object",
            },
            {
                R"({"language":{"en_us":{"name":"English","region":"US","unknown":true}},"filter":false})",
                ErrorCode::UNSUPPORTED_FEATURE,
                "Unknown language definition field",
            },
            {
                R"({"filter":{},"overlays":false})",
                ErrorCode::INVALID_METADATA,
                "Invalid filter object",
            },
            {
                R"({"filter":{"block":[],"unknown":true},"overlays":false})",
                ErrorCode::UNSUPPORTED_FEATURE,
                "Unknown filter field",
            },
            {
                R"({"overlays":{"entries":[{"directory":"assets","formats":false}]}})",
                ErrorCode::INVALID_METADATA,
                "Invalid overlay directory",
            },
            {
                R"({"overlays":{"entries":[{"directory":"overlay","formats":false}]}})",
                ErrorCode::INVALID_METADATA,
                "Invalid overlay format range",
            },
            {
                R"({"unknown":true,"language":false})",
                ErrorCode::UNSUPPORTED_FEATURE,
                "Unsupported metadata section",
            },
        };
        for (const auto& [sections, code, message] : cases)
        {
            auto meta = Json::parse(metadata());
            meta.update(Json::parse(sections));
            const auto path = fixture.Pack("pack", {}, std::string(meta.dump()));
            const auto result = InspectPack(path);
            expectError(result, code);
            require(result.error().Message == message, "Metadata diagnostic changed");
            require(result.error().Pack == path && result.error().Path == "pack.mcmeta",
                    "Metadata diagnostic lost its source");
        }
    }

    void testMetadataRootIsMandatory()
    {
        const auto fixture = Fixture{};
        const auto path = PackStacker::PathToUtf8(fixture.Temporary.Path() /
                                                  PackStacker::PathFromUtf8("nested.zip"));
        makeZip(path, {{"wrapper/pack.mcmeta", metadata()}});
        expectError(InspectPack(path), ErrorCode::INVALID_METADATA);
    }

    void testReplacedResourceDoesNotInheritLowerMetadata()
    {
        const auto fixture = Fixture{};
        const auto low =
            fixture.Pack("low", {
                                    {std::string(TEXTURE), "low"},
                                    {std::string(TEXTURE) + ".mcmeta", R"({"animation":{}})"},
                                });
        const auto high = fixture.Pack("high", {{std::string(TEXTURE), "high"}});
        require(!resolve({high, low}).Files.contains((std::string(TEXTURE) + ".mcmeta")),
                "Stale metadata survived");
    }

    void testUpperMetadataOnlyOverrideSurvives()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack(
            "low", {{std::string(TEXTURE), "low"}, {std::string(TEXTURE) + ".mcmeta", "old"}});
        const auto high = fixture.Pack("high", {{std::string(TEXTURE) + ".mcmeta", "new"}});
        const auto merged = resolve({high, low});
        require(content(merged, std::string(TEXTURE)) == "low",
                "Metadata override changed texture");
        require(content(merged, std::string(TEXTURE) + ".mcmeta") == "new",
                "Metadata override lost");
    }

    void testSamePackMetadataAndExternalMetadataOnlyFile()
    {
        const auto fixture = Fixture{};
        const auto path = std::string(TEXTURE) + ".mcmeta";
        const auto withTexture =
            fixture.Pack("full", {{std::string(TEXTURE), "png"}, {path, "metadata"}});
        require(content(resolve({withTexture}), path) == "metadata", "Same-pack metadata lost");
        const auto metadataOnly = fixture.Pack("metadata-only", {{path, "external"}});
        require(content(resolve({metadataOnly}), path) == "external",
                "External metadata override lost");
    }

    void testUnsupportedMetadataIsExplicit()
    {
        const auto fixture = Fixture{};
        for (const auto& section : {"features", "custom"})
        {
            auto meta = Json::parse(metadata());
            meta.emplace(section, Json::object());
            const auto pack = fixture.Pack("pack", {}, std::string(meta.dump()));
            expectError(ResolveStack({
                            .PacksHighToLow = {pack},
                            .Type = PackType::RESOURCE,
                            .Target = {.Major = 4, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        }),
                        ErrorCode::UNSUPPORTED_FEATURE);
        }
    }

    void testFontProvidersPreservePriorityAndInternalOrder()
    {
        const auto fixture = Fixture{};
        const auto path = std::string("assets/example/font/default.json");
        const auto lowProviders = Json(Json::array({
            {{"type", "space"}, {"advances", {{"A", 1}}}},
            {{"type", "reference"}, {"id", "example:base"}},
        }));
        const auto highProviders = Json(Json::array({
            {{"type", "space"}, {"advances", {{"A", 2}}}, {"filter", {{"uniform", true}}}},
            {{"type", "space"}, {"advances", {{"A", 3}}}},
        }));
        const auto low = fixture.Pack(
            "low", {{path, std::string(Json{{"providers", lowProviders}}.dump())}}, metadata(34));
        const auto high = fixture.Zip(
            "high.zip", {{path, std::string(Json{{"providers", highProviders}}.dump())}},
            metadata(34));
        auto expected = highProviders;
        for (const auto& provider : lowProviders)
        {
            expected.push_back(provider);
        }
        const auto merged = resolve({high, low}, PackType::RESOURCE, {.Major = 34, .Minor = 0});
        require(jsonContent(merged, path).at("providers") == expected,
                "Font provider priority/order changed");
        const auto reversed = resolve({low, high}, PackType::RESOURCE, {.Major = 34, .Minor = 0});
        require(jsonContent(reversed, path).at("providers").at(0) == lowProviders.at(0),
                "Font order reversal ignored");
    }

    void testAtlasSourcesAppendInExecutionOrderWithoutDeduplication()
    {
        const auto fixture = Fixture{};
        const auto path = std::string("assets/minecraft/atlases/blocks.json");
        const auto repeated =
            Json{{"type", "minecraft:single"}, {"resource", "example:block/test"}};
        const auto filter =
            Json{{"type", "minecraft:filter"}, {"namespace", "example"}, {"path", ".*"}};
        const auto low =
            fixture.Pack("low",
                         {
                             {
                                 path,
                                 std::string(Json{{"sources", Json::array({repeated})}}.dump()),
                             },
                         },
                         metadata(12));
        const auto high = fixture.Pack(
            "high",
            {
                {
                    path,
                    std::string(
                        Json{{"sources", Json::array({filter, repeated, repeated})}}.dump()),
                },
            },
            metadata(12));
        const auto merged = resolve({high, low}, PackType::RESOURCE, {.Major = 12, .Minor = 0});
        require(jsonContent(merged, path).at("sources") ==
                    Json::array({repeated, filter, repeated, repeated}),
                "Atlas operations reordered or deduplicated");
    }

    void testFontOverlayReplacesTheBaseProviderList()
    {
        const auto fixture = Fixture{};
        const auto path = std::string("assets/example/font/default.json");
        auto meta = Json::parse(metadata(34));
        meta.emplace("overlays",
                     Json{{"entries", Json::array({{{"directory", "overlay"}, {"formats", 34}}})}});
        const auto pack = fixture.Pack(
            "pack",
            {
                {path, R"({"providers":[{"type":"reference","id":"example:discarded"}]})"},
                {
                    "overlay/" + path,
                    R"({"providers":[{"type":"reference","id":"example:chosen"}]})",
                },
            },
            std::string(meta.dump()));
        require(jsonContent(resolve({pack}, PackType::RESOURCE, {.Major = 34, .Minor = 0}), path)
                        .at("providers")
                        .size() == 1,
                "Overlay provider list merged with base");
    }

    void testListResourceValidationAndUnknownRootFields()
    {
        const auto fixture = Fixture{};
        for (const auto& kind : {"font", "atlases"})
        {
            const auto path = std::string(std::string("assets/example/") + kind + "/test.json");
            const auto* const key = std::string(kind) == "font" ? "providers" : "sources";
            for (const auto& value : {
                     Json::array(),
                     Json::object(),
                     Json{{key, 1}},
                     Json{{key, Json::array({1})}},
                     Json{{key, Json::array({Json::object()})}},
                 })
            {
                const auto pack = fixture.Pack("pack", {{path, std::string(value.dump())}});
                expectError(ResolveStack({
                                .PacksHighToLow = {pack},
                                .Type = PackType::RESOURCE,
                                .Target = {.Major = 4, .Minor = 0},
                                .Limits = {},
                                .Rules = {},
                            }),
                            ErrorCode::INVALID_RESOURCE);
            }
            const auto pack = fixture.Pack(
                "pack", {
                            {
                                path,
                                std::string(Json{{key, Json::array()}, {"custom", true}}.dump()),
                            },
                        });
            expectError(ResolveStack({
                            .PacksHighToLow = {pack},
                            .Type = PackType::RESOURCE,
                            .Target = {.Major = 4, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        }),
                        ErrorCode::UNSUPPORTED_FEATURE);
        }
    }

    void testStreamingCopyHandlesPartialWritesForSourceAndGeneratedContent()
    {
        const auto fixture = Fixture{};
        const auto payload = std::string(200000, 'x');
        const auto directory = fixture.Pack(
            "pack", {{std::string(TEXTURE), payload}, {std::string(LANGUAGE), R"({"a":"value"})"}});
        const auto zip = fixture.Zip("pack.zip", {{std::string(TEXTURE), payload}});
        for (const auto& source : {directory, zip})
        {
            const auto merged = resolve({source});
            auto output = PartialOutput{};
            const auto copied = file(merged, std::string(TEXTURE)).CopyTo(output, 200000);
            require(copied.has_value(), "Streaming copy failed");
            require(output.Bytes == payload, "Partial writes lost bytes");
        }
        const auto merged = resolve({directory});
        auto output = PartialOutput{};
        require(file(merged, std::string(LANGUAGE)).CopyTo(output, 1024).has_value(),
                "Generated copy failed");
        require(output.Bytes == content(merged, std::string(LANGUAGE)), "Generated bytes changed");
    }

    void testStreamingFailuresAndDeterministicCancellation()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack("pack", {{std::string(TEXTURE), std::string(200000, 'x')}});
        const auto merged = resolve({pack});
        auto failing = PartialOutput{};
        failing.FailAfter = 14;
        expectError(file(merged, std::string(TEXTURE)).CopyTo(failing, 200000), ErrorCode::IO);
        auto stop = std::stop_source{};
        auto cancelled = PartialOutput{};
        cancelled.Cancellation = &stop;
        expectError(file(merged, std::string(TEXTURE)).CopyTo(cancelled, 200000, stop.get_token()),
                    ErrorCode::CANCELLED);
        require(cancelled.Bytes.size() < 200000, "Cancellation ignored until end");
        auto closed = PartialOutput{};
        closed.Writable = false;
        expectError(file(merged, std::string(TEXTURE)).CopyTo(closed, 200000), ErrorCode::IO);
        auto limited = PackStacker::MemoryWriter{};
        expectError(file(merged, std::string(TEXTURE)).CopyTo(limited, 10),
                    ErrorCode::LIMIT_EXCEEDED);
        require(limited.Bytes().empty(), "Oversized copy wrote bytes");
    }

    void testUnsupportedSpecialResourcesAreNotCopiedSilently()
    {
        const auto fixture = Fixture{};
        for (const auto& path : {"assets/example/lang/en_us.lang"})
        {
            const auto pack = fixture.Pack(fixtureName(path), {{path, "{}"}});
            expectError(ResolveStack({
                            .PacksHighToLow = {pack},
                            .Type = PackType::RESOURCE,
                            .Target = {.Major = 4, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        }),
                        ErrorCode::UNSUPPORTED_FEATURE);
        }
    }

    void testUnsafeZIPNames()
    {
        const auto fixture = Fixture{};
        auto index = std::size_t{0};
        for (const auto& name : {
                 "../escape",
                 "/absolute",
                 "C:/absolute",
                 "a\\b",
                 "a//b",
                 "a/./b",
                 "a/../b",
                 "a./b",
                 "NUL.txt",
                 "a/COM1.bin",
             })
        {
            const auto path = PackStacker::PathToUtf8(
                fixture.Temporary.Path() /
                PackStacker::PathFromUtf8(std::to_string(index++) + ".zip"));
            makeZip(path, {{name, "unsafe"}, {"pack.mcmeta", metadata()}});
            expectError(InspectPack(path), ErrorCode::INVALID_PATH);
        }
    }

    void testDuplicateZIPEntries()
    {
        const auto fixture = Fixture{};
        const auto path =
            fixture.Zip("duplicate.zip", {{"first.txt", "one"}, {"other.txt", "two"}});
        auto bytes = Read(path);
        const auto first = bytes.find("other.txt");
        const auto second = bytes.find("other.txt", first + 1);
        require(first != std::string::npos && second != std::string::npos &&
                    bytes.find("other.txt", second + 1) == std::string::npos,
                "Unexpected ZIP fixture layout");
        replaceBytes(bytes, "other.txt", "first.txt");
        Write(path, bytes);
        const auto inspected = InspectPack(path);
        require(!inspected, "Duplicate ZIP entry accepted");
        require(inspected.error().Code == ErrorCode::INVALID_ARCHIVE ||
                    inspected.error().Code == ErrorCode::INVALID_PATH,
                "Duplicate ZIP entry returned unrelated error");
    }

    void testZIPSymlinks()
    {
        const auto fixture = Fixture{};
        const auto path = PackStacker::PathToUtf8(fixture.Temporary.Path() /
                                                  PackStacker::PathFromUtf8("symlink.zip"));
        makeZip(path, {{"link", "../../outside"}, {"pack.mcmeta", metadata()}}, true);
        expectError(InspectPack(path), ErrorCode::INVALID_ARCHIVE);
    }

    void testZIPCaseCollisionsAndFileDirectoryCollisions()
    {
        const auto fixture = Fixture{};
        const auto casePath = fixture.Zip("case.zip", {{"Readme", "one"}, {"README", "two"}});
        expectError(InspectPack(casePath), ErrorCode::INVALID_PATH);
        const auto normalizedPath =
            fixture.Zip("normalized.zip", {
                                              {std::string("caf\xc3\xa9"), "one"},
                                              {std::string("cafe\xcc\x81"), "two"},
                                          });
        expectError(InspectPack(normalizedPath), ErrorCode::INVALID_PATH);
        const auto foldedPath = fixture.Zip("folded.zip", {{"Straße", "one"}, {"STRASSE", "two"}});
        expectError(InspectPack(foldedPath), ErrorCode::INVALID_PATH);
        const auto treePath = fixture.Zip("tree.zip", {{"file", "one"}, {"file/child", "two"}});
        expectError(InspectPack(treePath), ErrorCode::INVALID_PATH);
    }

    void testCrossPackOutputPathCollisions()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack("low", {{"Readme", "one"}});
        const auto high = fixture.Pack("high", {{"README", "two"}});
        expectError(ResolveStack({
                        .PacksHighToLow = {high, low},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INVALID_PATH);
        const auto composed = fixture.Pack("composed", {{std::string("caf\xc3\xa9"), "one"}});
        const auto decomposed = fixture.Pack("decomposed", {{std::string("cafe\xcc\x81"), "two"}});
        expectError(ResolveStack({
                        .PacksHighToLow = {composed, decomposed},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INVALID_PATH);
        const auto parent = fixture.Pack("parent", {{"docs", "file"}});
        const auto child = fixture.Pack("child", {{"docs/license", "file"}});
        expectError(ResolveStack({
                        .PacksHighToLow = {parent, child},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INVALID_PATH);
    }

    void testCorruptArchive()
    {
        const auto fixture = Fixture{};
        const auto path = PackStacker::PathToUtf8(fixture.Temporary.Path() /
                                                  PackStacker::PathFromUtf8("broken.zip"));
        Write(path, "not a ZIP");
        expectError(InspectPack(path), ErrorCode::INVALID_ARCHIVE);
    }

    void testZIPCRCFailureOnLazyRead()
    {
        const auto fixture = Fixture{};
        const auto path = fixture.Zip("crc.zip", {{std::string(TEXTURE), "payloadCRC"}});
        auto bytes = Read(path);
        require(bytes.contains("payloadCRC"), "Fixture is unexpectedly compressed");
        replaceBytes(bytes, "payloadCRC", "payloadBAD");
        Write(path, bytes);
        const auto merged = resolve({path});
        require(!file(merged, std::string(TEXTURE)).Read(1024),
                "Corrupt entry passed integrity check");
        const auto output = PackStacker::PathToUtf8(
            fixture.Temporary.Path() / PackStacker::PathFromUtf8("corrupt-output.zip"));
        require(!ExportPack(merged,
                            {
                                .Path = output,
                                .Format = OutputFormat::ZIP,
                                .Description = "test",
                                .Limits = {},
                            }),
                "Corrupt input was exported successfully");
        require(!exists(output), "Corrupt input published an output");
    }

    void testLimitsApplyToDirectoryAndZIP()
    {
        const auto fixture = Fixture{};
        const auto directory = fixture.Pack("pack", {{std::string(TEXTURE), "payload"}});
        const auto archive = fixture.Zip("pack.zip", {{std::string(TEXTURE), "payload"}});
        auto limits = Limits{};
        limits.MaxFileBytes = 2;
        expectError(InspectPack(directory, limits), ErrorCode::LIMIT_EXCEEDED);
        expectError(InspectPack(archive, limits), ErrorCode::LIMIT_EXCEEDED);
        limits = {};
        limits.MaxEntries = 1;
        expectError(InspectPack(directory, limits), ErrorCode::LIMIT_EXCEEDED);
        expectError(InspectPack(archive, limits), ErrorCode::LIMIT_EXCEEDED);
        limits = {};
        limits.MaxTotalBytes = 1;
        expectError(InspectPack(directory, limits), ErrorCode::LIMIT_EXCEEDED);
    }

    void testStackWideLimits()
    {
        const auto fixture = Fixture{};
        const auto low = fixture.Pack("low");
        const auto high = fixture.Pack("high");
        auto request = ResolveRequest{
            .PacksHighToLow = {high, low},
            .Type = PackType::RESOURCE,
            .Target = {.Major = 4, .Minor = 0},
            .Limits = {},
            .Rules = {},
        };
        request.Limits.MaxPacks = 1;
        expectError(ResolveStack(request), ErrorCode::LIMIT_EXCEEDED);
        request.Limits = {};
        request.Limits.MaxEntries = 1;
        expectError(ResolveStack(request), ErrorCode::LIMIT_EXCEEDED);
        request.Limits = {};
        request.Limits.MaxTotalBytes = static_cast<std::uint64_t>(metadata().size());
        expectError(ResolveStack(request), ErrorCode::LIMIT_EXCEEDED);
    }

    void testJSONSizeAndDepthLimits()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack("pack");
        auto limits = Limits{};
        limits.MaxJsonBytes = 2;
        expectError(InspectPack(pack, limits), ErrorCode::LIMIT_EXCEEDED);
        limits = {};
        limits.MaxJsonDepth = 2;
        const auto nested = fixture.Pack(
            "nested", {}, R"({"pack":{"pack_format":4,"description":{"text":"test"}}})");
        expectError(InspectPack(nested, limits), ErrorCode::LIMIT_EXCEEDED);
    }

    void testMergedJSONSizeLimit()
    {
        const auto fixture = Fixture{};
        const auto value = std::string(80, 'x');
        const auto low =
            fixture.Pack("low", {{std::string(LANGUAGE), std::string(Json{{"a", value}}.dump())}});
        const auto high =
            fixture.Pack("high", {{std::string(LANGUAGE), std::string(Json{{"b", value}}.dump())}});
        auto request = ResolveRequest{
            .PacksHighToLow = {high, low},
            .Type = PackType::RESOURCE,
            .Target = {.Major = 4, .Minor = 0},
            .Limits = {},
            .Rules = {},
        };
        request.Limits.MaxJsonBytes = 120;
        expectError(ResolveStack(request), ErrorCode::LIMIT_EXCEEDED);
    }

    void testLazyReadsEnforceCallerLimitAndCancellation()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack("pack", {
                                                   {std::string(TEXTURE), "payload"},
                                                   {std::string(LANGUAGE), R"({"a":"value"})"},
                                               });
        const auto merged = resolve({pack});
        expectError(file(merged, std::string(TEXTURE)).Read(1), ErrorCode::LIMIT_EXCEEDED);
        expectError(file(merged, std::string(LANGUAGE)).Read(1), ErrorCode::LIMIT_EXCEEDED);
        const auto cancellation = std::stop_source{};
        cancellation.request_stop();
        expectError(file(merged, std::string(TEXTURE)).Read(1024, cancellation.get_token()),
                    ErrorCode::CANCELLED);
        expectError(file(merged, std::string(LANGUAGE)).Read(1024, cancellation.get_token()),
                    ErrorCode::CANCELLED);
    }

    void testSourceChangesAreDetectedBeforeLazyReads()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack("pack", {{std::string(TEXTURE), "old"}});
        const auto merged = resolve({pack});
        Write(pack + '/' + std::string(TEXTURE), "changed size");
        expectError(file(merged, std::string(TEXTURE)).Read(1024), ErrorCode::SOURCE_CHANGED);

        const auto sameSize = fixture.Pack("same-size", {{std::string(TEXTURE), "old"}});
        const auto plan = resolve({sameSize});
        const auto physical = PackStacker::PathFromUtf8((sameSize + '/' + std::string(TEXTURE)));
        const auto modified = std::filesystem::last_write_time(physical);
        Write(sameSize + '/' + std::string(TEXTURE), "new");
        std::filesystem::last_write_time(physical, modified + std::chrono::seconds{2});
        expectError(file(plan, std::string(TEXTURE)).Read(1024), ErrorCode::SOURCE_CHANGED);

        const auto replaced = fixture.Pack("replaced", {{std::string(TEXTURE), "old"}});
        const auto replacedPlan = resolve({replaced});
        const auto replacedFile =
            PackStacker::PathFromUtf8((replaced + '/' + std::string(TEXTURE)));
        require(std::filesystem::remove(replacedFile), "Cannot remove indexed source file");
        require(std::filesystem::create_directory(replacedFile),
                "Cannot replace source file with a directory");
        expectError(file(replacedPlan, std::string(TEXTURE)).Read(1024), ErrorCode::SOURCE_CHANGED);
    }

#ifndef _WIN32
    void testPhysicalLinksAreRejected()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack("pack", {{std::string(TEXTURE), "payload"}});
        const auto link =
            PackStacker::PathToUtf8(fixture.Temporary.Path() / PackStacker::PathFromUtf8("link"));
        std::filesystem::create_directory_symlink(PackStacker::PathFromUtf8(pack),
                                                  PackStacker::PathFromUtf8(link));
        expectError(InspectPack(link), ErrorCode::INVALID_PATH);
        expectError(InspectPack((link + "/../pack")), ErrorCode::INVALID_PATH);

        const auto plan = resolve({pack});
        const auto assets = PackStacker::PathFromUtf8((pack + "/assets"));
        const auto moved = PackStacker::PathFromUtf8((pack + "/original-assets"));
        std::filesystem::rename(assets, moved);
        std::filesystem::create_directory_symlink(moved, assets);
        expectError(file(plan, std::string(TEXTURE)).Read(1024), ErrorCode::INVALID_PATH);
        expectError(InspectPack(pack), ErrorCode::INVALID_PATH);
    }
#endif

    void testChangedZIPSourceIsDetected()
    {
        const auto fixture = Fixture{};
        const auto path = fixture.Zip("pack.zip", {{std::string(TEXTURE), "before"}});
        const auto merged = resolve({path});
        makeZip(path, {{"pack.mcmeta", metadata()}, {std::string(TEXTURE), "different-size"}});
        expectError(file(merged, std::string(TEXTURE)).Read(1024), ErrorCode::SOURCE_CHANGED);
    }

    void testInvalidUTF8JSON()
    {
        const auto fixture = Fixture{};
        auto bytes = std::string(R"({"key":")");
        bytes.push_back(static_cast<char>(0xff));
        bytes.append("\"}");
        const auto pack = fixture.Pack("pack", {{std::string(LANGUAGE), bytes}});
        expectError(ResolveStack({
                        .PacksHighToLow = {pack},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INVALID_RESOURCE);
    }

    void testModernOverlayMinorBoundaries()
    {
        const auto fixture = Fixture{};
        const auto meta = std::string(
            R"({"pack":{"description":"test","min_format":88,"max_format":88},"overlays":{"entries":[{"directory":"modern","min_format":[88,1],"max_format":[88,2]}]}})");
        const auto pack = fixture.Pack(
            "pack", {{std::string(TEXTURE), "base"}, {"modern/" + std::string(TEXTURE), "overlay"}},
            meta);
        require(content(resolve({pack}, PackType::RESOURCE, {.Major = 88, .Minor = 0}),
                        std::string(TEXTURE)) == "base",
                "Overlay started too early");
        require(content(resolve({pack}, PackType::RESOURCE, {.Major = 88, .Minor = 1}),
                        std::string(TEXTURE)) == "overlay",
                "Overlay minimum not inclusive");
        require(content(resolve({pack}, PackType::RESOURCE, {.Major = 88, .Minor = 2}),
                        std::string(TEXTURE)) == "overlay",
                "Overlay maximum not inclusive");
        require(content(resolve({pack}, PackType::RESOURCE, {.Major = 88, .Minor = 3}),
                        std::string(TEXTURE)) == "base",
                "Overlay ended too late");
    }

    void testCancelledOperations()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack("pack");
        const auto cancellation = std::stop_source{};
        cancellation.request_stop();
        expectError(InspectPack(pack, {}, cancellation.get_token()), ErrorCode::CANCELLED);
        expectError(ResolveStack(
                        {
                            .PacksHighToLow = {pack},
                            .Type = PackType::RESOURCE,
                            .Target = {.Major = 4, .Minor = 0},
                            .Limits = {},
                            .Rules = {},
                        },
                        cancellation.get_token()),
                    ErrorCode::CANCELLED);
    }

    void testInvalidRequestsAndUnsupportedMixedPacks()
    {
        const auto fixture = Fixture{};
        const auto pack = fixture.Pack("pack");
        expectError(ResolveStack({}), ErrorCode::INVALID_RESOURCE);
        expectError(ResolveStack({
                        .PacksHighToLow = {""},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INVALID_PATH);
        expectError(ResolveStack({
                        .PacksHighToLow = {pack, pack},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::INVALID_PATH);
        expectError(ResolveStack({
                        .PacksHighToLow = {pack},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 3, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::UNSUPPORTED_FEATURE);
        const auto data = fixture.Pack("data", {{std::string(TAG), R"({"values":[]})"}});
        expectError(ResolveStack({
                        .PacksHighToLow = {data},
                        .Type = PackType::RESOURCE,
                        .Target = {.Major = 4, .Minor = 0},
                        .Limits = {},
                        .Rules = {},
                    }),
                    ErrorCode::UNSUPPORTED_FEATURE);
    }

    void testInvalidUtf8InputPaths()
    {
        const auto invalidPaths = std::vector<std::string>{
            std::string("\xed\xa0\x80", 3),
            std::string("\xc0\xaf", 2),
            std::string("\xf4\x90\x80\x80", 4),
            std::string("pack\0suffix", 11),
        };
        for (const auto& path : invalidPaths)
        {
            const auto inspected = InspectPack(path);
            expectError(inspected, ErrorCode::INVALID_PATH);
            require(inspected.error().Pack == path, "Invalid input path context changed");
            const auto resolved = ResolveStack({
                .PacksHighToLow = {path},
                .Type = PackType::RESOURCE,
                .Target = {.Major = 4, .Minor = 0},
                .Limits = {},
                .Rules = {},
            });
            expectError(resolved, ErrorCode::INVALID_PATH);
            require(resolved.error().Pack == path, "Invalid stack path context changed");
        }
    }

}

int32_t main()
{
    const auto tests = std::vector<std::pair<std::string, std::function<void()>>>{
        {
            "invalid UTF-8 input paths",
            testInvalidUtf8InputPaths,
        },
        {
            "priority and order reversal",
            testPriorityAndOrderReversal,
        },
        {
            "language keys and provenance",
            testLanguageKeysAndProvenance,
        },
        {
            "language namespaces remain separate",
            testLanguageNamespacesRemainSeparate,
        },
        {
            "opaque JSON and binary bytes are unchanged",
            testOpaqueJSONAndBinaryBytesAreUnchanged,
        },
        {
            "tags append without destroying entry types or duplicates",
            testTagsAppendWithoutDestroyingEntryTypesOrDuplicates,
        },
        {
            "tag replacement remains effective against external lower packs",
            testTagReplacementRemainsEffectiveAgainstExternalLowerPacks,
        },
        {
            "empty replacing tag",
            testEmptyReplacingTag,
        },
        {
            "non-vanilla tag fields are rejected",
            testNonVanillaTagFieldsAreRejected,
        },
        {
            "invalid tag shapes",
            testInvalidTagShapes,
        },
        {
            "invalid language shape",
            testInvalidLanguageShape,
        },
        {
            "duplicate JSON keys are rejected",
            testDuplicateJSONKeysAreRejected,
        },
        {
            "directory and ZIP parity with Unicode host paths",
            testDirectoryAndZIPParityWithUnicodeHostPaths,
        },
        {
            "mixed ZIP/directory stack",
            testMixedZIPDirectoryStack,
        },
        {
            "overlays replace within a pack before language merging",
            testOverlaysReplaceWithinAPackBeforeLanguageMerging,
        },
        {
            "modern format major/minor boundaries",
            testModernFormatMajorMinorBoundaries,
        },
        {
            "modern integer maximum includes all minor versions",
            testModernIntegerMaximumIncludesAllMinorVersions,
        },
        {
            "legacy range object",
            testLegacyRangeObject,
        },
        {
            "malformed format metadata",
            testMalformedFormatMetadata,
        },
        {
            "metadata diagnostics preserve section validation order",
            testMetadataDiagnosticsPreserveSectionValidationOrder,
        },
        {
            "metadata root is mandatory",
            testMetadataRootIsMandatory,
        },
        {
            "replaced resource does not inherit lower metadata",
            testReplacedResourceDoesNotInheritLowerMetadata,
        },
        {
            "upper metadata-only override survives",
            testUpperMetadataOnlyOverrideSurvives,
        },
        {
            "same-pack metadata and external metadata-only file",
            testSamePackMetadataAndExternalMetadataOnlyFile,
        },
        {
            "unsupported metadata is explicit",
            testUnsupportedMetadataIsExplicit,
        },
        {
            "font providers preserve priority and internal order",
            testFontProvidersPreservePriorityAndInternalOrder,
        },
        {
            "atlas sources append in execution order without deduplication",
            testAtlasSourcesAppendInExecutionOrderWithoutDeduplication,
        },
        {
            "font overlay replaces the base provider list",
            testFontOverlayReplacesTheBaseProviderList,
        },
        {
            "list resource validation and unknown root fields",
            testListResourceValidationAndUnknownRootFields,
        },
        {
            "streaming copy handles partial writes for source and generated content",
            testStreamingCopyHandlesPartialWritesForSourceAndGeneratedContent,
        },
        {
            "streaming failures and deterministic cancellation",
            testStreamingFailuresAndDeterministicCancellation,
        },
        {
            "unsupported special resources are not copied silently",
            testUnsupportedSpecialResourcesAreNotCopiedSilently,
        },
        {
            "unsafe ZIP names",
            testUnsafeZIPNames,
        },
        {
            "duplicate ZIP entries",
            testDuplicateZIPEntries,
        },
        {
            "ZIP symlinks",
            testZIPSymlinks,
        },
#ifndef _WIN32
        {
            "physical links are rejected before indexing and lazy reads",
            testPhysicalLinksAreRejected,
        },
#endif
        {
            "ZIP case collisions and file/directory collisions",
            testZIPCaseCollisionsAndFileDirectoryCollisions,
        },
        {
            "cross-pack output path collisions",
            testCrossPackOutputPathCollisions,
        },
        {
            "corrupt archive",
            testCorruptArchive,
        },
        {
            "ZIP CRC failure on lazy read",
            testZIPCRCFailureOnLazyRead,
        },
        {
            "limits apply to directory and ZIP",
            testLimitsApplyToDirectoryAndZIP,
        },
        {
            "stack-wide limits",
            testStackWideLimits,
        },
        {
            "JSON size and depth limits",
            testJSONSizeAndDepthLimits,
        },
        {
            "merged JSON size limit",
            testMergedJSONSizeLimit,
        },
        {
            "lazy reads enforce caller limit and cancellation",
            testLazyReadsEnforceCallerLimitAndCancellation,
        },
        {
            "source changes are detected before lazy reads",
            testSourceChangesAreDetectedBeforeLazyReads,
        },
        {
            "changed ZIP source is detected",
            testChangedZIPSourceIsDetected,
        },
        {
            "invalid UTF-8 JSON",
            testInvalidUTF8JSON,
        },
        {
            "modern overlay minor boundaries",
            testModernOverlayMinorBoundaries,
        },
        {
            "cancelled operations",
            testCancelledOperations,
        },
        {
            "invalid requests and unsupported mixed packs",
            testInvalidRequestsAndUnsupportedMixedPacks,
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
