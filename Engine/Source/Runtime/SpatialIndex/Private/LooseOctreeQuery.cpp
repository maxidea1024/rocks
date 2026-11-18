#include "LooseOctreeInternal.h"

// LooseOctreeReader의 버퍼·존재·개수 질의와 노드 묶음
// 수집이다(Docs/Design/SpatialIndex.ko.md 6.1절). 형상마다 같은 공통
// 구현(IntoShape·AnyShape·CountShape)을 쓴다.
namespace rocks::spatial {

template <typename Shape>
BufferQueryOutcome LooseOctreeReader::IntoShape(const Shape& shape, uint32 filterMask,
                                                Span<SpatialProxyHit> out) const noexcept {
    BufferQueryOutcome outcome;
    const auto sink = [&](const SpatialProxyHit& hit) noexcept {
        if (outcome.total < out.Size()) {
            out[static_cast<usize>(outcome.total)] = hit;
            ++outcome.written;
        }
        ++outcome.total;
        return QueryControl::Continue;
    };
    (void)tree_->RunQuery(shape, filterMask, sink, nullptr);
    outcome.truncated = outcome.total > out.Size();
    return outcome;
}

template <typename Shape>
bool LooseOctreeReader::AnyShape(const Shape& shape, uint32 filterMask) const noexcept {
    const auto sink = [](const SpatialProxyHit&) noexcept { return QueryControl::Stop; };
    return tree_->RunQuery(shape, filterMask, sink, nullptr).stopped;
}

template <typename Shape>
uint64 LooseOctreeReader::CountShape(const Shape& shape, uint32 filterMask) const noexcept {
    uint64 count = 0;
    const auto sink = [&](const SpatialProxyHit&) noexcept {
        ++count;
        return QueryControl::Continue;
    };
    (void)tree_->RunQuery(shape, filterMask, sink, nullptr);
    return count;
}

// ---- 박스 ----

Result<BufferQueryOutcome>
LooseOctreeReader::QueryBoxInto(const Box<double>& box, uint32 filterMask,
                                Span<SpatialProxyHit> out) const noexcept {
    if (!box.IsValid()) {
        return Result<BufferQueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<BufferQueryOutcome>::Success(
        IntoShape(detail::BoxQueryShape{box}, filterMask, out));
}

Result<bool> LooseOctreeReader::AnyInBox(const Box<double>& box, uint32 filterMask) const noexcept {
    if (!box.IsValid()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<bool>::Success(AnyShape(detail::BoxQueryShape{box}, filterMask));
}

Result<uint64> LooseOctreeReader::CountInBox(const Box<double>& box,
                                             uint32 filterMask) const noexcept {
    if (!box.IsValid()) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<uint64>::Success(CountShape(detail::BoxQueryShape{box}, filterMask));
}

// ---- 구 ----

Result<BufferQueryOutcome>
LooseOctreeReader::QuerySphereInto(const Sphere<double>& sphere, uint32 filterMask,
                                   Span<SpatialProxyHit> out) const noexcept {
    if (!sphere.IsValid()) {
        return Result<BufferQueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<BufferQueryOutcome>::Success(
        IntoShape(detail::SphereQueryShape{sphere}, filterMask, out));
}

Result<bool> LooseOctreeReader::AnyInSphere(const Sphere<double>& sphere,
                                            uint32 filterMask) const noexcept {
    if (!sphere.IsValid()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<bool>::Success(AnyShape(detail::SphereQueryShape{sphere}, filterMask));
}

Result<uint64> LooseOctreeReader::CountInSphere(const Sphere<double>& sphere,
                                                uint32 filterMask) const noexcept {
    if (!sphere.IsValid()) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<uint64>::Success(CountShape(detail::SphereQueryShape{sphere}, filterMask));
}

// ---- 볼록 영역 ----

Result<BufferQueryOutcome>
LooseOctreeReader::QueryFrustumInto(const ConvexVolume<double>& volume, uint32 filterMask,
                                    Span<SpatialProxyHit> out) const noexcept {
    if (!volume.IsValid()) {
        return Result<BufferQueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<BufferQueryOutcome>::Success(
        IntoShape(detail::VolumeQueryShape{&volume}, filterMask, out));
}

Result<bool> LooseOctreeReader::AnyInFrustum(const ConvexVolume<double>& volume,
                                             uint32 filterMask) const noexcept {
    if (!volume.IsValid()) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<bool>::Success(AnyShape(detail::VolumeQueryShape{&volume}, filterMask));
}

Result<uint64> LooseOctreeReader::CountInFrustum(const ConvexVolume<double>& volume,
                                                 uint32 filterMask) const noexcept {
    if (!volume.IsValid()) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<uint64>::Success(CountShape(detail::VolumeQueryShape{&volume}, filterMask));
}

// ---- ray ----

Result<BufferQueryOutcome>
LooseOctreeReader::QueryRayInto(const Ray<double>& ray, double maxDistance, uint32 filterMask,
                                Span<SpatialProxyHit> out) const noexcept {
    if (!IsValidRay(ray, maxDistance)) {
        return Result<BufferQueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<BufferQueryOutcome>::Success(
        IntoShape(detail::RayQueryShape{ray, maxDistance}, filterMask, out));
}

Result<bool> LooseOctreeReader::AnyOnRay(const Ray<double>& ray, double maxDistance,
                                         uint32 filterMask) const noexcept {
    if (!IsValidRay(ray, maxDistance)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<bool>::Success(AnyShape(detail::RayQueryShape{ray, maxDistance}, filterMask));
}

Result<uint64> LooseOctreeReader::CountOnRay(const Ray<double>& ray, double maxDistance,
                                             uint32 filterMask) const noexcept {
    if (!IsValidRay(ray, maxDistance)) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<uint64>::Success(CountShape(detail::RayQueryShape{ray, maxDistance}, filterMask));
}

// ---- 노드 묶음 ----

Result<BufferQueryOutcome>
LooseOctreeReader::CollectNodeBatches(const Box<double>& box,
                                      Span<SpatialNodeBatch> out) const noexcept {
    if (!box.IsValid()) {
        return Result<BufferQueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<BufferQueryOutcome>::Success(
        tree_->CollectBatches(detail::BoxQueryShape{box}, out));
}

Result<BufferQueryOutcome>
LooseOctreeReader::CollectNodeBatches(const Sphere<double>& sphere,
                                      Span<SpatialNodeBatch> out) const noexcept {
    if (!sphere.IsValid()) {
        return Result<BufferQueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<BufferQueryOutcome>::Success(
        tree_->CollectBatches(detail::SphereQueryShape{sphere}, out));
}

Result<BufferQueryOutcome>
LooseOctreeReader::CollectNodeBatches(const ConvexVolume<double>& volume,
                                      Span<SpatialNodeBatch> out) const noexcept {
    if (!volume.IsValid()) {
        return Result<BufferQueryOutcome>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<BufferQueryOutcome>::Success(
        tree_->CollectBatches(detail::VolumeQueryShape{&volume}, out));
}

} // namespace rocks::spatial
