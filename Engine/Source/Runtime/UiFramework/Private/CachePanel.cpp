#include <UiFramework/CachePanel.h>

namespace rocks::ui {

CachePanel::CachePanel(Allocator& allocator) noexcept : SizeBox(allocator), cache_(allocator) {
}

CacheRoot* CachePanel::LocalPaintCache() const noexcept {
    return &cache_;
}

} // namespace rocks::ui
