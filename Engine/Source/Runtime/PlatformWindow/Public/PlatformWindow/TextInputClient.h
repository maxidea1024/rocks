#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/Rect.h>

namespace rocks::platform {

// 플랫폼 IME가 부르는 텍스트 입력 문맥이다(UE ITextInputMethodContext의 플랫폼 쪽 —
// Docs/Design/UiToolkit 19.2절). 창 호스트가 UI 문맥(ui::TextInputContext)을 감싸 창에 넘긴다.
// 위치는 UTF-16 code unit, 사각형은 client 물리 픽셀이다.
//
// 조합 계약: BeginComposition 뒤의 SetText는 문맥의 미리보기 층만 바꾸고, EndComposition이 한 번
// 확정한다. 창은 창 소유 스레드(게임 lane)에서 ProcessTextInput·Activate·Deactivate 중에만 부른다.
// OS 메시지 처리 중에는 AllowsInputMethod만 묻는다.
class TextInputClient {
public:
    virtual ~TextInputClient() noexcept = default;

    // IME를 쓸 수 있는지다(비밀번호·읽기 전용이면 false — 창이 IME를 끈다).
    virtual bool AllowsInputMethod() const noexcept = 0;
    virtual void GetSelection(uint32& begin, uint32& length) const noexcept = 0;
    virtual void SetSelection(uint32 begin, uint32 length) noexcept = 0;
    virtual void SetText(uint32 begin, uint32 length, Span<const char16_t> text) noexcept = 0;
    // [begin, begin + length) 글자의 client 사각형이다(length 0이면 caret). 모르면 false다.
    virtual bool GetTextBounds(uint32 begin, uint32 length, Box2<float>& client) const noexcept = 0;
    virtual void BeginComposition() noexcept = 0;
    virtual void UpdateCompositionRange(uint32 begin, uint32 length) noexcept = 0;
    virtual void EndComposition() noexcept = 0;
};

} // namespace rocks::platform
