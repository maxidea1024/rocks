#include "MeshBuildInternal.h"

#include <Core/Algorithms.h>

// 겹치는 코너 찾기다(Docs/Design/MeshAnimationAssets 5.2절 4단계·5.4절 16, UE
// FStaticMeshOperations::FindOverlappingCorners). 렌더 정점 용접은 이 표의 앞 wedge만 후보로 본다.
namespace rocks::mesh_build {

namespace {

// UE FIndexAndZ의 정렬 키 계수다(StaticMeshOperations.cpp — 축 가중합이라 한 축이 일정한 평면
// 메시도 키가 퍼진다).
constexpr float SortKeyX = 0.30f;
constexpr float SortKeyY = 0.33f;
constexpr float SortKeyZ = 0.37f;

// 정렬 항목이다(같은 키는 wedge 번호 순 — 정렬 결과가 결정적이다).
struct WedgeKey {
    float key = 0.0f;
    uint32 wedge = 0;
};

// UE FIndexAndZ와 같은 식·연산 순서의 키다: 0.30X + 0.33Y + 0.37Z.
float SortKey(const Vec3& point) noexcept {
    return SortKeyX * point.x + SortKeyY * point.y + SortKeyZ * point.z;
}

// 겹치는 쌍 하나다(정렬 뒤 행 번호 순으로 CSR을 만든다).
struct WedgePair {
    uint32 wedge = 0;
    uint32 other = 0;
};

} // namespace

Result<void> FindOverlappingWedges(const MeshSource& mesh, float threshold, Allocator& scratch,
                                   OverlappingWedges& out) noexcept {
    const uint32 triangles = mesh.Count(MeshElement::Triangle);
    const uint32 wedges = triangles * 3;
    const Span<const Vec3> positions = mesh.Positions();
    const Span<const uint32> cornerVertices = mesh.CornerVertices();
    const Span<const MeshTriangleCorners> corners = mesh.TriangleCorners();
    auto position = [&](uint32 wedge) noexcept {
        return positions[cornerVertices[corners[wedge / 3].corners[wedge % 3]]];
    };

    Array<WedgeKey> sorted(scratch);
    if (auto reserved = sorted.TryReserve(wedges); !reserved) {
        return reserved;
    }

    for (uint32 wedge = 0; wedge < wedges; ++wedge) {
        (void)sorted.TryPushBack(WedgeKey{SortKey(position(wedge)), wedge});
    }

    algo::Sort(sorted.View(), [](const WedgeKey& a, const WedgeKey& b) noexcept {
        return a.key < b.key || (a.key == b.key && a.wedge < b.wedge);
    });

    // 양방향 쌍을 모은다(UE와 같이 키 차가 임계값을 넘으면 그 뒤는 볼 필요가 없다).
    Array<WedgePair> pairs(scratch);
    uint64 comparisons = 0;
    for (usize i = 0; i < sorted.Size(); ++i) {
        const Vec3 a = position(sorted[i].wedge);
        for (usize j = i + 1; j < sorted.Size(); ++j) {
            ++comparisons;
            if (math::Abs(sorted[j].key - sorted[i].key) > threshold) {
                break;
            }

            if (!PointsEqual(a, position(sorted[j].wedge), threshold)) {
                continue;
            }

            if (auto pushed = pairs.TryPushBack(WedgePair{sorted[i].wedge, sorted[j].wedge});
                !pushed) {
                return pushed;
            }

            if (auto pushed = pairs.TryPushBack(WedgePair{sorted[j].wedge, sorted[i].wedge});
                !pushed) {
                return pushed;
            }
        }
    }

    algo::Sort(pairs.View(), [](const WedgePair& a, const WedgePair& b) noexcept {
        return a.wedge < b.wedge || (a.wedge == b.wedge && a.other < b.other);
    });

    OverlappingWedges built(scratch);
    if (auto resized = built.starts.TryResize(static_cast<usize>(wedges) + 1); !resized) {
        return resized;
    }

    if (auto resized = built.items.TryResize(pairs.Size()); !resized) {
        return resized;
    }

    usize pair = 0;
    for (uint32 wedge = 0; wedge < wedges; ++wedge) {
        built.starts[wedge] = pair;
        while (pair < pairs.Size() && pairs[pair].wedge == wedge) {
            built.items[pair] = pairs[pair].other;
            ++pair;
        }
    }

    built.starts[wedges] = pair;
    built.comparisons = comparisons;
    out = Move(built);
    return Result<void>::Success();
}

} // namespace rocks::mesh_build
