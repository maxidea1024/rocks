#include <UiFramework/Effects/OffscreenLayer.h>
#include <UiCore/PaintList.h>

namespace rocks::ui {

Result<void> OffscreenLayer::Construct(Args&& args) noexcept {
    auto phase = SetPhase(args.phase, args.phaseCount);
    if (!phase) {
        return phase;
    }

    auto tinted = SetEffectTint(args.effectTint);
    if (!tinted) {
        return tinted;
    }

    return args.content.AttachTo(*this);
}

WidgetSurface* OffscreenLayer::GetWidgetSurface() noexcept {
    return this;
}

WidgetSurfaceKind OffscreenLayer::SurfaceKind() const noexcept {
    return WidgetSurfaceKind::RetainedContent;
}

bool OffscreenLayer::WantsManagedSurface() const noexcept {
    return true;
}

SlotHandle OffscreenLayer::ManagedSurface() const noexcept {
    return retained_;
}

void OffscreenLayer::SetManagedSurface(SlotHandle texture) noexcept {
    SetRetainedTexture(texture);
}

OffscreenLayer::OffscreenLayer(Allocator& allocator) noexcept
    : SizeBox(allocator), contentCache_(allocator) {
}

Result<void> OffscreenLayer::TryAddChild(const SharedPtr<Element>& child) noexcept {
    auto added = SizeBox::TryAddChild(child);
    if (added) {
        contentCache_.Reset();
        RequestRender();
    }

    return added;
}

bool OffscreenLayer::RemoveChild(Element& child) noexcept {
    if (!SizeBox::RemoveChild(child)) {
        return false;
    }

    contentCache_.Reset();
    RequestRender();
    return true;
}

Result<void> OffscreenLayer::SetPhase(uint32 phase, uint32 count) noexcept {
    if (count == 0 || phase >= count) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    phase_ = phase;
    phaseCount_ = count;
    return Result<void>::Success();
}

bool OffscreenLayer::NeedsCapture(uint64 frame) const noexcept {
    return requested_ || frame % phaseCount_ == phase_;
}

Result<void> OffscreenLayer::SetEffectTint(LinearColor tint) noexcept {
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (effectTint_ != tint) {
        effectTint_ = tint;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void OffscreenLayer::RequestRender() noexcept {
    requested_ = true;
}

void OffscreenLayer::SetRetainedTexture(SlotHandle texture) noexcept {
    requested_ = texture.IsNull();
    if (retained_ != texture) {
        retained_ = texture;
        InvalidatePaintPlan();
    }
}

Span<const SharedPtr<Element>> OffscreenLayer::PaintChildren() const noexcept {
    return retained_.IsNull() ? ActiveChildren() : Span<const SharedPtr<Element>>{};
}

Span<const SharedPtr<Element>> OffscreenLayer::HitTestChildren() const noexcept {
    return ActiveChildren();
}

Result<void> OffscreenLayer::RecordContent(PaintList& output) const noexcept {
    const auto* child = ChildAt(0);
    if (!child) {
        return Result<void>::Success();
    }

    AffineTransform2D inverse;
    if (!GetGeometry().render.TryInverse(inverse)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto painted = contentCache_.Update(*child, 0);
    if (!painted) {
        return painted;
    }

    for (auto box : contentCache_.List().Boxes()) {
        box.geometry.render = inverse.Compose(box.geometry.render);
        box.geometry.layout = inverse.Compose(box.geometry.layout);
        if (box.hasClip && !box.emptyClip) {
            if ((inverse.m01 != 0 || inverse.m10 != 0) && (inverse.m00 != 0 || inverse.m11 != 0)) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            Geometry clip = Geometry::Root(box.clip.max - box.clip.min, {});
            clip.render.tx = box.clip.min.x;
            clip.render.ty = box.clip.min.y;
            clip.render = inverse.Compose(clip.render);
            auto bounds = clip.WindowBounds();
            if (!bounds) {
                return Result<void>::Failure(bounds.Error());
            }

            box.clip = bounds.Value();
        }

        auto result = output.TryAppendRecorded({&box, 1});
        if (!result) {
            return result;
        }
    }

    return Result<void>::Success();
}

Result<void> OffscreenLayer::OnPaint(PaintList& list, int32 layer) const noexcept {
    if (retained_.IsNull()) {
        return Result<void>::Success();
    }

    PaintBox box{GetGeometry(), effectTint_, layer};
    box.resource = retained_;
    return list.TryAddBox(box);
}

} // namespace rocks::ui
