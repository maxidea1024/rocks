#include "MeshBuildInternal.h"

#include <Core/Algorithms.h>

// 겹치는 코너 찾기다(Docs/Design/MeshAnimationAssets.ko.md 5.2절 4단계, UE
// FStaticMeshOperations::FindOverlappingCorners). 렌더 정점 용접은 이 표의 앞 wedge만 후보로 본다.
namespace rocks::mesh_build {

namespace {

// z 정렬 항목이다(같은 z는 wedge 번호 순 — 정렬 결과가 결정적이다).
struct WedgeZ {
    float z = 0.0f;
    uint32 wedge = 0;
};

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

    Array<WedgeZ> sorted(scratch);
    if (auto reserved = sorted.TryReserve(wedges); !reserved) {
        return reserved;
    }

    for (uint32 wedge = 0; wedge < wedges; ++wedge) {
        (void)sorted.TryPushBack(WedgeZ{position(wedge).z, wedge});
    }

    algo::Sort(sorted.View(), [](const WedgeZ& a, const WedgeZ& b) noexcept {
        return a.z < b.z || (a.z == b.z && a.wedge < b.wedge);
    });

    // 양방향 쌍을 모은다(UE와 같이 z 차가 임계값을 넘으면 그 뒤는 볼 필요가 없다).
    Array<WedgePair> pairs(scratch);
    for (usize i = 0; i < sorted.Size(); ++i) {
        const Vec3 a = position(sorted[i].wedge);
        for (usize j = i + 1; j < sorted.Size(); ++j) {
            if (math::Abs(sorted[j].z - sorted[i].z) > threshold) {
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
    out = Move(built);
    return Result<void>::Success();
}

} // namespace rocks::mesh_build
