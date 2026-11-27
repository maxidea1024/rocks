#include <UiFramework/Docking/DockManager.h>

namespace rocks::ui {

namespace {

// 원래 위젯의 배치와 부모를 그대로 두고 그리기 명령만 미리보기 좌표로 옮긴다.
Result<void> PaintPreviewPart(const Element& element, Vec2 offset, PaintList& scratch,
                              PaintList& output) noexcept {
    scratch.Clear();
    auto painted = element.Paint(scratch, 1);
    if (!painted) {
        return painted;
    }

    for (auto box : scratch.Boxes()) {
        box.geometry.render.tx += offset.x;
        box.geometry.render.ty += offset.y;
        if (box.hasClip) {
            box.clip.min += offset;
            box.clip.max += offset;
        }

        auto added = output.TryAddBox(box);
        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

} // namespace

bool DockManager::GetFloatingPreview(FloatingTabPreview& preview) const noexcept {
    if (!dragging_ || !overDesktop_ || dragTab_ >= tabs_.Size()) {
        return false;
    }

    const Tab& tab = tabs_[dragTab_];
    const Node* node = nodes_.Find(tab.stack);
    if (!node) {
        return false;
    }

    const auto& transform = GetGeometry().render;
    const Vec2 grab = transform.Apply(press_) - transform.Apply(tab.headerBounds.min);
    const auto bounds = GetGeometry()
                            .MakeChild(node->bounds.Size(),
                                       {1, 0, 0, 1, node->bounds.min.x, node->bounds.min.y}, {}, {})
                            .WindowBounds();
    if (!bounds) {
        return false;
    }

    preview = {dragPosition_ - grab, bounds.Value().Size(), gesture_};
    return preview.size.x >= 1 && preview.size.y >= 1;
}

Result<void> DockManager::PaintFloatingPreview(PaintList& list) const noexcept {
    FloatingTabPreview preview;
    if (!GetFloatingPreview(preview)) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    const auto& tab = tabs_[dragTab_];
    const auto header = tab.header->GetGeometry().WindowBounds();
    const auto body = tab.content->GetGeometry().WindowBounds();
    if (!header || !body) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    PaintList scratch(allocator_);
    scratch.SetTheme(&list.Theme(), list.ThemeRevision());
    scratch.SetVectorImageResolver(list.GetVectorImageResolver());
    scratch.SetFrameTime(list.GetFrameTime());
    auto result = PaintPreviewPart(
        *tab.content, Vec2{0, header.Value().Size().y} - body.Value().min, scratch, list);
    if (result) {
        result = PaintPreviewPart(*tab.header, Vec2{} - header.Value().min, scratch, list);
    }

    if (!result) {
        return result;
    }

    // 내용과 탭 제목을 알아볼 수 있게 옅은 파란색만 덧입힌다.
    return list.TryAddBox({Geometry::Root(preview.size, {}),
                           {0.12f, 0.36f, 0.72f, 0.14f},
                           NumericLimits<int32>::Max()});
}

} // namespace rocks::ui
