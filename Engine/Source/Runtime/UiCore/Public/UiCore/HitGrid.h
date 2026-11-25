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
    SharedPtr<Element> Find(Vec2 position, const Element& scope) const noexcept;

private:
    struct Entry {
        WeakPtr<Element> element;
        Geometry geometry;
        usize parentClip;
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
    WeakPtr<Element> root_;
    uint64 plan_ = 0;
};

} // namespace rocks::ui
