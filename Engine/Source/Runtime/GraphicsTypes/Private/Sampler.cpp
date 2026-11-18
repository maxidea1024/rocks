#include "EnumNames.h"

#include <GraphicsTypes/Sampler.h>

namespace rocks::graphics {

bool IsValidSamplerDesc(const SamplerDesc& desc) noexcept {
    return static_cast<uint32>(desc.filter) <= static_cast<uint32>(MaxFilter) &&
           static_cast<uint32>(desc.addressU) <= static_cast<uint32>(AddressMode::Border) &&
           static_cast<uint32>(desc.addressV) <= static_cast<uint32>(AddressMode::Border) &&
           static_cast<uint32>(desc.addressW) <= static_cast<uint32>(AddressMode::Border) &&
           static_cast<uint32>(desc.borderColor) <= static_cast<uint32>(BorderColor::OpaqueWhite) &&
           IsValidCompareFunction(desc.compare) && desc.maxAnisotropy >= 1 &&
           desc.maxAnisotropy <= 16;
}

void BorderColorValue(BorderColor color, float (&out)[4]) noexcept {
    const float value = color == BorderColor::OpaqueWhite ? 1.0f : 0.0f;
    out[0] = value;
    out[1] = value;
    out[2] = value;
    out[3] = color == BorderColor::TransparentBlack ? 0.0f : 1.0f;
}

const char* FilterName(Filter filter) noexcept {
    switch (filter) {
    case Filter::Point:
        return "Point";
    case Filter::Bilinear:
        return "Bilinear";
    case Filter::Trilinear:
        return "Trilinear";
    case Filter::AnisotropicPoint:
        return "AnisotropicPoint";
    case Filter::AnisotropicLinear:
        return "AnisotropicLinear";
    }

    return "Unknown";
}

const char* AddressModeName(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Wrap:
        return "Wrap";
    case AddressMode::Clamp:
        return "Clamp";
    case AddressMode::Mirror:
        return "Mirror";
    case AddressMode::Border:
        return "Border";
    }

    return "Unknown";
}

const char* BorderColorName(BorderColor color) noexcept {
    switch (color) {
    case BorderColor::TransparentBlack:
        return "TransparentBlack";
    case BorderColor::OpaqueBlack:
        return "OpaqueBlack";
    case BorderColor::OpaqueWhite:
        return "OpaqueWhite";
    }

    return "Unknown";
}

Result<Filter> ParseFilter(StringView name) noexcept {
    return detail::ParseByName<Filter>(name, static_cast<uint32>(MaxFilter) + 1, &FilterName);
}

Result<AddressMode> ParseAddressMode(StringView name) noexcept {
    return detail::ParseByName<AddressMode>(name, 4, &AddressModeName);
}

Result<BorderColor> ParseBorderColor(StringView name) noexcept {
    return detail::ParseByName<BorderColor>(name, 3, &BorderColorName);
}

} // namespace rocks::graphics
