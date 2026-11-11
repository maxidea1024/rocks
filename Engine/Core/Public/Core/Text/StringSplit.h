#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

namespace rocks {

struct SplitOptions {
    // 빈 조각을 버린다(UE ParseIntoArray의 CullEmpty 기본값과 같다).
    bool cullEmpty = true;
    // 각 조각의 앞뒤 ASCII 공백을 먼저 지운다(지운 뒤 비면 cullEmpty를 따른다).
    bool trimAsciiWhitespace = false;
    StringComparison comparison = StringComparison::CaseSensitive;
};

// text를 delimiter로 나눠 조각 view를 out 뒤에 붙이고 붙인 수를 반환한다(UE ParseIntoArray 대응,
// out을 비우지 않는다는 점이 다르다). 조각은 text를 빌린다. 빈 delimiter는 InvalidArgument다.
// 실패하면 out을 호출 전 크기로 되돌린다. delimiter가 UTF-8 문자 중간과 일치할 수 있으므로 유효한
// UTF-8 text와 delimiter를 넘긴다.
Result<usize> TrySplitIntoArray(StringView text, StringView delimiter, Array<StringView>& out,
                                SplitOptions options = {}) noexcept;

// parts를 separator로 이은 새 String을 만든다(UE FString::Join 대응). 잘못된 UTF-8은
// InvalidArgument다.
Result<String> TryJoin(Allocator& allocator, Span<const StringView> parts,
                       StringView separator) noexcept;

} // namespace rocks
