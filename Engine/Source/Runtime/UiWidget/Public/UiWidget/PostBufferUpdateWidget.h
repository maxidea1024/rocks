#pragma once

#include <UiWidget/Widget.h>
#include <UiWidget/PostBufferEntry.h>
#include <UiFramework/PostBufferUpdate.h>

namespace rocks::ui {

// 위젯이 그려지는 시점의 화면을 등록한 후처리 텍스처에 복사한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API PostBufferUpdateWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    // 목적지 텍스처는 현재 패스의 크기 및 색상 형식과 일치해야 한다.
    // 처음 그린 뒤 후속 위젯이 사용할 브러시 핸들이다. 아직 준비되지 않았으면 빈 핸들이다.
    SlotHandle CapturedBuffer() const noexcept;
    void SetDestination(SlotHandle destination) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "bufferUpdates") Array<PostBufferEntry> bufferUpdates_;
    ROCKS_NATIVE() Array<PostBufferUpdateInfo> updates_;
    ROCKS_NATIVE() SlotHandle destination_ {};
};

} // namespace rocks::ui
