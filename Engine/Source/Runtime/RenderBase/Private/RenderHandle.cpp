#include <RenderBase/RenderHandle.h>

#include <Platform/Fatal.h>

namespace rocks::render {

RenderHandleTable::RenderHandleTable(Allocator& allocator) noexcept
    : entries_(allocator), releasedRetires_(allocator) {
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
    entry->state = RenderResourceState::Retired;
    if (entry->leases != 0) {
        // 승인된 기록이 아직 쓴다. 장치 handle은 마지막 lease가 풀릴 때 넘긴다.
        entry->retireDeferred = true;
        retired.deferred = true;
        return Result<RetiredResource>::Success(retired);
    }

    retired.device = entry->device;
    entry->device = {};
    return Result<RetiredResource>::Success(retired);
}

Result<RenderResourceLease> RenderHandleTable::AcquireLease(RenderHandle handle,
                                                            RenderResourceKind kind) noexcept {
    platform::LockGuard lock(gate_);
    Entry* entry = entries_.Find(handle.slot);
    if (entry == nullptr || entry->state == RenderResourceState::Retired) {
        return Result<RenderResourceLease>::Failure(ErrorCode::StaleHandle);
    }

    if (entry->kind != kind) {
        return Result<RenderResourceLease>::Failure(ErrorCode::TypeMismatch);
    }

    if (entry->state != RenderResourceState::Ready) {
        return Result<RenderResourceLease>::Failure(ErrorCode::NotFound);
    }

    ++entry->leases;
    RenderResourceLease lease;
    lease.handle = handle;
    lease.kind = kind;
    lease.device = entry->device;
    return Result<RenderResourceLease>::Success(lease);
}

void RenderHandleTable::ReleaseLease(const RenderResourceLease& lease) noexcept {
    platform::LockGuard lock(gate_);
    Entry* entry = entries_.Find(lease.handle.slot);
    if (entry == nullptr || entry->leases == 0) {
        return;
    }

    --entry->leases;
    if (entry->leases == 0 && entry->retireDeferred) {
        auto pushed = releasedRetires_.TryPushBack(lease.handle);
        if (!pushed) {
            // 목록에 넣지 못하면 장치 Retire를 이어 갈 수 없다. 장치 자원이 새지 않게 멈춘다.
            platform::Fatal("RenderHandleTable released retire list allocation failed");
        }
    }
}

uint32 RenderHandleTable::LeaseCount(RenderHandle handle) const noexcept {
    platform::LockGuard lock(gate_);
    const Entry* entry = entries_.Find(handle.slot);
    return entry == nullptr ? 0u : entry->leases;
}

bool RenderHandleTable::TakeReleasedRetire(RenderHandle& handle,
                                           RetiredResource& retired) noexcept {
    platform::LockGuard lock(gate_);
    while (!releasedRetires_.IsEmpty()) {
        const RenderHandle next = releasedRetires_[releasedRetires_.Size() - 1];
        auto popped = releasedRetires_.TryPopBack();
        (void)popped;
        Entry* entry = entries_.Find(next.slot);
        if (entry == nullptr || !entry->retireDeferred) {
            continue;
        }

        entry->retireDeferred = false;
        retired.kind = entry->kind;
        retired.device = entry->device;
        retired.deferred = false;
        entry->device = {};
        handle = next;
        return true;
    }

    return false;
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
