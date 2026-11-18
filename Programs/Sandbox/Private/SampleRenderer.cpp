#include "SampleRenderer.h"

#include <Core/Diagnostics/Log.h>
#include <Graphics/GraphicsLog.h>

#include <Base/Compiler.h>

namespace sandbox::sample {

namespace {

struct Vertex {
    float position[3];
    float uv[2];
    float color[4];
};

constexpr char ShaderHlsl[] = R"(
cbuffer DrawConstants : register(b0) {
    float4x4 transform;
    float4 color;
};

struct VSInput {
    float3 position : ATTRIBUTE0;
    float2 uv : ATTRIBUTE1;
    float4 color : ATTRIBUTE2;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

Texture2D albedo : register(t0);
SamplerState linearClamp : register(s0);

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), transform);
    output.uv = input.uv;
    output.color = input.color * color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return albedo.Sample(linearClamp, input.uv) * input.color;
}
)";

// OpenGL ES 3.2 원문(GLSL ES 3.20)이다. SH2 전 런타임 경로이지만 쿠킹된 GLES31_PC 출력과 같은
// 모양으로 쓴다(ShaderSystem 7.2절 4항): 정점 입력은 ATTRIBUTEn location, 단계 사이 변수는
// TEXCOORDn location, 그리기 상수는 루트 상수 에뮬레이션 배열 vu_h(DrawConstants의 vec4 5개),
// 텍스처는 combined sampler ps0(유닛 0). 행렬은 행 우선 byte를 그대로 받아 mat4(열 우선)로 읽으므로
// Mᵀ * v = v·M이다(HLSL mul(v, transform)과 같다, 7.2절 6항).
constexpr char VertexGlsl[] = R"(#version 320 es
precision highp float;
precision highp int;
uniform highp vec4 vu_h[5];
#define transform mat4(vu_h[0], vu_h[1], vu_h[2], vu_h[3])
#define color vu_h[4]
layout(location = 0) in vec3 in_var_ATTRIBUTE0;
layout(location = 1) in vec2 in_var_ATTRIBUTE1;
layout(location = 2) in vec4 in_var_ATTRIBUTE2;
layout(location = 0) out vec2 out_var_TEXCOORD0;
layout(location = 1) out vec4 out_var_TEXCOORD1;
void main() {
    gl_Position = transform * vec4(in_var_ATTRIBUTE0, 1.0);
    out_var_TEXCOORD0 = in_var_ATTRIBUTE1;
    out_var_TEXCOORD1 = in_var_ATTRIBUTE2 * color;
}
)";

constexpr char PixelGlsl[] = R"(#version 320 es
precision highp float;
precision highp int;
layout(binding = 0) uniform highp sampler2D ps0;
layout(location = 0) in vec2 in_var_TEXCOORD0;
layout(location = 1) in vec4 in_var_TEXCOORD1;
layout(location = 0) out vec4 out_var_SV_Target0;
void main() {
    out_var_SV_Target0 = texture(ps0, in_var_TEXCOORD0) * in_var_TEXCOORD1;
}
)";

// 로컬 Y(오른쪽)-Z(위) 평면의 단위 크기 메시다. +X를 바라보면 화면에서 오른쪽이 +Y다. 삼각형마다
// 꼭짓점 A·B·C는 화면에서 반시계(앞면)이며 법선 (C−A)×(B−A)가 -X(카메라 쪽)다.
constexpr Vertex TriangleVertices[] = {
    {{0.0f, 0.0f, 0.5f}, {0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    {{0.0f, -0.5f, -0.5f}, {0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    {{0.0f, 0.5f, -0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
};

// 좌상단(-Y,+Z) UV (0,0), 우하단(+Y,-Z) UV (1,1). 삼각형 두 개(좌상·좌하·우하, 좌상·우하·우상).
constexpr Vertex QuadVertices[] = {
    {{0.0f, -0.5f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, -0.5f, -0.5f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, 0.5f, -0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, -0.5f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, 0.5f, -0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, 0.5f, 0.5f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
};

template <usize Count>
Span<const uint8> Bytes(const Vertex (&vertices)[Count]) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(vertices), sizeof(vertices));
}

} // namespace

StringView BuiltinShaderHlsl() noexcept {
    return StringView(ShaderHlsl, sizeof(ShaderHlsl) - 1);
}

StringView BuiltinVertexGlsl() noexcept {
    return StringView(VertexGlsl, sizeof(VertexGlsl) - 1);
}

StringView BuiltinPixelGlsl() noexcept {
    return StringView(PixelGlsl, sizeof(PixelGlsl) - 1);
}

Renderer::~Renderer() {
    Shutdown();
}

Result<void> Renderer::Initialize(graphics::Device& device,
                                  graphics::PixelFormat colorFormat) noexcept {
    if (device_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    const graphics::VertexAttribute attributes[] = {
        {0, graphics::VertexFormat::Float3, 0, ROCKS_OFFSET_OF(Vertex, position)},
        {1, graphics::VertexFormat::Float2, 0, ROCKS_OFFSET_OF(Vertex, uv)},
        {2, graphics::VertexFormat::Float4, 0, ROCKS_OFFSET_OF(Vertex, color)},
    };
    const graphics::VertexStream streams[] = {{sizeof(Vertex), graphics::VertexStepRate::Vertex}};
    graphics::PipelineDesc pipeline;
    pipeline.vertex = {BuiltinShaderHlsl(), BuiltinVertexGlsl()};
    pipeline.pixel = {BuiltinShaderHlsl(), BuiltinPixelGlsl()};
    pipeline.layout = {attributes, streams};
    pipeline.colorFormats[0] = colorFormat;
    pipeline.blend[0] = graphics::AlphaBlend();
    pipeline.raster.cullMode = graphics::CullMode::Back;
    // 파이프라인(셰이더 컴파일 포함)의 실패를 결정하는 곳이 여기다. 장치는 원인을 LastError에만
    // 남긴다(셰이더 컴파일 실패면 "Shader <entry> (<target>) failed to compile ...").
    const auto logPipelineFailure = [&device](const char* variant, ErrorCode error) noexcept {
        Log::Error(graphics::LogShader,
                   "Sample renderer {Variant} pipeline could not be built on {Backend}: {Error}: "
                   "{Diagnostics}",
                   variant, graphics::BackendName(device.Info().backend), ErrorCodeName(error),
                   device.LastError().LeftBytesAtCodePoint(graphics::MaxShaderDiagnosticLogBytes));
    };
    auto createdPipeline = device.CreatePipeline(pipeline);
    if (!createdPipeline) {
        logPipelineFailure("back-face culling", createdPipeline.Error());
        return Result<void>::Failure(createdPipeline.Error());
    }
    pipeline.raster.cullMode = graphics::CullMode::Front;
    auto reversedPipeline = device.CreatePipeline(pipeline);
    if (!reversedPipeline) {
        logPipelineFailure("front-face culling", reversedPipeline.Error());
        device.Retire(createdPipeline.Value());
        return Result<void>::Failure(reversedPipeline.Error());
    }

    graphics::BufferDesc triangleDesc;
    triangleDesc.size = sizeof(TriangleVertices);
    auto triangle = device.CreateBuffer(triangleDesc, Bytes(TriangleVertices));
    graphics::BufferDesc quadDesc;
    quadDesc.size = sizeof(QuadVertices);
    auto quad = device.CreateBuffer(quadDesc, Bytes(QuadVertices));
    if (!triangle || !quad) {
        device.Retire(createdPipeline.Value());
        device.Retire(reversedPipeline.Value());
        if (triangle) {
            device.Retire(triangle.Value());
        }
        if (quad) {
            device.Retire(quad.Value());
        }
        return Result<void>::Failure(!triangle ? triangle.Error() : quad.Error());
    }

    device_ = &device;
    pipeline_ = createdPipeline.Value();
    reversedPipeline_ = reversedPipeline.Value();
    triangle_ = triangle.Value();
    quad_ = quad.Value();
    return Result<void>::Success();
}

void Renderer::Shutdown() noexcept {
    if (device_ == nullptr) {
        return;
    }
    device_->Retire(pipeline_);
    device_->Retire(reversedPipeline_);
    device_->Retire(triangle_);
    device_->Retire(quad_);
    device_ = nullptr;
    pipeline_ = {};
    reversedPipeline_ = {};
    triangle_ = {};
    quad_ = {};
}

void Renderer::Draw(graphics::CommandContext& context, const RenderView& view,
                    Span<const RenderItem> items) noexcept {
    stats_ = {};
    if (device_ == nullptr) {
        return;
    }

    // 거울 view는 모든 항목의 감김을 뒤집는다.
    const bool reversedView = view.worldToView.Determinant() < 0.0f;
    graphics::PipelineHandle bound;
    for (const RenderItem& item : items) {
        // 음수 scale(행렬식 < 0)도 감김을 뒤집는다. view와 함께 뒤집히면 원래대로다.
        const bool reversed = (item.world.Determinant() < 0.0f) != reversedView;
        const graphics::PipelineHandle wanted = reversed ? reversedPipeline_ : pipeline_;
        if (bound != wanted) {
            context.SetPipeline(wanted);
            bound = wanted;
        }

        // 행 벡터 규약(UE LocalToWorld * WorldToClip): 월드 변환 먼저, view·투영 나중. 행 우선
        // 저장 그대로 올리고 셰이더가 mul(v, transform)으로 쓴다(DrawConstants).
        const Mat4 clip = item.world * view.viewProjection;
        graphics::DrawConstants constants;
        for (uint32 index = 0; index < 16; ++index) {
            constants.transform[index] = clip.m[index];
        }
        constants.color[0] = item.color.r;
        constants.color[1] = item.color.g;
        constants.color[2] = item.color.b;
        constants.color[3] = item.color.a;
        context.SetConstants(constants);
        context.SetTexture(item.texture);
        const bool triangle = item.mesh == MeshKind::Triangle;
        const graphics::VertexBufferBinding binding{triangle ? triangle_ : quad_, 0};
        context.SetVertexBuffers(0, Span<const graphics::VertexBufferBinding>(&binding, 1));
        context.Draw(triangle ? 3u : 6u);
        ++stats_.items;
        ++stats_.draws;
        stats_.reversedCulling += reversed ? 1u : 0u;
    }
}

} // namespace sandbox::sample
