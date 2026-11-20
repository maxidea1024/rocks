#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/MathUtility.h>
#include <Core/Memory/SystemAllocator.h>
#include <MeshSource/MeshSourceValidation.h>
#include <MeshTools/StaticMeshBuilder.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// MeshTools 빌드 시험 공용 도우미다(Docs/Design/MeshAnimationAssets.ko.md 12절 "빌드"): 다각형
// 목록으로 메시 만들기, 코너 속성 채우기, 빌드 결과의 정규 byte(결정성 비교), 렌더 정점 찾기.
namespace rocks::mesh_tools_test {

// 다각형 하나다(정점 번호 목록, 묶음 번호). 삼각 분할은 0번 꼭짓점 부채꼴이다.
struct PolygonSpec {
    Span<const uint32> vertices;
    uint32 group = 0;
};

// positions·polygons로 메시를 만든다. shareCorners면 정점마다 코너 하나를 모든 다각형이 공유하고,
// 아니면 다각형마다 새 코너를 만든다. 둘레 변은 처음 나올 때 더한다(같은 정점 쌍은 건너뜀).
inline bool BuildPolygonMesh(MeshSource& mesh, Span<const Vec3> positions,
                             Span<const PolygonSpec> polygons, uint32 groups,
                             bool shareCorners) noexcept {
    for (const Vec3& position : positions) {
        if (!mesh.AddVertex(position)) {
            return false;
        }
    }

    for (uint32 group = 0; group < groups; ++group) {
        if (!mesh.AddPolygonGroup()) {
            return false;
        }
    }

    if (shareCorners) {
        for (uint32 vertex = 0; vertex < positions.Size(); ++vertex) {
            if (!mesh.AddCorner(vertex)) {
                return false;
            }
        }
    }

    for (const PolygonSpec& polygon : polygons) {
        uint32 corners[16] = {};
        const usize sides = polygon.vertices.Size();
        if (sides > 16) {
            return false;
        }

        for (usize side = 0; side < sides; ++side) {
            if (shareCorners) {
                corners[side] = polygon.vertices[side];
                continue;
            }

            auto corner = mesh.AddCorner(polygon.vertices[side]);
            if (!corner) {
                return false;
            }

            corners[side] = corner.Value();
        }

        for (usize side = 0; side < sides; ++side) {
            const uint32 a = polygon.vertices[side];
            const uint32 b = polygon.vertices[(side + 1) % sides];
            bool known = a == b;
            for (const MeshEdgeVertices& edge : mesh.Edges()) {
                known = known || (edge.first == (a < b ? a : b) && edge.second == (a < b ? b : a));
            }

            if (!known && !mesh.AddEdge(a, b)) {
                return false;
            }
        }

        auto added = mesh.AddPolygon(Span<const uint32>(corners, sides), polygon.group);
        if (!added) {
            return false;
        }

        for (usize fan = 1; fan + 1 < sides; ++fan) {
            if (!mesh.AddTriangle(corners[0], corners[fan], corners[fan + 1], added.Value())) {
                return false;
            }
        }
    }

    return ValidateMeshSource(mesh).IsOk();
}

// 코너 속성을 더하고 번호를 돌려준다(없던 것만 — 실패하면 MeshNoAttribute).
inline uint32 CornerAttribute(MeshSource& mesh, StringView name, MeshValueType type,
                              uint8 channels = 1) noexcept {
    const uint32 found = mesh.FindAttribute(MeshElement::Corner, name);
    if (found != MeshNoAttribute) {
        return found;
    }

    auto added = mesh.AddAttribute(MeshElement::Corner, name, type, channels);
    return added ? added.Value() : MeshNoAttribute;
}

// 코너 UV 채널 0을 정점 위치의 (x, y) / scale로 채운다(평면 매핑).
inline uint32 PlanarUvs(MeshSource& mesh, float scale, uint8 channels = 1) noexcept {
    const uint32 uv =
        CornerAttribute(mesh, MeshAttributeName::TexCoord, MeshValueType::Vec2, channels);
    if (uv == MeshNoAttribute) {
        return uv;
    }

    for (uint32 corner = 0; corner < mesh.Count(MeshElement::Corner); ++corner) {
        const Vec3 position = mesh.Positions()[mesh.CornerVertices()[corner]];
        for (uint32 channel = 0; channel < channels; ++channel) {
            mesh.SetVec2(
                uv, channel, corner,
                Vec2(position.x / scale + static_cast<float>(channel), position.y / scale));
        }
    }

    return uv;
}

// 다각형 묶음의 MaterialSlotName을 쓴다.
inline bool SlotNames(MeshSource& mesh, Span<const StringView> names) noexcept {
    auto added = mesh.AddAttribute(MeshElement::PolygonGroup, MeshAttributeName::MaterialSlotName,
                                   MeshValueType::Name, 1);
    if (!added) {
        return false;
    }

    for (uint32 group = 0; group < names.Size(); ++group) {
        if (!mesh.SetNameValue(added.Value(), 0, group, names[group])) {
            return false;
        }
    }

    return true;
}

// 축 정렬 정육면체(한 변 size, 원점 중심): 정점 8, 면마다 새 코너(24), 면마다 UV 평면 매핑, 묶음
// 하나.
inline bool BuildCube(MeshSource& mesh, float size) noexcept {
    const float h = size * 0.5f;
    const Vec3 points[8] = {Vec3(-h, -h, -h), Vec3(h, -h, -h), Vec3(h, h, -h), Vec3(-h, h, -h),
                            Vec3(-h, -h, h),  Vec3(h, -h, h),  Vec3(h, h, h),  Vec3(-h, h, h)};
    // 면 winding은 UE와 같이 바깥에서 볼 때 시계 방향이다(법선 = Cross(P2 − P0, P1 − P0)이 바깥).
    static constexpr uint32 Faces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1},
                                           {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
    PolygonSpec polygons[6];
    for (uint32 face = 0; face < 6; ++face) {
        polygons[face] = {Span<const uint32>(Faces[face], 4), 0};
    }

    if (!BuildPolygonMesh(mesh, points, polygons, 1, false)) {
        return false;
    }

    const uint32 uv = CornerAttribute(mesh, MeshAttributeName::TexCoord, MeshValueType::Vec2);
    if (uv == MeshNoAttribute) {
        return false;
    }

    static constexpr float FaceUvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    for (uint32 corner = 0; corner < mesh.Count(MeshElement::Corner); ++corner) {
        mesh.SetVec2(uv, 0, corner, Vec2(FaceUvs[corner % 4][0], FaceUvs[corner % 4][1]));
    }

    return true;
}

// 위도·경도 구(반지름 radius, 경도 segments·위도 rings): 코너는 정점 공유, 극은 삼각형, 이음매는
// 정점을 겹쳐 둔다(u = 0과 1). UV는 (경도/segments, 위도/rings).
inline bool BuildUvSphere(MeshSource& mesh, float radius, uint32 segments, uint32 rings) noexcept {
    Array<Vec3> points(mesh.GetAllocator());
    Array<Vec2> uvs(mesh.GetAllocator());
    for (uint32 ring = 0; ring <= rings; ++ring) {
        const double v = static_cast<double>(ring) / rings;
        for (uint32 segment = 0; segment <= segments; ++segment) {
            const double u = static_cast<double>(segment) / segments;
            const double theta = u * 2.0 * 3.141592653589793;
            const double phi = v * 3.141592653589793;
            // 시험 형상 생성용 근사다(결정성과 무관 — 입력 값일 뿐이다). sin·cos는 다항식으로
            // 만든다.
            auto sine = [](double x) noexcept {
                double sum = 0.0;
                double term = x;
                for (int k = 1; k < 40; k += 2) {
                    sum += term;
                    term = -term * x * x / ((k + 1) * (k + 2));
                }

                return sum;
            };
            const double sinPhi = sine(phi);
            const double cosPhi = sine(phi + 3.141592653589793 / 2);
            const double sinTheta = sine(theta);
            const double cosTheta = sine(theta + 3.141592653589793 / 2);
            if (!points.TryPushBack(Vec3(static_cast<float>(radius * sinPhi * cosTheta),
                                         static_cast<float>(radius * sinPhi * sinTheta),
                                         static_cast<float>(radius * cosPhi))) ||
                !uvs.TryPushBack(Vec2(static_cast<float>(u), static_cast<float>(v)))) {
                return false;
            }
        }
    }

    const uint32 stride = segments + 1;
    Array<uint32> quads(mesh.GetAllocator());
    Array<PolygonSpec> polygons(mesh.GetAllocator());
    if (!quads.TryReserve(static_cast<usize>(segments) * rings * 4)) {
        return false;
    }

    for (uint32 ring = 0; ring < rings; ++ring) {
        for (uint32 segment = 0; segment < segments; ++segment) {
            const uint32 a = ring * stride + segment;
            const uint32 b = a + 1;
            const uint32 c = a + stride + 1;
            const uint32 d = a + stride;
            const usize start = quads.Size();
            // 극 고리는 같은 위치 두 점을 가진 사각형 대신 삼각형으로 둔다.
            if (ring == 0) {
                (void)quads.TryPushBack(a);
                (void)quads.TryPushBack(c);
                (void)quads.TryPushBack(d);
            } else if (ring + 1 == rings) {
                (void)quads.TryPushBack(a);
                (void)quads.TryPushBack(b);
                (void)quads.TryPushBack(d);
            } else {
                (void)quads.TryPushBack(a);
                (void)quads.TryPushBack(b);
                (void)quads.TryPushBack(c);
                (void)quads.TryPushBack(d);
            }

            if (!polygons.TryPushBack(PolygonSpec{
                    Span<const uint32>(quads.Data() + start, quads.Size() - start), 0})) {
                return false;
            }
        }
    }

    if (!BuildPolygonMesh(mesh, points.View(), polygons.View(), 1, true)) {
        return false;
    }

    const uint32 uv = CornerAttribute(mesh, MeshAttributeName::TexCoord, MeshValueType::Vec2);
    for (uint32 corner = 0; corner < mesh.Count(MeshElement::Corner); ++corner) {
        mesh.SetVec2(uv, 0, corner, uvs[mesh.CornerVertices()[corner]]);
    }

    return uv != MeshNoAttribute;
}

// settings의 field를 기본값이 아닌 값으로 바꾼다(실수 필드는 -0.0 등 bit만 다른 값도 쓴다 — 보존
// 설정 경고 시험).
inline void ChangeSetting(SkinnedMeshBuildSettings& settings,
                          MeshBuildSettingField field) noexcept {
    switch (field) {
    case MeshBuildSettingField::RecomputeNormals:
        settings.recomputeNormals = false;
        break;
    case MeshBuildSettingField::RecomputeTangents:
        settings.recomputeTangents = false;
        break;
    case MeshBuildSettingField::UseMikkTSpace:
        settings.useMikkTSpace = false;
        break;
    case MeshBuildSettingField::ComputeWeightedNormals:
        settings.computeWeightedNormals = true;
        break;
    case MeshBuildSettingField::RemoveDegenerates:
        settings.removeDegenerates = false;
        break;
    case MeshBuildSettingField::BuildReversedIndexBuffer:
        settings.buildReversedIndexBuffer = false;
        break;
    case MeshBuildSettingField::UseHighPrecisionTangentBasis:
        settings.useHighPrecisionTangentBasis = true;
        break;
    case MeshBuildSettingField::UseFullPrecisionUVs:
        settings.useFullPrecisionUVs = true;
        break;
    case MeshBuildSettingField::GenerateLightmapUVs:
        settings.generateLightmapUVs = false;
        break;
    case MeshBuildSettingField::MinLightmapResolution:
        settings.minLightmapResolution = 128;
        break;
    case MeshBuildSettingField::SrcLightmapIndex:
        settings.srcLightmapIndex = 1;
        break;
    case MeshBuildSettingField::DstLightmapIndex:
        settings.dstLightmapIndex = 2;
        break;
    case MeshBuildSettingField::BuildScale:
        settings.buildScale = Vec3d(2.0, 2.0, 2.0);
        break;
    case MeshBuildSettingField::GenerateDistanceFieldAsIfTwoSided:
        settings.generateDistanceFieldAsIfTwoSided = true;
        break;
    case MeshBuildSettingField::DistanceFieldResolutionScale:
        settings.distanceFieldResolutionScale = -0.0f;
        break;
    case MeshBuildSettingField::MaxLumenMeshCards:
        settings.maxLumenMeshCards = 4;
        break;
    case MeshBuildSettingField::SupportFaceRemap:
        settings.supportFaceRemap = true;
        break;
    case MeshBuildSettingField::UseHighPrecisionSkinWeights:
        settings.useHighPrecisionSkinWeights = true;
        break;
    case MeshBuildSettingField::ThresholdPosition:
        settings.thresholdPosition = 0.001f;
        break;
    case MeshBuildSettingField::ThresholdTangentNormal:
        settings.thresholdTangentNormal = 0.001f;
        break;
    case MeshBuildSettingField::ThresholdUV:
        settings.thresholdUV = 0.01f;
        break;
    case MeshBuildSettingField::MorphThresholdPosition:
        settings.morphThresholdPosition = 0.5f;
        break;
    case MeshBuildSettingField::BoneInfluenceLimit:
        settings.boneInfluenceLimit = 4;
        break;
    }
}

// 정규 byte에 값 하나를 붙인다.
template <class T>
bool AppendBytes(Array<uint8>& bytes, const T& value) noexcept {
    return bytes.TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>(&value), sizeof(T)))
        .IsOk();
}

template <class T>
bool AppendArray(Array<uint8>& bytes, const Array<T>& values) noexcept {
    const uint64 count = values.Size();
    return AppendBytes(bytes, count) &&
           (values.IsEmpty() ||
            bytes
                .TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>(values.Data()),
                                             values.Size() * sizeof(T)))
                .IsOk());
}

// 빌드 결과의 정규 byte다(모든 배열과 값 — 같은 결과면 같은 byte, 결정성 비교용).
inline Array<uint8> CanonicalBytes(Allocator& allocator,
                                   const StaticMeshBuildResult& result) noexcept {
    Array<uint8> bytes(allocator);
    bool ok = AppendArray(bytes, result.positions) && AppendArray(bytes, result.normals) &&
              AppendArray(bytes, result.tangents) && AppendArray(bytes, result.binormalSigns) &&
              AppendBytes(bytes, result.texCoordChannels) && AppendArray(bytes, result.texCoords) &&
              AppendArray(bytes, result.colors) && AppendArray(bytes, result.indices);
    for (const MeshBuildSection& section : result.sections) {
        const uint32 fields[5] = {section.materialIndex, section.firstIndex, section.triangleCount,
                                  section.minVertex, section.maxVertex};
        ok = ok && AppendBytes(bytes, fields) && AppendBytes(bytes, section.castShadow);
    }

    ok = ok && AppendBytes(bytes, result.bounds.boxMin) &&
         AppendBytes(bytes, result.bounds.boxMax) && AppendBytes(bytes, result.bounds.sphereRadius);
    for (const MeshDiagnostic& warning : result.warnings) {
        ok = ok && AppendBytes(bytes, warning.code) && AppendBytes(bytes, warning.issue) &&
             AppendBytes(bytes, warning.element) && AppendBytes(bytes, warning.attribute) &&
             AppendBytes(bytes, warning.index) && AppendBytes(bytes, warning.count);
    }

    if (!ok) {
        bytes.Clear();
    }

    return bytes;
}

inline bool SameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || platform::MemoryEquals(left.Data(), right.Data(), left.Size()));
}

inline StaticMeshBuildInput InputFor(const MeshSource& mesh) noexcept {
    StaticMeshBuildInput input;
    input.source = &mesh;
    return input;
}

// warnings에서 code인 경고 수다.
inline uint32 CountWarnings(const StaticMeshBuildResult& result,
                            AssetDiagnosticCode code) noexcept {
    uint32 count = 0;
    for (const MeshDiagnostic& warning : result.warnings) {
        count += warning.code == code ? 1 : 0;
    }

    return count;
}

inline bool Near(float a, float b, float tolerance) noexcept {
    return math::Abs(a - b) <= tolerance;
}

inline bool FiniteVector(const Vec3& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

inline bool NearVector(const Vec3& a, const Vec3& b, float tolerance) noexcept {
    return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
}

} // namespace rocks::mesh_tools_test
