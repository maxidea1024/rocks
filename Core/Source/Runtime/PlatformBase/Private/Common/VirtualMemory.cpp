#include "VirtualMemoryBackend.h"

#include <Platform/Fatal.h>

namespace rocks::platform {
namespace {

VirtualMemoryResult Success() noexcept {
    return {VirtualMemoryCode::Success, 0};
}
VirtualMemoryResult Error(VirtualMemoryCode code) noexcept {
    return {code, 0};
}

} // namespace

VirtualMemoryRegion::~VirtualMemoryRegion() noexcept {
    if (!Release()) {
        Fatal("VirtualMemoryRegion release failed during destruction");
    }
}

VirtualMemoryResult VirtualMemoryRegion::Reserve(usize bytes) noexcept {
    LockGuard lock(mutex_);
    if (address_ != nullptr) {
        return Error(VirtualMemoryCode::AlreadyReserved);
    }

    VirtualMemoryInfo info{};
    const auto queried = QueryVirtualMemoryInfo(info);
    if (!queried) {
        return queried;
    }
    if (info.pageSize == 0 || bytes == 0 || bytes % info.pageSize != 0) {
        return Error(VirtualMemoryCode::InvalidArgument);
    }

    void* address = nullptr;
    const auto result = detail::ReserveVirtualMemory(bytes, address);
    if (!result) {
        return result;
    }

    address_ = address;
    size_ = bytes;
    pageSize_ = info.pageSize;
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
        return Error(VirtualMemoryCode::NotReserved);
    }
    if (poisoned_) {
        return Error(VirtualMemoryCode::Poisoned);
    }
    // 뺄셈으로 먼저 범위를 검사해 offset + bytes가 overflow하지 않도록 한다.
    if (bytes == 0 || offset % pageSize_ != 0 || bytes % pageSize_ != 0 || offset > size_ ||
        bytes > size_ - offset) {
        return Error(VirtualMemoryCode::InvalidArgument);
    }

    auto* start = static_cast<uint8*>(address_) + offset;
    const auto result = commit ? detail::CommitVirtualMemory(start, bytes)
                               : detail::DecommitVirtualMemory(start, bytes);
    if (!result) {
        // 보호 변경과 페이지 반환이 여러 단계일 수 있어 실패 후 이전 상태를 가정하지 않는다.
        poisoned_ = true;
    }
    return result;
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
