#pragma once

#include <AssetRuntime/Package.h>
#include <UiWidget/WidgetBinding.h>
#include <UiWidget/WidgetTemplate.h>

namespace rocks::ui {

// 게임 화면의 원본 트리를 보관한다. Compile은 필드 바인딩과 트리 연결을 검증한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API WidgetAsset final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit WidgetAsset(Allocator& allocator) noexcept;
    Result<void> Initialize(ObjectRegistry& objects) noexcept;
    ObjectHandle Tree() const noexcept;
    // 애니메이션을 에셋의 하위 객체로 만들고 영속 ID를 할당한다.
    Result<RootRef<WidgetAnimation>> CreateAnimation(ObjectRegistry& objects,
                                                     StringView name) noexcept;
    Span<const ObjectHandle> Animations() const noexcept;
    void SetController(ClassRef<UserWidget> controller) noexcept;
    Result<void> AddBinding(WidgetBinding binding) noexcept;
    // 한 이벤트에 연결한 여러 메서드는 등록 순서대로 호출한다.
    // 모델 타입을 생략하면 현재 화면 컨트롤러가 원천이다.
    void SetViewModelType(TypeId model) noexcept;
    Result<void> AddPropertyBinding(WidgetPropertyBinding binding) noexcept;
    Result<void> AddEventBinding(WidgetEventBinding binding) noexcept;
    Result<WidgetTemplate> Compile(ObjectRegistry& source, Allocator& allocator,
                                   const WidgetEventCatalog* events = nullptr) const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "templateTree") ObjectRef<WidgetTree> tree_;
    ROCKS_PROPERTY(ReadOnly, Name = "animations") ObjectRefArray<WidgetAnimation> animations_;
    ROCKS_PROPERTY(Name = "controller") ClassRef<UserWidget> controller_;
    ROCKS_PROPERTY(Name = "bindings") Array<WidgetBinding> bindings_;
    ROCKS_PROPERTY(Name = "viewModelType") Guid viewModelType_ {};
    ROCKS_PROPERTY(Name = "propertyBindings") Array<WidgetPropertyBinding> propertyBindings_;
    ROCKS_PROPERTY(Name = "eventBindings") Array<WidgetEventBinding> eventBindings_;
};

} // namespace rocks::ui
