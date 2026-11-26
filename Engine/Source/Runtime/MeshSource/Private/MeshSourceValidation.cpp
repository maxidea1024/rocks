#include "MeshSourceInternal.h"

#include <Core/Assert.h>
#include <Core/Containers/HashMap.h>
#include <Core/Math/MathUtility.h>
#include <MeshSource/MeshSourceValidation.h>
#include <Object/NameField.h>

// 원본 메시 내용 불변식 검증이다(Docs/Design/MeshAnimationAssets 3.1·3.5·3.7절).
namespace rocks {

namespace {

Result<void> InvalidMesh(MeshDiagnostic* failure, MeshIssue issue,
                         MeshElement element = MeshElement::Invalid,
                         uint32 attribute = MeshNoAttribute, uint32 index = MeshNoIndex) noexcept {
    return rocks::mesh_source_detail::Fail(failure, AssetDiagnosticCode::InvalidMeshData,
                                           ErrorCode::InvalidArgument, issue, element, attribute,
                                           index);
}

// 변 표의 키다(작은 번호가 위 32비트).
uint64 ValidationEdgeKey(uint32 first, uint32 second) noexcept {
    return (static_cast<uint64>(first) << 32) | second;
}

bool IsFiniteVector(const Vec3& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

Result<void> ValidateVertices(const MeshSource& mesh, MeshDiagnostic* failure) noexcept {
    const Span<const Vec3> positions = mesh.Positions();
    for (usize vertex = 0; vertex < positions.Size(); ++vertex) {
        if (!IsFiniteVector(positions[vertex])) {
            return InvalidMesh(failure, MeshIssue::NonFinitePosition, MeshElement::Vertex,
                               MeshNoAttribute, static_cast<uint32>(vertex));
        }
    }

    const uint32 vertices = mesh.Count(MeshElement::Vertex);
    const Span<const uint32> corners = mesh.CornerVertices();
    for (usize corner = 0; corner < corners.Size(); ++corner) {
        if (corners[corner] >= vertices) {
            return InvalidMesh(failure, MeshIssue::CornerVertexRange, MeshElement::Corner,
                               MeshNoAttribute, static_cast<uint32>(corner));
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateEdges(const MeshSource& mesh, HashMap<uint64, uint32>& edgeMap,
                           MeshDiagnostic* failure) noexcept {
    const uint32 vertices = mesh.Count(MeshElement::Vertex);
    const Span<const MeshEdgeVertices> edges = mesh.Edges();
    if (auto reserved = edgeMap.Reserve(edges.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < edges.Size(); ++index) {
        const MeshEdgeVertices edge = edges[index];
        const auto edgeIndex = static_cast<uint32>(index);
        if (edge.first >= vertices || edge.second >= vertices) {
            return InvalidMesh(failure, MeshIssue::EdgeVertexRange, MeshElement::Edge,
                               MeshNoAttribute, edgeIndex);
        }

        if (edge.first == edge.second) {
            return InvalidMesh(failure, MeshIssue::EdgeSameVertex, MeshElement::Edge,
                               MeshNoAttribute, edgeIndex);
        }

        if (edge.first > edge.second) {
            return InvalidMesh(failure, MeshIssue::EdgeOrder, MeshElement::Edge, MeshNoAttribute,
                               edgeIndex);
        }

        // 변 수만큼 미리 잡았으므로 삽입은 할당하지 않는다.
        auto inserted = edgeMap.Insert(ValidationEdgeKey(edge.first, edge.second), edgeIndex);
        ROCKS_ASSERT(inserted.IsOk());

        if (inserted.Value() == InsertOutcome::AlreadyPresent) {
            return InvalidMesh(failure, MeshIssue::DuplicateEdge, MeshElement::Edge,
                               MeshNoAttribute, edgeIndex);
        }
    }

    return Result<void>::Success();
}

Result<void> ValidatePolygons(const MeshSource& mesh, const HashMap<uint64, uint32>& edgeMap,
                              MeshDiagnostic* failure) noexcept {
    const uint32 corners = mesh.Count(MeshElement::Corner);
    const uint32 groups = mesh.Count(MeshElement::PolygonGroup);
    const Span<const uint32> cornerVertices = mesh.CornerVertices();
    const Span<const uint32> polygonGroups = mesh.PolygonGroups();
    for (uint32 polygon = 0; polygon < mesh.Count(MeshElement::Polygon); ++polygon) {
        if (polygonGroups[polygon] >= groups) {
            return InvalidMesh(failure, MeshIssue::PolygonGroupRange, MeshElement::Polygon,
                               MeshNoAttribute, polygon);
        }

        const Span<const uint32> perimeter = mesh.PolygonCorners(polygon);
        if (perimeter.Size() < MeshMinPolygonCorners) {
            return InvalidMesh(failure, MeshIssue::PolygonTooFewCorners, MeshElement::Polygon,
                               MeshNoAttribute, polygon);
        }

        for (uint32 corner : perimeter) {
            if (corner >= corners) {
                return InvalidMesh(failure, MeshIssue::PolygonCornerRange, MeshElement::Polygon,
                                   MeshNoAttribute, polygon);
            }
        }

        for (usize side = 0; side < perimeter.Size(); ++side) {
            const uint32 a = cornerVertices[perimeter[side]];
            const uint32 b = cornerVertices[perimeter[(side + 1) % perimeter.Size()]];
            // 같은 정점을 잇는 둘레 변은 퇴화 변이라 변 목록에 없다(3.1절).
            if (a == b) {
                continue;
            }

            const uint64 key = a < b ? ValidationEdgeKey(a, b) : ValidationEdgeKey(b, a);
            if (edgeMap.Find(key) == nullptr) {
                return InvalidMesh(failure, MeshIssue::PerimeterEdgeMissing, MeshElement::Polygon,
                                   MeshNoAttribute, polygon);
            }
        }
    }

    return Result<void>::Success();
}

// 삼각형 번호·다각형별 개수·둘레 소속을 본다. 임시 표: 다각형별 삼각형(CSR)과 코너 표식.
Result<void> ValidateTriangles(const MeshSource& mesh, Allocator& scratch,
                               MeshDiagnostic* failure) noexcept {
    const uint32 corners = mesh.Count(MeshElement::Corner);
    const uint32 polygons = mesh.Count(MeshElement::Polygon);
    const Span<const MeshTriangleCorners> triangles = mesh.TriangleCorners();
    const Span<const uint32> trianglePolygons = mesh.TrianglePolygons();
    Array<uint32> starts(scratch);
    if (auto resized = starts.TryResize(static_cast<usize>(polygons) + 1); !resized) {
        return resized;
    }

    for (usize triangle = 0; triangle < triangles.Size(); ++triangle) {
        const auto triangleIndex = static_cast<uint32>(triangle);
        for (uint32 corner : triangles[triangle].corners) {
            if (corner >= corners) {
                return InvalidMesh(failure, MeshIssue::TriangleCornerRange, MeshElement::Triangle,
                                   MeshNoAttribute, triangleIndex);
            }
        }

        if (trianglePolygons[triangle] >= polygons) {
            return InvalidMesh(failure, MeshIssue::TrianglePolygonRange, MeshElement::Triangle,
                               MeshNoAttribute, triangleIndex);
        }

        ++starts[trianglePolygons[triangle] + 1];
    }

    for (uint32 polygon = 0; polygon < polygons; ++polygon) {
        const usize expected = mesh.PolygonCorners(polygon).Size() - 2;
        if (starts[polygon + 1] != expected) {
            return InvalidMesh(failure, MeshIssue::TriangleCountMismatch, MeshElement::Polygon,
                               MeshNoAttribute, polygon);
        }

        starts[polygon + 1] += starts[polygon];
    }

    Array<uint32> byPolygon(scratch);
    if (auto resized = byPolygon.TryResize(triangles.Size()); !resized) {
        return resized;
    }

    Array<uint32> stamps(scratch);
    if (auto resized = stamps.TryResize(corners); !resized) {
        return resized;
    }

    // starts[p]를 채우기 커서로 쓰면 끝난 뒤 starts[p]는 p + 1의 시작이 된다.
    for (usize triangle = 0; triangle < triangles.Size(); ++triangle) {
        byPolygon[starts[trianglePolygons[triangle]]++] = static_cast<uint32>(triangle);
    }

    uint32 begin = 0;
    for (uint32 polygon = 0; polygon < polygons; ++polygon) {
        const uint32 end = starts[polygon];
        for (uint32 corner : mesh.PolygonCorners(polygon)) {
            stamps[corner] = polygon + 1;
        }

        for (uint32 position = begin; position < end; ++position) {
            const uint32 triangle = byPolygon[position];
            for (uint32 corner : triangles[triangle].corners) {
                if (stamps[corner] != polygon + 1) {
                    return InvalidMesh(failure, MeshIssue::TriangleCornerNotOnPerimeter,
                                       MeshElement::Triangle, MeshNoAttribute, triangle);
                }
            }
        }

        begin = end;
    }

    return Result<void>::Success();
}

// 속성 값을 본다(Bool 0·1, Name 내용 규칙, 영향).
Result<void> ValidateAttributeValues(const MeshSource& mesh, uint32 attribute,
                                     const MeshValidateOptions& options, Allocator& scratch,
                                     MeshDiagnostic* failure) noexcept {
    const MeshAttributeInfo info = mesh.AttributeInfo(attribute);
    const uint32 count = mesh.Count(info.element);
    switch (info.type) {
    case MeshValueType::Bool:
        for (uint32 channel = 0; channel < info.channels; ++channel) {
            const Span<const uint8> values = mesh.BoolValues(attribute, channel);
            for (uint32 index = 0; index < count; ++index) {
                if (values[index] > 1) {
                    return InvalidMesh(failure, MeshIssue::BoolValue, info.element, attribute,
                                       index);
                }
            }
        }

        break;
    case MeshValueType::Name:
        for (uint32 channel = 0; channel < info.channels; ++channel) {
            for (uint32 index = 0; index < count; ++index) {
                auto valid =
                    ValidateNameFieldText(mesh.NameValue(attribute, channel, index), scratch);
                if (!valid) {
                    if (valid.Error() != ErrorCode::InvalidArgument) {
                        return valid;
                    }

                    return InvalidMesh(failure, MeshIssue::NameValue, info.element, attribute,
                                       index);
                }
            }
        }

        break;
    case MeshValueType::BoneInfluences:
        for (uint32 vertex = 0; vertex < count; ++vertex) {
            const Span<const BoneInfluence> influences = mesh.Influences(attribute, vertex);
            if (influences.Size() > MeshMaxSourceInfluences) {
                return rocks::mesh_source_detail::Fail(
                    failure, AssetDiagnosticCode::TooManyInfluences, ErrorCode::InvalidArgument,
                    MeshIssue::TooManyInfluences, info.element, attribute, vertex);
            }

            for (usize index = 0; index < influences.Size(); ++index) {
                if (options.boneCount != MeshUnknownBoneCount &&
                    influences[index].boneIndex >= options.boneCount) {
                    return InvalidMesh(failure, MeshIssue::BoneIndexRange, info.element, attribute,
                                       vertex);
                }

                for (usize other = 0; other < index; ++other) {
                    if (influences[other].boneIndex == influences[index].boneIndex) {
                        return InvalidMesh(failure, MeshIssue::DuplicateInfluenceBone, info.element,
                                           attribute, vertex);
                    }
                }
            }
        }

        break;
    default:
        break;
    }

    return Result<void>::Success();
}

Result<void> ValidateAttributes(const MeshSource& mesh, const MeshValidateOptions& options,
                                Allocator& scratch, MeshDiagnostic* failure) noexcept {
    for (uint32 attribute = 0; attribute < mesh.AttributeCount(); ++attribute) {
        // 이름 내용 규칙(Name 내용·None 아님)은 만드는 두 길이 모두 지킨다: AddAttribute가
        // 거부하고, Load는 Name 필드 복원(ValidateNameFieldText)과 레코드
        // 판정(AttributeRecordName)을 거친다.
        const MeshAttributeInfo info = mesh.AttributeInfo(attribute);
        const rocks::mesh_source_detail::EngineName kind =
            rocks::mesh_source_detail::ClassifyAttributeName(info.name);
        // 정점 Position은 Positions()가 이미 쓰는 이름이다(3.7절 1).
        const bool position = kind == rocks::mesh_source_detail::EngineName::Position &&
                              info.element == MeshElement::Vertex;
        if (position) {
            return InvalidMesh(failure, MeshIssue::DuplicateAttributeName, info.element, attribute);
        }

        for (uint32 other = 0; other < attribute; ++other) {
            const MeshAttributeInfo earlier = mesh.AttributeInfo(other);
            if (earlier.element == info.element && NameTextEquals(earlier.name, info.name)) {
                return InvalidMesh(failure, MeshIssue::DuplicateAttributeName, info.element,
                                   attribute);
            }
        }

        // 사용자 이름(None)은 형태 제한이 없다. 엔진 이름은 3.2절 표의 형태여야 한다(Malformed는 빈
        // 규칙이라 늘 어긋난다).
        const rocks::mesh_source_detail::EngineRule rule =
            rocks::mesh_source_detail::EngineRuleFor(kind);
        const bool mismatch = info.element != rule.element || info.type != rule.type ||
                              info.channels > rule.maxChannels;
        if (kind != rocks::mesh_source_detail::EngineName::None && mismatch) {
            return InvalidMesh(failure, MeshIssue::EngineAttributeMismatch, info.element,
                               attribute);
        }

        if (auto values = ValidateAttributeValues(mesh, attribute, options, scratch, failure);
            !values) {
            return values;
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> ValidateMeshSource(const MeshSource& mesh, const MeshValidateOptions& options,
                                MeshDiagnostic* failure) noexcept {
    Allocator& scratch = options.scratch != nullptr ? *options.scratch : mesh.GetAllocator();
    if (auto vertices = ValidateVertices(mesh, failure); !vertices) {
        return vertices;
    }

    HashMap<uint64, uint32> edgeMap(scratch);
    if (auto edges = ValidateEdges(mesh, edgeMap, failure); !edges) {
        return edges;
    }

    if (auto polygons = ValidatePolygons(mesh, edgeMap, failure); !polygons) {
        return polygons;
    }

    if (auto triangles = ValidateTriangles(mesh, scratch, failure); !triangles) {
        return triangles;
    }

    return ValidateAttributes(mesh, options, scratch, failure);
}

} // namespace rocks
