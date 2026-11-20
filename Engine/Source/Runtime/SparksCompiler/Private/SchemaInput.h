#pragma once

#include <Core/Text/Json.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Source.h>

// manifest·binding schema reader가 함께 쓰는 도우미다: JSON 값의 원문 범위와 schema 타입 표기 문법.

namespace rocks::sparks::detail {

// JSON 값 하나의 원문 범위다. 문자열은 여는 따옴표부터 닫는 따옴표까지, 숫자·리터럴은 그 토큰,
// 배열·객체는 여는 괄호 한 글자다(진단 밑줄용).
SourceSpan JsonValueSpan(StringView text, JsonPosition position) noexcept;

// binding schema의 타입 표기(Docs/Design/SparksLanguage.ko.md 10.2절)가 문법에 맞는지 본다.
// 문법: 이름 {'.' 이름} ['<' 표기 {", " 표기} '>'] ['?']. 이름은 ASCII 영문자로 시작하는
// 영문자·숫자, 공백은 쉼표 뒤 한 칸만 쓴다(생성기 출력이 하나로 정해지게). 중첩은
// MaxSyntaxNesting까지다.
bool IsValidTypeNotation(StringView text) noexcept;

} // namespace rocks::sparks::detail
