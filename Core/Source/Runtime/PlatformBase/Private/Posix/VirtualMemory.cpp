#include "../Common/VirtualMemoryBackend.h"
#include "PosixPlatform.h"

#include <Platform/Fatal.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

namespace rocks::platform {
namespace {

VirtualMemoryResult Success() noexcept {
    return {VirtualMemoryCode::Success, 0};
}
VirtualMemoryResult OsFailure() noexcept {
    return {VirtualMemoryCode::OsFailure, static_cast<uint32>(errno)};
}

} // namespace

VirtualMemoryResult QueryVirtualMemoryInfo(VirtualMemoryInfo& info) noexcept {
    errno = 0;
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        return OsFailure();
    }
    info = {static_cast<usize>(pageSize), static_cast<usize>(pageSize)};
    return Success();
}

namespace detail {

VirtualMemoryResult ReserveVirtualMemory(usize bytes, void*& address) noexcept {
    void* result = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (result == MAP_FAILED) {
        return OsFailure();
    }
    // null은 공개 API에서 미예약 상태를 뜻한다. OS가 0번지를 매핑하면 소유권을
    // 잃지 않도록 여기서 해제하고 실패를 반환한다. 해제 실패는 조용히 누출하지 않는다.
    if (result == nullptr) {
        if (munmap(result, bytes) != 0) {
            Fatal("Failed to release unsupported null virtual memory mapping");
        }
        return {VirtualMemoryCode::UnsupportedAddress, 0};
    }
    address = result;
    return Success();
}

VirtualMemoryResult CommitVirtualMemory(void* address, usize bytes) noexcept {
    if (mprotect(address, bytes, PROT_READ | PROT_WRITE) != 0) {
        return OsFailure();
    }
    return Success();
}

VirtualMemoryResult DecommitVirtualMemory(void* address, usize bytes) noexcept {
    if (mprotect(address, bytes, PROT_NONE) != 0) {
        return OsFailure();
    }
    // 반환 힌트다(OS별 값은 PosixDecommitAdvice). 실패하면 이미 접근이 차단되었으므로 상위 영역을
    // 격리한다.
    if (madvise(address, bytes, detail::PosixDecommitAdvice()) != 0) {
        return OsFailure();
    }
    return Success();
}

VirtualMemoryResult ReleaseVirtualMemory(void* address, usize bytes) noexcept {
    if (munmap(address, bytes) != 0) {
        return OsFailure();
    }
    return Success();
}

} // namespace detail
} // namespace rocks::platform
