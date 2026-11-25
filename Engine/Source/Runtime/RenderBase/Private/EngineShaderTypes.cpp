#include <RenderBase/EngineShaderTypes.h>

// 엔진 셰이더 타입 desc다(Docs/Design/RenderBase 11.4절 2항). 원문 경로는 Engine/Shaders 아래다.
namespace rocks::render {

namespace {

constexpr shader::ShaderTypeDesc FullscreenTriangleVertexDesc = shader::MakeShaderTypeDesc(
    FullscreenTriangleVertexType, "/Engine/Private/Utility/FullscreenTriangle.rsf",
    "FullscreenTriangleVertex", shader::ShaderStageVertex, shader::ShaderTypeOwner::Engine);

constexpr shader::ShaderTypeDesc CopyTexturePixelDesc = shader::MakeShaderTypeDesc(
    CopyTexturePixelType, "/Engine/Private/Utility/CopyTexture.rsf", "CopyTexturePixel",
    shader::ShaderStagePixel, shader::ShaderTypeOwner::Engine);

constexpr const shader::ShaderTypeDesc* Types[] = {&FullscreenTriangleVertexDesc,
                                                   &CopyTexturePixelDesc};

} // namespace

Span<const shader::ShaderTypeDesc* const> EngineShaderTypes() noexcept {
    return Span<const shader::ShaderTypeDesc* const>(Types);
}

} // namespace rocks::render
