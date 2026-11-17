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
    // 쓰는 중인 자원이 남았으면(그래프가 아직 살아 있다) 그것도 장치 Retire로 넘긴다. 장치가 마지막
    // 사용 fence 뒤에 해제한다.
    for (const Entry& entry : entries_) {
        RetireEntry(entry);
    }

    entries_.Clear();
}

bool GraphResourcePool::SameKey(const Key& left, const Key& right) noexcept {
    if (left.texture != right.texture) {
        return false;
    }

    if (!left.texture) {
        return left.bufferDesc.size == right.bufferDesc.size &&
               left.bufferDesc.usage == right.bufferDesc.usage;
    }

    const graphics::TextureDesc& a = left.textureDesc;
    const graphics::TextureDesc& b = right.textureDesc;
    return a.width == b.width && a.height == b.height && a.format == b.format &&
           a.usage == b.usage && a.mipLevels == b.mipLevels && a.sampler == b.sampler;
}

void GraphResourcePool::RetireEntry(const Entry& entry) noexcept {
    if (entry.key.texture) {
        device_.Retire(graphics::TextureHandle{entry.device});
    } else {
        device_.Retire(graphics::BufferHandle{entry.device});
    }
}

Result<GraphResourcePool::Acquired> GraphResourcePool::Acquire(const Key& key) noexcept {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        Entry& entry = *it;
        if (entry.inUse || !SameKey(entry.key, key)) {
            continue;
        }

        entry.inUse = true;
        ++reused_;
        Acquired acquired;
        acquired.entry = GraphPoolEntry{it.Handle()};
        acquired.device = entry.device;
        acquired.reused = true;
        return Result<Acquired>::Success(acquired);
    }

    Entry entry;
    entry.key = key;
    entry.inUse = true;
    if (key.texture) {
        auto created = device_.CreateTexture(key.textureDesc, Span<const uint8>());
        if (!created) {
            return Result<Acquired>::Failure(created.Error());
        }

        entry.device = created.Value().slot;
        entry.bytes = TextureBytes(key.textureDesc);
    } else {
        // 장치 계약이 생성 때 초기 내용을 요구하므로 0으로 채워 만든다.
        const usize size = key.bufferDesc.size;
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
GraphResourcePool::AcquireTexture(const graphics::TextureDesc& desc) noexcept {
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0 ||
        desc.usage != graphics::TextureUsage::RenderTarget) {
        return Result<Acquired>::Failure(ErrorCode::InvalidArgument);
    }

    Key key;
    key.texture = true;
    key.textureDesc = desc;
    return Acquire(key);
}

Result<GraphResourcePool::Acquired>
GraphResourcePool::AcquireBuffer(const graphics::BufferDesc& desc) noexcept {
    if (desc.size == 0) {
        return Result<Acquired>::Failure(ErrorCode::InvalidArgument);
    }

    Key key;
    key.texture = false;
    key.bufferDesc = desc;
    return Acquire(key);
}

void GraphResourcePool::Release(GraphPoolEntry entry, graphics::GpuFencePoint lastUse,
                                uint64 frame) noexcept {
    Entry* found = entries_.Find(entry.slot);
    if (found == nullptr) {
        return;
    }

    found->inUse = false;
    found->lastUse = lastUse;
    found->lastFrame = frame;
}

Result<SlotHandle> GraphResourcePool::Detach(GraphPoolEntry entry) noexcept {
    auto taken = entries_.TryTake(entry.slot);
    if (!taken) {
        return Result<SlotHandle>::Failure(ErrorCode::StaleHandle);
    }

    bytes_ -= taken.Value().bytes;
    ++detached_;
    return Result<SlotHandle>::Success(taken.Value().device);
}

usize GraphResourcePool::Evict(uint64 frame) noexcept {
    if (bytes_ <= config_.budgetBytes) {
        return 0;
    }

    // 후보: 쓰지 않는 중, retainFrames가 지났고, 마지막 사용 fence가 GPU에서 끝났다.
    struct Candidate {
        SlotHandle slot;
        uint64 lastFrame = 0;
    };

    Array<Candidate> candidates(allocator_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        const Entry& entry = *it;
        if (entry.inUse || entry.lastFrame + config_.retainFrames > frame) {
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
        if (entry.inUse) {
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
        stats.inUse += entry.inUse ? 1u : 0u;
    }

    stats.bytes = bytes_;
    stats.created = created_;
    stats.reused = reused_;
    stats.evicted = evicted_;
    stats.detached = detached_;
    return stats;
}

} // namespace rocks::render
