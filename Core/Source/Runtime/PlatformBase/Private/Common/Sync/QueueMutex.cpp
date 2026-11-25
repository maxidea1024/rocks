#include "QueueMutex.h"

#include <Platform/Fatal.h>
#include <Platform/Sync/WaiterSignal.h>

namespace rocks::platform::detail {

namespace {

// 대기 스레드 스택의 노드다. 머리 노드의 queueTail이 꼬리를 가리킨다(머리만 그 칸이 맞다).
struct QueueNode {
    WaiterSignal signal;
    QueueNode* next = nullptr;
    QueueNode* queueTail = nullptr;
};

static_assert(alignof(QueueNode) > (QueueMutex::LockedBit | QueueMutex::QueueLockedBit));

// 대기자 없이 잠김만 본 때 잠깐 도는 횟수다(UE FWordMutex·WebKit WordLock과 같다).
constexpr uint32 SpinLimit = 40;

QueueNode* HeadOf(uptr word) noexcept {
    return reinterpret_cast<QueueNode*>(word & QueueMutex::QueueHeadMask);
}

} // namespace

// WebKit WordLock과 같은 알고리즘이다. 잠김이고 대기열이 비었으면 잠깐 돈다. 아니면 큐 잠금을 잡아
// 자기 노드를 꼬리에 붙이고 잔다. 깨면 처음부터 다시 잡는다(넘겨받지 않는다 — 공정하지 않다).
void QueueMutex::LockSlow() noexcept {
    uint32 spins = 0;
    while (true) {
        uptr current = word_.Load<MemoryOrder::Relaxed>();
        if ((current & LockedBit) == 0) {
            if (word_.CompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Relaxed>(
                    current, current | LockedBit)) {
                return;
            }

            continue;
        }

        if (HeadOf(current) == nullptr && spins < SpinLimit) {
            ++spins;
            CpuPause();
            continue;
        }

        // 큐 잠금은 잠김이 서 있을 때만 잡는다(풀린 잠금의 큐를 고치지 않는다).
        if ((current & QueueLockedBit) != 0 ||
            !word_.CompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Relaxed>(
                current, current | QueueLockedBit)) {
            CpuPause();
            continue;
        }

        QueueNode node;
        node.signal.Reset();
        QueueNode* head = HeadOf(current);
        uptr next = 0;
        if (head != nullptr) {
            head->queueTail->next = &node;
            head->queueTail = &node;
            next = current & ~QueueLockedBit;
        } else {
            node.queueTail = &node;
            next = (current | reinterpret_cast<uptr>(&node)) & ~QueueLockedBit;
        }

        // 큐 잠금을 쥔 동안 다른 스레드는 word를 바꾸지 못한다(잠김 bit가 서 있고 큐 잠금도 서
        // 있다).
        uptr expected = current | QueueLockedBit;
        if (!word_.CompareExchangeStrong<MemoryOrder::Release, MemoryOrder::Relaxed>(expected,
                                                                                     next)) {
            Fatal("QueueMutex word changed while the queue was locked");
        }

        node.signal.Wait();
        spins = 0;
    }
}

// 대기자가 있으면 큐 잠금을 잡고 머리를 떼어 잠김과 큐 잠금을 함께 푼 뒤 그 노드를 깨운다.
void QueueMutex::UnlockSlow() noexcept {
    uptr current = 0;
    while (true) {
        current = word_.Load<MemoryOrder::Relaxed>();
        if (current == LockedBit) {
            if (word_.CompareExchangeWeak<MemoryOrder::Release, MemoryOrder::Relaxed>(current, 0)) {
                return;
            }

            continue;
        }

        if ((current & QueueLockedBit) != 0) {
            CpuPause();
            continue;
        }

        if (word_.CompareExchangeWeak<MemoryOrder::Acquire, MemoryOrder::Relaxed>(
                current, current | QueueLockedBit)) {
            break;
        }
    }

    current |= QueueLockedBit;
    QueueNode* head = HeadOf(current);
    QueueNode* newHead = head->next;
    if (newHead != nullptr) {
        newHead->queueTail = head->queueTail;
    }

    uptr expected = current;
    if (!word_.CompareExchangeStrong<MemoryOrder::Release, MemoryOrder::Relaxed>(
            expected, reinterpret_cast<uptr>(newHead))) {
        Fatal("QueueMutex word changed while the queue was locked");
    }

    head->next = nullptr;
    head->queueTail = nullptr;
    // Notify 뒤로 head를 건드리지 않는다(대기자가 깨어 스택 노드가 사라진다).
    head->signal.Notify();
}

} // namespace rocks::platform::detail
