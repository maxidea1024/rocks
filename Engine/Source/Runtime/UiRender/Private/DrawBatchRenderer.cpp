#include <UiRender/DrawBatchRenderer.h>
#include <UiRender/DrawBatchBuffers.h>
#include <Core/ScopeExit.h>
#include <stddef.h>

namespace rocks::ui {

namespace {

constexpr char SolidHlsl[] = R"(
cbuffer DrawConstants : register(b0) { float4 screen; };
struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; };
VSOutput VSMain(float2 position : ATTRIBUTE0, float4 color : ATTRIBUTE1) {
    VSOutput output;
    output.position = float4(position * screen.xy + screen.zw, 0.0, 1.0);
    output.color = color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target { return input.color; }
)";

constexpr char SolidVertexGlsl[] = R"(#version 320 es
precision highp float;
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
uniform highp vec4 vu_h[1];
layout(location = 0) out vec4 outColor;
void main() {
    gl_Position = vec4(position * vu_h[0].xy + vu_h[0].zw, 0.0, 1.0);
    outColor = color;
}
)";

constexpr char SolidPixelGlsl[] = R"(#version 320 es
precision highp float;
layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = inColor; }
)";

constexpr char ImageHlsl[] = R"(
cbuffer DrawConstants : register(b0) { float4 screen; };
Texture2D image : register(t0);
SamplerState imageSampler : register(s0);
struct VSOutput { float4 position : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; };
VSOutput VSMain(float2 position : ATTRIBUTE0, float4 color : ATTRIBUTE1, float2 uv : ATTRIBUTE2) {
    VSOutput output;
    output.position = float4(position * screen.xy + screen.zw, 0.0, 1.0);
    output.color = color;
    output.uv = uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target { return image.Sample(imageSampler, input.uv) * input.color; }
)";

constexpr char ImageVertexGlsl[] = R"(#version 320 es
precision highp float;
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 2) in vec2 uv;
uniform highp vec4 vu_h[1];
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;
void main() {
    gl_Position = vec4(position * vu_h[0].xy + vu_h[0].zw, 0.0, 1.0);
    outColor = color;
    outUv = uv;
}
)";

constexpr char ImagePixelGlsl[] = R"(#version 320 es
precision highp float;
layout(binding = 0) uniform highp sampler2D ps0;
layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(ps0, inUv) * inColor; }
)";

// 투명 바탕에 합성한 텍스처는 RGB에 알파가 곱해져 있으므로, 위젯 불투명도도 RGB와 A에 함께
// 적용한다.
constexpr char RetainedPixelHlsl[] = R"(
Texture2D image : register(t0);
SamplerState imageSampler : register(s0);
float4 PSMain(float4 position : SV_Position, float4 tint : COLOR0, float2 uv : TEXCOORD0) : SV_Target {
    float4 sampleColor = image.Sample(imageSampler, uv);
    return float4(sampleColor.rgb * tint.rgb * tint.a, sampleColor.a * tint.a);
}
)";

constexpr char RetainedPixelGlsl[] = R"(#version 320 es
precision highp float;
layout(binding = 0) uniform highp sampler2D ps0;
layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 sampleColor = texture(ps0, inUv);
    outColor = vec4(sampleColor.rgb * inColor.rgb * inColor.a, sampleColor.a * inColor.a);
}
)";

constexpr char MaskPixelHlsl[] = R"(
Texture2D image : register(t0);
SamplerState imageSampler : register(s0);
float4 PSMain(float4 position : SV_Position, float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target {
    return float4(color.rgb, color.a * image.Sample(imageSampler, uv).r);
}
)";

constexpr char MaskPixelGlsl[] = R"(#version 320 es
precision highp float;
layout(binding = 0) uniform highp sampler2D ps0;
layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(inColor.rgb, inColor.a * texture(ps0, inUv).r); }
)";

// UV는 현재 패스의 전체 화면을 기준으로 계산한다. 위젯을 이동하거나 회전해도
// 뒤에 그린 배경 중 같은 화면 좌표를 샘플한다. 샘플 간격은 출력 픽셀 단위다.
constexpr char BlurHlsl[] = R"(
cbuffer DrawConstants : register(b0) { float4 screen; float4 cornerRadius; };
Texture2D image : register(t0);
SamplerState imageSampler : register(s0);
struct VSOutput { float4 position : SV_Position; float4 samplePoint : TEXCOORD0; float4 localBox : TEXCOORD1; float4 radii : TEXCOORD2; };
VSOutput VSMain(float2 p : ATTRIBUTE0, float4 color : ATTRIBUTE1, float2 uv : ATTRIBUTE2) {
    VSOutput o;
    o.position = float4(p * screen.xy + screen.zw, 0, 1);
    float ratio = uv.y / max(0.001, uv.x);
    o.samplePoint = float4(p * float2(screen.x, -screen.y) * 0.5, exp(-0.5 * ratio * ratio), uv.y);
    o.localBox = color;
    o.radii = cornerRadius;
    return o;
}
float4 PSMain(VSOutput i) : SV_Target {
    uint w, h; image.GetDimensions(w, h);
    if (i.radii.x < 0) return image.Load(int3(int2(i.position.xy), 0));
    float2 halfTexel = 0.5 / float2(w, h);
    float2 step = i.samplePoint.w * halfTexel;
    float2 boxHalf = 0.5 * i.localBox.zw;
    float2 delta = i.localBox.xy - boxHalf;
    float radius = delta.y < 0 ? (delta.x < 0 ? i.radii.x : i.radii.y) : (delta.x < 0 ? i.radii.w : i.radii.z);
    radius = min(radius, min(boxHalf.x, boxHalf.y));
    if (length(max(abs(delta) - boxHalf + radius, 0.0)) > radius) discard;
    float w1 = i.samplePoint.z, w2 = w1*w1*w1*w1;
    float weights[5] = {w2,w1,1,w1,w2};
    float total = 1 + 2*(w1+w2);
    float4 sum = 0;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            float weight = weights[x+2] * weights[y+2];
            sum += weight * image.SampleLevel(imageSampler, clamp(i.samplePoint.xy + float2(x,y)*step, halfTexel, 1.0-halfTexel), 0);
        }
    }
    return sum / (total * total);
}
)";

constexpr char BlurVertexGlsl[] = R"(#version 320 es
precision highp float;
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 box;
layout(location = 2) in vec2 uv;
uniform highp vec4 vu_h[2];
layout(location = 0) out vec4 samplePoint;
layout(location = 1) out vec4 localBox;
layout(location = 2) out vec4 radii;
void main() {
    gl_Position = vec4(position * vu_h[0].xy + vu_h[0].zw, 0, 1);
    float ratio = uv.y / max(0.001, uv.x);
    samplePoint = vec4(position * vec2(vu_h[0].x, -vu_h[0].y) * 0.5, exp(-0.5*ratio*ratio), uv.y);
    localBox = box;
    radii = vu_h[1];
}
)";

constexpr char BlurPixelGlsl[] = R"(#version 320 es
precision highp float;
layout(binding = 0) uniform highp sampler2D ps0;
layout(location = 0) in vec4 samplePoint;
layout(location = 1) in vec4 localBox;
layout(location = 2) in vec4 radii;
layout(location = 0) out vec4 outColor;
void main() {
    if (radii.x < 0.0) {
        ivec2 size = textureSize(ps0,0);
        outColor = texelFetch(ps0,clamp(ivec2(samplePoint.xy * vec2(size)),ivec2(0),size-1),0);
        return;
    }
    vec2 halfTexel = 0.5 / vec2(textureSize(ps0,0));
    vec2 stepSize = samplePoint.w * halfTexel;
    vec2 boxHalf = 0.5 * localBox.zw;
    vec2 delta = localBox.xy - boxHalf;
    float radius = delta.y < 0.0 ? (delta.x < 0.0 ? radii.x : radii.y) : (delta.x < 0.0 ? radii.w : radii.z);
    radius = min(radius,min(boxHalf.x,boxHalf.y));
    if (length(max(abs(delta) - boxHalf + radius,0.0)) > radius) discard;
    float w1 = samplePoint.z, w2 = w1*w1*w1*w1;
    float weights[5] = float[5](w2,w1,1.0,w1,w2);
    float total = 1.0 + 2.0*(w1+w2);
    vec4 sum = vec4(0.0);
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            float weight = weights[x+2] * weights[y+2];
            sum += weight * textureLod(ps0,clamp(samplePoint.xy+vec2(x,y)*stepSize,halfTexel,1.0-halfTexel),0.0);
        }
    }
    outColor = sum / (total * total);
}
)";

enum class UiShader {
    Solid,
    Image,
    Mask,
    Retained,
    Blur
};

Result<graphics::PipelineHandle>
CreateUiPipeline(graphics::Device& device, graphics::PixelFormat format, UiShader shader,
                 const graphics::ShaderSource* effect = nullptr) noexcept {
    const bool textured = shader != UiShader::Solid;
    const graphics::VertexAttribute attributes[] = {
        {0, graphics::VertexFormat::Float2, 0, offsetof(DrawVertex, position)},
        {1, graphics::VertexFormat::Float4, 0, offsetof(DrawVertex, color)},
        {2, graphics::VertexFormat::Float2, 0, offsetof(DrawVertex, uv)}};
    const graphics::VertexStream streams[] = {
        {sizeof(DrawVertex), graphics::VertexStepRate::Vertex}};
    graphics::PipelineDesc desc;
    if (textured) {
        desc.vertex = {ImageHlsl, ImageVertexGlsl};
        desc.pixel = {ImageHlsl, ImagePixelGlsl};
        if (shader == UiShader::Blur) {
            desc.vertex = {BlurHlsl, BlurVertexGlsl};
            desc.pixel = {BlurHlsl, BlurPixelGlsl};
        }
        if (shader == UiShader::Retained) {
            desc.pixel =
                effect ? *effect : graphics::ShaderSource{RetainedPixelHlsl, RetainedPixelGlsl};
        }
        if (shader == UiShader::Mask) {
            desc.pixel = {MaskPixelHlsl, MaskPixelGlsl};
        }
    } else {
        desc.vertex = {SolidHlsl, SolidVertexGlsl};
        desc.pixel = {SolidHlsl, SolidPixelGlsl};
    }
    desc.layout = {Span<const graphics::VertexAttribute>(attributes, textured ? 3u : 2u), streams};
    desc.drawConstantBytes = shader == UiShader::Blur ? 32 : 16;
    desc.colorFormats[0] = format;
    if (shader != UiShader::Blur) {
        desc.blend[0] = graphics::AlphaBlend();
        if (shader == UiShader::Retained) {
            desc.blend[0].sourceColor = graphics::BlendFactor::One;
        }
    }
    return device.CreatePipeline(desc);
}

} // namespace

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateSolidPipeline(graphics::Device& device,
                                       graphics::PixelFormat format) noexcept {
    return CreateUiPipeline(device, format, UiShader::Solid);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateImagePipeline(graphics::Device& device,
                                       graphics::PixelFormat format) noexcept {
    return CreateUiPipeline(device, format, UiShader::Image);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateMaskPipeline(graphics::Device& device,
                                      graphics::PixelFormat format) noexcept {
    return CreateUiPipeline(device, format, UiShader::Mask);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateRetainedPipeline(graphics::Device& device,
                                          graphics::PixelFormat format) noexcept {
    return CreateUiPipeline(device, format, UiShader::Retained);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateEffectPipeline(graphics::Device& device, graphics::PixelFormat format,
                                        graphics::ShaderSource pixel) noexcept {
    return CreateUiPipeline(device, format, UiShader::Retained, &pixel);
}

Result<graphics::PipelineHandle>
DrawBatchRenderer::CreateBlurPipeline(graphics::Device& device,
                                      graphics::PixelFormat format) noexcept {
    return CreateUiPipeline(device, format, UiShader::Blur);
}

Result<void> DrawBatchRenderer::Draw(graphics::Device& device, graphics::CommandContext& commands,
                                     graphics::PipelineHandle pipeline, const DrawBatch& batch,
                                     Vec2 viewportSize, const SlotArray<BrushTexture>* textures,
                                     graphics::PixelFormat targetFormat) noexcept {
    DrawBatchBuffers buffers(device, batch, targetFormat);
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
                                      : (texture ? texture->pipeline : pipeline);
        if (currentPipeline != nextPipeline) {
            commands.SetPipeline(nextPipeline);
            currentPipeline = nextPipeline;
        }
        struct BlurConstants {
            ScreenTransform screen;
            Vec4 radii;
        };
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
