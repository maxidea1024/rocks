#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
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
};

// text를 delimiter로 나눠 조각을 out 뒤에 붙이고 붙인 개수를 반환한다(UE ParseIntoArray 대응).
// - out을 먼저 비우지 않는다는 점이 UE와 다르다. 새로 받으려면 out.Clear()를 먼저 호출한다.
// - 조각은 text를 빌린 view다. text가 바뀌거나 사라지면 조각을 쓰지 않는다.
// - 빈 text는 cullEmpty면 0개, 아니면 빈 조각 1개다.
// 실패: 빈 delimiter는 InvalidArgument, 배열 확장 실패는 allocator 오류. 실패하면 out을 호출 전
// 크기로 되돌린다.
// 주의: byte 단위 검색이므로 text와 delimiter는 유효한 UTF-8이어야 한다(아니면 조각이 문자 중간에서
// 잘릴 수 있다).
Result<usize> TrySplitIntoArray(StringView text, StringView delimiter, Array<StringView>& out,
                                SplitOptions options = {}) noexcept;

// parts를 separator로 이은 새 String을 만든다(UE FString::Join 대응). parts가 비면 빈 문자열이다.
// 실패: 잘못된 UTF-8은 InvalidArgument, 그 밖에 Overflow·allocator 오류.
Result<String> TryJoin(Allocator& allocator, Span<const StringView> parts,
                       StringView separator) noexcept;

} // namespace rocks
