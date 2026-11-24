// ICU 경계·BiDi다(Docs/Design/Font 6.4·6.8절). 경계는 utext_openUTF8로 UTF-8 byte 위치를 그대로
// 받고, BiDi는 UTF-16으로 바꿔 ubidi로 푼 뒤 위치를 UTF-8 byte로 되돌린다.
#include "IcuText.h"

#include <TextLayout/Utf8.h>
#include <unicode/ubidi.h>
#include <unicode/ubrk.h>
#include <unicode/utext.h>

namespace rocks::text::detail {

namespace {

// 경계 규칙 로캘 이름의 최대 byte 수다(NUL 제외 — BCP 47 태그는 이보다 짧다).
constexpr usize MaxLocaleBytes = 63;

// UTF-16 대리 쌍이 되는 code point의 시작이다.
constexpr char32_t SupplementaryFirst = 0x10000;

UBreakIteratorType BreakType(TextBoundaryKind kind) noexcept {
    switch (kind) {
    case TextBoundaryKind::Word:
        return UBRK_WORD;
    case TextBoundaryKind::Line:
        return UBRK_LINE;
    case TextBoundaryKind::Sentence:
        return UBRK_SENTENCE;
    case TextBoundaryKind::Grapheme:
    default:
        return UBRK_CHARACTER;
    }
}

} // namespace

Result<void> FindBoundaries(TextBoundaryKind kind, StringView utf8, StringView culture,
                            Array<usize>& out) noexcept {
    out.Clear();
    if (!utf8.ValidateUtf8() || culture.ByteCount() > MaxLocaleBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (utf8.ByteCount() == 0) {
        return Result<void>::Success();
    }

    char locale[MaxLocaleBytes + 1] = {};
    for (usize index = 0; index < culture.ByteCount(); ++index) {
        locale[index] = culture.Data()[index];
    }

    UErrorCode status = U_ZERO_ERROR;
    UText* text =
        utext_openUTF8(nullptr, utf8.Data(), static_cast<int64_t>(utf8.ByteCount()), &status);
    UBreakIterator* iterator = ubrk_open(BreakType(kind), locale, nullptr, 0, &status);
    if (U_SUCCESS(status)) {
        ubrk_setUText(iterator, text, &status);
    }

    Result<void> result = U_SUCCESS(status) ? Result<void>::Success()
                                            : Result<void>::Failure(ErrorCode::InvalidArgument);
    for (int32_t boundary = U_SUCCESS(status) ? ubrk_next(iterator) : UBRK_DONE;
         result && boundary != UBRK_DONE; boundary = ubrk_next(iterator)) {
        result = out.TryPushBack(static_cast<usize>(boundary));
    }

    ubrk_close(iterator);
    utext_close(text);
    return result;
}

Result<void> ResolveBidi(StringView utf8, TextBaseDirection base, Array<BidiRun>& out) noexcept {
    out.Clear();
    if (!utf8.ValidateUtf8()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (utf8.ByteCount() == 0) {
        return Result<void>::Success();
    }

    // UTF-16 단위 하나마다 그 단위가 속한 code point의 UTF-8 byte 위치를 기억한다(끝 위치 하나를
    // 더한다).
    Allocator& allocator = out.GetAllocator();
    Array<UChar> units(allocator);
    Array<usize> byteOffsets(allocator);
    usize offset = 0;
    while (offset < utf8.ByteCount()) {
        char32_t codePoint = 0;
        const usize length = DecodeUtf8(utf8, offset, codePoint);
        if (length == 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        UChar pair[2] = {static_cast<UChar>(codePoint), 0};
        usize count = 1;
        if (codePoint >= SupplementaryFirst) {
            pair[0] = U16_LEAD(codePoint);
            pair[1] = U16_TRAIL(codePoint);
            count = 2;
        }

        for (usize unit = 0; unit < count; ++unit) {
            if (auto added = units.TryPushBack(pair[unit]); !added) {
                return added;
            }

            if (auto added = byteOffsets.TryPushBack(offset); !added) {
                return added;
            }
        }

        offset += length;
    }

    if (auto added = byteOffsets.TryPushBack(utf8.ByteCount()); !added) {
        return added;
    }

    UErrorCode status = U_ZERO_ERROR;
    UBiDi* bidi = ubidi_open();
    if (bidi == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    const UBiDiLevel level = base == TextBaseDirection::LeftToRight   ? 0
                             : base == TextBaseDirection::RightToLeft ? 1
                                                                      : UBIDI_DEFAULT_LTR;
    ubidi_setPara(bidi, units.Data(), static_cast<int32_t>(units.Size()), level, nullptr, &status);
    const int32_t runCount = U_SUCCESS(status) ? ubidi_countRuns(bidi, &status) : 0;
    Result<void> result = U_SUCCESS(status) ? Result<void>::Success()
                                            : Result<void>::Failure(ErrorCode::InvalidArgument);
    for (int32_t run = 0; result && run < runCount; ++run) {
        int32_t start = 0;
        int32_t length = 0;
        const UBiDiDirection direction = ubidi_getVisualRun(bidi, run, &start, &length);
        const usize byteStart = byteOffsets[static_cast<usize>(start)];
        const usize byteEnd = byteOffsets[static_cast<usize>(start + length)];
        result = out.TryPushBack({byteStart, byteEnd - byteStart, direction == UBIDI_RTL});
    }

    ubidi_close(bidi);
    return result;
}

} // namespace rocks::text::detail
