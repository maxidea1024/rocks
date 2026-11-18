#include <RenderGraph/GraphResourcePool.h>

#include <Core/Algorithms.h>
#include <Platform/Memory.h>

namespace rocks::render {

namespace {

// 텍스처 byte 추정이다(mip 사슬 전체, 장치 정렬 제외). PixelFormatInfo의 블록 크기·byte로
// 계산한다(MipChainBytes, 2D라 depth 1). 지금 텍스처 desc에는 깊이·array가 없다(GX2).
uint64 TextureBytes(const graphics::TextureDesc& desc) noexcept {
    return graphics::MipChainBytes(desc.format, desc.width, desc.height, 1, desc.mipLevels);
}

} // namespace

GraphResourcePool::GraphResourcePool(Allocator& allocator, graphics::Device& device,
                                     const GraphResourcePoolConfig& config) noexcept
    : allocator_(allocator), device_(device), config_(config), entries_(allocator) {
}

GraphResourcePool::~GraphResourcePool() noexcept {
    for (const Entry& entry : entries_) {
        // export 중인 자원은 handle에 소유를 넘긴다(그 뒤 handle의 Retire가 장치 Retire다).
        if (entry.state == EntryState::Exported) {
            entry.handles->DetachRetireSink(entry.handle, this);
            continue;
        }

        // 쓰는 중인 자원이 남았으면(그래프가 아직 살아 있다) 그것도 장치 Retire로 넘긴다. 장치가
        // 마지막 사용 fence 뒤에 해제한다.
        RetireEntry(entry);
    }

    entries_.Clear();
}

bool GraphResourcePool::SameTextureKey(const graphics::TextureDesc& left,
                                       const graphics::TextureDesc& right) noexcept {
    return left.width == right.width && left.height == right.height &&
           left.format == right.format && left.usage == right.usage &&
           left.mipLevels == right.mipLevels && left.sampler == right.sampler;
}

bool GraphResourcePool::SameBufferKey(const graphics::BufferDesc& left,
                                      const graphics::BufferDesc& right) noexcept {
    return left.size == right.size && left.usage == right.usage && left.stride == right.stride;
}

bool GraphResourcePool::SameKey(const Key& left, const Key& right) noexcept {
    if (left.texture != right.texture) {
        return false;
    }

    return left.texture ? SameTextureKey(left.textureDesc, right.textureDesc)
                        : SameBufferKey(left.bufferDesc, right.bufferDesc);
}

void GraphResourcePool::RetireEntry(const Entry& entry) noexcept {
    if (entry.key.texture) {
        device_.Retire(graphics::TextureHandle{entry.device});
    } else {
        device_.Retire(graphics::BufferHandle{entry.device});
    }
}

// Sampled 텍스처의 mip 사슬을 mip마다 장치 갱신으로 올린다(다시 준 자원).
Result<void> GraphResourcePool::FillTexture(const Entry& entry, Span<const uint8> pixels) noexcept {
    const graphics::TextureDesc& desc = entry.key.textureDesc;
    usize offset = 0;
    for (uint32 mip = 0; mip < desc.mipLevels; ++mip) {
        const usize bytes = static_cast<usize>(
            graphics::PixelFormatImageBytes(desc.format, graphics::MipExtent(desc.width, mip),
                                            graphics::MipExtent(desc.height, mip)));
        auto updated = device_.UpdateTexture(graphics::TextureHandle{entry.device}, mip,
                                             pixels.Slice(offset, bytes));
        if (!updated) {
            return updated;
        }

        offset += bytes;
    }

    return Result<void>::Success();
}

Result<GraphResourcePool::Acquired> GraphResourcePool::Acquire(const Key& key,
                                                               Span<const uint8> pixels) noexcept {
    (void)CollectReturned();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        Entry& entry = *it;
        if (entry.state != EntryState::Free || !SameKey(entry.key, key)) {
            continue;
        }

        if (!pixels.IsEmpty()) {
            auto filled = FillTexture(entry, pixels);
            if (!filled) {
                return Result<Acquired>::Failure(filled.Error());
            }
        }

        entry.state = EntryState::InUse;
        ++reused_;
        Acquired acquired;
        acquired.entry = GraphPoolEntry{it.Handle()};
        acquired.device = entry.device;
        acquired.reused = true;
        return Result<Acquired>::Success(acquired);
    }

    Entry entry;
    entry.key = key;
    entry.state = EntryState::InUse;
    if (key.texture) {
        auto created = device_.CreateTexture(key.textureDesc, pixels);
        if (!created) {
            return Result<Acquired>::Failure(created.Error());
        }

        entry.device = created.Value().slot;
        entry.bytes = TextureBytes(key.textureDesc);
    } else {
        // 장치 계약이 생성 때 초기 내용을 요구하므로 0으로 채워 만든다.
        const usize size = static_cast<usize>(key.bufferDesc.size);
        auto zeros = allocator_.Allocate(size, alignof(uint64));
        if (!zeros) {
            return Result<Acquired>::Failure(zeros.Error());
        }

        platform::MemoryZero(zeros.Value(), size);
        auto created = device_.CreateBuffer(
            key.bufferDesc, Span<const uint8>(static_cast<const uint8*>(zeros.Value()), size));
        (void)allocator_.Deallocate(zeros.Value());
        if (!created) {
            return Result<Acquired>::Failure(created.Error());
        }

        entry.device = created.Value().slot;
        entry.bytes = size;
    }

    auto added = entries_.TryAdd(entry);
    if (!added) {
        RetireEntry(entry);
        return Result<Acquired>::Failure(added.Error());
    }

    bytes_ += entry.bytes;
    ++created_;
    Acquired acquired;
    acquired.entry = GraphPoolEntry{added.Value()};
    acquired.device = entry.device;
    return Result<Acquired>::Success(acquired);
}

Result<GraphResourcePool::Acquired>
GraphResourcePool::AcquireTexture(const graphics::TextureDesc& desc,
                                  Span<const uint8> pixels) noexcept {
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0) {
        return Result<Acquired>::Failure(ErrorCode::InvalidArgument);
    }

    switch (desc.usage) {
    case graphics::TextureUsage::RenderTarget:
        if (!pixels.IsEmpty()) {
            return Result<Acquired>::Failure(ErrorCode::InvalidArgument);
        }

        break;
    case graphics::TextureUsage::Sampled:
        if (pixels.IsEmpty() || pixels.Size() != TextureBytes(desc)) {
            return Result<Acquired>::Failure(ErrorCode::InvalidArgument);
        }

        break;
    case graphics::TextureUsage::DepthStencil:
        return Result<Acquired>::Failure(ErrorCode::Unsupported);
    }

    Key key;
    key.texture = true;
    key.textureDesc = desc;
    return Acquire(key, pixels);
}

Result<GraphResourcePool::Acquired>
GraphResourcePool::AcquireBuffer(const graphics::BufferDesc& desc) noexcept {
    if (desc.size == 0) {
        return Result<Acquired>::Failure(ErrorCode::InvalidArgument);
    }

    Key key;
    key.texture = false;
    key.bufferDesc = desc;
    return Acquire(key, {});
}

void GraphResourcePool::Release(GraphPoolEntry entry, graphics::GpuFencePoint lastUse,
                                uint64 frame) noexcept {
    Entry* found = entries_.Find(entry.slot);
    if (found == nullptr || found->state != EntryState::InUse) {
        return;
    }

    found->state = EntryState::Free;
    found->lastUse = lastUse;
    found->lastFrame = frame;
    frame_ = frame > frame_ ? frame : frame_;
}

Result<void> GraphResourcePool::Export(GraphPoolEntry entry, RenderHandleTable& handles,
                                       RenderHandle handle) noexcept {
    Entry* found = entries_.Find(entry.slot);
    if (found == nullptr || found->state != EntryState::InUse) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    auto bound = found->key.texture
                     ? handles.BindTexture(handle, graphics::TextureHandle{found->device}, this)
                     : handles.BindBuffer(handle, graphics::BufferHandle{found->device}, this);
    if (!bound) {
        return bound;
    }

    found->state = EntryState::Exported;
    found->handles = &handles;
    found->handle = handle;
    ++exports_;
    return Result<void>::Success();
}

void GraphResourcePool::OnRenderRetire(RenderHandle handle, RenderResourceKind kind,
                                       SlotHandle device,
                                       graphics::GpuFencePoint lastUse) noexcept {
    for (Entry& entry : entries_) {
        if (entry.state != EntryState::Exported || !(entry.handle == handle) ||
            !(entry.device == device)) {
            continue;
        }

        // 반납 대기다. 마지막 사용 fence가 끝나야 다시 준다(CollectReturned).
        entry.state = EntryState::Returning;
        entry.lastUse = lastUse;
        entry.lastFrame = frame_;
        entry.handles = nullptr;
        entry.handle = {};
        return;
    }

    // 이 풀이 export하지 않은 자원이다. 소유자가 없으므로 장치 Retire로 넘긴다.
    if (kind == RenderResourceKind::Texture) {
        device_.Retire(graphics::TextureHandle{device});
    } else if (kind == RenderResourceKind::Buffer) {
        device_.Retire(graphics::BufferHandle{device});
    }
}

usize GraphResourcePool::CollectReturned() noexcept {
    usize collected = 0;
    for (Entry& entry : entries_) {
        if (entry.state != EntryState::Returning) {
            continue;
        }

        auto complete = device_.IsFenceComplete(entry.lastUse);
        if (!complete || !complete.Value()) {
            continue;
        }

        entry.state = EntryState::Free;
        ++returned_;
        ++collected;
    }

    return collected;
}

usize GraphResourcePool::Evict(uint64 frame) noexcept {
    frame_ = frame > frame_ ? frame : frame_;
    (void)CollectReturned();
    if (bytes_ <= config_.budgetBytes) {
        return 0;
    }

    // 후보: 쓰지 않음, retainFrames가 지났고, 마지막 사용 fence가 GPU에서 끝났다.
    struct Candidate {
        SlotHandle slot;
        uint64 lastFrame = 0;
    };

    Array<Candidate> candidates(allocator_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        const Entry& entry = *it;
        if (entry.state != EntryState::Free || entry.lastFrame + config_.retainFrames > frame) {
            continue;
        }

        auto complete = device_.IsFenceComplete(entry.lastUse);
        if (!complete || !complete.Value()) {
            continue;
        }

        if (!candidates.TryPushBack(Candidate{it.Handle(), entry.lastFrame})) {
            break;
        }
    }

    // 오래된 순으로 예산 아래가 될 때까지 회수한다.
    algo::Sort(candidates.View(), [](const Candidate& left, const Candidate& right) noexcept {
        return left.lastFrame < right.lastFrame;
    });
    usize evicted = 0;
    for (const Candidate& candidate : candidates) {
        if (bytes_ <= config_.budgetBytes) {
            break;
        }

        auto taken = entries_.TryTake(candidate.slot);
        if (!taken) {
            continue;
        }

        RetireEntry(taken.Value());
        bytes_ -= taken.Value().bytes;
        ++evicted_;
        ++evicted;
    }

    return evicted;
}

void GraphResourcePool::Clear() noexcept {
    for (auto it = entries_.begin(); it != entries_.end();) {
        const SlotHandle slot = it.Handle();
        const Entry entry = *it;
        ++it;
        if (entry.state == EntryState::InUse || entry.state == EntryState::Exported) {
            continue;
        }

        RetireEntry(entry);
        bytes_ -= entry.bytes;
        auto removed = entries_.TryRemove(slot);
        (void)removed;
    }
}

GraphPoolStats GraphResourcePool::Stats() const noexcept {
    GraphPoolStats stats;
    for (const Entry& entry : entries_) {
        ++stats.resources;
        stats.inUse += entry.state == EntryState::InUse ? 1u : 0u;
        stats.exported += entry.state == EntryState::Exported ? 1u : 0u;
        stats.returning += entry.state == EntryState::Returning ? 1u : 0u;
    }

    stats.bytes = bytes_;
    stats.created = created_;
    stats.reused = reused_;
    stats.evicted = evicted_;
    stats.exports = exports_;
    stats.returned = returned_;
    return stats;
}

} // namespace rocks::render
