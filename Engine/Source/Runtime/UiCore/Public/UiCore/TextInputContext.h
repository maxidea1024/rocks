#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Rect.h>
#include <Core/Optional.h>
#include <Core/Result.h>

namespace rocks::ui {

// 텍스트 입력 문맥이다(UE ITextInputMethodContext — Docs/Design/UiToolkit 19.2절). 편집 요소가
// 구현하고 플랫폼 IME가 당겨(pull) 묻고 바꾼다. 위치는 UTF-16 code unit이고 줄 끝은 \n 한
// 글자다(3.12절 — UTF-8 저장과의 변환은 구현이 한다). 사각형은 UI 창 좌표(= client 물리 픽셀)다.
//
// 조합 계약: BeginComposition 뒤 SetText는 문맥 안의 미리보기 층만 바꾸고(편집 이력·변경
// 알림·바인딩 쓰기 없음), EndComposition이 미리보기를 한 번 커밋한다(편집 이력 하나, 변경 알림 한
// 번).
//
// 스레드: 게임 lane에서만 불린다(창 IME 기록을 UI 입력 adapter가 처리할 때).
class TextInputContext {
public:
    virtual ~TextInputContext() noexcept = default;

    virtual bool IsReadOnly() const noexcept = 0;
    // IME를 쓸 수 없는 입력(비밀번호)이면 false다. 플랫폼이 IME를 끈다.
    virtual bool AllowsInputMethod() const noexcept = 0;
    virtual bool IsComposing() const noexcept = 0;
    virtual uint32 GetTextLength() const noexcept = 0;
    virtual void GetSelection(uint32& begin, uint32& length) const noexcept = 0;
    virtual void SetSelection(uint32 begin, uint32 length) noexcept = 0;
    virtual Result<void> GetText(uint32 begin, uint32 length,
                                 Array<char16_t>& out) const noexcept = 0;
    virtual void SetText(uint32 begin, uint32 length, Span<const char16_t> text) noexcept = 0;
    virtual Optional<uint32> GetCharacterIndexFromPoint(Vec2 window) const noexcept = 0;
    // [begin, begin + length) 글자들의 창 좌표 사각형이다. length 0이면 begin 자리 caret이다.
    virtual bool GetTextBounds(uint32 begin, uint32 length, Box2<float>& window) const noexcept = 0;
    virtual bool GetScreenBounds(Box2<float>& window) const noexcept = 0;
    virtual void BeginComposition() noexcept = 0;
    virtual void UpdateCompositionRange(uint32 begin, uint32 length) noexcept = 0;
    virtual void EndComposition() noexcept = 0;
};

} // namespace rocks::ui
