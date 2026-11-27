#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Platform/Sync.h>
#include <RenderBase/RenderLane.h>
#include <RenderGraph/GraphPassAccesses.h>
#include <RenderGraph/GraphResourcePool.h>
#include <RenderGraph/RenderGraph.h>
#include <UiRender/UiFramePacket.h>

namespace rocks::ui {

class UiScenePassContext;

} // namespace rocks::ui

namespace rocks::render {

class EngineRenderResources;
class ShaderSet;

} // namespace rocks::render

namespace rocks::ui {

// UI 프레임을 RenderGraph pass로 기록하는 Render lane 쪽 렌더러다(Docs/Design/UiToolkit 18.4절,
// UE FSlateRHIRenderer의 render thread 부분). 렌더 lane 처리기가 소유하고, 장치는 그 lane에서만
// 만진다. 게임 lane 쪽 짝은 LaneUiRenderer다.
//
// - 그리기 대상마다 그래프 하나를 만들어 실행한다(Render). 창 pass의 EndFrame이 그 창을
// present하고,
//   한 대상의 실패는 다른 대상을 막지 않는다.
// - 대상 하나 안의 순서: 표면·장면 pass(packet 순서 — 안쪽부터) → 대상 pass.
// - 다른 처리기(Sandbox ScreenRenderer 등)는 AddPasses로 자기 그래프에 UI pass를 더한다.
//
// 사용 예(UI 전용 처리기):
//   Result<void> OnLaneStart(render::RenderLaneContext& context) noexcept override {
//       ...엔진 셰이더 집합을 연다...
//       return ui.Open(context.Device(), *resources.Shaders(), PixelFormat::RGBA8UnormSrgb);
//   }
//   void OnLaneStop(render::RenderLaneContext& context) noexcept override {
//       ui.Close(context.Device());
//   }
//   // 프레임 함수는 LaneUiRenderer가 게시한다(packet.Renderer()->Render(context, packet)).
class ROCKS_UIRENDER_API UiLaneRenderer {
public:
    struct Stats {
        uint64 graphsExecuted = 0;
        uint64 targetsFailed = 0;
        uint64 passesRecorded = 0;
        uint64 commandsSkipped = 0;
    };

    explicit UiLaneRenderer(Allocator& allocator) noexcept;
    ~UiLaneRenderer() noexcept;

    UiLaneRenderer(const UiLaneRenderer&) = delete;
    UiLaneRenderer& operator=(const UiLaneRenderer&) = delete;

    // 렌더 lane에서 연다: UI 파이프라인(format 색 대상), 텍스처 slot, 그래프 자원 풀. 닫히거나 다른
    // 집합으로 다시 만들 때까지 shaders에 lease를 잡는다(ShaderSetSlot — 교체로 Retire되어도 그
    // 사이 파괴되지 않는다). 실패: 셰이더 집합이 Ready가 아니거나 Retire되었으면 NotReady, UI
    // 셰이더가 없으면 NotFound, 장치 오류는 그 오류.
    Result<void> Open(graphics::Device& device, render::ShaderSet& shaders,
                      graphics::PixelFormat format) noexcept;
    // resources의 지금 집합으로 연다. 프레임마다(Render 맨 앞) resources.ApplyShaderUpdates를 불러
    // 감시 재컴파일의 셰이더 교체를 받고, 바뀌면 UI 파이프라인을 새 집합으로 다시
    // 만든다(ShaderSystem 16.3절 SSD76 — Docs/Design/UiToolkit 18.4절). 다시 만들지 못하면 옛
    // 파이프라인으로 계속 그리고 그 집합은 다음 교체까지 다시 시도하지 않는다. resources는 이
    // 객체가 닫힐 때까지 산다. 교체를 스스로 받는 처리기(자기 그래프에 AddPasses로 UI를 더하는
    // 경우)는 ShaderSet 판 Open과 ReloadShaders를 쓴다.
    Result<void> Open(graphics::Device& device, render::EngineRenderResources& resources,
                      graphics::PixelFormat format) noexcept;
    // shaders로 UI 파이프라인을 다시 만든다(셰이더 교체 뒤). 새 집합에 lease를 잡고 새 파이프라인을
    // 모두 만든 뒤에만 옛 것을 Retire하고 옛 집합의 lease를 놓는다 — 실패하면 옛 파이프라인·집합·
    // lease를 그대로 둔다. 같은 집합이면 아무 일도 없다. 실패: 닫혀 있거나 shaders가 Ready가
    // 아니거나 Retire되었으면 NotReady, 그 밖은 Open과 같다.
    Result<void> ReloadShaders(graphics::Device& device, render::ShaderSet& shaders) noexcept;
    // 렌더 lane에서 닫는다(파이프라인·배경 텍스처·풀·남은 창 표면을 Retire한다).
    void Close(graphics::Device& device) noexcept;
    bool IsOpen() const noexcept;

    // packet의 대상마다 그래프 하나를 만들어 실행한다(기본 프레임 함수).
    void Render(render::RenderFrameContext& context, UiFramePacket& packet) noexcept;

    // target의 표면·장면 pass와 대상 pass를 graph에 더한다. 창 대상은 스왑 체인 pass(그 EndFrame이
    // present)다. 실패: 모르는 창 표면 NotFound, 그래프 선언 오류는 그 오류.
    Result<void> AddPasses(render::RenderGraph& graph, render::RenderLaneContext& context,
                           UiFrameTarget& target) noexcept;

    // UI 묶음(텍스처 없는 명령 — 단색·둥근 상자)을 장면 대상에 이어 그리는 pass를 더한다(장면 기록
    // 함수가 표식·배경을 UI 셰이더로 그릴 때). 첫 pass면 장면 clear 색으로 지운다.
    Result<void> AddBatchToScene(UiScenePassContext& scene, const UiFrameBatch& batch) noexcept;

    // --- 창 표면(렌더 lane — LaneUiRenderer의 표면 작업이 부른다) ---

    Result<void> CreateWindowSurface(UiWindowSurfaceId id, const graphics::SwapChainDesc& desc,
                                     graphics::Device& device) noexcept;
    Result<void> ResizeWindowSurface(UiWindowSurfaceId id, uint32 width, uint32 height,
                                     graphics::Device& device) noexcept;
    void DestroyWindowSurface(UiWindowSurfaceId id, graphics::Device& device) noexcept;

    // 표면 작업의 결과다(어느 스레드에서나). 게임 lane은 PostSurfaceAndWait 뒤에 읽는다. 읽으면
    // 지운다. 작업이 없었으면 None이다.
    ErrorCode TakeSurfaceResult(UiWindowSurfaceId id) noexcept;
    // 표면 작업을 게시하기 전에 결과 칸을 NotReady로 예약한다(게임 lane). 작업이 돌지 않았으면
    // TakeSurfaceResult가 NotReady다.
    Result<void> ReserveSurfaceResult(UiWindowSurfaceId id) noexcept;
    void StoreSurfaceResult(UiWindowSurfaceId id, ErrorCode error) noexcept;

    // --- 화면 읽기(렌더 lane — LaneUiRenderer의 읽기 작업이 부른다) ---

    // texture(렌더 타깃) 내용을 읽어 capture 번호로 보관한다. 장치 ReadPixels는 그때까지 제출한
    // 그리기의 GPU 완료를 기다린다. 실패도 번호로 보관한다.
    void Capture(uint64 capture, render::RenderHandle texture, uint32 width, uint32 height,
                 render::RenderLaneContext& context) noexcept;
    // 보관한 결과를 가져간다(어느 스레드에서나). 아직 없으면 NotReady, 읽기가 실패했으면 그 오류다.
    Result<void> TakeCapture(uint64 capture, Array<uint8>& rgba) noexcept;
    // 읽기 결과를 capture 번호로 보관한다(렌더 lane — 다른 lane 작업이 읽은 화소를 게임 lane에
    // 돌려줄 때). error가 None이 아니면 실패로 보관한다.
    void StoreCapture(uint64 capture, Array<uint8>&& rgba, ErrorCode error) noexcept;

    // UI 파이프라인을 만든 셰이더 집합이다(lease를 잡고 있다 — 닫혀 있으면 null). 렌더 lane에서만
    // 쓴다.
    render::ShaderSet* Shaders() const noexcept;
    Allocator& GetAllocator() const noexcept;

    // 렌더 lane에서 읽는다(시험은 lane이 멈춘 뒤 읽는다).
    const Stats& Statistics() const noexcept;

private:
    struct Pipelines;

    // shaders로 UI 파이프라인·텍스처 slot을 pipelines에 만든다. 실패하면 만든 것을 Retire한다.
    Result<void> BuildPipelines(graphics::Device& device, render::ShaderSet& shaders,
                                graphics::PixelFormat format, Pipelines& pipelines) noexcept;

    struct WindowSurface {
        UiWindowSurfaceId id{};
        graphics::SwapChainHandle swapChain{};
        uint32 width = 0;
        uint32 height = 0;
    };

    struct SurfaceResult {
        UiWindowSurfaceId id{};
        ErrorCode error = ErrorCode::None;
    };

    struct CaptureResult {
        uint64 id = 0;
        ErrorCode error = ErrorCode::None;
        Array<uint8> rgba;
    };

    Result<void> AddBatchPass(render::RenderGraph& graph, render::RenderLaneContext& context,
                              const UiFrameBatch& batch, uint32 width, uint32 height,
                              render::GraphPassAccesses& accesses,
                              Span<const render::RenderHandle> producedTargets,
                              Span<const render::GraphTexture> producedTextures,
                              TraceScopeName& name) noexcept;
    Result<graphics::TextureHandle> PrepareBackdrop(graphics::Device& device, uint32 width,
                                                    uint32 height) noexcept;
    WindowSurface* FindWindow(UiWindowSurfaceId id) noexcept;

    Allocator& allocator_;
    render::ShaderSet* shaders_ = nullptr;
    // 다시 만들기에 실패한 집합이다(다음 교체까지 다시 시도하지 않는다 — 비교만 하고 쓰지 않는다).
    const render::ShaderSet* failedShaders_ = nullptr;
    // Open(resources)로 열었으면 프레임마다 셰이더 교체를 받는 원천이다.
    render::EngineRenderResources* resources_ = nullptr;
    graphics::PixelFormat format_ = graphics::PixelFormat::RGBA8UnormSrgb;
    UniquePtr<Pipelines> pipelines_;
    UniquePtr<render::GraphResourcePool> pool_;
    Array<WindowSurface> windows_;
    platform::Mutex resultGate_;
    Array<SurfaceResult> results_;
    Array<CaptureResult> captures_;
    Stats stats_;
    // AddPasses마다 올리는 그래프 세대다(배경 텍스처 cache가 같은 그래프의 칸을 지킨다).
    uint64 graphGeneration_ = 0;
};

} // namespace rocks::ui
