#pragma once

#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/StepPolicy.h>
#include <Platform/Atomics.h>

namespace rocks {

// IntrusiveMpscQueue에 넣는 노드의 링크다. 노드 타입이 이것을 상속한다.
struct IntrusiveMpscNode {
    platform::Atomic<IntrusiveMpscNode*> mpscNext;
};

// 생산자 여럿·소비자 하나의 무한 침투형 큐다(Vyukov intrusive MPSC, UE TMpscQueue 대응 —
// Docs/Design/Synchronization 12.2절). 노드는 호출자 소유이고 링크를 노드가 갖는다. 그래서 push에
// 할당이 없다(미리 할당한 큐 항목 — 에셋 lease 해제 큐, 지연 정리 목록).
//
// 성질:
// - Push는 exchange 하나와 store 하나다(생산자 wait-free).
// - TryPop은 소비자 한 스레드만 부른다. 비었으면 nullptr이다. 생산자가 exchange와 링크 store
// 사이에서
//   멈추면 그 뒤의 노드는 그 생산자가 링크를 마칠 때까지 보이지 않는다(TryPop이 nullptr). 짧은
//   구간이라 소비자는 다음 차례에 다시 본다.
// - 노드 수명: 넣은 노드는 TryPop이 돌려줄 때까지 살아 있어야 한다. 소비자가 하나라 ABA가 없다.
// - 큐 안에 stub 노드가 있어 큐 객체는 옮길 수 없다.
//
// 메모리 순서: 생산자의 링크 store(release)와 소비자의 링크 load(acquire)가 노드 내용을 발행한다.
//
// 사용 예:
//   struct Release : IntrusiveMpscNode { Handle handle; };
//   IntrusiveMpscQueue<Release> queue;
//   queue.Push(&item);                 // 어느 스레드에서나
//   while (Release* next = queue.TryPop()) { Handle(next); }   // 소비자
template <class Node, class StepPolicy = NoSteps>
class IntrusiveMpscQueue {
    static_assert(IsBaseOfV<IntrusiveMpscNode, Node>, "Node must derive from IntrusiveMpscNode");

public:
    IntrusiveMpscQueue() noexcept {
        stub_.mpscNext.Store<platform::MemoryOrder::Relaxed>(nullptr);
        head_.Store<platform::MemoryOrder::Relaxed>(&stub_);
        tail_ = &stub_;
    }

    IntrusiveMpscQueue(const IntrusiveMpscQueue&) = delete;
    IntrusiveMpscQueue& operator=(const IntrusiveMpscQueue&) = delete;

    // node를 뒤에 넣는다(어느 스레드에서나). node는 다른 큐에 들어 있으면 안 된다.
    void Push(Node* node) noexcept {
        PushLink(node);
    }

    // 맨 앞 노드를 꺼낸다. 비었거나 맨 앞 생산자가 아직 링크를 마치지 않았으면 nullptr이다.
    Node* TryPop() noexcept {
        IntrusiveMpscNode* tail = tail_;
        IntrusiveMpscNode* next = tail->mpscNext.Load<platform::MemoryOrder::Acquire>();
        if (tail == &stub_) {
            if (next == nullptr) {
                return nullptr;
            }

            tail_ = next;
            tail = next;
            next = next->mpscNext.Load<platform::MemoryOrder::Acquire>();
        }

        if (next != nullptr) {
            tail_ = next;
            return static_cast<Node*>(tail);
        }

        StepPolicy::Step("IntrusiveMpscQueue.PopLastNode");
        if (tail != head_.Load<platform::MemoryOrder::Acquire>()) {
            // 다른 생산자가 head를 바꿨지만 아직 링크를 쓰지 않았다.
            return nullptr;
        }

        // 마지막 노드를 꺼내려면 stub을 뒤에 다시 넣어 tail 뒤를 채운다.
        PushLink(&stub_);
        next = tail->mpscNext.Load<platform::MemoryOrder::Acquire>();
        if (next != nullptr) {
            tail_ = next;
            return static_cast<Node*>(tail);
        }

        return nullptr;
    }

    // 소비자 스레드에서: 꺼낼 노드가 확실히 없는지다(생산자가 링크 중이면 false일 수 있다).
    bool IsEmpty() const noexcept {
        return tail_ == &stub_ &&
               stub_.mpscNext.Load<platform::MemoryOrder::Acquire>() == nullptr &&
               head_.Load<platform::MemoryOrder::Acquire>() == &stub_;
    }

private:
    void PushLink(IntrusiveMpscNode* node) noexcept {
        node->mpscNext.Store<platform::MemoryOrder::Relaxed>(nullptr);
        IntrusiveMpscNode* previous = head_.Exchange<platform::MemoryOrder::AcquireRelease>(node);
        StepPolicy::Step("IntrusiveMpscQueue.PushExchanged");
        previous->mpscNext.Store<platform::MemoryOrder::Release>(node);
    }

    alignas(CacheLineSize) platform::Atomic<IntrusiveMpscNode*> head_;
    alignas(CacheLineSize) IntrusiveMpscNode* tail_;
    IntrusiveMpscNode stub_;
};

} // namespace rocks
