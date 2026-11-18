#pragma once

#include "BenchInput.h"
#include "FlatScan.h"

#include <Core/Algorithms.h>
#include <Core/Math/RandomStream.h>
#include <Platform/PlatformMath.h>

#include <stdio.h>

// 측정 절차(Docs/Design/SpatialIndex.ko.md 9.3–9.5절)다. 모든 비교군이 같은 절차·입력·순서를 쓴다.
// 데이터 생성과 정답 대조는 타이밍 구간 밖이다.
namespace rocks::spatialbench {

// 규모별 반복 수와 묶음 크기 K다(결과 JSON에 기록한다).
struct Plan {
    uint32 queryCount;
    uint32 queryBatch;
    uint32 mutationCount;
    uint32 mutationBatch;
    uint32 frames;
    uint32 verifyQueries;
};

// 전체 측정 시간을 수십 분 안에 두려고 규모가 클수록 질의·프레임 수를 줄인다. 겹치는 과밀은 모든
// 질의가 프록시 전부를 돌려주므로(결과 출력이 비용을 지배) 10^5 이상에서 더 줄인다.
inline Plan PlanFor(uint32 size, Distribution distribution) noexcept {
    const bool degenerate = distribution == Distribution::OverlapDense && size >= 100000;
    if (size <= 1000) {
        return Plan{512, 32, 1000, 50, 30, 32};
    }
    if (size <= 10000) {
        return Plan{256, 16, 8192, 64, 12, 16};
    }
    if (size <= 100000) {
        return degenerate ? Plan{8, 2, 16384, 64, 2, 2} : Plan{64, 8, 16384, 64, 4, 8};
    }
    return degenerate ? Plan{4, 2, 16384, 64, 2, 1} : Plan{16, 2, 16384, 64, 2, 4};
}

// 측정할 절. 변형 측정(9.4절)은 일부만 켠다.
struct Sections {
    bool allQueries = true;
    bool mutations = true;
    bool treeStates = true;
    bool frames = true;
    bool lightVariants = true;
    bool removeAll = true;
};

// 한 사례(분포 × 규모)의 입력이다.
struct CaseInput {
    Distribution distribution;
    uint32 size;
    uint64 seed;
    Plan plan;
    const Dataset* dataset;
    // 질의 집합(결과 없음·적음·많음)이다.
    const QuerySet* queries[static_cast<uint32>(QueryClass::Count)];
};

// 비교군 성질이다. 박스 계열이 float 경계라 초과 후보가 정상인지, 접촉 경계에서 누락이 설계상
// 생기는지(C의 양의 쪽 엄격 비교), 유지보수를 미루는지.
struct GroupTraits {
    bool conservativeBox = false;
    bool touchMissAllowed = false;
    bool deferredMaintenance = false;
    bool boundedRemoveRow = false;
};

// 결과 비교용 정렬 사본을 만든다.
inline void SortedIds(const ResultBuffer& buffer, Array<uint32>& out) noexcept {
    out.Clear();
    const uint32 count = buffer.count < buffer.capacity ? buffer.count : buffer.capacity;
    for (uint32 index = 0; index < count; ++index) {
        (void)out.TryPushBack(buffer.ids[index]);
    }
    algo::Sort(out.View());
}

// 열린 구간 겹침(접촉만 하는 경우 거짓)이다. C의 접촉 누락을 실제 누락과 구분한다.
inline bool BoxesOverlapStrictly(const Box<double>& a, const Box<double>& b) noexcept {
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

template <bool Counting, typename Index>
uint64 RunQuery(const Index& index, QueryKind kind, const QuerySet& set, uint32 position,
                ResultBuffer& out, QueryCounters& counters) noexcept {
    switch (kind) {
    case QueryKind::Box:
        index.template QueryBox<Counting>(set.boxes[position], QueryFilterMask, out, counters);
        return out.count;
    case QueryKind::Sphere:
        index.template QuerySphere<Counting>(set.spheres[position], QueryFilterMask, out, counters);
        return out.count;
    case QueryKind::Frustum:
        index.template QueryFrustum<Counting>(set.volumes[position], QueryFilterMask, out,
                                              counters);
        return out.count;
    case QueryKind::Ray:
        index.template QueryRay<Counting>(set.rays[position], set.rayLengths[position],
                                          QueryFilterMask, out, counters);
        return out.count;
    case QueryKind::Nearest: {
        uint32 id = 0;
        double distance = 0.0;
        if (index.template QueryNearest<Counting>(set.rays[position], set.rayLengths[position],
                                                  QueryFilterMask, id, distance, counters)) {
            out.Push(id);
            return 1;
        }
        return 0;
    }
    case QueryKind::Any:
        return index.template AnyBox<Counting>(set.boxes[position], QueryFilterMask, counters) ? 1
                                                                                               : 0;
    case QueryKind::CountBox:
        return index.template CountBox<Counting>(set.boxes[position], QueryFilterMask, counters);
    case QueryKind::Count:
        break;
    }
    return 0;
}

// 정답(A1)과 대조한 결과다.
struct VerifyTally {
    uint64 queries = 0;
    uint64 missing = 0;
    uint64 touchOnlyMissing = 0;
    uint64 extra = 0;
    uint64 nearestMismatch = 0;
    uint64 scalarMismatch = 0;
};

// 질의 kind의 앞 count개를 A1과 대조한다. 박스 계열의 float 초과 후보·C의 접촉 누락은 따로 센다.
template <typename Index>
void VerifyKind(const Index& index, const FlatScanDouble& reference, QueryKind kind,
                const QuerySet& set, uint32 count, Span<const Box<double>> current,
                ResultBuffer& left, ResultBuffer& right, Array<uint32>& leftIds,
                Array<uint32>& rightIds, VerifyTally& tally) noexcept {
    QueryCounters ignored;
    for (uint32 position = 0; position < count && position < set.boxes.Size(); ++position) {
        ++tally.queries;
        left.Reset();
        right.Reset();
        if (kind == QueryKind::Nearest) {
            uint32 expectedId = 0;
            double expectedDistance = 0.0;
            uint32 actualId = 0;
            double actualDistance = 0.0;
            const bool expected = reference.QueryNearest<false>(
                set.rays[position], set.rayLengths[position], QueryFilterMask, expectedId,
                expectedDistance, ignored);
            const bool actual = index.template QueryNearest<false>(
                set.rays[position], set.rayLengths[position], QueryFilterMask, actualId,
                actualDistance, ignored);
            // 같은 거리의 동률은 다른 식별 값이 답일 수 있으므로 거리만 비교한다.
            if (expected != actual || (expected && expectedDistance != actualDistance)) {
                ++tally.nearestMismatch;
            }
            continue;
        }
        if (kind == QueryKind::Any || kind == QueryKind::CountBox) {
            const uint64 expected = RunQuery<false>(reference, kind, set, position, left, ignored);
            const uint64 actual = RunQuery<false>(index, kind, set, position, right, ignored);
            if (expected != actual) {
                ++tally.scalarMismatch;
            }
            continue;
        }
        (void)RunQuery<false>(reference, kind, set, position, left, ignored);
        (void)RunQuery<false>(index, kind, set, position, right, ignored);
        SortedIds(left, leftIds);
        SortedIds(right, rightIds);
        usize a = 0;
        usize b = 0;
        while (a < leftIds.Size() || b < rightIds.Size()) {
            if (b == rightIds.Size() || (a < leftIds.Size() && leftIds[a] < rightIds[b])) {
                const uint32 id = leftIds[a++];
                if (kind == QueryKind::Box &&
                    !BoxesOverlapStrictly(current[id], set.boxes[position])) {
                    ++tally.touchOnlyMissing;
                } else {
                    ++tally.missing;
                }
            } else if (a == leftIds.Size() || rightIds[b] < leftIds[a]) {
                ++tally.extra;
                ++b;
            } else {
                ++a;
                ++b;
            }
        }
    }
}

} // namespace rocks::spatialbench
