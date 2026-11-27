#pragma once

#include <UiWidget/Widget.h>
#include <UiCore/Brush.h>

namespace rocks::ui {

// 브러시 이미지의 크기와 색조를 설정한다. GPU 자원 핸들은 런타임에 SetBrush로 연결한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ImageWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetImageSize(Vec2 value) noexcept;
    Vec2 ImageSize() const noexcept;
    // 색조와 논리 크기는 애셋에 보존한다. 브러시의 텍스처 핸들은 렌더 자원이 준비될 때 연결한다.
    Result<void> SetTint(LinearColor tint) noexcept;
    // 현재 표시 중인 크기·틴트를 포함한 브러시를 복사한다. 텍스처 소유권은 렌더 호스트에 있다.
    Brush GetBrush() const noexcept;
    LinearColor Tint() const noexcept;
    Result<void> SetBrush(const Brush& brush) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "imageSize") Vec2 imageSize_ {};
    ROCKS_PROPERTY(Name = "tint") LinearColor tint_ { 1, 1, 1, 1 };
    ROCKS_NATIVE() Brush brush_ {.mode = BrushMode::None };
};

} // namespace rocks::ui
