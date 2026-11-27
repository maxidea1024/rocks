#pragma once

#include <Core/Result.h>
#include <GraphicsTypes/PixelFormat.h>
#include <ShaderCore/ShaderParameterTypes.h>
#include <ShaderCore/ShaderType.h>

// 엔진 셰이더 SL1 유틸리티 셰이더 타입이다(Docs/Design/EngineShaders 14.4절, RenderBase RR17).
// 원문은 /Engine/Private/Utility/{GenerateMips,ClearResources,CopyDepth,PlaceholderShaders}.rsf다.
// 등록은 EngineShaderTypes()(RenderBase/EngineShaderTypes.h)가 함께 한다. 그리기·dispatch 도우미는
// RenderBase/GenerateMips.h·RenderBase/ClearResources.h다.
//
// 그리기별 상수는 생성 파라미터 구조체(ShaderSystem 16.2절)이고, 자원은 원문 선언 + 아래 이름
// 상수다(결정 SLD27 — 도우미가 그래프 밖에서 쓰기 텍스처를 묶는다).
namespace rocks::render {

// --- storage image 형식(SPIR-V·GLES storage image 선언 형식, 퍼뮤테이션 값) ---

// 쓰기 view 형식의 storage image 형식이다(셰이더 ROCKS_IMAGE_FORMAT 철자 순서 — ClearResources.rsf
// CLEAR_FORMAT·GenerateMips.rsf MIPS_FORMAT 번호와 같다). 앞 여덟은 GLES 3.2 core image 형식이다.
enum class StorageImageFormat : uint8 {
    Rgba8,
    Rgba8Snorm,
    Rgba16f,
    Rgba32f,
    R32f,
    R32ui,
    R32i,
    Rgba32ui,
    Rg16f,
    R16f,
    Rg32f,
    R11g11b10f
};

inline constexpr uint32 StorageImageFormatCount = 12;
// GLES 3.2 core image 형식 수(앞에서부터)다.
inline constexpr uint32 GlesStorageImageFormatCount = 8;

// 픽셀 형식의 storage image 형식이다(RGBA8Unorm → Rgba8 …). 대응이 없는 형식(sRGB·BGRA·8·16 bit
// 정수· 깊이·압축 등)은 Unsupported다.
ROCKS_RENDERBASE_API Result<StorageImageFormat>
StorageImageFormatOf(graphics::PixelFormat format) noexcept;

// 정수 값 계열(R32ui·R32i·Rgba32ui)이 아닌가.
constexpr bool IsFloatStorageImageFormat(StorageImageFormat format) noexcept {
    return format != StorageImageFormat::R32ui && format != StorageImageFormat::R32i &&
           format != StorageImageFormat::Rgba32ui;
}

// --- 밉 생성(GenerateMips.rsf) ---

// compute 밉 생성 CS다. 퍼뮤테이션: MIPS_DIMENSION(GenerateMipsComputeDimension) × MIPS_FORMAT
// (StorageImageFormat — float 계열만). 텍스처 이름은 MipSourceName(Read view, 원본 mip)·
// MipTargetName(쓰기 view, 대상 mip)이다.
ROCKS_SHADER_TYPE_NAME(GenerateMipsComputeType, "GenerateMipsCompute");

// raster 밉 생성 PS다(FullscreenTriangleVertex와 짝). 퍼뮤테이션: MIPS_SOURCE_DIMENSION
// (GenerateMipsPixelDimension). cube는 point 샘플러로 표본한다(MipSourceName의 자기 샘플러).
ROCKS_SHADER_TYPE_NAME(GenerateMipsPixelType, "GenerateMipsPixel");

inline constexpr char MipSourceName[] = "MipSource";
inline constexpr char MipTargetName[] = "MipTarget";

// GenerateMipsCompute의 차원 퍼뮤테이션 값이다.
enum class GenerateMipsComputeDimension : uint8 {
    Texture2D,
    Texture2DArray,
    Texture3D
};

inline constexpr uint32 GenerateMipsComputeDimensionCount = 3;

// GenerateMipsPixel의 원본 차원 퍼뮤테이션 값이다.
enum class GenerateMipsPixelDimension : uint8 {
    Texture2D,
    Texture2DArray,
    TextureCube,
    Texture3D
};

inline constexpr uint32 GenerateMipsPixelDimensionCount = 4;

// 퍼뮤테이션 ID다(첫 차원이 가장 낮은 자리 — ShaderPermutation.h).
constexpr uint32 GenerateMipsComputePermutation(GenerateMipsComputeDimension dimension,
                                                StorageImageFormat format) noexcept {
    return static_cast<uint32>(dimension) +
           GenerateMipsComputeDimensionCount * static_cast<uint32>(format);
}

constexpr uint32 GenerateMipsPixelPermutation(GenerateMipsPixelDimension dimension) noexcept {
    return static_cast<uint32>(dimension);
}

// GenerateMips*의 그리기별 상수다(32 byte, 생성 HLSL
// /Engine/Generated/RenderBase/GenerateMipsConstants.rsh).
//
// 사용 예(mip k → k + 1, 2D):
//   render::GenerateMipsConstants constants;
//   constants.source = {MipExtent(w, k), MipExtent(h, k), 1, baseMip};
//   constants.destination = {MipExtent(w, k + 1), MipExtent(h, k + 1), 1, 0};
ROCKS_SHADER_PARAMETERS(Name = "GenerateMips", RootConstants)
struct alignas(16) GenerateMipsConstants {
    // 원본 mip 폭·높이·깊이(3D 밖은 1)와 원본 view 기준 mip(TextureViewBaseMip)이다.
    shader::ShaderUint4 source;
    // 대상 mip 폭·높이·깊이와 raster의 대상 slice(배열 층·cube 면·3D 깊이 면)다.
    shader::ShaderUint4 destination;
};

// --- 자원 clear(ClearResources.rsf) ---

// UAV clear CS다. 퍼뮤테이션: CLEAR_DIMENSION(ClearDimension) × CLEAR_FORMAT(StorageImageFormat).
// 쓰기 자원 이름은 ClearTargetName이다.
ROCKS_SHADER_TYPE_NAME(ClearWritableComputeType, "ClearWritableCompute");

// 부분 clear VS(상수 깊이 z)·PS(CLEAR_VALUE_TYPE — ClearValueType)다.
ROCKS_SHADER_TYPE_NAME(ClearRegionVertexType, "ClearRegionVertex");
ROCKS_SHADER_TYPE_NAME(ClearRegionPixelType, "ClearRegionPixel");

// 간접 인자 초기화 CS다. 쓰기 버퍼(raw) 이름은 DispatchArgumentsTargetName이다.
ROCKS_SHADER_TYPE_NAME(InitializeDispatchArgumentsComputeType,
                       "InitializeDispatchArgumentsCompute");

inline constexpr char ClearTargetName[] = "ClearTarget";
inline constexpr char DispatchArgumentsTargetName[] = "Arguments";

// UAV clear의 자원 차원 퍼뮤테이션 값이다.
enum class ClearDimension : uint8 {
    Buffer,
    Texture2D,
    Texture2DArray,
    Texture3D
};

inline constexpr uint32 ClearDimensionCount = 4;

// 부분 clear PS의 값 계열 퍼뮤테이션 값이다(색 대상 형식의 PixelShaderType).
enum class ClearValueType : uint8 {
    Float,
    Uint,
    Sint
};

inline constexpr uint32 ClearValueTypeCount = 3;

// UAV clear의 그룹 크기다(셰이더와 같다).
inline constexpr uint32 ClearBufferGroupThreads = 64;
inline constexpr uint32 ClearTexture2DGroupSide = 8;
inline constexpr uint32 ClearVolumeGroupSide = 4;
// 간접 인자 초기화의 그룹 크기다.
inline constexpr uint32 DispatchArgumentsInitGroupThreads = 64;

constexpr uint32 ClearWritablePermutation(ClearDimension dimension,
                                          StorageImageFormat format) noexcept {
    return static_cast<uint32>(dimension) + ClearDimensionCount * static_cast<uint32>(format);
}

constexpr uint32 ClearRegionPermutation(ClearValueType type) noexcept {
    return static_cast<uint32>(type);
}

// ClearWritableCompute의 그리기별 상수다(48 byte).
ROCKS_SHADER_PARAMETERS(Name = "ClearWritable", RootConstants)
struct alignas(16) ClearWritableConstants {
    // 값 bit다(float 형식은 float bit, 정수 형식은 정수).
    shader::ShaderUint4 value;
    // 범위 [minBounds, maxBounds)다(버퍼는 x 원소, 텍스처는 x·y texel·z 층·깊이).
    shader::ShaderUint4 minBounds;
    shader::ShaderUint4 maxBounds;
};

// ClearRegion*의 그리기별 상수다(32 byte).
ROCKS_SHADER_PARAMETERS(Name = "ClearRegion", RootConstants)
struct alignas(16) ClearRegionConstants {
    // 값 bit다(색 대상 형식의 값 계열로 읽는다).
    shader::ShaderUint4 value;
    // x: 깊이(reversed-Z 원평면 0)다.
    shader::ShaderFloat4 depth;
};

// InitializeDispatchArgumentsCompute의 그리기별 상수다(32 byte).
ROCKS_SHADER_PARAMETERS(Name = "DispatchArgumentsInit", RootConstants)
struct alignas(16) DispatchArgumentsInitConstants {
    // xyz: 쓸 그룹 수(DispatchIndirectArguments)다.
    shader::ShaderUint4 arguments;
    // x: 첫 기록 byte 위치(4 byte 배수), y: 기록 수다.
    shader::ShaderUint4 range;
};

// --- 깊이 복사(CopyDepth.rsf) ---

// 깊이 → 색 대상 (d, 0, 0, 1)·깊이 → 깊이 대상(SV_Depth) PS다(FullscreenTriangleVertex와 짝).
// 텍스처 이름은 CopyDepthSourceName이다.
ROCKS_SHADER_TYPE_NAME(CopyDepthToColorPixelType, "CopyDepthToColorPixel");
ROCKS_SHADER_TYPE_NAME(CopyDepthPixelType, "CopyDepthPixel");

inline constexpr char CopyDepthSourceName[] = "SourceDepth";

// --- 자리표시(PlaceholderShaders.rsf) ---

// 단색 PS(SolidColorConstants)·출력 없는 깊이 전용 PS·8 px 자홍·검정 바둑판 PS다.
ROCKS_SHADER_TYPE_NAME(SolidColorPixelType, "SolidColorPixel");
ROCKS_SHADER_TYPE_NAME(DepthOnlyPixelType, "DepthOnlyPixel");
ROCKS_SHADER_TYPE_NAME(MissingShaderPixelType, "MissingShaderPixel");

// MissingShaderPixel 바둑판의 칸 한 변(px)과 두 색(선형 RGBA)이다. 칸 (x/8 ^ y/8)이 짝수면 첫
// 색이다.
inline constexpr uint32 MissingShaderCellPixels = 8;
inline constexpr float MissingShaderFirstColor[4] = {1.0f, 0.0f, 1.0f, 1.0f};
inline constexpr float MissingShaderSecondColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

// SolidColorPixel의 그리기별 상수다(16 byte).
ROCKS_SHADER_PARAMETERS(Name = "SolidColor", RootConstants)
struct alignas(16) SolidColorConstants {
    shader::ShaderFloat4 color;
};

} // namespace rocks::render
