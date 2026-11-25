#include <UiWidget/UserWidget.h>

namespace rocks::ui {
Result<void> UserWidget::AddToViewport(ObjectRegistry& objects,const SharedPtr<AnchorCanvas>& layer,float zOrder) noexcept {
    if (!layer || !math::IsFinite(zOrder)) {return Result<void>::Failure(ErrorCode::InvalidArgument);}
    if (!Slot().IsNull() || IsInViewport()) {return Result<void>::Failure(ErrorCode::AlreadyExists);}
    auto element=TakeElement(objects);
    if (!element) {return Result<void>::Failure(element.Error());}
    auto layout=viewportSlot_;
    layout.zOrder=zOrder;
    auto added=layer->TryAddSlot(element.Value(),layout);
    if (added) {viewportLayer_=WeakPtr<AnchorCanvas>(layer); viewportSlot_=layout;}
    return added;
}
bool UserWidget::IsInViewport() const noexcept {
    auto layer=viewportLayer_.Lock();
    auto live=CachedElement();
    return layer && live && live->Parent()==layer.Get();
}
Result<void> UserWidget::RemoveFromParent(ObjectRegistry& objects) noexcept {
    auto layer=viewportLayer_.Lock();
    auto live=CachedElement();
    if (layer && live && live->Parent()==layer.Get()) {
        if (!layer->RemoveChild(*live)) {return Result<void>::Failure(ErrorCode::WrongContext);}
        viewportLayer_={};
        ReleaseElements();
        return Result<void>::Success();
    }
    return Widget::RemoveFromParent(objects);
}
Result<void> UserWidget::SetViewportLayout(const CanvasSlot& layout) noexcept {
    if (!layout.IsValid()) {return Result<void>::Failure(ErrorCode::InvalidArgument);}
    auto layer=viewportLayer_.Lock();
    auto live=CachedElement();
    if (layer && live && live->Parent()==layer.Get()) {
        auto result=layer->SetSlot(*live,layout);
        if (!result) {return result;}
    }
    viewportSlot_=layout;
    return Result<void>::Success();
}
CanvasSlot UserWidget::GetViewportLayout() const noexcept {return viewportSlot_;}
Result<void> UserWidget::SetPositionInViewport(Vec2 value) noexcept {
    CanvasSlot layout=viewportSlot_;
    layout.offset.x=value.x;layout.offset.y=value.y;
    return SetViewportLayout(layout);
}
Result<void> UserWidget::SetDesiredSizeInViewport(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x<0 || value.y<0) {return Result<void>::Failure(ErrorCode::InvalidArgument);}
    CanvasSlot layout=viewportSlot_;
    layout.anchorMax=layout.anchorMin;
    layout.offset.z=value.x;layout.offset.w=value.y;layout.autoSize=false;
    return SetViewportLayout(layout);
}
Result<void> UserWidget::SetAnchorsInViewport(Vec2 minimum,Vec2 maximum) noexcept {
    auto layout=viewportSlot_;layout.anchorMin=minimum;layout.anchorMax=maximum;return SetViewportLayout(layout);
}
Result<void> UserWidget::SetAlignmentInViewport(Vec2 align) noexcept {
    auto layout=viewportSlot_;layout.alignment=align;return SetViewportLayout(layout);
}
} // namespace rocks::ui
