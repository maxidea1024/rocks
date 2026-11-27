#pragma once

#include <Core/Containers/HashMap.h>
#include <UiCore/Element.h>

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
    uint64 plan_ = 0;
};

} // namespace rocks::ui
