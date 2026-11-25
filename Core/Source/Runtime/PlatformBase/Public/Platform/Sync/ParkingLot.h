#pragma once

#include <Base/Compiler.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Platform/Sync/WaitTypes.h>

namespace rocks::platform {

// ParkingLot::Wait의 결과다.
struct ParkingLotWaitOutcome {
    // 큐에 들어가 잠들었다(canWait가 false면 false다).
    bool waited = false;
    // 깨우기로 깼다(false면 시간 초과이거나 자지 않았다).
    bool woken = false;
    // 깨운 쪽 WakeOne 콜백이 돌려준 값이다(그 밖에는 0).
    uint64 wakeToken = 0;
};

// 깨우기 콜백이 받는 결과다.
struct ParkingLotWakeOutcome {
    // 스레드를 하나 이상 깨웠다.
    bool wokeThread = false;
    // 같은 주소에 아직 잠든 스레드가 있을 수 있다. false면 그 주소의 대기자가 없다(콜백은 bucket
    // 잠금 아래에서 불리므로 이 판단과 콜백 사이에 새 대기자가 끼지 못한다).
    bool mayHaveMoreWaiters = false;
};

namespace detail {

using ParkingLotCanWait = bool (*)(void* context) noexcept;
using ParkingLotBeforeWait = void (*)(void* context) noexcept;
using ParkingLotWakeOneCallback = uint64 (*)(void* context, ParkingLotWakeOutcome outcome) noexcept;
using ParkingLotWakeCallback = void (*)(void* context, ParkingLotWakeOutcome outcome) noexcept;

ROCKS_PLATFORMBASE_API ParkingLotWaitOutcome
ParkingLotWait(const void* address, ParkingLotCanWait canWait, void* canWaitContext,
               ParkingLotBeforeWait beforeWait, void* beforeWaitContext, uint32 timeoutMs) noexcept;
ROCKS_PLATFORMBASE_API void ParkingLotWakeOne(const void* address, ParkingLotWakeOneCallback onWake,
                                              void* context) noexcept;
ROCKS_PLATFORMBASE_API uint32 ParkingLotWakeMultiple(const void* address, uint32 count,
                                                     ParkingLotWakeCallback onWake,
                                                     void* context) noexcept;

} // namespace detail

// 주소를 키로 쓰는 대기 큐다(UE UE::ParkingLot 대응, Docs/Design/Synchronization 10절). 1 byte
// Mutex·이벤트·조건 변수가 모두 이 위에 선다: 잠금 객체는 상태 word만 갖고, 기다릴 스레드는 그
// 주소로 여기서 잔다. 스레드마다 레코드 하나(WaiterSignal 하나)를 쓰므로 잠금 객체마다 OS 객체가
// 없다.
//
// 규칙(10.3절 대기 표시 규칙):
// - canWait는 bucket 잠금 아래에서 불린다. false면 자지 않고 돌아온다. 상태 word를 다시 확인하는
//   자리다(밖에서 세운 대기 표시가 아직 있고 기다릴 조건이 참인지).
// - beforeWait는 bucket을 푼 뒤 잠들기 직전에 불린다. 깨우기 함수만 부를 수 있다(조건 변수가 짝
//   Mutex를 푸는 자리).
// - 깨우기 콜백은 bucket 잠금 아래에서 불린다. 대기 표시를 지우는 유일한 자리다.
// - 콜백 안에서 ParkingLot 함수를 부르면 Debug·Development에서 Fatal이다(beforeWait의 깨우기는
// 허용).
// - 깨우기는 대기자가 Wait로 돌아가기 전에 끝난다. 그래서 대기자의 레코드·신호는 깨운 쪽이 다 쓴
//   뒤에만 재사용된다.
//
// 사용 예(잠금의 느린 경로):
//   ParkingLot::Wait(&state_,
//       [&]() noexcept { return state_.Load<MemoryOrder::Relaxed>() == (LockedBit | WaitingBit); },
//       []() noexcept {});
//   ParkingLot::WakeOne(&state_, [&](ParkingLotWakeOutcome outcome) noexcept -> uint64 {
//       if (!outcome.mayHaveMoreWaiters) { state_.FetchAnd<MemoryOrder::Relaxed>(~WaitingBit); }
//       return 0;
//   });
class ParkingLot {
public:
    using WaitOutcome = ParkingLotWaitOutcome;
    using WakeOutcome = ParkingLotWakeOutcome;

    // address 큐에 들어가 잔다. timeoutMs(InfiniteTimeout이면 무한) 안에 깨우지 않으면 큐에서 나와
    // 돌아온다. canWait: bool() noexcept, beforeWait: void() noexcept.
    template <class CanWait, class BeforeWait>
    static WaitOutcome Wait(const void* address, CanWait&& canWait, BeforeWait&& beforeWait,
                            uint32 timeoutMs = InfiniteTimeout) noexcept {
        using CanWaitType = RemoveReferenceT<CanWait>;
        using BeforeWaitType = RemoveReferenceT<BeforeWait>;
        return detail::ParkingLotWait(
            address, [](void* context) noexcept { return (*static_cast<CanWaitType*>(context))(); },
            const_cast<void*>(static_cast<const volatile void*>(&canWait)),
            [](void* context) noexcept { (*static_cast<BeforeWaitType*>(context))(); },
            const_cast<void*>(static_cast<const volatile void*>(&beforeWait)), timeoutMs);
    }

    // 대기자 하나를 깨운다. onWake: uint64(WakeOutcome) noexcept — bucket 잠금 아래에서 한 번
    // 불리고, 돌려준 값이 깬 스레드의 wakeToken이다(깬 스레드가 없어도 불린다).
    template <class OnWake>
    static void WakeOne(const void* address, OnWake&& onWake) noexcept {
        using OnWakeType = RemoveReferenceT<OnWake>;
        detail::ParkingLotWakeOne(
            address,
            [](void* context, WakeOutcome outcome) noexcept -> uint64 {
                return (*static_cast<OnWakeType*>(context))(outcome);
            },
            const_cast<void*>(static_cast<const volatile void*>(&onWake)));
    }

    // 대기자 하나를 깨우고 결과를 돌려준다(콜백 없는 판).
    static WakeOutcome WakeOne(const void* address) noexcept {
        WakeOutcome result;
        WakeOne(address, [&](WakeOutcome outcome) noexcept -> uint64 {
            result = outcome;
            return 0;
        });
        return result;
    }

    // 최대 count개를 깨우고 깬 수를 돌려준다. onWake: void(WakeOutcome) noexcept — 꺼내기를 마친 뒤
    // bucket 잠금 아래에서 한 번 불린다(대기 표시를 지우는 자리, 10.3절).
    template <class OnWake>
    static uint32 WakeMultiple(const void* address, uint32 count, OnWake&& onWake) noexcept {
        using OnWakeType = RemoveReferenceT<OnWake>;
        return detail::ParkingLotWakeMultiple(
            address, count,
            [](void* context, WakeOutcome outcome) noexcept {
                (*static_cast<OnWakeType*>(context))(outcome);
            },
            const_cast<void*>(static_cast<const volatile void*>(&onWake)));
    }

    template <class OnWake>
    static uint32 WakeAll(const void* address, OnWake&& onWake) noexcept {
        return WakeMultiple(address, NumericLimits<uint32>::Max(), static_cast<OnWake&&>(onWake));
    }

    static uint32 WakeMultiple(const void* address, uint32 count) noexcept {
        return detail::ParkingLotWakeMultiple(address, count, nullptr, nullptr);
    }

    static uint32 WakeAll(const void* address) noexcept {
        return WakeMultiple(address, NumericLimits<uint32>::Max());
    }
};

} // namespace rocks::platform
