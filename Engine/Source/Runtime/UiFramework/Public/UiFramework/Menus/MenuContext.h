#pragma once

#include <Core/Delegate.h>
#include <UiCore/FocusCause.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Text/TextLabel.h>

namespace rocks::ui {

class Application;
class MenuAnchor;

// 메뉴의 글자 레이블 factory다(글자를 그 글꼴로 shaping한 레이블 — UiFramework에는 글꼴 서비스가
// 없어 호출자가 준다).
using MenuLabelFactory = Delegate<Result<SharedPtr<TextLabel>>(StringView, const FontStyle&)>;

// 한 메뉴 묶음(막대 버튼 하나가 연 메뉴와 그 하위 메뉴들)이 함께 쓰는
// 문맥이다(Docs/Design/UiToolkit 9.1절 UW7c). 항목을 실행하면 루트 anchor를 닫아 하위 메뉴까지 모두
// 닫고(하위 anchor는 루트 메뉴 안에 있어 함께 분리된다), 하위 메뉴 열림 지연 타이머는
// Application으로 건다. 막대나 메뉴 소유자가 만들고 메뉴 요소들이 SharedPtr로 든다.
class ROCKS_UIFRAMEWORK_API MenuContext final {
public:
    MenuContext(Allocator& allocator, Application& application, MenuLabelFactory&& makeLabel,
                const MenuStyle& style = DefaultMenuStyle()) noexcept;

    Allocator& GetAllocator() const noexcept;
    Application& GetApplication() const noexcept;
    const MenuStyle& Style() const noexcept;
    Result<SharedPtr<TextLabel>> MakeLabel(StringView text, const FontStyle& font) const noexcept;

    // 메뉴를 처음 연 anchor(막대 버튼)다. 항목 실행·바깥 누름이 이것을 닫는다.
    void SetRoot(MenuAnchor* root) noexcept;
    MenuAnchor* Root() const noexcept;
    // 루트 메뉴를 닫는다(하위 메뉴도 함께 닫힌다). 실행 뒤 닫기가 끝나면 onClosed를 부른다.
    void CloseAll() noexcept;
    void SetOnClosedAll(Delegate<void()>&& callback) noexcept;
    // 루트 메뉴에서 왼쪽·오른쪽을 누르면 막대의 옆 메뉴로 옮긴다(step -1·+1). 막대가 없으면
    // false다.
    bool NavigateRoot(int32 step) noexcept;
    void SetOnNavigateRoot(Delegate<bool(int32)>&& callback) noexcept;

    // 메뉴 세션이다(Docs/Design/UiToolkit 9.1절 UW7c R1 — UE FMenuStack의 초점 복원). 막대 메뉴를
    // 열기 직전·F10으로 막대에 초점을 두기 직전에 부른다. 지금 초점이 area(막대)나 열린 메뉴
    // 안이면 이미 세션 중이므로 기억을 그대로 두고, 아니면 지금 초점(없으면 "없음")과 원인을
    // 기억한다.
    void BeginSession(uint32 user, const Element& area) noexcept;
    bool InSession() const noexcept;
    // 세션을 끝낸다. restore이고 지금 초점이 area·열린 메뉴 안이거나 트리에서 떨어졌으면(닫힌
    // 메뉴의 항목) 기억한 초점으로 되돌린다 — 마우스로 Edit > Paste를 골라도 붙여 넣기가 원래 입력
    // 상자로 간다. 사용자가 메뉴 밖으로 옮긴 초점은 그대로 둔다.
    void EndSession(const Element& area, bool restore = true) noexcept;
    // element가 지금 열린 메뉴(루트 메뉴와 하위 메뉴 사슬) 안에 있는지다.
    bool IsInOpenMenus(const Element& element) const noexcept;

private:
    Allocator& allocator_;
    Application& application_;
    MenuLabelFactory makeLabel_;
    MenuStyle style_;
    MenuAnchor* root_ = nullptr;
    Delegate<void()> closedAll_;
    Delegate<bool(int32)> navigateRoot_;
    WeakPtr<Element> savedFocus_;
    FocusCause savedCause_ = FocusCause::Direct;
    uint32 sessionUser_ = 0;
    bool session_ = false;
};

} // namespace rocks::ui
