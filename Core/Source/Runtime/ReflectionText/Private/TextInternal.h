#pragma once

#include <ReflectionText/TextValue.h>

// ReflectionText 구현 안에서만 쓰는 도우미다(리플렉션 B9i3b).
namespace rocks::reflection_text::detail {

// 값 텍스트 하나의 해석 실패다(진단 종류와 사람이 읽는 설명 — 정적 문자열).
struct ScalarFailure {
    TextDiagnosticCode code = TextDiagnosticCode::InvalidValue;
    StringView message;
};

// 진단을 하나 남기고 Success(false)를 돌려준다(진단 할당 실패는 그 오류).
inline Result<bool> Report(TextCodecContext& context, TextDiagnosticCode code, TextSource source,
                           StringView path, StringView message) noexcept {
    auto added = context.Diagnostics().Add(code, source, path, message);
    if (!added) {
        return Result<bool>::Failure(added.Error());
    }

    return Result<bool>::Success(false);
}

// 단일 값 텍스트 하나를 out에 읽는다(입력 byte는 세지 않는다 — 호출자가 센다). 반환이 false면
// failure에 진단 종류·설명을 쓰고 진단은 남기지 않는다(호출자가 남긴다 — 경로 key hook은 경로
// 진단 하나로 보고한다).
Result<bool> ParseScalarCore(TextCodecContext& context, const TextScalarType& type, StringView text,
                             FieldValue& out, ScalarFailure& failure) noexcept;

// value를 숨김이 아닌 열거자로 나타낼 수 없는지(숨김 열거자를 썼는지) 본다. 보통 enum은 그 값의
// 열거자가 Hidden이면, Flags enum은 숨김이 아닌 열거자의 OR로 덮을 수 없는 비트가 남으면 참이다.
bool UsesHiddenEnumerator(const EnumSchema& schema, int64 value) noexcept;

// text가 ASCII 대소문자 무시로 word와 같은지다.
bool TextEqualsIgnoringCase(StringView text, StringView word) noexcept;

} // namespace rocks::reflection_text::detail
