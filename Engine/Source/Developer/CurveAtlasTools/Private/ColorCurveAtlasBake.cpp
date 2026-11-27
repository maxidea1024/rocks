#include <Core/Math/Float16.h>
#include <CurveAtlas/CurveAtlasConstants.h>
#include <CurveAtlasTools/ColorCurveAtlasBake.h>
#include <Platform/FloatingPointEnvironment.h>

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

// 행마다 커브를 굽는다(data는 sizeX × sizeY texel).
void BakeRows(Span<const ColorCurveAsset* const> curves, uint32 sizeX, uint32 sizeY,
              bool disableAdjustments, uint8* data) noexcept {
    const usize rowBytes = static_cast<usize>(sizeX) * ColorCurveAtlasTexelBytes;
    // UE PushToSourceData: x * (1.f / (Width - 1)).
    const float step = 1.0f / static_cast<float>(sizeX - 1);
    for (uint32 row = 0; row < sizeY; ++row) {
        const ColorCurveAsset* curve = row < curves.Size() ? curves[row] : nullptr;
        uint8* cursor = data + rowBytes * row;
        for (uint32 x = 0; x < sizeX; ++x) {
            const float time = static_cast<float>(x) * step;
            // 결정적 색 평가다(설계 4.3절, CAD12 — 원본 byte가 호스트와 무관하다).
            const LinearColor color = curve == nullptr ? NullCurveColor
                                      : disableAdjustments
                                          ? curve->EvaluateUnadjustedDeterministic(time)
                                          : curve->EvaluateDeterministic(time);
            WriteTexel(cursor + static_cast<usize>(x) * ColorCurveAtlasTexelBytes, color);
        }
    }
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

    // 굽기는 기본 부동소수점 환경에서 돈다(결정적 색 평가의 전제 — CurveData 5.9절).
    struct Job {
        Span<const ColorCurveAsset* const> curves;
        uint32 sizeX;
        uint32 sizeY;
        bool disableAdjustments;
        uint8* data;
    };
    Job job{curves, sizeX, sizeY, disableAdjustments, bytes.Data()};
    platform::RunWithDeterministicFloatingPoint(
        [](void* context) noexcept {
            const Job& run = *static_cast<const Job*>(context);
            BakeRows(run.curves, run.sizeX, run.sizeY, run.disableAdjustments, run.data);
        },
        &job);

    out.sizeX = sizeX;
    out.sizeY = sizeY;
    out.bytes = Move(bytes);
    return Result<void>::Success();
}

} // namespace rocks
