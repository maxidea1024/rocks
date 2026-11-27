#include <UiCore/TextInputPositions.h>

namespace rocks::ui::text_input {

namespace {

// BMP 밖 code point의 시작, surrogate 범위, 바꿈 문자다(UTF-16 — Unicode 3.9절).
constexpr uint32 FirstSupplementaryCodePoint = 0x10000;
constexpr uint32 FirstHighSurrogate = 0xD800;
constexpr uint32 FirstLowSurrogate = 0xDC00;
constexpr uint32 LastLowSurrogate = 0xDFFF;
constexpr uint32 SurrogatePayloadBits = 10;
constexpr uint32 SurrogatePayloadMask = 0x3FF;
constexpr uint32 ReplacementCharacter = 0xFFFD;

uint32 Utf16Units(char32_t point) noexcept {
    return point >= FirstSupplementaryCodePoint ? 2 : 1;
}

bool IsHighSurrogate(uint32 unit) noexcept {
    return unit >= FirstHighSurrogate && unit < FirstLowSurrogate;
}

bool IsLowSurrogate(uint32 unit) noexcept {
    return unit >= FirstLowSurrogate && unit <= LastLowSurrogate;
}

} // namespace

uint32 Utf16Length(StringView text) noexcept {
    return ToUtf16Index(text, text.ByteCount());
}

uint32 ToUtf16Index(StringView text, usize byte) noexcept {
    auto points = text.TryCodePoints();
    if (!points) {
        return 0;
    }

    // 끝이 byte 안에 드는 code point만 센다(가운데 byte면 그 code point 앞).
    uint32 index = 0;
    for (auto it = points.Value().begin(); it != points.Value().end();) {
        const uint32 units = Utf16Units(*it);
        ++it;
        if (static_cast<usize>(it.Position() - text.Data()) > byte) {
            break;
        }

        index += units;
    }

    return index;
}

usize ToByteOffset(StringView text, uint32 index) noexcept {
    auto points = text.TryCodePoints();
    if (!points) {
        return 0;
    }

    uint32 units = 0;
    for (auto it = points.Value().begin(); it != points.Value().end(); ++it) {
        const uint32 next = units + Utf16Units(*it);
        if (next > index) {
            return static_cast<usize>(it.Position() - text.Data());
        }

        units = next;
    }

    return text.ByteCount();
}

Result<void> AppendUtf16(StringView text, Array<char16_t>& out) noexcept {
    auto points = text.TryCodePoints();
    if (!points) {
        return Result<void>::Failure(points.Error());
    }

    auto reserved = out.TryEnsureCapacity(out.Size() + Utf16Length(text));
    if (!reserved) {
        return reserved;
    }

    for (const char32_t point : points.Value()) {
        if (point >= FirstSupplementaryCodePoint) {
            const uint32 value = static_cast<uint32>(point) - FirstSupplementaryCodePoint;
            (void)out.TryPushBack(
                static_cast<char16_t>(FirstHighSurrogate + (value >> SurrogatePayloadBits)));
            (void)out.TryPushBack(
                static_cast<char16_t>(FirstLowSurrogate + (value & SurrogatePayloadMask)));
        } else {
            (void)out.TryPushBack(static_cast<char16_t>(point));
        }
    }

    return Result<void>::Success();
}

Result<void> AppendUtf8(Span<const char16_t> text, StringBuilderBase& out) noexcept {
    for (usize i = 0; i < text.Size(); ++i) {
        uint32 point = text[i];
        if (IsHighSurrogate(point) && i + 1 < text.Size() && IsLowSurrogate(text[i + 1])) {
            point = FirstSupplementaryCodePoint +
                    ((point - FirstHighSurrogate) << SurrogatePayloadBits) +
                    (static_cast<uint32>(text[i + 1]) - FirstLowSurrogate);
            ++i;
        } else if (IsHighSurrogate(point) || IsLowSurrogate(point)) {
            point = ReplacementCharacter;
        }

        auto appended = out.TryAppendCodePoint(point);
        if (!appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui::text_input
