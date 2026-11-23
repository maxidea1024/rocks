#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>

// 반복형 Tarjan 강하게 연결된 묶음(SCC) 찾기다(재귀가 없어 노드 수와 무관하게 호출 stack을 쓰지
// 않는다). SparksCompiler 비공개 구현을 공용으로 올린 것이다(Docs/Design/AssetSystem 10.2절
// "A4c 구현 세부" — 쿠커의 Build·Hard SCC, 컴파일러의 패키지·struct 순환이 같은 것을 쓴다).
//
// 그래프는 노드 0..count-1과 노드마다 [firstEdge[n], firstEdge[n + 1]) 범위의 간선 대상 targets다
// (CSR — firstEdge는 count + 1개, 대상은 count 미만). 묶음은 끝(다른 묶음을 가리키지 않는 쪽)부터,
// 즉 의존이 먼저 visit(members, cyclic)으로 알린다. 뿌리는 0부터, 간선은 주어진 순서로 따라가므로
// 같은 그래프면 같은 순서다(결정적). members는 이 호출 동안만 유효하고 순서는 정해져 있지
// 않다(필요한 쪽이 정렬한다). cyclic은 노드가 둘 이상이거나 자기 간선이 있을 때다. visit은
// Result<void>를 돌려주고, 실패하면 그 오류로 멈춘다. 할당 실패면 OutOfMemory다.
//
// 사용 예:
//   // 0 → 1 → 0, 1 → 2
//   const uint32 first[] = {0, 1, 3, 3};
//   const uint32 to[] = {1, 0, 2};
//   auto found = algo::ForEachStrongComponent(allocator, 3, first, to,
//       [&](Span<const uint32> members, bool cyclic) noexcept -> Result<void> {
//           // 먼저 {2}(cyclic 거짓), 다음 {0, 1}(cyclic 참)
//           return Result<void>::Success();
//       });
namespace rocks::algo {

template <typename Visit>
Result<void> ForEachStrongComponent(Allocator& allocator, uint32 count,
                                    Span<const uint32> firstEdge, Span<const uint32> targets,
                                    Visit&& visit) noexcept {
    constexpr uint32 Unvisited = NumericLimits<uint32>::Max();
    struct Frame {
        uint32 node = Unvisited;
        uint32 edge = 0;
    };

    Array<uint32> order(allocator);
    Array<uint32> low(allocator);
    Array<uint8> done(allocator);
    Array<uint32> stack(allocator);
    Array<uint32> members(allocator);
    Array<Frame> frames(allocator);
    if (auto resized = order.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = low.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = done.TryResize(count); !resized) {
        return resized;
    }

    for (uint32 index = 0; index < count; ++index) {
        order[index] = Unvisited;
        done[index] = 0;
    }

    uint32 visited = 0;
    for (uint32 root = 0; root < count; ++root) {
        if (order[root] != Unvisited) {
            continue;
        }

        order[root] = visited;
        low[root] = visited;
        ++visited;
        if (auto pushed = stack.TryPushBack(root); !pushed) {
            return pushed;
        }

        if (auto pushed = frames.TryPushBack(Frame{root, firstEdge[root]}); !pushed) {
            return pushed;
        }

        while (!frames.IsEmpty()) {
            Frame& frame = frames[frames.Size() - 1];
            const uint32 node = frame.node;
            if (frame.edge < firstEdge[node + 1]) {
                const uint32 target = targets[frame.edge++];
                if (order[target] == Unvisited) {
                    order[target] = visited;
                    low[target] = visited;
                    ++visited;
                    if (auto pushed = stack.TryPushBack(target); !pushed) {
                        return pushed;
                    }

                    if (auto pushed = frames.TryPushBack(Frame{target, firstEdge[target]});
                        !pushed) {
                        return pushed;
                    }
                } else if (done[target] == 0 && order[target] < low[node]) {
                    low[node] = order[target]; // 스택 위의 대상
                }

                continue;
            }

            static_cast<void>(frames.TryPopBack()); // 비어 있지 않다
            if (!frames.IsEmpty()) {
                const uint32 parent = frames[frames.Size() - 1].node;
                low[parent] = low[node] < low[parent] ? low[node] : low[parent];
            }

            if (low[node] != order[node]) {
                continue;
            }

            members.Clear();
            uint32 popped = Unvisited;
            do {
                popped = stack.TryPopBack().Value(); // node가 스택에 있다
                done[popped] = 1;
                if (auto pushed = members.TryPushBack(popped); !pushed) {
                    return pushed;
                }
            } while (popped != node);

            bool cyclic = members.Size() > 1;
            for (uint32 at = firstEdge[node]; !cyclic && at < firstEdge[node + 1]; ++at) {
                cyclic = targets[at] == node;
            }

            if (auto reported = visit(members.View(), cyclic); !reported) {
                return reported;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::algo
