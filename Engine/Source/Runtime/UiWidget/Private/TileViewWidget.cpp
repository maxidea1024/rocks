#include "MakeWidgetElement.h"
#include <UiWidget/TileViewWidget.h>
#include <UiFramework/TileView.h>

namespace rocks::ui {

TileViewWidget::TileViewWidget(Allocator& allocator) noexcept : ListViewWidget(allocator) {
}

Result<SharedPtr<Element>>
TileViewWidget::CreateView(Allocator& allocator, Application& app,
                           const SharedRef<TableRowSource>& source) noexcept {
    return MakeWidgetElement<TileView>(allocator, app, source);
}

Result<void> TileViewWidget::SetTileWidth(float width) noexcept {
    if (!math::IsFinite(width) || width <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    tileWidth_ = width;
    if (auto live = CachedElement()) {
        return static_cast<TileView&>(*live).SetTileWidth(width);
    }

    return Result<void>::Success();
}

void TileViewWidget::ApplyProperties(Element& element) noexcept {
    ListViewWidget::ApplyProperties(element);
    (void)static_cast<TileView&>(element).SetTileWidth(tileWidth_);
}

} // namespace rocks::ui
