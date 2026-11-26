#pragma once

#include <UiFramework/Button.h>
#include <Core/Optional.h>

namespace rocks::ui {

class Application;
class MenuPopupLayer;
enum class MenuPlacement : uint8 {
    Below,
    CenteredBelow,
    BelowRight,
    ComboBox,
    ComboBoxRight,
    Right,
    Above,
    CenteredAbove,
    AboveRight,
    Left,
    Center,
    RightCenter,
    MatchBottomLeft
};

// 버튼에 붙여 둔 내용을 기준으로 메뉴를 연다. 메뉴는 가장 바깥 Overlay에서 그리므로
// 스크롤·패널 클리핑을 벗어날 수 있다. 아래쪽 공간이 좁으면 위쪽을 사용한다.
// 메뉴 밖의 누름이나 Escape로 닫으며, Close는 닫힌 상태에서도 호출할 수 있다.
class ROCKS_UIFRAMEWORK_API MenuAnchor : public Button {
public:
    explicit MenuAnchor(Allocator& allocator) noexcept;
    ~MenuAnchor() noexcept override;

    using MenuFactory = Delegate<Result<SharedPtr<Element>>()>;
    using Super = Button;
    struct Args {
        ElementChild content{};
        MenuFactory makeMenu{};
        Delegate<void()> onOpening{};
        MenuPlacement placement = MenuPlacement::Below;
        bool fitInWindow = true;
        ElementArgs common{};
    };

    Result<void> Construct(Args&& args) noexcept;
    // 메뉴를 만들기 직전에 호출한다. 콤보 상자에서는 이 시점에 항목을 갱신할 수 있다.
    void SetOnOpening(Delegate<void()>&& callback) noexcept;
    void SetMenuFactory(MenuFactory&& factory) noexcept;
    // 소유 모델을 해제할 때 생성기를 끊고 열린 메뉴도 제거한다.
    void DisconnectMenu() noexcept;
    Result<void> Open() noexcept;
    // 메뉴 안의 첫 포커스 가능 요소로 이동한다. 메뉴가 닫힐 때 그 안에 포커스가 남아 있으면
    // 열기 직전의 요소로 복귀한다. 사용자가 다른 곳으로 옮긴 포커스는 유지한다.
    // cause는 메뉴를 연 입력이다(Enter·Space·게임패드면 Navigation — 첫 항목에 외곽선). 비우면 직전
    // 초점의 원인을 쓴다.
    Result<void> OpenFocused(Application& app, uint32 user = 0,
                             Optional<FocusCause> cause = {}) noexcept;
    void SetPlacement(MenuPlacement placement) noexcept;
    // 메뉴 층이 메뉴 밖 누름을 받아 닫을지다(기본 켬). 하위 메뉴·메뉴 막대 버튼은 끈다 — 층이 창
    // 전체를 덮으면 부모 메뉴나 막대의 hover·누름을 가로챈다. 끄면 층은 메뉴 영역만 받고, 바깥
    // 누름은 루트 메뉴 층이나 Application 누름 관찰자(메뉴 막대)가 처리한다.
    void SetCatchOutsidePresses(bool catchOutside) noexcept;
    void SetFitInWindow(bool fit) noexcept;
    Optional<Vec2> MenuPosition() const noexcept;
    // 열린 메뉴 내용(factory가 만든 요소)이다. 닫혀 있으면 null이다.
    Element* MenuContent() const noexcept;
    void SetOnOpenChanged(Delegate<void(bool)>&& callback) noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept;

    Result<void> Arrange(const Geometry& geometry) noexcept override;
    bool OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    Allocator& allocator_;
    MenuFactory factory_;
    Delegate<void()> openingEvent_;
    SharedPtr<MenuPopupLayer> layer_;
    friend class MenuPopupLayer;
    MenuPlacement placement_ = MenuPlacement::Below;
    bool fit_ = true;
    bool catchOutside_ = true;
    WeakPtr<Application*> app_;
    WeakPtr<Element> restoreFocus_;
    uint32 focusUser_ = 0;

    Delegate<void(bool)> openChanged_;
    bool opening_ = false;
};

} // namespace rocks::ui
