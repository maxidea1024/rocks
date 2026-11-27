#include <RenderBase/EngineShaderTypes.h>

#include <RenderBase/BuiltinResources.shaderparams.h>
#include <RenderBase/EngineShaderTypes.shaderparams.h>
#include <RenderBase/UtilityShaderTypes.h>
#include <RenderBase/UtilityShaderTypes.shaderparams.h>
#include <ShaderCore/ShaderFormatFacts.h>

// 엔진 셰이더 타입 desc다(Docs/Design/RenderBase 11.4절 2항). 원문 경로는 Engine/Shaders 아래다.
namespace rocks::render {

namespace {

constexpr shader::ShaderTypeDesc FullscreenTriangleVertexDesc = shader::MakeShaderTypeDesc(
    FullscreenTriangleVertexType, "/Engine/Private/Utility/FullscreenTriangle.rsf",
    "FullscreenTriangleVertex", shader::ShaderStageVertex, shader::ShaderTypeOwner::Engine);

constexpr shader::ShaderTypeDesc CopyTexturePixelDesc = shader::MakeShaderTypeDesc(
    CopyTexturePixelType, "/Engine/Private/Utility/CopyTexture.rsf", "CopyTexturePixel",
    shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);

// BuiltinMeshVertex의 정점 입력 타입 거르기다(BuiltinVertexInput만).
constexpr bool BuiltinVertexInputOnly(shader::VertexInputTypeId vertexInputType, uint32,
                                      const shader::ShaderFormatFacts&) noexcept {
    return vertexInputType == BuiltinVertexInputName.hash;
}

// BuiltinMeshConstants의 생성 표다(ShaderSystem 16.2절 — C++ 구조체는
// RenderBase/BuiltinResources.h).
constexpr shader::ShaderParameterLayout BuiltinMeshLayouts[] = {
    shader::ShaderParameterLayoutOf<BuiltinMeshConstants>()};

constexpr shader::ShaderTypeDesc MakeBuiltinMeshVertex() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        BuiltinMeshVertexType, "/Engine/Private/Utility/BuiltinMesh.rsf", "BuiltinMeshVertex",
        shader::ShaderStageVertex, shader::ShaderTypeOwner::Engine);
    desc.flags = shader::ShaderTypeVertexInputAxis;
    desc.shouldCompile = &BuiltinVertexInputOnly;
    desc.parameterLayouts = BuiltinMeshLayouts;
    return desc;
}

constexpr shader::ShaderTypeDesc MakeBuiltinMeshPixel() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        BuiltinMeshPixelType, "/Engine/Private/Utility/BuiltinMesh.rsf", "BuiltinMeshPixel",
        shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);
    // 원문이 두 단계 모두 생성 원문을 include한다(PS는 Draw를 읽지 않아 대조할 버퍼가 없다).
    desc.parameterLayouts = BuiltinMeshLayouts;
    return desc;
}

// TonemapConstants의 생성 표다(C++ 구조체는 RenderBase/EngineShaderTypes.h).
constexpr shader::ShaderParameterLayout TonemapLayouts[] = {
    shader::ShaderParameterLayoutOf<TonemapConstants>()};

constexpr shader::ShaderTypeDesc MakeTonemapPixel() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        TonemapPixelType, "/Engine/Private/PostProcess/TonemapPass.rsf", "TonemapPixel",
        shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);
    desc.parameterLayouts = TonemapLayouts;
    return desc;
}

// TextureInspectorConstants의 생성 표다.
constexpr shader::ShaderParameterLayout TextureInspectorLayouts[] = {
    shader::ShaderParameterLayoutOf<TextureInspectorConstants>()};

constexpr shader::ShaderTypeDesc MakeTextureInspector(shader::ShaderTypeName name,
                                                      StringView entry) noexcept {
    shader::ShaderTypeDesc desc =
        shader::MakeShaderTypeDesc(name, "/Engine/Private/Debug/TextureInspector.rsf", entry,
                                   shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);
    desc.parameterLayouts = TextureInspectorLayouts;
    return desc;
}

constexpr shader::ShaderTypeDesc TextureInspector2DDesc =
    MakeTextureInspector(TextureInspector2DPixelType, "TextureInspector2DPixel");
constexpr shader::ShaderTypeDesc TextureInspector2DArrayDesc =
    MakeTextureInspector(TextureInspector2DArrayPixelType, "TextureInspector2DArrayPixel");
constexpr shader::ShaderTypeDesc TextureInspectorCubeDesc =
    MakeTextureInspector(TextureInspectorCubePixelType, "TextureInspectorCubePixel");
constexpr shader::ShaderTypeDesc TextureInspectorCubeArrayDesc =
    MakeTextureInspector(TextureInspectorCubeArrayPixelType, "TextureInspectorCubeArrayPixel");
constexpr shader::ShaderTypeDesc TextureInspectorVolumeDesc =
    MakeTextureInspector(TextureInspectorVolumePixelType, "TextureInspectorVolumePixel");

constexpr shader::ShaderTypeDesc TextureInspector2DUintDesc =
    MakeTextureInspector(TextureInspector2DUintPixelType, "TextureInspector2DUintPixel");
constexpr shader::ShaderTypeDesc TextureInspector2DArrayUintDesc =
    MakeTextureInspector(TextureInspector2DArrayUintPixelType, "TextureInspector2DArrayUintPixel");
constexpr shader::ShaderTypeDesc TextureInspectorVolumeUintDesc =
    MakeTextureInspector(TextureInspectorVolumeUintPixelType, "TextureInspectorVolumeUintPixel");
constexpr shader::ShaderTypeDesc TextureInspector2DIntDesc =
    MakeTextureInspector(TextureInspector2DIntPixelType, "TextureInspector2DIntPixel");
constexpr shader::ShaderTypeDesc TextureInspector2DArrayIntDesc =
    MakeTextureInspector(TextureInspector2DArrayIntPixelType, "TextureInspector2DArrayIntPixel");
constexpr shader::ShaderTypeDesc TextureInspectorVolumeIntDesc =
    MakeTextureInspector(TextureInspectorVolumeIntPixelType, "TextureInspectorVolumeIntPixel");

// --- SL1 유틸리티(Docs/Design/EngineShaders 14.4절, RenderBase/UtilityShaderTypes.h) ---

// storage image 형식 퍼뮤테이션 이름이다(StorageImageFormat 순서).
constexpr StringView StorageImageFormatNames[] = {"Rgba8", "Rgba8Snorm", "Rgba16f", "Rgba32f",
                                                  "R32f",  "R32ui",      "R32i",    "Rgba32ui",
                                                  "Rg16f", "R16f",       "Rg32f",   "R11g11b10f"};
static_assert(sizeof(StorageImageFormatNames) / sizeof(StorageImageFormatNames[0]) ==
                  StorageImageFormatCount,
              "StorageImageFormatNames must list every StorageImageFormat");

// 형식이 그 storage image 형식을 선언할 수 있는가(GLES는 GLES 3.2 core image 형식만).
constexpr bool FormatDeclaresStorageImage(uint32 format,
                                          const shader::ShaderFormatFacts& facts) noexcept {
    return facts.backend != shader::ShaderBackend::OpenGL || format < GlesStorageImageFormatCount;
}

constexpr StringView GenerateMipsComputeDimensionNames[] = {"Texture2D", "Texture2DArray",
                                                            "Texture3D"};
constexpr shader::ShaderPermutationDimension GenerateMipsComputeDimensions[] = {
    shader::ShaderPermutationDimension::Enum("MIPS_DIMENSION", GenerateMipsComputeDimensionNames),
    shader::ShaderPermutationDimension::Enum("MIPS_FORMAT", StorageImageFormatNames)};

// GenerateMipsCompute 거르기다(float storage 형식만, GLES는 core 형식만).
constexpr bool GenerateMipsComputeShouldCompile(shader::VertexInputTypeId, uint32 permutation,
                                                const shader::ShaderFormatFacts& facts) noexcept {
    const uint32 format = permutation / GenerateMipsComputeDimensionCount;
    return IsFloatStorageImageFormat(static_cast<StorageImageFormat>(format)) &&
           FormatDeclaresStorageImage(format, facts);
}

constexpr shader::ShaderParameterLayout GenerateMipsLayouts[] = {
    shader::ShaderParameterLayoutOf<GenerateMipsConstants>()};

constexpr shader::ShaderTypeDesc MakeGenerateMipsCompute() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        GenerateMipsComputeType, "/Engine/Private/Utility/GenerateMips.rsf", "GenerateMipsCompute",
        shader::ShaderStageCompute, shader::ShaderTypeOwner::Engine);
    desc.permutation.dimensions = GenerateMipsComputeDimensions;
    desc.shouldCompile = &GenerateMipsComputeShouldCompile;
    desc.parameterLayouts = GenerateMipsLayouts;
    return desc;
}

constexpr StringView GenerateMipsPixelDimensionNames[] = {"Texture2D", "Texture2DArray",
                                                          "TextureCube", "Texture3D"};
constexpr shader::ShaderPermutationDimension GenerateMipsPixelDimensions[] = {
    shader::ShaderPermutationDimension::Enum("MIPS_SOURCE_DIMENSION",
                                             GenerateMipsPixelDimensionNames)};

constexpr shader::ShaderTypeDesc MakeGenerateMipsPixel() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        GenerateMipsPixelType, "/Engine/Private/Utility/GenerateMips.rsf", "GenerateMipsPixel",
        shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);
    desc.permutation.dimensions = GenerateMipsPixelDimensions;
    desc.parameterLayouts = GenerateMipsLayouts;
    return desc;
}

constexpr StringView ClearDimensionNames[] = {"Buffer", "Texture2D", "Texture2DArray", "Texture3D"};
constexpr shader::ShaderPermutationDimension ClearWritableDimensions[] = {
    shader::ShaderPermutationDimension::Enum("CLEAR_DIMENSION", ClearDimensionNames),
    shader::ShaderPermutationDimension::Enum("CLEAR_FORMAT", StorageImageFormatNames)};

// ClearWritableCompute 거르기다(GLES는 core 형식만).
constexpr bool ClearWritableShouldCompile(shader::VertexInputTypeId, uint32 permutation,
                                          const shader::ShaderFormatFacts& facts) noexcept {
    return FormatDeclaresStorageImage(permutation / ClearDimensionCount, facts);
}

constexpr shader::ShaderParameterLayout ClearWritableLayouts[] = {
    shader::ShaderParameterLayoutOf<ClearWritableConstants>()};

constexpr shader::ShaderTypeDesc MakeClearWritableCompute() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        ClearWritableComputeType, "/Engine/Private/Utility/ClearResources.rsf",
        "ClearWritableCompute", shader::ShaderStageCompute, shader::ShaderTypeOwner::Engine);
    desc.permutation.dimensions = ClearWritableDimensions;
    desc.shouldCompile = &ClearWritableShouldCompile;
    desc.parameterLayouts = ClearWritableLayouts;
    return desc;
}

constexpr shader::ShaderParameterLayout ClearRegionLayouts[] = {
    shader::ShaderParameterLayoutOf<ClearRegionConstants>()};

constexpr shader::ShaderTypeDesc MakeClearRegionVertex() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        ClearRegionVertexType, "/Engine/Private/Utility/ClearResources.rsf", "ClearRegionVertex",
        shader::ShaderStageVertex, shader::ShaderTypeOwner::Engine);
    desc.parameterLayouts = ClearRegionLayouts;
    return desc;
}

constexpr StringView ClearValueTypeNames[] = {"Float", "Uint", "Sint"};
constexpr shader::ShaderPermutationDimension ClearRegionPixelDimensions[] = {
    shader::ShaderPermutationDimension::Enum("CLEAR_VALUE_TYPE", ClearValueTypeNames)};

constexpr shader::ShaderTypeDesc MakeClearRegionPixel() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        ClearRegionPixelType, "/Engine/Private/Utility/ClearResources.rsf", "ClearRegionPixel",
        shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);
    desc.permutation.dimensions = ClearRegionPixelDimensions;
    desc.parameterLayouts = ClearRegionLayouts;
    return desc;
}

constexpr shader::ShaderParameterLayout DispatchArgumentsInitLayouts[] = {
    shader::ShaderParameterLayoutOf<DispatchArgumentsInitConstants>()};

constexpr shader::ShaderTypeDesc MakeInitializeDispatchArguments() noexcept {
    shader::ShaderTypeDesc desc = shader::MakeShaderTypeDesc(
        InitializeDispatchArgumentsComputeType, "/Engine/Private/Utility/ClearResources.rsf",
        "InitializeDispatchArgumentsCompute", shader::ShaderStageCompute,
        shader::ShaderTypeOwner::Engine);
    desc.parameterLayouts = DispatchArgumentsInitLayouts;
    return desc;
}

constexpr shader::ShaderTypeDesc CopyDepthToColorPixelDesc = shader::MakeShaderTypeDesc(
    CopyDepthToColorPixelType, "/Engine/Private/Utility/CopyDepth.rsf", "CopyDepthToColorPixel",
    shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);
constexpr shader::ShaderTypeDesc CopyDepthPixelDesc = shader::MakeShaderTypeDesc(
    CopyDepthPixelType, "/Engine/Private/Utility/CopyDepth.rsf", "CopyDepthPixel",
    shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);

// 자리표시 셋은 같은 생성 원문을 include한다(쓰지 않는 엔트리는 컴파일러가 상수를 버린다).
constexpr shader::ShaderParameterLayout SolidColorLayouts[] = {
    shader::ShaderParameterLayoutOf<SolidColorConstants>()};

constexpr shader::ShaderTypeDesc MakePlaceholder(shader::ShaderTypeName name,
                                                 StringView entry) noexcept {
    shader::ShaderTypeDesc desc =
        shader::MakeShaderTypeDesc(name, "/Engine/Private/Utility/PlaceholderShaders.rsf", entry,
                                   shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);
    desc.parameterLayouts = SolidColorLayouts;
    return desc;
}

constexpr shader::ShaderTypeDesc GenerateMipsComputeDesc = MakeGenerateMipsCompute();
constexpr shader::ShaderTypeDesc GenerateMipsPixelDesc = MakeGenerateMipsPixel();
constexpr shader::ShaderTypeDesc ClearWritableComputeDesc = MakeClearWritableCompute();
constexpr shader::ShaderTypeDesc ClearRegionVertexDesc = MakeClearRegionVertex();
constexpr shader::ShaderTypeDesc ClearRegionPixelDesc = MakeClearRegionPixel();
constexpr shader::ShaderTypeDesc InitializeDispatchArgumentsDesc =
    MakeInitializeDispatchArguments();
constexpr shader::ShaderTypeDesc SolidColorPixelDesc =
    MakePlaceholder(SolidColorPixelType, "SolidColorPixel");
constexpr shader::ShaderTypeDesc DepthOnlyPixelDesc =
    MakePlaceholder(DepthOnlyPixelType, "DepthOnlyPixel");
constexpr shader::ShaderTypeDesc MissingShaderPixelDesc =
    MakePlaceholder(MissingShaderPixelType, "MissingShaderPixel");

constexpr shader::ShaderTypeDesc BuiltinMeshVertexDesc = MakeBuiltinMeshVertex();
constexpr shader::ShaderTypeDesc BuiltinMeshPixelDesc = MakeBuiltinMeshPixel();
constexpr shader::ShaderTypeDesc TonemapPixelDesc = MakeTonemapPixel();

constexpr const shader::ShaderTypeDesc* Types[] = {&FullscreenTriangleVertexDesc,
                                                   &CopyTexturePixelDesc,
                                                   &BuiltinMeshVertexDesc,
                                                   &BuiltinMeshPixelDesc,
                                                   &TonemapPixelDesc,
                                                   &TextureInspector2DDesc,
                                                   &TextureInspector2DArrayDesc,
                                                   &TextureInspectorCubeDesc,
                                                   &TextureInspectorCubeArrayDesc,
                                                   &TextureInspectorVolumeDesc,
                                                   &TextureInspector2DUintDesc,
                                                   &TextureInspector2DArrayUintDesc,
                                                   &TextureInspectorVolumeUintDesc,
                                                   &TextureInspector2DIntDesc,
                                                   &TextureInspector2DArrayIntDesc,
                                                   &TextureInspectorVolumeIntDesc,
                                                   &GenerateMipsComputeDesc,
                                                   &GenerateMipsPixelDesc,
                                                   &ClearWritableComputeDesc,
                                                   &ClearRegionVertexDesc,
                                                   &ClearRegionPixelDesc,
                                                   &InitializeDispatchArgumentsDesc,
                                                   &CopyDepthToColorPixelDesc,
                                                   &CopyDepthPixelDesc,
                                                   &SolidColorPixelDesc,
                                                   &DepthOnlyPixelDesc,
                                                   &MissingShaderPixelDesc};

constexpr shader::ShaderVertexInputType VertexInputs[] = {shader::MakeShaderVertexInputType(
    BuiltinVertexInputName, "/Engine/Private/VertexInput/BuiltinVertexInput.rsh",
    shader::ShaderTypeOwner::Engine)};

} // namespace

Span<const shader::ShaderTypeDesc* const> EngineShaderTypes() noexcept {
    return Span<const shader::ShaderTypeDesc* const>(Types);
}

Span<const shader::ShaderVertexInputType> EngineVertexInputTypes() noexcept {
    return Span<const shader::ShaderVertexInputType>(VertexInputs);
}

} // namespace rocks::render
