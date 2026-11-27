#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Color.h>
#include <UiCore/Rendering/UiDrawTypes.h>
#include <UiCore/Rendering/UiScenePacket.h>

namespace rocks::ui {

class UiSceneExtract;

// 한 번 그리기의 대상 목록이다(UE FSlateDrawBuffer). 게임 lane에서 채우고 UiRenderer::DrawWindows에
// 넘긴다. 렌더러가 내용을 복사하므로 DrawWindows가 돌아온 뒤 다시 써도 된다. 담긴 DrawBatch는
// DrawWindows가 돌아올 때까지 살아 있어야 한다.
//
// 사용 예(창 하나):
//   UiDrawBuffer& buffer = renderer.AcquireDrawBuffer().Value();
//   recorder.Record(root, frame, width, height, buffer);      // 표면 pass를 먼저 쌓는다
//   buffer.AddWindow(surface, width, height, background, batch);
//   renderer.DrawWindows(buffer);
//   renderer.ReleaseDrawBuffer(buffer);
class ROCKS_UICORE_API UiDrawBuffer {
public:
    explicit UiDrawBuffer(Allocator& allocator) noexcept;

    UiDrawBuffer(const UiDrawBuffer&) = delete;
    UiDrawBuffer& operator=(const UiDrawBuffer&) = delete;

    void Clear() noexcept;

    // 다음 대상 앞에 그릴 보관 표면 pass를 쌓는다. 크기 0이나 빈 대상은 InvalidArgument다.
    Result<void> AddRetainedPass(UiTextureId target, uint32 width, uint32 height, LinearColor clear,
                                 const DrawBatch& content) noexcept;

    // 다음 대상 앞에 그릴 장면 pass를 쌓고 그 pass에 packet을 게시하는 통로를 돌려준다. 통로는 다른
    // pass를 쌓기 전까지만 쓸 수 있다.
    Result<UiSceneExtract> AddScenePass(UiTextureId target, uint32 width, uint32 height,
                                        LinearColor clear) noexcept;

    // 쌓인 pass를 닫아 창 대상 하나를 더한다.
    Result<void> AddWindow(UiWindowSurfaceId window, uint32 width, uint32 height, LinearColor clear,
                           const DrawBatch& batch) noexcept;

    // 쌓인 pass를 닫아 오프스크린 대상 하나를 더한다(캡처·끌기 미리보기).
    Result<void> AddOffscreen(UiTextureId target, uint32 width, uint32 height, LinearColor clear,
                              const DrawBatch& batch) noexcept;

    // 대상으로 닫지 않은 pass를 버린다(기록 실패 뒤).
    void DiscardPending() noexcept;

    usize PendingPassCount() const noexcept;
    Span<const UiDrawTarget> Targets() const noexcept;
    Span<const UiSurfacePass> Passes() const noexcept;
    Span<UiScenePacket> Packets() noexcept;
    Allocator& GetAllocator() const noexcept;

private:
    friend class UiSceneExtract;

    Result<void> AddTarget(UiDrawTarget target, const DrawBatch& batch) noexcept;
    Result<void> PostPacket(usize pass, UiScenePacket&& packet) noexcept;

    Allocator& allocator_;
    Array<UiDrawTarget> targets_;
    Array<UiSurfacePass> passes_;
    Array<UiScenePacket> packets_;
    usize closedPasses_ = 0;
};

} // namespace rocks::ui
