#pragma once

#include <RenderBase/RenderLane.h>
#include <UiCore/Rendering/UiRenderer.h>
#include <UiRender/UiFramePacket.h>

namespace rocks::ui {

class UiLaneRenderer;

// RenderLane 위의 UiRenderer 구현이다(게임 lane, Docs/Design/UiToolkit 18.4절). 텍스처·렌더 타깃은
// 렌더 handle 예약 + 렌더 lane 생성 작업으로 만들고, DrawWindows는 draw buffer를 그 프레임 slot의
// arena에 복사해 SubmitFrame한다. lane 쪽 짝(UiLaneRenderer)이 그래프로 기록한다.
//
// - 창 표면의 생성·파괴는 lane 작업이 끝날 때까지 기다린다(생성 실패를 바로 알고, 파괴 뒤에는 그
//   창에 present할 프레임이 남지 않는다 — 그 뒤 native 창을 부순다).
// - 크기를 바꾼 렌더 타깃은 id가 그대로이고 새 렌더 handle을 쓴다. 이미 넘긴 프레임은 옛 handle을
//   쓰고, 옛 handle은 그 프레임의 GPU 완료 뒤 회수된다.
// - 화면 읽기는 lane 작업으로 읽어 보관한다. WaitCapture는 lane 표지를 기다린다(결정적).
//
// 스레드: 모든 함수가 lane.Start를 부른 게임 lane 전용이다. 수명: lane과 laneRenderer는 이 객체보다
// 오래 살고, 이 객체를 부수기 전에 만든 자원을 돌려준다(남은 것은 소멸자가 Retire한다).
class ROCKS_UIRENDER_API LaneUiRenderer final : public UiRenderer {
public:
    LaneUiRenderer(Allocator& allocator, render::RenderLane& lane,
                   UiLaneRenderer& laneRenderer) noexcept;
    ~LaneUiRenderer() noexcept override;

    Result<UiWindowSurfaceId>
    CreateWindowSurface(const UiWindowSurfaceDesc& desc) noexcept override;
    Result<void> ResizeWindowSurface(UiWindowSurfaceId surface, uint32 width,
                                     uint32 height) noexcept override;
    Result<void> DestroyWindowSurface(UiWindowSurfaceId surface) noexcept override;
    Result<UiTextureId> CreateTexture(const UiTextureDesc& desc,
                                      Span<const uint8> pixels) noexcept override;
    Result<void> UpdateTexture(UiTextureId texture, Span<const uint8> pixels) noexcept override;
    Result<void> ResizeRenderTarget(UiTextureId texture, uint32 width,
                                    uint32 height) noexcept override;
    void ReleaseTexture(UiTextureId texture) noexcept override;
    Result<UiTextureDesc> DescribeTexture(UiTextureId texture) const noexcept override;
    Result<void> DrawWindows(UiDrawBuffer& buffer) noexcept override;
    Result<uint64> RequestCapture(UiTextureId target) noexcept override;
    Result<void> WaitCapture(uint64 capture) noexcept override;
    Result<void> TakeCapture(uint64 capture, Array<uint8>& rgba) noexcept override;
    Result<void> Flush() noexcept override;

    // lane 쪽 짝이다(그 lane 작업 값에 담는다 — 렌더 lane 객체 주소).
    UiLaneRenderer& LaneRenderer() const noexcept {
        return laneRenderer_;
    }

    // id의 렌더 handle이다(다른 처리기가 직접 그릴 때). 모르는 id는 빈 handle이다.
    render::RenderHandle HandleOf(UiTextureId texture) const noexcept;

    // buffer를 frame arena로 복사한 packet을 만든다(처리기가 프레임을 직접 잡는 경우 — Sandbox).
    // buffer의 장면 packet은 arena로 옮겨진다.
    Result<UiFramePacket> RecordWindows(FrameArena& arena, UiDrawBuffer& buffer) noexcept;

    usize LiveTextures() const noexcept;
    usize LiveWindowSurfaces() const noexcept;
    uint64 FramesSubmitted() const noexcept;

private:
    struct TextureEntry {
        render::RenderHandle handle{};
        UiTextureDesc desc{};
    };

    struct WindowEntry {
        uint32 width = 0;
        uint32 height = 0;
    };

    Result<render::RenderHandle> CreateDeviceTexture(const UiTextureDesc& desc,
                                                     Span<const uint8> pixels) noexcept;
    Result<UiFrameBatch> CopyBatch(FrameArena& arena, const DrawBatch& batch) noexcept;

    Allocator& allocator_;
    render::RenderLane& lane_;
    UiLaneRenderer& laneRenderer_;
    SlotArray<TextureEntry> textures_;
    SlotArray<WindowEntry> windows_;
    // 요청했고 아직 가져가지 않은 화면 읽기 번호다(모르는 번호 NotFound와 대기 중 NotReady를
    // 가른다).
    Array<uint64> captures_;
    uint64 nextCapture_ = 0;
    uint64 framesSubmitted_ = 0;
};

} // namespace rocks::ui
