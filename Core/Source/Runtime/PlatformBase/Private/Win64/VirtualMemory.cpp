#include "../Common/VirtualMemoryBackend.h"

#include <Windows.h>

namespace rocks::platform {
namespace {

VirtualMemoryResult Success() noexcept {
    return {VirtualMemoryCode::Success, 0};
}
VirtualMemoryResult OsFailure() noexcept {
    return {VirtualMemoryCode::OsFailure, static_cast<uint32>(GetLastError())};
}

} // namespace

VirtualMemoryResult QueryVirtualMemoryInfo(VirtualMemoryInfo& info) noexcept {
    SYSTEM_INFO native{};
    GetSystemInfo(&native);
    if (native.dwPageSize == 0 || native.dwAllocationGranularity == 0) {
        return {VirtualMemoryCode::OsFailure, ERROR_INVALID_DATA};
    }
    info = {static_cast<usize>(native.dwPageSize),
            static_cast<usize>(native.dwAllocationGranularity)};
    return Success();
}

namespace detail {

VirtualMemoryResult ReserveVirtualMemory(usize bytes, void*& address) noexcept {
    void* result = VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (result == nullptr) {
        return OsFailure();
    }
    address = result;
    return Success();
}

VirtualMemoryResult CommitVirtualMemory(void* address, usize bytes) noexcept {
    if (VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        return OsFailure();
    }
    return Success();
}

VirtualMemoryResult DecommitVirtualMemory(void* address, usize bytes) noexcept {
    if (!VirtualFree(address, bytes, MEM_DECOMMIT)) {
        return OsFailure();
    }
    return Success();
}

VirtualMemoryResult ReleaseVirtualMemory(void* address, usize bytes) noexcept {
    (void)bytes;
    // MEM_RELEASE에는 최초 예약 주소와 size 0이 필요하다.
    if (!VirtualFree(address, 0, MEM_RELEASE)) {
        return OsFailure();
    }
    return Success();
}

} // namespace detail
} // namespace rocks::platform
