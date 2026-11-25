#pragma once

#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/StepPolicy.h>
#include <Platform/Atomics.h>

namespace rocks {

// TakeAllStack에 넣는 노드의 링크다. 노드 타입이 이것을 상속한다. next는 넣기 전 생산자만, 가져간
// 뒤 소비자만 쓰므로 원자가 아니다.
struct TakeAllStackNode {
    TakeAllStackNode* takeAllNext = nullptr;
};

// 침투형 스택이다(UE TConsumeAllMpmcQueue·TClosableMpscQueue 대응,
// Docs/Design/Synchronization 12.2절). push는 CAS, 소비는 TakeAll이 머리를 exchange로 통째로
// 가져간다. 한 노드씩 꺼내는 경합이 없어 ABA가 없다. Close 뒤의 Push는 false다(닫힌 목록 — 마지막
// 소비 뒤 들어오는 항목을 생산자가 직접 처리한다).
//
// 성질: Push·TakeAll·Close는 어느 스레드에서나 부른다(생산자·소비자 모두 lock-free). 가져간 목록은
// LIFO 순서다(Reverse로 FIFO). 노드는 호출자 소유이고 가져갈 때까지 살아 있어야 한다.
//
// 메모리 순서: Push의 CAS(release)가 노드 내용을 발행하고 TakeAll·Close의 exchange(acquire)가 본다.
//
// 사용 예(완료 통지 묶음):
//   struct Done : TakeAllStackNode { uint32 job; };
//   TakeAllStack<Done> completed;
//   completed.Push(&done);                          // worker
//   for (Done* item = TakeAllStack<Done>::Reverse(completed.TakeAll()); item != nullptr;
//        item = TakeAllStack<Done>::Next(item)) { Finish(item->job); }
template <class Node, class StepPolicy = NoSteps>
class TakeAllStack {
    static_assert(IsBaseOfV<TakeAllStackNode, Node>, "Node must derive from TakeAllStackNode");

public:
    constexpr TakeAllStack() noexcept = default;
    TakeAllStack(const TakeAllStack&) = delete;
    TakeAllStack& operator=(const TakeAllStack&) = delete;

    // node를 맨 위에 넣는다. 닫혔으면 넣지 않고 false다.
    bool Push(Node* node) noexcept {
        TakeAllStackNode* head = head_.Load<platform::MemoryOrder::Relaxed>();
        while (true) {
            if (head == ClosedMarker()) {
                return false;
            }

            node->takeAllNext = head;
            StepPolicy::Step("TakeAllStack.PushLinked");
            if (head_.CompareExchangeWeak<platform::MemoryOrder::Release,
                                          platform::MemoryOrder::Relaxed>(head, node)) {
                return true;
            }
        }
    }

    // 지금까지 넣은 노드를 모두 가져간다(LIFO 목록 머리, 비었으면 nullptr). 닫혔으면 nullptr이다.
    Node* TakeAll() noexcept {
        TakeAllStackNode* head = head_.Load<platform::MemoryOrder::Relaxed>();
        while (head != ClosedMarker()) {
            if (head_.CompareExchangeWeak<platform::MemoryOrder::Acquire,
                                          platform::MemoryOrder::Relaxed>(head, nullptr)) {
                return static_cast<Node*>(head);
            }
        }

        return nullptr;
    }

    // 닫고 남은 노드를 모두 가져간다(LIFO). 이후 Push는 false다. 이미 닫혔으면 nullptr이다.
    Node* Close() noexcept {
        TakeAllStackNode* head = head_.Exchange<platform::MemoryOrder::Acquire>(ClosedMarker());
        return head == ClosedMarker() ? nullptr : static_cast<Node*>(head);
    }

    bool IsClosed() const noexcept {
        return head_.Load<platform::MemoryOrder::Acquire>() == ClosedMarker();
    }

    // 가져간 목록의 다음 노드다.
    static Node* Next(Node* node) noexcept {
        return static_cast<Node*>(node->takeAllNext);
    }

    // 가져간 LIFO 목록을 뒤집어 넣은 순서(FIFO)로 만든다.
    static Node* Reverse(Node* list) noexcept {
        TakeAllStackNode* reversed = nullptr;
        TakeAllStackNode* current = list;
        while (current != nullptr) {
            TakeAllStackNode* next = current->takeAllNext;
            current->takeAllNext = reversed;
            reversed = current;
            current = next;
        }

        return static_cast<Node*>(reversed);
    }

private:
    // 닫힘 표지다. 실제 노드 주소와 겹치지 않는 이 스택 자신의 주소를 쓴다.
    TakeAllStackNode* ClosedMarker() const noexcept {
        return reinterpret_cast<TakeAllStackNode*>(const_cast<TakeAllStack*>(this));
    }

    platform::Atomic<TakeAllStackNode*> head_;
};

} // namespace rocks
