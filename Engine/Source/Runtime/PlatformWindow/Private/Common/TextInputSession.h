#pragma once

#include "TextInputRecord.h"

#include <Core/Containers/Span.h>
#include <PlatformWindow/TextInputClient.h>

// 창의 IME 기록 버퍼와 적용 규칙이다(Docs/Design/UiToolkit 19.2절 — 플랫폼 공용). OS 메시지 처리가
// 문자열을 메시지 시점에 Record하고 그 번호를 입력 큐 표지(TextInputMethod)에 싣는다. 게임 lane이
// 표지 자리에서 Process해 문맥에 적용한다. OS 호출이 없어 EngineTests로 시험한다.
//
// 적용 규칙(UE FTextInputMethodSystem IMM 경로와 같다):
//   - 첫 조합·확정이 조합을 시작한다. 그때의 선택 영역이 조합 범위가 된다(입력이 선택을 바꾼다).
//   - 조합: 조합 범위를 새 문자열로 바꾸고, 범위와 caret(문자열 안 커서)을 알린다.
//   - 확정: 조합 범위를 확정 문자열로 바꾸고 caret을 그 뒤에 둔 다음 EndComposition한다.
//   - 끝: 확정 없이 끝났으면(취소) 조합 범위를 지우고 EndComposition한다.
//   - TSF 기록(22.13절 — 거울 문서의 편집을 그대로 옮긴다): 조합 시작, 글자 바꿈(조합 중이면
//     미리보기, 아니면 Begin·SetText·End 확정 하나), 조합 범위, 조합 확정, 선택.
//
// 번호는 다시 쓰지 않는다. Activate·Deactivate·Cancel이 남은 기록을 지우므로 큐에 남은 옛 표지는
// 새 문맥의 기록을 처리하지 않는다.
//
// 사용 예(Win64 창):
//   auto result = session.Record(TextInputRecordKind::Commit, text, 0);   // WM_IME_COMPOSITION
//   if (result.serial) { queue.Push(InputEvent::TextInputMethod(*result.serial)); }
//   session.Process(event.textInputRecord);                               // 게임 lane, 표지 자리
namespace rocks::platform {

// 시험(TextInputSessionTests)이 부르므로 export한다(Modular 링크).
class ROCKS_PLATFORMWINDOW_API TextInputSession {
public:
    TextInputClient* Active() const noexcept;
    // IME 기록을 받는지다(활성 문맥이 있고 IME를 허용한다).
    bool AcceptsInputMethod() const noexcept;
    bool IsComposing() const noexcept;
    usize PendingCount() const noexcept;

    // 활성 문맥을 client로 바꾼다. 앞 문맥은 끝내지 않는다(호출자가 Deactivate를 먼저 한다).
    void Activate(TextInputClient& client) noexcept;
    // client가 활성이면 남은 기록을 처리하고, 그래도 조합 중이면 미리보기를 그대로 확정한 뒤 끈다.
    // 활성이었으면 true다.
    bool Deactivate(TextInputClient& client) noexcept;
    // client가 활성이면 남은 기록을 버리고 조합 추적을 끝낸다(문맥은 미리보기를 이미 버렸다).
    bool Cancel(TextInputClient& client) noexcept;

    // 기록 하나를 넣는다. 자리가 없으면 남은 조합 기록을 지우고(뒤 기록이 미리보기를 덮는다),
    // 그래도 없으면 넘침이다(남은 기록을 모두 버리고 '끝' 하나).
    TextInputRecordResult Record(TextInputRecordKind kind, Span<const char16_t> text,
                                 uint32 cursor) noexcept;
    // TSF 기록이다(kind는 CompositionBegin·TextReplace·CompositionRange·CompositionCommit·
    // Selection, 범위는 UTF-16). 글자는 TextReplace만 쓴다. 자리가 없으면 Record와 같다.
    TextInputRecordResult RecordEdit(TextInputRecordKind kind, uint32 begin, uint32 length,
                                     Span<const char16_t> text = {}) noexcept;
    // 문자열을 읽을 수 없을 만큼 길다(창 버퍼를 넘음) — Record의 넘침과 같다.
    TextInputRecordResult RecordOverflow() noexcept;
    // serial 이하의 남은 기록을 순서대로 적용한다. 적용한 것이 있으면 true다.
    bool Process(uint32 serial) noexcept;

private:
    struct Entry {
        uint32 serial;
        TextInputRecordKind kind;
        uint32 offset;
        uint32 length;
        uint32 cursor;
        // TSF 기록의 범위다.
        uint32 rangeBegin;
        uint32 rangeLength;
    };

    bool Fits(usize length) const noexcept;
    uint32 Append(TextInputRecordKind kind, Span<const char16_t> text, uint32 cursor) noexcept;
    void DropCompositions() noexcept;
    void Clear() noexcept;
    void BeginIfNeeded() noexcept;
    void Apply(const Entry& entry) noexcept;

    TextInputClient* active_ = nullptr;
    Entry entries_[MaxTextInputRecords]{};
    char16_t text_[MaxTextInputCodeUnits]{};
    usize head_ = 0;
    usize count_ = 0;
    usize textUsed_ = 0;
    uint32 nextSerial_ = 1;
    bool composing_ = false;
    uint32 compositionBegin_ = 0;
    uint32 compositionLength_ = 0;
};

} // namespace rocks::platform
