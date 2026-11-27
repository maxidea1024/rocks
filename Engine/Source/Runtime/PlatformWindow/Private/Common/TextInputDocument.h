#pragma once

#include "TextInputSession.h"

// TSF 거울 문서다(Docs/Design/UiToolkit 22.13절 — 플랫폼 공용, OS 호출 없음). TSF는 문서를 잠금
// 안에서 동기로 읽고 고친다. 창은 UI 문맥을 바로 고치지 않고(입력 큐의 키·문자와 순서가
// 어긋난다) 이 거울로 답한다. 잠금을 풀 때 Flush가 거울의 바뀐 것을 TSF 기록으로 TextInputSession에
// 넣고, 게임 lane이 표지 자리에서 문맥에 적용한다. 문맥 쪽 변화(타자·되돌리기·프로그램 교체)는
// 게임 lane이 Sync로 거울에 맞춘다.
//
// 용량은 고정이다(OS 콜백 안에서 할당하지 않는다). 위치는 UTF-16 code unit이다.
//
// 사용 예(Win64 TSF 저장소):
//   document.Replace(start, end, text, change);          // ITextStoreACP::SetText — 잠금 안
//   auto result = document.Flush(session);               // 잠금을 풀 때
//   if (result.serial) { PushMarker(*result.serial); }
//   auto synced = document.Sync(text, begin, length);    // 게임 lane, 적용 전 기록이 없을 때
//   if (synced.textChanged) { sink->OnTextChange(0, &change); }
namespace rocks::platform {

// 거울 용량이다(이보다 긴 문맥은 TSF 문서를 만들지 않고 IMM으로 받는다).
inline constexpr usize MaxTextInputDocumentCodeUnits = 16384;

// 바뀐 범위다(TSF TS_TEXTCHANGE 모양 — [start, oldEnd)가 [start, newEnd)가 됐다).
struct TextInputChange {
    uint32 start = 0;
    uint32 oldEnd = 0;
    uint32 newEnd = 0;
};

// Sync 결과다(TSF에 알릴 것).
struct TextInputSyncResult {
    bool textChanged = false;
    TextInputChange change{};
    bool selectionChanged = false;
    // 문맥 글자가 용량을 넘는다 — 거울은 비었고 이 문맥에는 TSF 문서를 쓰지 않는다.
    bool tooLong = false;
};

// 시험(TextInputDocumentTests)이 부르므로 export한다(Modular 링크).
class ROCKS_PLATFORMWINDOW_API TextInputDocument {
public:
    // 빈 거울로 되돌린다(기준도 비운다).
    void Reset() noexcept;
    // 문맥의 글자·선택으로 거울과 기준을 맞춘다(게임 lane — 적용 전 기록이 없을 때만 부른다).
    // 조합 표시는 그대로 둔다(문맥 미리보기가 거울과 같다).
    TextInputSyncResult Sync(Span<const char16_t> text, uint32 selectionBegin,
                             uint32 selectionLength) noexcept;

    Span<const char16_t> Text() const noexcept;
    uint32 Length() const noexcept;
    uint32 SelectionStart() const noexcept;
    uint32 SelectionEnd() const noexcept;
    // caret이 선택 앞쪽 끝인지다(TS_AE_START).
    bool CaretAtStart() const noexcept;
    bool IsComposing() const noexcept;
    uint32 CompositionStart() const noexcept;
    uint32 CompositionEnd() const noexcept;
    // 아직 기록으로 내지 않은 편집이 있는지다(거울 ≠ 기준).
    bool HasUnflushedEdits() const noexcept;

    // TSF 편집이다(잠금 안). 범위가 잘못됐거나 용량을 넘으면 false고 거울은 그대로다.
    bool SetSelection(uint32 start, uint32 end, bool caretAtStart) noexcept;
    // [start, end)를 text로 바꾸고 caret을 그 뒤에 둔다(UE FTextStoreACP::SetText —
    // SetSelection + InsertTextAtSelection). 조합 범위는 바뀐 글자를 따라간다.
    bool Replace(uint32 start, uint32 end, Span<const char16_t> text,
                 TextInputChange& change) noexcept;
    // 선택을 text로 바꾸고 caret을 그 뒤에 둔다(ITextStoreACP::InsertTextAtSelection).
    bool InsertAtSelection(Span<const char16_t> text, TextInputChange& change) noexcept;
    // 조합 시작·범위·끝이다(ITfContextOwnerCompositionSink). 범위가 잘못되면 글자 안으로 자른다.
    void StartComposition(uint32 start, uint32 end) noexcept;
    void UpdateComposition(uint32 start, uint32 end) noexcept;
    void EndComposition() noexcept;

    // 기준 뒤 바뀐 것을 session에 TSF 기록으로 낸다(22.13절 순서): 위치 기준(Rebase) → (기준과 조합
    // 세대가 다르면 앞 조합 확정) → 조합 시작 → 글자 바꿈 → 조합 범위 → 조합 확정 → 선택. 그다음
    // 기준을 거울로 옮긴다. 넘치면 남은 기록은 내지 않는다(버려진 기록을 전제로 한 위치라서).
    // 마지막 기록 번호(없으면 비어 있음)와 넘침을 돌려준다.
    TextInputRecordResult Flush(TextInputSession& session) noexcept;

private:
    static void Merge(TextInputRecordResult& total, const TextInputRecordResult& next) noexcept;
    void CommitBase() noexcept;
    uint32 CommonPrefix() const noexcept;
    uint32 CommonSuffix(uint32 prefix) const noexcept;

    char16_t text_[MaxTextInputDocumentCodeUnits]{};
    uint32 length_ = 0;
    uint32 selectionStart_ = 0;
    uint32 selectionEnd_ = 0;
    bool caretAtStart_ = false;
    bool composing_ = false;
    uint32 compositionStart_ = 0;
    uint32 compositionEnd_ = 0;
    // 조합 세대다(시작마다 오른다 — 한 잠금 안의 끝 → 시작을 가른다).
    uint32 generation_ = 0;
    // 기준 — 내보낸 기록을 다 적용한 뒤 문맥이 가질 상태다.
    char16_t baseText_[MaxTextInputDocumentCodeUnits]{};
    uint32 baseLength_ = 0;
    uint32 baseSelectionStart_ = 0;
    uint32 baseSelectionEnd_ = 0;
    bool baseComposing_ = false;
    uint32 baseCompositionStart_ = 0;
    uint32 baseCompositionEnd_ = 0;
    uint32 baseGeneration_ = 0;
};

} // namespace rocks::platform
