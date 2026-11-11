#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks {

// 문자열 전체를 숫자로 엄격하게 읽는다(UE LexFromString·FCString::Atoi 대응이지만 관대하지 않다).
// 앞뒤 공백·접두사(0x 등)·자릿수 구분자·부분 일치는 InvalidArgument다. 결과 범위를 넘으면
// Overflow다. 실패하면 값을 돌려주지 않는다.
// - 정수: [+|-]digits. base는 2–36이며 10보다 큰 자리는 대소문자 무관 영문자다. 부호 없는 형식은
//   '-'를 받지 않는다.
// - 실수: [+|-](digits[.digits]|.digits)([e|E][+|-]digits) 또는 inf·infinity·nan(대소문자 무관).
//   16진 실수는 받지 않는다. 가장 가까운 값으로 반올림하며 너무 작은 값은 0 또는 비정규 수가 된다.
//   1023 byte를 넘는 입력은 InvalidArgument다. 현재 C locale의 소수점 설정과 무관하게 '.'만 쓴다.
Result<int64> TryParseInt64(StringView text, uint32 base = 10) noexcept;
Result<uint64> TryParseUInt64(StringView text, uint32 base = 10) noexcept;
Result<int32> TryParseInt32(StringView text, uint32 base = 10) noexcept;
Result<uint32> TryParseUInt32(StringView text, uint32 base = 10) noexcept;
Result<double> TryParseDouble(StringView text) noexcept;
Result<float> TryParseFloat(StringView text) noexcept;

} // namespace rocks
