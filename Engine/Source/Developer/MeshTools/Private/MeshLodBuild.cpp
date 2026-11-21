#include "MeshBuildInternal.h"

#include <Core/Text/Name.h>
#include <Platform/Memory.h>

// 정적·스킨 메시 LOD 빌드의 공용 단계다(Docs/Design/MeshAnimationAssets.ko.md 5.2·5.4·6.5절, UE
// FStaticMeshBuilder::Build의 LOD 루프와 BuildVertexBuffer·BuildCombinedSectionIndices, UE 스킨
// 빌드의 BuildSkeletalMeshChunks).
namespace rocks::mesh_build {

namespace {

// 사본에 담는 코너 속성 하나다. required면 원본에 없어도 기본값(0)으로 더한다(빌드가 쓴다).
struct CopiedAttribute {
    StringView name;
    MeshValueType type;
    bool required;
};

constexpr CopiedAttribute CornerAttributes[CopiedCornerAttributes] = {
    {MeshAttributeName::Normal, MeshValueType::Vec3, true},
    {MeshAttributeName::Tangent, MeshValueType::Vec3, true},
    {MeshAttributeName::BinormalSign, MeshValueType::Float, true},
    {MeshAttributeName::Color, MeshValueType::Vec4, false},
    {MeshAttributeName::TexCoord, MeshValueType::Vec2, false},
};

Result<void> CopyTopology(const MeshSource& source, MeshSource& work) noexcept {
    for (const Vec3& position : source.Positions()) {
        if (auto added = work.AddVertex(position); !added) {
            return Result<void>::Failure(added.Error());
        }
    }

    for (uint32 vertex : source.CornerVertices()) {
        if (auto added = work.AddCorner(vertex); !added) {
            return Result<void>::Failure(added.Error());
        }
    }

    for (const MeshEdgeVertices& edge : source.Edges()) {
        if (auto added = work.AddEdge(edge.first, edge.second); !added) {
            return Result<void>::Failure(added.Error());
        }
    }

    for (uint32 group = 0; group < source.Count(MeshElement::PolygonGroup); ++group) {
        if (auto added = work.AddPolygonGroup(); !added) {
            return Result<void>::Failure(added.Error());
        }
    }

    for (uint32 polygon = 0; polygon < source.Count(MeshElement::Polygon); ++polygon) {
        auto added =
            work.AddPolygon(source.PolygonCorners(polygon), source.PolygonGroups()[polygon]);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }
    }

    for (uint32 triangle = 0; triangle < source.Count(MeshElement::Triangle); ++triangle) {
        const MeshTriangleCorners& corners = source.TriangleCorners()[triangle];
        auto added = work.AddTriangle(corners.corners[0], corners.corners[1], corners.corners[2],
                                      source.TrianglePolygons()[triangle]);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }
    }

    return Result<void>::Success();
}

// 원본 정점 가중치 속성 하나를 사본에 더하고 값을 옮긴다.
Result<void> CopyWeights(const MeshSource& source, uint32 attribute, MeshSource& work,
                         WorkAttributes& attributes) noexcept {
    const MeshAttributeInfo info = source.AttributeInfo(attribute);
    auto added =
        work.AddAttribute(MeshElement::Vertex, info.name, MeshValueType::BoneInfluences, 1);
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    for (uint32 vertex = 0; vertex < source.Count(MeshElement::Vertex); ++vertex) {
        if (auto set =
                work.SetInfluences(added.Value(), vertex, source.Influences(attribute, vertex));
            !set) {
            return set;
        }
    }

    if (auto pushed = attributes.weightWork.TryPushBack(added.Value()); !pushed) {
        return pushed;
    }

    return attributes.weightSource.TryPushBack(attribute);
}

// 정점 가중치 속성(기본 SkinWeights 먼저, 그다음 프로필을 원본 순서로)을 사본에 담는다.
Result<void> CopyAllWeights(const MeshSource& source, MeshSource& work,
                            WorkAttributes& attributes) noexcept {
    const uint32 base = source.FindAttribute(MeshElement::Vertex, MeshAttributeName::SkinWeights);
    if (base != MeshNoAttribute) {
        if (auto copied = CopyWeights(source, base, work, attributes); !copied) {
            return copied;
        }
    }

    for (uint32 attribute = 0; attribute < source.AttributeCount(); ++attribute) {
        // 접두 이름의 요소·값 타입은 원본 검증이 보장한다(정점 BoneInfluences).
        if (MatchesMeshAttributePrefix(source.AttributeInfo(attribute).name,
                                       MeshAttributeName::SkinWeightsPrefix)) {
            if (auto copied = CopyWeights(source, attribute, work, attributes); !copied) {
                return copied;
            }
        }
    }

    return Result<void>::Success();
}

// 렌더 정점을 만드는 중의 값이다(UE FPendingVertex).
struct PendingVertex {
    Vec3 position{};
    Vec3 tangentX{};
    Vec3 tangentY{};
    Vec3 tangentZ{};
    Color color{};
    Vec4 linearColor{};
    Vec2 uvs[MeshMaxTexCoords] = {};
};

bool VerticesEqual(const PendingVertex& pending, const StaticMeshBuildResult& result,
                   const VertexBuffers& buffers, uint32 vertex, uint32 channels, bool hasColors,
                   const WeldOptions& weld) noexcept {
    if (!PointsEqual(pending.position, result.positions[vertex], weld.positionThreshold) ||
        !PointsEqual(pending.tangentZ, result.normals[vertex], weld.normalThreshold) ||
        !PointsEqual(pending.tangentX, result.tangents[vertex], weld.normalThreshold) ||
        !PointsEqual(pending.tangentY, buffers.tangentY[vertex], weld.normalThreshold)) {
        return false;
    }

    if (hasColors && !(pending.color == buffers.colorKeys[vertex])) {
        return false;
    }

    for (uint32 channel = 0; channel < channels; ++channel) {
        const Vec2& other = buffers.uvs[static_cast<usize>(vertex) * channels + channel];
        if (!(math::Abs(pending.uvs[channel].x - other.x) <= weld.uvThreshold &&
              math::Abs(pending.uvs[channel].y - other.y) <= weld.uvThreshold)) {
            return false;
        }
    }

    return true;
}

Result<void> EmitVertex(const PendingVertex& pending, uint32 corner, uint32 channels,
                        bool hasColors, StaticMeshBuildResult& result,
                        VertexBuffers& buffers) noexcept {
    if (auto pushed = result.positions.TryPushBack(pending.position); !pushed) {
        return pushed;
    }

    if (auto pushed = result.normals.TryPushBack(pending.tangentZ); !pushed) {
        return pushed;
    }

    if (auto pushed = result.tangents.TryPushBack(pending.tangentX); !pushed) {
        return pushed;
    }

    const float sign = BasisDeterminantSign(pending.tangentX, pending.tangentY, pending.tangentZ);
    if (auto pushed = result.binormalSigns.TryPushBack(sign); !pushed) {
        return pushed;
    }

    if (auto pushed = buffers.tangentY.TryPushBack(pending.tangentY); !pushed) {
        return pushed;
    }

    if (auto pushed = buffers.vertexCorners.TryPushBack(corner); !pushed) {
        return pushed;
    }

    if (hasColors) {
        if (auto pushed = result.colors.TryPushBack(pending.linearColor); !pushed) {
            return pushed;
        }

        if (auto pushed = buffers.colorKeys.TryPushBack(pending.color); !pushed) {
            return pushed;
        }
    }

    return buffers.uvs.TryAppend(Span<const Vec2>(pending.uvs, channels));
}

// UE ScaleStaticMeshVertex(bNeedTangents, 옛 tangent 크기 규칙 제외): 위치와 tangent·쌍법선은
// 크기를 곱하고, 법선은 크기로 나눈다(역전치). 방향은 다시 정규화한다(UE Normalize — 너무 작으면
// 그대로).
void ScaleVertex(PendingVertex& vertex, const Vec3& scale) noexcept {
    vertex.position = vertex.position * scale;
    vertex.tangentX = vertex.tangentX * scale;
    vertex.tangentY = vertex.tangentY * scale;
    NormalizeInPlace(vertex.tangentX);
    NormalizeInPlace(vertex.tangentY);
    vertex.tangentZ = RenderNormal(vertex.tangentZ, scale);
    vertex.tangentX = FiniteOrZero(vertex.tangentX);
    vertex.tangentY = FiniteOrZero(vertex.tangentY);
}

// 묶음 group의 MaterialSlotName과 맞는 슬롯 번호다(없으면 MeshNoIndex — UE
// GetMaterialIndexFromImportedMaterialSlotName과 같이 첫 일치, Name 비교 규칙).
uint32 MatchSlot(const MeshSource& source, uint32 slotAttribute, uint32 group,
                 Span<const StringView> slotNames) noexcept {
    const StringView name =
        slotAttribute == MeshNoAttribute ? NoSlotName : source.NameValue(slotAttribute, 0, group);
    for (uint32 slot = 0; slot < slotNames.Size(); ++slot) {
        if (NameTextEquals(slotNames[slot], name)) {
            return slot;
        }
    }

    return MeshNoIndex;
}

} // namespace

uint32 WorkAttributes::ToSource(uint32 attribute) const noexcept {
    for (uint32 slot = 0; slot < CopiedCornerAttributes; ++slot) {
        if (work[slot] == attribute) {
            return source[slot];
        }
    }

    for (usize index = 0; index < weightWork.Size(); ++index) {
        if (weightWork[index] == attribute) {
            return weightSource[index];
        }
    }

    return attribute;
}

MeshDiagnostic MakeWarning(AssetDiagnosticCode code, MeshIssue issue, MeshElement element,
                           uint32 attribute, uint32 index, uint64 count) noexcept {
    MeshDiagnostic warning;
    warning.code = code;
    warning.issue = issue;
    warning.element = element;
    warning.attribute = attribute;
    warning.index = index;
    warning.count = count;
    return warning;
}

Result<void> CopyBuildMesh(const MeshSource& source, bool copyWeights, MeshSource& work,
                           WorkAttributes& attributes) noexcept {
    if (auto copied = CopyTopology(source, work); !copied) {
        return copied;
    }

    for (uint32 slot = 0; slot < CopiedCornerAttributes; ++slot) {
        const CopiedAttribute& entry = CornerAttributes[slot];
        const uint32 found = source.FindAttribute(MeshElement::Corner, entry.name);
        if (found == MeshNoAttribute && !entry.required) {
            continue;
        }

        const uint8 channels = found == MeshNoAttribute ? 1 : source.AttributeInfo(found).channels;
        auto added = work.AddAttribute(MeshElement::Corner, entry.name, entry.type, channels);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        attributes.work[slot] = added.Value();
        attributes.source[slot] = found;
        for (uint32 channel = 0; found != MeshNoAttribute && channel < channels; ++channel) {
            const Span<const float> from = source.FloatValues(found, channel);
            const Span<float> to = work.FloatValues(added.Value(), channel);
            if (!from.IsEmpty()) {
                platform::MemoryCopy(to.Data(), from.Data(), from.ByteCount());
            }
        }
    }

    return copyWeights ? CopyAllWeights(source, work, attributes) : Result<void>::Success();
}

Result<void> SanitizeWorkMesh(MeshSource& work, const WorkAttributes& attributes,
                              const MeshValidateOptions& options, Array<MeshDiagnostic>& out,
                              MeshDiagnostic* failure) noexcept {
    Array<MeshDiagnostic> warnings(*options.scratch);
    if (auto sanitized = SanitizeMeshSource(work, options, warnings, failure); !sanitized) {
        failure->attribute = attributes.ToSource(failure->attribute);
        return sanitized;
    }

    for (MeshDiagnostic warning : warnings) {
        warning.attribute = attributes.ToSource(warning.attribute);
        if (auto pushed = out.TryPushBack(warning); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

Result<void> ComputeLodBases(const MeshSource& source, MeshSource& work,
                             const WorkAttributes& attributes, const MeshBuildSettings& settings,
                             float threshold, Allocator& scratch, LodBases& out) noexcept {
    out.basis.normals = work.FloatValues(attributes.work[NormalSlot], 0);
    out.basis.tangents = work.FloatValues(attributes.work[TangentSlot], 0);
    out.basis.signs = work.FloatValues(attributes.work[SignSlot], 0);
    out.hasUv = attributes.work[TexCoordSlot] != MeshNoAttribute;
    if (out.hasUv) {
        out.basis.uv0 = work.FloatValues(attributes.work[TexCoordSlot], 0);
    }

    // 3단계.
    if (auto computed = ComputeTriangleBases(work, out.basis, threshold, out.triangles);
        !computed) {
        return computed;
    }

    // 5단계(UE SetupRenderMeshDescription의 플래그: 겹친 코너 법선 섞기는 늘 켜져 있고, tangent
    // 재계산은 법선 재계산이 켜도 켜진다).
    if (auto built = out.topology.Build(work); !built) {
        return built;
    }

    out.options.forceNormals = settings.recomputeNormals;
    out.options.forceTangents = settings.recomputeNormals || settings.recomputeTangents;
    out.options.mikkTSpace = out.options.forceTangents && settings.useMikkTSpace && out.hasUv;
    out.options.weighted = settings.computeWeightedNormals;
    const uint32 hardAttribute = source.FindAttribute(MeshElement::Edge, MeshAttributeName::IsHard);
    out.hardness = hardAttribute == MeshNoAttribute ? Span<const uint8>()
                                                    : source.BoolValues(hardAttribute, 0);
    if (auto computed = ComputeCornerBases(work, out.topology, out.hardness, out.triangles,
                                           out.options, scratch, out.basis);
        !computed) {
        return computed;
    }

    if (out.options.mikkTSpace && work.Count(MeshElement::Triangle) != 0) {
        return ComputeMikkTSpaceTangents(work, scratch, out.basis);
    }

    return Result<void>::Success();
}

Vec3 RenderNormal(const Vec3& normal, const Vec3& scale) noexcept {
    Vec3 value = normal / scale;
    NormalizeInPlace(value);
    return FiniteOrZero(value);
}

Result<void> BuildRenderVertices(const MeshSource& work, const CornerBasis& basis,
                                 const WorkAttributes& attributes,
                                 const OverlappingWedges& overlaps, const Vec3& scale,
                                 const WeldOptions& weld, StaticMeshBuildResult& result,
                                 VertexBuffers& buffers) noexcept {
    const uint32 triangles = work.Count(MeshElement::Triangle);
    const uint32 channels = result.texCoordChannels;
    const bool hasColors = attributes.work[ColorSlot] != MeshNoAttribute;
    if (auto resized = buffers.wedgeVertices.TryResize(static_cast<usize>(triangles) * 3);
        !resized) {
        return resized;
    }

    if (auto resized = buffers.triangleKept.TryResize(triangles); !resized) {
        return resized;
    }

    const Span<const Vec3> positions = work.Positions();
    const Span<const uint32> cornerVertices = work.CornerVertices();
    for (uint32 triangle = 0; triangle < triangles; ++triangle) {
        const MeshTriangleCorners& corners = work.TriangleCorners()[triangle];
        Vec3 points[3];
        for (uint32 slot = 0; slot < 3; ++slot) {
            points[slot] = positions[cornerVertices[corners.corners[slot]]];
        }

        const float threshold = weld.degenerateThreshold;
        const bool degenerate = PointsEqual(points[0], points[1], threshold) ||
                                PointsEqual(points[0], points[2], threshold) ||
                                PointsEqual(points[1], points[2], threshold);
        buffers.triangleKept[triangle] = degenerate ? 0 : 1;
        for (uint32 slot = 0; slot < 3; ++slot) {
            const uint32 wedge = 3 * triangle + slot;
            buffers.wedgeVertices[wedge] = NoRenderVertex;
            if (degenerate) {
                continue;
            }

            const uint32 corner = corners.corners[slot];
            PendingVertex pending;
            pending.position = points[slot];
            pending.tangentX = basis.Tangent(corner);
            pending.tangentZ = basis.Normal(corner);
            pending.tangentY = Cross(pending.tangentZ, pending.tangentX) * basis.signs[corner];
            ScaleVertex(pending, scale);
            if (hasColors) {
                const Span<const float> color = work.FloatValues(attributes.work[ColorSlot], 0);
                pending.linearColor = Vec4(color[4 * static_cast<usize>(corner)],
                                           color[4 * static_cast<usize>(corner) + 1],
                                           color[4 * static_cast<usize>(corner) + 2],
                                           color[4 * static_cast<usize>(corner) + 3]);
                pending.color = LinearColor(pending.linearColor.x, pending.linearColor.y,
                                            pending.linearColor.z, pending.linearColor.w)
                                    .ToSRGB();
            }

            for (uint32 channel = 0; channel < channels; ++channel) {
                const Span<const float> uv =
                    work.FloatValues(attributes.work[TexCoordSlot], channel);
                pending.uvs[channel] = Vec2(uv[2 * static_cast<usize>(corner)],
                                            uv[2 * static_cast<usize>(corner) + 1]);
            }

            // 겹치는 앞 wedge의 렌더 정점 가운데 같은 것을 찾는다(UE와 같이 wedge 번호 순 첫 일치).
            uint32 vertex = NoRenderVertex;
            for (uint32 other : overlaps.Row(wedge)) {
                if (other >= wedge) {
                    break;
                }

                const uint32 candidate = buffers.wedgeVertices[other];
                if (candidate != NoRenderVertex &&
                    VerticesEqual(pending, result, buffers, candidate, channels, hasColors, weld) &&
                    (weld.sameExtra == nullptr ||
                     weld.sameExtra(weld.context, corner, buffers.vertexCorners[candidate]))) {
                    vertex = candidate;
                    break;
                }
            }

            if (vertex == NoRenderVertex) {
                vertex = result.VertexCount();
                if (auto emitted =
                        EmitVertex(pending, corner, channels, hasColors, result, buffers);
                    !emitted) {
                    return emitted;
                }
            }

            buffers.wedgeVertices[wedge] = vertex;
        }

        // UE BuildSkeletalMeshChunks: 세 렌더 정점 번호가 서로 다르지 않으면 index에서 뺀다(그
        // 삼각형이 만든 렌더 정점과 wedge 대응은 남는다 — 뒤 wedge가 용접할 수 있다).
        const uint32* mapped = buffers.wedgeVertices.Data() + 3 * static_cast<usize>(triangle);
        if (!degenerate && weld.dropCollapsedTriangles &&
            (mapped[0] == mapped[1] || mapped[0] == mapped[2] || mapped[1] == mapped[2])) {
            buffers.triangleKept[triangle] = 0;
        }
    }

    // UV를 채널 우선으로 옮긴다(texCoords[c × 정점 수 + v]).
    const uint32 vertices = result.VertexCount();
    if (auto resized = result.texCoords.TryResize(static_cast<usize>(vertices) * channels);
        !resized) {
        return resized;
    }

    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        for (uint32 channel = 0; channel < channels; ++channel) {
            result.texCoords[static_cast<usize>(channel) * vertices + vertex] =
                buffers.uvs[static_cast<usize>(vertex) * channels + channel];
        }
    }

    return Result<void>::Success();
}

Result<void> BuildSections(const MeshSource& source, Span<const StringView> slotNames,
                           Span<const MeshBuildSectionInfo> sectionInfo,
                           const VertexBuffers& buffers, Allocator& scratch,
                           StaticMeshBuildResult& result) noexcept {
    const uint32 groups = source.Count(MeshElement::PolygonGroup);
    const uint32 triangles = source.Count(MeshElement::Triangle);
    Array<uint32> cursor(scratch);
    if (auto resized = cursor.TryResize(groups); !resized) {
        return resized;
    }

    if (auto resized = result.sections.TryResize(groups); !resized) {
        return resized;
    }

    auto groupOf = [&](uint32 triangle) noexcept {
        return source.PolygonGroups()[source.TrianglePolygons()[triangle]];
    };
    for (uint32 triangle = 0; triangle < triangles; ++triangle) {
        if (buffers.triangleKept[triangle] != 0) {
            ++result.sections[groupOf(triangle)].triangleCount;
        }
    }

    const uint32 slotAttribute =
        source.FindAttribute(MeshElement::PolygonGroup, MeshAttributeName::MaterialSlotName);
    uint32 next = 0;
    for (uint32 group = 0; group < groups; ++group) {
        MeshBuildSection& section = result.sections[group];
        section.firstIndex = section.triangleCount == 0 ? 0 : next;
        cursor[group] = next;
        next += 3 * section.triangleCount;
        uint32 material = MatchSlot(source, slotAttribute, group, slotNames);
        const bool overridden =
            group < sectionInfo.Size() && sectionInfo[group].materialIndex != MeshNoIndex;
        if (overridden) {
            material = sectionInfo[group].materialIndex;
        } else if (material == MeshNoIndex) {
            material = group;
            auto pushed = result.warnings.TryPushBack(MakeWarning(
                AssetDiagnosticCode::MaterialSlotFallback, MeshIssue::MaterialSlotFallback,
                MeshElement::PolygonGroup, MeshNoAttribute, group, 0));
            if (!pushed) {
                return pushed;
            }
        }

        section.materialIndex = material;
        section.castShadow = group < sectionInfo.Size() ? sectionInfo[group].castShadow : true;
    }

    if (auto resized = result.indices.TryResize(next); !resized) {
        return resized;
    }

    for (uint32 triangle = 0; triangle < triangles; ++triangle) {
        if (buffers.triangleKept[triangle] == 0) {
            continue;
        }

        const uint32 group = groupOf(triangle);
        MeshBuildSection& section = result.sections[group];
        for (uint32 slot = 0; slot < 3; ++slot) {
            const uint32 vertex = buffers.wedgeVertices[3 * triangle + slot];
            const bool first = cursor[group] == section.firstIndex;
            section.minVertex = first || vertex < section.minVertex ? vertex : section.minVertex;
            section.maxVertex = first || vertex > section.maxVertex ? vertex : section.maxVertex;
            result.indices[cursor[group]++] = vertex;
        }
    }

    return Result<void>::Success();
}

// 렌더 정점 위치의 상자와 상자 중심 구(UE BuildVertexBuffer 끝 부분), 그다음 bounds 확장(UE
// UStaticMesh::CalculateExtendedBounds — 확장이 있으면 구 반지름 = 상자 반대각선). 넘침 없이
// double로 계산한다.
MeshBuildBounds ComputeBounds(Span<const Vec3> positions, const Vec3d& positiveExtension,
                              const Vec3d& negativeExtension) noexcept {
    MeshBuildBounds bounds;
    if (!positions.IsEmpty()) {
        Vec3d minimum(positions[0]);
        Vec3d maximum(positions[0]);
        for (const Vec3& position : positions) {
            const Vec3d value(position);
            minimum = ComponentMin(minimum, value);
            maximum = ComponentMax(maximum, value);
        }

        const Vec3d center = (minimum + maximum) * 0.5;
        double radiusSquared = 0.0;
        for (const Vec3& position : positions) {
            const Vec3d offset = Vec3d(position) - center;
            radiusSquared = math::Max(radiusSquared, offset.x * offset.x + offset.y * offset.y +
                                                         offset.z * offset.z);
        }

        bounds.boxMin = minimum;
        bounds.boxMax = maximum;
        bounds.sphereRadius = math::Sqrt(radiusSquared);
    }

    const bool extended = !(positiveExtension == Vec3d(0.0, 0.0, 0.0)) ||
                          !(negativeExtension == Vec3d(0.0, 0.0, 0.0));
    if (extended) {
        bounds.boxMin = bounds.boxMin - negativeExtension;
        bounds.boxMax = bounds.boxMax + positiveExtension;
        const Vec3d extent = (bounds.boxMax - bounds.boxMin) * 0.5;
        bounds.sphereRadius =
            math::Sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z);
    }

    return bounds;
}

} // namespace rocks::mesh_build
