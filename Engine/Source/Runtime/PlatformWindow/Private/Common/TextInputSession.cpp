#include "TextInputSession.h"

namespace rocks::platform {

namespace {

// 번호 a가 b 이하인지다(번호가 한 바퀴 돌아도 맞다 — 남은 기록은 2^31개보다 훨씬 적다).
bool SerialAtOrBefore(uint32 a, uint32 b) noexcept {
    return static_cast<int32>(a - b) <= 0;
}

} // namespace

TextInputClient* TextInputSession::Active() const noexcept {
    return active_;
}

bool TextInputSession::AcceptsInputMethod() const noexcept {
    return active_ && active_->AllowsInputMethod();
}

bool TextInputSession::IsComposing() const noexcept {
    return composing_;
}

usize TextInputSession::PendingCount() const noexcept {
    return count_ - head_;
}

void TextInputSession::Activate(TextInputClient& client) noexcept {
    Clear();
    composing_ = false;
    active_ = &client;
}

bool TextInputSession::Deactivate(TextInputClient& client) noexcept {
    if (active_ != &client) {
        return false;
    }

    (void)Process(nextSerial_ - 1);
    if (composing_) {
        composing_ = false;
        client.EndComposition();
    }

    active_ = nullptr;
    Clear();
    return true;
}

bool TextInputSession::Cancel(TextInputClient& client) noexcept {
    if (active_ != &client) {
        return false;
    }

    Clear();
    composing_ = false;
    return true;
}

bool TextInputSession::Fits(usize length) const noexcept {
    return count_ < MaxTextInputRecords && length <= MaxTextInputCodeUnits - textUsed_;
}

uint32 TextInputSession::Append(TextInputRecordKind kind, Span<const char16_t> text,
                                uint32 cursor) noexcept {
    const uint32 serial = nextSerial_++;
    for (usize i = 0; i < text.Size(); ++i) {
        text_[textUsed_ + i] = text[i];
    }

    entries_[count_++] = {
        serial, kind, static_cast<uint32>(textUsed_), static_cast<uint32>(text.Size()), cursor,
        0,      0};
    textUsed_ += text.Size();
    return serial;
}

void TextInputSession::DropCompositions() noexcept {
    // 남은 기록을 앞으로 당겨 모은다. 쓰는 자리는 읽는 자리보다 앞이라 덮어쓰지 않는다.
    usize kept = 0;
    usize used = 0;
    for (usize i = head_; i < count_; ++i) {
        Entry entry = entries_[i];
        if (entry.kind == TextInputRecordKind::Composition) {
            continue;
        }

        for (usize unit = 0; unit < entry.length && entry.offset != used; ++unit) {
            text_[used + unit] = text_[entry.offset + unit];
        }

        entry.offset = static_cast<uint32>(used);
        used += entry.length;
        entries_[kept++] = entry;
    }

    head_ = 0;
    count_ = kept;
    textUsed_ = used;
}

void TextInputSession::Clear() noexcept {
    head_ = 0;
    count_ = 0;
    textUsed_ = 0;
}

TextInputRecordResult TextInputSession::Record(TextInputRecordKind kind, Span<const char16_t> text,
                                               uint32 cursor) noexcept {
    if (!AcceptsInputMethod()) {
        return {};
    }

    if (!Fits(text.Size())) {
        DropCompositions();
        if (!Fits(text.Size())) {
            return RecordOverflow();
        }
    }

    return {Append(kind, text, cursor), false};
}

TextInputRecordResult TextInputSession::RecordEdit(TextInputRecordKind kind, uint32 begin,
                                                   uint32 length,
                                                   Span<const char16_t> text) noexcept {
    auto result = Record(kind, text, 0);
    if (result.serial && !result.overflowed) {
        entries_[count_ - 1].rangeBegin = begin;
        entries_[count_ - 1].rangeLength = length;
    }

    return result;
}

TextInputRecordResult TextInputSession::RecordOverflow() noexcept {
    if (!AcceptsInputMethod()) {
        return {};
    }

    Clear();
    return {Append(TextInputRecordKind::End, {}, 0), true};
}

bool TextInputSession::Process(uint32 serial) noexcept {
    bool applied = false;
    while (active_ && head_ < count_ && SerialAtOrBefore(entries_[head_].serial, serial)) {
        const Entry entry = entries_[head_++];
        Apply(entry);
        applied = true;
    }

    if (head_ == count_) {
        Clear();
    }

    return applied;
}

void TextInputSession::BeginIfNeeded() noexcept {
    if (composing_) {
        return;
    }

    uint32 begin = 0;
    uint32 length = 0;
    active_->GetSelection(begin, length);
    compositionBegin_ = begin;
    compositionLength_ = length;
    composing_ = true;
    active_->BeginComposition();
}

void TextInputSession::Apply(const Entry& entry) noexcept {
    TextInputClient& client = *active_;
    const Span<const char16_t> text(text_ + entry.offset, entry.length);
    switch (entry.kind) {
    case TextInputRecordKind::Composition:
        if (!composing_ && entry.length == 0) {
            // 조합 밖의 빈 조합은 아무 일도 아니다(선택 영역을 조합으로 잡아 지우지 않는다).
            break;
        }

        BeginIfNeeded();
        client.SetText(compositionBegin_, compositionLength_, text);
        compositionLength_ = entry.length;
        client.UpdateCompositionRange(compositionBegin_, compositionLength_);
        client.SetSelection(
            compositionBegin_ + (entry.cursor < entry.length ? entry.cursor : entry.length), 0);
        break;
    case TextInputRecordKind::Commit:
        BeginIfNeeded();
        client.SetText(compositionBegin_, compositionLength_, text);
        client.SetSelection(compositionBegin_ + entry.length, 0);
        composing_ = false;
        client.EndComposition();
        break;
    case TextInputRecordKind::End:
        if (composing_) {
            client.SetText(compositionBegin_, compositionLength_, {});
            client.SetSelection(compositionBegin_, 0);
            composing_ = false;
            client.EndComposition();
        }
        break;
    case TextInputRecordKind::CompositionBegin:
        BeginIfNeeded();
        break;
    case TextInputRecordKind::TextReplace:
        if (composing_) {
            client.SetText(entry.rangeBegin, entry.rangeLength, text);
        } else {
            // 조합 밖 편집은 확정 하나다(이력·변경 알림 한 번 — 자동 고침·이모지 패널).
            client.BeginComposition();
            client.SetText(entry.rangeBegin, entry.rangeLength, text);
            client.EndComposition();
        }
        break;
    case TextInputRecordKind::CompositionRange:
        BeginIfNeeded();
        client.UpdateCompositionRange(entry.rangeBegin, entry.rangeLength);
        break;
    case TextInputRecordKind::CompositionCommit:
        if (composing_) {
            composing_ = false;
            client.EndComposition();
        }
        break;
    case TextInputRecordKind::Selection:
        client.SetSelection(entry.rangeBegin, entry.rangeLength);
        break;
    }
}

} // namespace rocks::platform
