#pragma once

#include <Core/Containers/HashMap.h>
#include <UiCore/Element.h>
#include <UiCore/FocusSearch.h>

namespace rocks::ui {

// 마지막으로 화면에 제출한 배치에서 포인터 대상을 찾는다. 셀 내에서는 그리기의 역순으로 검사한다.
class ROCKS_UICORE_API HitGrid {
public:
    explicit HitGrid(Allocator& allocator) noexcept;
    Result<void> Update(const SharedPtr<Element>& root) noexcept;
    void Reset() noexcept;
    bool HasSnapshot() const noexcept;
    // 맞은 요소다. scope에서 맞은 요소로 내려가는 경로를 첫 비활성 요소 앞에서 자른다(UE
    // GetBubblePath — Docs/Design/UiToolkit 20.5절: 비활성 버튼을 누르면 그 부모가 받는다).
    // ignoreEnabled면 자르지 않는다(툴팁 — 비활성 위젯도 툴팁을 띄운다, 영역 지도).
    SharedPtr<Element> Find(Vec2 position, const Element& scope,
                            bool ignoreEnabled = false) const noexcept;

    // hit 후보 수다(Visible이고 크기가 있는 항목 — 그리기 순서). 영역 지도 생성이 쓴다.
    usize CandidateCount() const noexcept;
    // 위에서부터 fromTop번째 후보와 그 클립된 창 영역(AABB)이다. 사라졌거나 지금 입력 경로가 아닌
    // 후보는 빈 포인터다.
    SharedPtr<Element> CandidateFromTop(usize fromTop, Box2<float>& area) const noexcept;

    // start에서 direction(상하좌우)으로 다음 초점 대상을 찾는다(UE FindNextFocusableWidgetDefault ·
    // FindFocusableWidget — HittestGrid.cpp 366–640행, Docs/Design/UiToolkit 20.9절).
    //   - 진행 축으로 boundary 사각형 끝까지 쓸어 넓히고 수직 축을 0.5씩 줄인 사각형과 겹치는
    //     후보만 본다. 셀을 진행 방향으로 한 줄씩 훑어 후보가 있는 첫 줄에서 고른다(진행 축 거리,
    //     같으면 중심 거리 제곱 — 비교 허용오차 0.1).
    //   - 후보: 초점 받을 수 있고 활성이며 filter를 통과한다. rule이 Escape가 아니면 boundary의
    //     자손이어야 한다. 조상이 비활성이면 빼고 다시 찾는다.
    //   - rule: Stop은 경계 밖 후보를 버리고, Wrap은 반대쪽 변에서 한 번 더 훑는다.
    //   CustomBoundary는
    //     BeyondBoundary·ReachedEdge로 호출자에게 넘긴다. Explicit·Custom은 호출자가 먼저 처리한다.
    // grid가 없으면 None이다.
    FocusSearchResult FindNextFocusable(const Element& start, FocusDirection direction,
                                        NavigationRule rule, const Element& boundary,
                                        const FocusCandidateFilter& filter) const noexcept;

private:
    struct Entry {
        WeakPtr<Element> element;
        Geometry geometry;
        usize parentClip;
    };

    struct Candidate {
        usize entry;
        Box2<float> area;
    };
    struct Link {
        usize entry;
        usize previous;
    };
    Result<void> Record(const SharedPtr<Element>& element, usize parentClip,
                        const Box2<float>* clip) noexcept;
    Result<void> Index(usize entry, const Box2<float>& bounds) noexcept;
    bool Accepts(usize entry, Vec2 point, const Element& scope,
                 SharedPtr<Element>& hit) const noexcept;
    Array<Entry> entries_;
    HashMap<uint64, usize> cells_;
    Array<Link> links_;
    Array<usize> broad_;
    Array<Candidate> candidates_;
    WeakPtr<Element> root_;
    // 마지막으로 걸은 루트의 창 영역이다(방향 탐색의 셀 범위).
    Box2<float> extent_{};
    uint64 plan_ = 0;
};

} // namespace rocks::ui
