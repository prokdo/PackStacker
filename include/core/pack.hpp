#pragma once

#include "core/model.hpp"
#include "core/streams.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace PackStacker
{
    class ResolvedFile
    {
    public:
        using Reader = std::function<Result<std::string>(std::uint64_t, std::stop_token)>;
        using Copier = std::function<Result<void>(ByteWriter&, std::uint64_t, std::stop_token)>;

        ResolvedFile(std::vector<std::string> sourcePacks, std::uint64_t size, Reader reader,
                     Copier copier = {});

        const std::vector<std::string>& SourcePacks() const;
        std::uint64_t Size() const;
        Result<std::string> Read(std::uint64_t maxBytes, std::stop_token stop = {}) const;
        Result<void> CopyTo(ByteWriter& output, std::uint64_t maxBytes,
                            std::stop_token stop = {}) const;

    private:
        std::vector<std::string> sourcePacks;
        std::uint64_t size;
        Reader reader;
        Copier copier;
    };

    struct ResolvedPack
    {
        PackType Type = PackType::RESOURCE;
        PackFormat Target;
        std::map<std::string, ResolvedFile> Files;
        std::vector<std::string> SourcePaths;
        std::map<std::string, LanguageDefinition> Languages;
        std::vector<ResourceFilterBlock> Filters;
    };

    Result<PackInfo> InspectPack(std::string_view path, const Limits& limits = {},
                                 const std::stop_token& stop = {});
    Result<ResolvedPack> ResolveStack(const ResolveRequest& request,
                                      const std::stop_token& stop = {});
    Result<void> ExportPack(const ResolvedPack& pack, const ExportRequest& request,
                            const std::stop_token& stop = {});
}
