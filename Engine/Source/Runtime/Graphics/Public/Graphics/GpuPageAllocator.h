#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>

namespace rocks::graphics {

// 페이지 안에서 잡은 구간이다. offset이 정렬된 시작이고, reserved*는 정렬 앞 칸까지 포함한 실제
// 차지 구간이다(Free에 그대로 돌려준다).
struct GpuPageAllocation {
    uint64 offset = 0;
    uint64 size = 0;
    uint64 reservedOffset = 0;
    uint64 reservedSize = 0;
};

// GPU 메모리 페이지 하나의 구간 장부다(Docs/Design/VulkanGraphics.ko.md 5절). backend가 큰 메모리
// 덩어리(Vulkan VkDeviceMemory)를 잡고, 이 장부가 그 안의 byte 구간을 나눈다. API 객체를 모르므로
// CPU 시험으로 확인한다.
//
// 사용 예(backend):
//   GpuPageAllocator page(allocator);
//   page.Reset(64 << 20);
//   auto block = page.Allocate(requirements.size, requirements.alignment);
//   if (!block) { /* CapacityExceeded: 다른 페이지나 새 페이지 */ }
//   vkBindBufferMemory(device, buffer, memory, block.Value().offset);
//   ...
//   page.Free(block.Value());              // GPU가 다 쓴 뒤(회수 fence 뒤)
//
// 계약:
// - first-fit: 빈 구간을 offset 순으로 보며 정렬을 맞춰 들어가는 첫 구간을 쓴다.
// - Free는 이웃 빈 구간과 합친다. 그래서 모두 놓으면 빈 구간이 하나(전체)다.
// - 잘못된 Free(겹침·범위 밖)는 이후 할당이 GPU 메모리를 겹쳐 주게 하므로 Fatal이다.
// - 스레드 안전하지 않다. backend가 자기 잠금 안에서 쓴다.
class GpuPageAllocator {
public:
    explicit GpuPageAllocator(Allocator& allocator) noexcept;

    // 용량을 정하고 비운다. 0은 InvalidArgument, 할당 실패는 그 오류다.
    Result<void> Reset(uint64 capacity) noexcept;

    // size byte를 alignment(2의 거듭제곱) 정렬로 잡는다. 실패: 크기 0·잘못된 정렬 InvalidArgument,
    // 들어갈 빈 구간이 없으면 CapacityExceeded, 빈 구간 목록을 늘리지 못하면 그 오류다.
    Result<GpuPageAllocation> Allocate(uint64 size, uint64 alignment) noexcept;

    // Allocate가 준 구간을 돌려준다.
    void Free(const GpuPageAllocation& allocation) noexcept;

    uint64 Capacity() const noexcept {
        return capacity_;
    }

    uint64 UsedBytes() const noexcept {
        return used_;
    }

    bool IsEmpty() const noexcept {
        return used_ == 0;
    }

    // 빈 구간 수다(조각화 진단·시험).
    usize FreeRangeCount() const noexcept {
        return free_.Size();
    }

private:
    struct Range {
        uint64 offset = 0;
        uint64 size = 0;
    };

    // offset 순 빈 구간이다.
    Array<Range> free_;
    uint64 capacity_ = 0;
    uint64 used_ = 0;
};

} // namespace rocks::graphics
