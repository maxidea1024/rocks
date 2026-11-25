#include <UiCore/WidgetSurface.h>

namespace rocks::ui {

SharedPtr<WidgetSurfaceScene> WidgetSurface::SceneSource() const noexcept {
    return {};
}

uint32 WidgetSurface::RequestedPostBuffers() const noexcept {
    return 0;
}
SlotHandle WidgetSurface::GetPostBuffer(uint32) const noexcept {
    return {};
}
void WidgetSurface::SetPostBuffer(uint32, SlotHandle) noexcept {
}

bool WidgetSurface::NeedsCapture(uint64) const noexcept {
    return false;
}

Result<void> WidgetSurface::RecordContent(PaintList&) const noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::ui
