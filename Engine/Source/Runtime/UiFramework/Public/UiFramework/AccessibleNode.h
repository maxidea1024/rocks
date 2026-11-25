#pragma once

#include <Core/Text/String.h>
#include <UiCore/Element.h>
#include <Core/Memory/SharedPtr.h>

namespace rocks::ui {
class Element;
// 플랫폼 접근성 어댑터가 UI 스레드에서 만든 스냅샷이다. 문자열과 창 좌표는 다음 프레임에도
// 유효하며 native는 접근성 동작을 전달할 때만 잠그는 약한 참조다.
struct AccessibleNode {
    explicit AccessibleNode(Allocator& allocator) noexcept : name(allocator), summary(allocator) {
    }
    String name;
    String summary;
    Box2<float> bounds{};
    WeakPtr<Element> native;
    usize parent = IndexNone;
    AccessibleRole role = AccessibleRole::Group;
    bool enabled = true;
    bool focusable = false;
    bool focused = false;
};
} // namespace rocks::ui
