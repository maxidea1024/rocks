#include <UiFramework/TileView.h>

namespace rocks::ui {

Result<void> TileView::SetTileWidth(float width) noexcept {
    if (!math::IsFinite(width) || width <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (tileWidth_ != width) {
        tileWidth_ = width;
        InvalidateArrangement();
    }
    return Result<void>::Success();
}

uint32 TileView::Columns(float width) const noexcept {
    return static_cast<uint32>(math::Clamp(static_cast<double>(width) / tileWidth_, 1.0,
                                           static_cast<double>(NumericLimits<uint32>::Max())));
}

} // namespace rocks::ui
