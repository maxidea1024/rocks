#pragma once

#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Source.h>

// 언어 서버의 위치·URI 변환이다(Docs/Design/SparksTools.ko.md 10.6절).
namespace rocks::sparks::tools::detail {

// LSP 위치(0부터 센 줄, UTF-16 code unit 열)다.
struct LspPosition {
    uint32 line = 0;
    uint32 character = 0;
};

// bytes의 UTF-16 code unit 수다: 유효한 UTF-8 sequence는 U+10000 미만 1·이상 2, 잘못된 byte는
// 하나에 1(U+FFFD).
uint32 LspUtf16Units(StringView bytes) noexcept;

// file 원문의 byte 위치(0부터 Text().ByteCount()까지)를 LSP 위치로 바꾼다. BOM 안은 0:0, 1줄의 열은
// BOM 뒤부터 센다.
LspPosition LspPositionOf(const SourceFile& file, uint32 offset) noexcept;

// LspPositionOf의 역이다(Docs/Design/SparksTools.ko.md 11.2절): 줄이 줄 수 이상이면 원문 끝, 열이
// 줄 끝을 넘으면 줄 끝(LF 앞, CRLF면 CR 앞 — 외톨이 CR은 줄 안의 글자), 서로게이트 쌍 가운데면 그
// 글자의 시작. 1줄은 BOM 뒤부터 센다.
uint32 LspOffsetOf(const SourceFile& file, LspPosition position) noexcept;

// `file` URI를 경로(`/` 구분, Windows 드라이브 `c:/…`)로 바꿔 path에 둔다. 값 false는 받지 않는
// URI다(scheme이 file이 아님·authority 있음·잘못된 percent·UTF-8 아님). 실패: 할당 실패.
Result<bool> LspPathFromUri(StringView uri, String& path) noexcept;

// 경로를 `file` URI로 바꿔 uri에 둔다(segment마다 percent-encoding — 드라이브 `c:`는 `c%3A`, `\`는
// `/`로). 실패: 할당 실패.
Result<void> LspUriFromPath(StringView path, String& uri) noexcept;

// 경로 비교 key다: `\`를 `/`로, 끝 `/`를 떼고, Windows 드라이브 글자는 소문자로. 실패: 할당 실패.
Result<void> LspPathKey(StringView path, String& key) noexcept;

} // namespace rocks::sparks::tools::detail
