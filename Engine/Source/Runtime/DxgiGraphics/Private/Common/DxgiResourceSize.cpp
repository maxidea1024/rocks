#include <DxgiGraphics/DxgiAdapter.h>

namespace rocks::graphics::dxgi {

namespace {

// D3D1x_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A·B·C_TERM이다(d3d11.h·d3d12.h, 두 판의 값이
// 같다).
constexpr uint64 MinimumMiB = 128;
constexpr double VideoMemoryFraction = 0.25;
constexpr uint64 MaximumMiB = 2048;

} // namespace

uint64 GuaranteedResourceBytes(uint64 dedicatedVideoMemory) noexcept {
    const uint64 videoMiB = dedicatedVideoMemory >> 20;
    uint64 mib = static_cast<uint64>(static_cast<double>(videoMiB) * VideoMemoryFraction);
    mib = mib < MinimumMiB ? MinimumMiB : mib;
    mib = mib > MaximumMiB ? MaximumMiB : mib;
    return mib << 20;
}

} // namespace rocks::graphics::dxgi
