#include <UiFramework/Effects/RetainerBox.h>
#include <UiCore/PaintList.h>

namespace rocks::ui {

Result<void> RetainerBox::Construct(Args&& args) noexcept {
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

WidgetSurface* RetainerBox::GetWidgetSurface() noexcept {
    return this;
}

WidgetSurfaceKind RetainerBox::SurfaceKind() const noexcept {
    return WidgetSurfaceKind::RetainedContent;
}

bool RetainerBox::WantsManagedSurface() const noexcept {
    return true;
}

SlotHandle RetainerBox::ManagedSurface() const noexcept {
    return retained_;
}

void RetainerBox::SetManagedSurface(SlotHandle texture) noexcept {
    SetRetainedTexture(texture);
}

RetainerBox::RetainerBox(Allocator& allocator) noexcept
    : SizeBox(allocator), contentCache_(allocator) {
}

Result<void> RetainerBox::TryAddChild(const SharedPtr<Element>& child) noexcept {
    auto added = SizeBox::TryAddChild(child);
    if (added) {
        contentCache_.Reset();
        RequestRender();
    }

    return added;
}

bool RetainerBox::RemoveChild(Element& child) noexcept {
    if (!SizeBox::RemoveChild(child)) {
        return false;
    }

    contentCache_.Reset();
    RequestRender();
    return true;
}

Result<void> RetainerBox::SetPhase(uint32 phase, uint32 count) noexcept {
    if (count == 0 || phase >= count) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    phase_ = phase;
    phaseCount_ = count;
    return Result<void>::Success();
}

bool RetainerBox::NeedsCapture(uint64 frame) const noexcept {
    return requested_ || frame % phaseCount_ == phase_;
}

Result<void> RetainerBox::SetEffectTint(LinearColor tint) noexcept {
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (effectTint_ != tint) {
        effectTint_ = tint;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void RetainerBox::RequestRender() noexcept {
    requested_ = true;
}

void RetainerBox::SetRetainedTexture(SlotHandle texture) noexcept {
    requested_ = texture.IsNull();
    if (retained_ != texture) {
        retained_ = texture;
        InvalidatePaintPlan();
    }
}

Span<const SharedPtr<Element>> RetainerBox::PaintChildren() const noexcept {
    return retained_.IsNull() ? ActiveChildren() : Span<const SharedPtr<Element>>{};
}

Result<void> RetainerBox::RecordContent(PaintList& output) const noexcept {
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

Result<void> RetainerBox::OnPaint(PaintList& list, int32 layer) const noexcept {
    if (retained_.IsNull()) {
        return Result<void>::Success();
    }

    PaintBox box{GetGeometry(), effectTint_, layer};
    box.resource = retained_;
    return list.TryAddBox(box);
}

} // namespace rocks::ui
