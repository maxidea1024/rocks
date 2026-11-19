#pragma once

#include "FlatScan.h"

#include <Core/Move.h>
#include <SpatialIndex/LooseOctree.h>

// 비교군 D(Docs/Design/SpatialIndex.ko.md 9.1절): 엔진 LooseOctree를 공통 벤치 API로 감싼다. 변형
// (9.4절 중 이번에 구조를 바꾸지 않고 실현 가능한 것)은 LooseVariant로 고른다.
namespace rocks::spatialbench {

struct LooseVariant {
    const char* name = "D";
    spatial::LooseOctreeConfig config;
    // 거짓이면 경계 갱신을 TryRemove + TryAdd(삭제 후 재삽입)로 한다(제자리 갱신 끄기).
    bool inPlaceUpdate = true;
    // 참이면 변경 호출마다 RunMaintenance(All)을 불러 병합·회수를 바로 한다(즉시 병합 근사).
    bool immediateMaintenance = false;
    // 참이면 박스 질의를 축 정렬 평면 6개 ConvexVolume으로 보낸다. 프록시 판정이 float 상대 경계
    // 대신 레코드의 double AABB(Core 술어)가 된다(박스 질의 double 변형의 근사).
    bool boxAsVolume = false;
};

class LooseIndex {
public:
    LooseIndex(Allocator& allocator, const LooseVariant& variant) noexcept
        : allocator_(&allocator), variant_(variant), handles_(allocator) {
    }

    ~LooseIndex() noexcept {
        Destroy();
    }

    LooseIndex(const LooseIndex&) = delete;
    LooseIndex& operator=(const LooseIndex&) = delete;

    const char* Name() const noexcept {
        return variant_.name;
    }

    bool Create(uint32 capacity) noexcept {
        Destroy();
        Result<spatial::LooseOctree> created =
            spatial::LooseOctree::TryCreate(variant_.config, *allocator_);
        if (!created) {
            return false;
        }
        tree_ = ConstructAt<spatial::LooseOctree>(treeStorage_, Move(created.Value()));
        return static_cast<bool>(handles_.TryResize(capacity));
    }

    void Destroy() noexcept {
        EndRead();
        if (tree_ != nullptr) {
            DestroyAt(tree_);
            tree_ = nullptr;
        }
        handles_.Empty();
    }

    bool Insert(uint32 id, const Box<double>& bounds, uint32 filter) noexcept {
        if (id >= handles_.Size() && !handles_.TryResize(id + 1)) {
            return false;
        }
        spatial::SpatialProxyDesc desc;
        desc.bounds = bounds;
        desc.filterMask = filter;
        desc.userValue = id;
        Result<spatial::SpatialProxyHandle> added = tree_->TryAdd(desc);
        if (!added) {
            return false;
        }
        handles_[id] = added.Value();
        AfterMutation();
        return true;
    }

    bool Remove(uint32 id, const Box<double>&) noexcept {
        const bool removed = static_cast<bool>(tree_->TryRemove(handles_[id]));
        AfterMutation();
        return removed;
    }

    bool Update(uint32 id, const Box<double>&, const Box<double>& bounds) noexcept {
        if (variant_.inPlaceUpdate) {
            const bool updated = static_cast<bool>(tree_->TryUpdateBounds(handles_[id], bounds));
            AfterMutation();
            return updated;
        }
        if (!tree_->TryRemove(handles_[id])) {
            return false;
        }
        spatial::SpatialProxyDesc desc;
        desc.bounds = bounds;
        desc.filterMask = ProxyFilter(id);
        desc.userValue = id;
        Result<spatial::SpatialProxyHandle> added = tree_->TryAdd(desc);
        if (!added) {
            return false;
        }
        handles_[id] = added.Value();
        AfterMutation();
        return true;
    }

    void Maintain(uint32 operations) noexcept {
        (void)tree_->RunMaintenance(spatial::MaintenanceBudget::Operations(operations));
    }

    void MaintainAll() noexcept {
        (void)tree_->RunMaintenance(spatial::MaintenanceBudget::All());
    }

    // 읽기 범위는 질의 묶음 단위로 연다(6.9절: 질의마다 atomic을 건드리지 않는다).
    void BeginRead() noexcept {
        if (scope_ == nullptr) {
            scope_ = ConstructAt<spatial::SpatialReadScope>(scopeStorage_, *tree_);
        }
    }

    void EndRead() noexcept {
        if (scope_ != nullptr) {
            DestroyAt(scope_);
            scope_ = nullptr;
        }
    }

    template <bool Counting>
    void QueryBox(const Box<double>& box, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        if (variant_.boxAsVolume) {
            QueryFrustum<Counting>(BoxVolume(box), mask, out, counters);
            return;
        }
        spatial::QueryStats stats;
        (void)Reader().QueryBox(
            box, mask,
            [&](const spatial::SpatialProxyHit& hit) noexcept {
                out.Push(static_cast<uint32>(hit.userValue));
                return spatial::QueryControl::Continue;
            },
            Counting ? &stats : nullptr);
        Accumulate<Counting>(stats, counters);
    }

    template <bool Counting>
    void QuerySphere(const Sphere<double>& sphere, uint32 mask, ResultBuffer& out,
                     QueryCounters& counters) const noexcept {
        spatial::QueryStats stats;
        (void)Reader().QuerySphere(
            sphere, mask,
            [&](const spatial::SpatialProxyHit& hit) noexcept {
                out.Push(static_cast<uint32>(hit.userValue));
                return spatial::QueryControl::Continue;
            },
            Counting ? &stats : nullptr);
        Accumulate<Counting>(stats, counters);
    }

    template <bool Counting>
    void QueryFrustum(const ConvexVolume<double>& volume, uint32 mask, ResultBuffer& out,
                      QueryCounters& counters) const noexcept {
        spatial::QueryStats stats;
        (void)Reader().QueryFrustum(
            volume, mask,
            [&](const spatial::SpatialProxyHit& hit) noexcept {
                out.Push(static_cast<uint32>(hit.userValue));
                return spatial::QueryControl::Continue;
            },
            Counting ? &stats : nullptr);
        Accumulate<Counting>(stats, counters);
    }

    template <bool Counting>
    void QueryRay(const Ray<double>& ray, double maxDistance, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        spatial::QueryStats stats;
        (void)Reader().QueryRay(
            ray, maxDistance, mask,
            [&](const spatial::SpatialProxyHit& hit) noexcept {
                out.Push(static_cast<uint32>(hit.userValue));
                return spatial::QueryControl::Continue;
            },
            Counting ? &stats : nullptr);
        Accumulate<Counting>(stats, counters);
    }

    // hitTest는 등록 AABB를 정밀 형상으로 보고 레코드의 AABB를 다시 읽어 Raycast한다(다른 비교군의
    // 정밀 판정과 같은 식). 레코드 조회는 TryGet이다.
    template <bool Counting>
    bool QueryNearest(const Ray<double>& ray, double maxDistance, uint32 mask, uint32& id,
                      double& distance, QueryCounters& counters) const noexcept {
        spatial::QueryStats stats;
        const spatial::LooseOctreeReader reader = Reader();
        Result<spatial::NearestHit> nearest = reader.QueryRayNearest(
            ray, maxDistance, mask,
            [&](const spatial::SpatialProxyHit& hit, double currentMax) noexcept {
                spatial::RayHitResult result{false, 0.0};
                Result<spatial::SpatialProxyDesc> desc = reader.TryGet(hit.handle);
                if (desc) {
                    result.hit = RayEntry(ray, desc.Value().bounds, currentMax, result.distance);
                }
                return result;
            },
            Counting ? &stats : nullptr);
        Accumulate<Counting>(stats, counters);
        if (!nearest || !nearest.Value().found) {
            return false;
        }
        id = static_cast<uint32>(nearest.Value().proxy.userValue);
        distance = nearest.Value().distance;
        return true;
    }

    template <bool Counting>
    bool AnyBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        if constexpr (Counting) {
            spatial::QueryStats stats;
            bool any = false;
            (void)Reader().QueryBox(
                box, mask,
                [&](const spatial::SpatialProxyHit&) noexcept {
                    any = true;
                    return spatial::QueryControl::Stop;
                },
                &stats);
            Accumulate<Counting>(stats, counters);
            return any;
        } else {
            Result<bool> any = Reader().AnyInBox(box, mask);
            return any && any.Value();
        }
    }

    template <bool Counting>
    uint64 CountBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        if constexpr (Counting) {
            spatial::QueryStats stats;
            uint64 total = 0;
            (void)Reader().QueryBox(
                box, mask,
                [&](const spatial::SpatialProxyHit&) noexcept {
                    ++total;
                    return spatial::QueryControl::Continue;
                },
                &stats);
            Accumulate<Counting>(stats, counters);
            return total;
        } else {
            Result<uint64> total = Reader().CountInBox(box, mask);
            return total ? total.Value() : 0;
        }
    }

    // 구조 통계(OctreeStats)를 쓴다. 읽기 범위를 잠깐 연다.
    void Describe(JsonWriter& json) noexcept {
        const bool wasOpen = scope_ != nullptr;
        BeginRead();
        const spatial::OctreeStats stats = Reader().Stats();
        if (!wasOpen) {
            EndRead();
        }
        json.FieldInteger("proxies", stats.proxyCount);
        json.FieldInteger("outOfRange", stats.outOfRangeCount);
        json.FieldInteger("largeProxies", stats.largeProxyCount);
        json.FieldInteger("nodes", stats.nodeCount);
        json.FieldInteger("leaves", stats.leafCount);
        json.FieldInteger("maxDepth", stats.maxDepth);
        json.FieldInteger("rootLevel", stats.rootLevel);
        json.FieldInteger("maxDirect", stats.maxDirectCount);
        json.FieldInteger("interiorDirect", stats.interiorDirectCount);
        json.FieldInteger("heldNodes", stats.heldNodeCount);
        json.FieldInteger("overParentWarnNodes", stats.overParentWarnNodeCount);
        json.FieldInteger("recordBytesReserved", stats.recordBytesReserved);
        json.FieldInteger("chunkBytesUsed", stats.chunkBytesUsed);
        json.FieldInteger("chunkBytesReserved", stats.chunkBytesReserved);
        json.FieldInteger("nodeBytesReserved", stats.nodeBytesReserved);
        json.FieldInteger("liveAllocatedBytes", stats.liveAllocatedBytes);
        json.FieldInteger("peakAllocatedBytes", stats.peakAllocatedBytes);
        json.FieldInteger("allocations", stats.allocationCount);
        json.FieldInteger("deallocations", stats.deallocationCount);
        json.FieldInteger("splits", stats.splits);
        json.FieldInteger("chainSplits", stats.chainSplits);
        json.FieldInteger("merges", stats.merges);
        json.FieldInteger("reclaims", stats.reclaims);
        json.FieldInteger("reinserts", stats.reinserts);
        json.FieldInteger("inPlaceUpdates", stats.inPlaceUpdates);
        json.FieldInteger("relocatedProxies", stats.relocatedProxies);
        json.FieldInteger("rootExpansions", stats.rootExpansions);
        json.FieldInteger("rootShrinks", stats.rootShrinks);
        json.FieldInteger("splitEvaluations", stats.splitEvaluations);
        json.FieldInteger("holds", stats.holds);
        json.FieldInteger("forcedMaintenanceRuns", stats.forcedMaintenanceRuns);
        json.FieldInteger("pendingTotal", stats.pendingTotal);
    }

    // 누적 계수 중 9.6 판정에 쓰는 값이다.
    spatial::OctreeStats Stats() noexcept {
        const bool wasOpen = scope_ != nullptr;
        BeginRead();
        const spatial::OctreeStats stats = Reader().Stats();
        if (!wasOpen) {
            EndRead();
        }
        return stats;
    }

    // 정확성 대조에서 Validate()를 함께 부른다.
    bool Validate() noexcept {
        const bool wasOpen = scope_ != nullptr;
        BeginRead();
        Result<spatial::OctreeValidationReport> report = Reader().Validate();
        if (!wasOpen) {
            EndRead();
        }
        return report && report.Value().IsValid();
    }

    // 병렬 측정용 reader다. BeginRead로 연 읽기 범위 안에서만 쓰며 여러 스레드에 복사해 넘긴다.
    spatial::LooseOctreeReader Reader() const noexcept {
        return scope_->Reader();
    }

private:
    void AfterMutation() noexcept {
        if (variant_.immediateMaintenance) {
            (void)tree_->RunMaintenance(spatial::MaintenanceBudget::All());
        }
    }

    template <bool Counting>
    static void Accumulate(const spatial::QueryStats& stats, QueryCounters& counters) noexcept {
        if constexpr (Counting) {
            counters.visitedNodes += stats.visitedNodes;
            counters.testedProxies += stats.testedProxies;
            counters.containedProxies += stats.containedProxies;
            counters.results += stats.results;
        }
    }

    // 축 정렬 평면 6개(법선 안쪽)다. SignedDistance = Dot(n, p) − d에서 단위 축 법선의 내적은 한
    // 성분 그대로라 반올림이 없어 닫힌 박스 겹침과 같다.
    static ConvexVolume<double> BoxVolume(const Box<double>& box) noexcept {
        ConvexVolume<double> volume;
        volume.planes[0] = Plane<double>(Vec3d(1.0, 0.0, 0.0), box.min.x);
        volume.planes[1] = Plane<double>(Vec3d(-1.0, 0.0, 0.0), -box.max.x);
        volume.planes[2] = Plane<double>(Vec3d(0.0, 1.0, 0.0), box.min.y);
        volume.planes[3] = Plane<double>(Vec3d(0.0, -1.0, 0.0), -box.max.y);
        volume.planes[4] = Plane<double>(Vec3d(0.0, 0.0, 1.0), box.min.z);
        volume.planes[5] = Plane<double>(Vec3d(0.0, 0.0, -1.0), -box.max.z);
        volume.planeCount = 6;
        return volume;
    }

    Allocator* allocator_;
    LooseVariant variant_;
    Array<spatial::SpatialProxyHandle> handles_;
    alignas(spatial::LooseOctree) unsigned char treeStorage_[sizeof(spatial::LooseOctree)];
    spatial::LooseOctree* tree_ = nullptr;
    alignas(
        spatial::SpatialReadScope) unsigned char scopeStorage_[sizeof(spatial::SpatialReadScope)];
    spatial::SpatialReadScope* scope_ = nullptr;
};

} // namespace rocks::spatialbench
