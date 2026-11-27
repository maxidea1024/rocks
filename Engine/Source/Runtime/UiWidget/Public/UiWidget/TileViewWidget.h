#pragma once

#include <UiWidget/ListViewWidget.h>

namespace rocks::ui {

// 목록 데이터를 같은 너비의 타일로 배치한다. 화면 너비가 바뀌면 열 수를 다시 계산한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API TileViewWidget final : public ListViewWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit TileViewWidget(Allocator& allocator) noexcept;
    Result<void> SetTileWidth(float width) noexcept;

protected:
    Result<SharedPtr<Element>>
    CreateView(Allocator& allocator, Application& app,
               const SharedRef<TableRowSource>& source) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "tileWidth") float tileWidth_ = 96;
};

} // namespace rocks::ui
