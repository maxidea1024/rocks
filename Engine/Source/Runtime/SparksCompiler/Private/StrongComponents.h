#pragma once

#include <Core/Containers/Array.h>

// 반복형 Tarjan 강하게 연결된 묶음 찾기다(재귀가 없어 노드 수와 무관하게 stack을 쓰지 않는다).
// 그래프는 노드 0..count-1과 노드마다 [firstEdge[n], firstEdge[n + 1])의 간선 대상 targets다.
// 묶음은 끝(다른 묶음을 가리키지 않는 쪽)부터 visit(members, cyclic)으로 알린다 — cyclic은 노드가
// 둘 이상이거나 자기 간선이 있을 때다. 할당 실패면 그 오류를 돌려준다.

namespace rocks::sparks::detail {

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

            visit(members.View(), cyclic);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks::detail
