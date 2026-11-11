#include <Platform/VirtualMemory.h>
#include <stdio.h>

namespace rocks::test {

bool PrepareVirtualMemoryAccessProbe() noexcept;

// 독립 프로세스 전용이다. 준비 실패와 의도한 접근 위반을 별도로 식별한다.
int RunVirtualMemoryAccessProbe(uint32 state, bool write) noexcept {
    if (!PrepareVirtualMemoryAccessProbe()) {
        return 2;
    }
    platform::VirtualMemoryInfo info{};
    if (!platform::QueryVirtualMemoryInfo(info) || info.pageSize == 0) {
        return 3;
    }
    platform::VirtualMemoryRegion region;
    if (!region.Reserve(info.pageSize)) {
        return 4;
    }
    if (state != 0) {
        if (!region.Commit(0, info.pageSize)) {
            return 5;
        }
        auto* writable = static_cast<volatile uint8*>(region.Address());
        if (writable == nullptr) {
            return 6;
        }
        *writable = 42;
        if (state == 1 && !region.Decommit(0, info.pageSize)) {
            return 7;
        }
    }
    auto* address = static_cast<volatile uint8*>(region.Address());
    if (address == nullptr) {
        return 8;
    }
    // 오류 전 상태가 정상 준비되었음을 부모가 파이프로 확인한다.
    if (fprintf(stderr, "VM_ACCESS_READY\n") < 0 || fflush(stderr) != 0) {
        return 9;
    }
    if (write) {
        *address = 99;
    } else {
        const uint8 value = *address;
        if (state == 2 && value != 42) {
            return 10;
        }
    }
    return state == 2 ? 0 : 11;
}

} // namespace rocks::test
