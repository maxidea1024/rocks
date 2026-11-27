#pragma once

#include <UiFramework/Lists/ListView.h>

namespace rocks::ui {

class ROCKS_UIFRAMEWORK_API TileView final : public ListView {
    ROCKS_UI_ELEMENT(TileView, ListView)

public:
    using ListView::ListView;
    Result<void> SetTileWidth(float width) noexcept;

protected:
    uint32 Columns(float width) const noexcept override;

private:
    float tileWidth_ = 96;
};

} // namespace rocks::ui
