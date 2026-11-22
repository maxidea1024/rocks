#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// 문자열을 구분자로 나누거나(TrySplitIntoArray) 여러 조각을 구분자로 잇는(TryJoin) 함수다.
//
// 사용 예:
//   Array<StringView> parts(allocator);
//   auto count = TrySplitIntoArray("red, green,,blue", ",", parts);   // 기본: 빈 조각 버림
//   // parts = {"red", " green", "blue"}   (공백은 그대로)
//
//   SplitOptions options;
//   options.trimAsciiWhitespace = true;                              // 조각 앞뒤 공백 제거
//   TrySplitIntoArray("a = 1", "=", parts, options);                 // {"a", "1"} 추가
//
//   SplitOptions escaped;
//   escaped.escapeCharacters = ",;:";                                // \, 는 구분자가 아니다
//   TrySplitIntoArray("a\\,b,c", ",", parts, escaped);               // {"a\\,b", "c"} 추가
//   StringBuilder<64> first(allocator);
//   TryAppendUnescaped(first, parts[0], escaped.escapeCharacters);   // first == "a,b"
//
//   const StringView words[] = {"alpha", "beta"};
//   auto joined = TryJoin(allocator, Span<const StringView>(words, 2), " + ");  // "alpha + beta"
namespace rocks {

struct SplitOptions {
    // 빈 조각을 버린다(UE ParseIntoArray의 CullEmpty 기본값과 같다). false면 "a,,b"는
    // {"a","","b"}다.
    bool cullEmpty = true;

    // 각 조각의 앞뒤 ASCII 공백(스페이스·탭·개행 등)을 먼저 지운다. 지운 뒤 비면 cullEmpty를
    // 따른다.
    bool trimAsciiWhitespace = false;

    // 구분자 비교 방식이다. 기본은 대소문자를 구분한다(UE ParseIntoArray와 같다).
    StringComparison comparison = StringComparison::CaseSensitive;

    // escape로 쓸 수 있는 글자 목록이다(byte 하나가 글자 하나). 비어 있으면(기본) escape가 없다.
    // 비어 있지 않으면 백슬래시 바로 뒤에 이 목록의 글자가 오면 두 byte를 한 글자(escape 쌍)로
    // 보고 구분자 검색에서 건너뛴다. 그 밖의 백슬래시는 글자 그대로다(C:\x는 escape가 아니다).
    // 왼쪽부터 차례로 보므로 목록에 백슬래시가 없으면 \\, 는 글자 \ 와 escape 쌍 \, 이다(값 끝의
    // 백슬래시를 구분자 바로 앞에 두려면 목록에 백슬래시를 넣어 \\ 로 쓴다). 조각은 text를 빌린
    // view라 escape가 그대로 남는다 — 쓰기 전에 TryAppendUnescaped로 푼다. 여러 단계로 나누는
    // 표기는 마지막 단계에서 한 번만 푼다: 명령행 Map 목록은 ","로 나눈 조각을 TryFindUnescaped로
    // 첫 ':'에서 다시 나눈 뒤 키·값을 푼다(Docs/Design/ReflectionExtensions.ko.md 10.4절 escape
    // 표). 목록 글자는 ASCII 0x21–0x7E여야 한다(공백·제어 문자·비ASCII는 InvalidArgument — 공백
    // escape는 trimAsciiWhitespace와 어긋난다).
    StringView escapeCharacters;
};

// text를 delimiter로 나눠 조각을 out 뒤에 붙이고 붙인 개수를 반환한다(UE ParseIntoArray 대응).
// - out을 먼저 비우지 않는다는 점이 UE와 다르다. 새로 받으려면 out.Clear()를 먼저 호출한다.
// - 조각은 text를 빌린 view다. text가 바뀌거나 사라지면 조각을 쓰지 않는다.
// - 빈 text는 cullEmpty면 0개, 아니면 빈 조각 1개다.
// - options.escapeCharacters가 있으면 escape 쌍 안의 글자는 구분자로 보지 않는다(SplitOptions).
// 실패: 빈 delimiter·잘못된 escapeCharacters는 InvalidArgument, 배열 확장 실패는 allocator 오류.
// 실패하면 out을 호출 전 크기로 되돌린다.
// 주의: byte 단위 검색이므로 text와 delimiter는 유효한 UTF-8이어야 한다(아니면 조각이 문자 중간에서
// 잘릴 수 있다).
Result<usize> TrySplitIntoArray(StringView text, StringView delimiter, Array<StringView>& out,
                                SplitOptions options = {}) noexcept;

// text에서 escape되지 않은 첫 delimiter의 byte 위치를 돌려준다(없으면 StringView::NotFound).
// escape 규칙은 SplitOptions::escapeCharacters와 같다(빈 목록이면 StringView::Find와 같다). 첫
// 구분자에서만 둘로 나누는 표기(명령행 Map 항목 k:v의 첫 escape되지 않은 ':')에 쓴다.
// 실패: 빈 delimiter·잘못된 escapeCharacters는 InvalidArgument.
//
// 사용 예:
//   auto colon = TryFindUnescaped("a\\:b:C:\\x", ":", ",;:");   // 4 — 키 "a\\:b", 값 "C:\\x"
ROCKS_CORE_API Result<usize>
TryFindUnescaped(StringView text, StringView delimiter, StringView escapeCharacters,
                 StringComparison comparison = StringComparison::CaseSensitive) noexcept;

// piece의 escape 쌍(백슬래시 + escapeCharacters의 글자)을 그 글자로 풀어 out 뒤에 붙인다. 그 밖의
// 백슬래시(목록에 없는 글자 앞, 끝의 백슬래시)는 그대로 붙인다. piece는 out의 View()를 빌린 것이면
// 안 된다(붙이는 중 버퍼가 옮겨질 수 있다). 실패: 잘못된 escapeCharacters는 InvalidArgument(out은
// 그대로), 붙이기 실패는 StringBuilder의 오류(잘못된 UTF-8은 InvalidArgument, Overflow·allocator
// 오류 — 그때 out에는 앞부분이 남을 수 있다).
//
// 사용 예:
//   StringBuilder<64> key(allocator);
//   TryAppendUnescaped(key, "a\\:b", ",;:");   // key == "a:b"
ROCKS_CORE_API Result<void> TryAppendUnescaped(StringBuilderBase& out, StringView piece,
                                               StringView escapeCharacters) noexcept;

// parts를 separator로 이은 새 String을 만든다(UE FString::Join 대응). parts가 비면 빈 문자열이다.
// 실패: 잘못된 UTF-8은 InvalidArgument, 그 밖에 Overflow·allocator 오류.
Result<String> TryJoin(Allocator& allocator, Span<const StringView> parts,
                       StringView separator) noexcept;

} // namespace rocks
