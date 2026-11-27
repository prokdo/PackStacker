#pragma once

#include "core/filesystem.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace PackStacker::Testing
{
    inline void Write(std::string_view path, std::string_view bytes)
    {
        const auto physical = PathFromUtf8(path);
        std::filesystem::create_directories(physical.parent_path());
        auto file = std::ofstream(physical, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            throw std::runtime_error("Cannot open fixture file");
        }
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (file.fail())
        {
            throw std::runtime_error("Cannot write fixture file");
        }
    }

    inline std::string Read(std::string_view path)
    {
        auto file = std::ifstream(PathFromUtf8(path), std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("Cannot read fixture file");
        }
        auto bytes = std::string{};
        auto buffer = std::array<char, 65536>{};
        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            bytes.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
        }
        if (!file.eof() || file.bad())
        {
            throw std::runtime_error("Fixture read failed");
        }
        return bytes;
    }

    inline StagingDirectory TemporaryDirectory()
    {
        auto directory = StagingDirectory::Create(std::filesystem::temp_directory_path());
        if (!directory)
        {
            throw std::runtime_error(directory.error());
        }
        return std::move(*directory);
    }
}
