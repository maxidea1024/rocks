#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/RandomStream.h>
#include <Core/Math/Intersection.h>
#include <Core/Memory/Allocator.h>
#include <Platform/PlatformMath.h>
#include <SpatialIndex/LooseOctree.h>
#include <TestSupport/Test.h>

// LooseOctree 시험(Docs/Design/SpatialIndex.ko.md 8절 LO 시험)이 함께 쓰는 정답 모형과 입력
// 생성이다. 정답 기준은 조밀한 배열의 전수 검색이며 정확한 double AABB로 판정한다. 옥트리 결과를
// 정답으로 보지 않는다.
namespace rocks::spatial::oracle {

// 정답 모형의 프록시 하나다. userValue는 모형 색인이라 질의 결과를 모형 항목으로 되찾는다.
struct OracleProxy {
    SpatialProxyHandle handle;
    Box<double> bounds;
    uint32 filter = 0;
    bool live = false;
};

// 시험용 기본 설정이다. 작은 셀(1 cm)과 낮은 루트 level로 몇백 개 프록시에서도 분할·병합·확장이
// 일어나게 한다.
inline LooseOctreeConfig SmallConfig() noexcept {
    LooseOctreeConfig config;
    config.minCellSize = 1.0;
    config.initialRootLevel = 6;
    config.maxRootLevel = 16;
    config.looseness = 2.0;
    config.splitThreshold = 8;
    config.mergeThreshold = 4;
    config.maxProxies = 1u << 16;
    config.parentWarnCount = 1u << 20;
    config.outOfRangeWarnCount = 1u << 20;
    return config;
}

// 전수 검색 정답과 옥트리를 함께 바꾸는 시험 대상이다. 변경은 옥트리 결과가 성공일 때만 모형에
// 반영한다.
class OracleTree {
public:
    // reuseIndices이면 지운 프록시의 모형 색인(userValue)을 다음 추가에 다시 쓴다. 긴 시퀀스에서
    // 전수 대조 비용이 지금까지 만든 수가 아니라 살아 있는 수에 비례하게 한다.
    OracleTree(LooseOctree&& tree, Allocator& allocator, bool reuseIndices = false) noexcept
        : tree_(rocks::Move(tree)), proxies_(allocator), free_(allocator),
          reuseIndices_(reuseIndices) {
    }

    LooseOctree& Tree() noexcept {
        return tree_;
    }

    const Array<OracleProxy>& Proxies() const noexcept {
        return proxies_;
    }

    uint32 LiveCount() const noexcept {
        return live_;
    }

    // 추가가 성공하면 모형 색인, 실패하면 IndexNone이다.
    usize Add(const Box<double>& bounds, uint32 filter) noexcept {
        const bool reuse = reuseIndices_ && !free_.IsEmpty();
        const usize index = reuse ? free_[free_.Size() - 1] : proxies_.Size();
        SpatialProxyDesc desc;
        desc.bounds = bounds;
        desc.filterMask = filter;
        desc.userValue = index;
        auto added = tree_.TryAdd(desc);
        if (!added) {
            return IndexNone;
        }
        OracleProxy proxy;
        proxy.handle = added.Value();
        proxy.bounds = desc.bounds;
        proxy.filter = filter;
        proxy.live = true;
        if (reuse) {
            proxies_[index] = proxy;
            (void)free_.TryPopBack();
        } else if (!proxies_.TryPushBack(proxy)) {
            return IndexNone;
        }
        ++live_;
        return index;
    }

    // 옥트리에 이미 넣은 프록시를 모형에 기록한다(할당 실패 주입 시험처럼 옥트리를 직접 부를 때).
    // userValue는 이 함수가 돌려줄 색인(Proxies().Size())으로 넣었어야 한다.
    usize Record(SpatialProxyHandle handle, const Box<double>& bounds, uint32 filter) noexcept {
        OracleProxy proxy;
        proxy.handle = handle;
        proxy.bounds = bounds;
        proxy.filter = filter;
        proxy.live = true;
        if (!proxies_.TryPushBack(proxy)) {
            return IndexNone;
        }
        ++live_;
        return proxies_.Size() - 1;
    }

    // 옥트리에서 직접 성공한 경계 갱신을 모형에 기록한다.
    void RecordBounds(usize index, const Box<double>& bounds) noexcept {
        proxies_[index].bounds = bounds;
    }

    bool Remove(usize index) noexcept {
        if (!tree_.TryRemove(proxies_[index].handle)) {
            return false;
        }
        proxies_[index].live = false;
        --live_;
        if (reuseIndices_ && !free_.TryPushBack(index)) {
            return false;
        }
        return true;
    }

    bool Update(usize index, const Box<double>& bounds) noexcept {
        if (!tree_.TryUpdateBounds(proxies_[index].handle, bounds)) {
            return false;
        }
        proxies_[index].bounds = bounds;
        return true;
    }

    bool SetFilter(usize index, uint32 filter) noexcept {
        if (!tree_.TrySetFilter(proxies_[index].handle, filter)) {
            return false;
        }
        proxies_[index].filter = filter;
        return true;
    }

    // 살아 있는 프록시 하나를 무작위로 고른다. 없으면 IndexNone이다.
    usize PickLive(RandomStream& random) const noexcept {
        if (live_ == 0) {
            return IndexNone;
        }
        usize index = random.NextUInt32Below(static_cast<uint32>(proxies_.Size()));
        while (!proxies_[index].live) {
            index = (index + 1) % proxies_.Size();
        }
        return index;
    }

private:
    LooseOctree tree_;
    Array<OracleProxy> proxies_;
    Array<usize> free_;
    bool reuseIndices_ = false;
    uint32 live_ = 0;
};

inline bool BoxesOverlap(const Box<double>& a, const Box<double>& b) noexcept {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y &&
           a.min.z <= b.max.z && b.min.z <= a.max.z;
}

// 질의 하나를 정답과 대조한 결과다.
struct QueryCheck {
    bool ok = true;
    uint64 exact = 0;
    uint64 results = 0;
    uint64 extra = 0;
};

// 옥트리 박스 질의를 전수 검색과 대조한다. 누락 0건, 중복 0건, 결과의 handle·filter가 모형과 같고,
// 초과 후보는 질의를 tolerance만큼 넓혀야 겹치는 것뿐이어야 한다(float 상대 경계의 보수성, 6.8절).
// 버퍼·존재·개수 질의가 같은 후보 집합을 세는지도 확인한다.
inline QueryCheck CheckBoxQuery(const LooseOctreeReader& reader, const Array<OracleProxy>& proxies,
                                const Box<double>& box, uint32 mask, double tolerance,
                                Allocator& allocator) noexcept {
    QueryCheck check;
    Array<uint8> seen(allocator);
    if (!seen.TryResize(proxies.Size())) {
        check.ok = false;
        return check;
    }
    auto queried = reader.QueryBox(box, mask, [&](const SpatialProxyHit& hit) noexcept {
        ++check.results;
        const uint64 index = hit.userValue;
        if (index >= proxies.Size() || seen[index] != 0) {
            check.ok = false;
            return QueryControl::Continue;
        }
        seen[index] = 1;
        const OracleProxy& proxy = proxies[index];
        if (!proxy.live || !(proxy.handle == hit.handle) || proxy.filter != hit.filterMask ||
            (proxy.filter & mask) == 0) {
            check.ok = false;
        }
        return QueryControl::Continue;
    });
    if (!queried || queried.Value().stopped || queried.Value().visited != check.results) {
        check.ok = false;
        return check;
    }
    const Box<double> widened = box.ExpandedBy(Vec3d::Splat(tolerance));
    for (usize index = 0; index < proxies.Size(); ++index) {
        const OracleProxy& proxy = proxies[index];
        const bool exact =
            proxy.live && (proxy.filter & mask) != 0 && BoxesOverlap(proxy.bounds, box);
        if (exact) {
            ++check.exact;
            if (seen[index] == 0) {
                check.ok = false;
            }
        } else if (seen[index] != 0) {
            ++check.extra;
            if (!BoxesOverlap(proxy.bounds, widened)) {
                check.ok = false;
            }
        }
    }

    auto counted = reader.CountInBox(box, mask);
    auto any = reader.AnyInBox(box, mask);
    if (!counted || counted.Value() != check.results || !any ||
        any.Value() != (check.results != 0)) {
        check.ok = false;
    }
    SpatialProxyHit buffer[4];
    auto into = reader.QueryBoxInto(box, mask, Span<SpatialProxyHit>(buffer, 4));
    if (!into || into.Value().total != check.results ||
        into.Value().written != (check.results < 4 ? check.results : 4) ||
        into.Value().truncated != (check.results > 4)) {
        check.ok = false;
    }
    return check;
}

// 읽기 범위를 잠깐 열어 통계를 얻는다.
inline OctreeStats StatsOf(const LooseOctree& tree) noexcept {
    SpatialReadScope read(tree);
    return read.Reader().Stats();
}

// 열린 읽기 범위 없이 불변 조건을 검사한다.
inline bool ValidateTree(const LooseOctree& tree,
                         OctreeValidationReport* report = nullptr) noexcept {
    SpatialReadScope read(tree);
    auto validated = read.Reader().Validate();
    if (!validated) {
        return false;
    }
    if (report != nullptr) {
        *report = validated.Value();
    }
    return validated.Value().IsValid();
}

// 모형의 살아 있는 모든 프록시가 같은 내용으로 조회되고 수가 같은지다(논리 상태 비교).
inline bool LogicalStateMatches(const LooseOctree& tree,
                                const Array<OracleProxy>& proxies) noexcept {
    SpatialReadScope read(tree);
    const LooseOctreeReader reader = read.Reader();
    uint32 live = 0;
    for (usize index = 0; index < proxies.Size(); ++index) {
        const OracleProxy& proxy = proxies[index];
        if (!proxy.live) {
            if (reader.IsValid(proxy.handle)) {
                return false;
            }
            continue;
        }
        ++live;
        auto got = reader.TryGet(proxy.handle);
        if (!got || !(got.Value().bounds.min == proxy.bounds.min) ||
            !(got.Value().bounds.max == proxy.bounds.max) ||
            got.Value().filterMask != proxy.filter || got.Value().userValue != index) {
            return false;
        }
    }
    return reader.Count() == live;
}

// 시험 입력 분포(9.2절)다.
enum class Distribution : uint8 {
    Uniform,
    Terrain,
    Clustered,
    WideSparse,
    SizeVariance,
    CellBoundary,
    SeparableCrowd,
    OverlappingCrowd
};

inline constexpr Distribution AllDistributions[] = {
    Distribution::Uniform,        Distribution::Terrain,         Distribution::Clustered,
    Distribution::WideSparse,     Distribution::SizeVariance,    Distribution::CellBoundary,
    Distribution::SeparableCrowd, Distribution::OverlappingCrowd};

inline double Uniform(RandomStream& random, double minimum, double maximum) noexcept {
    return minimum + (maximum - minimum) * random.NextDouble01();
}

// extent는 분포의 대표 크기(cm)다. 반환 box는 항상 유효하다.
inline Box<double> MakeBox(Distribution distribution, RandomStream& random,
                           double extent) noexcept {
    Vec3d center;
    double half = 0.0;
    switch (distribution) {
    case Distribution::Uniform:
        center = Vec3d(Uniform(random, -extent, extent), Uniform(random, -extent, extent),
                       Uniform(random, -extent, extent));
        half = Uniform(random, 0.0, extent / 50.0);
        break;
    case Distribution::Terrain:
        center = Vec3d(Uniform(random, -extent, extent), Uniform(random, -extent, extent),
                       Uniform(random, -extent / 100.0, extent / 100.0));
        half = Uniform(random, 0.0, extent / 40.0);
        break;
    case Distribution::Clustered: {
        const double cluster = static_cast<double>(random.NextUInt32Below(4)) - 1.5;
        center = Vec3d(cluster * extent / 2.0 + Uniform(random, -extent / 20.0, extent / 20.0),
                       Uniform(random, -extent / 20.0, extent / 20.0),
                       cluster * extent / 3.0 + Uniform(random, -extent / 20.0, extent / 20.0));
        half = Uniform(random, 0.0, extent / 200.0);
        break;
    }
    case Distribution::WideSparse:
        center = Vec3d(Uniform(random, -extent * 40.0, extent * 40.0),
                       Uniform(random, -extent * 40.0, extent * 40.0),
                       Uniform(random, -extent * 40.0, extent * 40.0));
        half = Uniform(random, 0.0, extent / 10.0);
        break;
    case Distribution::SizeVariance: {
        center = Vec3d(Uniform(random, -extent, extent), Uniform(random, -extent, extent),
                       Uniform(random, -extent, extent));
        const uint32 scale = random.NextUInt32Below(4);
        half = scale == 0   ? 0.0
               : scale == 1 ? Uniform(random, 0.0, extent / 100.0)
               : scale == 2 ? Uniform(random, 0.0, extent / 4.0)
                            : Uniform(random, extent / 2.0, extent * 2.0);
        break;
    }
    case Distribution::CellBoundary: {
        // 2의 거듭제곱 격자선(셀 경계) 바로 위·근처에 중심을 둔다.
        const double grid = 16.0;
        const double snap[3] = {grid * static_cast<double>(random.NextInt32InRange(-8, 8)),
                                grid * static_cast<double>(random.NextInt32InRange(-8, 8)),
                                grid * static_cast<double>(random.NextInt32InRange(-8, 8))};
        const double jitter = random.NextUInt32Below(2) == 0 ? 0.0 : 0.25;
        center = Vec3d(snap[0] + Uniform(random, -jitter, jitter),
                       snap[1] + Uniform(random, -jitter, jitter),
                       snap[2] + Uniform(random, -jitter, jitter));
        half = random.NextUInt32Below(3) == 0 ? 0.0 : Uniform(random, 0.0, 2.0);
        break;
    }
    case Distribution::SeparableCrowd:
        // 아주 좁은 영역의 작은 점들: 분리 level까지 체인 분할된다.
        center = Vec3d(37.0 + Uniform(random, 0.0, 3.0), -11.0 + Uniform(random, 0.0, 3.0),
                       5.0 + Uniform(random, 0.0, 3.0));
        half = Uniform(random, 0.0, 0.01);
        break;
    case Distribution::OverlappingCrowd:
        // 실제로 겹치는 같은 크기 box들: 분리할 수 없어 보류된다.
        center = Vec3d(-20.0 + Uniform(random, 0.0, 0.001), 7.0, 3.0);
        half = 4.0;
        break;
    }
    const Vec3d halfVector = Vec3d::Splat(half);
    return Box<double>(center - halfVector, center + halfVector);
}

// 무작위 질의 box다(작은·중간·큰 질의를 섞는다).
inline Box<double> MakeQuery(RandomStream& random, double extent) noexcept {
    const Vec3d center(Uniform(random, -extent * 1.2, extent * 1.2),
                       Uniform(random, -extent * 1.2, extent * 1.2),
                       Uniform(random, -extent * 1.2, extent * 1.2));
    const uint32 scale = random.NextUInt32Below(3);
    const double half = scale == 0   ? Uniform(random, 0.0, extent / 50.0)
                        : scale == 1 ? Uniform(random, 0.0, extent / 4.0)
                                     : Uniform(random, extent / 2.0, extent * 3.0);
    const Vec3d halfVector(half, half * Uniform(random, 0.2, 1.0),
                           half * Uniform(random, 0.2, 1.0));
    return Box<double>(center - halfVector, center + halfVector);
}

// 루트 level에서 float 상대 경계의 초과 후보 폭 상한(cm)이다. 확장 경계 반 변의 float ULP 몇 배로
// 넉넉히 잡는다(정밀 한계 검증 LO02는 OT3 범위).
inline double ToleranceFor(const LooseOctree& tree, const LooseOctreeConfig& config) noexcept {
    SpatialReadScope read(tree);
    const uint32 rootLevel = read.Reader().Stats().rootLevel;
    double half = config.minCellSize * config.looseness * 0.5;
    for (uint32 level = 0; level < rootLevel; ++level) {
        half *= 2.0;
    }
    return half * 0x1.0p-20 + config.minCellSize * 0x1.0p-20;
}

// ---- 구·볼록 영역·ray 질의(OT3) ----

enum class ShapeKind : uint8 {
    Box,
    Sphere,
    Volume,
    Ray
};

// 시험 질의 하나다. kind에 맞는 필드만 쓴다.
struct ShapeQuery {
    ShapeKind kind = ShapeKind::Box;
    Box<double> box;
    Sphere<double> sphere;
    ConvexVolume<double> volume;
    Ray<double> ray;
    double maxDistance = 0.0;
};

// 정답 술어다. 구·볼록 영역·ray는 Core 술어(Overlap·Raycast)를 정확한 double AABB에 그대로 쓴다
// (6.8절: 프러스텀은 "어느 평면에 대해서도 엄격히 바깥이 아닌 AABB").
inline bool ExactHit(const ShapeQuery& query, const Box<double>& bounds) noexcept {
    switch (query.kind) {
    case ShapeKind::Box:
        return BoxesOverlap(query.box, bounds);
    case ShapeKind::Sphere:
        return Overlap(query.sphere, bounds) == QueryResult::Hit;
    case ShapeKind::Volume:
        return Overlap(query.volume, bounds) == QueryResult::Hit;
    case ShapeKind::Ray:
        return Raycast(query.ray, bounds, query.maxDistance).IsHit();
    }
    return false;
}

template <typename Visitor>
Result<QueryOutcome> RunShape(const LooseOctreeReader& reader, const ShapeQuery& query, uint32 mask,
                              Visitor&& visitor) noexcept {
    switch (query.kind) {
    case ShapeKind::Box:
        return reader.QueryBox(query.box, mask, visitor);
    case ShapeKind::Sphere:
        return reader.QuerySphere(query.sphere, mask, visitor);
    case ShapeKind::Volume:
        return reader.QueryFrustum(query.volume, mask, visitor);
    case ShapeKind::Ray:
        break;
    }
    return reader.QueryRay(query.ray, query.maxDistance, mask, visitor);
}

inline Result<uint64> CountShape(const LooseOctreeReader& reader, const ShapeQuery& query,
                                 uint32 mask) noexcept {
    switch (query.kind) {
    case ShapeKind::Box:
        return reader.CountInBox(query.box, mask);
    case ShapeKind::Sphere:
        return reader.CountInSphere(query.sphere, mask);
    case ShapeKind::Volume:
        return reader.CountInFrustum(query.volume, mask);
    case ShapeKind::Ray:
        break;
    }
    return reader.CountOnRay(query.ray, query.maxDistance, mask);
}

inline Result<bool> AnyShape(const LooseOctreeReader& reader, const ShapeQuery& query,
                             uint32 mask) noexcept {
    switch (query.kind) {
    case ShapeKind::Box:
        return reader.AnyInBox(query.box, mask);
    case ShapeKind::Sphere:
        return reader.AnyInSphere(query.sphere, mask);
    case ShapeKind::Volume:
        return reader.AnyInFrustum(query.volume, mask);
    case ShapeKind::Ray:
        break;
    }
    return reader.AnyOnRay(query.ray, query.maxDistance, mask);
}

inline Result<BufferQueryOutcome> IntoShape(const LooseOctreeReader& reader,
                                            const ShapeQuery& query, uint32 mask,
                                            Span<SpatialProxyHit> out) noexcept {
    switch (query.kind) {
    case ShapeKind::Box:
        return reader.QueryBoxInto(query.box, mask, out);
    case ShapeKind::Sphere:
        return reader.QuerySphereInto(query.sphere, mask, out);
    case ShapeKind::Volume:
        return reader.QueryFrustumInto(query.volume, mask, out);
    case ShapeKind::Ray:
        break;
    }
    return reader.QueryRayInto(query.ray, query.maxDistance, mask, out);
}

// 형상 질의를 전수 검색과 대조한다. 누락 0건·중복 0건이고 handle·필터가 모형과 같아야 한다. 박스는
// boxTolerance 폭의 초과 후보를 허용하고(float 상대 경계), 구·볼록 영역·ray는 초과 후보가 없어야
// 한다(정답과 같은 Core 술어). Into·Any·Count도 같은 후보 수를 세는지 본다.
inline QueryCheck CheckShapeQuery(const LooseOctreeReader& reader,
                                  const Array<OracleProxy>& proxies, const ShapeQuery& query,
                                  uint32 mask, double boxTolerance, Allocator& allocator) noexcept {
    QueryCheck check;
    Array<uint8> seen(allocator);
    if (!seen.TryResize(proxies.Size())) {
        check.ok = false;
        return check;
    }
    auto queried = RunShape(reader, query, mask, [&](const SpatialProxyHit& hit) noexcept {
        ++check.results;
        const uint64 index = hit.userValue;
        if (index >= proxies.Size() || seen[index] != 0) {
            check.ok = false;
            return QueryControl::Continue;
        }
        seen[index] = 1;
        const OracleProxy& proxy = proxies[index];
        if (!proxy.live || !(proxy.handle == hit.handle) || proxy.filter != hit.filterMask ||
            (proxy.filter & mask) == 0) {
            check.ok = false;
        }
        return QueryControl::Continue;
    });
    if (!queried || queried.Value().stopped || queried.Value().visited != check.results) {
        check.ok = false;
        return check;
    }
    const Box<double> widened = query.box.ExpandedBy(Vec3d::Splat(boxTolerance));
    for (usize index = 0; index < proxies.Size(); ++index) {
        const OracleProxy& proxy = proxies[index];
        const bool exact =
            proxy.live && (proxy.filter & mask) != 0 && ExactHit(query, proxy.bounds);
        if (exact) {
            ++check.exact;
            if (seen[index] == 0) {
                check.ok = false;
            }
        } else if (seen[index] != 0) {
            ++check.extra;
            if (query.kind != ShapeKind::Box || !BoxesOverlap(proxy.bounds, widened)) {
                check.ok = false;
            }
        }
    }
    auto counted = CountShape(reader, query, mask);
    auto any = AnyShape(reader, query, mask);
    if (!counted || counted.Value() != check.results || !any ||
        any.Value() != (check.results != 0)) {
        check.ok = false;
    }
    SpatialProxyHit buffer[4];
    auto into = IntoShape(reader, query, mask, Span<SpatialProxyHit>(buffer, 4));
    if (!into || into.Value().total != check.results ||
        into.Value().written != (check.results < 4 ? check.results : 4) ||
        into.Value().truncated != (check.results > 4)) {
        check.ok = false;
    }
    return check;
}

inline Vec3d RandomUnitVector(RandomStream& random) noexcept {
    for (;;) {
        const Vec3d candidate(Uniform(random, -1, 1), Uniform(random, -1, 1),
                              Uniform(random, -1, 1));
        const double lengthSquared = candidate.LengthSquared();
        if (lengthSquared > 1.0e-4 && lengthSquared <= 1.0) {
            auto unit = candidate.TryNormalize();
            if (unit) {
                return unit.Value();
            }
        }
    }
}

// 무작위 질의 형상이다. extent는 분포의 대표 크기(cm)이고 center는 질의가 모이는 곳이다.
inline ShapeQuery MakeShapeQuery(ShapeKind kind, RandomStream& random, double extent,
                                 const Vec3d& center = Vec3d(0, 0, 0)) noexcept {
    ShapeQuery query;
    query.kind = kind;
    const Vec3d focus = center + Vec3d(Uniform(random, -extent * 1.2, extent * 1.2),
                                       Uniform(random, -extent * 1.2, extent * 1.2),
                                       Uniform(random, -extent * 1.2, extent * 1.2));
    const uint32 scale = random.NextUInt32Below(3);
    const double size = scale == 0   ? Uniform(random, 0.0, extent / 50.0)
                        : scale == 1 ? Uniform(random, 0.0, extent / 4.0)
                                     : Uniform(random, extent / 2.0, extent * 3.0);
    switch (kind) {
    case ShapeKind::Box: {
        const Vec3d half(size, size * Uniform(random, 0.2, 1.0), size * Uniform(random, 0.2, 1.0));
        query.box = Box<double>(focus - half, focus + half);
        break;
    }
    case ShapeKind::Sphere:
        query.sphere = Sphere<double>(focus, size);
        break;
    case ShapeKind::Volume: {
        // focus를 안쪽에 두는 평면 4–8개(안쪽 normal, focus에서 평면까지 거리 r_i).
        const uint32 count = 4 + random.NextUInt32Below(5);
        Plane<double> planes[8];
        for (uint32 index = 0; index < count; ++index) {
            const Vec3d normal = RandomUnitVector(random);
            const double margin = Uniform(random, 0.0, size + extent / 100.0);
            planes[index] = Plane<double>(normal, Dot(normal, focus) - margin);
        }
        auto volume = ConvexVolume<double>::TryMake(Span<const Plane<double>>(planes, count));
        if (volume) {
            query.volume = volume.Value();
        }
        break;
    }
    case ShapeKind::Ray: {
        auto ray = Ray<double>::TryMake(focus, RandomUnitVector(random));
        if (ray) {
            query.ray = ray.Value();
        }
        query.maxDistance = random.NextUInt32Below(4) == 0
                                ? platform::PlatformMath::Infinity<double>()
                                : Uniform(random, 0.0, extent * 3.0);
        break;
    }
    }
    return query;
}

// 경계 사례 질의 형상이다. 살아 있는 프록시 하나를 골라 그 꼭짓점·면에 정확히 닿게 만든다.
// - 볼록 영역: 축 평행 normal의 두께 0 slab(마주 보는 두 평면이 같은 좌표), 빈 slab(평면 사이가
//   뒤집힘), 프록시 꼭짓점을 지나는 임의 평면.
// - ray: 축 평행 방향(−0 성분 포함)으로 프록시 면 위를 스치는 ray, maxDistance = 정확히 그 프록시의
//   진입 거리.
// - sphere: 중심이 프록시 꼭짓점이고 반지름 0, 또는 면에서 축 방향으로 정확히 반지름만큼 떨어진
// 중심.
// - box: 프록시 면에 면으로 닿는 box.
// 살아 있는 프록시가 없으면 일반 생성기로 대신한다.
inline ShapeQuery MakeEdgeShapeQuery(ShapeKind kind, RandomStream& random, double extent,
                                     const Array<OracleProxy>& proxies) noexcept {
    usize live = 0;
    for (const OracleProxy& proxy : proxies) {
        live += proxy.live ? 1 : 0;
    }
    if (live == 0) {
        return MakeShapeQuery(kind, random, extent);
    }
    usize pick = random.NextUInt32Below(static_cast<uint32>(proxies.Size()));
    while (!proxies[pick].live) {
        pick = (pick + 1) % proxies.Size();
    }
    const Box<double> target = proxies[pick].bounds;
    const uint32 axisIndex = random.NextUInt32Below(3);
    const auto axisVector = [](uint32 index, double sign) noexcept {
        // 다른 성분은 −0으로 둔다(−0 성분도 유효한 단위 벡터다).
        return Vec3d(index == 0 ? sign : -0.0, index == 1 ? sign : -0.0, index == 2 ? sign : -0.0);
    };
    const double faceLow = target.min[axisIndex];
    const double faceHigh = target.max[axisIndex];
    ShapeQuery query;
    query.kind = kind;
    switch (kind) {
    case ShapeKind::Box: {
        Vec3d minimum = target.max;
        Vec3d maximum = target.max + Vec3d::Splat(Uniform(random, 0, extent / 10.0));
        if (axisIndex == 0) {
            minimum.y = target.min.y;
            minimum.z = target.min.z;
        }
        query.box = Box<double>(minimum, maximum);
        break;
    }
    case ShapeKind::Sphere: {
        if (random.NextUInt32Below(2) == 0) {
            query.sphere = Sphere<double>(target.min, 0.0);
        } else {
            const double radius = static_cast<double>(1 + random.NextUInt32Below(8));
            Vec3d center = target.Center();
            if (axisIndex == 0) {
                center.x = faceHigh + radius;
            } else if (axisIndex == 1) {
                center.y = faceHigh + radius;
            } else {
                center.z = faceHigh + radius;
            }
            query.sphere = Sphere<double>(center, radius);
        }
        break;
    }
    case ShapeKind::Volume: {
        const uint32 variant = random.NextUInt32Below(3);
        Plane<double> planes[2];
        uint32 count = 2;
        if (variant == 0) {
            // 두께 0 slab: 프록시 면 좌표에서 마주 보는 두 평면.
            planes[0] = Plane<double>(axisVector(axisIndex, 1.0), faceLow);
            planes[1] = Plane<double>(axisVector(axisIndex, -1.0), -faceLow);
        } else if (variant == 1) {
            // 빈 slab: 앞 평면이 뒤 평면보다 앞에 있다(술어는 평면별이라 걸친 box는 여전히 후보다).
            planes[0] = Plane<double>(axisVector(axisIndex, 1.0), faceHigh);
            planes[1] = Plane<double>(axisVector(axisIndex, -1.0), -faceLow);
        } else {
            // 프록시 꼭짓점을 지나는 임의 평면 하나.
            const Vec3d normal = RandomUnitVector(random);
            planes[0] = Plane<double>(normal, Dot(normal, target.max));
            count = 1;
        }
        auto volume = ConvexVolume<double>::TryMake(Span<const Plane<double>>(planes, count));
        if (volume) {
            query.volume = volume.Value();
        }
        break;
    }
    case ShapeKind::Ray: {
        // 프록시 면(axisIndex 축의 min 면) 위에서 다른 축 방향으로 스치는 ray, 또는 축 방향으로
        // 들어가 maxDistance가 정확히 진입 거리인 ray.
        const uint32 along = (axisIndex + 1) % 3;
        Vec3d origin = target.Center();
        if (random.NextUInt32Below(2) == 0) {
            if (axisIndex == 0) {
                origin.x = faceLow;
            } else if (axisIndex == 1) {
                origin.y = faceLow;
            } else {
                origin.z = faceLow;
            }
            const double back = target.max[along] - target.min[along] + 5.0;
            if (along == 0) {
                origin.x -= back;
            } else if (along == 1) {
                origin.y -= back;
            } else {
                origin.z -= back;
            }
            query.ray = Ray<double>(origin, axisVector(along, 1.0));
            query.maxDistance = platform::PlatformMath::Infinity<double>();
        } else {
            const double back = Uniform(random, 1.0, extent / 4.0);
            if (axisIndex == 0) {
                origin.x = faceLow - back;
            } else if (axisIndex == 1) {
                origin.y = faceLow - back;
            } else {
                origin.z = faceLow - back;
            }
            query.ray = Ray<double>(origin, axisVector(axisIndex, 1.0));
            query.maxDistance =
                Raycast(query.ray, target, platform::PlatformMath::Infinity<double>()).distance;
        }
        break;
    }
    }
    return query;
}

inline constexpr ShapeKind AllShapeKinds[] = {ShapeKind::Box, ShapeKind::Sphere, ShapeKind::Volume,
                                              ShapeKind::Ray};

} // namespace rocks::spatial::oracle
