#pragma once

#include <Object/ClassRef.h>
#include <UiWidget/UserWidget.h>
#include <UiWidget/WidgetEventBinding.h>
#include <UiWidget/WidgetPropertyBinding.h>
#include <UiWidget/WidgetModelSource.h>

namespace rocks::ui {

class WidgetEventCatalog;

// 컴파일된 위젯 트리와 바인딩을 이용해 독립적인 화면 인스턴스를 생성한다.
class ROCKS_UIWIDGET_API WidgetTemplate {
public:
    explicit WidgetTemplate(Allocator& allocator) noexcept;

    // 에셋 저장소와 다른 게임 저장소에도 생성할 수 있다. 객체 그래프와 결합을 한 번에 공개한다.
    Result<RootRef<UserWidget>> Instantiate(ObjectRegistry& objects, ObjectHandle outer = {},
                                            StringView name = {},
                                            const WidgetEventCatalog* events = nullptr,
                                            WidgetModelSource model = {}) const noexcept;

private:
    friend class WidgetAsset;
    struct Binding {
        TypeId declaringType;
        Guid field;
        usize nodeIndex;
        bool animation;
    };

    struct EventConnection {
        Array<WidgetEventBinding> bindings;
        TypeId senderType;
        usize nodeIndex;
    };

    Result<void> ConnectViewModel(ObjectRegistry& objects, ConstructionContext& stage,
                                  ObjectHandle owner, UserWidget& screen,
                                  Span<const ObjectHandle> nodes,
                                  WidgetModelSource source) const noexcept;
    struct PropertyConnection {
        WidgetPropertyBinding binding;
        TypeId targetType;
        usize nodeIndex;
    };
    ClassRef<UserWidget> controller_;
    TypeId viewModelType_{};
    Array<PropertyConnection> properties_;
    Array<Array<uint8>> animations_;
    Array<uint8> graph_;
    Array<Binding> bindings_;
    Array<EventConnection> events_;
};

} // namespace rocks::ui
