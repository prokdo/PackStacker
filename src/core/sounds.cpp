#include "core/sounds.hpp"

#include "core/metadata.hpp"
#include "core/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace PackStacker
{
    namespace
    {
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

        bool identifier(const std::string& value, bool namespaceAllowed)
        {
            if (value.empty())
            {
                return false;
            }
            const auto colon = value.find(':');
            if (colon != std::string::npos &&
                (!namespaceAllowed || colon == 0 || colon + 1 == value.size()))
            {
                return false;
            }
            for (auto index = std::size_t{0}; index < value.size(); ++index)
            {
                const auto character = value.at(index);
                if ((character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9') || character == '_' ||
                    character == '.' || character == '-' ||
                    (character == '/' && (colon == std::string::npos || index > colon)) ||
                    (character == ':' && index == colon))
                {
                    continue;
                }
                return false;
            }
            return true;
        }

        bool positiveFloat(const Json& value)
        {
            if (!value.is_number())
            {
                return false;
            }
            const auto number = value.get<double>();
            return std::isfinite(number) && number > 0 &&
                   number <= std::numeric_limits<float>::max() && static_cast<float>(number) > 0;
        }

        bool signedInteger(const Json& value, std::int64_t minimum)
        {
            if (!value.is_number_integer())
            {
                return false;
            }
            if (value.is_number_unsigned())
            {
                const auto number = value.get<std::uint64_t>();
                return number <= static_cast<std::uint64_t>(std::numeric_limits<int32_t>::max()) &&
                       (minimum <= 0 || std::cmp_greater_equal(number, minimum));
            }
            const auto number = value.get<std::int64_t>();
            return number >= minimum && number <= std::numeric_limits<int32_t>::max();
        }

        std::unexpected<Diagnostic> soundError(ErrorCode code, std::string_view message,
                                               std::string_view pack, std::string_view path)
        {
            return std::unexpected(Diagnostic{
                .Code = code,
                .Message = std::string(message),
                .Pack = std::string(pack),
                .Path = std::string(path),
            });
        }

        Result<void> validateSoundEntry(const Json& entry, std::string_view pack,
                                        std::string_view path)
        {
            const auto invalid = [&](std::string_view message)
            { return soundError(ErrorCode::INVALID_RESOURCE, message, pack, path); };
            const auto unsupported = [&](std::string_view message)
            { return soundError(ErrorCode::UNSUPPORTED_FEATURE, message, pack, path); };
            if (entry.is_string())
            {
                if (!identifier(entry.get_ref<const std::string&>(), true))
                {
                    return invalid("Invalid sound identifier");
                }
                return {};
            }
            if (!entry.is_object() || !entry.contains("name") || !entry.at("name").is_string() ||
                !identifier(entry.at("name").get_ref<const std::string&>(), true))
            {
                return invalid("Invalid sound entry");
            }
            if (!knownKeys(entry, {
                                      "name",
                                      "type",
                                      "volume",
                                      "pitch",
                                      "weight",
                                      "stream",
                                      "preload",
                                      "attenuation_distance",
                                  }))
            {
                return unsupported("Unknown sound entry field");
            }
            if (entry.contains("type"))
            {
                if (!entry.at("type").is_string())
                {
                    return invalid("Invalid sound entry type");
                }
                if (entry.at("type") != "file" && entry.at("type") != "event")
                {
                    return unsupported("Non-vanilla sound entry type");
                }
            }
            for (const auto* key : {"volume", "pitch"})
            {
                if (entry.contains(key) && !positiveFloat(entry.at(key)))
                {
                    return invalid("Invalid positive sound parameter");
                }
            }
            if ((entry.contains("weight") && !signedInteger(entry.at("weight"), 1)) ||
                (entry.contains("attenuation_distance") &&
                 !signedInteger(entry.at("attenuation_distance"),
                                std::numeric_limits<int32_t>::min())))
            {
                return invalid("Invalid integer sound parameter");
            }
            for (const auto* key : {"stream", "preload"})
            {
                if (entry.contains(key) && !entry.at(key).is_boolean())
                {
                    return invalid("Invalid boolean sound parameter");
                }
            }
            return {};
        }

        Result<void> validateSoundEvent(const std::string& name, const Json& event,
                                        std::string_view pack, std::string_view path)
        {
            if (!identifier(name, false) || !event.is_object() ||
                (event.contains("replace") && !event.at("replace").is_boolean()) ||
                (event.contains("subtitle") && !event.at("subtitle").is_string()) ||
                (event.contains("sounds") && !event.at("sounds").is_array()))
            {
                return soundError(ErrorCode::INVALID_RESOURCE, "Invalid sound event: " + name, pack,
                                  path);
            }
            if (!knownKeys(event, {"replace", "subtitle", "sounds"}))
            {
                return soundError(ErrorCode::UNSUPPORTED_FEATURE, "Unknown sound event field", pack,
                                  path);
            }
            if (event.contains("sounds"))
            {
                for (const auto& entry : event.at("sounds"))
                {
                    if (auto valid = validateSoundEntry(entry, pack, path); !valid)
                    {
                        return valid;
                    }
                }
            }
            return {};
        }
    }

    Result<void> ValidateSounds(const Json& sounds, std::string_view pack, std::string_view path)
    {
        if (!sounds.is_object())
        {
            return soundError(ErrorCode::INVALID_RESOURCE, "sounds.json must be an object", pack,
                              path);
        }
        for (const auto& [name, event] : sounds.items())
        {
            if (auto valid = validateSoundEvent(name, event, pack, path); !valid)
            {
                return valid;
            }
        }
        return {};
    }

    Json MergeSounds(Json lower, Json higher)
    {
        for (const auto& [name, event] : higher.items())
        {
            const auto existing = lower.find(name);
            if (existing == lower.end() || event.value("replace", false))
            {
                if (!event.contains("sounds"))
                {
                    event.emplace("sounds", Json::array());
                }
                if (existing == lower.end())
                {
                    lower.emplace(name, std::move(event));
                }
                else
                {
                    existing.value() = std::move(event);
                }
            }
            else if (event.contains("sounds"))
            {
                for (auto& sound : event.at("sounds"))
                {
                    existing->at("sounds").push_back(std::move(sound));
                }
            }
        }
        return lower;
    }
}
