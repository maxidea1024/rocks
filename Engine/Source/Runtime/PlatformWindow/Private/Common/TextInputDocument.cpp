#include "TextInputDocument.h"

namespace rocks::platform {

namespace {

uint32 Smaller(uint32 a, uint32 b) noexcept {
    return a < b ? a : b;
}

uint32 Larger(uint32 a, uint32 b) noexcept {
    return a < b ? b : a;
}

} // namespace

void TextInputDocument::Reset() noexcept {
    length_ = 0;
    selectionStart_ = 0;
    selectionEnd_ = 0;
    caretAtStart_ = false;
    composing_ = false;
    compositionStart_ = 0;
    compositionEnd_ = 0;
    CommitBase();
}

TextInputSyncResult TextInputDocument::Sync(Span<const char16_t> text, uint32 selectionBegin,
                                            uint32 selectionLength) noexcept {
    TextInputSyncResult result;
    if (text.Size() > MaxTextInputDocumentCodeUnits) {
        Reset();
        result.tooLong = true;
        return result;
    }

    // 공통 앞·뒤를 뺀 한 구간이 바뀐 범위다(TSF OnTextChange).
    const uint32 next = static_cast<uint32>(text.Size());
    const uint32 shorter = Smaller(length_, next);
    uint32 prefix = 0;
    while (prefix < shorter && text_[prefix] == text[prefix]) {
        ++prefix;
    }

    uint32 suffix = 0;
    while (suffix < shorter - prefix && text_[length_ - 1 - suffix] == text[next - 1 - suffix]) {
        ++suffix;
    }

    if (prefix != length_ || next != length_) {
        result.textChanged = true;
        result.change = {prefix, length_ - suffix, next - suffix};
        for (uint32 i = 0; i < next; ++i) {
            text_[i] = text[i];
        }

        length_ = next;
    }

    const uint32 start = Smaller(selectionBegin, length_);
    const uint32 end = Smaller(selectionBegin + selectionLength, length_);
    if (start != selectionStart_ || end != selectionEnd_) {
        result.selectionChanged = true;
        selectionStart_ = start;
        selectionEnd_ = end;
        caretAtStart_ = false;
    }

    compositionStart_ = Smaller(compositionStart_, length_);
    compositionEnd_ = Smaller(compositionEnd_, length_);
    CommitBase();
    return result;
}

Span<const char16_t> TextInputDocument::Text() const noexcept {
    return {text_, length_};
}

uint32 TextInputDocument::Length() const noexcept {
    return length_;
}

uint32 TextInputDocument::SelectionStart() const noexcept {
    return selectionStart_;
}

uint32 TextInputDocument::SelectionEnd() const noexcept {
    return selectionEnd_;
}

bool TextInputDocument::CaretAtStart() const noexcept {
    return caretAtStart_;
}

bool TextInputDocument::IsComposing() const noexcept {
    return composing_;
}

uint32 TextInputDocument::CompositionStart() const noexcept {
    return compositionStart_;
}

uint32 TextInputDocument::CompositionEnd() const noexcept {
    return compositionEnd_;
}

bool TextInputDocument::HasUnflushedEdits() const noexcept {
    return CommonPrefix() != length_ || length_ != baseLength_ ||
           selectionStart_ != baseSelectionStart_ || selectionEnd_ != baseSelectionEnd_ ||
           composing_ != baseComposing_ ||
           (composing_ &&
            (compositionStart_ != baseCompositionStart_ || compositionEnd_ != baseCompositionEnd_));
}

bool TextInputDocument::SetSelection(uint32 start, uint32 end, bool caretAtStart) noexcept {
    if (start > end || end > length_) {
        return false;
    }

    selectionStart_ = start;
    selectionEnd_ = end;
    caretAtStart_ = caretAtStart;
    return true;
}

bool TextInputDocument::Replace(uint32 start, uint32 end, Span<const char16_t> text,
                                TextInputChange& change) noexcept {
    if (start > end || end > length_) {
        return false;
    }

    const usize inserted = text.Size();
    const usize next = static_cast<usize>(length_) - (end - start) + inserted;
    if (inserted > MaxTextInputDocumentCodeUnits || next > MaxTextInputDocumentCodeUnits) {
        return false;
    }

    // 뒤 글자를 새 자리로 옮긴다(겹칠 수 있어 방향을 고른다).
    const uint32 tail = length_ - end;
    const uint32 to = start + static_cast<uint32>(inserted);
    if (to > end) {
        for (uint32 i = tail; i > 0; --i) {
            text_[to + i - 1] = text_[end + i - 1];
        }
    } else if (to < end) {
        for (uint32 i = 0; i < tail; ++i) {
            text_[to + i] = text_[end + i];
        }
    }

    for (usize i = 0; i < inserted; ++i) {
        text_[start + i] = text[i];
    }

    length_ = static_cast<uint32>(next);
    // 조합 범위는 바뀐 글자를 따라간다: 뒤에 떨어져 있으면 밀리고, 겹치거나 맞닿으면 바뀐 구간을
    // 품게 넓힌다(빈 조합의 caret에 넣은 글자는 조합 안이다). TSF가 곧 OnUpdateComposition으로
    // 정확한 범위를 준다 — 이것은 그 전까지의 근사다.
    if (composing_) {
        const uint32 removed = end - start;
        if (compositionStart_ > end) {
            compositionStart_ = compositionStart_ - removed + static_cast<uint32>(inserted);
            compositionEnd_ = compositionEnd_ - removed + static_cast<uint32>(inserted);
        } else if (compositionEnd_ >= start) {
            compositionStart_ = Smaller(compositionStart_, start);
            compositionEnd_ = compositionEnd_ >= end
                                  ? compositionEnd_ - removed + static_cast<uint32>(inserted)
                                  : to;
        }
    }

    // caret은 넣은 글자 뒤다(UE FTextStoreACP::InsertTextAtSelection).
    selectionStart_ = to;
    selectionEnd_ = to;
    caretAtStart_ = false;
    change = {start, end, to};
    return true;
}

bool TextInputDocument::InsertAtSelection(Span<const char16_t> text,
                                          TextInputChange& change) noexcept {
    return Replace(selectionStart_, selectionEnd_, text, change);
}

void TextInputDocument::StartComposition(uint32 start, uint32 end) noexcept {
    composing_ = true;
    UpdateComposition(start, end);
}

void TextInputDocument::UpdateComposition(uint32 start, uint32 end) noexcept {
    const uint32 first = Smaller(start, length_);
    compositionStart_ = first;
    compositionEnd_ = Larger(first, Smaller(end, length_));
}

void TextInputDocument::EndComposition() noexcept {
    composing_ = false;
}

void TextInputDocument::Merge(TextInputRecordResult& total,
                              const TextInputRecordResult& next) noexcept {
    if (next.serial) {
        total.serial = next.serial;
    }

    total.overflowed = total.overflowed || next.overflowed;
}

TextInputRecordResult TextInputDocument::Flush(TextInputSession& session) noexcept {
    TextInputRecordResult total;
    if (composing_ && !baseComposing_) {
        Merge(total, session.RecordEdit(TextInputRecordKind::CompositionBegin, 0, 0));
    }

    const uint32 prefix = CommonPrefix();
    const bool textChanged = prefix != length_ || length_ != baseLength_;
    if (textChanged) {
        // 공통 앞·뒤를 뺀 한 구간이다(조합 중이면 미리보기, 아니면 확정 편집 하나).
        const uint32 suffix = CommonSuffix(prefix);
        Merge(total, session.RecordEdit(TextInputRecordKind::TextReplace, prefix,
                                        baseLength_ - suffix - prefix,
                                        {text_ + prefix, length_ - suffix - prefix}));
    }

    if (composing_ &&
        (textChanged || !baseComposing_ || compositionStart_ != baseCompositionStart_ ||
         compositionEnd_ != baseCompositionEnd_)) {
        Merge(total, session.RecordEdit(TextInputRecordKind::CompositionRange, compositionStart_,
                                        compositionEnd_ - compositionStart_));
    }

    if (!composing_ && baseComposing_) {
        Merge(total, session.RecordEdit(TextInputRecordKind::CompositionCommit, 0, 0));
    }

    if (textChanged || selectionStart_ != baseSelectionStart_ ||
        selectionEnd_ != baseSelectionEnd_) {
        Merge(total, session.RecordEdit(TextInputRecordKind::Selection, selectionStart_,
                                        selectionEnd_ - selectionStart_));
    }

    CommitBase();
    return total;
}

void TextInputDocument::CommitBase() noexcept {
    for (uint32 i = 0; i < length_; ++i) {
        baseText_[i] = text_[i];
    }

    baseLength_ = length_;
    baseSelectionStart_ = selectionStart_;
    baseSelectionEnd_ = selectionEnd_;
    baseComposing_ = composing_;
    baseCompositionStart_ = compositionStart_;
    baseCompositionEnd_ = compositionEnd_;
}

uint32 TextInputDocument::CommonPrefix() const noexcept {
    const uint32 shorter = Smaller(length_, baseLength_);
    uint32 prefix = 0;
    while (prefix < shorter && text_[prefix] == baseText_[prefix]) {
        ++prefix;
    }

    return prefix;
}

uint32 TextInputDocument::CommonSuffix(uint32 prefix) const noexcept {
    const uint32 shorter = Smaller(length_, baseLength_);
    uint32 suffix = 0;
    while (suffix < shorter - prefix &&
           text_[length_ - 1 - suffix] == baseText_[baseLength_ - 1 - suffix]) {
        ++suffix;
    }

    return suffix;
}

} // namespace rocks::platform
