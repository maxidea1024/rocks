#pragma once
#include <UiWidget/PanelWidget.h>
#include <UiFramework/Application/Application.h>

namespace rocks::ui {

// 페이지를 수평으로 넘기는 게임 화면 패널이다. 자식 순서가 페이지 순서이며 양 끝은 서로 이어진다.
// 자동 넘김은 연결한 Application이 화면을 갱신하는 동안만 진행한다. 수동 이동은 화면 생성 전에도
// 가능하다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API CarouselWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit CarouselWidget(Allocator& allocator) noexcept;
    uint32 ActiveIndex() const noexcept;
    Result<void> SetActiveIndex(uint32 index) noexcept;
    Result<void> SetActiveWidget(ObjectRegistry& objects, ObjectHandle page) noexcept;
    ObjectHandle WidgetAt(ObjectRegistry& objects, usize index) const noexcept;
    Result<void> NextPage() noexcept;
    Result<void> PreviousPage() noexcept;
    // 초당 페이지 폭으로 이동 속도를 지정한다. 0이면 즉시 전환한다.
    Result<void> SetMoveSpeed(float pagesPerSecond) noexcept;
    float MoveSpeed() const noexcept;
    // 캐시를 끄면 현재 페이지와 전환에 사용 중인 페이지의 화면 트리만 보관한다.
    Result<void> SetCacheChildren(bool cache) noexcept;
    bool CacheChildren() const noexcept;
    Result<void> BeginAutoScrolling(Application& app, float intervalSeconds = 10) noexcept;
    void EndAutoScrolling() noexcept;
    MulticastDelegate<void()>& OnPagesChanged() noexcept;
    Result<ObjectHandle> AddChild(ObjectRegistry& objects, ObjectHandle page) noexcept override;
    Result<void> RemoveChild(ObjectRegistry& objects, ObjectHandle page) noexcept override;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;

private:
    void AdvanceAutoPage() noexcept;
    Result<void> ChangePage(uint32 index, bool forward) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "activeIndex") uint32 activeIndex_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "moveSpeed") float moveSpeed_ = 5;
    ROCKS_PROPERTY(ReadOnly, Name = "cacheChildren") bool cacheChildren_ = true;
    ROCKS_NATIVE() MulticastDelegate<void()> pagesChanged_;
    ROCKS_NATIVE() SharedPtr<UiTimer> autoTimer_;
    ROCKS_NATIVE() ObjectRegistry* registry_ = nullptr;
};

} // namespace rocks::ui
