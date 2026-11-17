#include <RenderBase/RenderHandle.h>

namespace rocks::render {

RenderHandleTable::RenderHandleTable(Allocator& allocator) noexcept : entries_(allocator) {
}

Result<RenderHandle> RenderHandleTable::Reserve(RenderResourceKind kind) noexcept {
    platform::LockGuard lock(gate_);
    Entry entry;
    entry.kind = kind;
    auto added = entries_.TryAdd(entry);
    if (!added) {
        return Result<RenderHandle>::Failure(added.Error());
    }

    return Result<RenderHandle>::Success(RenderHandle{added.Value()});
}

Result<void> RenderHandleTable::Bind(RenderHandle handle, RenderResourceKind kind,
                                     SlotHandle device) noexcept {
    platform::LockGuard lock(gate_);
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (entry->kind != kind) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (entry->state == RenderResourceState::Retired) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (entry->state != RenderResourceState::Pending || device.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    entry->device = device;
    entry->state = RenderResourceState::Ready;
    return Result<void>::Success();
}

Result<void> RenderHandleTable::BindBuffer(RenderHandle handle,
                                           graphics::BufferHandle buffer) noexcept {
    return Bind(handle, RenderResourceKind::Buffer, buffer.slot);
}

Result<void> RenderHandleTable::BindTexture(RenderHandle handle,
                                            graphics::TextureHandle texture) noexcept {
    return Bind(handle, RenderResourceKind::Texture, texture.slot);
}

Result<void> RenderHandleTable::BindPipeline(RenderHandle handle,
                                             graphics::PipelineHandle pipeline) noexcept {
    return Bind(handle, RenderResourceKind::Pipeline, pipeline.slot);
}

Result<void> RenderHandleTable::MarkFailed(RenderHandle handle, ErrorCode error) noexcept {
    platform::LockGuard lock(gate_);
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (entry->state != RenderResourceState::Pending) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    entry->state = RenderResourceState::Failed;
    entry->error = error;
    return Result<void>::Success();
}

Result<SlotHandle> RenderHandleTable::Lookup(RenderHandle handle,
                                             RenderResourceKind kind) const noexcept {
    platform::LockGuard lock(gate_);
    const Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr) {
        return Result<SlotHandle>::Failure(ErrorCode::StaleHandle);
    }

    if (entry->kind != kind) {
        return Result<SlotHandle>::Failure(ErrorCode::TypeMismatch);
    }

    if (entry->state != RenderResourceState::Ready) {
        return Result<SlotHandle>::Failure(ErrorCode::NotFound);
    }

    return Result<SlotHandle>::Success(entry->device);
}

Result<graphics::BufferHandle> RenderHandleTable::Buffer(RenderHandle handle) const noexcept {
    auto found = Lookup(handle, RenderResourceKind::Buffer);
    if (!found) {
        return Result<graphics::BufferHandle>::Failure(found.Error());
    }

    return Result<graphics::BufferHandle>::Success(graphics::BufferHandle{found.Value()});
}

Result<graphics::TextureHandle> RenderHandleTable::Texture(RenderHandle handle) const noexcept {
    auto found = Lookup(handle, RenderResourceKind::Texture);
    if (!found) {
        return Result<graphics::TextureHandle>::Failure(found.Error());
    }

    return Result<graphics::TextureHandle>::Success(graphics::TextureHandle{found.Value()});
}

Result<graphics::PipelineHandle> RenderHandleTable::Pipeline(RenderHandle handle) const noexcept {
    auto found = Lookup(handle, RenderResourceKind::Pipeline);
    if (!found) {
        return Result<graphics::PipelineHandle>::Failure(found.Error());
    }

    return Result<graphics::PipelineHandle>::Success(graphics::PipelineHandle{found.Value()});
}

Result<RenderResourceState> RenderHandleTable::State(RenderHandle handle) const noexcept {
    platform::LockGuard lock(gate_);
    const Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr) {
        return Result<RenderResourceState>::Failure(ErrorCode::StaleHandle);
    }

    return Result<RenderResourceState>::Success(entry->state);
}

Result<RenderHandleTable::RetiredResource>
RenderHandleTable::BeginRetire(RenderHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr) {
        return Result<RetiredResource>::Failure(ErrorCode::StaleHandle);
    }

    if (entry->state == RenderResourceState::Retired) {
        return Result<RetiredResource>::Failure(ErrorCode::InvalidArgument);
    }

    RetiredResource retired;
    retired.kind = entry->kind;
    if (entry->state == RenderResourceState::Ready) {
        retired.device = entry->device;
    }

    entry->state = RenderResourceState::Retired;
    entry->device = {};
    return Result<RetiredResource>::Success(retired);
}

Result<void> RenderHandleTable::Remove(RenderHandle handle) noexcept {
    platform::LockGuard lock(gate_);
    const Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (entry->state != RenderResourceState::Retired) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return entries_.TryRemove(handle.slot);
}

usize RenderHandleTable::Count() const noexcept {
    platform::LockGuard lock(gate_);
    return entries_.Size();
}

} // namespace rocks::render
