#include "ProgressText.h"

#include "ProgressInternal.h"

#include <Core/Guid.h>
#include <Core/Move.h>

namespace rocks::detail {

namespace {

// 쉼표를 넣는 자리 간격이다.
constexpr uint32 ProgressDigitGroup = 3;
// uint64 10진 최대 자릿수다.
constexpr uint32 ProgressMaxDecimalDigits = 20;

} // namespace

bool ProgressSourceInto(Allocator& allocator, const Text& text, String& out) noexcept {
    out.Clear();
    if (!text.IsValid()) {
        return true;
    }

    auto resolved = ProgressSourceText(allocator, text);
    if (!resolved) {
        return false;
    }

    out = Move(resolved.Value().value);
    return true;
}

void ProgressPersistentId(const Text& text, StringBuilderBase& id, uint64& revision) noexcept {
    revision = 0;
    if (!text.IsValid()) {
        return;
    }

    TextPersistentView view;
    if (!text.TryGetPersistentForm(view) || view.form != TextPersistentForm::Localized) {
        return;
    }

    if (id.TryAppendFormat("{}", view.id)) {
        revision = view.sourceRevision;
    }
}

void ProgressAppendGrouped(StringBuilderBase& out, uint64 value) noexcept {
    char digits[ProgressMaxDecimalDigits];
    uint32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    for (uint32 index = count; index != 0; --index) {
        const char digit[] = {digits[index - 1]};
        (void)out.TryAppend(StringView(digit, 1));
        if (index != 1 && (index - 1) % ProgressDigitGroup == 0) {
            (void)out.TryAppend(",");
        }
    }
}

} // namespace rocks::detail
