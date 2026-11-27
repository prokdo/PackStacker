#include "core/filesystem.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/streams.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::string pack(const std::filesystem::path& root,
                     const std::map<std::string, std::string>& files)
    {
        for (const auto& [name, bytes] : files)
        {
            const auto path = root / PackStacker::PathFromUtf8(name);
            std::filesystem::create_directories(path.parent_path());
            auto output = PackStacker::FileWriter::Open(path);
            require(output.has_value(), "Cannot create fixture file");
            require(PackStacker::WriteBytes(*output, bytes).has_value(), "Cannot write fixture");
            require(output->Close().has_value(), "Cannot close fixture");
        }
        return PackStacker::PathToUtf8(root);
    }

    void checkResolution()
    {
        auto directory =
            PackStacker::StagingDirectory::Create(std::filesystem::temp_directory_path());
        require(directory.has_value(), "Cannot create temporary directory");
        const auto low = pack(
            directory->Path() / std::filesystem::path(u8"нижний-😀"),
            {
                {
                    "pack.mcmeta",
                    R"({"pack":{"pack_format":34,"description":"low"},"language":{"custom":{"name":"Low","region":"Region"}}})",
                },
                {"assets/example/lang/en_us.json", R"({"a":"low","b":"kept"})"},
                {"assets/example/textures/test.bin", "low"},
                {"assets/example/textures/test.bin.mcmeta", "low-meta"},
                {"assets/example/textures/blocked.bin", "blocked"},
            });
        const auto high = pack(
            directory->Path() / std::filesystem::path(u8"верхний-😀"),
            {
                {
                    "pack.mcmeta",
                    R"({"pack":{"pack_format":34,"description":"high"},"language":{"custom":{"name":"High","region":"Region"}},"filter":{"block":[{"namespace":"example","path":"textures/blocked\\.bin"}]},"overlays":{"entries":[{"directory":"overlay","formats":34}]}})",
                },
                {"assets/example/lang/en_us.json", R"({"a":"high"})"},
                {"assets/example/textures/test.bin", "base"},
                {"overlay/assets/example/textures/test.bin", "overlay"},
                {"extra/текст-😀", "opaque"},
            });
        const auto resolved = PackStacker::ResolveStack({
            .PacksHighToLow = {high, low},
            .Type = PackStacker::PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        });
        require(resolved.has_value(), "Cannot resolve stack");
        require(resolved->SourcePaths == std::vector<std::string>{high, low},
                "Source order changed");
        require(resolved->Languages.at("custom").Name == "Low", "Language first-wins changed");
        require(!resolved->Files.contains("assets/example/textures/blocked.bin"),
                "Pack filter ignored");
        require(!resolved->Files.contains("assets/example/textures/test.bin.mcmeta"),
                "Stale companion metadata retained");
        const auto translated = resolved->Files.at("assets/example/lang/en_us.json").Read(1024);
        require(translated && *translated == R"({"a":"high","b":"kept"})",
                "Translation priority changed");
        const auto& texture = resolved->Files.at("assets/example/textures/test.bin");
        const auto bytes = texture.Read(1024);
        require(bytes && *bytes == "overlay", "Overlay priority changed");
        auto output = PackStacker::MemoryWriter{};
        require(texture.CopyTo(output, 1024).has_value() && output.Bytes() == "overlay",
                "Lazy source copier changed");
        const auto extra = resolved->Files.at("extra/текст-😀").Read(1024);
        require(extra && *extra == "opaque", "Unicode extra entry changed");
        const auto duplicate = PackStacker::ResolveStack({
            .PacksHighToLow = {high, high + "/."},
            .Type = PackStacker::PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        });
        require(!duplicate && duplicate.error().Code == PackStacker::ErrorCode::INVALID_PATH &&
                    duplicate.error().Message == "The same pack was selected more than once",
                "Duplicate source detection changed");
    }

    void checkDiagnosticOrder()
    {
        auto directory =
            PackStacker::StagingDirectory::Create(std::filesystem::temp_directory_path());
        require(directory.has_value(), "Cannot create temporary directory");
        const auto input =
            pack(directory->Path(),
                 {
                     {"pack.mcmeta", R"({"pack":{"pack_format":34,"description":"order"}})"},
                     {"assets/\ue000/file", "x"},
                     {"assets/😀/file", "x"},
                 });
        const auto resolved = PackStacker::ResolveStack({
            .PacksHighToLow = {input},
            .Type = PackStacker::PackType::RESOURCE,
            .Target = {.Major = 34, .Minor = 0},
            .Limits = {},
            .Rules = {},
        });
        require(!resolved && resolved.error().Code == PackStacker::ErrorCode::INVALID_RESOURCE &&
                    resolved.error().Path == "assets/😀/file",
                "Resolution diagnostic order changed from UTF-16 ordering");
        const auto cancellation = std::stop_source{};
        cancellation.request_stop();
        const auto cancelled = PackStacker::ResolveStack({}, cancellation.get_token());
        require(!cancelled && cancelled.error().Code == PackStacker::ErrorCode::CANCELLED,
                "Cancellation precedence changed");
    }
}

int32_t main()
{
    try
    {
        checkResolution();
        checkDiagnosticOrder();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
