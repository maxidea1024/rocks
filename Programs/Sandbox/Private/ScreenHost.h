#pragma once

#include "ScreenRenderer.h"

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Matrix4.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringView.h>
#include <Gameplay/PlayerController.h>
#include <Graphics/Device.h>
#include <Input/InputState.h>
#include <PlatformWindow/Window.h>
#include <RenderBase/RenderLane.h>
#include <WorldIntegration/Textures/PlatformTextureSource.h>
#include <WorldIntegration/Textures/TextureRenderCache.h>

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

// 표시 목록 항목 이름의 byte 상한이다(넘으면 자른다).
inline constexpr usize MaxInspectorNameBytes = 96;

// 표시 목록 항목의 업로드 상태다(렌더 lane 통지로 바뀐다).
enum class InspectorEntryState : uint8 {
    // 업로드를 게시했고 통지를 기다린다(그리면 typed 대체다).
    Pending,
    Ready,
    // 장치 생성이 실패했다(ResourceFailed).
    Failed,
    // CPU Availability — 올리지 않고 검정으로 대체한다.
    Placeholder
};

// 표시 목록 항목 하나다(18.5절 7항).
struct InspectorEntry {
    char name[MaxInspectorNameBytes] = {};
    InspectorTexture texture;
    InspectorEntryState state = InspectorEntryState::Pending;
    // 실패면 그 오류다.
    ErrorCode error = ErrorCode::None;
    // 텍스처 렌더 캐시의 handle이다(캐시가 Retire한다 — ScreenHost는 놓지 않는다, 18.5절 8항).
    bool cacheOwned = false;
};

// 형식의 표시 방식·성분 계열·정수 배율이다(float는 HDR이면 노출 + filmic, unorm·snorm은 그대로,
// 정수는 형식 최대값으로 나눈다 — R8Uint 1/255).
void DescribeInspectorFormat(graphics::PixelFormat format, InspectorTexture& out) noexcept;

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

    // 엔진 절차 텍스처 표시를 고른다(18.5절 6항 — None은 장면). 층·mip은 그 텍스처 범위로
    // 자른다. 장치가 못 만든 텍스처(cube 배열 능력 없음)면 장면이다.
    void SetInspector(InspectorView view, uint32 layer, uint32 mip) noexcept;

    // 표시 목록 항목을 고른다(index가 범위 밖이면 장면). 층·mip은 그 텍스처 범위로 자른다.
    void SetInspectorEntry(uint32 index, uint32 layer, uint32 mip) noexcept;

    // 고른 항목 index다(장면이면 NoInspectorEntry).
    uint32 SelectedEntry() const noexcept {
        return selected_;
    }

    static constexpr uint32 NoInspectorEntry = NumericLimits<uint32>::Max();

    // 쿠킹 텍스처 데이터를 올리고 표시 목록 끝에 더한다(18.5절 7항 — 패키지 공개 뒤 게임 lane).
    // cpuPlaceholder면 올리지 않고 검정 대체 항목만 더한다(CPU Availability). 오류: lane 시작 전
    // NotReady, 변환(MakePlatformTextureSource)·검사(CheckCookedTexture)의 오류 — issue가 원인,
    // handle 예약·게시 실패. 실패하면 목록을 바꾸지 않는다.
    Result<uint32> AddTexture(StringView name, const TexturePlatformData& data,
                              PlatformBulkReader reader, void* context, bool cpuPlaceholder,
                              render::CookedTextureIssue* issue = nullptr) noexcept;

    // 텍스처 렌더 캐시 항목을 표시 목록 끝에 더한다(18.5절 8항 — 쿠킹 텍스처 에셋). handle은 캐시
    // 것이라 ScreenHost가 Retire하지 않는다. 상태는 info.state에서 온다(Uploading → Pending, Ready,
    // handle 있는 Failed, CpuPlaceholder → 검정 대체). 오류: lane 시작 전 NotReady, Loading·handle
    // 없는 Failed InvalidArgument, 할당 실패.
    Result<uint32> AddCachedTexture(StringView name, const TextureRenderInfo& info) noexcept;

    // 렌더 통지를 함께 받을 텍스처 렌더 캐시를 붙인다(null이면 뗀다). ProcessNotices가 거둔 통지를
    // 캐시에도 넘긴다(통지 큐 소비자는 하나 — RenderBase 5.2절). cache는 붙어 있는 동안 산다.
    void AttachTextureCache(TextureRenderCache* cache) noexcept {
        cache_ = cache;
    }

    // 검사를 거치지 않은 업로드 값을 그대로 게시하고 항목을 더한다(self-test 음성 — 장치 생성
    // 실패의 ResourceFailed 경로). upload.handle은 이 함수가 예약해 채운다.
    Result<uint32> AddUnchecked(StringView name, render::CookedTextureUpload&& upload,
                                const InspectorTexture& texture) noexcept;

    uint32 InspectorEntryCount() const noexcept {
        return static_cast<uint32>(inspectors_.Size());
    }

    const InspectorEntry& InspectorEntryAt(uint32 index) const noexcept {
        return inspectors_[index];
    }

    // 엔진 절차 텍스처의 항목 index다(만들지 않았으면 NoInspectorEntry).
    uint32 EngineEntry(InspectorView view) const noexcept;

    // 아직 통지를 기다리는 업로드 수다(ProcessNotices가 줄인다).
    uint32 PendingUploads() const noexcept;

    // 렌더 lane 통지를 거둬 항목 상태를 바꾼다(SubmitFrame이 프레임마다 부른다).
    void ProcessNotices() noexcept;

    // 다음 프레임부터 capture를 남길 자리다(0–MaxCaptureSlots−1, self-test).
    void SetCaptureSlot(uint32 slot) noexcept {
        captureSlot_ = slot < MaxCaptureSlots ? slot : 0;
    }

    // 호스트 입력으로 표시를 바꾼다(숫자 0 장면, 1–6 엔진 절차 텍스처, `,`·`.`·패드 D-pad 좌우
    // 목록 이전·다음, `[`·`]`·패드 어깨 버튼 층, `-`·`=` mip).
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

    // 장치다(업로드 검사의 능력 값 — Start 뒤 렌더 lane이 소유하므로 게임 lane은 능력 질의만 한다).
    const graphics::Device& Device() const noexcept {
        return device_;
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
    // 엔진 절차 텍스처를 만들어 목록 앞에 둔다(Start).
    void AddEngineTextures() noexcept;
    Result<uint32> AppendEntry(StringView name, const InspectorTexture& texture,
                               InspectorEntryState state) noexcept;

    Allocator& allocator_;
    TextureRenderCache* cache_ = nullptr;
    render::RenderHandle pending_;
    render::RenderHandle capture_;
    Array<InspectorEntry> inspectors_;
    Array<render::RenderNotice> notices_;
    uint32 engineEntries_[InspectorViewCount];
    uint32 selected_ = NoInspectorEntry;
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
    bool inspectorsRetired_ = false;
};

} // namespace sandbox::screen
