#pragma once
#include <UiCore/ElementAttribute.h>
#include <UiFramework/Lists/CarouselPage.h>
#include <UiFramework/Layout/Switcher.h>

namespace rocks::ui {

// 전환 동안에는 이전 페이지도 함께 그리고, 보이는 두 쪽 모두 포인터 hit 대상이다(그리기 자식 —
// Docs/Design/UiToolkit 20.4절). 키 입력은 현재 페이지로 간다. 완료되면 시간 속성의 등록을 해제해
// 정지 화면에서 측정·배치·그리기 기록을 재사용한다.
class ROCKS_UIFRAMEWORK_API Carousel final : public Switcher {
public:
    explicit Carousel(Allocator& allocator) noexcept;
    Result<void> AddPage(const SharedPtr<CarouselPage>& page) noexcept;
    Result<void> Show(usize index, bool forward, float speed) noexcept;
    Result<void> SetCacheChildren(bool cache) noexcept;
    bool RemoveChild(Element& page) noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;

protected:
    Span<const SharedPtr<Element>> PaintChildren() const noexcept override;

private:
    float Progress() const noexcept;
    void Finish() noexcept;
    SharedPtr<Element> sliding_[2]{};
    ElementAttribute<float, InvalidateReason::Arrangement> progress_;
    double start_ = 0;
    float speed_ = 0;
    float direction_ = 1;
    bool cache_ = true;
};

} // namespace rocks::ui
