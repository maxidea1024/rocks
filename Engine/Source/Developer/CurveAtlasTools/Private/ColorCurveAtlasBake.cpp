#include <Core/Math/Float16.h>
#include <CurveAtlas/CurveAtlasConstants.h>
#include <CurveAtlasTools/ColorCurveAtlasBake.h>

// 색 커브 atlas 굽기다(Docs/Design/CurveAssets 4.3절).
namespace rocks {

namespace {

// null 커브 행의 색이다(UE FLinearColor::White).
constexpr LinearColor NullCurveColor{1.0f, 1.0f, 1.0f, 1.0f};

void WriteHalf(uint8* at, float value) noexcept {
    const uint16 bits = Float16::FromFloat(value).bits;
    at[0] = static_cast<uint8>(bits & 0xffu);
    at[1] = static_cast<uint8>(bits >> 8);
}

void WriteTexel(uint8* at, const LinearColor& color) noexcept {
    WriteHalf(at, color.r);
    WriteHalf(at + 2, color.g);
    WriteHalf(at + 4, color.b);
    WriteHalf(at + 6, color.a);
}

} // namespace

Result<void> BakeColorCurveAtlas(Span<const ColorCurveAsset* const> curves, uint32 width,
                                 bool disableAdjustments, ColorCurveAtlasPixels& out) noexcept {
    const uint32 sizeX = width < MinColorCurveAtlasWidth ? MinColorCurveAtlasWidth : width;
    const usize rows = curves.IsEmpty() ? 1 : curves.Size();
    if (sizeX > MaxColorCurveAtlasDimension || rows > MaxColorCurveAtlasDimension) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    const uint32 sizeY = static_cast<uint32>(rows);
    Array<uint8> bytes(out.bytes.GetAllocator());
    const usize rowBytes = static_cast<usize>(sizeX) * ColorCurveAtlasTexelBytes;
    if (auto sized = bytes.TryResize(rowBytes * sizeY); !sized) {
        return sized;
    }

    // UE PushToSourceData: x * (1.f / (Width - 1)).
    const float step = 1.0f / static_cast<float>(sizeX - 1);
    for (uint32 row = 0; row < sizeY; ++row) {
        const ColorCurveAsset* curve = row < curves.Size() ? curves[row] : nullptr;
        uint8* cursor = bytes.Data() + rowBytes * row;
        for (uint32 x = 0; x < sizeX; ++x) {
            const float time = static_cast<float>(x) * step;
            const LinearColor color = curve == nullptr     ? NullCurveColor
                                      : disableAdjustments ? curve->EvaluateUnadjusted(time)
                                                           : curve->Evaluate(time);
            WriteTexel(cursor + static_cast<usize>(x) * ColorCurveAtlasTexelBytes, color);
        }
    }

    out.sizeX = sizeX;
    out.sizeY = sizeY;
    out.bytes = Move(bytes);
    return Result<void>::Success();
}

} // namespace rocks
