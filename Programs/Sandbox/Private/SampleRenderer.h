#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/Color.h>
#include <Core/Math/Matrix4.h>
#include <Graphics/Device.h>

// Sandbox 시험용 샘플 그리기다. 내장 메시(삼각형·사각형)와 파이프라인 하나(텍스처 × 정점색 ×
// 항목색, 일반 알파 블렌드)로 RenderItem 목록을 그린다. 엔진 렌더러가 아니다: 원문 셰이더 문자열과
// 고정 파이프라인으로 Graphics backend·렌더 lane self-test를 그리는 데만 쓴다. 정식
// 렌더러(Forward+· Deferred)는 Engine/Source/Runtime/Renderer 모듈로 따로 둔다.
namespace sandbox::sample {

using namespace rocks;

// 내장 메시다. 둘 다 원점 중심 단위 크기이며 로컬 Y(오른쪽)-Z(위) 평면에 놓인다(UE 좌표: +X 전방,
// +Y 오른쪽, +Z 위). 앞면 법선은 -X다(렌더 메시 법선 (C−A)×(B−A), UE
// MeshDescription.cpp:1311–1313). 그래서 +X를 바라보는 카메라에는 화면에서 반시계로 감긴 앞면으로
// 보인다.
enum class MeshKind : uint8 {
    // 꼭짓점 색을 가진 삼각형이다. 위 꼭짓점 (0, 0, 0.5) 빨강, 왼쪽 아래 (0, -0.5, -0.5) 초록,
    // 오른쪽 아래 (0, 0.5, -0.5) 파랑.
    Triangle,
    // 텍스처 좌표를 가진 사각형이다. UV (0,0)이 화면 좌상단(로컬 -Y, +Z 모서리)이다.
    Quad
};

// 그릴 항목 하나다.
struct RenderItem {
    // 로컬 → 월드 변환(행 벡터 규약 p · M, 이동은 마지막 행 — Core Matrix4)이다.
    Mat4 world;
    // 항목 색(선형)이다. 텍스처·꼭짓점 색에 곱한다.
    LinearColor color = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    // null이면 흰색 텍스처를 쓴다.
    graphics::TextureHandle texture;
    MeshKind mesh = MeshKind::Quad;
};

// 카메라와 화면 설정이다.
struct RenderView {
    // 월드 → clip 변환(행 벡터 규약 worldToView × projection — 월드 → view 먼저)이다.
    Mat4 viewProjection;
    // viewProjection 앞부분의 월드 → view 변환이다. 컬 방향에만 쓴다: 행렬식이 음수(거울 view)면
    // 모든 항목의 컬을 뒤집는다(UE FSceneView::bReverseCulling, SceneView.cpp:1000). 기본은 단위
    // 행렬이다.
    Mat4 worldToView;
    graphics::ClearColor clear;
};

struct RenderStats {
    uint32 items = 0;
    uint32 draws = 0;
    // 컬을 뒤집어(앞면 컬 파이프라인으로) 그린 항목 수다.
    uint32 reversedCulling = 0;
};

// 장치 자원(메시 버퍼·파이프라인)을 소유하고 RenderItem 목록을 그린다.
//
// 사용 예:
//   sample::Renderer renderer;
//   if (!renderer.Initialize(device, graphics::Format::RGBA8UnormSrgb)) { ... }
//   auto frame = device.BeginFrame(swapChain, view.clear);
//   if (frame) {
//       renderer.Draw(*frame.Value(), view, items);
//       auto submitted = device.EndFrame();      // GpuFencePoint
//   }
//   renderer.Shutdown();                     // 장치보다 먼저
//
// 장치는 Renderer보다 오래 살아야 한다. 장치 소유 스레드에서만 사용한다. 렌더 lane(RenderBase)이
// 장치를 가지면 lane 쪽 객체(RenderLaneProcessor)가 Renderer를 소유하고 프레임 packet 작업 안에서
// 그린다.
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

    // 기록 중인 프레임에 items를 순서대로 그린다(뒤에 오는 항목이 위에 그려진다). 뒤면은 컬한다.
    // 항목 world 행렬식과 view.worldToView 행렬식 가운데 하나만 음수면 그 항목은 앞면을 컬한다(음수
    // scale·거울 view로 감김이 뒤집힌 메시가 그대로 보인다). 둘 다 음수면 원래대로다(UE
    // StaticMeshSceneProxy.cpp:1318–1327, MeshPassProcessor.cpp:1697).
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
    // 뒤면 컬(일반) 파이프라인과 앞면 컬(반전) 파이프라인이다.
    graphics::PipelineHandle pipeline_;
    graphics::PipelineHandle reversedPipeline_;
    graphics::BufferHandle triangle_;
    graphics::BufferHandle quad_;
    RenderStats stats_;
};

// "Rocks 정점 형식"(위치 float3, UV float2, 색 float4)의 파이프라인 셰이더다. 사용자 파이프라인의
// 시작점으로 쓸 수 있다.
StringView BuiltinShaderHlsl() noexcept;
StringView BuiltinVertexGlsl() noexcept;
StringView BuiltinPixelGlsl() noexcept;

} // namespace sandbox::sample
