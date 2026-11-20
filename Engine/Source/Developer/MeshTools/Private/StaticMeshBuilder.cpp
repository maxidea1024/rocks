#include "MeshBuildInternal.h"

#include <Base/NumericLimits.h>
#include <Core/Math/Color.h>
#include <Core/Text/Name.h>
#include <MeshSource/MeshSourceValidation.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>

// 정적 메시 LOD 빌드의 단계 연결이다(Docs/Design/MeshAnimationAssets.ko.md 5.2·5.4절, UE
// FStaticMeshBuilder::Build의 LOD 루프와 BuildVertexBuffer·BuildCombinedSectionIndices).
namespace rocks {

namespace {

// buildScale 축 수다(x·y·z).
constexpr uint32 BuildScaleAxes = 3;

// 사본이 담는 코너 속성 수다(Normal·Tangent·BinormalSign·Color·TexCoord).
constexpr uint32 CopiedCornerAttributes = 5;

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

// 사본 속성 번호와 원본 속성 번호다(없으면 MeshNoAttribute).
struct CopiedAttributes {
    uint32 work[CopiedCornerAttributes] = {MeshNoAttribute, MeshNoAttribute, MeshNoAttribute,
                                           MeshNoAttribute, MeshNoAttribute};
    uint32 source[CopiedCornerAttributes] = {MeshNoAttribute, MeshNoAttribute, MeshNoAttribute,
                                             MeshNoAttribute, MeshNoAttribute};
};

enum CopiedSlot : uint32 {
    NormalSlot,
    TangentSlot,
    SignSlot,
    ColorSlot,
    TexCoordSlot
};

// failure는 BuildStaticMeshLod의 지역 진단이라 늘 있다.
void Fail(MeshDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error, MeshIssue issue,
          uint32 index) noexcept {
    MeshDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.error = error;
    diagnostic.issue = issue;
    diagnostic.index = index;
    *failure = diagnostic;
}

MeshDiagnostic Warning(AssetDiagnosticCode code, MeshIssue issue, MeshElement element, uint32 index,
                       uint64 count) noexcept {
    MeshDiagnostic warning;
    warning.code = code;
    warning.issue = issue;
    warning.element = element;
    warning.index = index;
    warning.count = count;
    return warning;
}

// buildScale을 float로 바꾸고 검사한다(축마다 0이 아닌 유한값, 역수도 유한, double이 float 범위
// 안). 음수 성분 수를 센다.
bool ConvertBuildScale(const Vec3d& scale, Vec3& out, uint32& negatives,
                       MeshDiagnostic* failure) noexcept {
    const double axes[BuildScaleAxes] = {scale.x, scale.y, scale.z};
    float converted[BuildScaleAxes] = {};
    negatives = 0;
    for (uint32 axis = 0; axis < BuildScaleAxes; ++axis) {
        const bool inRange =
            math::Abs(axes[axis]) <= static_cast<double>(NumericLimits<float>::Max());
        converted[axis] = inRange ? static_cast<float>(axes[axis]) : 0.0f;
        if (!inRange || converted[axis] == 0.0f || !math::IsFinite(1.0f / converted[axis])) {
            Fail(failure, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                 MeshIssue::InvalidBuildScale, axis);
            return false;
        }

        negatives += converted[axis] < 0.0f ? 1 : 0;
    }

    out = Vec3(converted[0], converted[1], converted[2]);
    return true;
}

// 크기를 곱한 위치가 모두 유한한지 본다(넘치면 bounds·용접이 비유한 값을 다루게 된다).
bool CheckScaledPositions(const MeshSource& source, const Vec3& scale,
                          MeshDiagnostic* failure) noexcept {
    for (const Vec3& position : source.Positions()) {
        const float scaled[BuildScaleAxes] = {position.x * scale.x, position.y * scale.y,
                                              position.z * scale.z};
        for (uint32 axis = 0; axis < BuildScaleAxes; ++axis) {
            if (!math::IsFinite(scaled[axis])) {
                Fail(failure, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                     MeshIssue::InvalidBuildScale, axis);
                return false;
            }
        }
    }

    return true;
}

// 원본의 위상과 빌드가 쓰는 코너 속성만 담은 사본을 만든다(5.2절 "원본 사본" — 스킨·morph·사용자
// 속성은 정적 메시 빌드가 쓰지 않아 담지 않는다).
Result<void> CopyBuildMesh(const MeshSource& source, MeshSource& work,
                           CopiedAttributes& attributes) noexcept {
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

    return Result<void>::Success();
}

// 값 정리 경고의 속성 번호를 원본 번호로 바꿔 붙인다.
Result<void> AppendSanitizeWarnings(Span<const MeshDiagnostic> warnings,
                                    const CopiedAttributes& attributes,
                                    Array<MeshDiagnostic>& out) noexcept {
    for (MeshDiagnostic warning : warnings) {
        for (uint32 slot = 0; slot < CopiedCornerAttributes; ++slot) {
            if (attributes.work[slot] == warning.attribute) {
                warning.attribute = attributes.source[slot];
            }
        }

        if (auto pushed = out.TryPushBack(warning); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 5.2절 6·7단계(보존 설정)와 8단계 경고: 기본값이 아닌 보존 필드마다 UnsupportedSetting, 음수
// 성분이 홀수 개면 NegativeBuildScale.
Result<void> AppendSettingWarnings(const MeshBuildSettings& settings, uint32 negatives,
                                   Array<MeshDiagnostic>& out) noexcept {
    for (uint32 field = 0; field < MeshBuildSettingFieldCount; ++field) {
        const auto value = static_cast<MeshBuildSettingField>(field);
        if (IsPreservedMeshBuildSetting(value) && IsMeshBuildSettingChanged(settings, value)) {
            auto pushed = out.TryPushBack(Warning(AssetDiagnosticCode::UnsupportedSetting,
                                                  MeshIssue::UnsupportedSetting,
                                                  MeshElement::Invalid, field, 0));
            if (!pushed) {
                return pushed;
            }
        }
    }

    if (negatives % 2 != 0) {
        return out.TryPushBack(Warning(AssetDiagnosticCode::NegativeBuildScale,
                                       MeshIssue::NegativeBuildScale, MeshElement::Invalid,
                                       MeshNoIndex, negatives));
    }

    return Result<void>::Success();
}

// 렌더 정점을 만드는 중의 값과 용접 비교용 보조 배열이다(UE FPendingVertex·FMeshBuildVertexData).
struct PendingVertex {
    Vec3 position{};
    Vec3 tangentX{};
    Vec3 tangentY{};
    Vec3 tangentZ{};
    Color color{};
    Vec4 linearColor{};
    Vec2 uvs[MeshMaxTexCoords] = {};
};

struct VertexBuffers {
    explicit VertexBuffers(Allocator& allocator) noexcept
        : tangentY(allocator), colorKeys(allocator), uvs(allocator), wedgeVertices(allocator) {
    }

    // 용접 비교에 쓰는 쌍법선과 8비트 sRGB 색(UE는 FColor로 비교한다)이다.
    Array<Vec3> tangentY;
    Array<Color> colorKeys;
    // 정점 우선 UV(정점마다 채널 수만큼) — 끝에 채널 우선으로 옮긴다.
    Array<Vec2> uvs;
    // wedge마다의 렌더 정점 번호(퇴화로 빠진 삼각형은 NoRenderVertex).
    Array<uint32> wedgeVertices;
};

bool VerticesEqual(const PendingVertex& pending, const StaticMeshBuildResult& result,
                   const VertexBuffers& buffers, uint32 vertex, uint32 channels, bool hasColors,
                   float positionThreshold) noexcept {
    if (!mesh_build::PointsEqual(pending.position, result.positions[vertex], positionThreshold) ||
        !mesh_build::PointsEqual(pending.tangentZ, result.normals[vertex],
                                 MeshNormalsSameThreshold) ||
        !mesh_build::PointsEqual(pending.tangentX, result.tangents[vertex],
                                 MeshNormalsSameThreshold) ||
        !mesh_build::PointsEqual(pending.tangentY, buffers.tangentY[vertex],
                                 MeshNormalsSameThreshold)) {
        return false;
    }

    if (hasColors && !(pending.color == buffers.colorKeys[vertex])) {
        return false;
    }

    for (uint32 channel = 0; channel < channels; ++channel) {
        if (!mesh_build::UvsEqual(pending.uvs[channel],
                                  buffers.uvs[static_cast<usize>(vertex) * channels + channel])) {
            return false;
        }
    }

    return true;
}

Result<void> EmitVertex(const PendingVertex& pending, uint32 channels, bool hasColors,
                        StaticMeshBuildResult& result, VertexBuffers& buffers) noexcept {
    if (auto pushed = result.positions.TryPushBack(pending.position); !pushed) {
        return pushed;
    }

    if (auto pushed = result.normals.TryPushBack(pending.tangentZ); !pushed) {
        return pushed;
    }

    if (auto pushed = result.tangents.TryPushBack(pending.tangentX); !pushed) {
        return pushed;
    }

    const float sign =
        mesh_build::BasisDeterminantSign(pending.tangentX, pending.tangentY, pending.tangentZ);
    if (auto pushed = result.binormalSigns.TryPushBack(sign); !pushed) {
        return pushed;
    }

    if (auto pushed = buffers.tangentY.TryPushBack(pending.tangentY); !pushed) {
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
    mesh_build::NormalizeInPlace(vertex.tangentX);
    mesh_build::NormalizeInPlace(vertex.tangentY);
    vertex.tangentZ = vertex.tangentZ / scale;
    mesh_build::NormalizeInPlace(vertex.tangentZ);
    vertex.tangentX = mesh_build::FiniteOrZero(vertex.tangentX);
    vertex.tangentY = mesh_build::FiniteOrZero(vertex.tangentY);
    vertex.tangentZ = mesh_build::FiniteOrZero(vertex.tangentZ);
}

// 5.2절 8·9단계: 삼각형 순서로 코너를 렌더 정점으로 옮긴다(UE BuildVertexBuffer). 퇴화 삼각형(두
// 꼭짓점 위치가 threshold 안)은 빼고, 겹치는 앞 wedge의 렌더 정점과
// 위치·법선·tangent·쌍법선·색·UV가 같으면 그 정점을 쓴다.
Result<void> BuildRenderVertices(const MeshSource& work, const mesh_build::CornerBasis& basis,
                                 const CopiedAttributes& attributes,
                                 const mesh_build::OverlappingWedges& overlaps, const Vec3& scale,
                                 float threshold, StaticMeshBuildResult& result,
                                 VertexBuffers& buffers) noexcept {
    const uint32 triangles = work.Count(MeshElement::Triangle);
    const uint32 channels = result.texCoordChannels;
    const bool hasColors = attributes.work[ColorSlot] != MeshNoAttribute;
    if (auto resized = buffers.wedgeVertices.TryResize(static_cast<usize>(triangles) * 3);
        !resized) {
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

        const bool degenerate = mesh_build::PointsEqual(points[0], points[1], threshold) ||
                                mesh_build::PointsEqual(points[0], points[2], threshold) ||
                                mesh_build::PointsEqual(points[1], points[2], threshold);
        for (uint32 slot = 0; slot < 3; ++slot) {
            const uint32 wedge = 3 * triangle + slot;
            buffers.wedgeVertices[wedge] = mesh_build::NoRenderVertex;
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
                pending.linearColor = Vec4(color[4 * corner], color[4 * corner + 1],
                                           color[4 * corner + 2], color[4 * corner + 3]);
                pending.color = LinearColor(pending.linearColor.x, pending.linearColor.y,
                                            pending.linearColor.z, pending.linearColor.w)
                                    .ToSRGB();
            }

            for (uint32 channel = 0; channel < channels; ++channel) {
                const Span<const float> uv =
                    work.FloatValues(attributes.work[TexCoordSlot], channel);
                pending.uvs[channel] = Vec2(uv[2 * corner], uv[2 * corner + 1]);
            }

            // 겹치는 앞 wedge의 렌더 정점 가운데 같은 것을 찾는다(UE와 같이 wedge 번호 순 첫 일치).
            uint32 vertex = mesh_build::NoRenderVertex;
            for (uint32 other : overlaps.Row(wedge)) {
                if (other >= wedge) {
                    break;
                }

                const uint32 candidate = buffers.wedgeVertices[other];
                if (candidate != mesh_build::NoRenderVertex &&
                    VerticesEqual(pending, result, buffers, candidate, channels, hasColors,
                                  threshold)) {
                    vertex = candidate;
                    break;
                }
            }

            if (vertex == mesh_build::NoRenderVertex) {
                vertex = result.VertexCount();
                if (auto emitted = EmitVertex(pending, channels, hasColors, result, buffers);
                    !emitted) {
                    return emitted;
                }
            }

            buffers.wedgeVertices[wedge] = vertex;
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

// 묶음 group의 MaterialSlotName과 맞는 슬롯 번호다(없으면 MeshNoIndex — UE
// GetMaterialIndexFromImportedMaterialSlotName과 같이 첫 일치, Name 비교 규칙).
uint32 MatchSlot(const MeshSource& source, uint32 slotAttribute, uint32 group,
                 Span<const StringView> slotNames) noexcept {
    const StringView name = slotAttribute == MeshNoAttribute
                                ? mesh_build::NoSlotName
                                : source.NameValue(slotAttribute, 0, group);
    for (uint32 slot = 0; slot < slotNames.Size(); ++slot) {
        if (NameTextEquals(slotNames[slot], name)) {
            return slot;
        }
    }

    return MeshNoIndex;
}

// 5.2절 10단계: 묶음 순서로 섹션을 만들고 각 섹션의 index를 삼각형 순서로 이어 붙인다(UE
// BuildCombinedSectionIndices). 슬롯 대응이 없으면 묶음 번호 슬롯을 쓰고 MaterialSlotFallback
// 경고다.
Result<void> BuildSections(const MeshSource& source, const StaticMeshBuildInput& input,
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
        if (buffers.wedgeVertices[3 * triangle] != mesh_build::NoRenderVertex) {
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
        uint32 material = MatchSlot(source, slotAttribute, group, input.slotNames);
        const bool overridden = group < input.sectionInfo.Size() &&
                                input.sectionInfo[group].materialIndex != MeshNoIndex;
        if (overridden) {
            material = input.sectionInfo[group].materialIndex;
        } else if (material == MeshNoIndex) {
            material = group;
            auto pushed = result.warnings.TryPushBack(
                Warning(AssetDiagnosticCode::MaterialSlotFallback, MeshIssue::MaterialSlotFallback,
                        MeshElement::PolygonGroup, group, 0));
            if (!pushed) {
                return pushed;
            }
        }

        section.materialIndex = material;
        section.castShadow =
            group < input.sectionInfo.Size() ? input.sectionInfo[group].castShadow : true;
    }

    if (auto resized = result.indices.TryResize(next); !resized) {
        return resized;
    }

    for (uint32 triangle = 0; triangle < triangles; ++triangle) {
        if (buffers.wedgeVertices[3 * triangle] == mesh_build::NoRenderVertex) {
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

// 5.2절 12단계: 렌더 정점 위치의 상자와 상자 중심 구(UE BuildVertexBuffer 끝 부분), 그다음 bounds
// 확장(UE UStaticMesh::CalculateExtendedBounds — 확장이 있으면 구 반지름 = 상자 반대각선). 넘침
// 없이 double로 계산한다.
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

// 5.2절 1–5·8–10·12단계를 차례로 한다(결정적 부동소수점 환경 안).
Result<void> BuildInEnvironment(const StaticMeshBuildInput& input, Allocator& scratch,
                                StaticMeshBuildResult& result, MeshDiagnostic* failure) noexcept {
    const MeshSource& source = *input.source;
    MeshValidateOptions options;
    options.scratch = &scratch;
    if (auto valid = ValidateMeshSource(source, options, failure); !valid) {
        return valid;
    }

    const uint32 triangles = source.Count(MeshElement::Triangle);
    if (static_cast<uint64>(triangles) * 3 > NumericLimits<uint32>::Max() ||
        triangles > MeshMikkTSpaceMaxTriangles) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    Vec3 scale;
    uint32 negatives = 0;
    if (!ConvertBuildScale(input.settings.buildScale, scale, negatives, failure) ||
        !CheckScaledPositions(source, scale, failure)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 2단계: 사본에 값 정리.
    MeshSource work(scratch);
    CopiedAttributes attributes;
    if (auto copied = CopyBuildMesh(source, work, attributes); !copied) {
        return copied;
    }

    Array<MeshDiagnostic> sanitizeWarnings(scratch);
    if (auto sanitized = SanitizeMeshSource(work, options, sanitizeWarnings, failure); !sanitized) {
        return sanitized;
    }

    if (auto appended =
            AppendSanitizeWarnings(sanitizeWarnings.View(), attributes, result.warnings);
        !appended) {
        return appended;
    }

    if (auto appended = AppendSettingWarnings(input.settings, negatives, result.warnings);
        !appended) {
        return appended;
    }

    mesh_build::CornerBasis basis;
    basis.normals = work.FloatValues(attributes.work[NormalSlot], 0);
    basis.tangents = work.FloatValues(attributes.work[TangentSlot], 0);
    basis.signs = work.FloatValues(attributes.work[SignSlot], 0);
    const bool hasUv = attributes.work[TexCoordSlot] != MeshNoAttribute;
    if (hasUv) {
        basis.uv0 = work.FloatValues(attributes.work[TexCoordSlot], 0);
        result.texCoordChannels = work.AttributeInfo(attributes.work[TexCoordSlot]).channels;
    }

    // 3·4단계.
    const float threshold = input.settings.removeDegenerates ? MeshPointsSameThreshold : 0.0f;
    mesh_build::TriangleBases triangleBases(scratch);
    if (auto computed = mesh_build::ComputeTriangleBases(work, basis, threshold, triangleBases);
        !computed) {
        return computed;
    }

    mesh_build::OverlappingWedges overlaps(scratch);
    if (auto found = mesh_build::FindOverlappingWedges(work, threshold, scratch, overlaps);
        !found) {
        return found;
    }

    // 5단계(UE SetupRenderMeshDescription의 플래그: 겹친 코너 법선 섞기는 정적 메시에 늘 켜져 있고,
    // tangent 재계산은 법선 재계산이 켜도 켜진다).
    MeshTopologyIndex topology(scratch);
    if (auto built = topology.Build(work); !built) {
        return built;
    }

    mesh_build::CornerBasisOptions basisOptions;
    basisOptions.forceNormals = input.settings.recomputeNormals;
    basisOptions.forceTangents =
        input.settings.recomputeNormals || input.settings.recomputeTangents;
    basisOptions.mikkTSpace = basisOptions.forceTangents && input.settings.useMikkTSpace && hasUv;
    basisOptions.weighted = input.settings.computeWeightedNormals;
    const uint32 hardAttribute = source.FindAttribute(MeshElement::Edge, MeshAttributeName::IsHard);
    const Span<const uint8> hardness = hardAttribute == MeshNoAttribute
                                           ? Span<const uint8>()
                                           : source.BoolValues(hardAttribute, 0);
    if (auto computed = mesh_build::ComputeCornerBases(work, topology, hardness, triangleBases,
                                                       basisOptions, scratch, basis);
        !computed) {
        return computed;
    }

    if (basisOptions.mikkTSpace && triangles != 0) {
        if (auto computed = mesh_build::ComputeMikkTSpaceTangents(work, scratch, basis);
            !computed) {
            return computed;
        }
    }

    // 8–10·12단계(11단계 정점 cache 최적화는 meshoptimizer 어댑터 전까지 원본 순서).
    VertexBuffers buffers(scratch);
    if (auto built = BuildRenderVertices(work, basis, attributes, overlaps, scale, threshold,
                                         result, buffers);
        !built) {
        return built;
    }

    if (auto built = BuildSections(source, input, buffers, scratch, result); !built) {
        return built;
    }

    result.bounds = ComputeBounds(result.positions.View(), input.positiveBoundsExtension,
                                  input.negativeBoundsExtension);
    return Result<void>::Success();
}

struct BuildJob {
    const StaticMeshBuildInput* input = nullptr;
    Allocator* scratch = nullptr;
    StaticMeshBuildResult* result = nullptr;
    MeshDiagnostic* failure = nullptr;
    Result<void> status = Result<void>::Success();
};

} // namespace

Result<void> BuildStaticMeshLod(const StaticMeshBuildInput& input, StaticMeshBuildResult& out,
                                MeshDiagnostic* failure) noexcept {
    if (input.source == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = out.positions.GetAllocator();
    StaticMeshBuildResult result(allocator);
    MeshDiagnostic diagnostic;
    BuildJob job;
    job.input = &input;
    job.scratch = input.scratch != nullptr ? input.scratch : &allocator;
    job.result = &result;
    job.failure = &diagnostic;
    platform::RunWithDeterministicFloatingPoint(
        [](void* context) noexcept {
            auto& build = *static_cast<BuildJob*>(context);
            build.status =
                BuildInEnvironment(*build.input, *build.scratch, *build.result, build.failure);
        },
        &job);
    if (!job.status) {
        if (failure != nullptr) {
            diagnostic.error = job.status.Error();
            *failure = diagnostic;
        }

        return Result<void>::Failure(job.status.Error());
    }

    out = Move(result);
    return Result<void>::Success();
}

} // namespace rocks
