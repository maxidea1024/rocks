#pragma once

#include <Core/Memory/Allocator.h>

// 외부 라이브러리(OpenEXRCore·libtiff) 할당을 Rocks Allocator로
// 잇는다(Docs/Design/AssetSystem 12.10절 공통 규칙 1·2). 라이브러리 할당 콜백은 user_data가
// 없으므로 디코딩 동안만 이 스레드의 현재 Allocator를 묶고(ExternalAllocationScope), 블록 앞
// 머리(16 byte — 정렬 유지)에 Allocator*와 크기를 적어 해제·재할당이 원래 Allocator를 찾는다.
// 할당이 실패하면 스레드 표지를 세워, 라이브러리 실패를 OutOfMemory와 손상(InvalidSource)으로
// 가른다(라이브러리 오류 코드로 가르지 않는다).
namespace rocks::image::detail {

// 이 스레드의 외부 할당을 allocator로 묶는다. 중첩되면 바깥 묶음을 되돌린다.
//
// 사용 예:
//   ExternalAllocationScope scope(allocator);
//   ... 라이브러리 호출 ...
//   if (!ok) { return scope.AllocationFailed() ? Resource(out, ErrorCode::OutOfMemory)
//                                               : Invalid(out, 0); }
class ExternalAllocationScope {
public:
    explicit ExternalAllocationScope(Allocator& allocator) noexcept;
    ~ExternalAllocationScope() noexcept;

    ExternalAllocationScope(const ExternalAllocationScope&) = delete;
    ExternalAllocationScope& operator=(const ExternalAllocationScope&) = delete;

    // 이 묶음 동안 할당이 한 번이라도 실패했는지다.
    bool AllocationFailed() const noexcept;

private:
    Allocator* previousAllocator_ = nullptr;
    bool previousFailed_ = false;
};

// 묶인 Allocator에서 bytes를 잡는다(16 byte 정렬). 묶음이 없거나 실패하면 nullptr이고 표지를
// 세운다.
void* ExternalAllocate(usize bytes) noexcept;

// count × size를 0으로 채워 잡는다(곱이 넘치면 nullptr).
void* ExternalAllocateZeroed(usize count, usize size) noexcept;

// C realloc과 같다: address가 nullptr이면 할당, 실패하면 nullptr이고 원래 블록은 그대로다.
void* ExternalReallocate(void* address, usize bytes) noexcept;

// ExternalAllocate 계열이 준 블록을 돌려준다(nullptr은 무시).
void ExternalFree(void* address) noexcept;

} // namespace rocks::image::detail
