#include <Core/Graph/StrongComponents.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 공용 SCC 시험이다(Core/Graph/StrongComponents.h — Docs/Design/AssetSystem.ko.md 10.2절 "A4c 구현
// 세부" 기반 보강 2): 빈 그래프, 간선 없는 노드, 자기 간선, 순환과 꼬리(의존 먼저), 떨어진 묶음의
// 순서(뿌리 0부터 — 결정적), 깊은 사슬(재귀 없음), visit 실패 전파(멈춤), 모든 할당 실패.
using namespace rocks;

namespace strong_components_tests_file {

namespace {

// 깊은 사슬의 노드 수다(재귀 구현이면 호출 stack이 넘치는 깊이).
constexpr uint32 DeepChainNodes = 200000;
// 할당 실패 시험의 사슬 노드 수다(stack·frames가 여러 번 자라는 길이).
constexpr uint32 FailureChainNodes = 64;

// 알린 묶음을 (정렬한 구성원, cyclic)으로 모은다.
struct Collected {
    explicit Collected(Allocator& allocator) noexcept : members(allocator), starts(allocator) {
    }

    Result<void> Add(Span<const uint32> component, bool isCyclic) noexcept {
        if (auto pushed = starts.TryPushBack(static_cast<uint32>(members.Size())); !pushed) {
            return pushed;
        }

        const usize base = members.Size();
        for (uint32 node : component) {
            usize at = members.Size();
            while (at > base && members[at - 1] > node) {
                --at;
            }

            if (auto inserted = members.TryInsert(node, at); !inserted) {
                return inserted;
            }
        }

        cyclic = cyclic * 2 + (isCyclic ? 1 : 0);
        return Result<void>::Success();
    }

    // index번째 묶음이 expected와 같은지 본다.
    bool Is(usize index, Span<const uint32> expected) const noexcept {
        const usize begin = starts[index];
        const usize end = index + 1 < starts.Size() ? starts[index + 1] : members.Size();
        if (end - begin != expected.Size()) {
            return false;
        }

        for (usize at = 0; at < expected.Size(); ++at) {
            if (members[begin + at] != expected[at]) {
                return false;
            }
        }

        return true;
    }

    Array<uint32> members;
    Array<uint32> starts;
    // 묶음마다 cyclic bit를 알린 순서로 쌓은 값이다.
    uint64 cyclic = 0;
};

Result<void> Run(Allocator& allocator, uint32 count, Span<const uint32> first,
                 Span<const uint32> targets, Collected& out) noexcept {
    return algo::ForEachStrongComponent(
        allocator, count, first, targets,
        [&](Span<const uint32> members, bool cyclic) noexcept { return out.Add(members, cyclic); });
}

} // namespace

// 모양: 빈 그래프는 알리지 않는다. 간선 없는 노드는 cyclic 거짓, 자기 간선은 참이다. 0 → 1 → 0,
// 1 → 2, 3 → 2, 4 → 4이면 {2}, {0, 1}(순환), {3}, {4}(자기 간선) 순이다(의존 먼저, 뿌리 0부터).
TEST(StrongComponents_ShapesAndOrder) {
    SystemAllocator allocator;
    const uint32 none[] = {0};
    Collected empty(allocator);
    REQUIRE(Run(allocator, 0, none, Span<const uint32>(), empty));
    CHECK(empty.starts.IsEmpty());

    const uint32 first[] = {0, 1, 3, 3, 4, 5};
    const uint32 targets[] = {1, 0, 2, 2, 4};
    Collected found(allocator);
    REQUIRE(Run(allocator, 5, first, targets, found));
    REQUIRE(found.starts.Size() == 4);
    const uint32 sink[] = {2};
    const uint32 cycle[] = {0, 1};
    const uint32 tail[] = {3};
    const uint32 self[] = {4};
    CHECK(found.Is(0, sink) && found.Is(1, cycle) && found.Is(2, tail) && found.Is(3, self));
    // cyclic bit: {2} 0, {0, 1} 1, {3} 0, {4} 1.
    CHECK(found.cyclic == 0b0101);

    // 같은 그래프면 같은 순서다.
    Collected again(allocator);
    REQUIRE(Run(allocator, 5, first, targets, again));
    CHECK(again.members.Size() == found.members.Size() && again.cyclic == found.cyclic);
}

// 깊은 사슬(0 → 1 → … → n-1)은 재귀 없이 끝(n-1)부터 하나씩 알린다. 끝을 0으로 이으면 묶음 하나다.
TEST(StrongComponents_DeepChain) {
    SystemAllocator allocator;
    Array<uint32> first(allocator);
    Array<uint32> targets(allocator);
    REQUIRE(first.TryResize(DeepChainNodes + 1));
    REQUIRE(targets.TryResize(DeepChainNodes - 1));
    for (uint32 node = 0; node <= DeepChainNodes; ++node) {
        first[node] = node < DeepChainNodes ? node : DeepChainNodes - 1;
    }

    for (uint32 node = 0; node + 1 < DeepChainNodes; ++node) {
        targets[node] = node + 1;
    }

    uint32 reported = 0;
    uint32 lastNode = 0;
    auto chain = algo::ForEachStrongComponent(
        allocator, DeepChainNodes, first.View(), targets.View(),
        [&](Span<const uint32> members, bool cyclic) noexcept -> Result<void> {
            CHECK(members.Size() == 1 && !cyclic);
            CHECK(members[0] == DeepChainNodes - 1 - reported);
            lastNode = members[0];
            ++reported;
            return Result<void>::Success();
        });
    REQUIRE(chain);
    CHECK(reported == DeepChainNodes && lastNode == 0);

    // 끝 → 0을 더하면 전체가 한 묶음이다.
    REQUIRE(targets.TryPushBack(0));
    first[DeepChainNodes] = DeepChainNodes;
    uint32 components = 0;
    usize size = 0;
    auto ring = algo::ForEachStrongComponent(
        allocator, DeepChainNodes, first.View(), targets.View(),
        [&](Span<const uint32> members, bool cyclic) noexcept -> Result<void> {
            CHECK(cyclic);
            size = members.Size();
            ++components;
            return Result<void>::Success();
        });
    REQUIRE(ring);
    CHECK(components == 1 && size == DeepChainNodes);
}

// visit 실패는 그 오류로 멈춘다(뒤 묶음을 알리지 않는다). 모든 할당 지점은 OutOfMemory다.
TEST(StrongComponents_FailuresPropagate) {
    SystemAllocator allocator;
    const uint32 first[] = {0, 1, 3, 3, 4, 5};
    const uint32 targets[] = {1, 0, 2, 2, 4};
    uint32 calls = 0;
    auto stopped = algo::ForEachStrongComponent(
        allocator, 5, first, targets, [&](Span<const uint32>, bool) noexcept -> Result<void> {
            ++calls;
            return calls == 2 ? Result<void>::Failure(ErrorCode::Busy) : Result<void>::Success();
        });
    CHECK(stopped.Error() == ErrorCode::Busy && calls == 2);

    test::NFailAllocator failing(allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        Collected found(allocator);
        failing.FailOnAttempt(attempt);
        auto run = Run(failing, 5, first, targets, found);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(run);
            CHECK(found.starts.Size() == 4);
            break;
        }

        CHECK(run.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures >= 6);

    // 긴 사슬: 뿌리 아닌 노드를 넣으며 stack·frames가 자라는 지점도 실패한다.
    Array<uint32> chainFirst(allocator);
    Array<uint32> chainTargets(allocator);
    REQUIRE(chainFirst.TryResize(FailureChainNodes + 1));
    for (uint32 node = 0; node <= FailureChainNodes; ++node) {
        chainFirst[node] = node < FailureChainNodes ? node : FailureChainNodes - 1;
    }

    for (uint32 node = 0; node + 1 < FailureChainNodes; ++node) {
        REQUIRE(chainTargets.TryPushBack(node + 1));
    }

    uint32 chainFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        Collected found(allocator);
        failing.FailOnAttempt(attempt);
        auto run = Run(failing, FailureChainNodes, chainFirst.View(), chainTargets.View(), found);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(run);
            CHECK(found.starts.Size() == FailureChainNodes);
            break;
        }

        CHECK(run.Error() == ErrorCode::OutOfMemory);
        ++chainFailures;
    }

    CHECK(chainFailures > failures);
}

} // namespace strong_components_tests_file
