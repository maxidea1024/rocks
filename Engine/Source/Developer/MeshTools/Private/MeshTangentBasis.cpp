#include "MeshBuildInternal.h"

#include <Core/Algorithms.h>
#include <DeterministicMath/DeterministicMath.h>

// 삼각형·코너 법선과 tangent다(Docs/Design/MeshAnimationAssets 5.2절 3·5단계). 식과 임계값은
// UE StaticMeshOperations.cpp의
// GetTriangleTangentsAndNormalsWithUV·WithNoUVs·ComputeTangentsAndNormals와 같다. 합산 순서는
// 삼각형 번호 오름차순으로 고정한다(UE는 해시 표 순서라 bit 일치는 보장하지 않는다 — 5.2절 결정성).
namespace rocks::mesh_build {

namespace {

// UE CreateOrthonormalBasis: x·y를 z에 수직인 평면으로 옮기고, z와 평행해 길이를 잃으면 다른 축과의
// 외적으로 대신한 뒤 셋을 정규화한다.
void CreateOrthonormalBasis(Vec3& x, Vec3& y, Vec3& z) noexcept {
    const float zz = Dot3(z, z);
    x -= z * (Dot3(x, z) / zz);
    y -= z * (Dot3(y, z) / zz);
    if (SquareSum(x) < UeDelta * UeDelta) {
        x = Cross(y, z);
    }

    if (SquareSum(y) < UeDelta * UeDelta) {
        y = Cross(x, z);
    }

    NormalizeInPlace(x);
    NormalizeInPlace(y);
    NormalizeInPlace(z);
}

struct TriangleBasis {
    Vec3 normal{};
    Vec3 tangent{};
    Vec3 binormal{};
};

// UE GetTriangleTangentsAndNormalsWithUV. UV 행렬 [dUV1; dUV2]의 역을 위치 차에 곱해 UV 방향을
// 구한다 (UE는 4×4 역행렬 — 행렬식이 0이거나 비유한이면 단위 행렬로 대신하므로 그때는 위치 차
// 그대로다).
TriangleBasis BasisWithUv(const Vec3 (&positions)[3], const Vec2 (&uvs)[3],
                          float threshold) noexcept {
    const float squareThreshold = math::Max(threshold * threshold, UeMinFloat);
    const Vec3 delta1 = positions[1] - positions[0];
    const Vec3 delta2 = positions[2] - positions[0];
    const float du1 = uvs[1].x - uvs[0].x;
    const float dv1 = uvs[1].y - uvs[0].y;
    const float du2 = uvs[2].x - uvs[0].x;
    const float dv2 = uvs[2].y - uvs[0].y;
    // 왼손 좌표계에 반시계 winding이라 외적 순서를 뒤집는다(UE 주석과 같다).
    Vec3 normal = SafeNormal(Cross(delta2, delta1), squareThreshold);
    if (IsNearlyZero(normal, threshold)) {
        return {};
    }

    Vec3 tangentAxis = delta1;
    Vec3 binormalAxis = delta2;
    const float determinant = du1 * dv2 - dv1 * du2;
    if (determinant != 0.0f && math::IsFinite(determinant)) {
        const float inverse = 1.0f / determinant;
        tangentAxis = (delta1 * dv2 - delta2 * dv1) * inverse;
        binormalAxis = (delta2 * du1 - delta1 * du2) * inverse;
    }

    Vec3 tangent = SafeNormal(tangentAxis);
    Vec3 binormal = SafeNormal(binormalAxis);
    CreateOrthonormalBasis(tangent, binormal, normal);
    if (IsNearlyZero(tangent, UeKindaSmallNumber) || ContainsNaN(tangent) ||
        IsNearlyZero(binormal, UeKindaSmallNumber) || ContainsNaN(binormal)) {
        tangent = Vec3(0.0f, 0.0f, 0.0f);
        binormal = Vec3(0.0f, 0.0f, 0.0f);
    }

    if (IsNearlyZero(normal, UeKindaSmallNumber) || ContainsNaN(normal)) {
        normal = Vec3(0.0f, 0.0f, 0.0f);
    }

    return {SafeNormal(normal), SafeNormal(tangent), SafeNormal(binormal)};
}

// UE GetTriangleTangentsAndNormalsWithNoUVs: 면 법선 하나로 Duff·Frisvad(JCGT 2017) 기저를 만든다.
TriangleBasis BasisWithoutUv(const Vec3 (&positions)[3], float threshold) noexcept {
    const float squareThreshold = math::Max(threshold * threshold, UeMinFloat);
    const Vec3 delta1 = positions[1] - positions[0];
    const Vec3 delta2 = positions[2] - positions[0];
    Vec3 normal = SafeNormal(Cross(delta2, delta1), squareThreshold);
    if (!NormalizeInPlace(normal, threshold)) {
        return {};
    }

    TriangleBasis basis;
    basis.normal = normal;
    if (normal.z < 0.0f) {
        const float a = 1.0f / (1.0f - normal.z);
        const float b = normal.x * normal.y * a;
        basis.tangent = Vec3(1.0f - normal.x * normal.x * a, -b, normal.x);
        basis.binormal = Vec3(b, normal.y * normal.y * a - 1.0f, -normal.y);
    } else {
        const float a = 1.0f / (1.0f + normal.z);
        const float b = -normal.x * normal.y * a;
        basis.tangent = Vec3(1.0f - normal.x * normal.x * a, b, -normal.x);
        basis.binormal = Vec3(b, 1.0f - normal.y * normal.y * a, -normal.y);
    }

    return basis;
}

// 삼각형 하나의 정점 정보다(5단계 정점 루프의 지역 표).
struct VertexTriangle {
    uint32 triangle = 0;
    // 이 정점을 가리키는 삼각형의 첫 코너와 그 칸(0–2)이다(UE VertexInfo.VertexInstanceID).
    uint32 corner = 0;
    uint32 slot = 0;
};

// 정점에서 나가는 삼각형 변 하나다(other = 다른 끝 정점, local = 지역 삼각형 번호).
struct VertexSide {
    uint32 other = 0;
    uint32 local = 0;
};

// UV0 값으로 묶는 항목이다(-0은 +0으로 바꿔 둔다 — UE 해시 키 규칙).
struct UvEntry {
    float u = 0.0f;
    float v = 0.0f;
    uint32 local = 0;
};

uint32 FindRoot(Span<uint32> parent, uint32 item) noexcept {
    while (parent[item] != item) {
        parent[item] = parent[parent[item]];
        item = parent[item];
    }

    return item;
}

// 작은 번호를 뿌리로 남긴다(묶음 번호가 삼각형 순서로 정해진다).
void Unite(Span<uint32> parent, uint32 a, uint32 b) noexcept {
    const uint32 rootA = FindRoot(parent, a);
    const uint32 rootB = FindRoot(parent, b);
    if (rootA < rootB) {
        parent[rootB] = rootA;
    } else if (rootB < rootA) {
        parent[rootA] = rootB;
    }
}

float PositiveZero(float value) noexcept {
    return value == 0.0f ? 0.0f : value;
}

// 정점 루프가 다시 쓰는 임시 배열이다(정점마다 Clear하고 용량은 유지한다).
struct VertexScratch {
    explicit VertexScratch(Allocator& allocator) noexcept
        : triangles(allocator), sides(allocator), parent(allocator), order(allocator),
          uvs(allocator), uvGroupOf(allocator), groupTangents(allocator),
          groupBinormals(allocator) {
    }

    Array<VertexTriangle> triangles;
    Array<VertexSide> sides;
    Array<uint32> parent;
    Array<uint32> order;
    Array<UvEntry> uvs;
    Array<uint32> uvGroupOf;
    Array<Vec3> groupTangents;
    Array<Vec3> groupBinormals;
};

// 코너 각과 면적(가중 법선)이다. angles[3t + k]는 삼각형 t의 칸 k 각이다.
struct TriangleWeights {
    explicit TriangleWeights(Allocator& allocator) noexcept : areas(allocator), angles(allocator) {
    }

    Array<float> areas;
    Array<float> angles;
};

// UE TriangleUtilities::ComputeTriangleCornerAngle: 두 변을 정규화(실패하면 0)한 내적의 arccos.
float CornerAngle(const Vec3& a, const Vec3& b, const Vec3& c) noexcept {
    Vec3 edge1 = b - a;
    Vec3 edge2 = c - a;
    if (!NormalizeInPlace(edge1) || !NormalizeInPlace(edge2)) {
        return 0.0f;
    }

    const float cosine = math::Clamp(Dot3(edge1, edge2), -CosineLimit, CosineLimit);
    return static_cast<float>(deterministic::Acos(static_cast<double>(cosine)));
}

Result<void> ComputeWeights(const MeshSource& mesh, TriangleWeights& weights) noexcept {
    const uint32 triangles = mesh.Count(MeshElement::Triangle);
    if (auto reserved = weights.areas.TryResize(triangles); !reserved) {
        return reserved;
    }

    if (auto reserved = weights.angles.TryResize(static_cast<usize>(triangles) * 3); !reserved) {
        return reserved;
    }

    const Span<const Vec3> positions = mesh.Positions();
    const Span<const uint32> cornerVertices = mesh.CornerVertices();
    const Span<const MeshTriangleCorners> corners = mesh.TriangleCorners();
    for (uint32 triangle = 0; triangle < triangles; ++triangle) {
        const Vec3 a = positions[cornerVertices[corners[triangle].corners[0]]];
        const Vec3 b = positions[cornerVertices[corners[triangle].corners[1]]];
        const Vec3 c = positions[cornerVertices[corners[triangle].corners[2]]];
        weights.areas[triangle] = math::Sqrt(SquareSum(Cross(b - a, c - a))) / 2.0f;
        weights.angles[3 * triangle] = CornerAngle(a, b, c);
        weights.angles[3 * triangle + 1] = CornerAngle(b, c, a);
        weights.angles[3 * triangle + 2] = CornerAngle(c, a, b);
    }

    return Result<void>::Success();
}

// 정점 v의 코너가 모두 유효한 법선·tangent·쌍법선을 갖는지다(UE bPointHasAllTangents).
bool HasAllTangents(const CornerBasis& basis, Span<const VertexTriangle> triangles) noexcept {
    for (const VertexTriangle& entry : triangles) {
        const Vec3 normal = basis.Normal(entry.corner);
        const Vec3 tangent = basis.Tangent(entry.corner);
        if (IsNearlyZero(normal, UeKindaSmallNumber) || IsNearlyZero(tangent, UeKindaSmallNumber)) {
            return false;
        }

        const Vec3 x = SafeNormal(tangent);
        const Vec3 z = SafeNormal(normal);
        const Vec3 y = SafeNormal(SafeNormal(Cross(z, x)) * basis.signs[entry.corner]);
        // UE는 x·y·z의 NaN과 0 벡터를 보지만 여기 입력은 값 정리로 유한하고 유한한 벡터의
        // SafeNormal은 유한하다(제곱합이 넘치면 0 벡터). x·z는 단위 벡터 또는 0 벡터이고 어느 쪽이
        // 0이면 y도 0이라 y 하나로 같은 판정이 된다.
        if (IsNearlyZero(y, UeSmallNumber)) {
            return false;
        }
    }

    return true;
}

// 정점 vertex를 쓰는 삼각형을 모은다(삼각형 번호 오름차순, 중복 없음).
Result<void> CollectVertexTriangles(const MeshSource& mesh, const MeshTopologyIndex& topology,
                                    uint32 vertex, Array<VertexTriangle>& out) noexcept {
    out.Clear();
    for (uint32 corner : topology.VertexCorners(vertex)) {
        for (uint32 triangle : topology.CornerTriangles(corner)) {
            if (auto pushed = out.TryPushBack(VertexTriangle{triangle, 0, 0}); !pushed) {
                return pushed;
            }
        }
    }

    algo::Sort(out.View(), [](const VertexTriangle& a, const VertexTriangle& b) noexcept {
        return a.triangle < b.triangle;
    });
    usize unique = 0;
    const Span<const uint32> cornerVertices = mesh.CornerVertices();
    for (usize index = 0; index < out.Size(); ++index) {
        if (unique != 0 && out[unique - 1].triangle == out[index].triangle) {
            continue;
        }

        VertexTriangle entry = out[index];
        const MeshTriangleCorners& corners = mesh.TriangleCorners()[entry.triangle];
        uint32 slot = 0;
        while (cornerVertices[corners.corners[slot]] != vertex) {
            ++slot;
        }

        entry.corner = corners.corners[slot];
        entry.slot = slot;
        out[unique++] = entry;
    }

    return out.TryResize(unique);
}

// 정점 vertex의 삼각형을 soft 변으로 이은 묶음으로 나눈다. 결과 order는 (묶음의 첫 삼각형, 삼각형)
// 순으로 지역 번호를 담고, 같은 묶음은 연속한다(parent의 뿌리가 같다).
Result<void> GroupVertexTriangles(const MeshSource& mesh, const MeshTopologyIndex& topology,
                                  Span<const uint8> hardness, uint32 vertex,
                                  VertexScratch& scratch) noexcept {
    const uint32 count = static_cast<uint32>(scratch.triangles.Size());
    scratch.sides.Clear();
    const Span<const uint32> cornerVertices = mesh.CornerVertices();
    for (uint32 local = 0; local < count; ++local) {
        const MeshTriangleCorners& corners =
            mesh.TriangleCorners()[scratch.triangles[local].triangle];
        uint32 previous = vertex;
        for (uint32 slot = 0; slot < 3; ++slot) {
            const uint32 other = cornerVertices[corners.corners[slot]];
            if (other == vertex || other == previous) {
                continue;
            }

            if (auto pushed = scratch.sides.TryPushBack(VertexSide{other, local}); !pushed) {
                return pushed;
            }

            previous = other;
        }
    }

    algo::Sort(scratch.sides.View(), [](const VertexSide& a, const VertexSide& b) noexcept {
        return a.other < b.other || (a.other == b.other && a.local < b.local);
    });
    if (auto resized = scratch.parent.TryResize(count); !resized) {
        return resized;
    }

    for (uint32 local = 0; local < count; ++local) {
        scratch.parent[local] = local;
    }

    for (usize begin = 0; begin < scratch.sides.Size();) {
        usize end = begin + 1;
        while (end < scratch.sides.Size() &&
               scratch.sides[end].other == scratch.sides[begin].other) {
            ++end;
        }

        // 변 목록에 없는 쌍은 다각형 안 대각선이라 soft다(UE는 다각형 내부 변을 soft로 만든다).
        const uint32 edge = topology.FindEdge(vertex, scratch.sides[begin].other);
        const bool hard = edge != MeshNoIndex && !hardness.IsEmpty() && hardness[edge] != 0;
        if (!hard) {
            for (usize item = begin + 1; item < end; ++item) {
                Unite(scratch.parent.View(), scratch.sides[begin].local, scratch.sides[item].local);
            }
        }

        begin = end;
    }

    if (auto resized = scratch.order.TryResize(count); !resized) {
        return resized;
    }

    for (uint32 local = 0; local < count; ++local) {
        scratch.order[local] = local;
    }

    // 뿌리 = 묶음의 가장 작은 지역 번호(Unite가 작은 쪽을 남긴다)라 (뿌리, 번호) 순이 원하는
    // 순서다.
    Span<uint32> parent = scratch.parent.View();
    for (uint32 local = 0; local < count; ++local) {
        (void)FindRoot(parent, local);
    }

    algo::Sort(scratch.order.View(), [parent](uint32 a, uint32 b) noexcept {
        return parent[a] < parent[b] || (parent[a] == parent[b] && a < b);
    });
    return Result<void>::Success();
}

// 묶음 [begin, end)(order 안)의 UV0 값별 묶음을 만든다. uvGroupOf[local]이 그 값 묶음 번호다.
Result<void> GroupByUv(const CornerBasis& basis, VertexScratch& scratch, usize begin,
                       usize end) noexcept {
    scratch.uvs.Clear();
    for (usize item = begin; item < end; ++item) {
        const uint32 local = scratch.order[item];
        const Vec2 uv = basis.Uv(scratch.triangles[local].corner);
        if (auto pushed =
                scratch.uvs.TryPushBack(UvEntry{PositiveZero(uv.x), PositiveZero(uv.y), local});
            !pushed) {
            return pushed;
        }
    }

    algo::Sort(scratch.uvs.View(), [](const UvEntry& a, const UvEntry& b) noexcept {
        if (a.u != b.u) {
            return a.u < b.u;
        }

        if (a.v != b.v) {
            return a.v < b.v;
        }

        return a.local < b.local;
    });
    if (auto resized = scratch.uvGroupOf.TryResize(scratch.triangles.Size()); !resized) {
        return resized;
    }

    uint32 groups = 0;
    for (usize item = 0; item < scratch.uvs.Size(); ++item) {
        if (item != 0 && (scratch.uvs[item].u != scratch.uvs[item - 1].u ||
                          scratch.uvs[item].v != scratch.uvs[item - 1].v)) {
            ++groups;
        }

        scratch.uvGroupOf[scratch.uvs[item].local] = groups;
    }

    // 묶음에는 삼각형이 하나 이상 있어 uvs가 비지 않는다.
    const usize groupCount = static_cast<usize>(groups) + 1;
    if (auto resized = scratch.groupTangents.TryResize(groupCount); !resized) {
        return resized;
    }

    if (auto resized = scratch.groupBinormals.TryResize(groupCount); !resized) {
        return resized;
    }

    for (usize group = 0; group < groupCount; ++group) {
        scratch.groupTangents[group] = Vec3(0.0f, 0.0f, 0.0f);
        scratch.groupBinormals[group] = Vec3(0.0f, 0.0f, 0.0f);
    }

    return Result<void>::Success();
}

// 묶음 하나의 평균을 그 코너들에 쓴다(UE ComputeTangentsAndNormals의 묶음 적용부).
void ApplyGroup(const TriangleBases& triangles, const TriangleWeights* weights,
                const CornerBasisOptions& options, VertexScratch& scratch, usize begin, usize end,
                CornerBasis& basis) noexcept {
    Vec3 groupNormal(0.0f, 0.0f, 0.0f);
    for (usize item = begin; item < end; ++item) {
        const VertexTriangle& entry = scratch.triangles[scratch.order[item]];
        float weight = 1.0f;
        if (weights != nullptr) {
            weight =
                weights->areas[entry.triangle] * weights->angles[3 * entry.triangle + entry.slot];
        }

        const Vec3 normal = triangles.normals[entry.triangle] * weight;
        const Vec3 tangent = triangles.tangents[entry.triangle] * weight;
        const Vec3 binormal = triangles.binormals[entry.triangle] * weight;
        if (!IsNearlyZero(normal, UeSmallNumber) && !ContainsNaN(normal)) {
            groupNormal += normal;
        }

        if (!options.mikkTSpace) {
            const uint32 uvGroup = scratch.uvGroupOf[scratch.order[item]];
            if (!IsNearlyZero(tangent, UeSmallNumber) && !ContainsNaN(tangent)) {
                scratch.groupTangents[uvGroup] += tangent;
            }

            if (!IsNearlyZero(binormal, UeSmallNumber) && !ContainsNaN(binormal)) {
                scratch.groupBinormals[uvGroup] += binormal;
            }
        }
    }

    NormalizeInPlace(groupNormal);
    if (!options.mikkTSpace) {
        for (Vec3& value : scratch.groupTangents) {
            NormalizeInPlace(value);
        }

        for (Vec3& value : scratch.groupBinormals) {
            NormalizeInPlace(value);
        }
    }

    for (usize item = begin; item < end; ++item) {
        const uint32 local = scratch.order[item];
        const uint32 corner = scratch.triangles[local].corner;
        if (IsNearlyZero(basis.Normal(corner), UeSmallNumber)) {
            basis.SetNormal(corner, FiniteOrZero(groupNormal));
        }

        if (options.mikkTSpace) {
            continue;
        }

        Vec3 tangent = scratch.groupTangents[scratch.uvGroupOf[local]];
        Vec3 binormal = scratch.groupBinormals[scratch.uvGroupOf[local]];
        const Vec3 current = basis.Tangent(corner);
        if (!IsNearlyZero(current, UeSmallNumber)) {
            tangent = current;
        }

        const Vec3 normal = basis.Normal(corner);
        Vec3 derived(0.0f, 0.0f, 0.0f);
        if (!IsNearlyZero(normal, UeSmallNumber) && !IsNearlyZero(current, UeSmallNumber)) {
            derived = SafeNormal(Cross(normal, current)) * basis.signs[corner];
        }

        if (!IsNearlyZero(derived, UeSmallNumber)) {
            binormal = derived;
        }

        // Gram-Schmidt(UE와 같은 순서).
        binormal -= tangent * Dot3(tangent, binormal);
        NormalizeInPlace(binormal);
        tangent -= normal * Dot3(normal, tangent);
        NormalizeInPlace(tangent);
        binormal -= normal * Dot3(normal, binormal);
        NormalizeInPlace(binormal);
        const Vec3 finiteTangent = FiniteOrZero(tangent);
        basis.SetTangent(corner, finiteTangent);
        basis.signs[corner] = BasisDeterminantSign(finiteTangent, FiniteOrZero(binormal), normal);
    }
}

} // namespace

Result<void> ComputeTriangleBases(const MeshSource& mesh, const CornerBasis& basis, float threshold,
                                  TriangleBases& out) noexcept {
    const uint32 triangles = mesh.Count(MeshElement::Triangle);
    if (auto resized = out.normals.TryResize(triangles); !resized) {
        return resized;
    }

    if (auto resized = out.tangents.TryResize(triangles); !resized) {
        return resized;
    }

    if (auto resized = out.binormals.TryResize(triangles); !resized) {
        return resized;
    }

    const Span<const Vec3> positions = mesh.Positions();
    const Span<const uint32> cornerVertices = mesh.CornerVertices();
    const Span<const MeshTriangleCorners> corners = mesh.TriangleCorners();
    for (uint32 triangle = 0; triangle < triangles; ++triangle) {
        Vec3 points[3];
        Vec2 uvs[3];
        for (uint32 slot = 0; slot < 3; ++slot) {
            const uint32 corner = corners[triangle].corners[slot];
            points[slot] = positions[cornerVertices[corner]];
            uvs[slot] = basis.Uv(corner);
        }

        const TriangleBasis value = basis.uv0.IsEmpty() ? BasisWithoutUv(points, threshold)
                                                        : BasisWithUv(points, uvs, threshold);
        // 아주 큰 좌표의 넘침(비유한)은 퇴화와 같이 0으로 둔다(결과 bit가 호스트 NaN에 기대지
        // 않게).
        out.normals[triangle] = FiniteOrZero(value.normal);
        out.tangents[triangle] = FiniteOrZero(value.tangent);
        out.binormals[triangle] = FiniteOrZero(value.binormal);
    }

    return Result<void>::Success();
}

Result<void> ComputeCornerBases(const MeshSource& mesh, const MeshTopologyIndex& topology,
                                Span<const uint8> hardness, const TriangleBases& triangles,
                                const CornerBasisOptions& options, Allocator& scratch,
                                CornerBasis& basis) noexcept {
    const uint32 corners = mesh.Count(MeshElement::Corner);
    for (uint32 corner = 0; corner < corners; ++corner) {
        if (options.forceNormals) {
            basis.SetNormal(corner, Vec3(0.0f, 0.0f, 0.0f));
        }

        if (options.forceTangents) {
            basis.SetTangent(corner, Vec3(0.0f, 0.0f, 0.0f));
            basis.signs[corner] = 0.0f;
        }
    }

    if (mesh.Count(MeshElement::Triangle) == 0) {
        return Result<void>::Success();
    }

    TriangleWeights weights(scratch);
    if (options.weighted) {
        if (auto computed = ComputeWeights(mesh, weights); !computed) {
            return computed;
        }
    }

    VertexScratch work(scratch);
    const uint32 vertices = mesh.Count(MeshElement::Vertex);
    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        if (auto collected = CollectVertexTriangles(mesh, topology, vertex, work.triangles);
            !collected) {
            return collected;
        }

        if (HasAllTangents(basis, work.triangles.View())) {
            continue;
        }

        if (auto grouped = GroupVertexTriangles(mesh, topology, hardness, vertex, work); !grouped) {
            return grouped;
        }

        const Span<const uint32> parent = work.parent.View();
        for (usize begin = 0; begin < work.order.Size();) {
            usize end = begin + 1;
            while (end < work.order.Size() &&
                   parent[work.order[end]] == parent[work.order[begin]]) {
                ++end;
            }

            if (!options.mikkTSpace) {
                if (auto grouped = GroupByUv(basis, work, begin, end); !grouped) {
                    return grouped;
                }
            }

            ApplyGroup(triangles, options.weighted ? &weights : nullptr, options, work, begin, end,
                       basis);
            begin = end;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::mesh_build
