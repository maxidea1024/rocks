#include <MeshSource/MeshTopologyIndex.h>

#include <Core/Assert.h>

// 원본 메시 이웃 조회 표다(Docs/Design/MeshAnimationAssets 3.7절 11).
namespace rocks {

namespace {

// rows개 행의 CSR 표를 두 번 훑어(개수 → 채우기) 만든다. visit(emit)은 (행, 항목)을 emit으로 내며
// 두 번 모두 같은 순서·같은 항목을 내야 한다(중복 제거 표식은 visit가 매번 처음부터 다시 쓴다).
template <class Visit>
Result<void> BuildTopologyTable(MeshTopologyIndex::Table& table, uint32 rows,
                                Visit&& visit) noexcept {
    if (auto resized = table.starts.TryResize(static_cast<usize>(rows) + 1); !resized) {
        return resized;
    }

    for (uint32& start : table.starts) {
        start = 0;
    }

    visit([&](uint32 row, uint32) noexcept { ++table.starts[row + 1]; });

    for (uint32 row = 0; row < rows; ++row) {
        table.starts[row + 1] += table.starts[row];
    }

    if (auto resized = table.items.TryResize(table.starts[rows]); !resized) {
        return resized;
    }

    // starts[row]를 채우기 커서로 쓰고 끝나면 한 칸씩 되돌린다.
    visit([&](uint32 row, uint32 item) noexcept { table.items[table.starts[row]++] = item; });

    for (uint32 row = rows; row > 0; --row) {
        table.starts[row] = table.starts[row - 1];
    }

    table.starts[0] = 0;
    return Result<void>::Success();
}

// 다각형 둘레 하나를 훑으며 (행, 다각형)을 내되, 같은 다각형 안에서 같은 행을 두 번 내지 않는다.
// stamps[row] = 마지막으로 낸 다각형 + 1.
void ResetTopologyStamps(Array<uint32>& stamps) noexcept {
    for (uint32& stamp : stamps) {
        stamp = 0;
    }
}

} // namespace

Span<const uint32> MeshTopologyIndex::Table::Row(uint32 row) const noexcept {
    ROCKS_ASSERT(static_cast<usize>(row) + 1 < starts.Size());
    const uint32 begin = starts[row];
    return Span<const uint32>(items.Data() + begin, starts[row + 1] - begin);
}

MeshTopologyIndex::MeshTopologyIndex(Allocator& allocator) noexcept
    : allocator_(&allocator), polygonTriangles_(allocator), groupPolygons_(allocator),
      vertexCorners_(allocator), vertexEdges_(allocator), cornerTriangles_(allocator),
      cornerPolygons_(allocator), edgePolygons_(allocator) {
}

Result<void> MeshTopologyIndex::Build(const MeshSource& mesh) noexcept {
    Allocator& allocator = *allocator_;
    MeshTopologyIndex built(allocator);
    built.mesh_ = &mesh;
    const Span<const uint32> cornerVertices = mesh.CornerVertices();
    const Span<const MeshEdgeVertices> edges = mesh.Edges();
    const Span<const MeshTriangleCorners> triangles = mesh.TriangleCorners();
    const Span<const uint32> trianglePolygons = mesh.TrianglePolygons();
    const Span<const uint32> polygonGroups = mesh.PolygonGroups();
    const uint32 polygons = mesh.Count(MeshElement::Polygon);

    auto polygonTriangles = [&](auto&& emit) noexcept {
        for (usize triangle = 0; triangle < trianglePolygons.Size(); ++triangle) {
            emit(trianglePolygons[triangle], static_cast<uint32>(triangle));
        }
    };
    if (auto made = BuildTopologyTable(built.polygonTriangles_, polygons, polygonTriangles);
        !made) {
        return made;
    }

    auto groupPolygons = [&](auto&& emit) noexcept {
        for (usize polygon = 0; polygon < polygonGroups.Size(); ++polygon) {
            emit(polygonGroups[polygon], static_cast<uint32>(polygon));
        }
    };
    if (auto made = BuildTopologyTable(built.groupPolygons_, mesh.Count(MeshElement::PolygonGroup),
                                       groupPolygons);
        !made) {
        return made;
    }

    auto vertexCorners = [&](auto&& emit) noexcept {
        for (usize corner = 0; corner < cornerVertices.Size(); ++corner) {
            emit(cornerVertices[corner], static_cast<uint32>(corner));
        }
    };
    if (auto made = BuildTopologyTable(built.vertexCorners_, mesh.Count(MeshElement::Vertex),
                                       vertexCorners);
        !made) {
        return made;
    }

    auto vertexEdges = [&](auto&& emit) noexcept {
        for (usize edge = 0; edge < edges.Size(); ++edge) {
            emit(edges[edge].first, static_cast<uint32>(edge));
            emit(edges[edge].second, static_cast<uint32>(edge));
        }
    };
    if (auto made =
            BuildTopologyTable(built.vertexEdges_, mesh.Count(MeshElement::Vertex), vertexEdges);
        !made) {
        return made;
    }

    // 같은 코너를 두 번 쓰는 퇴화 삼각형은 그 코너 행에 한 번만 든다.
    auto cornerTriangles = [&](auto&& emit) noexcept {
        for (usize triangle = 0; triangle < triangles.Size(); ++triangle) {
            const uint32* corners = triangles[triangle].corners;
            for (uint32 slot = 0; slot < 3; ++slot) {
                const bool repeated = (slot >= 1 && corners[slot] == corners[0]) ||
                                      (slot == 2 && corners[slot] == corners[1]);
                if (!repeated) {
                    emit(corners[slot], static_cast<uint32>(triangle));
                }
            }
        }
    };
    if (auto made = BuildTopologyTable(built.cornerTriangles_, mesh.Count(MeshElement::Corner),
                                       cornerTriangles);
        !made) {
        return made;
    }

    Array<uint32> stamps(allocator);
    if (auto resized = stamps.TryResize(mesh.Count(MeshElement::Corner)); !resized) {
        return resized;
    }

    auto cornerPolygons = [&](auto&& emit) noexcept {
        ResetTopologyStamps(stamps);
        for (uint32 polygon = 0; polygon < polygons; ++polygon) {
            for (uint32 corner : mesh.PolygonCorners(polygon)) {
                if (stamps[corner] != polygon + 1) {
                    stamps[corner] = polygon + 1;
                    emit(corner, polygon);
                }
            }
        }
    };
    if (auto made = BuildTopologyTable(built.cornerPolygons_, mesh.Count(MeshElement::Corner),
                                       cornerPolygons);
        !made) {
        return made;
    }

    if (auto resized = stamps.TryResize(mesh.Count(MeshElement::Edge)); !resized) {
        return resized;
    }

    // 둘레 변은 정점 → 변 표(위에서 만든 것)로 찾는다. 퇴화 변(같은 정점)은 건너뛴다.
    auto edgePolygons = [&](auto&& emit) noexcept {
        ResetTopologyStamps(stamps);
        for (uint32 polygon = 0; polygon < polygons; ++polygon) {
            const uint32 sides = static_cast<uint32>(mesh.PolygonCorners(polygon).Size());
            for (uint32 side = 0; side < sides; ++side) {
                const uint32 edge = built.PolygonSideEdge(polygon, side);
                if (edge != MeshNoIndex && stamps[edge] != polygon + 1) {
                    stamps[edge] = polygon + 1;
                    emit(edge, polygon);
                }
            }
        }
    };
    if (auto made =
            BuildTopologyTable(built.edgePolygons_, mesh.Count(MeshElement::Edge), edgePolygons);
        !made) {
        return made;
    }

    *this = Move(built);
    return Result<void>::Success();
}

Span<const uint32> MeshTopologyIndex::PolygonTriangles(uint32 polygon) const noexcept {
    return polygonTriangles_.Row(polygon);
}

Span<const uint32> MeshTopologyIndex::GroupPolygons(uint32 group) const noexcept {
    return groupPolygons_.Row(group);
}

Span<const uint32> MeshTopologyIndex::VertexCorners(uint32 vertex) const noexcept {
    return vertexCorners_.Row(vertex);
}

Span<const uint32> MeshTopologyIndex::VertexEdges(uint32 vertex) const noexcept {
    return vertexEdges_.Row(vertex);
}

Span<const uint32> MeshTopologyIndex::CornerTriangles(uint32 corner) const noexcept {
    return cornerTriangles_.Row(corner);
}

Span<const uint32> MeshTopologyIndex::CornerPolygons(uint32 corner) const noexcept {
    return cornerPolygons_.Row(corner);
}

Span<const uint32> MeshTopologyIndex::EdgePolygons(uint32 edge) const noexcept {
    return edgePolygons_.Row(edge);
}

uint32 MeshTopologyIndex::FindEdge(uint32 vertexA, uint32 vertexB) const noexcept {
    ROCKS_ASSERT(mesh_ != nullptr);
    const uint32 low = vertexA < vertexB ? vertexA : vertexB;
    const uint32 high = vertexA < vertexB ? vertexB : vertexA;
    const Span<const MeshEdgeVertices> edges = mesh_->Edges();
    for (uint32 edge : vertexEdges_.Row(low)) {
        if (edges[edge].first == low && edges[edge].second == high) {
            return edge;
        }
    }

    return MeshNoIndex;
}

uint32 MeshTopologyIndex::PolygonSideEdge(uint32 polygon, uint32 side) const noexcept {
    ROCKS_ASSERT(mesh_ != nullptr);
    const Span<const uint32> corners = mesh_->PolygonCorners(polygon);
    ROCKS_ASSERT(side < corners.Size());
    const Span<const uint32> cornerVertices = mesh_->CornerVertices();
    const uint32 a = cornerVertices[corners[side]];
    const uint32 b = cornerVertices[corners[(side + 1) % corners.Size()]];
    if (a == b) {
        return MeshNoIndex;
    }

    return FindEdge(a, b);
}

} // namespace rocks
