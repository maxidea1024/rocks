#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Texture/TextureEnums.h>

// 텍스처 빌드의 작업 픽셀과 공용 단계다(Docs/Design/AssetSystem 12.13절·12.14절 A5j2b). 빌드
// (TextureBuild.cpp)와 원본 변환(TextureSourceConvert.cpp)이 같은 풀기·재표본·양자화를 쓴다(두 번
// 구현하지 않는다).
namespace rocks::texture_build_detail {

inline constexpr usize Channels = 4;
inline constexpr usize Red = 0;
inline constexpr usize Green = 1;
inline constexpr usize Blue = 2;
inline constexpr usize Alpha = 3;

// 한 mip 단계의 작업 픽셀이다: layers(2DArray·Cube 면 — Volume은 1) × depth(Volume 깊이 — 그 밖 1)
// × height × width × RGBA float32.
struct Level {
    explicit Level(Allocator& allocator) noexcept : texels(allocator) {
    }

    uint32 width = 0;
    uint32 height = 0;
    uint32 depth = 1;
    uint32 layers = 1;
    Array<float> texels;

    usize PlaneTexels() const noexcept {
        return usize{width} * height;
    }

    float* At(uint32 layer, uint32 z, uint32 y, uint32 x) noexcept {
        return texels.Data() + ((((usize{layer} * depth + z) * height + y) * width + x) * Channels);
    }

    const float* At(uint32 layer, uint32 z, uint32 y, uint32 x) const noexcept {
        return texels.Data() + ((((usize{layer} * depth + z) * height + y) * width + x) * Channels);
    }

    Result<void> Allocate(uint32 w, uint32 h, uint32 d, uint32 l) noexcept {
        width = w;
        height = h;
        depth = d;
        layers = l;
        return texels.TryResize(usize{w} * h * d * l * Channels);
    }
};

// 원본 색 채널의 감마다(UE EGammaSpace — 8비트 정수 원본에만 뜻이 있다).
enum class TexelGamma : uint8 {
    Linear,
    Srgb,
    Pow22,
};

// 원본 byte를 작업 픽셀로 푼다(UE CopyImage 원본 → RGBA32F): 회색(G8·GA8·G16·GA16)은 (v, v, v, a),
// R16F·R32F는 (v, 0, 0, 1), 알파 없으면 1, 8비트 알파는 선형, 16비트·float는 gamma와 무관하게
// 선형이다. out은 width × height × depth × layers다. 오류: byte 수가 맞지 않음 InvalidArgument,
// 할당 실패.
Result<void> DecodeTexels(TextureSourceFormat format, TexelGamma gamma, uint32 width, uint32 height,
                          uint32 depth, uint32 layers, Span<const uint8> pixels,
                          Level& out) noexcept;

// level의 각 평면을 (targetX, targetY)로 재표본한다(UE 기본 필터 AdaptiveSharp — 12.13절). 오류:
// 할당 실패.
Result<void> ResampleLevel(Allocator& allocator, Level& level, uint32 targetX,
                           uint32 targetY) noexcept;

// UE FColor::QuantizeUNormFloatTo8·16이다((uint)(0.5f + Clamp01NansTo0(v) × 최대)).
uint8 QuantizeUnorm8Value(float value) noexcept;
uint16 QuantizeUnorm16Value(float value) noexcept;

// 작업 픽셀 byte 수가 빌드 상한(TextureBuildMaxWorkingBytes) 안인지다.
bool WithinTextureWorkingBytes(uint64 x, uint64 y, uint64 z, uint64 layers) noexcept;

} // namespace rocks::texture_build_detail
