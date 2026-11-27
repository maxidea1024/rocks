#pragma once
#include <UiWidget/ContentWidget.h>
#include <UiScreens/ScreenDeviceState.h>

namespace rocks::ui {

// 한 플레이어의 입력 장치에 따라 콘텐츠를 표시한다. 장치가 바뀔 때만 가시성을 다시 평가한다.
// mask의 비트 순서는 키보드·마우스(1), 게임패드(2), 터치(4)다. 숨김은 레이아웃 유지 또는 회수를
// 선택한다.
ROCKS_CLASS()
class ROCKS_UISCREENS_API HardwareVisibilityWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit HardwareVisibilityWidget(Allocator& allocator) noexcept;
    Result<void> SetDeviceState(const SharedPtr<ScreenDeviceState>& state) noexcept;
    Result<void> SetDeviceMask(uint8 mask, Visibility hidden = Visibility::Collapsed) noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    Result<void> Subscribe(const SharedPtr<Element>& live) noexcept;
    void Unsubscribe() noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "deviceMask") uint8 deviceMask_ = 7;
    ROCKS_PROPERTY(ReadOnly, Name = "hiddenMode")
    uint8 hiddenMode_ = static_cast<uint8>(Visibility::Collapsed);
    ROCKS_NATIVE() SharedPtr<ScreenDeviceState> state_;
    ROCKS_NATIVE() DelegateHandle handle_ {};
};
} // namespace rocks::ui
