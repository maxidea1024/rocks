#pragma once

#include <Base/NumericLimits.h>

namespace rocks {

// OS 헤더나 할당기에 의존하지 않는 공통 정수 타입이다.
// 아래 크기 검사는 지원 대상 ABI가 기대하는 저장 폭과 일치하는지 확인한다.
using uint8 = unsigned char;
using uint16 = unsigned short;
using uint32 = unsigned int;
using uint64 = unsigned long long;
using int8 = signed char;
using int16 = short;
using int32 = int;
using int64 = long long;

// usize는 크기·인덱스, uptr는 주소의 정수 표현에 사용한다.
// 둘 다 실행 대상의 포인터 폭을 따르므로 파일·네트워크의 고정 폭 필드로 쓰지 않는다.
using usize = decltype(sizeof(0));
using uptr = usize;

// 포인터 차이·반복자 거리(STL difference_type)에 쓰는 부호 있는 크기다. usize와 같은 폭이다.
using isize = decltype(static_cast<char*>(nullptr) - static_cast<char*>(nullptr));

static_assert(sizeof(uint8) == 1 && sizeof(uint16) == 2);
static_assert(sizeof(uint32) == 4 && sizeof(uint64) == 8);
static_assert(sizeof(int8) == 1 && sizeof(int16) == 2);
static_assert(sizeof(int32) == 4 && sizeof(int64) == 8);
static_assert(sizeof(uptr) == sizeof(void*));
static_assert(sizeof(isize) == sizeof(usize) && static_cast<isize>(-1) < 0);

// usize의 최대값(NumericLimits<usize>::Max())이다. 크기·byte 수 계산의 상한·포화 검사에 쓴다.
inline constexpr usize MaxSize = NumericLimits<usize>::Max();

// usize 색인·위치의 "없음" 표지다(UE5.8 INDEX_NONE 대응. UE는 int32 -1이지만 Rocks의 색인은 부호
// 없는 usize라 최대값이다). 찾지 못한 검색 결과, 아직 정하지 않은 색인 칸에 쓴다. 값은 MaxSize와
// 같지만 뜻이 다르다. 상한 검사와 "가능한 만큼"을 뜻하는 개수 인자(TryMidBytes의 count 기본값 등)는
// MaxSize를 쓴다.
// - uint32 같은 다른 폭의 칸에는 쓰지 않는다. 그 형식으로 선언한 영역 상수
//   (예: SlotArray::EndOfList)를 NumericLimits<T>::Max()로 정의해 쓴다. 폭이 다른 IndexNone과
//   비교하면 항상 거짓이다.
// - 공개 검색 API의 결과 표지(Array::NotFound·StringView::NotFound 등)는 이 값과 같다.
inline constexpr usize IndexNone = MaxSize;

} // namespace rocks
