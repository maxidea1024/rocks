#pragma once

#include <Core/Math/Color.h>
#include <UiCore/FrameTime.h>
#include <UiCore/Rendering/UiSceneExtract.h>
#include <UiCore/Rendering/ViewportTarget.h>

namespace rocks::ui {

// viewport Element에 장면을 대는 source다(UE ISlateViewport, Docs/Design/UiToolkit 3.11·18.3절).
// 게임 lane에서만 불리고 그래픽 타입이 없다. 장치 기록은 Extract가 게시한 함수가 Render lane에서
// 한다.
class ROCKS_UICORE_API ViewportSource {
public:
    virtual ~ViewportSource() noexcept = default;

    // 이번 프레임의 장면 상태를 진행한다(World tick 등). 같은 frame으로 두 번 불려도 한 번만
    // 진행해야 한다. time은 viewport가 놓인 UI의 프레임 시각이다(Docs/Design/UiToolkit 20.3절 —
    // 실제 시계를 직접 읽지 않는다).
    virtual Result<void> Advance(uint64 frame, const FrameTime& time) noexcept = 0;
    virtual LinearColor Background() const noexcept = 0;
    // 이번 프레임의 장면 pass packet을 게시한다. 게시하지 않으면 대상은 Background로 지워진다.
    virtual Result<void> Extract(UiSceneExtract& extract,
                                 const ViewportTarget& target) noexcept = 0;
};

} // namespace rocks::ui
