#pragma once

#include <Core/Memory/FrameArena.h>
#include <RenderGraph/GraphTypes.h>

namespace rocks::render {

class RenderGraph;

// pass 렌더 타깃 종류다.
enum class GraphTargetKind : uint8 {
    None,
    // 그래프 텍스처 attachment(색 0–8, 깊이 하나, 장치 BeginPass)다.
    Texture,
    // 스왑체인 백버퍼(BeginFrame, 기록 끝에 present)다. 그래프 밖 출력이므로 그 pass는 부수 효과
    // pass다.
    SwapChain
};

// 색 attachment 하나다(그래프 텍스처의 mipLevel·slice, 규칙은 graphics::ColorAttachment). load
// Load는 이전 내용을 읽는 것으로도 센다(생산 전 읽기 검사).
//
// resolveTarget이 있으면 pass 끝에 이 MSAA attachment를 그 1표본 렌더 타깃 그래프 텍스처의
// (resolveMipLevel, resolveSlice)로 resolve한다(박스 평균, 결정 R17). 대상 subresource는 이 pass의
// ResolveDestination 접근(생산)이고 전이는 장치가 낸다. 원본 attachment는 pass 뒤에도 RenderTarget
// 상태다(RenderBase 8절 RG1 남은 것 설계 2).
//
// 사용 예:
//   accesses.ColorTarget({.texture = msaaColor, .resolveTarget = sceneColor});
struct GraphColorTarget {
    GraphTexture texture;
    GraphLoadAction load = GraphLoadAction::Clear;
    graphics::ClearColor clear;
    uint32 mipLevel = 0;
    uint32 slice = 0;
    GraphTexture resolveTarget;
    uint32 resolveMipLevel = 0;
    uint32 resolveSlice = 0;
};

// 깊이·스텐실 attachment다(규칙은 graphics::DepthAttachment). 접근은 DepthWrite, readOnly면
// DepthRead다. clearDepth 기본 0은 reverse-Z 먼 평면이다.
struct GraphDepthTarget {
    GraphTexture texture;
    GraphLoadAction depthLoad = GraphLoadAction::Clear;
    float clearDepth = 0.0f;
    GraphLoadAction stencilLoad = GraphLoadAction::Clear;
    uint8 clearStencil = 0;
    bool readOnly = false;
    uint32 mipLevel = 0;
    uint32 slice = 0;
};

// Raster pass의 렌더 타깃이다. Texture는 색 attachment colorCount개(0–MaxColorTargets)와 깊이
// attachment 하나(hasDepth)를 장치 BeginPass로 붙이고, SwapChain은 스왑체인 백버퍼에 clear로
// 그린다(BeginFrame). attachment 전이는 장치가 낸다(RenderBase 8절 RG1 남은 것 A부).
struct GraphRenderTarget {
    GraphTargetKind kind = GraphTargetKind::None;
    GraphColorTarget colors[graphics::MaxColorTargets];
    uint32 colorCount = 0;
    GraphDepthTarget depth;
    bool hasDepth = false;
    graphics::SwapChainHandle swapChain;
    // 스왑체인 백버퍼의 clear 색이다.
    graphics::ClearColor clear;
};

// pass 하나의 자원 접근 목록을 만드는 명시 선언 builder다(그래프 내부 접근 모델의 입력, 결정 R4의
// 파라미터 구조체가 들어오기 전 경로). 목록은 그래프의 프레임 arena에 쌓이며 AddPass가 복사한다.
//
// 사용 예:
//   GraphPassAccesses accesses(graph);
//   accesses.Texture(sceneColor, graphics::ResourceAccess::SrvGraphics)
//       .RenderTarget(output, GraphLoadAction::Clear, {0, 0, 0, 1});
//   graph.AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
//                 [=](GraphPassContext& pass) noexcept { ... });
//
// 파라미터 구조체로 이어질 경계: 그래프는 GraphResourceAccess 목록과 GraphRenderTarget만 본다.
// 파라미터 구조체 수집기는 구조체 layout의 그래프 자원 멤버(SRV·UAV·접근만 선언·렌더 타깃 slot)를
// 훑어 이 builder와 같은 값을 만들어 같은 AddPass 내부 경로로 넘긴다. 멤버 종류 → 접근 상태 대응은
// pass 종류와 함께 정한다(Raster의 SRV는 SrvGraphics, Compute의 SRV·UAV는 SrvCompute·UavCompute,
// 9.6절).
//
// 실패: arena 할당 실패는 기록해 두고 AddPass가 그 오류를 돌려준다(이후 호출은 무시한다). 인자의
// 정당성(handle·접근 조합·pass 종류·attachment 개수와 범위)은 AddPass가 검사한다.
//
// 스레드·수명: 그래프와 같은 스레드(렌더 lane)에서 쓰고 AddPass 뒤 버린다. 그래프보다 오래 살지
// 않는다.
class ROCKS_RENDERGRAPH_API GraphPassAccesses {
public:
    explicit GraphPassAccesses(FrameArena& arena) noexcept : arena_(arena) {
    }

    // 그래프의 프레임 arena를 쓴다.
    explicit GraphPassAccesses(RenderGraph& graph) noexcept;

    GraphPassAccesses(const GraphPassAccesses&) = delete;
    GraphPassAccesses& operator=(const GraphPassAccesses&) = delete;

    // 텍스처 subresource 범위(기본 전체)를 access로 쓴다고 선언한다.
    GraphPassAccesses& Texture(GraphTexture texture, graphics::ResourceAccess access,
                               const graphics::SubresourceRange& range = {},
                               GraphAccessFlags flags = GraphAccessFlags::None) noexcept;

    // texture view(RenderGraph::CreateTextureView)의 범위를 access로 쓴다고 선언한다. 읽기 view는
    // SRV 접근(SrvGraphics·SrvCompute), 쓰기 view는 UAV 접근만 받는다. 기록 함수는
    // GraphPassContext::View로 장치 view를 받는다.
    //
    // 사용 예(HZB 한 단계: mip k 읽기 + mip k + 1 쓰기, 다른 subresource라 상충 아님):
    //   accesses.TextureView(mipK, graphics::ResourceAccess::SrvCompute)
    //       .TextureView(mipK1Write, graphics::ResourceAccess::UavCompute);
    GraphPassAccesses& TextureView(GraphTextureView view, graphics::ResourceAccess access,
                                   GraphAccessFlags flags = GraphAccessFlags::None) noexcept;

    // 버퍼 전체를 access로 쓴다고 선언한다.
    GraphPassAccesses& Buffer(GraphBuffer buffer, graphics::ResourceAccess access,
                              GraphAccessFlags flags = GraphAccessFlags::None) noexcept;

    // 색 attachment(mip 0, slice 0)를 하나 덧붙인다. 접근은 RenderTarget이며, load가 Load면 이전
    // 내용을 읽는 것으로도 센다. 색 attachment가 MaxColorTargets를 넘으면 AddPass가 검증 오류로
    // 거부한다.
    GraphPassAccesses& RenderTarget(GraphTexture texture,
                                    GraphLoadAction load = GraphLoadAction::Clear,
                                    const graphics::ClearColor& clear = {}) noexcept;

    // 색 attachment(mip·slice 지정)를 하나 덧붙인다(MRT는 부른 순서가 slot 순서다).
    //
    // 사용 예(GBuffer 둘 + 깊이):
    //   accesses.ColorTarget({albedo}).ColorTarget({normal, GraphLoadAction::Clear, {0.5f, 0.5f,
    //   1, 1}}).DepthTarget({depth});
    GraphPassAccesses& ColorTarget(const GraphColorTarget& target) noexcept;

    // 깊이 attachment를 정한다(다시 부르면 바꾼다).
    GraphPassAccesses& DepthTarget(const GraphDepthTarget& target) noexcept;

    // 스왑체인 백버퍼를 렌더 타깃으로 정한다(present pass).
    GraphPassAccesses& SwapChainTarget(graphics::SwapChainHandle swapChain,
                                       const graphics::ClearColor& clear = {}) noexcept;

    Span<const GraphResourceAccess> Accesses() const noexcept {
        return Span<const GraphResourceAccess>(data_, size_);
    }

    const GraphRenderTarget& Target() const noexcept {
        return target_;
    }

    // 선언 중 생긴 할당 오류다(없으면 None).
    ErrorCode Error() const noexcept {
        return error_;
    }

private:
    void Push(const GraphResourceAccess& access) noexcept;

    FrameArena& arena_;
    GraphResourceAccess* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
    GraphRenderTarget target_;
    ErrorCode error_ = ErrorCode::None;
};

} // namespace rocks::render
