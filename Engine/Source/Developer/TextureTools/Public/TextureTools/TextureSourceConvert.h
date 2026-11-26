#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Texture/TextureEnums.h>

// 텍스처 원본 형식 변환·재표본이다(Docs/Design/AssetSystem 12.14절 A5j2b — UE 5.8
// FImageCoreUtils::GetCommonSourceFormat·FImageCore::CopyImage·ResizeImageAllocDest). 원본 묶음
// (2DArray·CubeArray)이 형식·감마·크기가 다른 원소를 공통 형식으로 맞출 때 쓴다.
//
// 사용 예:
//   TextureSourceFormat common = CommonTextureSourceFormat(TextureSourceFormat::G8,
//                                                          TextureSourceFormat::RGBA8);  // RGBA8
//   TextureSourceImage image{TextureSourceFormat::G8, TextureSourceGamma::Srgb, 4, 4, 1, bytes};
//   Array<uint8> out(allocator);
//   ConvertTextureSource(allocator, image, common, TextureSourceGamma::Srgb, 8, 8, out);
namespace rocks {

// 원본 색 채널의 감마다(UE EGammaSpace). 8비트 정수 원본에만 뜻이 있고, 16비트·float는 늘 Linear다.
enum class TextureSourceGamma : uint8 {
    Linear,
    Srgb,
    // legacy gamma 2.2(UE Pow22) — 원본으로만 온다(변환 대상 감마로는 받지 않는다).
    Pow22,
};

// 두 원본 형식을 손실 없이 담는 가장 낮은 공통 형식이다(UE GetCommonSourceFormat 그래프를 Rocks
// 형식으로 — 12.14절 A5j2b 1번 표). 정수·float가 만나거나 표에 없는 쌍은 RGBA32F, Invalid가 끼면
// Invalid다. 감마가 같을 때의 표다(감마가 갈린 묶음은 호출자가 RGBA32F·선형으로 둔다).
ROCKS_TEXTURETOOLS_API TextureSourceFormat
CommonTextureSourceFormat(TextureSourceFormat a, TextureSourceFormat b) noexcept;

// 변환할 원본 한 장(또는 slice 묶음)이다. bytes는 sizeX × sizeY × numSlices × texel byte다.
struct TextureSourceImage {
    TextureSourceFormat format = TextureSourceFormat::Invalid;
    TextureSourceGamma gamma = TextureSourceGamma::Linear;
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 numSlices = 1;
    Span<const uint8> bytes{};
};

// image를 (format, gamma)로 바꾸고 slice마다 (sizeX, sizeY)로 재표본해 out 끝에 붙인다(UE 순서 —
// 형식 변환 뒤 재표본, 12.14절 A5j2b 2·3번). 형식·감마가 같으면 byte 그대로, 같은 감마의 넓히는
// 변환은 직접 경로(byte 복제·v·257), 그 밖은 RGBA32F 선형을 거친 두 단계다. 재표본은 A5i Resample
// (AdaptiveSharp)이다.
//
// 오류: Invalid 형식·byte 수 불일치·크기 0·대상 감마 Pow22·8비트가 아닌 대상에 sRGB
// InvalidArgument, 작업 메모리 상한(TextureBuildMaxWorkingBytes) 초과 CapacityExceeded, 할당 실패.
// 실패하면 out 길이는 그대로가 아닐 수 있다(호출자가 버린다).
ROCKS_TEXTURETOOLS_API Result<void> ConvertTextureSource(Allocator& allocator,
                                                         const TextureSourceImage& image,
                                                         TextureSourceFormat format,
                                                         TextureSourceGamma gamma, uint32 sizeX,
                                                         uint32 sizeY, Array<uint8>& out) noexcept;

} // namespace rocks
