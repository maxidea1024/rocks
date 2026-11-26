#include "MakeWidgetElement.h"
#include <UiWidget/PostBufferUpdateWidget.h>
#include <UiFramework/Effects/PostBufferUpdate.h>

namespace rocks::ui {

namespace {

Result<Array<PostBufferUpdateInfo>> ToNativeBuffers(Allocator& allocator,
                                                    Span<const PostBufferEntry> entries) noexcept {
    Array<PostBufferUpdateInfo> result(allocator);
    for (const auto& entry : entries) {
        if (entry.channel < 0 || entry.channel >= static_cast<int32>(PostBufferChannelCount) ||
            !math::IsFinite(entry.blurStrength) || entry.blurStrength < 0) {
            return Result<Array<PostBufferUpdateInfo>>::Failure(ErrorCode::InvalidArgument);
        }

        auto added = result.TryPushBack({{}, entry.blurStrength, entry.channel});
        if (!added) {
            return Result<Array<PostBufferUpdateInfo>>::Failure(added.Error());
        }
    }

    return Result<Array<PostBufferUpdateInfo>>::Success(Move(result));
}
} // namespace

Result<void>
PostBufferUpdateWidget::SetBufferUpdates(Span<const PostBufferEntry> entries) noexcept {
    auto native = ToNativeBuffers(updates_.GetAllocator(), entries);
    if (!native) {
        return Result<void>::Failure(native.Error());
    }

    Array<PostBufferEntry> next(updates_.GetAllocator());
    auto copied = next.TryAppend(entries);
    if (copied) {
        if (auto live = CachedElement()) {
            copied = static_cast<PostBufferUpdate&>(*live).SetUpdates(native.Value().View());
        }
    }

    if (copied) {
        bufferUpdates_ = Move(next);
        updates_.Clear();
        destination_ = {};
    }

    return copied;
}

SlotHandle PostBufferUpdateWidget::GetPostBuffer(uint32 channel) const noexcept {
    if (auto live = CachedElement()) {
        return static_cast<PostBufferUpdate&>(*live).GetPostBuffer(channel);
    }

    return {};
}

PostBufferUpdateWidget::PostBufferUpdateWidget(Allocator& allocator) noexcept
    : bufferUpdates_(allocator), updates_(allocator) {
}

Result<void> PostBufferUpdateWidget::SetUpdates(Span<const PostBufferUpdateInfo> updates) noexcept {
    for (const auto& item : updates) {
        if (!math::IsFinite(item.blurStrength) || item.blurStrength < 0 || item.channel < -1 ||
            item.channel >= static_cast<int32>(PostBufferChannelCount) ||
            (item.channel >= 0 && !item.destination.IsNull())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    Array<PostBufferUpdateInfo> next(updates_.GetAllocator());
    auto status = next.TryAppend(updates);
    if (status) {
        if (auto live = CachedElement()) {
            status = static_cast<PostBufferUpdate&>(*live).SetUpdates(updates);
        }
    }

    if (status) {
        bufferUpdates_.Clear();
        updates_ = Move(next);
    }

    return status;
}

Result<SharedPtr<Element>> PostBufferUpdateWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto made = MakeWidgetElement<PostBufferUpdate>(objects.ObjectAllocator());
    if (made) {
        auto& native = static_cast<PostBufferUpdate&>(*made.Value());
        native.SetDestination(destination_);
        auto configured = ToNativeBuffers(updates_.GetAllocator(), bufferUpdates_.View());
        if (!configured) {
            return Result<SharedPtr<Element>>::Failure(configured.Error());
        }

        auto updated = native.SetUpdates(bufferUpdates_.IsEmpty() ? updates_.View()
                                                                  : configured.Value().View());
        if (!updated) {
            return Result<SharedPtr<Element>>::Failure(updated.Error());
        }
    }

    return made;
}

SlotHandle PostBufferUpdateWidget::CapturedBuffer() const noexcept {
    if (!bufferUpdates_.IsEmpty()) {
        return GetPostBuffer(static_cast<uint32>(bufferUpdates_[0].channel));
    }

    if (!updates_.IsEmpty() && updates_[0].channel >= 0) {
        return GetPostBuffer(static_cast<uint32>(updates_[0].channel));
    }

    if (!updates_.IsEmpty() && !updates_[0].destination.IsNull()) {
        return updates_[0].destination;
    }

    if (updates_.IsEmpty() && !destination_.IsNull()) {
        return destination_;
    }

    if (auto live = CachedElement()) {
        return static_cast<PostBufferUpdate&>(*live).ManagedSurface();
    }

    return {};
}

void PostBufferUpdateWidget::SetDestination(SlotHandle destination) noexcept {
    bufferUpdates_.Clear();
    updates_.Clear();
    destination_ = destination;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
}

void PostBufferUpdateWidget::ApplyProperties(Element& element) noexcept {
    auto& native = static_cast<PostBufferUpdate&>(element);
    if (updates_.IsEmpty() && bufferUpdates_.IsEmpty()) {
        native.SetDestination(destination_);
    }
}

} // namespace rocks::ui
