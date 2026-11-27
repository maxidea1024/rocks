#include <UiRender/DrawBatchRenderer.h>
#include <RenderBase/PipelineStatePresets.h>
#include <UiRender/UiShaderTypes.h>
#include <stddef.h>

namespace rocks::ui {

namespace {

enum class UiShader {
    Solid,
    Image,
    Mask,
    Retained,
    Blur,
    RoundedBox
};

// 그리기별 상수 byte 수다(UiElements.rsf UiDrawConstants·UiBackdropBlur.rsf UiBlurConstants).
constexpr uint32 UiDrawConstantBytes = 16;
constexpr uint32 UiBlurConstantBytes = 32;
// UiRoundedBox.rsf UiRoundedBoxConstants(화면·params·반경·외곽선 색)다.
constexpr uint32 UiRoundedBoxConstantBytes = 64;

// 정점 속성 수다(위치·색, 텍스처는 UV를 더한다).
constexpr uint32 SolidAttributeCount = 2;
constexpr uint32 TexturedAttributeCount = 3;

Result<graphics::PipelineHandle> CreatePipeline(graphics::Device& device,
                                                render::ShaderSet& shaders,
                                                graphics::PixelFormat format,
                                                UiShader shader) noexcept {
    const bool textured = shader != UiShader::Solid;
    const graphics::VertexAttribute attributes[] = {
        {0, graphics::VertexFormat::Float2, 0, offsetof(DrawVertex, position)},
        {1, graphics::VertexFormat::Float4, 0, offsetof(DrawVertex, color)},
        {2, graphics::VertexFormat::Float2, 0, offsetof(DrawVertex, uv)}};
    const graphics::VertexStream streams[] = {
        {sizeof(DrawVertex), graphics::VertexStepRate::Vertex}};
    graphics::PipelineDesc desc;
    desc.layout = {Span<const graphics::VertexAttribute>(
                       attributes, textured ? TexturedAttributeCount : SolidAttributeCount),
                   streams};
    desc.drawConstantBytes = shader == UiShader::Blur         ? UiBlurConstantBytes
                             : shader == UiShader::RoundedBox ? UiRoundedBoxConstantBytes
                                                              : UiDrawConstantBytes;
    desc.colorFormats[0] = format;
    if (shader != UiShader::Blur) {
        // 보관 표면은 premultiplied다(RGB에 알파가 이미 곱해져 있다).
        desc.blend[0] =
            shader == UiShader::Retained ? render::PremultipliedAlphaBlend() : render::AlphaBlend();
    }

    switch (shader) {
    case UiShader::Solid:
        return CreateUiPipeline(device, shaders, UiSolidVertexType, UiSolidPixelType, desc);
    case UiShader::Image:
        return CreateUiPipeline(device, shaders, UiImageVertexType, UiImagePixelType, desc);
    case UiShader::Mask:
        return CreateUiPipeline(device, shaders, UiImageVertexType, UiMaskPixelType, desc);
    case UiShader::Retained:
        return CreateUiPipeline(device, shaders, UiImageVertexType, UiRetainedPixelType, desc);
    case UiShader::RoundedBox:
        return CreateUiPipeline(device, shaders, UiRoundedBoxVertexType, UiRoundedBoxPixelType,
                                desc);
    case UiShader::Blur:
    default:
        return CreateUiPipeline(device, shaders, UiBlurVertexType, UiBlurPixelType, desc);
    }
}

} // namespace

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateSolidPipeline(graphics::Device& device, render::ShaderSet& shaders,
                                       graphics::PixelFormat format) noexcept {
    return CreatePipeline(device, shaders, format, UiShader::Solid);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateImagePipeline(graphics::Device& device, render::ShaderSet& shaders,
                                       graphics::PixelFormat format) noexcept {
    return CreatePipeline(device, shaders, format, UiShader::Image);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateMaskPipeline(graphics::Device& device, render::ShaderSet& shaders,
                                      graphics::PixelFormat format) noexcept {
    return CreatePipeline(device, shaders, format, UiShader::Mask);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateRetainedPipeline(graphics::Device& device, render::ShaderSet& shaders,
                                          graphics::PixelFormat format) noexcept {
    return CreatePipeline(device, shaders, format, UiShader::Retained);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateRoundedBoxPipeline(graphics::Device& device, render::ShaderSet& shaders,
                                            graphics::PixelFormat format) noexcept {
    return CreatePipeline(device, shaders, format, UiShader::RoundedBox);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateBlurPipeline(graphics::Device& device, render::ShaderSet& shaders,
                                      graphics::PixelFormat format) noexcept {
    return CreatePipeline(device, shaders, format, UiShader::Blur);
}

} // namespace rocks::ui
