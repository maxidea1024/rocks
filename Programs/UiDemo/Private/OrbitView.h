#pragma once

#include <UiCore/Element.h>
#include <Graphics/Device.h>

namespace rocks::ui {

class OrbitView final : public Element {
public:
    OrbitView(Allocator& allocator, SlotHandle image) noexcept;
    static Result<graphics::TextureHandle> CreateMap(Allocator& allocator,
                                                     graphics::Device& device) noexcept;
    void SetProgress(float progress) noexcept;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    SlotHandle image_;
    float progress_ = 0;
};

} // namespace rocks::ui
