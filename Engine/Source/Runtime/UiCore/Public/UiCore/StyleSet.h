#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <UiCore/Style.h>

// 이름으로 형식별 스타일을 찾는 표다(Docs/Design/UiToolkit 9.1절 UW3b — UE FSlateStyleSet의
// GetWidgetStyle). 전역 등록부는 없고, 소유자(에디터·게임)가 집합을 만들어 위젯에 넘긴다(4.1절).
//
// 사용 예:
//   StyleSet styles(allocator);
//   if (auto added = AddCoreStyles(styles); !added) { return added; }
//   const ButtonStyle* primary = styles.FindButton("PrimaryButton");
namespace rocks::ui {

class ROCKS_UICORE_API StyleSet {
public:
    explicit StyleSet(Allocator& allocator) noexcept;

    StyleSet(const StyleSet&) = delete;
    StyleSet& operator=(const StyleSet&) = delete;

    // 같은 이름이 있으면 값을 바꾼다. 빈 이름은 InvalidArgument, 할당 실패면 표를 바꾸지 않는다.
    Result<void> SetButton(StringView name, const ButtonStyle& style) noexcept;
    Result<void> SetText(StringView name, const TextStyle& style) noexcept;

    // 없으면 nullptr이다. 반환한 주소는 다음 Set 호출 전까지 유효하다. 이름은 대소문자를 가린다.
    const ButtonStyle* FindButton(StringView name) const noexcept;
    const TextStyle* FindText(StringView name) const noexcept;

    // 없으면 형식의 기본 스타일(DefaultButtonStyle·NormalTextStyle)이다(UE는 경고 뒤 기본값).
    const ButtonStyle& GetButton(StringView name) const noexcept;
    const TextStyle& GetText(StringView name) const noexcept;

private:
    template <typename T>
    struct Entry {
        String name;
        T style;
    };

    template <typename T>
    static Result<void> Set(Allocator& allocator, Array<Entry<T>>& entries, StringView name,
                            const T& style) noexcept;

    template <typename T>
    static const T* Find(const Array<Entry<T>>& entries, StringView name) noexcept;

    Allocator& allocator_;
    Array<Entry<ButtonStyle>> buttons_;
    Array<Entry<TextStyle>> texts_;
};

// UE 5.8 에디터 핵심 스타일 중 Rocks 형식이 있는 항목을 넣는다(14장 — "Button", "PrimaryButton",
// "SimpleButton", "NormalText", "SmallText", "Text.Large", "PrimaryButtonText"). 이름은 UE 키와
// 같다 (StarshipCoreStyle.cpp 1333–1339행, 1503–1511행).
ROCKS_UICORE_API Result<void> AddCoreStyles(StyleSet& styles) noexcept;

} // namespace rocks::ui
