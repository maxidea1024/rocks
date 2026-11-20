#include "../Common/VirtualMemoryBackend.h"
#include "PosixPlatform.h"

#include <Base/Compiler.h>
#include <Platform/Fatal.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#if ROCKS_PLATFORM_MAC
#include <sys/sysctl.h>
#endif

namespace rocks::platform {
namespace {

VirtualMemoryResult VirtualMemoryLocalSuccess() noexcept {
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
    return VirtualMemoryLocalSuccess();
}

VirtualMemoryResult QueryPhysicalMemory(uint64& totalBytes) noexcept {
#if ROCKS_PLATFORM_MAC
    uint64_t bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0) {
        return OsFailure();
    }

    totalBytes = static_cast<uint64>(bytes);
#else
    errno = 0;
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageSize <= 0) {
        return OsFailure();
    }

    totalBytes = static_cast<uint64>(pages) * static_cast<uint64>(pageSize);
#endif
    return VirtualMemoryLocalSuccess();
}

namespace detail {

VirtualMemoryResult ReserveVirtualMemory(usize bytes, VirtualMemoryAccess access,
                                         void*& address) noexcept {
    // Open은 읽기·쓰기 매핑 하나로 잡는다(UE 5.8 UnixPlatformMemory.cpp
    // 635·AndroidPlatformMemory.cpp 618·ApplePlatformMemory.cpp 753과 같다). 물리 메모리는 처음 쓸
    // 때 붙고, MAP_NORESERVE로 swap 약정을 미룬다. 뒤의 Commit·Decommit이 보호를 바꾸지 않아 매핑이
    // 쪼개지지 않는다.
    const bool open = access == VirtualMemoryAccess::Open;
    void* result = mmap(nullptr, bytes, open ? PROT_READ | PROT_WRITE : PROT_NONE,
                        MAP_PRIVATE | MAP_ANON | (open ? MAP_NORESERVE : 0), -1, 0);
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
    return VirtualMemoryLocalSuccess();
}

VirtualMemoryResult CommitVirtualMemory(void* address, usize bytes,
                                        VirtualMemoryAccess access) noexcept {
    if (access == VirtualMemoryAccess::Open) {
        // 접근은 이미 열려 있다. OS별 힌트만 준다(Apple은 다시 쓴다고 알림, 그 밖은 할 일이 없다).
        detail::PosixOpenCommit(address, bytes);
        return VirtualMemoryLocalSuccess();
    }

    if (mprotect(address, bytes, PROT_READ | PROT_WRITE) != 0) {
        return OsFailure();
    }
    return VirtualMemoryLocalSuccess();
}

VirtualMemoryResult DecommitVirtualMemory(void* address, usize bytes,
                                          VirtualMemoryAccess access) noexcept {
    if (access == VirtualMemoryAccess::Open) {
        // 보호는 그대로 두고 페이지만 돌려준다. 실패하면 범위는 그대로다(접근·내용 유지).
        if (detail::PosixOpenDecommit(address, bytes) != 0) {
            return OsFailure();
        }

        return VirtualMemoryLocalSuccess();
    }

    if (mprotect(address, bytes, PROT_NONE) != 0) {
        return OsFailure();
    }
    // 반환 힌트다(OS별 값은 PosixDecommitAdvice). 실패하면 접근은 이미 막혔고 페이지만 돌아가지
    // 않은 것이다 — 호출자는 이 범위를 commit되지 않은 것으로 다룬다(VirtualMemory.h).
    if (madvise(address, bytes, detail::PosixDecommitAdvice()) != 0) {
        return OsFailure();
    }
    return VirtualMemoryLocalSuccess();
}

VirtualMemoryResult ReleaseVirtualMemory(void* address, usize bytes) noexcept {
    if (munmap(address, bytes) != 0) {
        return OsFailure();
    }
    return VirtualMemoryLocalSuccess();
}

} // namespace detail
} // namespace rocks::platform
