#pragma once

#include <Base/Types.h>

// C 라이브러리의 malloc 계열 hook을 Rocks 할당기로 잇는 공용 힙이다. 할당 함수를 직접 받지 않는
// 라이브러리(HarfBuzz hb_*_impl·ICU u_setMemoryFunctions·nanosvg glue의 malloc 치환)가 쓴다. 모두
// 프로세스 수명 SystemAllocator를 지난다 — 라이브러리의 지연 정적 객체가 프로세스 종료 때 풀리므로
// 할당기는 소멸하지 않는다.
//
// realloc은 이전 크기를 주지 않으므로 블록 앞에 크기를 적는다(머리 16 byte 이상).
// 돌려주는 주소는 플랫폼 malloc과 같은 정렬(max(alignof(max_align_t), 16))이다. C 함수와 같이
// 실패하면 nullptr이고, 0 byte 요청도 서로 다른 유효 주소다(머리만 있는 블록).
//
// 사용 예(라이브러리를 링크하는 모듈의 hook 정의):
//   extern "C" void* RocksFooMalloc(size_t size) { return rocks::LibraryHeapAllocate(size); }
//   extern "C" void RocksFooFree(void* memory) { rocks::LibraryHeapFree(memory); }
namespace rocks {

// size byte 블록이다. 넘침·할당 실패는 nullptr다.
ROCKS_CORE_API void* LibraryHeapAllocate(usize size) noexcept;

// count × size byte의 0으로 채운 블록이다(calloc). 곱이 넘치거나 할당이 실패하면 nullptr다.
ROCKS_CORE_API void* LibraryHeapAllocateZeroed(usize count, usize size) noexcept;

// 크기를 바꾼 블록이다(realloc). memory가 nullptr이면 할당과 같다. 실패하면 nullptr이고 memory는
// 그대로 유효하다(C realloc과 같다). 성공하면 memory는 풀린다.
ROCKS_CORE_API void* LibraryHeapReallocate(void* memory, usize size) noexcept;

// 이 힙의 블록을 푼다. nullptr는 아무것도 하지 않는다.
ROCKS_CORE_API void LibraryHeapFree(void* memory) noexcept;

// 이 힙에서 블록이 아직 풀리지 않은 수다(시험이 hook 연결과 누수를 확인한다).
ROCKS_CORE_API usize LibraryHeapLiveBlocks() noexcept;

} // namespace rocks
