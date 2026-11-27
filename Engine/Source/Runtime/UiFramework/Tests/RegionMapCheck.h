#pragma once

#include <UiFramework/Application/Application.h>

namespace rocks::ui::test {

// 영역 지도 격자 간격(px)이다.
inline constexpr int32 RegionMapProbeStep = 3;

// 영역 지도가 점마다 QueryWindowRegion과 같은 답을 내는지 본다(Docs/Design/UiToolkit 19.2절 —
// 창 backend는 지도만 읽으므로 둘이 같아야 한다). 픽셀 가운데를 격자로 찍어 처음 어긋난 점을
// mismatch에 돌려준다. 같으면 true다.
inline bool RegionMapMatchesQuery(const Application& app, IntVec2 size,
                                  IntVec2& mismatch) noexcept {
    input::WindowRegionMap map;
    app.BuildWindowRegionMap(map);
    for (int32 y = 0; y < size.y; y += RegionMapProbeStep) {
        for (int32 x = 0; x < size.x; x += RegionMapProbeStep) {
            const Vec2 center{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            if (map.Find({x, y}) != app.QueryWindowRegion(center)) {
                mismatch = {x, y};
                return false;
            }
        }
    }

    return true;
}

} // namespace rocks::ui::test
