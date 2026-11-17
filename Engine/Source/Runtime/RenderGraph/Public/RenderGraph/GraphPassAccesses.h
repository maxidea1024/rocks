#pragma once

#include <Core/Memory/FrameArena.h>
#include <RenderGraph/GraphTypes.h>

namespace rocks::render {

class RenderGraph;

// pass 렌더 타깃 종류다.
enum class GraphTargetKind : uint8 {
    None,
    // 그래프 텍스처(BeginOffscreen)다.
    Texture,
    // 스왑체인 백버퍼(BeginFrame, 기록 끝에 present)다. 그래프 밖 출력이므로 그 pass는 부수 효과
    // pass다.
    SwapChain
};

// Raster pass의 렌더 타깃이다. RG1 장치 계약은 색 타깃 하나와 clear만 받는다(GX4에서 MRT·깊이·
// load/store·mip·slice로 넓힌다).
struct GraphRenderTarget {
    GraphTargetKind kind = GraphTargetKind::None;
    GraphTexture texture;
    graphics::SwapChainHandle swapChain;
    GraphLoadAction load = GraphLoadAction::Clear;
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
// 정당성(handle·접근 조합·pass 종류)은 AddPass가 검사한다.
//
// 스레드·수명: 그래프와 같은 스레드(렌더 lane)에서 쓰고 AddPass 뒤 버린다. 그래프보다 오래 살지
// 않는다.
class GraphPassAccesses {
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

    // 버퍼 전체를 access로 쓴다고 선언한다.
    GraphPassAccesses& Buffer(GraphBuffer buffer, graphics::ResourceAccess access,
                              GraphAccessFlags flags = GraphAccessFlags::None) noexcept;

    // 렌더 타깃(색 하나)을 정한다. 접근은 RenderTarget이며, load가 Load면 이전 내용을 읽는 것으로도
    // 센다. 다시 부르면 바꾼다.
    GraphPassAccesses& RenderTarget(GraphTexture texture,
                                    GraphLoadAction load = GraphLoadAction::Clear,
                                    const graphics::ClearColor& clear = {}) noexcept;

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
