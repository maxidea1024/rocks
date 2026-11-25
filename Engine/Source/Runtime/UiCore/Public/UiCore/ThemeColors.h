#pragma once

#include <Core/Math/Color.h>
#include <Core/Memory/Allocator.h>

namespace rocks::ui {

// 색의 용도를 저장하며 실제 선형 RGB 값은 현재 앱의 테마에서 읽는다.
enum class ThemeSlot : uint8 {
    Black,
    Title,
    WindowBorder,
    Foldout,
    Input,
    InputOutline,
    Recessed,
    Background,
    Panel,
    Header,
    Dropdown,
    DropdownOutline,
    Hover,
    Hover2,
    White,
    White25,
    Highlight,
    Primary,
    PrimaryHover,
    PrimaryPress,
    Secondary,
    Foreground,
    ForegroundHover,
    ForegroundInverted,
    ForegroundHeader,
    Select,
    SelectInactive,
    SelectParent,
    SelectHover,
    Notifications,
    AccentBlue,
    AccentPurple,
    AccentPink,
    AccentRed,
    AccentOrange,
    AccentYellow,
    AccentGreen,
    AccentBrown,
    AccentBlack,
    AccentGray,
    AccentWhite,
    AccentFolder,
    Warning,
    Error,
    Success,
    User1,
    User2,
    User3,
    User4,
    User5,
    User6,
    User7,
    User8,
    User9,
    User10,
    User11,
    User12,
    User13,
    User14,
    User15,
    User16,
    Count
};

class ROCKS_UICORE_API ThemeColors {
public:
    ThemeColors() noexcept;
    LinearColor Get(ThemeSlot slot) const noexcept;
    // 각 문서는 {"colors":{"Panel":"#242424"}} 형식이다. 빈 단계는 건너뛰고 사용자 값을
    // 마지막에 적용한다. 알 수 없는 키나 잘못된 색이 있으면 이전 테마 전체를 유지한다.
    Result<void> Load(Allocator& allocator, StringView engine, StringView project = {},
                      StringView user = {}) noexcept;
    bool operator==(const ThemeColors&) const noexcept = default;
private:
    LinearColor values_[static_cast<usize>(ThemeSlot::Count)]{};
};

} // namespace rocks::ui
