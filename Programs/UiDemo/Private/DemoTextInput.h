#pragma once

#include "DemoTextInputClient.h"

#include <PlatformWindow/Window.h>
#include <UiCore/TextInputService.h>

namespace rocks::ui {

// 창 하나의 텍스트 입력 서비스다(Docs/Design/UiToolkit 19.2절). UI 문맥(TextInputContext)을 플랫폼
// 문맥(platform::TextInputClient)으로 감싸 창에 넘긴다. 창 호스트가
// Application::SetTextInputService로 등록하고, 창을 부수기 전에 Detach한다(편집 요소가 서비스를 더
// 오래 쥘 수 있다).
//
// 사용 예:
//   auto service = SharedPtr<DemoTextInput>::TryMake(allocator, *window);
//   (void)app.SetTextInputService(service.Value());
//   ...
//   service.Value()->Detach();   // 창을 부수기 전
class DemoTextInput final : public TextInputService {
public:
    explicit DemoTextInput(platform::Window& window) noexcept;
    ~DemoTextInput() noexcept override;

    // 활성 문맥을 끄고 창과의 연결을 끊는다. 그 뒤 호출은 아무것도 하지 않는다.
    void Detach() noexcept;

    void Activate(TextInputContext& context) noexcept override;
    void Deactivate(TextInputContext& context) noexcept override;
    void CancelComposition(TextInputContext& context) noexcept override;
    void NotifyLayoutChanged(TextInputContext& context) noexcept override;
    void Process(uint32 record) noexcept override;

private:
    platform::Window* window_;
    DemoTextInputClient client_;
};

} // namespace rocks::ui
