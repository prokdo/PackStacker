#include "core/metadata.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/pack_source.hpp"

#include <cstdint>
#include <expected>
#include <stop_token>
#include <string_view>

namespace PackStacker
{
    Result<Metadata> ReadMetadata(const PackSource& source, const Limits& limits,
                                  const std::stop_token& stop)
    {
        constexpr auto path = std::string_view{"pack.mcmeta"};
        const auto& sourcePath = source.Path();
        if (!source.Entries().contains(path))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::INVALID_METADATA,
                .Message = "pack.mcmeta must be at the pack root",
                .Pack = sourcePath,
                .Path = "pack.mcmeta",
            });
        }
        auto bytes = source.Read(path, limits.MaxJsonBytes, stop);
        if (!bytes)
        {
            return std::unexpected(bytes.error());
        }
        return ParseMetadata(*bytes, sourcePath, limits);
    }

    Result<PackInfo> InspectPack(std::string_view path, const Limits& limits,
                                 const std::stop_token& stop)
    {
        auto source = PackSource::Open(path, limits, stop);
        if (!source)
        {
            return std::unexpected(source.error());
        }
        auto metadata = ReadMetadata(**source, limits, stop);
        if (!metadata)
        {
            return std::unexpected(metadata.error());
        }
        return PackInfo{
            .Path = (*source)->Path(),
            .Formats = metadata->Formats,
            .FileCount = static_cast<std::uint64_t>((*source)->Entries().size()),
            .TotalBytes = (*source)->TotalBytes(),
        };
    }
}
