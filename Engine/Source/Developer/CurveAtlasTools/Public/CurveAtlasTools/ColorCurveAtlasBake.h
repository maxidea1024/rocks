#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <CurveAssets/ColorCurveAsset.h>

// 색 커브 atlas 굽기다(Docs/Design/CurveAssets 4.3절 — UE UCurveLinearColorAtlas::UpdateTextures,
// UCurveLinearColor::PushToSourceData).
namespace rocks {

// 구운 원본 픽셀이다: RGBA16F(성분 r g b a, 각 IEEE binary16 little-endian), 행 우선, 행 i = 커브
// i. bytes는 sizeX × sizeY × 8 byte다.
struct ColorCurveAtlasPixels {
    explicit ColorCurveAtlasPixels(Allocator& allocator) noexcept : bytes(allocator) {
    }

    uint32 sizeX = 0;
    uint32 sizeY = 0;
    Array<uint8> bytes;
};

// 한 texel의 byte 수다(RGBA16F).
inline constexpr uint32 ColorCurveAtlasTexelBytes = 8;

// curves(null 원소는 흰 행)를 굽는다. 너비 W = max(width, MinColorCurveAtlasWidth), 높이 =
// max(커브 수, 1)이다(커브가 없으면 흰 한 줄). texel x는 시간 float(x) × (1 / float(W − 1))의
// 색이다(UE 식 그대로 — 역수 곱). 색은 결정적 색 평가(CurveAssets 2.4.5절 — 호스트와 무관한
// 비트)로 disableAdjustments면 EvaluateUnadjustedDeterministic, 아니면 EvaluateDeterministic(조정
// 적용 — UE GetLinearColorValue 대응)이고 float16은 Float16::FromFloat(round-to-nearest-even, UE
// FFloat16 Set과 같다 — NaN은 고정 quiet NaN이라 half 0x7E00)이다.
//
// 오류: W 또는 높이가 MaxColorCurveAtlasDimension 초과 CapacityExceeded, 할당 실패 OutOfMemory.
// 실패하면 out은 그대로다. 성공하면 out.bytes는 out의 allocator로 다시 만든다.
//
// 사용 예:
//   ColorCurveAtlasPixels pixels(allocator);
//   if (auto baked = BakeColorCurveAtlas(curves.View(), atlas.width, false, pixels); !baked) { ...
//   }
ROCKS_CURVEATLASTOOLS_API Result<void>
BakeColorCurveAtlas(Span<const ColorCurveAsset* const> curves, uint32 width,
                    bool disableAdjustments, ColorCurveAtlasPixels& out) noexcept;

} // namespace rocks
