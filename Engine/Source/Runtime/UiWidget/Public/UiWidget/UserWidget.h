#pragma once

#include <UiWidget/WidgetTree.h>
#include <UiWidget/UserWidgetAnimation.h>
#include <UiFramework/AnchorCanvas.h>

namespace rocks::ui {
class WidgetModelConnections;

// 위젯 트리를 소유하는 게임 화면이다. 부착과 해제에 따라 OnConstruct와 OnDestruct가 호출된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API UserWidget : public Widget {
    ROCKS_GENERATED_BODY()

public:
    explicit UserWidget(Allocator& allocator) noexcept;

    Result<void> Initialize(ObjectRegistry& objects) noexcept;
    ObjectHandle Tree() const noexcept;
    ObjectHandle ViewModel() const noexcept;
    // 에셋에서 복제한 애니메이션은 화면 안에서 이름 또는 안정 ID로 조회한다.
    Result<ObjectHandle> FindAnimation(ObjectRegistry& objects, Guid id) const noexcept;
    Result<ObjectHandle> FindAnimation(ObjectRegistry& objects, StringView name) const noexcept;
    Result<SharedPtr<UserWidgetAnimation>> PlayAnimation(ObjectRegistry& objects, Application& app,
        Guid animation, WidgetAnimationPlayback options = {}) noexcept;

    // 현재 루트 아래의 NamedSlotWidget 이름을 정렬해 반환한다. 중첩 UserWidget은 자기 슬롯을 별도로
    // 공개한다.
    Result<Array<String>> GetSlotNames(ObjectRegistry& objects) const noexcept;
    // 없는 이름은 NotFound, 비어 있는 슬롯은 성공과 빈 핸들이다. content는 이 화면의 WidgetTree에서
    // 생성한다.
    Result<ObjectHandle> GetContentForSlot(ObjectRegistry& objects, StringView name) const noexcept;
    // 이 화면의 어느 이름 있는 슬롯이 content를 소유하는지 역으로 찾는다.
    Result<String> FindSlotForContent(ObjectRegistry& objects, ObjectHandle content) const noexcept;
    Result<bool> ContainsContent(ObjectRegistry& objects, ObjectHandle content) const noexcept;
    // 빈 핸들이면 제거한다. 교체에 실패하면 이전 내용과 슬롯의 네이티브 트리가 유지된다.
    Result<void> SetContentForSlot(ObjectRegistry& objects, StringView name,
                                   ObjectHandle content) noexcept;
    // 화면에 연결된 Application이 재생 중인 애니메이션의 시간을 진행한다.
    // QueuePlayAnimation은 다음 프레임 시작까지 초기값과 시작 콜백 적용을 미룬다.
    Result<SharedPtr<UserWidgetAnimation>> PlayAnimation(ObjectRegistry& objects, Application& app,
        const WidgetAnimation& clip, WidgetAnimationPlayback options = {}) noexcept;
    Result<SharedPtr<UserWidgetAnimation>> QueuePlayAnimation(ObjectRegistry& objects, Application& app,
        const WidgetAnimation& clip, WidgetAnimationPlayback options = {}) noexcept;
    void StopAllAnimations() noexcept;
    bool IsAnyAnimationPlaying() const noexcept;
    void ReleaseElements() noexcept override;
    // 호스트가 화면 또는 분할 화면 플레이어 영역에 배치한 캔버스에 부착한다.
    // 기본 배치는 영역 전체를 채우며 z 순서는 같은 영역의 화면 사이에서 비교한다.
    Result<void> AddToViewport(ObjectRegistry& objects,const SharedPtr<AnchorCanvas>& layer,float zOrder=0) noexcept;
    Result<void> SetViewportLayout(const CanvasSlot& placement) noexcept;
    CanvasSlot GetViewportLayout() const noexcept;
    Result<void> SetPositionInViewport(Vec2 position) noexcept;
    Result<void> SetDesiredSizeInViewport(Vec2 size) noexcept;
    Result<void> SetAnchorsInViewport(Vec2 minimum,Vec2 maximum) noexcept;
    Result<void> SetAlignmentInViewport(Vec2 alignment) noexcept;
    bool IsInViewport() const noexcept;
    Result<void> RemoveFromParent(ObjectRegistry& objects) noexcept override;
    void SetFocusable(bool focusable) noexcept;

    // 초기화는 게임 객체당 한 번, PreConstruct와 Construct는 화면을 다시 만들 때마다 호출한다.
    virtual void OnInitialized() noexcept;
    virtual void OnPreConstruct() noexcept;
    virtual void OnConstruct() noexcept;

    // true 또는 handled를 반환하면 해당 입력의 상위 위젯 전파를 끝낸다.
    virtual bool OnPreviewKey(uint32 user, const input::InputEvent& event) noexcept;
    virtual bool OnKey(uint32 user, const input::InputEvent& event) noexcept;
    virtual PointerReply OnPreviewPointer(const PointerEvent& event) noexcept;
    virtual PointerReply OnPointer(const PointerEvent& event) noexcept;
    virtual void OnFocusReceived(uint32 user) noexcept;
    virtual void OnFocusLost(uint32 user) noexcept;
    virtual void OnPointerCaptureLost(PointerSource source,uint32 user,uint32 pointer) noexcept;
    virtual Result<void> OnPaintOverlay(PaintList& list,const Geometry& geometry,int32 layer) const noexcept;
    bool IsFocusable() const noexcept;
    virtual void OnDestruct() noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept final;
    void OnRebuilt() noexcept override;

private:
    friend class WidgetTemplate;
    ROCKS_PROPERTY(ReadOnly, Name = "animations") ObjectRefArray<WidgetAnimation> animations_;
    ROCKS_PROPERTY(Transient, ReadOnly, Name = "viewModel") ObjectRef<Object> viewModel_;
    ROCKS_NATIVE() SharedPtr<WidgetModelConnections> modelConnections_;
    Result<SharedPtr<UserWidgetAnimation>> StartAnimation(ObjectRegistry& objects, Application& app,
        const WidgetAnimation& clip, WidgetAnimationPlayback&& options, bool queued) noexcept;
    ROCKS_NATIVE() SharedPtr<UserWidgetAnimationSet> animationPlayers_;
    ROCKS_PROPERTY(ReadOnly, Name = "focusable") bool focusable_ = false;
    ROCKS_NATIVE() bool initialized_ = false;
    ROCKS_NATIVE() WeakPtr<AnchorCanvas> viewportLayer_;
    ROCKS_NATIVE() CanvasSlot viewportSlot_ {.anchorMax={1,1},.offset={},.alignment={}};
    ROCKS_PROPERTY(ReadOnly, Name = "widgetTree") ObjectRef<WidgetTree> tree_;
};

} // namespace rocks::ui
