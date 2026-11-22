#pragma once

#include <Core/Containers/Array.h>
#include <Graphics/DeviceTypes.h>

namespace rocks::graphics {

// backend가 resolve해 둔 timestamp 결과를 읽어 주는 쪽이다(TimestampRing::Harvest가 부른다).
class TimestampSource {
public:
    // query pool의 [first, first + count) 결과를 ticks에 쓴다. 결과가 아직 없으면 NotReady를
    // 돌려주고 ticks는 무시된다(다음 Harvest에서 다시 묻는다). Disjoint면 그 구간 값이 무효다.
    virtual TimestampReadState ReadTimestamps(uint32 first, uint32 count,
                                              uint64* ticks) noexcept = 0;

protected:
    ~TimestampSource() = default;
};

// timestamp query를 프레임 slot 링으로 나눠 쓰고, GPU 완료 뒤 비차단으로 회수하는
// backend 공용 장부다(Docs/Design/RenderBase.ko.md 13.2절, GX9). query pool은 slot × queriesPerSlot
// 칸이며 slot 하나가 제출 하나(프레임)에 대응한다.
//
// 사용 예(backend):
//   ring.Reset(TimestampFrameSlots, MaxTimestampsPerFrame);
//   auto write = ring.Allocate();                 // 기록 중: query 칸 하나
//   if (write) { WriteQuery(write.Value().index); return write.Value().query; }
//   const TimestampRange range = ring.OpenRange(); // 제출 직전: resolve할 구간
//   ring.Close(signaledFence, true);              // 제출 뒤
//   ring.Harvest(completedFence, source);         // 완료 뒤(비차단)
//   usize n = ring.Collect(out);                  // 쓴 순서대로 결과
//
// 계약:
// - 닫힌 slot은 fence가 끝났다고 Harvest가 본 뒤에만 읽고 비운다. 그래서 GPU 완료 전 값은 없다.
// - 빈 slot이 없으면(앞 프레임들이 아직 회수 전) Allocate는 기다리지 않고 Busy다.
// - 회수한 결과는 내부 목록에 쌓이며, 목록을 늘리지 못하면 그 결과를 버리고 Dropped를 올린다.
// - 스레드 안전하지 않다. backend가 장치 소유 스레드(와 자기 잠금) 안에서 쓴다.
class ROCKS_GRAPHICS_API TimestampRing {
public:
    explicit TimestampRing(Allocator& allocator) noexcept;

    // slot 수와 slot당 query 수를 정하고 비운다. 둘 중 하나가 0이면 InvalidArgument다. 할당 실패는
    // 그 오류다.
    Result<void> Reset(uint32 slots, uint32 queriesPerSlot) noexcept;

    // 열린 slot에 query 칸 하나를 준다. 열린 slot이 없으면 다음 빈 slot을 연다. 실패: Reset 전이면
    // Unsupported, 빈 slot이 없으면 Busy, 열린 slot이 가득 찼으면 CapacityExceeded.
    Result<TimestampWrite> Allocate() noexcept;

    // 열린 slot이 쓴 pool 구간이다(제출 전 resolve용). 열린 slot이 없으면 count 0이다.
    TimestampRange OpenRange() const noexcept;

    // 열린 slot을 fence로 닫는다. valid가 false면(제출 실패 등) 그 slot 결과를 모두 disjoint로
    // 낸다. 열린 slot이 없으면 아무것도 하지 않는다.
    void Close(uint64 fence, bool valid) noexcept;

    // fence ≤ completed인 닫힌 slot을 오래된 것부터 읽어 결과 목록에 넣고 비운다. 원본이
    // NotReady면 그 slot에서 멈춘다(다음 호출에서 다시).
    void Harvest(uint64 completed, TimestampSource& source) noexcept;

    // 쌓인 결과를 쓴 순서대로 out 앞에서부터 옮기고 옮긴 수를 돌려준다. 남은 결과는 다음 호출로
    // 간다.
    usize Collect(Span<TimestampSample> out) noexcept;

    // slot이 비어 있는지다(열리지도 닫히지도 않았다). 범위 밖이면 false다. query를 다시 쓰기 전에
    // 초기화해야 하는 backend(Vulkan vkCmdResetQueryPool)가 다음 기록에서 초기화할 slot을 고른다.
    bool IsSlotFree(uint32 slot) const noexcept {
        return slot < slots_.Size() && slots_[slot].state == SlotState::Free;
    }

    // slot당 query 수다(Reset 값).
    uint32 QueriesPerSlot() const noexcept {
        return perSlot_;
    }

    // 아직 Collect하지 않은 결과 수다.
    usize Ready() const noexcept {
        return samples_.Size();
    }

    // 결과 목록을 늘리지 못해 버린 결과 수다.
    uint64 Dropped() const noexcept {
        return dropped_;
    }

private:
    enum class SlotState : uint8 {
        Free,
        Open,
        Closed
    };

    struct Slot {
        SlotState state = SlotState::Free;
        bool valid = true;
        uint32 count = 0;
        uint64 fence = 0;
        uint64 firstQuery = 0;
    };

    Array<Slot> slots_;
    Array<TimestampSample> samples_;
    Array<uint64> scratch_;
    uint32 perSlot_ = 0;
    // 다음에 열 slot, 가장 오래된 사용 중 slot 위치와 사용 중(열림·닫힘) slot 수다. 열린 slot은
    // 언제나 가장 최근에 연 slot이다.
    uint32 next_ = 0;
    uint32 oldest_ = 0;
    uint32 inUse_ = 0;
    uint64 nextQuery_ = 1;
    uint64 dropped_ = 0;
};

} // namespace rocks::graphics
