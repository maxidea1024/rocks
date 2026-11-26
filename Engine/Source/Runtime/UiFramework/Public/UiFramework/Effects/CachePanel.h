#pragma once

#include <UiCore/CacheRoot.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

// 창 전체 캐시를 사용하지 않는 화면에서 내용 하나의 그리기 기록을 보존한다.
class ROCKS_UIFRAMEWORK_API CachePanel final : public SizeBox {
public:
    explicit CachePanel(Allocator& allocator) noexcept;

protected:
    CacheRoot* LocalPaintCache() const noexcept override;

private:
    mutable CacheRoot cache_;
};

} // namespace rocks::ui
