#include "DemoCapture.h"

namespace rocks::ui {

Result<void> CaptureTarget(UiRenderer& renderer, UiTextureId target, Array<uint8>& rgba) noexcept {
    auto capture = renderer.RequestCapture(target);
    if (!capture) {
        return Result<void>::Failure(capture.Error());
    }

    if (auto waited = renderer.WaitCapture(capture.Value()); !waited) {
        return waited;
    }

    return renderer.TakeCapture(capture.Value(), rgba);
}

Result<void> DrawAndCapture(UiRenderer& renderer, UiDrawBuffer& buffer, UiTextureId target,
                            uint32 width, uint32 height, LinearColor clear, const DrawBatch& batch,
                            Array<uint8>& rgba) noexcept {
    auto added = buffer.AddOffscreen(target, width, height, clear, batch);
    if (added) {
        added = renderer.DrawWindows(buffer);
    }

    buffer.Clear();
    if (!added) {
        return added;
    }

    return CaptureTarget(renderer, target, rgba);
}

} // namespace rocks::ui
