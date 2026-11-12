#pragma once

#include <Base/Types.h>

// CRT 없는 10진 ↔ 2진 실수 변환이다(Docs/Design/CoreDataFoundations.ko.md 3.2절).
// NumberText(파싱)와 StringBuilder(서식)가 쓴다. 정수 연산만 쓰므로 결과가 C
// runtime·locale·부동소수점 환경(반올림 방향·FTZ)과 무관하다.
//
// - 파싱: 유효 숫자 앞 19자리를 128비트 5^q 근사(생성 표)와 곱해 참값을 아래·위에서 감싼다.
//   두 경계를 반올림한 결과가 같으면 그것이 답이고(반올림은 단조), 다르면 큰 정수로 10진 값과
//   두 후보의 중간점을 정확히 비교한다.
// - 최단 왕복 표기: Ryu(Ulf Adams, PLDI 2018)를 double·float 공통으로 쓴다(125비트 생성 표).
// - 정밀도 지정 표기('f'·'e'·'g'): 큰 정수로 정확한 10진 전개를 만든 뒤 가장 가까운 값(동점은
//   짝수)으로 자른다.
namespace rocks::detail::decimal {

// IEEE 2진 형식: 숨은 비트를 뺀 가수 비트 수와 지수 비트 수.
struct BinaryFormat {
    int32 mantissaBits;
    int32 exponentBits;
};

inline constexpr BinaryFormat DoubleFormat{52, 11};
inline constexpr BinaryFormat FloatFormat{23, 8};

// 정확한 반올림에 쓰는 최대 유효 숫자 수다. 그 뒤 숫자는 0이 아닌지(truncated)만 기록한다. double
// 두 이웃 값의 중간점은 10진으로 768자리 이하(float는 113자리 이하)이므로, 769자리에서 자른 수가
// 중간점보다 작으면 원래 수도 작다.
inline constexpr usize MaxSignificantDigits = 769;

// 10진 수 0.d1d2…dn × 10^pointPosition(+ 잘린 0이 아닌 꼬리). digits는 0–9 값이며 첫 숫자는 0이
// 아니다. count가 0이면 값이 0이다.
struct DecimalNumber {
    uint8 digits[MaxSignificantDigits];
    usize count = 0;
    int64 pointPosition = 0;
    bool truncated = false;
};

// number를 format으로 가장 가깝게(동점은 짝수) 반올림한 비트(부호 제외)다. 최댓값을 넘어 무한대로
// 반올림되면 무한대 비트를, 가장 작은 비정규 수의 절반 이하면 0을 돌려준다. 입력 크기와 무관하게
// 정확하다.
uint64 ToBinary(const DecimalNumber& number, BinaryFormat format) noexcept;

// number와 bits·bits+1(유한 양수 또는 0인 이웃 두 값)의 중간점을 정확히 비교한다(-1·0·1). 잘린
// 꼬리가 있으면 같음을 큼으로 본다. 시험이 ToBinary와 독립인 이분 탐색 기준값을 만들 때도 쓴다.
// bits는 형식의 어떤 유한 값이어도 된다(최악 약 4650비트 — 큰 정수 용량 5120비트 안).
int32 CompareWithHalfway(const DecimalNumber& number, uint64 bits, BinaryFormat format) noexcept;

// 최단 왕복 표기 value = significand × 10^exponent. 다시 읽으면 같은 값이 되는 가장 짧은 10진
// 수이며, 그런 수가 여럿이면 참값에 가장 가까운 것(동점은 짝수)이다. significand에 끝 0이 남을 수
// 있다.
struct ShortestDecimal {
    uint64 significand;
    int32 exponent;
};

// bits는 부호를 뺀 0이 아닌 유한 값의 IEEE 비트다(0이면 Fatal — 호출자가 먼저 처리한다).
ShortestDecimal Shortest(uint64 bits, BinaryFormat format) noexcept;

// float 비트를 같은 값의 double 비트로 넓힌다(정수 연산 — FTZ·DAZ와 무관, NaN은 quiet NaN 비트).
uint64 FloatBitsToDoubleBits(uint32 bits) noexcept;

// double의 정확한 10진 전개 0.d1d2…dn × 10^pointPosition. digits는 ASCII이고 끝 0은 없다(값 0이면
// count 0). 가장 긴 경우는 정수부 16자리 + 소수부 1074자리다.
struct ExactDecimal {
    char digits[1100];
    usize count = 0;
    int32 pointPosition = 0;
};

// doubleBits는 유한 double의 비트다(부호는 무시한다).
void ExpandExact(uint64 doubleBits, ExactDecimal& out) noexcept;

// out을 앞에서부터 keep자리만 남기도록 가장 가까운 값(동점은 짝수)으로 반올림한다. keep이 0 이하면
// 첫 숫자보다 앞 자리에서 반올림한다(결과는 0 또는 10^pointPosition). 끝 0은 지운다.
void RoundExact(ExactDecimal& out, int64 keep) noexcept;

} // namespace rocks::detail::decimal
