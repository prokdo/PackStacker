#include "core/metadata.hpp"

#include "core/filters.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/unicode.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        std::optional<int32_t> integer(const Json& value)
        {
            if (!value.is_number_integer())
            {
                return std::nullopt;
            }
            if (value.is_number_unsigned())
            {
                const auto number = value.get<std::uint64_t>();
                if (number <= static_cast<std::uint64_t>(std::numeric_limits<int32_t>::max()))
                {
                    return static_cast<int32_t>(number);
                }
                return std::nullopt;
            }
            const auto number = value.get<std::int64_t>();
            if (number < 0 || number > std::numeric_limits<int32_t>::max())
            {
                return std::nullopt;
            }
            return static_cast<int32_t>(number);
        }

        std::optional<PackFormat> format(const Json& value, bool maximum)
        {
            const auto defaultMinor = maximum ? std::numeric_limits<int32_t>::max() : 0;
            if (const auto number = integer(value))
            {
                return PackFormat{.Major = *number, .Minor = defaultMinor};
            }
            if (!value.is_array() || value.empty() || value.size() > 2)
            {
                return std::nullopt;
            }
            const auto major = integer(value.at(0));
            const auto minor =
                value.size() == 2 ? integer(value.at(1)) : std::optional<int32_t>(defaultMinor);
            if (!major || !minor)
            {
                return std::nullopt;
            }
            return PackFormat{.Major = *major, .Minor = *minor};
        }

        std::optional<FormatRange> legacyRange(const Json& value)
        {
            auto minimum = std::optional<int32_t>{};
            auto maximum = std::optional<int32_t>{};
            if (const auto number = integer(value))
            {
                minimum = maximum = number;
            }
            else if (value.is_array() && value.size() == 2)
            {
                minimum = integer(value.at(0));
                maximum = integer(value.at(1));
            }
            else if (value.is_object() && value.size() == 2 && value.contains("min_inclusive") &&
                     value.contains("max_inclusive"))
            {
                minimum = integer(value.at("min_inclusive"));
                maximum = integer(value.at("max_inclusive"));
            }
            if (!minimum || !maximum || *minimum > *maximum)
            {
                return std::nullopt;
            }
            return FormatRange{
                .Minimum = {.Major = *minimum, .Minor = 0},
                .Maximum = {.Major = *maximum, .Minor = std::numeric_limits<int32_t>::max()},
            };
        }

        std::optional<FormatRange> modernRange(const Json& object)
        {
            if (!object.contains("min_format") || !object.contains("max_format"))
            {
                return std::nullopt;
            }
            const auto minimum = format(object.at("min_format"), false);
            const auto maximum = format(object.at("max_format"), true);
            if (!minimum || !maximum || *minimum > *maximum)
            {
                return std::nullopt;
            }
            return FormatRange{.Minimum = *minimum, .Maximum = *maximum};
        }

        std::optional<FormatRange> formats(const Json& object, const char* legacyKey, bool pack)
        {
            auto result = std::optional<FormatRange>{};
            const auto modern = object.contains("min_format") || object.contains("max_format");
            if (modern)
            {
                result = modernRange(object);
                if (!result)
                {
                    return std::nullopt;
                }
            }
            if (object.contains(legacyKey))
            {
                const auto legacy = legacyRange(object.at(legacyKey));
                if (!legacy || (result && (result->Minimum.Major != legacy->Minimum.Major ||
                                           result->Maximum.Major != legacy->Maximum.Major)))
                {
                    return std::nullopt;
                }
                if (!result)
                {
                    result = legacy;
                }
            }
            if (pack)
            {
                if (object.contains("pack_format"))
                {
                    const auto primary = integer(object.at("pack_format"));
                    if (!primary || (result && (*primary < result->Minimum.Major ||
                                                *primary > result->Maximum.Major)))
                    {
                        return std::nullopt;
                    }
                    if (!result)
                    {
                        result = FormatRange{
                            .Minimum = {.Major = *primary, .Minor = 0},
                            .Maximum =
                                {
                                    .Major = *primary,
                                    .Minor = std::numeric_limits<int32_t>::max(),
                                },
                        };
                    }
                }
                else if (!modern)
                {
                    return std::nullopt;
                }
            }
            return result;
        }

        bool knownKeys(const Json& object, std::initializer_list<const char*> allowed)
        {
            for (auto entry = object.cbegin(); entry != object.cend(); ++entry)
            {
                if (std::ranges::none_of(
                        allowed, [&](const char* candidate) { return entry.key() == candidate; }))
                {
                    return false;
                }
            }
            return true;
        }

        std::unexpected<Diagnostic> metadataError(ErrorCode code, std::string_view message,
                                                  std::string_view pack)
        {
            return std::unexpected(Diagnostic{
                .Code = code,
                .Message = std::string(message),
                .Pack = std::string(pack),
                .Path = "pack.mcmeta",
            });
        }

        Result<std::map<std::string, LanguageDefinition>> readLanguages(const Json& languages,
                                                                        const Limits& limits,
                                                                        std::string_view pack)
        {
            if (!languages.is_object())
            {
                return metadataError(ErrorCode::INVALID_METADATA,
                                     "Invalid language metadata object", pack);
            }
            if (languages.size() > limits.MaxEntries)
            {
                return metadataError(ErrorCode::LIMIT_EXCEEDED,
                                     "Language definition limit exceeded", pack);
            }
            auto result = std::map<std::string, LanguageDefinition>{};
            for (const auto& [code, value] : languages.items())
            {
                if (!value.is_object() || !value.contains("name") ||
                    !value.at("name").is_string() || !value.contains("region") ||
                    !value.at("region").is_string() ||
                    (value.contains("bidirectional") && !value.at("bidirectional").is_boolean()))
                {
                    return metadataError(ErrorCode::INVALID_METADATA, "Invalid language definition",
                                         pack);
                }
                if (!knownKeys(value, {"name", "region", "bidirectional"}))
                {
                    return metadataError(ErrorCode::UNSUPPORTED_FEATURE,
                                         "Unknown language definition field", pack);
                }
                result.emplace(code, LanguageDefinition{
                                         .Name = value.at("name").get<std::string>(),
                                         .Region = value.at("region").get<std::string>(),
                                         .Bidirectional = value.value("bidirectional", false),
                                     });
            }
            if (auto valid = ValidateLanguages(result, pack); !valid)
            {
                return std::unexpected(valid.error());
            }
            return result;
        }

        Result<std::vector<CompiledFilter>> readFilters(const Json& filter, const Limits& limits,
                                                        std::string_view pack)
        {
            if (!filter.is_object() || !filter.contains("block") || !filter.at("block").is_array())
            {
                return metadataError(ErrorCode::INVALID_METADATA, "Invalid filter object", pack);
            }
            if (!knownKeys(filter, {"block"}))
            {
                return metadataError(ErrorCode::UNSUPPORTED_FEATURE, "Unknown filter field", pack);
            }
            if (filter.at("block").size() > limits.MaxFilterBlocks)
            {
                return metadataError(ErrorCode::LIMIT_EXCEEDED, "Filter block limit exceeded",
                                     pack);
            }
            auto blocks = std::vector<ResourceFilterBlock>{};
            for (const auto& block : filter.at("block"))
            {
                if (!block.is_object() ||
                    (block.contains("namespace") && !block.at("namespace").is_string()) ||
                    (block.contains("path") && !block.at("path").is_string()))
                {
                    return metadataError(ErrorCode::INVALID_METADATA, "Invalid filter block", pack);
                }
                if (!knownKeys(block, {"namespace", "path"}))
                {
                    return metadataError(ErrorCode::UNSUPPORTED_FEATURE,
                                         "Unknown filter block field", pack);
                }
                blocks.push_back({
                    .NamespacePattern = block.value("namespace", std::string(".*")),
                    .PathPattern = block.value("path", std::string(".*")),
                });
            }
            return CompileFilters(blocks, limits, pack);
        }

        Result<Overlay> readOverlay(const Json& overlay, std::string_view pack)
        {
            if (!overlay.is_object() ||
                !knownKeys(overlay, {"directory", "formats", "min_format", "max_format"}) ||
                !overlay.contains("directory") || !overlay.at("directory").is_string())
            {
                return metadataError(ErrorCode::INVALID_METADATA, "Invalid overlay entry", pack);
            }
            const auto& directory = overlay.at("directory").get_ref<const std::string&>();
            if (directory.empty() || directory.contains('/') || directory == "assets" ||
                directory == "data" || !ValidatePortablePath(directory))
            {
                return metadataError(ErrorCode::INVALID_METADATA, "Invalid overlay directory",
                                     pack);
            }
            for (const auto character : directory)
            {
                const auto code = character;
                if ((code < 'a' || code > 'z') && (code < '0' || code > '9') && code != '_' &&
                    code != '-' && code != '.')
                {
                    return metadataError(ErrorCode::INVALID_METADATA, "Invalid overlay directory",
                                         pack);
                }
            }
            const auto range = formats(overlay, "formats", false);
            if (!range)
            {
                return metadataError(ErrorCode::INVALID_METADATA, "Invalid overlay format range",
                                     pack);
            }
            return Overlay{.Directory = directory, .Formats = *range};
        }

        Result<std::vector<Overlay>> readOverlays(const Json& overlays, std::string_view pack)
        {
            if (!overlays.is_object() || !knownKeys(overlays, {"entries"}) ||
                !overlays.contains("entries") || !overlays.at("entries").is_array())
            {
                return metadataError(ErrorCode::INVALID_METADATA, "Invalid overlays object", pack);
            }
            auto result = std::vector<Overlay>{};
            for (const auto& entry : overlays.at("entries"))
            {
                auto overlay = readOverlay(entry, pack);
                if (!overlay)
                {
                    return std::unexpected(overlay.error());
                }
                result.push_back(std::move(*overlay));
            }
            return result;
        }
    }

    Result<Json> ParseJson(std::string_view bytes, std::string_view pack, std::string_view path,
                           const Limits& limits, ErrorCode code)
    {
        if (std::cmp_greater(bytes.size(), limits.MaxJsonBytes))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::LIMIT_EXCEEDED,
                .Message = "JSON size limit exceeded",
                .Pack = std::string(pack),
                .Path = std::string(path),
            });
        }
        auto depth = std::int64_t{0};
        auto quoted = false;
        auto escaped = false;
        for (const auto character : bytes)
        {
            if (quoted)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (character == '\\')
                {
                    escaped = true;
                }
                else if (character == '"')
                {
                    quoted = false;
                }
            }
            else if (character == '"')
            {
                quoted = true;
            }
            else if (character == '{' || character == '[')
            {
                if (++depth > limits.MaxJsonDepth)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::LIMIT_EXCEEDED,
                        .Message = "JSON nesting limit exceeded",
                        .Pack = std::string(pack),
                        .Path = std::string(path),
                    });
                }
            }
            else if (character == '}' || character == ']')
            {
                --depth;
            }
        }
        auto objectKeys = std::vector<std::set<std::string>>{};
        auto duplicate = false;
        const auto callback = [&](int32_t, Json::parse_event_t event, Json& parsed)
        {
            if (event == Json::parse_event_t::object_start)
            {
                objectKeys.emplace_back();
            }
            else if (event == Json::parse_event_t::object_end)
            {
                objectKeys.pop_back();
            }
            else if (event == Json::parse_event_t::key)
            {
                duplicate |= !objectKeys.back().insert(parsed.get<std::string>()).second;
            }
            return true;
        };
        auto parsed = Json::parse(bytes.begin(), bytes.end(), callback, false);
        if (parsed.is_discarded() || duplicate)
        {
            return std::unexpected(Diagnostic{
                .Code = code,
                .Message = duplicate ? "Duplicate JSON key" : "Invalid JSON",
                .Pack = std::string(pack),
                .Path = std::string(path),
            });
        }
        return parsed;
    }

    Result<void> ValidateLanguages(const std::map<std::string, LanguageDefinition>& languages,
                                   std::string_view pack)
    {
        for (const auto& [code, language] : languages)
        {
            auto validCode = !code.empty() && code.size() <= 16;
            for (const auto character : code)
            {
                const auto value = character;
                validCode &= (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
                             value == '_' || value == '-' || value == '.';
            }
            if (!validCode || language.Name.empty() || language.Region.empty() ||
                !Utf16FromUtf8(language.Name) || !Utf16FromUtf8(language.Region))
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::INVALID_METADATA,
                    .Message = "Invalid language definition: " + code,
                    .Pack = std::string(pack),
                    .Path = "pack.mcmeta",
                });
            }
        }
        return {};
    }

    Result<Metadata> ParseMetadata(std::string_view bytes, std::string_view sourcePath,
                                   const Limits& limits)
    {
        const auto invalid = [&](std::string_view message)
        { return metadataError(ErrorCode::INVALID_METADATA, message, sourcePath); };
        const auto unsupported = [&](std::string_view message)
        { return metadataError(ErrorCode::UNSUPPORTED_FEATURE, message, sourcePath); };
        auto parsed =
            ParseJson(bytes, sourcePath, "pack.mcmeta", limits, ErrorCode::INVALID_METADATA);
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }
        const auto& root = *parsed;
        if (!root.is_object() || !root.contains("pack") || !root.at("pack").is_object())
        {
            return invalid("Missing pack metadata object");
        }
        if (!knownKeys(root, {"pack", "overlays", "language", "filter"}))
        {
            return unsupported("Unsupported metadata section");
        }
        const auto& pack = root.at("pack");
        if (!knownKeys(pack, {
                                 "description",
                                 "pack_format",
                                 "supported_formats",
                                 "min_format",
                                 "max_format",
                             }))
        {
            return unsupported("Unknown pack metadata field");
        }
        if (!pack.contains("description") ||
            (!pack.at("description").is_string() && !pack.at("description").is_object() &&
             !pack.at("description").is_array()))
        {
            return invalid("Invalid pack description");
        }
        const auto range = formats(pack, "supported_formats", true);
        if (!range)
        {
            return invalid("Invalid or inconsistent pack format range");
        }
        auto result = Metadata{.Formats = *range, .Overlays = {}, .Languages = {}, .Filters = {}};
        if (root.contains("language"))
        {
            auto languages = readLanguages(root.at("language"), limits, sourcePath);
            if (!languages)
            {
                return std::unexpected(languages.error());
            }
            result.Languages = std::move(*languages);
        }
        if (root.contains("filter"))
        {
            auto filters = readFilters(root.at("filter"), limits, sourcePath);
            if (!filters)
            {
                return std::unexpected(filters.error());
            }
            result.Filters = std::move(*filters);
        }
        if (root.contains("overlays"))
        {
            auto overlays = readOverlays(root.at("overlays"), sourcePath);
            if (!overlays)
            {
                return std::unexpected(overlays.error());
            }
            result.Overlays = std::move(*overlays);
        }
        return result;
    }

}
