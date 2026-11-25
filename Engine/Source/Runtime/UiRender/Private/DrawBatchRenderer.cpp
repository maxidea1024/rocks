#include <UiRender/DrawBatchRenderer.h>
#include <UiRender/DrawBatchBuffers.h>
#include <RenderBase/PipelineStatePresets.h>
#include <UiRender/UiShaderTypes.h>
#include <Core/ScopeExit.h>
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
        desc.bindings.texture = UiTextureName;
        return CreateUiPipeline(device, shaders, UiImageVertexType, UiImagePixelType, desc);
    case UiShader::Mask:
        desc.bindings.texture = UiTextureName;
        return CreateUiPipeline(device, shaders, UiImageVertexType, UiMaskPixelType, desc);
    case UiShader::Retained:
        desc.bindings.texture = UiTextureName;
        return CreateUiPipeline(device, shaders, UiImageVertexType, UiRetainedPixelType, desc);
    case UiShader::RoundedBox:
        return CreateUiPipeline(device, shaders, UiRoundedBoxVertexType, UiRoundedBoxPixelType,
                                desc);
    case UiShader::Blur:
    default:
        desc.bindings.texture = UiBackdropName;
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

Result<void> DrawBatchRenderer::Draw(graphics::Device& device, render::ShaderSet& shaders,
                                     graphics::CommandContext& commands,
                                     graphics::PipelineHandle pipeline, const DrawBatch& batch,
                                     Vec2 viewportSize, const SlotArray<BrushTexture>* textures,
                                     graphics::PixelFormat targetFormat) noexcept {
    DrawBatchBuffers buffers(device, shaders, batch, targetFormat);
    auto uploaded = buffers.Upload();
    if (!uploaded) {
        return uploaded;
    }
    return Draw(commands, pipeline, buffers, viewportSize, textures);
}

Result<void> DrawBatchRenderer::Draw(graphics::CommandContext& commands,
                                     graphics::PipelineHandle pipeline,
                                     const DrawBatchBuffers& buffers, Vec2 viewportSize,
                                     const SlotArray<BrushTexture>* textures) noexcept {
    const auto& batch = buffers.batch_;
    if (!buffers.ready_ || buffers.revision_ != batch.Revision()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    if (pipeline.IsNull() || !viewportSize.IsFinite() || viewportSize.x < 1 || viewportSize.y < 1 ||
        static_cast<double>(viewportSize.x) > NumericLimits<uint32>::Max() ||
        static_cast<double>(viewportSize.y) > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (batch.Indices().IsEmpty()) {
        return Result<void>::Success();
    }
    if (batch.Vertices().IsEmpty() || batch.Indices().Size() > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const DrawCommand& command : batch.Commands()) {
        if (command.roundedBox) {
            if (auto ready = buffers.PrepareRoundedBox(); !ready) {
                return ready;
            }
        }
        if (command.operation == PaintOperation::BlurBackdrop ||
            command.operation == PaintOperation::BlurToBuffer) {
            auto ready = buffers.PrepareBlur(static_cast<uint32>(viewportSize.x),
                                             static_cast<uint32>(viewportSize.y));
            if (!ready) {
                return ready;
            }
        }
        if ((command.operation == PaintOperation::CopyBackdrop ||
             command.operation == PaintOperation::BlurToBuffer) &&
            command.resource.IsNull()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        if (!command.resource.IsNull()) {
            if (!textures) {
                return Result<void>::Failure(ErrorCode::NotReady);
            }
            const auto* texture = textures->Find(command.resource);
            if (!texture || texture->texture.IsNull() ||
                (command.operation == PaintOperation::Draw && texture->pipeline.IsNull())) {
                return Result<void>::Failure(ErrorCode::StaleHandle);
            }
        }
    }

    struct ScreenTransform {
        float scaleX;
        float scaleY;
        float offsetX;
        float offsetY;
    };
    const ScreenTransform transform{2.0f / viewportSize.x, -2.0f / viewportSize.y, -1.0f, 1.0f};
    commands.SetViewport({0, 0, viewportSize.x, viewportSize.y, 0, 1});
    commands.SetPipeline(pipeline);
    auto constants = commands.SetDrawConstants(graphics::DrawConstantBytes(transform));
    if (!constants) {
        return constants;
    }

    const graphics::VertexBufferBinding binding[] = {{buffers.vertices_, 0}};
    commands.SetVertexBuffers(0, binding);
    commands.SetIndexBuffer(buffers.indices_, graphics::IndexFormat::UInt32);
    auto currentPipeline = pipeline;
    for (const DrawCommand& command : batch.Commands()) {
        float left = 0;
        float top = 0;
        float right = viewportSize.x;
        float bottom = viewportSize.y;
        if (command.hasClip) {
            left = math::Clamp(math::Floor(command.clip.min.x), 0.0f, viewportSize.x);
            top = math::Clamp(math::Floor(command.clip.min.y), 0.0f, viewportSize.y);
            right = math::Clamp(math::Ceil(command.clip.max.x), 0.0f, viewportSize.x);
            bottom = math::Clamp(math::Ceil(command.clip.max.y), 0.0f, viewportSize.y);
        }
        if (right <= left || bottom <= top) {
            continue;
        }
        commands.SetScissor({static_cast<uint32>(left), static_cast<uint32>(top),
                             static_cast<uint32>(right - left), static_cast<uint32>(bottom - top)});
        const BrushTexture* texture =
            command.resource.IsNull() ? nullptr : textures->Find(command.resource);
        if (command.operation == PaintOperation::CopyBackdrop ||
            command.operation == PaintOperation::BlurBackdrop ||
            command.operation == PaintOperation::BlurToBuffer) {
            commands.SetTexture({});
            auto copied = commands.CopyPassTarget(command.operation == PaintOperation::CopyBackdrop
                                                      ? texture->texture
                                                      : buffers.backdrop_);
            if (!copied) {
                return copied;
            }
            if (command.operation == PaintOperation::CopyBackdrop) {
                continue;
            }
        }
        const auto nextPipeline = (command.operation == PaintOperation::BlurBackdrop ||
                                   command.operation == PaintOperation::BlurToBuffer)
                                      ? buffers.blurPipeline_
                                  : command.roundedBox ? buffers.roundedPipeline_
                                                       : (texture ? texture->pipeline : pipeline);
        if (currentPipeline != nextPipeline) {
            commands.SetPipeline(nextPipeline);
            currentPipeline = nextPipeline;
        }
        struct BlurConstants {
            ScreenTransform screen;
            Vec4 radii;
        };
        struct RoundedBoxConstants {
            ScreenTransform screen;
            Vec4 params;
            Vec4 radii;
            LinearColor outline;
        };
        if (command.roundedBox) {
            const RoundedBoxConstants rounded{
                transform,
                {0.0f, command.outlineWidth, command.roundedSize.x, command.roundedSize.y},
                command.cornerRadius,
                command.outlineColor};
            if (auto set = commands.SetDrawConstants(graphics::DrawConstantBytes(rounded)); !set) {
                return set;
            }
        } else if (command.operation == PaintOperation::Draw) {
            // 다른 크기의 상수(블러·둥근 상자) 뒤에는 화면 변환만 다시 넣는다.
            if (auto set = commands.SetDrawConstants(graphics::DrawConstantBytes(transform));
                !set) {
                return set;
            }
        }
        if (command.operation == PaintOperation::BlurBackdrop ||
            command.operation == PaintOperation::BlurToBuffer) {
            const BlurConstants blur{transform, command.cornerRadius};
            auto set = commands.SetDrawConstants(graphics::DrawConstantBytes(blur));
            if (!set) {
                return set;
            }
            commands.SetTexture(buffers.backdrop_);
        } else if (texture) {
            commands.SetTexture(texture->texture);
        }
        commands.DrawIndexed(command.indexCount, 1, command.firstIndex);
        if (command.operation == PaintOperation::BlurToBuffer) {
            // 같은 패스에서 필터 결과를 복사한 뒤 변경한 픽셀만 원본으로 복구한다.
            // 뒤에 그리는 위젯과 다음 후처리 항목은 최초 화면에서 출발한다.
            auto captured = commands.CopyPassTarget(texture->texture);
            const BlurConstants restore{transform, {-1, -1, -1, -1}};
            auto set = commands.SetDrawConstants(graphics::DrawConstantBytes(restore));
            if (!set) {
                return set;
            }
            commands.DrawIndexed(command.indexCount, 1, command.firstIndex);
            if (!captured) {
                return captured;
            }
        }
    }
    return Result<void>::Success();
}

Result<void> DrawBatchRenderer::ReadPixels(graphics::Device& device,
                                           graphics::PipelineHandle pipeline,
                                           const DrawBatchBuffers& buffers,
                                           const SlotArray<BrushTexture>& textures, uint32 width,
                                           uint32 height, Array<uint8>& rgba) noexcept {
    if (width == 0 || height == 0 || uint64(width) * height > MaxSize / 4) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    graphics::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.usage = graphics::TextureUsage::RenderTarget;
    auto target = device.CreateTexture(desc, {});
    if (!target) {
        return Result<void>::Failure(target.Error());
    }
    ROCKS_ON_SCOPE_EXIT {
        device.Retire(target.Value());
    };
    auto frame = device.BeginOffscreen(target.Value(), {0.025f, 0.03f, 0.04f, 1});
    if (!frame) {
        return Result<void>::Failure(frame.Error());
    }
    auto drawn =
        DrawBatchRenderer::Draw(*frame.Value(), pipeline, buffers,
                                {static_cast<float>(width), static_cast<float>(height)}, &textures);
    auto ended = device.EndFrame();
    if (!drawn) {
        return drawn;
    }
    if (!ended) {
        return Result<void>::Failure(ended.Error());
    }

    auto allocated = rgba.TryResize(static_cast<usize>(width) * height * 4);
    if (!allocated) {
        return allocated;
    }
    return device.ReadPixels(target.Value(), rgba.View());
}

} // namespace rocks::ui
