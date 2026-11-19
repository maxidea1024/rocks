#include "EnumNames.h"

#include <GraphicsTypes/ChannelSwizzle.h>
#include <GraphicsTypes/CompareFunction.h>
#include <GraphicsTypes/TextureDimension.h>

namespace rocks::graphics {

const char* CompareFunctionName(CompareFunction function) noexcept {
    switch (function) {
    case CompareFunction::Never:
        return "Never";
    case CompareFunction::Less:
        return "Less";
    case CompareFunction::Equal:
        return "Equal";
    case CompareFunction::LessEqual:
        return "LessEqual";
    case CompareFunction::Greater:
        return "Greater";
    case CompareFunction::NotEqual:
        return "NotEqual";
    case CompareFunction::GreaterEqual:
        return "GreaterEqual";
    case CompareFunction::Always:
        return "Always";
    }

    return "Unknown";
}

Result<CompareFunction> ParseCompareFunction(StringView name) noexcept {
    return detail::ParseByName<CompareFunction>(name, static_cast<uint32>(MaxCompareFunction) + 1,
                                                &CompareFunctionName);
}

const char* TextureDimensionName(TextureDimension dimension) noexcept {
    switch (dimension) {
    case TextureDimension::Texture2D:
        return "Texture2D";
    case TextureDimension::Texture2DArray:
        return "Texture2DArray";
    case TextureDimension::Texture3D:
        return "Texture3D";
    case TextureDimension::TextureCube:
        return "TextureCube";
    case TextureDimension::TextureCubeArray:
        return "TextureCubeArray";
    }

    return "Unknown";
}

Result<TextureDimension> ParseTextureDimension(StringView name) noexcept {
    return detail::ParseByName<TextureDimension>(name, static_cast<uint32>(MaxTextureDimension) + 1,
                                                 &TextureDimensionName);
}

const char* SwizzleSourceName(SwizzleSource source) noexcept {
    switch (source) {
    case SwizzleSource::Red:
        return "Red";
    case SwizzleSource::Green:
        return "Green";
    case SwizzleSource::Blue:
        return "Blue";
    case SwizzleSource::Alpha:
        return "Alpha";
    case SwizzleSource::Zero:
        return "Zero";
    case SwizzleSource::One:
        return "One";
    }

    return "Unknown";
}

Result<SwizzleSource> ParseSwizzleSource(StringView name) noexcept {
    return detail::ParseByName<SwizzleSource>(name, static_cast<uint32>(MaxSwizzleSource) + 1,
                                              &SwizzleSourceName);
}

} // namespace rocks::graphics
