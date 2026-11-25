#include <UiFramework/PostBufferUpdate.h>
#include <UiCore/PaintList.h>

namespace rocks::ui {

Result<void> PostBufferUpdate::Construct(Args&& args) noexcept {
    destination_.Assign(Move(args.destination));
    return SetUpdates(args.updates);
}

WidgetSurface* PostBufferUpdate::GetWidgetSurface() noexcept {
    return this;
}

uint32 PostBufferUpdate::RequestedPostBuffers() const noexcept {
    uint32 mask = 0;
    for (const auto& item : updates_) {
        if (item.channel >= 0) {
            mask |= 1u << static_cast<uint32>(item.channel);
        }
    }

    return mask;
}

SlotHandle PostBufferUpdate::GetPostBuffer(uint32 channel) const noexcept {
    return channel < PostBufferChannelCount ? channels_[channel] : SlotHandle{};
}

void PostBufferUpdate::SetPostBuffer(uint32 channel, SlotHandle texture) noexcept {
    if (channel < PostBufferChannelCount && channels_[channel] != texture) {
        channels_[channel] = texture;
        InvalidatePaint();
    }
}

WidgetSurfaceKind PostBufferUpdate::SurfaceKind() const noexcept {
    return WidgetSurfaceKind::PostBuffer;
}

bool PostBufferUpdate::WantsManagedSurface() const noexcept {
    if (updates_.IsEmpty()) {
        return destination_.Get().IsNull();
    }

    for (const auto& item : updates_) {
        if (item.destination.IsNull() && item.channel < 0) {
            return true;
        }
    }

    return false;
}

SlotHandle PostBufferUpdate::ManagedSurface() const noexcept {
    return managed_;
}

void PostBufferUpdate::SetManagedSurface(SlotHandle texture) noexcept {
    if (managed_ != texture) {
        managed_ = texture;
        InvalidatePaint();
    }
}

PostBufferUpdate::PostBufferUpdate(Allocator& allocator) noexcept
    : Element(allocator), updates_(allocator), destination_(*this, {}) {
    SetVisibility(Visibility::HitTestInvisible);
}

void PostBufferUpdate::SetDestination(SlotHandle destination) noexcept {
    if (!updates_.IsEmpty()) {
        updates_.Clear();
        InvalidatePaint();
    }

    destination_.Set(destination);
}

Result<void> PostBufferUpdate::SetUpdates(Span<const PostBufferUpdateInfo> updates) noexcept {
    for (const auto& item : updates) {
        if (!math::IsFinite(item.blurStrength) || item.blurStrength < 0 || item.channel < -1 ||
            item.channel >= static_cast<int32>(PostBufferChannelCount) ||
            (item.channel >= 0 && !item.destination.IsNull())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    Array<PostBufferUpdateInfo> next(updates_.GetAllocator());
    auto copied = next.TryAppend(updates);
    if (copied) {
        updates_ = Move(next);
        InvalidatePaint();
    }

    return copied;
}

Result<void> PostBufferUpdate::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> PostBufferUpdate::OnPaint(PaintList& list, int32 layer) const noexcept {
    if (!updates_.IsEmpty()) {
        for (const auto& item : updates_) {
            PaintBox box{GetGeometry(), {}, layer};
            box.resource = item.channel >= 0
                               ? channels_[item.channel]
                               : (item.destination.IsNull() ? managed_ : item.destination);
            if (box.resource.IsNull()) {
                continue;
            }

            box.blurRadius = item.blurStrength;
            box.operation =
                item.blurStrength > 0 ? PaintOperation::BlurToBuffer : PaintOperation::CopyBackdrop;
            auto added = list.TryAddBox(box);
            if (!added) {
                return added;
            }
        }

        return Result<void>::Success();
    }

    const SlotHandle texture = destination_.Get().IsNull() ? managed_ : destination_.Get();
    if (texture.IsNull()) {
        return Result<void>::Success();
    }

    PaintBox box{GetGeometry(), {}, layer};
    box.resource = texture;
    box.operation = PaintOperation::CopyBackdrop;
    return list.TryAddBox(box);
}

} // namespace rocks::ui
