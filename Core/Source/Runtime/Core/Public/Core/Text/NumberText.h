#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 문자열을 숫자로 읽는 엄격한 해석 함수다(UE LexFromString·FCString::Atoi 대응이지만 관대하지
// 않다).
//
// 언제 쓰나: 설정 파일·명령행·네트워크 텍스트처럼 외부에서 온 숫자 문자열을 읽을 때. "12abc"를
// 12로 읽는 식의 조용한 부분 해석 대신 오류를 받는다.
//
// 사용 예:
//   auto port = TryParseUInt32(text);              // "8080" → 8080
//   if (!port) { Report("포트 번호가 아니다"); }
//   auto mask = TryParseUInt64("ff00", 16);        // 16진(접두사 0x 없이)
//   auto scale = TryParseDouble("1.5e-3");          // 0.0015
//
// 공통 규칙:
// - 문자열 전체가 숫자여야 한다. 앞뒤 공백·접두사(0x 등)·자릿수 구분자(1,000·1_000)·뒤의 문자는
//   InvalidArgument다. 빈 문자열도 InvalidArgument다.
// - 결과 타입의 범위를 넘으면 Overflow다. 실패하면 값을 돌려주지 않는다.
// - 실수 문자열의 소수점은 항상 '.'다. 정수 연산만 쓰므로 C runtime·locale·부동소수점 환경과
//   무관하게 모든 플랫폼에서 결과가 같다.
namespace rocks {

// 부호 있는 정수다. 형식: [+|-]숫자. base는 2–36이며 10보다 큰 자리는 대소문자 무관 영문자다
// (base 16의 "ff"·"FF"는 255). 범위 밖 base는 InvalidArgument다.
Result<int64> TryParseInt64(StringView text, uint32 base = 10) noexcept;
Result<int32> TryParseInt32(StringView text, uint32 base = 10) noexcept;

// 부호 없는 정수다. 형식: [+]숫자. '-'로 시작하면 InvalidArgument다("-0"도 마찬가지).
Result<uint64> TryParseUInt64(StringView text, uint32 base = 10) noexcept;
Result<uint32> TryParseUInt32(StringView text, uint32 base = 10) noexcept;

// 10진 실수다. 형식: [+|-](숫자[.[숫자]] | .숫자)([e|E][+|-]숫자), 또는 inf·infinity·nan(대소문자
// 무관). "5."·"1.e5"처럼 소수점 뒤 숫자는 없어도 된다(JSON은 따로 더 엄격하다).
// - 가장 가까운 값으로 반올림한다. float는 double을 거치지 않고 바로 float로 반올림한다.
// - 너무 큰 값(예: "1e400")은 Overflow, 너무 작은 값(예: "1e-400")은 0 또는 비정규 수로 성공한다.
// - 16진 실수("0x1p3")는 받지 않는다. 길이 제한은 없다(유효 숫자가 길어도 정확히 반올림한다).
Result<double> TryParseDouble(StringView text) noexcept;
Result<float> TryParseFloat(StringView text) noexcept;

} // namespace rocks
