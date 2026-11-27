#pragma once

#include <Object/ClassRef.h>
#include <UiWidget/DynamicEntryBoxBaseWidget.h>
#include <UiWidget/UserWidget.h>

namespace rocks::ui {

// 아이템 수가 적고 행을 모두 표시하는 화면에서 UserWidget을 생성하고 회수한다.
// CreateEntry는 같은 클래스의 회수된 인스턴스를 우선 사용한다. 재대여 시 모델 데이터를
// 다시 설정해야 한다. 화면 밖의 항목까지 많으면 가시 행만 생성하는 ListViewWidget을 쓴다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API DynamicEntryBoxWidget final : public DynamicEntryBoxBaseWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit DynamicEntryBoxWidget(Allocator& allocator) noexcept;
    Result<ObjectHandle> CreateEntry(ObjectRegistry& objects, ClassRef<UserWidget> type) noexcept;
    Result<void> RemoveChild(ObjectRegistry& objects, ObjectHandle child) noexcept override;

private:
    ROCKS_PROPERTY(Transient, ReadOnly, Name = "inactive") ObjectRefArray<UserWidget> inactive_;
    ROCKS_NATIVE() uint64 nextEntry_ = 0;
    ROCKS_NATIVE() bool changing_ = false;
};

} // namespace rocks::ui
