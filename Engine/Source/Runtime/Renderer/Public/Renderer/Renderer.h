#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/Color.h>
#include <Core/Math/Matrix4.h>
#include <Graphics/Device.h>

// 최소 렌더러다. 내장 메시(삼각형·사각형)와 파이프라인 하나(텍스처 × 정점색 × 항목색, 일반 알파
// 블렌드)로 RenderItem 목록을 그린다. Deferred/Forward 경로·깊이·조명·RenderGraph는 후속 범위다.
namespace rocks::render {

// 내장 메시다. 둘 다 원점 중심 단위 크기이며 로컬 Y(왼쪽)-Z(위) 평면에 놓인다(Rocks 좌표: +X 전방,
// +Y 왼쪽, +Z 위). 카메라가 +X를 바라보면 정면으로 보인다.
enum class MeshKind : uint8 {
    // 꼭짓점 색(빨강·초록·파랑)을 가진 삼각형이다. 위 꼭짓점 (0, 0, 0.5).
    Triangle,
    // 텍스처 좌표를 가진 사각형이다. UV (0,0)이 화면 좌상단(로컬 +Y, +Z 모서리)이다.
    Quad
};

// 그릴 항목 하나다.
struct RenderItem {
    // 로컬 → 월드 변환(열 우선 column vector)이다.
    Mat4 world;
    // 항목 색(선형)이다. 텍스처·꼭짓점 색에 곱한다.
    LinearColor color = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    // null이면 흰색 텍스처를 쓴다.
    graphics::TextureHandle texture;
    MeshKind mesh = MeshKind::Quad;
};

// 카메라와 화면 설정이다.
struct RenderView {
    // 월드 → clip 변환(projection × view)이다.
    Mat4 viewProjection;
    graphics::ClearColor clear;
};

struct RenderStats {
    uint32 items = 0;
    uint32 draws = 0;
};

// 장치 자원(메시 버퍼·파이프라인)을 소유하고 RenderItem 목록을 그린다.
//
// 사용 예:
//   render::Renderer renderer;
//   if (!renderer.Initialize(device, graphics::Format::RGBA8UnormSrgb)) { ... }
//   auto frame = device.BeginFrame(swapChain, view.clear);
//   if (frame) {
//       renderer.Draw(*frame.Value(), view, items);
//       auto presented = device.EndFrame();
//   }
//   renderer.Shutdown();                     // 장치보다 먼저
//
// 장치는 Renderer보다 오래 살아야 한다. 생성한 스레드에서만 사용한다.
class Renderer {
public:
    Renderer() noexcept = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    // Shutdown하지 않았으면 자원을 정리한다.
    ~Renderer();

    // 내장 메시와 파이프라인을 만든다. 이미 초기화됐으면 AlreadyExists, 셰이더·자원 생성 실패는 그
    // 오류다 (장치 LastError에 진단 문자열).
    Result<void> Initialize(graphics::Device& device, graphics::Format colorFormat) noexcept;

    // 장치 자원을 해제한다. 여러 번 호출해도 안전하다.
    void Shutdown() noexcept;

    // 기록 중인 프레임에 items를 순서대로 그린다(뒤에 오는 항목이 위에 그려진다).
    void Draw(graphics::CommandContext& context, const RenderView& view,
              Span<const RenderItem> items) noexcept;

    RenderStats LastStats() const noexcept {
        return stats_;
    }

    bool IsInitialized() const noexcept {
        return device_ != nullptr;
    }

private:
    graphics::Device* device_ = nullptr;
    graphics::PipelineHandle pipeline_;
    graphics::BufferHandle triangle_;
    graphics::BufferHandle quad_;
    RenderStats stats_;
};

// "Rocks 정점 형식"(위치 float3, UV float2, 색 float4)의 파이프라인 셰이더다. 사용자 파이프라인의
// 시작점으로 쓸 수 있다.
StringView BuiltinShaderHlsl() noexcept;
StringView BuiltinVertexGlsl() noexcept;
StringView BuiltinPixelGlsl() noexcept;

} // namespace rocks::render
