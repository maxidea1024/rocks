#pragma once

#include <Core/Memory/SharedPtr.h>
#include <UiCore/DrawBatch.h>
#include <UiCore/Rendering/UiDrawBuffer.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/ProgressBar.h>

// Sandbox 엔진 창의 상태 패널이다(Docs/Design/RenderBase 18.5절 9항 — UT5). 게임 lane에서 UI
// Element 트리(UiFramework)를 그리기 목록으로 만들어 창 대상으로 draw buffer에 더한다. 렌더 lane은
// ScreenRenderer가 장면 tone map 뒤 같은 스왑체인에 Load로 그린다. 글꼴 에셋(Font FT3)이 아직 없어
// 글자 없이 왼쪽 아래 패널 하나에 진행 막대 셋(위에서부터 표시 목록 위치, mip, 층)을 둔다. 막대
// 영역 뒤는 배경 흐림이다. 입력은 받지 않는다(Application에 입력을 넘기지 않는다).
//
// 사용 예(게임 lane, 프레임마다):
//   screen::ScreenOverlay overlay(allocator);
//   overlay.Create();
//   auto buffer = ui.AcquireDrawBuffer();
//   overlay.Record({entry, mip, layer}, window, width, height, *buffer.Value());
//   auto packet = ui.RecordWindows(arena, *buffer.Value());
//   ui.ReleaseDrawBuffer(*buffer.Value());
namespace sandbox::screen {

using namespace rocks;

// 패널 배치 값이다(배율 1 논리 픽셀). 패널은 창 왼쪽 아래에서 OverlayMargin 떨어져 있고, 막대는
// 패널 안 OverlayInset 안쪽에 OverlayBarHeight 높이로 OverlayBarGap 간격을 두고 쌓인다.
inline constexpr float OverlayMargin = 4.0f;
inline constexpr float OverlayWidth = 64.0f;
inline constexpr float OverlayFramePadding = 3.0f;
inline constexpr float OverlaySlotPadding = 1.0f;
inline constexpr float OverlayBarHeight = 4.0f;
inline constexpr uint32 OverlayBarCount = 3;
// 패널 테두리에서 막대까지와 막대 사이 간격이다(Frame 여백 + 슬롯 여백, 슬롯 여백 둘).
inline constexpr float OverlayInset = OverlayFramePadding + OverlaySlotPadding;
inline constexpr float OverlayBarGap = OverlaySlotPadding * 2.0f;
inline constexpr float OverlayHeight =
    OverlayFramePadding * 2.0f + (OverlayBarHeight + OverlayBarGap) * OverlayBarCount;
// 막대 영역 뒤 배경 흐림 강도(픽셀)다. 패널 여백(OverlayFramePadding)보다 작아 패널 밖 장면을
// 섞지 않는다.
inline constexpr float OverlayBlurStrength = 2.0f;
static_assert(OverlayBlurStrength < OverlayFramePadding, "the blur must stay inside the panel");
// 패널 바탕 색(선형, 불투명)이다.
inline constexpr float OverlayPanelColor[4] = {0.02f, 0.02f, 0.03f, 1.0f};
// 창 높이 이 픽셀마다 배율 1을 더한다(정수 배율, 최소 1).
inline constexpr uint32 OverlayReferenceHeight = 360;

// 막대 번호다(위에서부터).
enum class OverlayBar : uint8 {
    Entry,
    Mip,
    Layer
};

// 창 높이의 패널 배율이다(height / OverlayReferenceHeight, 최소 1).
uint32 OverlayScale(uint32 height) noexcept;

// 막대 값이다(0–1 — 범위 밖은 자른다). 장면이면 모두 0이다.
struct OverlayState {
    // (고른 표시 항목 + 1) / 항목 수.
    float entry = 0.0f;
    // (mip + 1) / mip 수.
    float mip = 0.0f;
    // (층 + 1) / 층 수.
    float layer = 0.0f;
};

class ScreenOverlay {
public:
    // allocator는 이 객체보다 오래 산다.
    explicit ScreenOverlay(Allocator& allocator) noexcept;
    // 루트를 떼고 Application을 닫는다(미룬 해체 통지 — UiToolkit 21.4절).
    ~ScreenOverlay() noexcept;
    ScreenOverlay(const ScreenOverlay&) = delete;
    ScreenOverlay& operator=(const ScreenOverlay&) = delete;

    // Element 트리를 만든다. 실패: Element 생성 오류(할당 실패 등). 두 번 부르면 Busy다.
    Result<void> Create() noexcept;

    // 막대 값을 넣고 그리기 목록을 갱신해 창 대상(window, width × height 픽셀)을 buffer에 더한다.
    // buffer는 다음 LaneUiRenderer::RecordWindows까지 이 객체의 그리기 묶음을 가리킨다. 실패:
    // 만들지 않았으면 NotReady, 크기 0이면 InvalidArgument, 배치·그리기·버퍼 오류는 그 오류.
    Result<void> Record(const OverlayState& state, ui::UiWindowSurfaceId window, uint32 width,
                        uint32 height, ui::UiDrawBuffer& buffer) noexcept;

private:
    Allocator& allocator_;
    ui::Application app_;
    ui::DrawBatch batch_;
    SharedPtr<ui::Element> root_;
    SharedPtr<ui::ProgressBar> bars_[OverlayBarCount];
};

} // namespace sandbox::screen
