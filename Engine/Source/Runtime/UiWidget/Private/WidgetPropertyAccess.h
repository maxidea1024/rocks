#pragma once

#include <Object/ConstructionContext.h>
#include <UiWidget/WidgetFieldBindings.h>
#include <UiWidget/WidgetPropertyBinding.h>

namespace rocks::ui {

// 생성 트랜잭션 안에서는 staged 접근자를, 게시 후 변경 통지에서는 live 접근자를 쓴다.
struct WidgetPropertyAccess {
    Result<void> (*apply)(ConstructionContext&, ObjectHandle, const FieldValue&,
                          Allocator&) noexcept;
    WidgetFieldBindings::Setter (*live)() noexcept;
    bool notifies;
};

Result<WidgetPropertyAccess>
ValidateWidgetPropertyBinding(const TypeLease& source, const TypeLease& target,
                              const WidgetPropertyBinding& binding) noexcept;

Result<WidgetPropertyAccess> ResolveWidgetProperty(const TypeLease& widget, Guid field) noexcept;

} // namespace rocks::ui
