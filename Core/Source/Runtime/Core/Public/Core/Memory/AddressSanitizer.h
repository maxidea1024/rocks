#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>

// AddressSanitizer 수동 poison이다. 자체 할당기가 OS에서 직접 받은 메모리(가상 메모리 arena·슬롯)를
// 쓰지 않는 동안 ASan에 "접근 금지"로 표시해, 해제 뒤 접근·슬롯 꼬리 넘침을 ASan 보고로 드러낸다.
// Asan 구성에서 계측하는 번역 단위에서만 동작하고, 그 밖에서는 아무 일도 하지 않는다(호출 비용
// 없음).
//
// - poison 단위는 8 byte다. 범위가 8 byte 경계에서 시작하지 않으면 앞쪽 일부는 poison되지 않을 수
//   있다(ASan 제약).
// - 계측하지 않은 코드(sanitizerInstrumentation이 꺼진 외부 라이브러리)의 접근은 검사되지 않는다.
// - 할당기가 다시 내줄 때 AsanUnpoisonMemory로 먼저 푼다. 푼 범위만 사용자에게 보인다.
//
// 사용 예:
//   AsanUnpoisonMemory(slot, requestedBytes);   // 할당: 요청 크기만 연다
//   AsanPoisonMemory(slot, slotBytes);          // 해제: 슬롯 전체를 닫는다
#if ROCKS_ADDRESS_SANITIZER
// ASan runtime이 내보내는 함수다. 모든 Rocks 대상의 runtime(clang compiler-rt, MSVC ASan)이 같은
// 이름을 제공한다. 일부 툴체인에는 <sanitizer/asan_interface.h>가 없어서 직접 선언한다.
extern "C" void __asan_poison_memory_region(void const volatile* address,
                                            decltype(sizeof(0)) bytes);
extern "C" void __asan_unpoison_memory_region(void const volatile* address,
                                              decltype(sizeof(0)) bytes);
#endif

namespace rocks {

// [address, address + bytes)를 ASan 접근 금지로 표시한다. bytes가 0이면 아무 일도 하지 않는다.
inline void AsanPoisonMemory(const void* address, usize bytes) noexcept {
#if ROCKS_ADDRESS_SANITIZER
    __asan_poison_memory_region(address, bytes);
#else
    (void)address;
    (void)bytes;
#endif
}

// AsanPoisonMemory로 막은 범위를 다시 연다.
inline void AsanUnpoisonMemory(const void* address, usize bytes) noexcept {
#if ROCKS_ADDRESS_SANITIZER
    __asan_unpoison_memory_region(address, bytes);
#else
    (void)address;
    (void)bytes;
#endif
}

} // namespace rocks
