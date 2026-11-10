#pragma once

namespace rocks {

// OS 헤더나 할당기에 의존하지 않는 공통 정수 타입이다.
// 아래 크기 검사는 지원 대상 ABI가 기대하는 저장 폭과 일치하는지 확인한다.
using uint8 = unsigned char;
using uint16 = unsigned short;
using uint32 = unsigned int;
using uint64 = unsigned long long;
using int32 = int;
using int64 = long long;
// usize는 크기·인덱스, uptr는 주소의 정수 표현에 사용한다.
// 둘 다 실행 대상의 포인터 폭을 따르므로 파일·네트워크의 고정 폭 필드로 쓰지 않는다.
using usize = decltype(sizeof(0));
using uptr = usize;

static_assert(sizeof(uint8) == 1 && sizeof(uint16) == 2);
static_assert(sizeof(uint32) == 4 && sizeof(uint64) == 8);
static_assert(sizeof(int32) == 4 && sizeof(int64) == 8);
static_assert(sizeof(uptr) == sizeof(void*));

inline constexpr usize MaxSize = static_cast<usize>(-1);

} // namespace rocks
