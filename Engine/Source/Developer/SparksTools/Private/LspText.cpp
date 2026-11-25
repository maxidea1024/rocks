#include <Core/Containers/Array.h>
#include <Core/Text/Uri.h>
#include "LspText.h"

// 언어 서버의 위치·URI 변환이다(Docs/Design/SparksTools.ko.md 10.6절).
namespace rocks::sparks::tools::detail {

namespace {

// UTF-8 판별 상수다(lead byte 범위와 이어지는 byte 10xxxxxx).
constexpr uint8 LspAsciiLimit = 0x80;
constexpr uint8 LspTrailMask = 0xC0;
constexpr uint8 LspTrailBits = 0x80;
constexpr uint8 LspTwoByteFirst = 0xC2; // C0·C1은 overlong
constexpr uint8 LspThreeByteFirst = 0xE0;
constexpr uint8 LspFourByteFirst = 0xF0;
constexpr uint8 LspFourByteLast = 0xF4;  // F5 이상은 U+10FFFF 초과
constexpr uint8 LspOverlongThree = 0xE0; // E0 뒤는 A0 이상
constexpr uint8 LspSurrogateLead = 0xED; // ED 뒤는 9F 이하(surrogate 제외)
constexpr uint8 LspOverlongFour = 0xF0;  // F0 뒤는 90 이상
constexpr uint8 LspThreeSecondMin = 0xA0;
constexpr uint8 LspSurrogateSecondMax = 0x9F;
constexpr uint8 LspFourSecondMin = 0x90;
constexpr uint8 LspFourSecondMax = 0x8F; // F4 뒤는 8F 이하

constexpr StringView LspFileScheme = "file";
constexpr StringView LspUriPrefix = "file:///";
constexpr StringView LspLocalHost = "localhost";
constexpr usize LspEscapeBytes = 3; // `%XX`

bool LspTrail(uint8 value) noexcept {
    return (value & LspTrailMask) == LspTrailBits;
}

// text의 첫 sequence가 유효하면 그 byte 수, 아니면 0이다. units에는 UTF-16 code unit 수를 둔다.
usize LspSequence(const char* text, usize available, uint32& units) noexcept {
    const auto lead = static_cast<uint8>(text[0]);
    units = 1;
    if (lead < LspAsciiLimit) {
        return 1;
    }

    usize length = 0;
    uint8 secondMin = LspTrailBits;
    uint8 secondMax = 0xBF;
    if (lead >= LspTwoByteFirst && lead < LspThreeByteFirst) {
        length = 2;
    } else if (lead >= LspThreeByteFirst && lead < LspFourByteFirst) {
        length = 3;
        secondMin = lead == LspOverlongThree ? LspThreeSecondMin : secondMin;
        secondMax = lead == LspSurrogateLead ? LspSurrogateSecondMax : secondMax;
    } else if (lead >= LspFourByteFirst && lead <= LspFourByteLast) {
        length = 4;
        units = 2;
        secondMin = lead == LspOverlongFour ? LspFourSecondMin : secondMin;
        secondMax = lead == LspFourByteLast ? LspFourSecondMax : secondMax;
    } else {
        units = 1;
        return 0;
    }

    if (length > available) {
        units = 1;
        return 0;
    }

    const auto second = static_cast<uint8>(text[1]);
    bool valid = second >= secondMin && second <= secondMax;
    for (usize at = 2; at < length; ++at) {
        valid = valid && LspTrail(static_cast<uint8>(text[at]));
    }

    if (!valid) {
        units = 1;
        return 0;
    }

    return length;
}

bool LspDrive(StringView path) noexcept {
    return path.ByteCount() >= 2 && path.Data()[1] == ':' &&
           ((path.Data()[0] >= 'a' && path.Data()[0] <= 'z') ||
            (path.Data()[0] >= 'A' && path.Data()[0] <= 'Z'));
}

} // namespace

uint32 LspUtf16Units(StringView bytes) noexcept {
    uint32 count = 0;
    usize at = 0;
    while (at < bytes.ByteCount()) {
        uint32 units = 1;
        const usize length = LspSequence(bytes.Data() + at, bytes.ByteCount() - at, units);
        count += units;
        at += length == 0 ? 1 : length;
    }

    return count;
}

LspPosition LspPositionOf(const SourceFile& file, uint32 offset) noexcept {
    const SourceLocation location = file.Locate(offset);
    LspPosition position;
    position.line = location.line > 0 ? location.line - 1 : 0;
    uint32 begin = file.LineBegin(location.line);
    begin = begin < file.ContentBegin() ? file.ContentBegin() : begin;
    if (offset > begin) {
        position.character = LspUtf16Units(StringView(file.Text().Data() + begin, offset - begin));
    }

    return position;
}

uint32 LspOffsetOf(const SourceFile& file, LspPosition position) noexcept {
    const StringView text = file.Text();
    const uint32 end = static_cast<uint32>(text.ByteCount());
    if (file.LineCount() == 0 || position.line >= file.LineCount()) {
        return end;
    }

    uint32 at = file.LineBegin(position.line + 1);
    at = at < file.ContentBegin() ? file.ContentBegin() : at;
    uint32 remaining = position.character;
    // 줄 끝은 LF와 그 앞의 CR이다(외톨이 CR은 lexer처럼 줄 안의 글자 — LspPositionOf와 같다).
    while (remaining > 0 && at < end && text.Data()[at] != '\n' &&
           !(text.Data()[at] == '\r' && at + 1 < end && text.Data()[at + 1] == '\n')) {
        uint32 units = 1;
        const usize length = LspSequence(text.Data() + at, end - at, units);
        if (units > remaining) {
            break; // 서로게이트 쌍 가운데 — 그 글자의 시작
        }

        remaining -= units;
        at += length == 0 ? 1 : static_cast<uint32>(length);
    }

    return at;
}

Result<bool> LspPathFromUri(StringView uri, String& path) noexcept {
    // authority는 비었거나 `localhost`(RFC 8089 — 같은 기기)만 받는다.
    auto parsed = TryParseUri(uri);
    if (!parsed || !EqualsIgnoringAsciiCase(parsed.Value().scheme, LspFileScheme) ||
        (parsed.Value().hasAuthority && !parsed.Value().authority.IsEmpty() &&
         !EqualsIgnoringAsciiCase(parsed.Value().authority, LspLocalHost)) ||
        parsed.Value().hasQuery || parsed.Value().hasFragment) {
        return Result<bool>::Success(false);
    }

    // percent로 쓴 경로 구분자(`%2F`·`%5C`)는 segment 안 글자일 수 없다 — 받지 않는다.
    const StringView encoded = parsed.Value().path;
    for (usize at = 0; at + LspEscapeBytes <= encoded.ByteCount(); ++at) {
        const StringView escape(encoded.Data() + at, LspEscapeBytes);
        if (EqualsIgnoringAsciiCase(escape, "%2F") || EqualsIgnoringAsciiCase(escape, "%5C")) {
            return Result<bool>::Success(false);
        }
    }

    Array<char> decoded(path.GetAllocator());
    if (auto sized = decoded.TryResize(encoded.ByteCount()); !sized) {
        return Result<bool>::Failure(sized.Error());
    }

    auto written = TryPercentDecode(encoded, decoded.View());
    if (!written) {
        return Result<bool>::Success(false);
    }

    StringView text(decoded.Data(), written.Value());
    if (!text.ValidateUtf8()) {
        return Result<bool>::Success(false);
    }

    // `/c:/…`의 드라이브 앞 `/`를 뗀다.
    if (text.ByteCount() >= 3 && text.Data()[0] == '/' &&
        LspDrive(StringView(text.Data() + 1, text.ByteCount() - 1))) {
        text = StringView(text.Data() + 1, text.ByteCount() - 1);
    }

    // 드라이브 글자는 소문자로 둔다(VS Code의 URI와 같은 모양).
    path.Clear();
    if (LspDrive(text)) {
        const char drive = text.Data()[0] >= 'A' && text.Data()[0] <= 'Z'
                               ? static_cast<char>(text.Data()[0] - 'A' + 'a')
                               : text.Data()[0];
        if (auto appended = path.TryAppend(StringView(&drive, 1)); !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        text = StringView(text.Data() + 1, text.ByteCount() - 1);
    }

    if (auto appended = path.TryAppend(text); !appended) {
        return Result<bool>::Failure(appended.Error());
    }

    return Result<bool>::Success(true);
}

Result<void> LspUriFromPath(StringView path, String& uri) noexcept {
    if (auto assigned = uri.TryAssign(LspUriPrefix); !assigned) {
        return assigned;
    }

    usize at = 0;
    while (at < path.ByteCount() && (path.Data()[at] == '/' || path.Data()[at] == '\\')) {
        ++at; // POSIX 절대 경로의 첫 `/`는 접두가 이미 가졌다
    }

    Array<char> buffer(uri.GetAllocator());
    bool first = true;
    while (at <= path.ByteCount()) {
        usize end = at;
        while (end < path.ByteCount() && path.Data()[end] != '/' && path.Data()[end] != '\\') {
            ++end;
        }

        const StringView segment(path.Data() + at, end - at);
        auto size = PercentEncodedSize(segment);
        if (!size) {
            return Result<void>::Failure(size.Error());
        }

        if (auto sized = buffer.TryResize(size.Value()); !sized) {
            return sized;
        }

        auto written = TryPercentEncode(segment, buffer.View());
        if (!written) {
            return Result<void>::Failure(written.Error());
        }

        if (!first) {
            if (auto appended = uri.TryAppend("/"); !appended) {
                return appended;
            }
        }

        if (auto appended = uri.TryAppend(StringView(buffer.Data(), written.Value())); !appended) {
            return appended;
        }

        first = false;
        if (end == path.ByteCount()) {
            break;
        }

        at = end + 1;
    }

    return Result<void>::Success();
}

Result<void> LspPathKey(StringView path, String& key) noexcept {
    usize count = path.ByteCount();
    while (count > 1 && (path.Data()[count - 1] == '/' || path.Data()[count - 1] == '\\')) {
        --count;
    }

    // 드라이브 글자(ASCII)만 따로, 나머지는 `\` 사이 구간을 통째로 붙인다(String은 UTF-8
    // sequence를 쪼개 붙일 수 없다 — 한글 경로).
    key.Clear();
    usize run = 0;
    if (LspDrive(path)) {
        const char c = path.Data()[0];
        const char drive = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        if (auto appended = key.TryAppend(StringView(&drive, 1)); !appended) {
            return appended;
        }

        run = 1;
    }

    for (usize at = run; at <= count; ++at) {
        if (at < count && path.Data()[at] != '\\') {
            continue;
        }

        if (auto appended = key.TryAppend(StringView(path.Data() + run, at - run)); !appended) {
            return appended;
        }

        if (at < count) {
            if (auto appended = key.TryAppend("/"); !appended) {
                return appended;
            }
        }

        run = at + 1;
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks::tools::detail
