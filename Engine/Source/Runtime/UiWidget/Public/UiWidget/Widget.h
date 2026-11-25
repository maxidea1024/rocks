#pragma once

#include <Meta/Annotations.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <UiCore/Element.h>
#include <UiWidget/WidgetTransform.h>
#include <UiWidget/WidgetNavigationRule.h>
#include <Localization/Text.h>

namespace rocks::ui {

class Application;
class PanelWidget;
class WidgetSlot;
class WidgetFieldBindings;

// 게임 스레드의 Borrow 범위에서 사용한다. TakeElement는 화면에 붙일 트리를 만들거나 재사용한다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API Widget : public Object {
    ROCKS_GENERATED_BODY()

public:
    Result<SharedPtr<Element>> TakeElement(ObjectRegistry& objects) noexcept;
    virtual void ReleaseElements() noexcept;
    void SetEnabled(bool enabled) noexcept;
    bool IsEnabled() const noexcept;
    // Collapsed는 배치 공간까지 회수하고 Hidden은 배치 공간을 유지한다.
    // 처음 값을 지정하기 전에는 네이티브 위젯의 기본 히트 테스트 정책을 따른다.
    Result<void> SetVisibility(Visibility visibility) noexcept;
    Visibility GetVisibility() const noexcept;
    // 매 프레임 그려야 하는 서브트리에만 지정한다. 자체 애니메이션은 별도로 갱신된다.
    void SetForceVolatile(bool enabled) noexcept;
    bool IsForceVolatile() const noexcept;
    void SetClipToBounds(bool enabled) noexcept;
    bool IsVisible() const noexcept;
    bool IsRendered() const noexcept;
    bool ClipsToBounds() const noexcept;
    float GetRenderOpacity() const noexcept;
    Result<void> SetRenderOpacity(float opacity) noexcept;

    // 애셋으로 저장한 변환을 화면 재생성 시 복원한다. 원하는 크기는 유지하고 히트 테스트에는
    // 반영한다.
    const WidgetTransform& GetRenderTransform() const noexcept;
    Result<void> SetRenderTransform(const WidgetTransform& transform) noexcept;
    Result<void> SetRenderScale(Vec2 scale) noexcept;
    Result<void> SetRenderShear(Vec2 shear) noexcept;
    Result<void> SetRenderTranslation(Vec2 translation) noexcept;
    Result<void> SetRenderTransformAngle(float degrees) noexcept;
    Vec2 GetRenderTransformPivot() const noexcept;
    Result<void> SetRenderTransformPivot(Vec2 pivot) noexcept;

    // 포커스와 캡처는 창과 사용자마다 독립적이다. 화면이 부착된 Application을 전달한다.
    Result<void> SetCursor(input::MouseCursor cursor) noexcept;
    void ResetCursor() noexcept;
    bool IsHovered(const Application& app, uint32 user = 0) const noexcept;
    bool SupportsKeyboardFocus() const noexcept;
    bool HasUserFocus(const Application& app, uint32 user = 0) const noexcept;
    bool HasFocusedDescendants(const Application& app, uint32 user = 0) const noexcept;
    bool HasAnyUserFocus(const Application& app) const noexcept;
    bool HasMouseCapture(const Application& app, uint32 user = 0,
                         uint32 pointer = 0) const noexcept;
    Result<void> SetUserFocus(Application& app, uint32 user = 0) noexcept;
    Vec2 GetDesiredSize() const noexcept;
    Result<Geometry> GetCachedGeometry() const noexcept;
    Result<void> ForceLayoutPrepass(float scale = 1) noexcept;
    void InvalidateLayout() noexcept;

    // 패널 슬롯의 소유자를 조회한다. 루트 위젯은 빈 핸들을 반환한다.
    Result<ObjectHandle> GetParent(ObjectRegistry& objects) const noexcept;
    virtual Result<void> RemoveFromParent(ObjectRegistry& objects) noexcept;
    ObjectHandle Slot() const noexcept;

    // 명시 대상은 같은 WidgetTree의 안정 ID다. 모달 경계와 다른 템플릿 인스턴스를 넘어가지 않는다.
    Result<void> SetNavigationRule(FocusDirection direction, NavigationRule rule,
                                   Guid target = {}) noexcept;
    Result<void> SetAllNavigationRules(NavigationRule rule) noexcept;
    Result<void> SetNavigationHandler(ObjectRegistry& objects,
                                      NavigationHandler&& handler) noexcept;
    NavigationBoundary GetNavigationRule(FocusDirection direction) const noexcept;

    // 툴팁 내용은 지연 후에 열린다. 명시적 위젯이 있으면 본문보다 먼저 표시한다.
    Result<void> SetToolTipText(ObjectRegistry& objects, const Text& text) noexcept;
    Result<void> SetToolTip(ObjectRegistry& objects, ObjectHandle content) noexcept;
    const Text& ToolTipText() const noexcept;
    ObjectHandle ToolTipWidget() const noexcept;

    // 이름과 요약의 수집 방식은 각각 정한다. 화면 판독기는 자식 허용 여부와 가시성도 따른다.
    Result<void> SetAccessibleText(ObjectRegistry& objects, const Text& name,
                                   const Text& summary) noexcept;
    Result<void> SetAccessibleBehavior(ObjectRegistry& objects, AccessibleBehavior name,
                                       AccessibleBehavior summary, bool allowChildren) noexcept;
    const Text& GetAccessibleText() const noexcept;
    const Text& GetAccessibleSummaryText() const noexcept;
    // 모델로 사용한 위젯의 알림 저장소다. 구독자가 있는 속성만 변경을 전달한다.
    Result<SharedPtr<WidgetFieldBindings>> FieldNotifications(ObjectRegistry& objects) noexcept;
    Guid StableId() const noexcept;
    SharedPtr<Element> CachedElement() const noexcept;

protected:
    void NotifyPropertyChanged(StringView name) noexcept;
    virtual Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept = 0;
    virtual void ApplyProperties(Element& element) noexcept;
    virtual void OnRebuilt() noexcept;

private:
    Result<ObjectHandle> PresentationTree(ObjectRegistry& objects) const noexcept;
    WidgetNavigationRule* NavigationEntry(FocusDirection direction) noexcept;
    Result<void> ApplySemantics(ObjectRegistry& objects, Element& element) noexcept;
    Result<void> ApplyToolTip(ObjectRegistry& objects, Element& element, const Text& text,
                              ObjectHandle custom) noexcept;
    Result<void> ApplyAccessibility(ObjectRegistry& objects, Element& element, const Text& name,
                                    const Text& summary, AccessibleBehavior behavior,
                                    AccessibleBehavior summaryBehavior,
                                    bool allowChildren) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "toolTipText") Text toolTipText_;
    ROCKS_PROPERTY(ReadOnly, Name = "toolTipWidget") ObjectRef<Widget> toolTipWidget_;
    ROCKS_PROPERTY(ReadOnly, Name = "accessibleText") Text accessibleText_;
    ROCKS_PROPERTY(ReadOnly, Name = "accessibleSummaryText") Text accessibleSummaryText_;
    ROCKS_PROPERTY(ReadOnly, Name = "accessibleBehavior") uint8 accessibleBehavior_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "accessibleSummaryBehavior")
    uint8 accessibleSummaryBehavior_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "childrenAccessible") bool childrenAccessible_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "navLeft") WidgetNavigationRule navLeft_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "navRight") WidgetNavigationRule navRight_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "navUp") WidgetNavigationRule navUp_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "navDown") WidgetNavigationRule navDown_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "navNext") WidgetNavigationRule navNext_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "navPrevious") WidgetNavigationRule navPrevious_ {};
    ROCKS_NATIVE() SharedPtr<NavigationHandler> navigationHandler_;
    friend class PanelWidget;
    friend class WidgetTree;
    ROCKS_PROPERTY(ReadOnly, Name = "stableId") Guid stableId_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "slot") ObjectRef<WidgetSlot> slot_;
    ROCKS_PROPERTY(ReadOnly, Name = "enabled") bool enabled_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "visibility") uint8 visibility_ = 255;
    ROCKS_PROPERTY(ReadOnly, Name = "forceVolatile") bool forceVolatile_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "clipToBounds") uint8 clipToBounds_ = 255;
    ROCKS_PROPERTY(ReadOnly, Name = "cursorOverride") uint8 cursorOverride_ = 255;
    ROCKS_PROPERTY(ReadOnly, Name = "renderTransform") WidgetTransform transform_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "renderPivot") Vec2 renderPivot_ { 0.5f, 0.5f };
    ROCKS_PROPERTY(ReadOnly, Name = "renderOpacity") float renderOpacity_ = 1;
    ROCKS_NATIVE() SharedPtr<WidgetFieldBindings> fieldNotifications_;
    ROCKS_NATIVE() WeakPtr<Element> cached_;
    ROCKS_NATIVE() bool rebuilding_ = false;
};

ROCKS_UIWIDGET_API Result<void> RegisterWidgetTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks::ui
