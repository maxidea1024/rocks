#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/HitGrid.h>

// hit grid 방향 탐색을 grid에 직접 묻는 시험이다(Docs/Design/UiToolkit 20.9절 — UE
// FHittestGrid::FindFocusableWidget 366–570행). Application의 후보 거르기 없이 다음을 본다.
//   - 비활성 조상 아래 후보를 빼고 다시 찾음(UE IsParentsEnabled 528–533행)
//   - 경계 자손 검사(UE NotADescendant 454–460행)와 Escape
//   - 경계 밖 후보의 Stop·CustomBoundary·Wrap(UE 495–523행), 끝까지 없음(ReachedEdge, 543–566행)
//   - 셀 64개를 넘는 넓은 후보는 자기가 덮는 줄에서만 만남
//   - 창 밖(셀 범위 밖) 후보는 가장자리 셀에 들어감(UE GetCellCoordinate)

namespace rocks::ui {

namespace {

constexpr Vec2 GridWindow{400, 200};

class GridFocusBox final : public Element {
    ROCKS_UI_ELEMENT(GridFocusBox, Element)

public:
    explicit GridFocusBox(Allocator& allocator) noexcept : Element(allocator) {
    }

    bool SupportsKeyboardFocus() const noexcept override {
        return true;
    }
};

Result<SharedPtr<Element>> Add(Allocator& allocator, Element& parent, Vec2 origin, Vec2 size,
                               bool focusable = true) noexcept {
    SharedPtr<Element> element;
    if (focusable) {
        auto box = SharedPtr<GridFocusBox>::TryMake(allocator, allocator);
        if (!box) {
            return Result<SharedPtr<Element>>::Failure(box.Error());
        }

        element = box.Value();
    } else {
        auto plain = SharedPtr<Element>::TryMake(allocator, allocator);
        if (!plain) {
            return plain;
        }

        element = plain.Value();
    }

    if (auto added = parent.TryAddChild(element); !added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    element->SetGeometry(Geometry::Root(size, {1, 0, 0, 1, origin.x, origin.y}));
    return Result<SharedPtr<Element>>::Success(Move(element));
}

Result<SharedPtr<Element>> MakeWindow(Allocator& allocator, Vec2 size) noexcept {
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    if (root) {
        root.Value()->SetGeometry(Geometry::Root(size, {}));
    }

    return root;
}

constexpr Vec2 BoxSize{20, 20};

} // namespace

TEST(UiCore_FocusSearchRestartsPastCandidatesUnderDisabledAncestors) {
    SystemAllocator allocator;
    auto root = MakeWindow(allocator, GridWindow);
    REQUIRE(root);
    auto start = Add(allocator, *root.Value(), {0, 0}, BoxSize);
    auto panel = Add(allocator, *root.Value(), {30, 0}, {60, 40}, false);
    REQUIRE(start && panel);
    auto inside = Add(allocator, *panel.Value(), {40, 0}, BoxSize);
    auto beyond = Add(allocator, *root.Value(), {200, 0}, BoxSize);
    REQUIRE(inside && beyond);
    panel.Value()->SetEnabled(false);
    HitGrid grid(allocator);
    REQUIRE(grid.Update(root.Value()));
    // 거르기가 없어도 grid가 비활성 조상 아래 후보를 빼고 다시 찾는다.
    const auto found = grid.FindNextFocusable(*start.Value(), FocusDirection::Right,
                                              NavigationRule::Escape, *root.Value(), {});
    CHECK(found.outcome == FocusSearchOutcome::Found);
    CHECK(found.target.Get() == beyond.Value().Get());
    // 조상을 켜면 가까운 후보다.
    panel.Value()->SetEnabled(true);
    const auto enabled = grid.FindNextFocusable(*start.Value(), FocusDirection::Right,
                                                NavigationRule::Escape, *root.Value(), {});
    CHECK(enabled.target.Get() == inside.Value().Get());
}

TEST(UiCore_FocusSearchRequiresDescendantsOfTheBoundaryUnlessEscaping) {
    SystemAllocator allocator;
    auto root = MakeWindow(allocator, GridWindow);
    REQUIRE(root);
    auto popup = Add(allocator, *root.Value(), {0, 0}, {200, 40}, false);
    REQUIRE(popup);
    auto first = Add(allocator, *popup.Value(), {0, 0}, BoxSize);
    // 팝업 띠 안에 겹쳐 있지만 팝업의 자손이 아니다.
    auto sibling = Add(allocator, *root.Value(), {60, 0}, BoxSize);
    REQUIRE(first && sibling);
    HitGrid grid(allocator);
    REQUIRE(grid.Update(root.Value()));
    const auto contained = grid.FindNextFocusable(*first.Value(), FocusDirection::Right,
                                                  NavigationRule::Stop, *popup.Value(), {});
    CHECK(contained.outcome == FocusSearchOutcome::None);
    // Escape는 자손 검사를 하지 않는다.
    const auto escaped = grid.FindNextFocusable(*first.Value(), FocusDirection::Right,
                                                NavigationRule::Escape, *popup.Value(), {});
    CHECK(escaped.outcome == FocusSearchOutcome::Found);
    CHECK(escaped.target.Get() == sibling.Value().Get());
}

TEST(UiCore_FocusSearchAppliesBoundaryRulesToCandidatesPastTheEdge) {
    SystemAllocator allocator;
    auto root = MakeWindow(allocator, GridWindow);
    REQUIRE(root);
    auto panel = Add(allocator, *root.Value(), {0, 0}, {200, 40}, false);
    REQUIRE(panel);
    auto start = Add(allocator, *panel.Value(), {0, 0}, BoxSize);
    // 경계(패널) 오른쪽 변에 붙은 자손이다 — 진행 축 시작 변이 경계 끝 이상이라 경계 밖이다.
    auto edge = Add(allocator, *panel.Value(), {200, 0}, BoxSize);
    REQUIRE(start && edge);
    HitGrid grid(allocator);
    REQUIRE(grid.Update(root.Value()));
    const auto stop = grid.FindNextFocusable(*start.Value(), FocusDirection::Right,
                                             NavigationRule::Stop, *panel.Value(), {});
    CHECK(stop.outcome == FocusSearchOutcome::None);
    const auto custom = grid.FindNextFocusable(*start.Value(), FocusDirection::Right,
                                               NavigationRule::CustomBoundary, *panel.Value(), {});
    CHECK(custom.outcome == FocusSearchOutcome::BeyondBoundary);
    CHECK(!custom.target);
    // Wrap은 UE처럼 그 후보를 돌려준다(반대 변에서 다시 훑을 준비만 한다).
    const auto wrap = grid.FindNextFocusable(*start.Value(), FocusDirection::Right,
                                             NavigationRule::Wrap, *panel.Value(), {});
    CHECK(wrap.outcome == FocusSearchOutcome::Found);
    CHECK(wrap.target.Get() == edge.Value().Get());
    // 후보 없이 경계 끝까지 가면 CustomBoundary는 ReachedEdge다.
    edge.Value()->SetVisibility(Visibility::Collapsed);
    HitGrid without(allocator);
    REQUIRE(without.Update(root.Value()));
    const auto reached = without.FindNextFocusable(
        *start.Value(), FocusDirection::Right, NavigationRule::CustomBoundary, *panel.Value(), {});
    CHECK(reached.outcome == FocusSearchOutcome::ReachedEdge);
}

TEST(UiCore_FocusSearchMeetsWideCandidatesOnlyOnTheRowsTheyCover) {
    SystemAllocator allocator;
    constexpr Vec2 LargeWindow{1400, 1800};
    auto root = MakeWindow(allocator, LargeWindow);
    REQUIRE(root);
    auto start = Add(allocator, *root.Value(), {0, 0}, BoxSize);
    // 9×9셀(64 초과)이라 셀에 넣지 않는 넓은 후보다. 5번째 줄부터 덮는다.
    auto wide = Add(allocator, *root.Value(), {0, 660}, {1100, 1100});
    // 2번째 줄의 작은 후보가 더 가깝다.
    auto near = Add(allocator, *root.Value(), {0, 270}, BoxSize);
    REQUIRE(start && wide && near);
    HitGrid grid(allocator);
    REQUIRE(grid.Update(root.Value()));
    const auto found = grid.FindNextFocusable(*start.Value(), FocusDirection::Down,
                                              NavigationRule::Escape, *root.Value(), {});
    CHECK(found.target.Get() == near.Value().Get());
    // 작은 후보가 없으면 넓은 후보다.
    near.Value()->SetVisibility(Visibility::Collapsed);
    HitGrid without(allocator);
    REQUIRE(without.Update(root.Value()));
    const auto wideOnly = without.FindNextFocusable(*start.Value(), FocusDirection::Down,
                                                    NavigationRule::Escape, *root.Value(), {});
    CHECK(wideOnly.target.Get() == wide.Value().Get());
}

TEST(UiCore_FocusSearchFindsCandidatesOutsideTheWindowCells) {
    SystemAllocator allocator;
    auto root = MakeWindow(allocator, GridWindow);
    REQUIRE(root);
    // 창(400)보다 넓은 내용(1000) 안의 후보가 창 밖(x 600)에 있다.
    auto content = Add(allocator, *root.Value(), {0, 0}, {1000, 40}, false);
    REQUIRE(content);
    auto start = Add(allocator, *content.Value(), {0, 0}, BoxSize);
    auto offscreen = Add(allocator, *content.Value(), {600, 0}, BoxSize);
    REQUIRE(start && offscreen);
    HitGrid grid(allocator);
    REQUIRE(grid.Update(root.Value()));
    const auto found = grid.FindNextFocusable(*start.Value(), FocusDirection::Right,
                                              NavigationRule::Escape, *content.Value(), {});
    CHECK(found.outcome == FocusSearchOutcome::Found);
    CHECK(found.target.Get() == offscreen.Value().Get());
    // 점 찾기도 같은 가장자리 셀을 쓴다.
    CHECK(grid.Find({605, 5}, *root.Value()).Get() == offscreen.Value().Get());
}

} // namespace rocks::ui
