#pragma once

#include <Core/Delegate.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 입력 컨트롤의 텍스트 자식을 연결하고 화면 트리의 셰이퍼로 갱신할 콜백을 만든다.
Result<Delegate<Result<void>(StringView)>>
AttachControlCaption(ObjectRegistry& objects, ObjectHandle owner, Element& control) noexcept;

} // namespace rocks::ui
