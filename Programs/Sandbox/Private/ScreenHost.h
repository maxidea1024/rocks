#pragma once

#include "ScreenRenderer.h"

#include <Core/Containers/Array.h>
#include <Core/Math/Matrix4.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringView.h>
#include <Gameplay/PlayerController.h>
#include <Graphics/Device.h>
#include <Input/InputState.h>
#include <PlatformWindow/Window.h>
#include <RenderBase/RenderLane.h>

namespace rocks {

class GameInstanceClient;

} // namespace rocks

// Sandbox 엔진 창 렌더 경로의 게임 lane 쪽이다(Docs/Design/RenderBase 18.5절 1·2항 — SM1·SM2).
// 장치와 렌더 lane을 묶고, 엔진 프레임 끝에 첫 LocalPlayer의 카메라와 Sandbox 장면으로 프레임
// packet을 만들어 넘긴다. 엔진 종료 hook 다섯을 lane 종료 단계에 잇는다(5.5절).
//
// 사용 예(창 모드 엔진 호스트):
//   auto device = screen::CreateScreenDevice(allocator, screen::ScreenBackend::D3D12, false);
//   screen::ScreenHost host(allocator, *device.Value(), shaders::Host());
//   if (host.Start(window, render::RenderLaneMode::Thread)) { ... }
//   // EngineLoopHooks: EndFrame → host.SubmitFrame(client), BeginShutdown → host.BlockFrames() ...
//   // 엔진 Shutdown 뒤 host 파괴 → 장치 파괴 → 창 파괴
namespace sandbox::screen {

using namespace rocks;

// 장치 backend다(명령줄 --graphics=). Auto는 D3D12 → Vulkan → OpenGL ES 순으로 만들 수 있는 첫
// 장치다(이 플랫폼에 없는 backend는 생성이 Unsupported로 끝난다 — 플랫폼 분기를 두지 않는다).
enum class ScreenBackend : uint8 {
    Auto,
    D3D12,
    D3D11,
    Vulkan,
    OpenGL
};

// --graphics= 값(auto·d3d12·d3d11·vulkan·opengl)을 푼다. 모르는 이름이면 false다.
bool ParseScreenBackend(StringView text, ScreenBackend& backend) noexcept;

const char* ScreenBackendName(ScreenBackend backend) noexcept;

// 장치를 만든다(Auto는 위 순서로 시도). 실패는 Warning(생성 보고의 단계·원인)과 그 오류다.
Result<UniquePtr<graphics::Device>> CreateScreenDevice(Allocator& allocator, ScreenBackend backend,
                                                       bool validation) noexcept;

// order(쿠킹 매니페스트의 graphicsBackends 순서 — ShaderSystem 16.1절 SSD48)로 만들 수 있는 첫
// 장치를 만든다. Metal 등 이 실행 파일이 만들지 않는 backend는 건너뛴다. 모두 실패하면 마지막 오류
// (없으면 Unsupported)다.
Result<UniquePtr<graphics::Device>> CreateScreenDeviceInOrder(Allocator& allocator,
                                                              Span<const graphics::Backend> order,
                                                              bool validation) noexcept;

// 엔진 셰이더를 열 원천(쿠킹 라이브러리 원천이나 공급자)이 있는가. 없으면(Release·Android) 렌더
// 경로를 시작하지 않는다(18.5절 1항).
bool CanOpenEngineShaders(const render::EngineShaderHost& host) noexcept;

// 카메라에서 만든 화면 view다(18.5절 2항). worldToClip은 카메라 상대 world(카메라 위치를 뺀
// 좌표) → clip이다.
struct ScreenView {
    Vec3d origin;
    Mat4d relativeWorldToClip = Mat4d::Identity();
    // view 회전 행렬식이 음수인가(거울 view — 모든 컬을 뒤집는다).
    bool mirrored = false;
};

// near 평면 거리다(cm, UE BaseEngine.ini NearClipPlane 10 — 무한 원평면 reversed-Z).
inline constexpr double ScreenNearPlane = 10.0;

// 카메라 결과와 화면 크기로 view를 만든다. 실패: 크기 0·FOV 범위 밖 InvalidArgument.
Result<ScreenView> MakeScreenView(const CameraView& camera, uint32 width, uint32 height) noexcept;

// 첫 LocalPlayer의 CameraDirector 결과를 읽는다. 없으면 false다.
bool FindPlayerCamera(GameInstanceClient& client, CameraView& camera) noexcept;

// Sandbox 장면 항목 하나다(월드 cm, 회전 없음).
struct SceneObject {
    render::BuiltinMesh mesh = render::BuiltinMesh::Cube;
    Vec3d location;
    Vec3d scale = Vec3d(1.0, 1.0, 1.0);
    // 선형 HDR 색(장면 색에 그대로 곱한다)이다.
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    render::BuiltinTexture builtin = render::BuiltinTexture::White2D;
    // true면 호스트가 예약한 Pending 텍스처 handle로 그린다(typed 대체 경로 — 6.2절).
    bool pendingTexture = false;
};

// Sandbox 기본 장면이다(정적 수명): 바닥 plane과 HDR 색 cube들. 카메라 원점에서 +X로 본다.
Span<const SceneObject> SandboxScene() noexcept;

// 장면의 가운데 cube 색(화면 가운데 픽셀 — self-test 기준)이다.
inline constexpr float SceneCenterColor[3] = {2.0f, 1.0f, 0.5f};
// 장면의 오른쪽 cube 색이다(과노출 흰색).
inline constexpr float SceneRightColor[3] = {8.0f, 8.0f, 8.0f};
// 오른쪽 cube 위치(y, cm)와 장면 cube의 x 거리다.
inline constexpr double SceneCubeDistance = 500.0;
inline constexpr double SceneRightOffset = 250.0;
// 배경(clear) 색이다(선형 HDR).
inline constexpr float SceneClearColor[3] = {0.05f, 0.08f, 0.12f};

class ScreenHost {
public:
    // device·host는 이 객체보다 오래 산다.
    ScreenHost(Allocator& allocator, graphics::Device& device,
               const render::EngineShaderHost& host) noexcept;
    // 시작했으면 Shutdown을 부른다(종료 hook이 이미 닫았으면 아무 일도 하지 않는다).
    ~ScreenHost() noexcept;
    ScreenHost(const ScreenHost&) = delete;
    ScreenHost& operator=(const ScreenHost&) = delete;

    // 렌더 lane을 시작한다(창 표면으로 스왑체인을 만든다). 첫 프레임 slot 확보가 시작 결과를
    // 기다린다. pump는 slot 대기 중 창 메시지를 펌프하는 hook이다(5.3절 — 창 스레드가 게임 lane).
    // 실패: lane 시작 오류(RenderLane::Start).
    Result<void> Start(platform::Window& window, render::RenderLaneMode mode,
                       render::RenderLanePump pump) noexcept;

    bool IsRunning() const noexcept {
        return started_ && !failed_;
    }

    // 프레임 packet을 만들어 넘긴다(엔진 EndFrame). 카메라가 없으면 기본 view(원점에서 +X)다.
    // lane 시작·프레임 실패는 Error 한 번을 남기고 이후 프레임을 보내지 않는다.
    void SubmitFrame(GameInstanceClient* client) noexcept;

    // 창 크기 변경·표면 소실·새 표면이다(PumpEvents).
    void Resize(uint32 width, uint32 height) noexcept;
    void ReleaseSurface() noexcept;
    void RecreateSurface(platform::Window& window) noexcept;

    // 다음 SubmitFrame부터 tone map 결과를 capture 텍스처에도 그린다(self-test). 크기는 창 크기다.
    Result<void> RequestCapture(uint32 width, uint32 height) noexcept;

    // 텍스처 표시 view를 고른다(18.5절 6항). 층·mip은 그 텍스처 범위로 자른다.
    void SetInspector(InspectorView view, uint32 layer, uint32 mip) noexcept;

    InspectorView Inspector() const noexcept {
        return inspector_;
    }

    // 다음 프레임부터 capture를 남길 자리다(0–MaxCaptureSlots−1, self-test).
    void SetCaptureSlot(uint32 slot) noexcept {
        captureSlot_ = slot < MaxCaptureSlots ? slot : 0;
    }

    // 호스트 입력으로 표시 view를 바꾼다(숫자 0 장면, 1–6 view, `[`·`]`·패드 어깨 버튼 층,
    // `-`·`=` mip).
    void HandleInspectorInput(const input::InputState& input) noexcept;

    // 노출 배율이다(tone map, 기본 1).
    void SetExposureScale(float scale) noexcept {
        tonemap_.exposure[0] = scale;
    }

    // --- 종료(엔진 종료 hook, 5.5절) ---
    void BlockFrames() noexcept;
    Result<void> WaitFramesIdle() noexcept;
    Result<uint64> DrainRetires() noexcept;
    Result<void> CloseResources() noexcept;
    Result<void> Stop() noexcept;

    render::RenderLane& Lane() noexcept {
        return lane_;
    }

    const ScreenRenderer& Renderer() const noexcept {
        return renderer_;
    }

    uint64 FramesSubmitted() const noexcept {
        return submitted_;
    }

private:
    graphics::Device& device_;
    ScreenRenderer renderer_;
    render::RenderLane lane_;
    platform::Window* window_ = nullptr;
    render::TonemapConstants tonemap_;
    render::RenderHandle pending_;
    render::RenderHandle capture_;
    InspectorView inspector_ = InspectorView::None;
    uint32 inspectorLayer_ = 0;
    uint32 inspectorMip_ = 0;
    uint32 captureSlot_ = 0;
    uint32 width_ = 0;
    uint32 height_ = 0;
    uint64 submitted_ = 0;
    bool started_ = false;
    bool failed_ = false;
    bool surfaceLost_ = false;
    bool pendingRetired_ = false;
};

} // namespace sandbox::screen
