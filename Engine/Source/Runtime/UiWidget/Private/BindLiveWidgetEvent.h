#pragma once

#include <UiWidget/Widget.h>

namespace rocks::ui {

// 해제된 네이티브 위젯이 뒤늦게 이벤트를 보내더라도 재구성된 모델에 전달하지 않는다.
template <typename W, typename... A>
Delegate<void(A...)> BindLiveWidgetEvent(ObjectRegistry& objects, ObjectHandle owner,
                                         const Element* native,
                                         Delegate<void(A...)> W::* event) noexcept {
    return Delegate<void(A...)>::CreateLambda([&objects, owner, native, event](A... args) noexcept {
        auto borrow = objects.ResolveForWrite<W>(objects.OwnerAccess(), owner);
        if (borrow) {
            auto* widget = borrow.Value().Get().Value();
            if (widget->CachedElement().Get() == native) {
                (widget->*event).ExecuteIfBound(args...);
            }
        }
    });
}

// Element 사건을 Widget의 리플렉션 이벤트로 발행한다(Docs/Design/UiToolkit 21.5a절). 해제된
// Element가 뒤늦게 보낸 사건은 재구성된 Widget에 보내지 않는다. Element delegate 인자 형식이 이벤트
// 형식과 같다.
template <typename W, typename... A>
Delegate<void(A...)> BindLiveWidgetEvent(ObjectRegistry& objects, ObjectHandle owner,
                                         const Element* native, Event<A...> W::* event) noexcept {
    return Delegate<void(A...)>::CreateLambda([&objects, owner, native, event](A... args) noexcept {
        auto borrow = objects.ResolveForWrite<W>(objects.OwnerAccess(), owner);
        if (borrow) {
            auto* widget = borrow.Value().Get().Value();
            if (widget->CachedElement().Get() == native) {
                (widget->*event).Broadcast({objects, objects.OwnerAccess(), owner}, args...);
            }
        }
    });
}

} // namespace rocks::ui
