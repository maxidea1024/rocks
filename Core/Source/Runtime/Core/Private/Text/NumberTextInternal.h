#pragma once

#include <Base/Types.h>

// 서식 엔진이 쓰는 실수 표기 도구다. C runtime의 printf·strtod를 쓰되 C locale의 소수점 설정과
// 무관하게 항상 '.'를 쓰고 읽는다.
namespace rocks::detail {

// value를 printf 형식 '%.<precision><type>'(type은 f·e·g·E·G)으로 buffer에 쓴다. 필요한 길이
// (NUL 제외)를 반환하며 capacity가 부족하면 buffer 내용은 잘린다. 실패하면 MaxSize다. 무한·NaN은
// 호출자가 처리한다.
usize FormatFloatingPoint(double value, char type, int precision, char* buffer,
                          usize capacity) noexcept;

// 다시 읽었을 때 같은 값이 되는 가장 적은 유효 자릿수로 buffer(32 byte 이상)에 쓰고 길이를
// 반환한다. 첫 유효 숫자의 10진 지수가 -4 이상 16 미만이면 고정 소수점, 아니면 과학 표기다.
// single이면 float 정밀도로 왕복한다. 음수가 아닌 유한 값만 받는다(부호·무한·NaN은 호출자 처리).
usize FormatShortestFloatingPoint(double value, bool single, char* buffer, usize capacity) noexcept;

} // namespace rocks::detail
