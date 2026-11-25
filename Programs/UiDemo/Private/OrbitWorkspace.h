#pragma once

#include "DemoText.h"
#include "GameHud.h"
#include "OrbitView.h"

#include <UiFramework/AnchorCanvas.h>
#include <UiFramework/Docking/DockManager.h>
#include <UiFramework/Timeline.h>
#include <UiFramework/Commands.h>

namespace rocks::ui {

// 모든 도킹 창이 닫힐 때까지 입력 콜백과 화면 상태를 소유한다.
class OrbitWorkspace {
public:
    OrbitWorkspace(Allocator& allocator, ObjectRegistry& objects, DemoText& font) noexcept;
    const SharedPtr<CommandList>& Commands() const noexcept;
    Result<SharedPtr<DockManager>> Create(SlotHandle map) noexcept;

private:
    Result<SharedPtr<TextLabel>> Label(StringView value, int32 size, Color color) noexcept;
    Result<SharedPtr<Frame>> Tab(StringView name) noexcept;
    Result<SharedPtr<Element>> Preview(SlotHandle map) noexcept;
    Result<SharedPtr<Element>> Inspector() noexcept;
    Result<void> SetProgress(float value) noexcept;
    Result<SharedPtr<Element>> BuildTimeline() noexcept;
    Result<void> BindCommands() noexcept;
    void Seek(float value) noexcept;
    void Rewind() noexcept;

    Allocator& allocator_;
    ObjectRegistry& objects_;
    DemoText& font_;
    GameHud hud_{};
    SharedPtr<OrbitView> orbit_;
    SharedPtr<TextLabel> percent_;
    SharedPtr<Slider> slider_;
    SharedPtr<Timeline> timeline_;
    SharedPtr<TextLabel> phase_;
    CommandSet definitions_;
    BindingManager bindings_;
    SharedPtr<CommandList> commands_;
    int32 displayed_ = -1;
    int32 phaseDegrees_ = -1;
};

} // namespace rocks::ui
