#include <Core/Memory/SystemAllocator.h>
#include <Graphics/SplitTransitionTracker.h>
#include <TestSupport/Test.h>

// 분리 전이 짝 표 시험이다(Graphics/SplitTransitionTracker.h, RenderBase 8절 RG2).
using namespace rocks;
using namespace rocks::graphics;

TEST(SplitTransitionTracker_MatchesPairsAndCountsUnended) {
    SystemAllocator allocator;
    SplitTransitionTracker tracker(allocator);
    ResourceTransition transitions[2];
    transitions[0].buffer = BufferHandle{SlotHandle{1, 1}};
    transitions[0].before = ResourceAccess::UavCompute;
    transitions[0].after = ResourceAccess::SrvCompute;
    transitions[1].texture = TextureHandle{SlotHandle{2, 1}};
    transitions[1].before = ResourceAccess::RenderTarget;
    transitions[1].after = ResourceAccess::SrvGraphics;
    const Span<const ResourceTransition> list(transitions);

    // 빈 목록은 null 짝이고, null 짝의 end는 Null이다.
    CHECK(tracker.Begin(Span<const ResourceTransition>()).IsNull());
    CHECK(tracker.End(SplitTransition{}, list) == SplitEndResult::Null);

    const SplitTransition first = tracker.Begin(list);
    const SplitTransition second = tracker.Begin(list.Left(1));
    CHECK(!first.IsNull() && !second.IsNull() && !(first == second));
    CHECK(tracker.OpenCount() == 2);
    // 다른 목록(개수·내용)은 Mismatch, 같은 목록은 Matched, 다시 닫으면 Unknown이다.
    CHECK(tracker.End(second, list) == SplitEndResult::Mismatch);
    CHECK(tracker.End(second, list.Left(1)) == SplitEndResult::Unknown);
    ResourceTransition changed[2] = {transitions[0], transitions[1]};
    changed[1].after = ResourceAccess::SrvCompute;
    const SplitTransition third = tracker.Begin(list);
    CHECK(tracker.End(third, Span<const ResourceTransition>(changed)) == SplitEndResult::Mismatch);
    CHECK(tracker.End(first, list) == SplitEndResult::Matched);
    CHECK(tracker.End(first, list) == SplitEndResult::Unknown);
    CHECK(tracker.OpenCount() == 0);

    // 끝나지 않은 짝은 Close가 세고 비운다.
    (void)tracker.Begin(list);
    (void)tracker.Begin(list);
    CHECK(tracker.Close() == 2);
    CHECK(tracker.OpenCount() == 0);
    CHECK(tracker.Close() == 0);
}
