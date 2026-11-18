#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/Allocator.h>
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

} // namespace rocks::spatial::oracle
