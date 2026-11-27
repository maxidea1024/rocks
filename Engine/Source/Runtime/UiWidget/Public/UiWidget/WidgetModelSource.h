#pragma once

#include <UiWidget/WidgetFieldBindings.h>

namespace rocks::ui {

// 화면의 모든 속성 바인딩이 읽을 모델이다. Widget 모델의 변경 통지는 자동으로 연결한다.
// 다른 Object 모델은 네이티브 setter에서 notifications->Notify를 호출하거나 Write로 값을 바꾼다.
struct WidgetModelSource {
    ObjectHandle object;
    SharedPtr<WidgetFieldBindings> notifications;
};

// 화면 수명 동안 구독을 유지한다. 인스턴스 생성 도중 실패해도 설치한 구독을 모두 해제한다.
class ROCKS_UIWIDGET_API WidgetModelConnections {
public:
    WidgetModelConnections(Allocator& allocator, SharedPtr<WidgetFieldBindings> source) noexcept;
    ~WidgetModelConnections() noexcept;
    Result<void> Add(ObjectHandle source, Guid field, ObjectHandle target,
                     WidgetFieldBindings::Setter setter) noexcept;

    Result<void> AddTwoWay(ObjectHandle model, Guid field, ObjectHandle target, Guid targetField,
                           SharedPtr<WidgetFieldBindings> targetChanges,
                           WidgetFieldBindings::Setter forward,
                           WidgetFieldBindings::Setter reverse) noexcept;

private:
    struct Subscription {
        SharedPtr<WidgetFieldBindings> stream;
        DelegateHandle handle;
    };
    Result<void> Subscribe(const SharedPtr<WidgetFieldBindings>& stream, ObjectHandle source,
                           Guid field, ObjectHandle target,
                           WidgetFieldBindings::Setter setter) noexcept;
    SharedPtr<WidgetFieldBindings> source_;
    Array<Subscription> handles_;
};

} // namespace rocks::ui
