#include "VirtualMemoryBackend.h"

#include <Platform/Fatal.h>

namespace rocks::platform {
namespace {

VirtualMemoryResult Success() noexcept {
    return {VirtualMemoryCode::Success, 0};
}
VirtualMemoryResult VirtualMemoryError(VirtualMemoryCode code) noexcept {
    return {code, 0};
}

// 이 스레드의 주입 계획에서 이번 호출이 실패할 차례인지 본다.
bool TakeInjectedFault(bool commit) noexcept {
    detail::VirtualMemoryFaults& faults = detail::virtualMemoryFaults;
    uint64& calls = commit ? faults.commitCalls : faults.decommitCalls;
    const uint64 failing = commit ? faults.failCommitCall : faults.failDecommitCall;
    ++calls;
    return failing != 0 && calls == failing;
}

} // namespace

namespace detail {

thread_local VirtualMemoryFaults virtualMemoryFaults;

VirtualMemoryFaults& ThisThreadVirtualMemoryFaults() noexcept {
    return virtualMemoryFaults;
}

} // namespace detail

VirtualMemoryRegion::~VirtualMemoryRegion() noexcept {
    if (!Release()) {
        Fatal("VirtualMemoryRegion release failed during destruction");
    }
}

VirtualMemoryResult VirtualMemoryRegion::Reserve(usize bytes, VirtualMemoryAccess access) noexcept {
    LockGuard lock(mutex_);
    if (address_ != nullptr) {
        return VirtualMemoryError(VirtualMemoryCode::AlreadyReserved);
    }

    VirtualMemoryInfo info{};
    const auto queried = QueryVirtualMemoryInfo(info);
    if (!queried) {
        return queried;
    }
    if (info.pageSize == 0 || bytes == 0 || bytes % info.pageSize != 0) {
        return VirtualMemoryError(VirtualMemoryCode::InvalidArgument);
    }

    void* address = nullptr;
    const auto result = detail::ReserveVirtualMemory(bytes, access, address);
    if (!result) {
        return result;
    }

    address_ = address;
    size_ = bytes;
    pageSize_ = info.pageSize;
    access_ = access;
    poisoned_ = false;
    return Success();
}

VirtualMemoryResult VirtualMemoryRegion::Commit(usize offset, usize bytes) noexcept {
    return Change(offset, bytes, true);
}

VirtualMemoryResult VirtualMemoryRegion::Decommit(usize offset, usize bytes) noexcept {
    return Change(offset, bytes, false);
}

VirtualMemoryResult VirtualMemoryRegion::Change(usize offset, usize bytes, bool commit) noexcept {
    LockGuard lock(mutex_);
    if (address_ == nullptr) {
        return VirtualMemoryError(VirtualMemoryCode::NotReserved);
    }
    if (poisoned_) {
        return VirtualMemoryError(VirtualMemoryCode::Poisoned);
    }
    // 뺄셈으로 먼저 범위를 검사해 offset + bytes가 overflow하지 않도록 한다.
    if (bytes == 0 || offset % pageSize_ != 0 || bytes % pageSize_ != 0 || offset > size_ ||
        bytes > size_ - offset) {
        return VirtualMemoryError(VirtualMemoryCode::InvalidArgument);
    }

    if (TakeInjectedFault(commit)) {
        return {VirtualMemoryCode::OsFailure, detail::InjectedVirtualMemoryError};
    }

    // 실패해도 영역을 닫지 않는다: commit과 decommit은 실패가 어떤 상태를 남기든 같은 범위에 다시
    // 할 수 있고 다른 범위에는 영향이 없다. 실패한 범위를 어떻게 다룰지는 호출자가
    // 계약(VirtualMemory.h)에 따라 정한다.
    auto* start = static_cast<uint8*>(address_) + offset;
    return commit ? detail::CommitVirtualMemory(start, bytes, access_)
                  : detail::DecommitVirtualMemory(start, bytes, access_);
}

VirtualMemoryResult VirtualMemoryRegion::Release() noexcept {
    LockGuard lock(mutex_);
    if (address_ == nullptr) {
        return Success();
    }

    const auto result = detail::ReleaseVirtualMemory(address_, size_);
    if (!result) {
        poisoned_ = true;
        return result;
    }

    address_ = nullptr;
    size_ = 0;
    pageSize_ = 0;
    access_ = VirtualMemoryAccess::Guarded;
    poisoned_ = false;
    return Success();
}

void* VirtualMemoryRegion::Address() noexcept {
    LockGuard lock(mutex_);
    return poisoned_ ? nullptr : address_;
}

usize VirtualMemoryRegion::Size() noexcept {
    LockGuard lock(mutex_);
    return size_;
}

bool VirtualMemoryRegion::IsPoisoned() noexcept {
    LockGuard lock(mutex_);
    return poisoned_;
}

} // namespace rocks::platform
