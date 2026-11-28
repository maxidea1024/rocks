#include <UiFramework/Text/TextShaper.h>

namespace rocks::ui {

namespace {

// 기본 단어 경계의 문자 부류다.
enum class WordClass : uint8 {
    Space,
    Punctuation,
    Other
};

// 공백 code point다(ASCII 공백·제어, NBSP, 전각 공백).
constexpr char32_t NoBreakSpace = 0x00A0;
constexpr char32_t IdeographicSpace = 0x3000;
// 문장부호 구간이다: ASCII 기호, 일반 문장부호(U+2000–U+206F), CJK 기호·문장부호
// (U+3001–U+303F), 전각 ASCII 기호(U+FF01–U+FF0F 등).
constexpr char32_t GeneralPunctuationFirst = 0x2000;
constexpr char32_t GeneralPunctuationLast = 0x206F;
constexpr char32_t CjkPunctuationFirst = 0x3001;
constexpr char32_t CjkPunctuationLast = 0x303F;
constexpr char32_t FullwidthPunctuationFirst = 0xFF01;
constexpr char32_t FullwidthPunctuationLast = 0xFF0F;

WordClass ClassOf(char32_t point) noexcept {
    if (point <= U' ' || point == NoBreakSpace || point == IdeographicSpace) {
        return WordClass::Space;
    }

    const bool ascii = point < 0x80;
    const bool alnum = (point >= U'0' && point <= U'9') || (point >= U'A' && point <= U'Z') ||
                       (point >= U'a' && point <= U'z') || point == U'_';
    if ((ascii && !alnum) ||
        (point >= GeneralPunctuationFirst && point <= GeneralPunctuationLast) ||
        (point >= CjkPunctuationFirst && point <= CjkPunctuationLast) ||
        (point >= FullwidthPunctuationFirst && point <= FullwidthPunctuationLast)) {
        return WordClass::Punctuation;
    }

    return WordClass::Other;
}

} // namespace

Result<SharedPtr<TextLabel>> TextShaper::ShapeEllipsis(Allocator& allocator,
                                                       float fontSize) noexcept {
    auto marker = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!marker) {
        return marker;
    }

    auto text = Text::Invariant(allocator, "…");
    if (!text) {
        return Result<SharedPtr<TextLabel>>::Failure(text.Error());
    }

    auto shaped = Shape(*marker.Value(), text.Value(), fontSize);
    if (!shaped) {
        return Result<SharedPtr<TextLabel>>::Failure(shaped.Error());
    }

    if (marker.Value()->GetText() != "…" || marker.Value()->Layout().Lines().Size() != 1 ||
        marker.Value()->Layout().Lines()[0].width <= 0) {
        return Result<SharedPtr<TextLabel>>::Failure(ErrorCode::InvalidArgument);
    }

    return marker;
}

Result<void> TextShaper::FindWordBoundaries(StringView utf8, Array<usize>& out) noexcept {
    out.Clear();
    auto points = utf8.TryCodePoints();
    if (!points) {
        return Result<void>::Failure(points.Error());
    }

    const char* const start = utf8.Data();
    bool first = true;
    WordClass previous = WordClass::Other;
    for (auto it = points.Value().begin(); it != points.Value().end(); ++it) {
        const WordClass current = ClassOf(*it);
        const usize offset = static_cast<usize>(it.Position() - start);
        // 부류가 바뀌거나 문장부호가 이어지면(문장부호는 하나씩) 그 앞이 경계다.
        if (!first && (current != previous || current == WordClass::Punctuation)) {
            if (!out.TryPushBack(offset)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }

        previous = current;
        first = false;
    }

    if (!utf8.IsEmpty() && !out.TryPushBack(utf8.ByteCount())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
