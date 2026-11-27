#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// 텍스트 입력 문맥의 위치 변환이다(Docs/Design/UiToolkit 19.2절). 문맥 위치는 UTF-16 code unit이고
// 편집 요소의 글자는 UTF-8이다. 줄 끝은 \n 한 글자다(편집 요소가 CRLF를 LF로 맞춰 둔다).
//
// 사용 예:
//   const usize byte = text_input::ToByteOffset(text, begin);           // UTF-16 → UTF-8 byte
//   const uint32 index = text_input::ToUtf16Index(text, caretByte);      // UTF-8 byte → UTF-16
//   StringBuilder<> utf8(allocator);
//   if (text_input::AppendUtf8(utf16, utf8)) { ... }
namespace rocks::ui::text_input {

// text의 UTF-16 길이다. text는 올바른 UTF-8이어야 한다(아니면 0).
ROCKS_UICORE_API uint32 Utf16Length(StringView text) noexcept;

// byte 위치의 UTF-16 위치다. code point 중간이면 그 code point 앞이고, 끝을 넘으면 길이다.
ROCKS_UICORE_API uint32 ToUtf16Index(StringView text, usize byte) noexcept;

// UTF-16 위치의 byte 위치다. surrogate 쌍 가운데면 그 code point 앞이고, 길이를 넘으면 끝이다.
ROCKS_UICORE_API usize ToByteOffset(StringView text, uint32 index) noexcept;

// text를 UTF-16으로 out 뒤에 붙인다. 할당 실패면 out은 그대로다.
ROCKS_UICORE_API Result<void> AppendUtf16(StringView text, Array<char16_t>& out) noexcept;

// UTF-16을 UTF-8로 out 뒤에 붙인다. 짝 없는 surrogate는 U+FFFD가 된다(IME가 보낸 글자를
// 버리지 않는다). 할당에 실패하면 앞부분만 붙은 out을 버려야 한다.
ROCKS_UICORE_API Result<void> AppendUtf8(Span<const char16_t> text,
                                         StringBuilderBase& out) noexcept;

} // namespace rocks::ui::text_input
