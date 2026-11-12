#pragma once

#include <Base/Types.h>

// 서식 엔진이 쓰는 실수 표기 도구다. C runtime(printf·strtod)·locale·부동소수점 환경을 쓰지 않고
// 정확한 10진 전개와 Ryu 최단 표기로 만든다(DecimalConversion.h). 소수점은 항상 '.'다. 값은 IEEE
// 비트로 받는다(부동소수점 변환 명령을 거치지 않으므로 FTZ·DAZ 환경에서도 비정규 수가 그대로다).
namespace rocks::detail {

// doubleBits(부호를 뺀 유한 double 비트)를 printf 형식 '%.<precision><type>'(type은 f·e·g·E·G)과
// 같은 규칙으로 buffer에 쓴다. 반올림은 정확한 2진 값에서 가장 가까운 값(동점은 짝수)이다. 필요한
// 길이(NUL 제외)를 반환하며 capacity가 부족하면 buffer 내용은 잘린다. 형식·정밀도가 잘못되면
// MaxSize다.
usize FormatFloatingPoint(uint64 doubleBits, char type, int precision, char* buffer,
                          usize capacity) noexcept;

// 다시 읽었을 때 같은 값이 되는 가장 적은 유효 자릿수로 buffer(32 byte 이상)에 쓰고 길이를
// 반환한다. 같은 자릿수 후보가 여럿이면 참값에 가장 가까운 것(동점은 짝수)이다. 첫 유효 숫자의 10진
// 지수가 -4 이상 16 미만이면 고정 소수점, 아니면 과학 표기다. bits는 부호를 뺀 유한 값의 비트이며
// single이면 float 비트(float 정밀도로 왕복), 아니면 double 비트다.
usize FormatShortestFloatingPoint(uint64 bits, bool single, char* buffer, usize capacity) noexcept;

} // namespace rocks::detail
