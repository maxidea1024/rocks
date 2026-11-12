#include <GraphicsTypes/PixelFormat.h>

namespace rocks::graphics {

uint32 BytesPerPixel(Format format) noexcept {
    switch (format) {
    case Format::RGBA8Unorm:
    case Format::RGBA8UnormSrgb:
        return 4;
    case Format::Unknown:
        break;
    }

    return 0;
}

const char* FormatName(Format format) noexcept {
    switch (format) {
    case Format::Unknown:
        return "Unknown";
    case Format::RGBA8Unorm:
        return "RGBA8Unorm";
    case Format::RGBA8UnormSrgb:
        return "RGBA8UnormSrgb";
    }

    return "Unknown";
}

uint64 MipChainBytes(uint32 width, uint32 height, uint32 mipLevels, Format format) noexcept {
    const uint32 bytesPerPixel = BytesPerPixel(format);
    if (width == 0 || height == 0 || bytesPerPixel == 0 || mipLevels == 0 ||
        mipLevels > FullMipCount(width, height)) {
        return 0;
    }
    uint64 total = 0;
    for (uint32 level = 0; level < mipLevels; ++level) {
        total +=
            static_cast<uint64>(MipExtent(width, level)) * MipExtent(height, level) * bytesPerPixel;
    }

    return total;
}

} // namespace rocks::graphics
