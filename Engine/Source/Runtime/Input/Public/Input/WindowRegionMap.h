#pragma once

#include <Core/Math/Rect.h>
#include <Input/InputTypes.h>

namespace rocks::input {

// 영역 지도의 최대 사각형 수다(Docs/Design/UiToolkit 19.2절 영역 지도 게시).
inline constexpr usize MaxWindowRegionRects = 64;

// 영역 지도 항목 하나다(client 픽셀 사각형 [min, max)와 그 영역).
struct WindowRegionRect {
    IntRect rect;
    WindowRegion region = WindowRegion::Client;

    friend bool operator==(const WindowRegionRect&, const WindowRegionRect&) noexcept = default;
};

// UI가 그리기마다 만들어 창에 게시하는 영역 판정 지도다(Docs/Design/UiToolkit 19.2절). 창 backend는
// OS 영역 질의(Windows WM_NCHITTEST)에서 UI 트리 대신 이 지도만 읽는다. 앞 항목이 이긴다(앞 = 위).
// popupOpen이면 열린 팝업(메뉴)이 있어 backend가 크기 조절 가장자리를 끈다(UE AnyMenusVisible).
//
// 사용 예(backend):
//   const WindowRegion region = map.Find(clientPoint);   // 어느 항목에도 없으면 Client
struct WindowRegionMap {
    WindowRegionRect rects[MaxWindowRegionRects]{};
    uint32 count = 0;
    bool popupOpen = false;

    // point를 담은 첫 항목의 영역이다. 없으면 Client다.
    WindowRegion Find(IntVec2 point) const noexcept {
        for (uint32 index = 0; index < count; ++index) {
            const IntRect& rect = rects[index].rect;
            if (point.x >= rect.min.x && point.x < rect.max.x && point.y >= rect.min.y &&
                point.y < rect.max.y) {
                return rects[index].region;
            }
        }

        return WindowRegion::Client;
    }

    // 항목을 더한다. 가득 찼으면 false다.
    bool TryAdd(const WindowRegionRect& entry) noexcept {
        if (count == MaxWindowRegionRects) {
            return false;
        }

        rects[count++] = entry;
        return true;
    }

    bool SameAs(const WindowRegionMap& other) const noexcept {
        if (count != other.count || popupOpen != other.popupOpen) {
            return false;
        }

        for (uint32 index = 0; index < count; ++index) {
            if (!(rects[index] == other.rects[index])) {
                return false;
            }
        }

        return true;
    }
};

} // namespace rocks::input
