#pragma once

#include <Meta/Annotations.h>
#include <Object/ObjectRegistry.h>

namespace rocks::ui {

class Widget;

// 패널에 추가된 자식의 GC 참조를 보관한다. 슬롯의 outer는 그 슬롯을 소유한 패널이다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API WidgetSlot : public Object {
    ROCKS_GENERATED_BODY()
public:
    ObjectHandle Content() const noexcept;
    // 현재 연결된 부모 패널을 돌려준다. 분리된 슬롯은 빈 핸들을 반환한다.
    Result<ObjectHandle> Parent(ObjectRegistry& objects) const noexcept;

private:
    friend class PanelWidget;
    ROCKS_PROPERTY(ReadOnly, Name = "content") ObjectRef<Widget> content_;
};

} // namespace rocks::ui
