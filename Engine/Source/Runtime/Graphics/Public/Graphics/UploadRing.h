#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>

namespace rocks::graphics {

// 업로드 ring의 공간 할당기다(RenderBase 4.4절 GX8 "프레임 slot별 업로드 ring"). backend가 CPU 가시
// 버퍼 하나(D3D12 upload heap)를 잡고, 이 할당기가 그 안의 byte 구간을 나눠 준다. 구간은 그것을
// 쓰는 제출 묶음(batch)이 닫힐 때 fence 값을 받고, 그 fence가 끝나야 다시 쓴다. 프레임 slot마다 한
// 묶음이 닫히므로 프레임 slot별 ring과 같은 효과다.
//
// 사용 예(backend):
//   auto offset = ring.Allocate(bytes, 512);
//   if (!offset) { /* 꽉 참: 오래된 fence를 기다려 Reclaim하거나 전용 staging을 쓴다 */ }
//   platform::MemoryCopy(mapped + offset.Value(), data, bytes);   // map 쓰기
//   list->CopyBufferRegion(target, 0, ringBuffer, offset.Value(), bytes);   // 복사 기록
//   ...
//   ring.CloseBatch(signaledFence);     // 제출 뒤
//   ring.Reclaim(completedFence);       // 완료 뒤
//
// 계약:
// - 열린 묶음(아직 CloseBatch하지 않은 할당)은 회수하지 않는다. 그래서 fence 전 재사용은 없다.
// - 할당은 연속 구간이다. 끝에 자리가 없으면 0으로 감는다(wrap). 감을 때 끝에 남은 칸은 그 묶음이
//   함께 붙잡는다. 다 회수돼 비어도 위치를 0으로 되돌리지 않고 이어 쓰므로 링 전체를 고르게 돈다.
// - 스레드 안전하지 않다. backend가 자기 잠금 안에서 쓴다.
class ROCKS_GRAPHICS_API UploadRingAllocator {
public:
    explicit UploadRingAllocator(Allocator& allocator) noexcept;

    // 용량을 정하고 비운다(감은 횟수도 0으로). 0은 InvalidArgument다.
    Result<void> Reset(uint64 capacity) noexcept;

    // size byte를 alignment(2의 거듭제곱) 정렬로 잡는다. 실패: 크기 0·용량 초과·잘못된 정렬
    // InvalidArgument, 지금 빈 공간이 없으면 CapacityExceeded(앞 묶음의 fence를 기다린 뒤
    // Reclaim하면 생긴다). 묶음 기록 칸을 늘리지 못하면 그 오류다.
    Result<uint64> Allocate(uint64 size, uint64 alignment) noexcept;

    // 지금까지 할당한 열린 구간을 fence 값의 묶음으로 닫는다. 열린 구간이 없으면 아무것도 하지
    // 않는다. fence는 앞 묶음보다 크거나 같아야 한다.
    void CloseBatch(uint64 fence) noexcept;

    // completed 이하 fence의 묶음 공간을 돌려준다.
    void Reclaim(uint64 completed) noexcept;

    // 닫힌 묶음 가운데 가장 오래된 것의 fence다(없으면 0). 꽉 찼을 때 기다릴 값이다.
    uint64 OldestFence() const noexcept;

    bool HasOpenAllocations() const noexcept {
        return openBytes_ != 0;
    }

    uint64 Capacity() const noexcept {
        return capacity_;
    }

    // 쓰는 중인 byte 수(열린 구간과 회수 전 묶음, 감을 때 버린 끝 칸 포함)다.
    uint64 UsedBytes() const noexcept {
        return used_;
    }

    // 0으로 감은 횟수다(진단·시험).
    uint64 Wraps() const noexcept {
        return wraps_;
    }

private:
    struct Batch {
        uint64 end = 0;
        uint64 bytes = 0;
        uint64 fence = 0;
    };

    Array<Batch> batches_;
    uint64 capacity_ = 0;
    // 다음 할당 위치와 가장 오래된 사용 구간의 시작이다. used_가 0이면 head_ = tail_이고 링 전체가
    // 비어 있다.
    uint64 head_ = 0;
    uint64 tail_ = 0;
    uint64 used_ = 0;
    uint64 openBytes_ = 0;
    uint64 wraps_ = 0;
};

} // namespace rocks::graphics
