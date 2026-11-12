#include <GraphicsTypes/Sampler.h>
#include <Platform/Memory.h>

namespace rocks::graphics {

bool IsValidSamplerDesc(const SamplerDesc& desc) noexcept {
    return static_cast<uint32>(desc.filter) <= static_cast<uint32>(Filter::Anisotropic) &&
           static_cast<uint32>(desc.addressU) <= static_cast<uint32>(AddressMode::Border) &&
           static_cast<uint32>(desc.addressV) <= static_cast<uint32>(AddressMode::Border) &&
           static_cast<uint32>(desc.borderColor) <= static_cast<uint32>(BorderColor::OpaqueWhite) &&
           desc.maxAnisotropy >= 1 && desc.maxAnisotropy <= 16;
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
    case Filter::Nearest:
        return "Nearest";
    case Filter::Bilinear:
        return "Bilinear";
    case Filter::Trilinear:
        return "Trilinear";
    case Filter::Anisotropic:
        return "Anisotropic";
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

namespace {

bool EqualsIgnoringCase(StringView text, const char* name) noexcept {
    const usize nameBytes = platform::CStringLength(name);
    if (text.ByteCount() != nameBytes) {
        return false;
    }
    for (usize index = 0; index < nameBytes; ++index) {
        char left = text.Data()[index];
        char right = name[index];
        left = left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left;
        right = right >= 'A' && right <= 'Z' ? static_cast<char>(right - 'A' + 'a') : right;
        if (left != right) {
            return false;
        }
    }

    return true;
}

template <class Enum>
Result<Enum> ParseByName(StringView name, uint32 count, const char* (*nameOf)(Enum)) noexcept {
    for (uint32 value = 0; value < count; ++value) {
        if (EqualsIgnoringCase(name, nameOf(static_cast<Enum>(value)))) {
            return Result<Enum>::Success(static_cast<Enum>(value));
        }
    }

    return Result<Enum>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

Result<Filter> ParseFilter(StringView name) noexcept {
    return ParseByName<Filter>(name, 4, &FilterName);
}

Result<AddressMode> ParseAddressMode(StringView name) noexcept {
    return ParseByName<AddressMode>(name, 4, &AddressModeName);
}

Result<BorderColor> ParseBorderColor(StringView name) noexcept {
    return ParseByName<BorderColor>(name, 3, &BorderColorName);
}

} // namespace rocks::graphics
