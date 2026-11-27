#pragma once

#include <UiCore/Styles/CoreIcons.h>
#include <UiFramework/Popups/MenuAnchor.h>
#include <UiFramework/Layout/SizeBox.h>
#include <UiFramework/Scrolling/ScrollView.h>

namespace rocks::ui {

// 콤보 아래 화살표의 크기와 둘레 여백이다(UE FComboButtonStyle DownArrowImage 16x16 —
// "wide-chevron-down", DownArrowPadding 기본 2, SlateTypes.cpp 176행).
inline constexpr float ComboArrowSize = 16.0f;
inline constexpr float ComboArrowPadding = 2.0f;

// 선택 값의 표현과 메뉴 행의 표현을 생성기로 분리한다. 생성기는 매번 부모 없는 새 Element를
// 반환해야 한다. 선택 인덱스 -1은 선택 해제다. 열려 있는 동안 방향키로 이동하고 Enter로 확정한다.
class ROCKS_UIFRAMEWORK_API ComboBox final : public MenuAnchor {
public:
    using MakeOption = Delegate<Result<SharedPtr<Element>>(usize, bool)>;
    using Super = MenuAnchor;
    struct Args {
        usize count = 0;
        int32 selected = -1;
        MakeOption makeOption{};
        Delegate<void(int32)> onSelectionChanged{};
        Delegate<void()> onOpening{};
        float maxListHeight = 450;
        ElementArgs common{};
    };

    Result<void> Construct(Args&& args) noexcept;
    explicit ComboBox(Allocator& allocator) noexcept;
    void SetOptionFactory(MakeOption&& factory) noexcept;
    void SetOnSelectionChanged(Delegate<void(int32)>&& callback) noexcept;
    // 외부에서 네이티브 참조를 계속 보유해도 해제된 게임 모델을 다시 호출하지 않는다.
    void DisconnectModel() noexcept;
    Result<void> Refresh(usize count, int32 selected) noexcept;
    // 항목 추가·삭제 후 같은 선택 항목의 인덱스만 갱신한다. 선택이 제거되면 -1을 넘긴다.
    void UpdateOptions(usize count, int32 selected) noexcept;
    Result<void> Select(int32 index) noexcept;
    Result<void> SetMaxListHeight(float height) noexcept;
    int32 SelectedIndex() const noexcept;
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;
    // 버튼 위 오른쪽에 아래 화살표를 그린다(UE SComboButton 오른쪽 칸 — 전경색 tint, 세로 가운데).
    // 화살표 자리는 내용 여백으로 비워 둔다.
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    Result<SharedPtr<Element>> MakeMenu() noexcept;
    Result<void> SetDisplay(int32 index) noexcept;
    void Highlight(int32 index) noexcept;
    Allocator& allocator_;
    float maxListHeight_ = 450;
    WeakPtr<SizeBox> menuBounds_;
    WeakPtr<ScrollView> scroll_;
    MakeOption makeOption_;
    Delegate<void(int32)> selectionChanged_;
    usize count_ = 0;
    int32 selected_ = -1;
    int32 highlighted_ = -1;
    Array<WeakPtr<Button>> rows_;
    bool changing_ = false;
};

} // namespace rocks::ui
