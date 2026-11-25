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
    explicit PostBufferUpdateWidget(Allocator& allocator) noexcept;

    // 직렬화되는 채널·블러 항목을 바꾼다. 항목이 규칙 밖이면 InvalidArgument이고 이전 값을 둔다.
    Result<void> SetBufferUpdates(Span<const PostBufferEntry> entries) noexcept;

    // 실행 중 갱신 목록을 바꾼다(직렬화되지 않는다). 규칙은 SetBufferUpdates와 같다.
    Result<void> SetUpdates(Span<const PostBufferUpdateInfo> updates) noexcept;

    // 채널의 후처리 텍스처 handle이다. 아직 없으면 빈 handle이다.
    SlotHandle GetPostBuffer(uint32 channel) const noexcept;

    // 처음 그린 뒤 후속 위젯이 사용할 브러시 핸들이다. 아직 준비되지 않았으면 빈 핸들이다.
    SlotHandle CapturedBuffer() const noexcept;

    // 목적지 텍스처는 현재 패스의 크기 및 색상 형식과 일치해야 한다.
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
