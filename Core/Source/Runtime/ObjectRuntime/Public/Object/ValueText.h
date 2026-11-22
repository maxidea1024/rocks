#pragma once

#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Object/PathKey.h>

// 문맥 없이 정해지는 단일 값의 텍스트 표기다(리플렉션 B9i3b — Docs/Design/
// ReflectionExtensions.ko.md 1.5·10.4절). 문자열 경로의 key 텍스트(ParsePropertyPath·
// FormatPropertyPath)와 ReflectionText의 값 표기가 이 규칙 하나를 함께 쓴다:
//   - Bool: 입력 true·false·on·off(ASCII 대소문자 무시), 출력 true·false
//   - 정수 8종: 정규 10진(숫자 하나 이상, 앞자리 0은 `0` 하나만, 부호는 부호 있는 종류의 `-`만 —
//     `+`·`-0` 거부)
//   - Enum: 열거자 도구 이름(FindEnumerator — 대소문자 무시·toolPrefix), 출력은 실효 도구 이름.
//     Flags enum은 `A+B`(입력은 `A|B`도), 0은 빈 값, 출력은 선언 순 `+`
//   - Guid·AssetId: 36자 소문자(AssetId::TryParse — 대문자·32자 거부)
//   - Digest256: 64자 소문자 16진
//   - Utf8String·Name: 그대로(Name 정규화·intern은 호출자 — 입력 계층)
// 그 밖의 종류(실수·참조·ClassRef·컨테이너)는 문맥(resolver·TypeRegistry)이 필요해 이 함수가 다루지
// 않는다(Unsupported). 할당하지 않고(출력은 out의 할당만) 스레드와 무관하다.
namespace rocks {

// text를 type의 값으로 읽는다. enumSchema는 type이 Enum일 때의 enum 스키마다. 값은 PathKeyValue의
// 칸(PathKey.h)에 담고 Utf8String·Name의 text는 입력을 빌린다. 좁은 정수 폭(UInt8·Int16 등)의
// 범위는 보지 않는다 — 호출자가 그 종류의 범위로 판정한다(경로 key는 AppendPathKey가 Overflow로
// 본다). 숨김 열거자(EnumeratorFlags::Hidden)도 받는다(설정 값으로 받을지는 호출자가 판정한다).
// 오류: 형식이 맞지 않음(문법·모르는 열거자·enumSchema 없음) InvalidArgument, 64비트 정수 범위 밖
// Overflow, 다루지 않는 종류 Unsupported.
//
// 사용 예:
//   auto parsed = ParseScalarText(ValueType{ValueKind::UInt32, {}}, nullptr, "42");
//   if (parsed) { /* parsed.Value().unsignedInteger == 42 */ }
ROCKS_OBJECTRUNTIME_API Result<PathKeyValue>
ParseScalarText(const ValueType& type, const EnumSchema* enumSchema, StringView text) noexcept;

// value(type의 값 — PathKeyValue 칸)의 정규 텍스트를 out 뒤에 붙인다(ParseScalarText와 왕복한다).
// 오류: 이름 있는 열거자(조합)로 나타낼 수 없는 enum 값·enumSchema 없음·다루지 않는 종류
// Unsupported, 붙이기 실패는 StringBuilder의 오류(잘못된 UTF-8은 InvalidArgument). 실패하면 out에
// 앞부분이 남을 수 있다.
//
// 사용 예:
//   StringBuilder<64> text(allocator);
//   PathKeyValue value;
//   value.kind = ValueKind::Int32;
//   value.signedInteger = -7;
//   auto written = AppendScalarText(ValueType{ValueKind::Int32, {}}, nullptr, value, text);
//   // 성공하면 text == "-7"
ROCKS_OBJECTRUNTIME_API Result<void> AppendScalarText(const ValueType& type,
                                                      const EnumSchema* enumSchema,
                                                      const PathKeyValue& value,
                                                      StringBuilderBase& out) noexcept;

namespace detail {

// 36자 Guid 표기를 담는 byte 수다.
inline constexpr usize GuidTextBytes = 36;

// 64비트 정수 10진 표기(부호 포함)를 담는 byte 수 하한이다.
inline constexpr usize DecimalTextBytes = 21;

// value의 10진 표기를 out 끝(오른쪽 정렬)에 쓰고 쓴 범위를 돌려준다(capacity ≥ DecimalTextBytes).
ROCKS_OBJECTRUNTIME_API Span<const char> FormatDecimalText(uint64 value, bool negative, char* out,
                                                           usize capacity) noexcept;

// Guid의 36자 소문자 표기(RFC 9562 — Core Guid "{}"·AssetId 표기와 같다)를 out(GuidTextBytes
// 이상)에 쓴다.
Span<const char> FormatGuidText(Guid value, char* out) noexcept;

// value와 같은 값의 열거자다(없으면 nullptr — 별칭은 등록이 막는다).
ROCKS_OBJECTRUNTIME_API const EnumeratorSchema* FindEnumeratorByValue(const EnumSchema& schema,
                                                                      int64 value) noexcept;

// Flags enum 값의 정규 텍스트(선언 순 `A+B`, 0은 빈 값)를 out(없으면 길이만 센다)에 쓰고 byte 수를
// bytes에 둔다. Out은 TryAppend(StringView) -> Result<void>를 가진 String·StringBuilderBase다. 이름
// 있는 열거자 조합으로 나타낼 수 없는 비트가 남거나 붙이기가 실패하면 false다.
template <class Out>
bool FormatFlagsText(const EnumSchema& schema, int64 value, Out* out, usize& bytes) noexcept {
    bytes = 0;
    int64 remaining = value;
    bool first = true;
    for (usize index = 0; index < schema.values.Size() && remaining != 0; ++index) {
        const EnumeratorSchema& enumerator = schema.values[index];
        if (enumerator.value == 0 || (enumerator.value & ~value) != 0 ||
            (enumerator.value & remaining) == 0) {
            continue;
        }

        const Span<const char> name = EffectiveToolName(enumerator);
        if (out && ((!first && !out->TryAppend("+")) ||
                    !out->TryAppend(StringView(name.Data(), name.Size())))) {
            return false;
        }

        bytes += name.Size() + (first ? 0 : 1);
        first = false;
        remaining &= ~enumerator.value;
    }

    return remaining == 0;
}

} // namespace detail

} // namespace rocks
