#include <Renderer/Renderer.h>
#include <Base/Compiler.h>

namespace rocks::render {

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
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
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
    output.position = mul(transform, float4(input.position, 1.0));
    output.uv = input.uv;
    output.color = input.color * color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return albedo.Sample(linearClamp, input.uv) * input.color;
}
)";

constexpr char VertexGlsl[] = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform mat4 uTransform;
uniform vec4 uColor;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
void main() {
    gl_Position = uTransform * vec4(aPosition, 1.0);
    vUv = aUv;
    vColor = aColor * uColor;
}
)";

constexpr char PixelGlsl[] = R"(#version 450 core
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(binding = 0) uniform sampler2D uTexture;
layout(location = 0) out vec4 oColor;
void main() {
    oColor = texture(uTexture, vUv) * vColor;
}
)";

// 로컬 Y(왼쪽)-Z(위) 평면의 단위 크기 메시다. 화면에서 오른쪽은 -Y다.
constexpr Vertex TriangleVertices[] = {
    {{0.0f, 0.0f, 0.5f}, {0.5f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    {{0.0f, -0.5f, -0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
    {{0.0f, 0.5f, -0.5f}, {0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
};

// 좌상단(+Y,+Z) UV (0,0), 우하단(-Y,-Z) UV (1,1). 삼각형 두 개.
constexpr Vertex QuadVertices[] = {
    {{0.0f, 0.5f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, -0.5f, 0.5f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, -0.5f, -0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, 0.5f, 0.5f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, -0.5f, -0.5f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    {{0.0f, 0.5f, -0.5f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
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

Result<void> Renderer::Initialize(graphics::Device& device, graphics::Format colorFormat) noexcept {
    if (device_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    const graphics::VertexAttribute attributes[] = {
        {0, graphics::VertexFormat::Float3, ROCKS_OFFSET_OF(Vertex, position)},
        {1, graphics::VertexFormat::Float2, ROCKS_OFFSET_OF(Vertex, uv)},
        {2, graphics::VertexFormat::Float4, ROCKS_OFFSET_OF(Vertex, color)},
    };
    graphics::PipelineDesc pipeline;
    pipeline.vertex = {BuiltinShaderHlsl(), BuiltinVertexGlsl()};
    pipeline.pixel = {BuiltinShaderHlsl(), BuiltinPixelGlsl()};
    pipeline.attributes = attributes;
    pipeline.vertexStride = sizeof(Vertex);
    pipeline.colorFormat = colorFormat;
    pipeline.alphaBlend = true;
    auto createdPipeline = device.CreatePipeline(pipeline);
    if (!createdPipeline) {
        return Result<void>::Failure(createdPipeline.Error());
    }

    graphics::BufferDesc triangleDesc;
    triangleDesc.size = sizeof(TriangleVertices);
    auto triangle = device.CreateBuffer(triangleDesc, Bytes(TriangleVertices));
    graphics::BufferDesc quadDesc;
    quadDesc.size = sizeof(QuadVertices);
    auto quad = device.CreateBuffer(quadDesc, Bytes(QuadVertices));
    if (!triangle || !quad) {
        device.Destroy(createdPipeline.Value());
        if (triangle) {
            device.Destroy(triangle.Value());
        }
        if (quad) {
            device.Destroy(quad.Value());
        }
        return Result<void>::Failure(!triangle ? triangle.Error() : quad.Error());
    }

    device_ = &device;
    pipeline_ = createdPipeline.Value();
    triangle_ = triangle.Value();
    quad_ = quad.Value();
    return Result<void>::Success();
}

void Renderer::Shutdown() noexcept {
    if (device_ == nullptr) {
        return;
    }
    device_->Destroy(pipeline_);
    device_->Destroy(triangle_);
    device_->Destroy(quad_);
    device_ = nullptr;
    pipeline_ = {};
    triangle_ = {};
    quad_ = {};
}

void Renderer::Draw(graphics::CommandContext& context, const RenderView& view,
                    Span<const RenderItem> items) noexcept {
    stats_ = {};
    if (device_ == nullptr) {
        return;
    }

    context.SetPipeline(pipeline_);
    for (const RenderItem& item : items) {
        const Mat4 clip = view.viewProjection * item.world;
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
        if (item.mesh == MeshKind::Triangle) {
            context.SetVertexBuffer(triangle_);
            context.Draw(3);
        } else {
            context.SetVertexBuffer(quad_);
            context.Draw(6);
        }
        ++stats_.items;
        ++stats_.draws;
    }
}

} // namespace rocks::render
