#pragma once

#include <Base/Types.h>

// AddressSanitizer 수동 poison이다. 자체 할당기가 OS에서 직접 받은 메모리(가상 메모리 arena·슬롯)를
// 쓰지 않는 동안 ASan에 "접근 금지"로 표시해, 해제 뒤 접근과 슬롯 꼬리 넘침을 ASan 보고로 드러낸다.
//
// - Core가 계측되는 구성(Asan)에서만 동작하고, 그 밖에서는 아무 일도 하지 않는다. 함수 본문은
//   Core 번역 단위에 있으므로, 이 헤더를 포함하는 번역 단위의 계측 여부와 관계없이 같다.
// - poison 단위는 8 byte다. 범위가 8 byte 경계에서 시작하지 않으면 앞쪽 일부는 poison되지 않을
//   수 있다(ASan 제약).
// - 계측하지 않은 코드(sanitizerInstrumentation이 꺼진 외부 라이브러리)의 접근은 검사되지 않는다.
// - 할당기가 다시 내줄 때 AsanUnpoisonMemory로 먼저 푼다. 푼 범위만 사용자에게 보인다.
//
// 사용 예:
//   AsanUnpoisonMemory(slot, requestedBytes);   // 할당: 요청 크기만 연다
//   AsanPoisonMemory(slot, slotBytes);          // 해제: 슬롯 전체를 닫는다
namespace rocks {

// [address, address + bytes)를 ASan 접근 금지로 표시한다. bytes가 0이면 아무 일도 하지 않는다.
void AsanPoisonMemory(const void* address, usize bytes) noexcept;

// AsanPoisonMemory로 막은 범위를 다시 연다.
void AsanUnpoisonMemory(const void* address, usize bytes) noexcept;

} // namespace rocks
