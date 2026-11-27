#pragma once

#include <RenderBase/RenderTransfer.h>
#include <UiRender/UiFrameTypes.h>

namespace rocks::ui {

class UiLaneRenderer;

// 게임 lane이 RenderLane::SubmitFrame으로 넘기는 UI 프레임
// packet이다(Docs/Design/UiToolkit 18.4절). 목록은 모두 그 프레임 slot의 arena에 있어 게임 lane
// 주소를 들지 않는다. renderer는 Render lane 쪽 객체다(게임 주소가 아니다). 소멸자가 장면 packet을
// 한 번만 부순다(프레임 arena의 Reset은 저장소만 회수한다). 이동하면 원본은 빈다.
class ROCKS_UIRENDER_API UiFramePacket {
public:
    UiFramePacket() noexcept = default;
    UiFramePacket(UiLaneRenderer* renderer, Span<UiFrameTarget> targets) noexcept
        : renderer_(renderer), targets_(targets) {
    }

    ~UiFramePacket() noexcept;

    UiFramePacket(UiFramePacket&& other) noexcept;
    UiFramePacket& operator=(UiFramePacket&& other) noexcept;
    UiFramePacket(const UiFramePacket&) = delete;
    UiFramePacket& operator=(const UiFramePacket&) = delete;

    UiLaneRenderer* Renderer() const noexcept {
        return renderer_;
    }

    Span<UiFrameTarget> Targets() const noexcept {
        return targets_;
    }

    // 장면 packet을 부수고 비운다. 프레임 함수가 그리기 직후 Render lane에서 부른다: RenderLane은
    // 프레임 함수 값(이 packet)을 slot 반환 뒤에 부수므로, 그때까지 미루면 게임 lane이 다시 쓰는
    // 프레임 arena를 읽게 된다(Docs/Design/UiToolkit 18.4절). 두 번 불러도 된다.
    void ReleaseScenes() noexcept;

private:
    UiLaneRenderer* renderer_ = nullptr;
    Span<UiFrameTarget> targets_;
};

} // namespace rocks::ui

ROCKS_RENDER_TRANSFERABLE(rocks::ui::UiFramePacket);
