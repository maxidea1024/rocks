#pragma once

#include <Platform/VirtualMemory.h>

namespace rocks::platform::detail {

// 검증과 수명 관리는 호출자의 책임이다. 운영체제 타입을 공개 헤더로 전달하지 않는다.
VirtualMemoryResult ReserveVirtualMemory(usize bytes, void*& address) noexcept;
VirtualMemoryResult CommitVirtualMemory(void* address, usize bytes) noexcept;
VirtualMemoryResult DecommitVirtualMemory(void* address, usize bytes) noexcept;
VirtualMemoryResult ReleaseVirtualMemory(void* address, usize bytes) noexcept;

} // namespace rocks::platform::detail
