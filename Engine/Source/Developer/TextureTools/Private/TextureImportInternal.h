#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <ImageCodec/ImageDecode.h>
#include <Texture/TextureImportOptions.h>

// TextureTools 내부 이미지 처리다(Docs/Design/AssetSystem 12.12절 — 회색 자동 감지, PNG infill,
// 한 파일 Cube 배치·Volume 타일 나누기, TGA 머리 판정). 시험도 쓴다(내부 헤더).
namespace rocks::texture_import {

// UE TPNGDataFill의 이웃 반지름이다(NearNeighborRadius).
inline constexpr int64 InfillRadius = 4;

// 회색 자동 감지 대상인지다(UE AutoDetectAndChangeGrayScale — 12.12절 기본값 2번): RGBA8은 모든
// texel이 A = 255이고 R = G = B, GA8은 모든 texel이 A = 255. 그 밖 형식은 거짓이다.
bool IsGrayConvertible(const image::DecodedImage& image) noexcept;

// IsGrayConvertible인 image를 G8로 바꾼다(제자리 — 회색 값만 남긴다). 오류: 없음(줄이기만 한다).
void ConvertToG8(image::DecodedImage& image) noexcept;

// PNG infill(UE FillZeroAlphaPNGData — 12.12절 기본값 3번)을 image에 한다(RGBA8·RGBA16·GA8·GA16만 —
// 그 밖 형식과 Never는 그대로). scratch는 행 하나의 작업 메모리다. 오류: 할당 실패.
ROCKS_TEXTURETOOLS_API Result<void> FillTransparentTexels(image::DecodedImage& image,
                                                          TextureImportPngInfill mode,
                                                          Allocator& scratch) noexcept;

// TGA 머리의 8비트 색표(형식 1 — color map type 1, image type 1, 8 bpp)·회색(형식 3 — color map
// type 0, image type 3, 8 bpp)인지다(UE EditorFactories.cpp 3050–3083과 같은 판정).
bool IsTgaColorMapped8(Span<const uint8> file) noexcept;
bool IsTgaGray8(Span<const uint8> file) noexcept;

// 2D 그림(image — width × height, slice 하나)에서 Cube 면 6개를 배치대로 떼어 out에 잇는다(면 순서
// +X −X +Y −Y +Z −Z, VerticalCross −Z는 180° 되돌린다). 칸이 나뉘지 않거나 정사각이 아니면 false.
// face는 칸 한 변이다. 오류: 할당 실패.
ROCKS_TEXTURETOOLS_API Result<bool> ExtractCubeLayout(const image::DecodedImage& image,
                                                      TextureCubeLayout layout, Array<uint8>& out,
                                                      uint32& face) noexcept;

// 2D 그림에서 타일(tileX × tileY)을 왼쪽 → 오른쪽, 위 → 아래 순으로 떼어 out에 잇는다(UE
// UVolumeTexture::UpdateSourceFromSourceTexture 순서). 크기가 나뉘지 않거나 slice 수가 maxSlices를
// 넘으면 false. 오류: 할당 실패.
ROCKS_TEXTURETOOLS_API Result<bool> ExtractVolumeTiles(const image::DecodedImage& image,
                                                       uint32 tileX, uint32 tileY, uint32 maxSlices,
                                                       Array<uint8>& out, uint32& slices) noexcept;

// Texture2DArray·TextureCubeArray·VolumeTexture의 원본 재구성 hook을
// 등록한다(Docs/Design/AssetSystem 12.14절 A5j2 — RegisterTextureTypeHooks가 부른다). 오류: 표
// 등록의 오류, 할당 실패.
Result<void> RegisterTextureSourceRebuildHooks(AssetTypeHookTable& table,
                                               Array<AssetTypeHookRegistration>& tokens) noexcept;

// 8비트 선형 float → sRGB 8비트다(UE FLinearColor::ToFColorSRGB — stb_image_resize 표 변환,
// 시험용).
ROCKS_TEXTURETOOLS_API uint8 LinearToSrgb8(float value) noexcept;

} // namespace rocks::texture_import
