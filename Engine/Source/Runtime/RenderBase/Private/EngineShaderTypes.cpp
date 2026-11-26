#include <RenderBase/EngineShaderTypes.h>

#include <RenderBase/BuiltinResources.shaderparams.h>
#include <RenderBase/EngineShaderTypes.shaderparams.h>

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

constexpr shader::ShaderTypeDesc BuiltinMeshVertexDesc = MakeBuiltinMeshVertex();
constexpr shader::ShaderTypeDesc BuiltinMeshPixelDesc = MakeBuiltinMeshPixel();
constexpr shader::ShaderTypeDesc TonemapPixelDesc = MakeTonemapPixel();

constexpr const shader::ShaderTypeDesc* Types[] = {
    &FullscreenTriangleVertexDesc,   &CopyTexturePixelDesc,       &BuiltinMeshVertexDesc,
    &BuiltinMeshPixelDesc,           &TonemapPixelDesc,           &TextureInspector2DDesc,
    &TextureInspector2DArrayDesc,    &TextureInspectorCubeDesc,   &TextureInspectorCubeArrayDesc,
    &TextureInspectorVolumeDesc,     &TextureInspector2DUintDesc, &TextureInspector2DArrayUintDesc,
    &TextureInspectorVolumeUintDesc, &TextureInspector2DIntDesc,  &TextureInspector2DArrayIntDesc,
    &TextureInspectorVolumeIntDesc};

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
