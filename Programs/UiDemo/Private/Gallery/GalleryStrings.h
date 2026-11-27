#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/StringView.h>

// 갤러리 견본 문장 표다(Docs/Design/UiToolkit 16.3절). 한글·아랍 문자·이모지 같은 견본은 소스
// 문자열 영어 정책 때문에 소스에 두지 않고 Programs/UiDemo/Content/GalleryText.txt에서 읽는다. 한
// 줄이 "key = value"이고 값의 "\n"은 줄바꿈, '#'으로 시작하는 줄은 주석이다.
//
// 사용 예:
//   GalleryStrings strings(allocator);
//   if (auto loaded = strings.Load(); !loaded) { ... }
//   StringView pangram = strings.Get("pangram.hangul");
namespace rocks::ui::gallery {

class GalleryStrings {
public:
    explicit GalleryStrings(Allocator& allocator) noexcept;

    // 파일을 읽어 표를 만든다. 파일이 없으면 NotFound, 형식이 틀린 줄이 있으면 InvalidArgument다.
    Result<void> Load() noexcept;

    // 없는 키는 키 문자열 자체를 돌려준다(빠진 견본이 화면에 키로 드러난다).
    StringView Get(StringView key) const noexcept;

private:
    struct Entry {
        usize keyStart;
        usize keyLength;
        usize valueStart;
        usize valueLength;
    };

    Result<void> Parse() noexcept;
    StringView View(usize start, usize length) const noexcept;

    Array<uint8> bytes_;
    Array<Entry> entries_;
};

} // namespace rocks::ui::gallery
