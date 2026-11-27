#include "core/filesystem.hpp"
#include "core/filters.hpp"
#include "core/io.hpp"
#include "core/metadata.hpp"
#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/pack_source.hpp"
#include "core/rules.hpp"
#include "core/sounds.hpp"
#include "core/streams.hpp"
#include "core/unicode.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        enum class Rule : std::uint8_t
        {
            COPY,
            LANGUAGE,
            TAG,
            FONT,
            ATLAS,
            SOUNDS,
            LOW_PRIORITY,
            UNSUPPORTED,
        };

        Result<Rule> ruleFor(std::string_view path, PackType type, std::string_view pack)
        {
            const auto root = type == PackType::RESOURCE ? std::string_view{"assets/"}
                                                         : std::string_view{"data/"};
            const auto opposite = type == PackType::RESOURCE ? std::string_view{"data/"}
                                                             : std::string_view{"assets/"};
            if (path.starts_with(opposite))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::UNSUPPORTED_FEATURE,
                    .Message = "Mixed resource/data packs are not supported yet",
                    .Pack = std::string(pack),
                    .Path = std::string(path),
                });
            }
            if (!path.starts_with(root))
            {
                return Rule::COPY;
            }
            const auto identifier = path.substr(root.size());
            const auto separator = identifier.find('/');
            if (separator == std::string_view::npos)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INVALID_RESOURCE,
                    .Message = "Resource must have a namespace and path",
                    .Pack = std::string(pack),
                    .Path = std::string(path),
                });
            }
            const auto validIdentifier = std::ranges::all_of(
                identifier,
                [](const char code)
                {
                    return (code >= 'a' && code <= 'z') || (code >= '0' && code <= '9') ||
                           code == '_' || code == '-' || code == '.' || code == '/';
                });
            if (!validIdentifier)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INVALID_RESOURCE,
                    .Message = "Invalid resource identifier",
                    .Pack = std::string(pack),
                    .Path = std::string(path),
                });
            }
            const auto resource = identifier.substr(separator + 1);
            if (type != PackType::RESOURCE)
            {
                return resource.starts_with("tags/") && resource.ends_with(".json") ? Rule::TAG
                                                                                    : Rule::COPY;
            }
            if (resource.starts_with("lang/") && resource.find('/', 5) == std::string_view::npos)
            {
                if (path.ends_with(".lang"))
                {
                    return Rule::UNSUPPORTED;
                }
                if (path.ends_with(".json"))
                {
                    return Rule::LANGUAGE;
                }
            }
            if (resource.starts_with("font/") && resource.ends_with(".json"))
            {
                return Rule::FONT;
            }
            if (resource.starts_with("atlases/") && resource.ends_with(".json"))
            {
                return Rule::ATLAS;
            }
            if (resource == "sounds.json")
            {
                return Rule::SOUNDS;
            }
            return Rule::COPY;
        }

        Result<void> validateLanguage(const Json& value, std::string_view pack,
                                      std::string_view path)
        {
            if (value.is_object() &&
                std::ranges::all_of(value, [](const Json& item) { return item.is_string(); }))
            {
                return {};
            }
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::INVALID_RESOURCE,
                .Message = "Language JSON must map keys to strings",
                .Pack = std::string(pack),
                .Path = std::string(path),
            });
        }

        Result<void> validateTag(const Json& value, std::string_view pack, std::string_view path)
        {
            const auto invalid =
                [&](std::string_view message, ErrorCode code = ErrorCode::INVALID_RESOURCE)
            {
                return std::unexpected(Diagnostic{
                    .Code = code,
                    .Message = std::string(message),
                    .Pack = std::string(pack),
                    .Path = std::string(path),
                });
            };
            if (!value.is_object() || !value.contains("values") || !value.at("values").is_array() ||
                (value.contains("replace") && !value.at("replace").is_boolean()))
            {
                return invalid("Invalid tag object");
            }
            for (auto item = value.cbegin(); item != value.cend(); ++item)
            {
                if (item.key() != "values" && item.key() != "replace")
                {
                    return invalid("Non-vanilla tag fields are not supported",
                                   ErrorCode::UNSUPPORTED_FEATURE);
                }
            }
            for (const auto& entry : value.at("values"))
            {
                if (entry.is_string() && !entry.get_ref<const std::string&>().empty())
                {
                    continue;
                }
                if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string() ||
                    entry.at("id").get_ref<const std::string&>().empty() ||
                    (entry.contains("required") && !entry.at("required").is_boolean()))
                {
                    return invalid("Invalid tag value");
                }
                for (auto item = entry.cbegin(); item != entry.cend(); ++item)
                {
                    if (item.key() != "id" && item.key() != "required")
                    {
                        return invalid("Non-vanilla tag entry fields are not supported",
                                       ErrorCode::UNSUPPORTED_FEATURE);
                    }
                }
            }
            return {};
        }

        ResolvedFile sourceFile(const std::shared_ptr<const PackSource>& source,
                                std::string_view path)
        {
            const auto logical = std::string(path);
            return ResolvedFile(
                {source->Path()}, source->Entries().at(logical).Size,
                [source, logical](std::uint64_t limit, const std::stop_token& stop)
                { return source->Read(logical, limit, stop); },
                [source, logical](ByteWriter& output, std::uint64_t limit,
                                  const std::stop_token& stop)
                { return source->CopyTo(logical, output, limit, stop); });
        }

        Result<void> validateList(const Json& value, const char* key, std::string_view pack,
                                  std::string_view path)
        {
            if (!value.is_object() || !value.contains(key) || !value.at(key).is_array())
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INVALID_RESOURCE,
                    .Message = std::string("Missing resource list: ") + key,
                    .Pack = std::string(pack),
                    .Path = std::string(path),
                });
            }
            if (value.size() != 1)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::UNSUPPORTED_FEATURE,
                    .Message = "Unknown list resource fields",
                    .Pack = std::string(pack),
                    .Path = std::string(path),
                });
            }
            for (const auto& entry : value.at(key))
            {
                if (!entry.is_object() || !entry.contains("type") ||
                    !entry.at("type").is_string() ||
                    entry.at("type").get_ref<const std::string&>().empty())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::INVALID_RESOURCE,
                        .Message = "Resource list entries must have a type",
                        .Pack = std::string(pack),
                        .Path = std::string(path),
                    });
                }
            }
            return {};
        }

        void mergeJson(Json& merged, Json& parsed, Rule rule, bool replaces)
        {
            if (rule == Rule::SOUNDS)
            {
                merged = MergeSounds(std::move(merged), std::move(parsed));
            }
            else if (rule == Rule::LANGUAGE)
            {
                merged.update(parsed);
            }
            else if (rule == Rule::TAG)
            {
                merged.at("replace") = replaces || merged.value("replace", false);
                auto& values = merged.at("values");
                for (auto& value : parsed.at("values"))
                {
                    values.push_back(std::move(value));
                }
            }
            else if (rule == Rule::FONT)
            {
                auto providers = std::move(parsed.at("providers"));
                for (auto& provider : merged.at("providers"))
                {
                    providers.push_back(std::move(provider));
                }
                merged.at("providers") = std::move(providers);
            }
            else
            {
                for (auto& entry : parsed.at("sources"))
                {
                    merged.at("sources").push_back(std::move(entry));
                }
            }
        }

        Result<ResolvedFile> mergeFile(const ResolvedFile* lower, const ResolvedFile& higher,
                                       Rule rule, std::string_view pack, std::string_view path,
                                       const Limits& limits, const std::stop_token& stop)
        {
            auto bytes = higher.Read(limits.MaxJsonBytes, stop);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            auto parsed = ParseJson(*bytes, pack, path, limits, ErrorCode::INVALID_RESOURCE);
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }
            const auto valid = [&] -> Result<void>
            {
                switch (rule)
                {
                    case Rule::LANGUAGE:
                        return validateLanguage(*parsed, pack, path);
                    case Rule::SOUNDS:
                        return ValidateSounds(*parsed, pack, path);
                    case Rule::TAG:
                        return validateTag(*parsed, pack, path);
                    default:
                        return validateList(*parsed, rule == Rule::FONT ? "providers" : "sources",
                                            pack, path);
                }
            }();
            if (!valid)
            {
                return std::unexpected(valid.error());
            }
            auto merged = Json::object();
            if (rule == Rule::TAG)
            {
                merged = {{"replace", false}, {"values", Json::array()}};
            }
            else if (rule == Rule::FONT || rule == Rule::ATLAS)
            {
                merged.emplace(rule == Rule::FONT ? "providers" : "sources", Json::array());
            }
            auto origins = std::vector<std::string>{};
            const auto replaces = rule == Rule::TAG && parsed->value("replace", false);
            if ((lower != nullptr) && !replaces)
            {
                auto previous = lower->Read(limits.MaxJsonBytes, stop);
                if (!previous)
                {
                    return std::unexpected(previous.error());
                }
                auto previousJson =
                    ParseJson(*previous, pack, path, limits, ErrorCode::INVALID_RESOURCE);
                if (!previousJson)
                {
                    return std::unexpected(previousJson.error());
                }
                merged = std::move(*previousJson);
                origins = lower->SourcePacks();
            }
            mergeJson(merged, *parsed, rule, replaces);
            origins.insert(origins.begin(), std::string(pack));
            auto serialized = merged.dump();
            if (serialized.size() > limits.MaxJsonBytes)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::LIMIT_EXCEEDED,
                    .Message = "Merged JSON size limit exceeded",
                    .Pack = std::string(pack),
                    .Path = std::string(path),
                });
            }
            const auto contentSize = static_cast<std::uint64_t>(serialized.size());
            return ResolvedFile(
                std::move(origins), contentSize,
                [content = std::move(serialized), pack = std::string(pack),
                 path = std::string(path)](std::uint64_t limit,
                                           const std::stop_token& token) -> Result<std::string>
                {
                    if (token.stop_requested())
                    {
                        return std::unexpected(Diagnostic{
                            .Code = ErrorCode::CANCELLED,
                            .Message = "Cancelled",
                            .Pack = pack,
                            .Path = path,
                        });
                    }
                    if (std::cmp_greater(content.size(), limit))
                    {
                        return std::unexpected(Diagnostic{
                            .Code = ErrorCode::LIMIT_EXCEEDED,
                            .Message = "Read size limit exceeded",
                            .Pack = pack,
                            .Path = path,
                        });
                    }
                    return content;
                });
        }

        struct EffectiveEntry
        {
            std::string_view Logical;
            std::string_view Physical;
        };

        std::map<std::u16string, EffectiveEntry> effectiveEntries(const PackSource& source,
                                                                  const Metadata& metadata,
                                                                  PackFormat target)
        {
            auto result = std::map<std::u16string, EffectiveEntry>{};
            for (const auto& entry : source.Entries())
            {
                const auto& path = entry.first;
                if (path == "pack.mcmeta")
                {
                    continue;
                }
                const auto inOverlay =
                    std::ranges::any_of(metadata.Overlays, [&](const Overlay& overlay)
                                        { return path.starts_with(overlay.Directory + '/'); });
                if (!inOverlay)
                {
                    result.insert_or_assign(*Utf16FromUtf8(path),
                                            EffectiveEntry{.Logical = path, .Physical = path});
                }
            }
            for (const auto& overlay : metadata.Overlays)
            {
                if (!overlay.Formats.Contains(target))
                {
                    continue;
                }
                const auto prefix = overlay.Directory + '/';
                for (const auto& entry : source.Entries())
                {
                    const auto path = std::string_view(entry.first);
                    if (path.starts_with(prefix))
                    {
                        const auto logical = path.substr(prefix.size());
                        if (logical.starts_with("assets/") || logical.starts_with("data/"))
                        {
                            result.insert_or_assign(
                                *Utf16FromUtf8(logical),
                                EffectiveEntry{.Logical = logical, .Physical = path});
                        }
                    }
                }
            }
            return result;
        }

        Result<void> applyMetadata(ResolvedPack& result, const Metadata& metadata,
                                   const ResolveRequest& request, std::string_view pack,
                                   std::uint64_t& filterBudget, const std::stop_token& stop)
        {
            if (!metadata.Formats.Contains(request.Target))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INCOMPATIBLE_FORMAT,
                    .Message = "Pack does not declare support for the target format",
                    .Pack = std::string(pack),
                    .Path = "pack.mcmeta",
                });
            }
            if (request.Type == PackType::DATA && !metadata.Languages.empty())
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::UNSUPPORTED_FEATURE,
                    .Message = "Language definitions are supported for resource packs only",
                    .Pack = std::string(pack),
                    .Path = "pack.mcmeta",
                });
            }
            for (const auto& [code, language] : metadata.Languages)
            {
                result.Languages.try_emplace(code, language);
            }
            if (std::cmp_greater(result.Languages.size(), request.Limits.MaxEntries))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::LIMIT_EXCEEDED,
                    .Message = "Language definition limit exceeded",
                    .Pack = std::string(pack),
                    .Path = "pack.mcmeta",
                });
            }
            for (const auto& filter : metadata.Filters)
            {
                result.Filters.push_back(filter.Block);
            }
            if (!result.Filters.empty() &&
                request.Target.Major < (request.Type == PackType::RESOURCE ? 9 : 10))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::UNSUPPORTED_FEATURE,
                    .Message = "The target format predates pack filters",
                    .Pack = std::string(pack),
                    .Path = "pack.mcmeta",
                });
            }
            if (auto valid = ValidateFilterSet(result.Filters, request.Limits, pack); !valid)
            {
                return std::unexpected(valid.error());
            }
            if (auto applied = ApplyFilters(result.Files, metadata.Filters, request.Type,
                                            filterBudget, pack, stop);
                !applied)
            {
                return std::unexpected(applied.error());
            }
            return {};
        }

        Rule selectedRule(Rule standard, MergeStrategy strategy)
        {
            switch (strategy)
            {
                case MergeStrategy::STANDARD:
                    return standard;
                case MergeStrategy::HIGH_PRIORITY:
                    return Rule::COPY;
                case MergeStrategy::LOW_PRIORITY:
                    return Rule::LOW_PRIORITY;
                case MergeStrategy::LANGUAGE:
                    return Rule::LANGUAGE;
                case MergeStrategy::TAG:
                    return Rule::TAG;
                case MergeStrategy::FONT:
                    return Rule::FONT;
                case MergeStrategy::ATLAS:
                    return Rule::ATLAS;
                case MergeStrategy::SOUNDS:
                    return Rule::SOUNDS;
            }
            return standard;
        }

        Result<Rule> entryRule(std::string_view path, const std::string& pack,
                               const ResolveRequest& request, std::uint64_t& budget,
                               const std::stop_token& stop)
        {
            auto rule = ruleFor(path, request.Type, pack);
            if (!rule || path.ends_with(".mcmeta"))
            {
                return rule;
            }
            auto strategy = MatchRules(request.Rules, path, budget, stop);
            if (!strategy)
            {
                strategy.error().Pack = pack;
                return std::unexpected(std::move(strategy.error()));
            }
            return selectedRule(*rule, *strategy);
        }

        Result<void> mergeCompanions(ResolvedPack& result,
                                     const std::shared_ptr<const PackSource>& source,
                                     const std::map<std::u16string, EffectiveEntry>& effective,
                                     const std::set<std::string>& preservedMetadata,
                                     const ResolveRequest& request, std::uint64_t& ruleBudget,
                                     const std::stop_token& stop)
        {
            constexpr auto suffix = std::string_view(".mcmeta");
            for (const auto& [key, entry] : effective)
            {
                if (!entry.Logical.ends_with(suffix))
                {
                    continue;
                }
                const auto path = std::string(entry.Logical);
                if (preservedMetadata.contains(path))
                {
                    continue;
                }
                const auto base = path.substr(0, path.size() - suffix.size());
                if (!request.Rules.empty() && result.Files.contains(base) &&
                    !effective.contains(key.substr(0, key.size() - suffix.size())))
                {
                    const auto rule = entryRule(base, source->Path(), request, ruleBudget, stop);
                    if (!rule)
                    {
                        return std::unexpected(rule.error());
                    }
                    if (*rule == Rule::LOW_PRIORITY)
                    {
                        continue;
                    }
                }
                result.Files.insert_or_assign(path, sourceFile(source, entry.Physical));
            }
            return {};
        }

        Result<void> mergeEntries(ResolvedPack& result,
                                  const std::shared_ptr<const PackSource>& source,
                                  const Metadata& metadata, const ResolveRequest& request,
                                  std::uint64_t& ruleBudget, const std::stop_token& stop)
        {
            const auto& pack = source->Path();
            const auto effective = effectiveEntries(*source, metadata, request.Target);
            auto preservedMetadata = std::set<std::string>{};
            for (const auto& item : effective)
            {
                const auto& entry = item.second;
                if (stop.stop_requested())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::CANCELLED,
                        .Message = "Cancelled",
                        .Pack = pack,
                        .Path = std::string(entry.Logical),
                    });
                }
                const auto rule = entryRule(entry.Logical, pack, request, ruleBudget, stop);
                if (!rule)
                {
                    return std::unexpected(rule.error());
                }
                if (*rule == Rule::UNSUPPORTED)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::UNSUPPORTED_FEATURE,
                        .Message = "This resource requires a merger not implemented yet",
                        .Pack = pack,
                        .Path = std::string(entry.Logical),
                    });
                }
                if (entry.Logical.ends_with(".mcmeta"))
                {
                    continue;
                }
                const auto path = std::string(entry.Logical);
                if (*rule == Rule::LOW_PRIORITY && result.Files.contains(path))
                {
                    preservedMetadata.insert(path + ".mcmeta");
                    continue;
                }
                result.Files.erase(path + ".mcmeta");
                auto file = sourceFile(source, entry.Physical);
                if (*rule != Rule::COPY && *rule != Rule::LOW_PRIORITY)
                {
                    const auto lower = result.Files.find(path);
                    auto merged = mergeFile(lower == result.Files.cend() ? nullptr : &lower->second,
                                            file, *rule, pack, entry.Logical, request.Limits, stop);
                    if (!merged)
                    {
                        return std::unexpected(merged.error());
                    }
                    file = std::move(*merged);
                }
                result.Files.insert_or_assign(path, std::move(file));
            }
            return mergeCompanions(result, source, effective, preservedMetadata, request,
                                   ruleBudget, stop);
        }

    }

    ResolvedFile::ResolvedFile(std::vector<std::string> sourcePacks, std::uint64_t size,
                               Reader reader, Copier copier)
        : sourcePacks(std::move(sourcePacks)),
          size(size),
          reader(std::move(reader)),
          copier(std::move(copier))
    {
    }

    const std::vector<std::string>& ResolvedFile::SourcePacks() const
    {
        return sourcePacks;
    }

    std::uint64_t ResolvedFile::Size() const
    {
        return size;
    }

    Result<std::string> ResolvedFile::Read(std::uint64_t maxBytes, std::stop_token stop) const
    {
        if (stop.stop_requested())
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::CANCELLED,
                .Message = "Cancelled",
                .Pack = {},
                .Path = {},
            });
        }
        if (size > maxBytes)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::LIMIT_EXCEEDED,
                .Message = "Read size limit exceeded",
                .Pack = {},
                .Path = {},
            });
        }
        if (!reader)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::INVALID_RESOURCE,
                .Message = "Missing content reader",
                .Pack = {},
                .Path = {},
            });
        }
        auto result = reader(maxBytes, std::move(stop));
        if (result && std::cmp_not_equal(result->size(), size))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::SOURCE_CHANGED,
                .Message = "Entry size changed",
                .Pack = {},
                .Path = {},
            });
        }
        return result;
    }

    Result<void> ResolvedFile::CopyTo(ByteWriter& output, std::uint64_t maxBytes,
                                      std::stop_token stop) const
    {
        if (stop.stop_requested())
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::CANCELLED,
                .Message = "Cancelled",
                .Pack = {},
                .Path = {},
            });
        }
        if (size > maxBytes)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::LIMIT_EXCEEDED,
                .Message = "Copy size limit exceeded",
                .Pack = {},
                .Path = {},
            });
        }
        if (!output.IsWritable())
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::IO,
                .Message = "Output device must be writable in binary mode",
                .Pack = {},
                .Path = {},
            });
        }
        if (copier)
        {
            return copier(output, maxBytes, std::move(stop));
        }
        auto bytes = Read(maxBytes, stop);
        if (!bytes)
        {
            return std::unexpected(bytes.error());
        }
        const auto origin =
            sourcePacks.empty() ? std::string_view{} : std::string_view(sourcePacks.front());
        return WriteAll(output, *bytes, origin, {}, stop);
    }

    Result<ResolvedPack> ResolveStack(const ResolveRequest& request, const std::stop_token& stop)
    {
        if (stop.stop_requested())
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::CANCELLED,
                .Message = "Cancelled",
                .Pack = {},
                .Path = {},
            });
        }
        if (request.PacksHighToLow.empty())
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::INVALID_RESOURCE,
                .Message = "No input packs",
                .Pack = {},
                .Path = {},
            });
        }
        if (std::cmp_greater(request.PacksHighToLow.size(), request.Limits.MaxPacks))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::LIMIT_EXCEEDED,
                .Message = "Too many input packs",
                .Pack = {},
                .Path = {},
            });
        }
        if (request.Target.Major < 4 || request.Target.Minor < 0)
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::UNSUPPORTED_FEATURE,
                .Message = "This stage supports pack formats starting at 4",
                .Pack = {},
                .Path = {},
            });
        }
        if (auto valid = ValidateRules(request.Rules, request.Limits); !valid)
        {
            return std::unexpected(valid.error());
        }
        auto result = ResolvedPack{
            .Type = request.Type,
            .Target = request.Target,
            .Files = {},
            .SourcePaths = {},
            .Languages = {},
            .Filters = {},
        };
        auto sourcePaths = std::set<std::string>{};
        auto totalBytes = std::uint64_t{0};
        auto totalEntries = std::uint64_t{0};
        auto filterBudget = request.Limits.MaxFilterMatchSteps;
        auto ruleBudget = request.Limits.MaxFilterMatchSteps;
        for (auto input = request.PacksHighToLow.crbegin(); input != request.PacksHighToLow.crend();
             ++input)
        {
            if (input->empty())
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INVALID_PATH,
                    .Message = "Empty pack path",
                    .Pack = {},
                    .Path = {},
                });
            }
            auto source = PackSource::Open(*input, request.Limits, stop);
            if (!source)
            {
                return std::unexpected(source.error());
            }
            const auto& pack = (*source)->Path();
            auto error = std::error_code{};
            const auto canonicalPath = std::filesystem::canonical(PathFromUtf8(pack), error);
            if (error)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = "Cannot resolve source path",
                    .Pack = pack,
                    .Path = {},
                });
            }
            const auto canonical = PathToUtf8(canonicalPath);
            if (sourcePaths.contains(canonical))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INVALID_PATH,
                    .Message = "The same pack was selected more than once",
                    .Pack = pack,
                    .Path = {},
                });
            }
            sourcePaths.insert(canonical);
            result.SourcePaths.insert(result.SourcePaths.begin(), canonical);
            const auto count = static_cast<std::uint64_t>((*source)->Entries().size());
            if (count > request.Limits.MaxEntries - totalEntries ||
                (*source)->TotalBytes() > request.Limits.MaxTotalBytes - totalBytes)
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::LIMIT_EXCEEDED,
                    .Message = "Stack size limit exceeded",
                    .Pack = pack,
                    .Path = {},
                });
            }
            totalEntries += count;
            totalBytes += (*source)->TotalBytes();
            auto metadata = ReadMetadata(**source, request.Limits, stop);
            if (!metadata)
            {
                return std::unexpected(metadata.error());
            }
            if (auto applied = applyMetadata(result, *metadata, request, pack, filterBudget, stop);
                !applied)
            {
                return std::unexpected(applied.error());
            }
            if (auto merged = mergeEntries(result, *source, *metadata, request, ruleBudget, stop);
                !merged)
            {
                return std::unexpected(merged.error());
            }
        }
        if (auto valid = ValidateOutputPaths(result.Files, request.Limits.MaxEntries); !valid)
        {
            return std::unexpected(valid.error());
        }
        return result;
    }
}
