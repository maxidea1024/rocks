#pragma once

#include <UiCore/TextInputContext.h>

namespace rocks::ui {

// 창 호스트가 주는 텍스트 입력 서비스다(UE ITextInputMethodSystem + ITextInputMethodChangeNotifier
// — Docs/Design/UiToolkit 19.2절). 편집 요소는 초점을 받으면 Activate, 잃으면 Deactivate한다(소멸
// 전에도 Deactivate). 앱이 조합 중에 글자를 바꾸면(되돌리기·바인딩) CancelComposition으로 알린다.
// Process는 UI 입력 adapter가 입력 큐의 IME 표지 자리에서 부른다(키·문자와 순서를 지킨다).
//
// 스레드: 게임 lane에서만 부른다.
class TextInputService {
public:
    virtual ~TextInputService() noexcept = default;

    virtual void Activate(TextInputContext& context) noexcept = 0;
    virtual void Deactivate(TextInputContext& context) noexcept = 0;
    virtual void CancelComposition(TextInputContext& context) noexcept = 0;
    // 문맥의 배치(글자 위치)가 바뀌었다(후보 창 위치를 다시 당긴다).
    virtual void NotifyLayoutChanged(TextInputContext& context) noexcept = 0;
    virtual void Process(uint32 record) noexcept = 0;
};

} // namespace rocks::ui
