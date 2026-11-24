#include <RenderGraph/GraphResourcePool.h>
#include <RenderGraph/GraphTypes.h>

// 그래프 transient heap 캐시다(Docs/Design/RenderBase 8절 "GX12·RG5 transient aliasing
// 설계"). heap은 그래프 사이에 남고, 그래프는 두 queue의 마지막 사용이 끝난 heap만 받는다. heap마다
// 만든 placed 자원을 (offset, desc)로 다시 쓴다(UE RHICoreTransientResourceAllocator의 heap 캐시와
// 자원 캐시와 같은 역할).
namespace rocks::render {

bool GraphResourcePool::TransientHeapIdle(const TransientHeapEntry& heap) const noexcept {
    const graphics::GpuFencePoint points[] = {heap.graphicsUse, heap.computeUse};
    for (const graphics::GpuFencePoint& point : points) {
        auto complete = device_.IsFenceComplete(point);
        if (!complete || !complete.Value()) {
            return false;
        }
    }

    return true;
}

Result<uint32> GraphResourcePool::AcquireTransientHeap(graphics::TransientHeapKind kind,
                                                       uint64 minBytes, uint64 frame) noexcept {
    uint32 found = GraphIndexNone;
    for (uint32 index = 0; index < transientHeaps_.Size(); ++index) {
        const TransientHeapEntry& heap = transientHeaps_[index];
        if (!heap.live || heap.inUse || heap.kind != kind || heap.bytes < minBytes ||
            !TransientHeapIdle(heap)) {
            continue;
        }

        if (found == GraphIndexNone || heap.bytes < transientHeaps_[found].bytes) {
            found = index;
        }
    }

    if (found == GraphIndexNone) {
        graphics::TransientHeapDesc desc;
        desc.kind = kind;
        desc.bytes = minBytes > config_.transientHeapBytes ? minBytes : config_.transientHeapBytes;
        auto created = device_.CreateTransientHeap(desc);
        if (!created) {
            return Result<uint32>::Failure(created.Error());
        }

        TransientHeapEntry heap;
        heap.handle = created.Value();
        heap.kind = kind;
        heap.bytes = desc.bytes;
        heap.live = true;
        // 놓인 칸을 다시 쓴다(placed 자원이 heap 번호로 가리키므로 번호는 바꾸지 않는다).
        for (uint32 index = 0; index < transientHeaps_.Size() && found == GraphIndexNone; ++index) {
            if (!transientHeaps_[index].live) {
                found = index;
            }
        }

        if (found == GraphIndexNone) {
            auto pushed = transientHeaps_.TryPushBack(heap);
            if (!pushed) {
                device_.RetireTransientHeap(heap.handle);
                return Result<uint32>::Failure(pushed.Error());
            }

            found = static_cast<uint32>(transientHeaps_.Size() - 1);
        } else {
            transientHeaps_[found] = heap;
        }

        ++transientHeapsCreated_;
    }

    transientHeaps_[found].inUse = true;
    transientHeaps_[found].lastFrame = frame;
    return Result<uint32>::Success(found);
}

Result<SlotHandle> GraphResourcePool::Placed(uint32 heap, uint64 offset, const Key& key,
                                             uint64 frame) noexcept {
    if (heap >= transientHeaps_.Size() || !transientHeaps_[heap].live) {
        return Result<SlotHandle>::Failure(ErrorCode::StaleHandle);
    }

    // 같은 heap·offset·desc이고 이 그래프가 아직 쓰지 않은 객체를 다시 준다(UE 자원 캐시도 한
    // 프레임 안에서는 객체 하나를 한 번만 준다). 수명이 이어지는 두 그래프 자원은 다른 객체이고
    // 장치의 획득이 둘 사이를 맞춘다.
    for (PlacedEntry& entry : placed_) {
        if (entry.live && !entry.inUse && entry.heap == heap && entry.offset == offset &&
            SameKey(entry.key, key)) {
            entry.lastFrame = frame;
            entry.inUse = true;
            return Result<SlotHandle>::Success(entry.device);
        }
    }

    const graphics::TransientHeapHandle handle = transientHeaps_[heap].handle;
    SlotHandle device;
    if (key.texture) {
        auto created = device_.CreatePlacedTexture(handle, offset, key.textureDesc);
        if (!created) {
            return Result<SlotHandle>::Failure(created.Error());
        }

        device = created.Value().slot;
    } else {
        auto created = device_.CreatePlacedBuffer(handle, offset, key.bufferDesc);
        if (!created) {
            return Result<SlotHandle>::Failure(created.Error());
        }

        device = created.Value().slot;
    }

    PlacedEntry entry;
    entry.heap = heap;
    entry.offset = offset;
    entry.key = key;
    entry.device = device;
    entry.lastFrame = frame;
    entry.live = true;
    entry.inUse = true;
    for (PlacedEntry& slot : placed_) {
        if (!slot.live) {
            slot = entry;
            ++placedCreated_;
            return Result<SlotHandle>::Success(device);
        }
    }

    auto pushed = placed_.TryPushBack(entry);
    if (!pushed) {
        RetirePlaced(entry);
        return Result<SlotHandle>::Failure(pushed.Error());
    }

    ++placedCreated_;
    return Result<SlotHandle>::Success(device);
}

Result<graphics::TextureHandle> GraphResourcePool::PlacedTexture(uint32 heap, uint64 offset,
                                                                 const graphics::TextureDesc& desc,
                                                                 uint64 frame) noexcept {
    Key key;
    key.texture = true;
    key.textureDesc = desc;
    auto placed = Placed(heap, offset, key, frame);
    if (!placed) {
        return Result<graphics::TextureHandle>::Failure(placed.Error());
    }

    return Result<graphics::TextureHandle>::Success(graphics::TextureHandle{placed.Value()});
}

Result<graphics::BufferHandle> GraphResourcePool::PlacedBuffer(uint32 heap, uint64 offset,
                                                               const graphics::BufferDesc& desc,
                                                               uint64 frame) noexcept {
    Key key;
    key.texture = false;
    key.bufferDesc = desc;
    auto placed = Placed(heap, offset, key, frame);
    if (!placed) {
        return Result<graphics::BufferHandle>::Failure(placed.Error());
    }

    return Result<graphics::BufferHandle>::Success(graphics::BufferHandle{placed.Value()});
}

void GraphResourcePool::ReleaseTransientHeap(uint32 heap, graphics::GpuFencePoint graphicsUse,
                                             graphics::GpuFencePoint computeUse,
                                             uint64 frame) noexcept {
    if (heap >= transientHeaps_.Size()) {
        return;
    }

    TransientHeapEntry& entry = transientHeaps_[heap];
    entry.inUse = false;
    for (PlacedEntry& placed : placed_) {
        if (placed.heap == heap) {
            placed.inUse = false;
        }
    }

    entry.graphicsUse = graphicsUse;
    entry.computeUse = computeUse;
    entry.lastFrame = frame;
}

void GraphResourcePool::RetirePlaced(PlacedEntry& entry) noexcept {
    if (!entry.live) {
        return;
    }

    if (entry.key.texture) {
        device_.Retire(graphics::TextureHandle{entry.device});
    } else {
        device_.Retire(graphics::BufferHandle{entry.device});
    }

    entry.live = false;
}

void GraphResourcePool::EvictTransient(uint64 frame) noexcept {
    // placed 자원: 오래 쓰지 않았고 heap이 쓰는 중이 아니며 heap의 마지막 사용이 끝났다.
    for (PlacedEntry& entry : placed_) {
        const TransientHeapEntry& heap = transientHeaps_[entry.heap];
        if (entry.live && !heap.inUse && entry.lastFrame + config_.placedRetainFrames <= frame &&
            TransientHeapIdle(heap)) {
            RetirePlaced(entry);
        }
    }

    // heap: 같은 조건이면 남은 placed 자원과 함께 놓는다(장치가 두 queue 마지막 사용 뒤 해제).
    for (uint32 index = 0; index < transientHeaps_.Size(); ++index) {
        TransientHeapEntry& heap = transientHeaps_[index];
        if (!heap.live || heap.inUse ||
            heap.lastFrame + config_.transientHeapRetainFrames > frame ||
            !TransientHeapIdle(heap)) {
            continue;
        }

        for (PlacedEntry& entry : placed_) {
            if (entry.live && entry.heap == index) {
                RetirePlaced(entry);
            }
        }

        device_.RetireTransientHeap(heap.handle);
        heap.live = false;
    }
}

} // namespace rocks::render
