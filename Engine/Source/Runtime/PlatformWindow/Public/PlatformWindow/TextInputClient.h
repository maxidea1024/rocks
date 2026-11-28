#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Rect.h>
#include <Core/Optional.h>

namespace rocks::platform {

// 플랫폼 IME가 부르는 텍스트 입력 문맥이다(UE ITextInputMethodContext의 플랫폼 쪽 —
// Docs/Design/UiToolkit 19.2절). 창 호스트가 UI 문맥(ui::TextInputContext)을 감싸 창에 넘긴다.
// 위치는 UTF-16 code unit, 사각형은 client 물리 픽셀이다.
//
// 조합 계약: BeginComposition 뒤의 SetText는 문맥의 미리보기 층만 바꾸고, EndComposition이 한 번
// 확정한다. 창은 창 소유 스레드(게임 lane)에서 ProcessTextInput·Activate·Deactivate·
// NotifyTextInputLayoutChanged 중에만 고치거나 글자를 읽는다. OS 메시지 처리 중에는
// AllowsInputMethod와 위치 질의(GetTextBounds·GetScreenBounds·GetCharacterIndexFromPoint — 읽기만,
// TSF 22.13절)만 묻는다.
class TextInputClient {
public:
    virtual ~TextInputClient() noexcept = default;

    // IME를 쓸 수 있는지다(비밀번호·읽기 전용이면 false — 창이 IME를 끈다).
    virtual bool AllowsInputMethod() const noexcept = 0;
    virtual void GetSelection(uint32& begin, uint32& length) const noexcept = 0;
    // 글자 길이(UTF-16)와 [begin, begin + length) 글자다(out 끝에 덧붙인다). 범위 밖은
    // InvalidArgument다. TSF 거울 문서를 맞출 때 읽는다(22.13절).
    virtual uint32 GetTextLength() const noexcept = 0;
    virtual Result<void> GetText(uint32 begin, uint32 length,
                                 Array<char16_t>& out) const noexcept = 0;
    virtual void SetSelection(uint32 begin, uint32 length) noexcept = 0;
    virtual void SetText(uint32 begin, uint32 length, Span<const char16_t> text) noexcept = 0;
    // [begin, begin + length) 글자의 client 사각형이다(length 0이면 caret). 모르면 false다.
    virtual bool GetTextBounds(uint32 begin, uint32 length, Box2<float>& client) const noexcept = 0;
    // 입력란 전체의 client 사각형이다(TSF GetScreenExt). 모르면 false다.
    virtual bool GetScreenBounds(Box2<float>& client) const noexcept = 0;
    // client 점에 가장 가까운 글자 위치다(TSF GetACPFromPoint). 모르면 비어 있다.
    virtual Optional<uint32> GetCharacterIndexFromPoint(Vec2 client) const noexcept = 0;
    virtual void BeginComposition() noexcept = 0;
    virtual void UpdateCompositionRange(uint32 begin, uint32 length) noexcept = 0;
    virtual void EndComposition() noexcept = 0;
};

} // namespace rocks::platform
