#include "GraphInternal.h"

// 편의 복원의 도달 검사다(Docs/Design/ObjectFoundations.ko.md 5.5절 — 단계형 복원은 고아를
// 허용하므로 편의 API만 쓴다).
namespace rocks::graph_detail {

// 편의 복원의 도달 검사다(Docs/Design/ObjectFoundations.ko.md 5.5절): 노드 1에서 영속 강한 참조,
// inner → outer(노드 표의 outer), 패키지 정책 Registry면 outer → inner와 기준 Outer 아래 보존(기준
// Outer가 있으면 outer 0 노드는 기준 Outer가 붙잡는다 — 1.7절)으로 닿지 않는 노드가 있으면
// InvalidArgument다(공개 뒤 다음 GC가 조용히 지우지 않게 — 생성 전에 판정한다). 패키지 정책이고
// 기준 Outer가 없으면 노드 1이 아닌 outer 0 노드(다른 패키지)도 InvalidArgument다.
Result<void> CheckReachable(Allocator& allocator, const Array<LoadNode>& nodes, bool packagePolicy,
                            bool hasBase, Budget& budget) noexcept {
    const usize count = nodes.Size();
    auto spent = budget.Spend(count);
    if (!spent) {
        return spent;
    }

    // visited와 큐, 패키지 정책이면 자식 목록(첫 자식·다음 형제 — 노드 번호 + 1, 0 = 없음)이다.
    Array<uint8> visited(allocator);
    Array<uint32> queue(allocator);
    Array<uint32> firstChild(allocator);
    Array<uint32> nextSibling(allocator);
    if (auto resized = visited.TryResize(count); !resized) {
        return resized;
    }

    if (auto reserved = queue.TryReserve(count); !reserved) {
        return reserved;
    }

    if (packagePolicy) {
        if (auto resized = firstChild.TryResize(count); !resized) {
            return resized;
        }

        if (auto resized = nextSibling.TryResize(count); !resized) {
            return resized;
        }

        // 역순으로 앞에 끼워 자식 목록이 노드 순서가 되게 한다(순서는 판정에 영향이 없다).
        for (usize index = count; index > 0; --index) {
            const uint32 outer = nodes[index - 1].outer;
            if (outer != 0) {
                nextSibling[index - 1] = firstChild[outer - 1];
                firstChild[outer - 1] = static_cast<uint32>(index);
            }
        }
    }

    auto visit = [&](usize node) noexcept {
        if (visited[node] == 0) {
            visited[node] = 1;
            if (!queue.TryPushBack(static_cast<uint32>(node))) {
                platform::Fatal("Object graph reachability queue reservation failed");
            }
        }
    };

    visit(0);
    for (usize index = 0; packagePolicy && index < count; ++index) {
        if (nodes[index].outer != 0) {
            continue;
        }

        // 패키지 정책에서 기준 Outer는 생존 객체이고 outer → inner로 자손을 붙잡는다. 기준 Outer가
        // 없으면 outer 0 노드는 패키지이고, 편의 복원은 노드 1(root)인 패키지 하나만 공개한다 —
        // 다른 패키지는 RootRef 없이 공개되어 다음 GC가 Discard 없이 회수하므로 거부한다(여러
        // 패키지는 단계형 복원과 Commit(roots) — 저장 쪽 결정 17과 대칭).
        if (!hasBase && index != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        visit(index);
    }

    for (usize position = 0; position < queue.Size(); ++position) {
        const LoadNode& node = nodes[queue[position]];
        spent = budget.Spend(node.tokens.Size() + 2);
        if (!spent) {
            return spent;
        }

        // 외부 참조는 노드가 아니므로 따라가지 않는다(대상은 그래프 밖 생존 객체다).
        for (const ReferenceToken& token : node.tokens) {
            if (ExternalIndexOf(token) == 0) {
                visit(token.target - 1);
            }
        }

        if (node.outer != 0) {
            visit(node.outer - 1);
        }

        for (uint32 child = packagePolicy ? firstChild[queue[position]] : 0; child != 0;
             child = nextSibling[child - 1]) {
            spent = budget.Spend(1);
            if (!spent) {
                return spent;
            }

            visit(child - 1);
        }
    }

    if (queue.Size() != count) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks::graph_detail
