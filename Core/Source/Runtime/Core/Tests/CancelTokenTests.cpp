#include <Core/CancelToken.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace cancel_token_tests_file {

// 기본 token·source는 취소될 수 없고, CheckCancelled는 성공이다.
TEST(CancelToken_DefaultCannotBeCancelled) {
    CancelToken token;
    CHECK(!token.CanBeCancelled());
    CHECK(!token.IsCancellationRequested());
    CHECK(token.CheckCancelled());
    CancelSource empty;
    CHECK(!empty.IsValid());
    empty.Cancel();
    CHECK(!empty.Token().IsCancellationRequested());
}

// CheckCancelled는 취소 뒤 Cancelled를 돌려준다(PG 시험 2).
TEST(CancelToken_CheckCancelledReportsCancelled) {
    SystemAllocator allocator;
    {
        auto source = CancelSource::Create(allocator);
        REQUIRE(source);
        const CancelToken token = source.Value().Token();
        CHECK(token.CheckCancelled());
        source.Value().Cancel();
        CHECK(token.CheckCancelled().Error() == ErrorCode::Cancelled);
        CHECK(source.Value().IsCancellationRequested());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 연결 취소(PG 시험 1): 부모 취소는 자식·손자에 보이고, 자식 취소는 부모에 닿지 않는다. Source 쪽
// 조회도 사슬을 본다.
TEST(CancelToken_LinkedCancellationFollowsTheChain) {
    SystemAllocator allocator;
    {
        auto parent = CancelSource::Create(allocator);
        REQUIRE(parent);
        auto child = CancelSource::CreateLinked(allocator, parent.Value().Token());
        REQUIRE(child);
        auto grandchild = CancelSource::CreateLinked(allocator, child.Value().Token());
        REQUIRE(grandchild);
        auto sibling = CancelSource::CreateLinked(allocator, parent.Value().Token());
        REQUIRE(sibling);

        child.Value().Cancel();
        CHECK(child.Value().IsCancellationRequested());
        CHECK(grandchild.Value().Token().IsCancellationRequested());
        CHECK(!parent.Value().IsCancellationRequested());
        CHECK(!sibling.Value().IsCancellationRequested());

        parent.Value().Cancel();
        CHECK(sibling.Value().IsCancellationRequested());
        CHECK(sibling.Value().Token().CheckCancelled().Error() == ErrorCode::Cancelled);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 기본 부모 token이면 일반 source다(사슬 없음, 깊이 1).
TEST(CancelToken_LinkedToDefaultTokenIsPlain) {
    SystemAllocator allocator;
    {
        auto source = CancelSource::CreateLinked(allocator, CancelToken());
        REQUIRE(source);
        CHECK(source.Value().IsValid());
        CHECK(!source.Value().IsCancellationRequested());
        source.Value().Cancel();
        CHECK(source.Value().Token().IsCancellationRequested());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 사슬 깊이는 MaxCancelChainDepth까지이고 넘으면 CapacityExceeded다.
TEST(CancelToken_ChainDepthIsLimited) {
    SystemAllocator allocator;
    {
        CancelSource sources[MaxCancelChainDepth];
        auto root = CancelSource::Create(allocator);
        REQUIRE(root);
        sources[0] = Move(root.Value());
        for (uint32 depth = 1; depth < MaxCancelChainDepth; ++depth) {
            auto linked = CancelSource::CreateLinked(allocator, sources[depth - 1].Token());
            REQUIRE(linked);
            sources[depth] = Move(linked.Value());
        }

        CHECK(CancelSource::CreateLinked(allocator, sources[MaxCancelChainDepth - 1].Token())
                  .Error() == ErrorCode::CapacityExceeded);
        sources[0].Cancel();
        CHECK(sources[MaxCancelChainDepth - 1].IsCancellationRequested());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 자식이 부모 상태를 붙잡는다: 부모 source·token이 먼저 사라져도 자식이 부모의 표시를 읽고, 마지막
// 참조가 놓일 때 사슬 전체가 반환된다.
TEST(CancelToken_ChildKeepsParentStateAlive) {
    SystemAllocator allocator;
    {
        CancelToken childToken;
        {
            auto parent = CancelSource::Create(allocator);
            REQUIRE(parent);
            auto child = CancelSource::CreateLinked(allocator, parent.Value().Token());
            REQUIRE(child);
            childToken = child.Value().Token();
            parent.Value().Cancel();
        }

        CHECK(childToken.IsCancellationRequested());
        CHECK(allocator.Stats().liveAllocations == 2);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace cancel_token_tests_file
