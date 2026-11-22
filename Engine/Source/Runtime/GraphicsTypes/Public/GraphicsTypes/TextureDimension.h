#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 텍스처 차원과 cube 면 순서다(AssetSystem A0-G). 쿠킹 데이터(TexturePlatformData)가 장치 없이 담는
// 값이라 픽셀 형식·샘플러와 함께 GraphicsTypes에 둔다. 열거값은 에셋 파일에 저장되므로 정한 뒤에는
// 바꾸지 않고 추가만 한다.
namespace rocks::graphics {

// 텍스처 차원이다(UE ETextureDimension과 같은 순서, RHI/Public/RHIDefinitions.h).
// - Texture2D: 2D 하나.
// - Texture2DArray: 같은 크기 2D의 배열(층 수 TextureDesc::arraySize).
// - Texture3D: 부피 텍스처(깊이 TextureDesc::depth). mip마다 깊이도 절반으로 준다.
// - TextureCube: 정사각 2D 6면(CubeFace 순서).
// - TextureCubeArray: cube의 배열(cube 수 TextureDesc::arraySize, 면은 cube마다 6개).
enum class TextureDimension : uint8 {
    Texture2D = 0,
    Texture2DArray = 1,
    Texture3D = 2,
    TextureCube = 3,
    TextureCubeArray = 4
};

// 마지막 열거값이다(범위 검사용).
inline constexpr TextureDimension MaxTextureDimension = TextureDimension::TextureCubeArray;

// cube 면 하나의 순서다. UE ECubeFace(RHIDefinitions.h)·D3D TEXTURECUBE array slice·Vulkan cube
// layer·OpenGL ES TEXTURE_CUBE_MAP_POSITIVE_X + n이 모두 같은 순서(+X, −X, +Y, −Y, +Z, −Z)다. 면
// 방향은 D3D 규약(왼손 좌표, cube 중심에서 그 면을 볼 때 u가 오른쪽·v가 아래쪽: +X 면은 u = −Z, v =
// −Y)이며 UE가 그대로 쓴다. 그래서 cube 데이터는 backend마다 바꾸지 않고 같은 바이트를
// 올린다(OpenGL ES의 cube 면 정의도 같은 축 대응이다 — ES 3.2 8.13절 표 8.19).
enum class CubeFace : uint8 {
    PositiveX = 0,
    NegativeX = 1,
    PositiveY = 2,
    NegativeY = 3,
    PositiveZ = 4,
    NegativeZ = 5
};

// cube 하나의 면 수다.
inline constexpr uint32 CubeFaceCount = 6;

constexpr bool IsValidTextureDimension(TextureDimension dimension) noexcept {
    return static_cast<uint32>(dimension) <= static_cast<uint32>(MaxTextureDimension);
}

// Cube·CubeArray인지다.
constexpr bool IsCubeDimension(TextureDimension dimension) noexcept {
    return dimension == TextureDimension::TextureCube ||
           dimension == TextureDimension::TextureCubeArray;
}

// 층 수(arraySize)를 받는 배열 차원(2DArray·CubeArray)인지다.
constexpr bool IsArrayDimension(TextureDimension dimension) noexcept {
    return dimension == TextureDimension::Texture2DArray ||
           dimension == TextureDimension::TextureCubeArray;
}

// 진단·도구용 이름이다(열거자 이름, 범위 밖은 "Unknown"). Parse는 대소문자를 무시하며 모르는
// 이름은 InvalidArgument다.
ROCKS_GRAPHICSTYPES_API const char* TextureDimensionName(TextureDimension dimension) noexcept;
ROCKS_GRAPHICSTYPES_API Result<TextureDimension> ParseTextureDimension(StringView name) noexcept;

} // namespace rocks::graphics
