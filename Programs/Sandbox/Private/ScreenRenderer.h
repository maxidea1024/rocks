#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/UniquePtr.h>
#include <Graphics/Device.h>
#include <GraphicsTypes/ChannelSwizzle.h>
#include <RenderBase/BuiltinResources.h>
#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/RenderLane.h>
#include <RenderBase/RenderTransfer.h>
#include <RenderGraph/GraphResourcePool.h>
#include <RenderGraph/RenderGraph.h>

// Sandbox 엔진 창 렌더 경로의 렌더 lane 쪽이다(Docs/Design/RenderBase 18.5절 1항 — SM1). 엔진 층이
// 아니라 실행 파일의 화면 마일스톤 장면 그리기다. 정식 장면 렌더러는 RS0 Renderer다.
//
// 프레임 그래프: SceneColor(RGBA16Float)·SceneDepth(D32Float, reversed-Z clear 0) → BasePass(내장
// 메시를 fallback 메시 셰이더로) → Tonemap(전체화면, 스왑체인) → (요청하면) TonemapCapture(capture
// 텍스처). 텍스처 표시 프레임은 장면 대신 TextureInspector 한 pass다(텍스처는 게임 lane이 올린
// RenderHandle — 18.5절 7항).
//
// 사용 예(게임 lane):
//   screen::ScreenRenderer renderer(allocator, shaders::Host());
//   renderer.SetInitialSurface(surface, width, height);
//   config.processor = &renderer;
//   lane.Start(config);
//   ... lane.SubmitFrame(Move(ticket), screen::ScreenPacket{...}, &screen::RenderScreen);
//   ... lane.PostSurface(screen::SurfaceResize{w, h}, &screen::ResizeSurface);
namespace sandbox::screen {

using namespace rocks;

// 그릴 항목 하나다(게임 lane이 카메라 상대 local → clip을 계산해 담는다 — 18.5절 2항).
struct ScreenItem {
    render::BuiltinMeshConstants constants;
    render::BuiltinMesh mesh = render::BuiltinMesh::Cube;
    // 렌더 handle 텍스처다(null이면 builtin). Pending·Failed면 typed 대체로 그린다(6.2절).
    render::RenderHandle texture;
    render::BuiltinTexture builtin = render::BuiltinTexture::White2D;
    // 앞면 컬로 그린다(물체·view 행렬식 부호가 다를 때 — UE bReverseCulling).
    bool reverseCull = false;
};

// 엔진 절차 텍스처다(Docs/Design/RenderBase 18.5절 6항 — SM6). 게임 lane(ScreenHost)이 메모리
// TexturePlatformData로 만들어 쿠킹 텍스처와 같은 업로드 경로로 올리고(18.5절 7항), 표시 목록의 앞
// 여섯 칸이 된다. None은 장면이다.
enum class InspectorView : uint8 {
    None,
    // 2D 4×4 RGBA8Unorm, mip 셋(0 빨강·초록 바둑판, 1 파랑, 2 흰색)
    Texture2D,
    // 2D 1×1 RGBA16Float (2, 1, 0.5) — HDR 표시(filmic)
    Hdr2D,
    // 2D 배열 1×1, 층 셋(빨강·초록·파랑)
    Texture2DArray,
    // cube 1×1, 면 여섯(+X 빨강, -X 청록, +Y 초록, -Y 자홍, +Z 파랑, -Z 노랑)
    Cube,
    // cube 배열 1×1, cube 둘(0은 Cube와 같은 색, 1은 흰색) — DeviceFeatures::textureCubeArray
    CubeArray,
    // 3D 1×1×4(z 0 빨강, 1 초록, 2 파랑, 3 흰색)
    Volume
};

inline constexpr uint32 InspectorViewCount = 7;
// HDR 표시 텍스처의 값이다(장면 가운데 cube와 같은 색 — self-test가 같은 CPU 기준을 쓴다).
inline constexpr float InspectorHdrColor[3] = {2.0f, 1.0f, 0.5f};

// lane이 남길 수 있는 capture 자리 수다(self-test가 프레임마다 다른 표시를 읽는다).
inline constexpr uint32 MaxCaptureSlots = 64;

// 표시하는 텍스처의 셰이더 성분 계열이다(PixelFormatInfo::shaderType — 정수 형식은 Load 엔트리).
enum class InspectorComponent : uint8 {
    Float,
    Uint,
    Int
};

// 표시 하나다(18.5절 7항). 게임 lane의 표시 목록 항목이 담고, 고른 것을 프레임 packet에 넣는다.
struct InspectorTexture {
    // 장치 텍스처의 렌더 handle이다(Pending·Failed면 typed 대체로 그린다 — 6.2절).
    render::RenderHandle handle;
    graphics::TextureDimension dimension = graphics::TextureDimension::Texture2D;
    InspectorComponent component = InspectorComponent::Float;
    render::TextureInspectorDisplay display = render::TextureInspectorDisplay::Ldr;
    // 쿠킹 swizzle(셰이더가 적용 — view swizzle은 항등)이다.
    graphics::ChannelSwizzle swizzle;
    // 정수 형식의 값 → 표시 배율이다.
    float integerScale = 1.0f;
    // mip 0 크기다(depth는 volume만).
    uint32 width = 1;
    uint32 height = 1;
    uint32 depth = 1;
    // 배열 층 수(2D 배열)·cube 수(cube 배열), 그 밖은 1이다.
    uint32 layers = 1;
    uint32 mips = 1;
    // CPU Availability 텍스처다(올리지 않고 내장 검정 2D로 대체 — 6.5절).
    bool cpuPlaceholder = false;
};

// 고른 mip에서 고를 수 있는 층 수다(배열 층·cube 배열 원소·volume z texel, 그 밖 1).
uint32 InspectorLayerCount(const InspectorTexture& texture, uint32 mip) noexcept;

// 프레임 packet이다. items는 그 slot의 프레임 arena에 있다.
struct ScreenPacket {
    Span<const ScreenItem> items;
    graphics::ClearColor clear;
    render::TonemapConstants tonemap;
    // null이 아니면 tone map(또는 텍스처 표시) 결과를 이 텍스처(RGBA8UnormSrgb RenderTarget,
    // 스왑체인과 같은 크기)에도 그리고 lane이 captureSlot 자리에 읽어 둔다(self-test).
    render::RenderHandle capture;
    uint32 captureSlot = 0;
    // 참이면 장면 대신 텍스처 표시를 그린다(SM6·SM7). 층·mip은 게임 lane이 범위 안으로 맞춘다.
    bool inspecting = false;
    InspectorTexture inspector;
    uint32 inspectorLayer = 0;
    uint32 inspectorMip = 0;
};

// 표면 작업 값이다.
struct SurfaceCreate {
    graphics::NativeSurface surface;
    uint32 width = 0;
    uint32 height = 0;
};

struct SurfaceResize {
    uint32 width = 0;
    uint32 height = 0;
};

struct SurfaceRelease {
    uint32 reserved = 0;
};

// capture 텍스처 생성 작업 값이다.
struct CaptureCreate {
    render::RenderHandle handle;
    uint32 width = 0;
    uint32 height = 0;
};

} // namespace sandbox::screen

ROCKS_RENDER_TRANSFERABLE(sandbox::screen::ScreenPacket);
ROCKS_RENDER_TRANSFERABLE(sandbox::screen::SurfaceCreate);
ROCKS_RENDER_TRANSFERABLE(sandbox::screen::SurfaceResize);
ROCKS_RENDER_TRANSFERABLE(sandbox::screen::SurfaceRelease);
ROCKS_RENDER_TRANSFERABLE(sandbox::screen::CaptureCreate);

namespace sandbox::screen {

// 텍스처 표시 파이프라인 종류 수다(float 다섯 차원 + 정수 2D·2D 배열·volume × uint·int).
inline constexpr uint32 InspectorKindCount = 11;

// lane 쪽 통계다. lane이 끝난 뒤(Join) 게임 lane이 읽는다.
struct ScreenRendererStats {
    uint64 framesRendered = 0;
    // 스왑체인이 없어(표면 없음·소실) 건너뛴 프레임 수다.
    uint64 framesSkipped = 0;
    uint64 graphFailures = 0;
    uint64 captures = 0;
    uint64 resizes = 0;
    // 텍스처 표시로 그린 프레임 수다.
    uint64 inspectorFrames = 0;
    // 텍스처 표시가 준비되지 않은 handle을 대체(typed 대체·검정)로 그린 프레임 수다.
    uint64 inspectorFallbacks = 0;
};

// 렌더 lane 처리기다. 스왑체인·엔진 렌더 자원·파이프라인 캐시·그래프 풀을 lane에서 만들고 lane에서
// 정리한다. allocator와 host는 이 객체보다 오래 산다.
class ScreenRenderer final : public render::RenderLaneProcessor {
public:
    ScreenRenderer(Allocator& allocator, const render::EngineShaderHost& host) noexcept;
    ~ScreenRenderer() noexcept;
    ScreenRenderer(const ScreenRenderer&) = delete;
    ScreenRenderer& operator=(const ScreenRenderer&) = delete;

    // lane 시작 때 만들 스왑체인 표면과 크기다(Start 전에 부른다). 표면이 비면 스왑체인 없이
    // 시작한다.
    void SetInitialSurface(graphics::NativeSurface surface, uint32 width, uint32 height) noexcept;

    Result<void> OnLaneStart(render::RenderLaneContext& context) noexcept override;
    void OnLaneStop(render::RenderLaneContext& context) noexcept override;

    // lane이 끝난 뒤 게임 lane이 읽는다.
    const ScreenRendererStats& Stats() const noexcept {
        return stats_;
    }

    // capture 자리의 마지막 픽셀(RGBA8, 첫 행이 위)이다(비었으면 그 자리에 capture가 없었다).
    const Array<uint8>& CapturedPixels(uint32 slot = 0) const noexcept {
        return captured_[slot < captured_.Size() ? slot : 0];
    }

    uint32 CaptureWidth() const noexcept {
        return captureWidth_;
    }

    uint32 CaptureHeight() const noexcept {
        return captureHeight_;
    }

    // lane 시작이 엔진 렌더 자원을 열었는가(lane이 끝난 뒤 읽는다).
    bool OpenedResources() const noexcept {
        return opened_;
    }

    // 연 엔진 셰이더 집합이 쿠킹 라이브러리였는가와 그 셰이더 형식 id다(ShaderSystem 16.1절
    // SSD48 — lane이 끝난 뒤 읽는다).
    bool OpenedCookedShaders() const noexcept {
        return resources_.OpenedFromCookedLibrary();
    }

    StringView ShaderFormat() const noexcept {
        return resources_.FormatId();
    }

private:
    friend void RenderScreen(render::RenderFrameContext& context, ScreenPacket& packet) noexcept;
    friend void CreateSurface(render::RenderLaneContext& context, SurfaceCreate& value) noexcept;
    friend void ResizeSurface(render::RenderLaneContext& context, SurfaceResize& value) noexcept;
    friend void ReleaseSurface(render::RenderLaneContext& context, SurfaceRelease& value) noexcept;
    friend void CreateCapture(render::RenderLaneContext& context, CaptureCreate& value) noexcept;

    Result<void> CreateSwapChain(graphics::Device& device, graphics::NativeSurface surface,
                                 uint32 width, uint32 height) noexcept;
    // 메시 파이프라인(뒤면 컬 0, 앞면 컬 1)·tone map·텍스처 표시 파이프라인 요청을 준비한다.
    Result<void> PrepareRequests() noexcept;
    // 그래프를 실행하고 capture를 slot 자리에 읽는다. 그래프가 실패하면 false다.
    bool FinishFrame(render::RenderFrameContext& context, render::RenderGraph& graph,
                     graphics::TextureHandle capture, uint32 slot) noexcept;
    void RenderInspector(render::RenderFrameContext& context, const ScreenPacket& packet) noexcept;

    Allocator& allocator_;
    const render::EngineShaderHost& host_;
    graphics::NativeSurface surface_;
    uint32 width_ = 0;
    uint32 height_ = 0;
    graphics::SwapChainHandle swapChain_;
    render::EngineRenderResources resources_;
    UniquePtr<render::PipelineCache> cache_;
    UniquePtr<render::GraphResourcePool> pool_;
    render::GraphicsPipelineRequest meshRequests_[2];
    render::GraphicsPipelineRequest tonemapRequest_;
    // 차원·성분 계열마다의 텍스처 표시 요청이다(ScreenRenderer.cpp InspectorKinds 순서).
    render::GraphicsPipelineRequest inspectorRequests_[InspectorKindCount];
    // 요청마다 PS 텍스처 slot이다(값 1 위치 — Docs/Design/RenderBase 9.13절).
    uint32 meshTextureSlot_ = graphics::MaxResourceSlots;
    uint32 tonemapTextureSlot_ = graphics::MaxResourceSlots;
    uint32 inspectorTextureSlots_[InspectorKindCount] = {};
    Array<Array<uint8>> captured_;
    uint32 captureWidth_ = 0;
    uint32 captureHeight_ = 0;
    ScreenRendererStats stats_;
    bool opened_ = false;
};

// 프레임 packet 작업이다.
void RenderScreen(render::RenderFrameContext& context, ScreenPacket& packet) noexcept;

// 표면 작업이다(5.2절 — 스왑체인 생성·resize·소실).
void CreateSurface(render::RenderLaneContext& context, SurfaceCreate& value) noexcept;
void ResizeSurface(render::RenderLaneContext& context, SurfaceResize& value) noexcept;
// 스왑체인을 Retire하고 장치가 끝날 때까지 기다린다(Android 표면 소실 handshake).
void ReleaseSurface(render::RenderLaneContext& context, SurfaceRelease& value) noexcept;

// capture 텍스처를 만들어 handle에 잇는다(자원 작업).
void CreateCapture(render::RenderLaneContext& context, CaptureCreate& value) noexcept;

} // namespace sandbox::screen
