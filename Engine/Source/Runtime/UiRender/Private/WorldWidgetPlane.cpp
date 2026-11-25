#include <UiRender/WorldWidgetPlane.h>

namespace rocks::ui {
namespace {
constexpr char PlaneVsHlsl[] = R"(
cbuffer DrawConstants : register(b0) {float4 plane[6];};
struct V {float4 p : SV_Position; float2 uv : TEXCOORD0; float4 now : TEXCOORD1; float4 old : TEXCOORD2;};
V VSMain(float2 uv : ATTRIBUTE0) {
    V o;
    o.now = plane[0]+uv.x*plane[1]+uv.y*plane[2];
    o.old = plane[3]+uv.x*plane[4]+uv.y*plane[5];
    o.p = o.now; o.uv = uv; return o;
})";
constexpr char PlaneVsGlsl[] = R"(#version 320 es
precision highp float;
layout(location=0) in vec2 uv;
uniform highp vec4 vu_h[6];
layout(location=0) out vec2 tex;
layout(location=1) out vec4 current;
layout(location=2) out vec4 previous;
void main() {
    current = vu_h[0]+uv.x*vu_h[1]+uv.y*vu_h[2];
    previous = vu_h[3]+uv.x*vu_h[4]+uv.y*vu_h[5];
    gl_Position = current; tex = uv;
})";
constexpr char PlanePsHlsl[] = R"(
Texture2D surface : register(t0); SamplerState sampler0 : register(s0);
float4 PSMain(float4 p:SV_Position, float2 uv:TEXCOORD0) : SV_Target {
    float4 color = surface.Sample(sampler0,uv); if(color.a<=0) discard; return color;
})";
constexpr char VelocityPsHlsl[] = R"(
Texture2D surface : register(t0); SamplerState sampler0 : register(s0);
struct Out { float4 color:SV_Target0; float4 velocity:SV_Target1; };
Out PSMain(float4 p:SV_Position,float2 uv:TEXCOORD0,float4 now:TEXCOORD1,float4 old:TEXCOORD2) {
    Out o; o.color=surface.Sample(sampler0,uv); if(o.color.a<=0) discard;
    o.velocity = old.w>0 ? float4((now.xy/now.w-old.xy/old.w)*float2(.5,-.5),0,0) : 0;
    return o;
})";
constexpr char PlanePsGlsl[] = R"(#version 320 es
precision highp float;
layout(binding=0) uniform highp sampler2D ps0;
layout(location=0) in vec2 tex;
layout(location=0) out vec4 color;
void main(){ color=texture(ps0,tex); if(color.a<=0.0) discard; })";
constexpr char VelocityPsGlsl[] = R"(#version 320 es
precision highp float;
layout(binding=0) uniform highp sampler2D ps0;
layout(location=0) in vec2 tex;
layout(location=1) in vec4 current;
layout(location=2) in vec4 previous;
layout(location=0) out vec4 color;
layout(location=1) out vec4 velocity;
void main(){
    color=texture(ps0,tex); if(color.a<=0.0) discard;
    velocity=previous.w>0.0?vec4((current.xy/current.w-previous.xy/previous.w)*vec2(.5,-.5),0,0):vec4(0);
})";
} // namespace

Result<WorldWidgetClipPlane> WorldWidgetClipPlane::Project(Vec3d tl, Vec3d tr, Vec3d bl,
                                                           const Mat4d& viewProjection) noexcept {
    auto point = [&](Vec3d world) noexcept {
        auto clip = viewProjection.TransformVector4({world.x, world.y, world.z, 1});
        return Vec4{static_cast<float>(clip.x), static_cast<float>(clip.y),
                    static_cast<float>(clip.z), static_cast<float>(clip.w)};
    };
    WorldWidgetClipPlane plane{point(tl), point(tr), point(bl)};
    if (!plane.topLeft.IsFinite() || !plane.topRight.IsFinite() || !plane.bottomLeft.IsFinite()) {
        return Result<WorldWidgetClipPlane>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<WorldWidgetClipPlane>::Success(plane);
}

WorldWidgetPlane::WorldWidgetPlane(graphics::Device& device) noexcept : device_(device) {
}
WorldWidgetPlane::~WorldWidgetPlane() noexcept {
    if (!quad_.IsNull()) {
        device_.Retire(quad_);
    }
    if (!pipeline_.IsNull()) {
        device_.Retire(pipeline_);
    }
}

Result<void> WorldWidgetPlane::Initialize(graphics::PixelFormat color, graphics::PixelFormat depth,
                                          graphics::PixelFormat velocity, bool writeDepth,
                                          uint32 sampleCount) noexcept {
    const graphics::VertexAttribute attributes[] = {{0, graphics::VertexFormat::Float2, 0, 0}};
    const graphics::VertexStream streams[] = {{sizeof(Vec2), graphics::VertexStepRate::Vertex}};
    graphics::PipelineDesc desc;
    desc.layout = {attributes, streams};
    desc.topology = graphics::PrimitiveTopology::TriangleStrip;
    desc.vertex = {PlaneVsHlsl, PlaneVsGlsl};
    desc.pixel = {PlanePsHlsl, PlanePsGlsl};
    desc.colorFormats[0] = color;
    desc.blend[0] = graphics::AlphaBlend();
    desc.blend[0].sourceColor = graphics::BlendFactor::One;
    desc.depthFormat = depth;
    desc.depthStencil.depthTest = depth != graphics::PixelFormat::Unknown;
    desc.depthStencil.depthWrite = writeDepth;
    desc.drawConstantBytes = sizeof(Vec4) * 6;
    desc.sampleCount = sampleCount;
    if (velocity != graphics::PixelFormat::Unknown) {
        desc.colorTargetCount = 2;
        desc.colorFormats[1] = velocity;
        desc.pixel = {VelocityPsHlsl, VelocityPsGlsl};
    }
    auto pipeline = device_.CreatePipeline(desc);
    if (!pipeline) {
        return Result<void>::Failure(pipeline.Error());
    }
    if (quad_.IsNull()) {
        constexpr Vec2 quad[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        graphics::BufferDesc buffer;
        buffer.size = sizeof(quad);
        buffer.usage = graphics::BufferUsage::Vertex;
        const Span<const uint8> bytes{reinterpret_cast<const uint8*>(quad), sizeof(quad)};
        auto made = device_.CreateBuffer(buffer, bytes);
        if (!made) {
            device_.Retire(pipeline.Value());
            return Result<void>::Failure(made.Error());
        }
        quad_ = made.Value();
    }
    if (!pipeline_.IsNull()) {
        device_.Retire(pipeline_);
    }
    pipeline_ = pipeline.Value();
    return Result<void>::Success();
}
Result<void> WorldWidgetPlane::Draw(graphics::CommandContext& commands,
                                    graphics::TextureHandle texture,
                                    const WorldWidgetClipPlane& current,
                                    const WorldWidgetClipPlane& previous) noexcept {
    if (pipeline_.IsNull() || texture.IsNull()) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    const Vec4 values[] = {current.topLeft,
                           current.topRight - current.topLeft,
                           current.bottomLeft - current.topLeft,
                           previous.topLeft,
                           previous.topRight - previous.topLeft,
                           previous.bottomLeft - previous.topLeft};
    for (const auto value : values) {
        if (!value.IsFinite()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }
    commands.SetPipeline(pipeline_);
    auto set = commands.SetDrawConstants(graphics::DrawConstantBytes(values));
    if (!set) {
        return set;
    }
    const graphics::VertexBufferBinding binding[] = {{quad_, 0}};
    commands.SetVertexBuffers(0, binding);
    commands.SetTexture(texture);
    commands.Draw(4);
    return Result<void>::Success();
}

} // namespace rocks::ui
