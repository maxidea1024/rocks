#pragma once

#include <Core/Math/Rect.h>
#include <Core/Memory/SharedPtr.h>
#include <UiCore/FrameTime.h>
#include <UiCore/StyleRegistry.h>
#include <UiCore/TextInputService.h>

namespace rocks::ui {

// 창 호스트가 UI에 주는 서비스 묶음이다(Docs/Design/UiToolkit 20.2절). Application마다 하나이고
// Application이 루트(툴팁 임시 호스트 포함)에 단다. Element는 Element::FindUiHostServices로
// 루트까지 올라가 찾는다. 트리에 붙기 전에는 못 찾는다(시각 0, 서비스 없음).
//
// - 팝업 허용 영역(9.1절 UW7d): 창 좌표 지점이 놓인 모니터의 작업 영역을 창 좌표로 준다.
// - 텍스트 입력 서비스(19.2절): 편집 요소가 초점 때 Activate한다.
// - 프레임 시각(20.3절): Application::BeginFrame이 정한다. 그리기 밖(속성 갱신·배치·입력)에서도
//   같은 값을 읽는다.
//
// 스레드: 게임 lane에서만 쓴다.
class ROCKS_UICORE_API UiHostServices {
public:
    using PopupWorkArea = Delegate<bool(Vec2 window, Box2<float>& area)>;

    void SetPopupWorkArea(PopupWorkArea&& provider) noexcept;
    // 공급자가 없거나 모르면 false다.
    bool QueryPopupWorkArea(Vec2 window, Box2<float>& area) const noexcept;

    void SetTextInputService(SharedPtr<TextInputService> service) noexcept;
    const SharedPtr<TextInputService>& TextInput() const noexcept;

    void SetFrameTime(const FrameTime& time) noexcept;
    const FrameTime& Time() const noexcept;

    // 스타일 등록부와 활성 앱 스타일 집합이다(20.11절 — UE FAppStyle). 바꾸면 StyleRevision이
    // 오른다.
    void SetStyles(SharedPtr<StyleRegistry> registry, SharedPtr<StyleSet> appStyle) noexcept;
    // 활성 집합에서 부모로 올라가며 찾는다. 없으면 nullptr이다(부품은 내장 기본값을 쓴다).
    template <typename T>
    const T* FindStyle(StringView name) const noexcept {
        return registry_ && appStyle_ ? registry_->FindStyle<T>(*appStyle_, name) : nullptr;
    }

    // 스타일이 바뀐 횟수다(등록부·집합 값·활성 집합). 0이 아니다(처음 측정에서 한 번 적용하게).
    uint64 StyleRevision() const noexcept;

private:
    PopupWorkArea popupWorkArea_;
    SharedPtr<TextInputService> textInput_;
    FrameTime time_{};
    SharedPtr<StyleRegistry> registry_;
    SharedPtr<StyleSet> appStyle_;
    uint64 styleChanges_ = 1;
};

} // namespace rocks::ui
