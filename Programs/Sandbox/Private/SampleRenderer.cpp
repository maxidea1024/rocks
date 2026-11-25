#include "SampleRenderer.h"
#include "SandboxShaders.h"

#include <Core/Diagnostics/Log.h>
#include <Graphics/GraphicsLog.h>
#include <RenderBase/PipelineStatePresets.h>

#include <Base/Compiler.h>

namespace sandbox::sample {

namespace {

struct Vertex {
    float position[3];
    float uv[2];
    float color[4];
};

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
    pipeline.bindings.texture = "Albedo";
    pipeline.layout = {attributes, streams};
    pipeline.drawConstantBytes = sizeof(ObjectConstants);
    pipeline.colorFormats[0] = colorFormat;
    pipeline.blend[0] = render::AlphaBlend();
    pipeline.raster.cullMode = graphics::CullMode::Back;
    // 파이프라인 실패를 결정하는 곳이 여기다. 장치는 원인을 LastError에만 남긴다(셰이더는
    // Sample.rsf의 쿠킹 코드 — ShaderSystem 16.1절 SSD19).
    const auto logPipelineFailure = [&device](const char* variant, ErrorCode error) noexcept {
        Log::Error(graphics::LogShader,
                   "Sample renderer {Variant} pipeline could not be built on {Backend}: {Error}: "
                   "{Diagnostics}",
                   variant, graphics::BackendName(device.Info().backend), ErrorCodeName(error),
                   device.LastError().LeftBytesAtCodePoint(graphics::MaxShaderDiagnosticLogBytes));
    };
    auto createdPipeline =
        shaders::CreateGraphicsPipeline(device, shaders::SampleVS, shaders::SamplePS, pipeline);
    if (!createdPipeline) {
        logPipelineFailure("back-face culling", createdPipeline.Error());
        return Result<void>::Failure(createdPipeline.Error());
    }
    pipeline.raster.cullMode = graphics::CullMode::Front;
    auto reversedPipeline =
        shaders::CreateGraphicsPipeline(device, shaders::SampleVS, shaders::SamplePS, pipeline);
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
        // 저장 그대로 올리고 셰이더가 mul(v, transform)으로 쓴다(ObjectConstants).
        const Mat4 clip = item.world * view.viewProjection;
        ObjectConstants constants;
        for (uint32 index = 0; index < 16; ++index) {
            constants.transform[index] = clip.m[index];
        }
        constants.color[0] = item.color.r;
        constants.color[1] = item.color.g;
        constants.color[2] = item.color.b;
        constants.color[3] = item.color.a;
        // 크기는 파이프라인 선언과 같으므로 실패하지 않는다.
        auto set = context.SetDrawConstants(graphics::DrawConstantBytes(constants));
        (void)set;
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
