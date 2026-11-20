#pragma once

#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Tests/AssetTestSupport.h>
#include <AssetTestFixtures/MeshAssets.generated.h>
#include <AssetTestFixtures/MeshAssets.h>
#include <MeshSource/MeshSource.h>
#include <MeshSource/MeshSourceValidation.h>
#include <MeshSource/MeshTopologyIndex.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// MeshSource 시험 공용 도우미다(Docs/Design/MeshAnimationAssets.ko.md 12절): 표본 메시(사각형·
// 코너 공유·퇴화·모든 엔진 속성), 깊은 비교, 패키지 왕복(TestMeshAsset을 저장해 다른 Registry에
// 로드), 저장 데이터 변조 도우미.
namespace rocks::mesh_source_test {

using asset_fixture::TestMeshAsset;

inline TypeId MeshAssetType() noexcept {
    return GeneratedType<TestMeshAsset>::Id();
}

// 시험 입력용 비유한 float다(quiet NaN·+Inf 비트 패턴).
inline float TestQuietNaN() noexcept {
    return platform::BitCast<float>(0x7fc00000u);
}

inline float TestInfinity() noexcept {
    return platform::BitCast<float>(0x7f800000u);
}

// 사각형 하나: 정점·코너 넷, 변 넷, 묶음 하나, 다각형 하나(둘레 4), 삼각형 둘.
inline bool BuildQuad(MeshSource& mesh) noexcept {
    const Vec3 points[4] = {Vec3(0, 0, 0), Vec3(100, 0, 0), Vec3(100, 100, 0), Vec3(0, 100, 0)};
    uint32 corners[4] = {};
    const uint32 group = mesh.AddPolygonGroup().Value();
    for (uint32 index = 0; index < 4; ++index) {
        auto vertex = mesh.AddVertex(points[index]);
        if (!vertex) {
            return false;
        }

        auto corner = mesh.AddCorner(vertex.Value());
        if (!corner) {
            return false;
        }

        corners[index] = corner.Value();
    }

    for (uint32 index = 0; index < 4; ++index) {
        if (!mesh.AddEdge(index, (index + 1) % 4)) {
            return false;
        }
    }

    auto polygon = mesh.AddPolygon(Span<const uint32>(corners, 4), group);
    return polygon && mesh.AddTriangle(corners[0], corners[1], corners[2], polygon.Value()) &&
           mesh.AddTriangle(corners[0], corners[2], corners[3], polygon.Value());
}

// 코너 공유와 퇴화를 함께 담은 메시다: 변 하나를 공유하는 두 사각형이 그 변의 두 코너를 공유하고
// (매끈한 면), 셋째 다각형은 두 코너가 같은 정점을 가리키는 퇴화 삼각형(둘레의 같은 정점 사이 변은
// 건너뜀)이며, 넷째 다각형은 같은 코너를 두 번 쓰는 삼각형이다. 묶음 둘.
//
//   v3 --- v2 --- v5
//   |  p0  |  p1  |
//   v0 --- v1 --- v4
inline bool BuildSharedMesh(MeshSource& mesh) noexcept {
    const Vec3 points[6] = {Vec3(0, 0, 0),   Vec3(100, 0, 0), Vec3(100, 100, 0),
                            Vec3(0, 100, 0), Vec3(200, 0, 0), Vec3(200, 100, 0)};
    for (const Vec3& point : points) {
        if (!mesh.AddVertex(point)) {
            return false;
        }
    }

    // 코너 0..5 = 정점 0..5(공유 코너 1·2), 코너 6 = 정점 0(퇴화 삼각형용 같은 정점 둘째 코너).
    for (uint32 vertex = 0; vertex < 6; ++vertex) {
        if (!mesh.AddCorner(vertex)) {
            return false;
        }
    }

    if (!mesh.AddCorner(0)) {
        return false;
    }

    const uint32 edges[][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {1, 4}, {4, 5}, {5, 2}};
    for (const auto& edge : edges) {
        if (!mesh.AddEdge(edge[1], edge[0])) {
            return false;
        }
    }

    const uint32 groupA = mesh.AddPolygonGroup().Value();
    const uint32 groupB = mesh.AddPolygonGroup().Value();
    const uint32 quadA[] = {0, 1, 2, 3};
    const uint32 quadB[] = {1, 4, 5, 2};
    // 퇴화: 코너 0(정점 0)·6(정점 0)·1(정점 1) — 둘레 0→0은 건너뛰고 0→1·1→0은 변 (0,1).
    const uint32 sliver[] = {0, 6, 1};
    // 같은 코너 반복: 코너 1·1·4 — 둘레 1→1 건너뜀, 1→4·4→1은 변 (1,4).
    const uint32 repeat[] = {1, 1, 4};
    auto p0 = mesh.AddPolygon(quadA, groupA);
    auto p1 = mesh.AddPolygon(quadB, groupA);
    auto p2 = mesh.AddPolygon(sliver, groupB);
    auto p3 = mesh.AddPolygon(repeat, groupB);
    if (!p0 || !p1 || !p2 || !p3) {
        return false;
    }

    return mesh.AddTriangle(0, 1, 2, p0.Value()) && mesh.AddTriangle(0, 2, 3, p0.Value()) &&
           mesh.AddTriangle(1, 4, 5, p1.Value()) && mesh.AddTriangle(1, 5, 2, p1.Value()) &&
           mesh.AddTriangle(0, 6, 1, p2.Value()) && mesh.AddTriangle(1, 1, 4, p3.Value());
}

// 표본 이름 값(Name 속성용)이다. 비ASCII NFC 포함.
inline constexpr StringView SampleNames[] = {"Body", "Visor", "\xED\x8C\x94", "Trim_7"};

// BuildSharedMesh에 엔진 이름 속성 전부와 사용자 속성(여러 채널 UInt32·Bool·Name)을 더한다. morph
// 값에는 -0.0 성분을 넣는다.
inline bool AddRichAttributes(MeshSource& mesh) noexcept {
    const uint32 corners = mesh.Count(MeshElement::Corner);
    const uint32 vertices = mesh.Count(MeshElement::Vertex);
    auto normal =
        mesh.AddAttribute(MeshElement::Corner, MeshAttributeName::Normal, MeshValueType::Vec3, 1);
    auto tangent =
        mesh.AddAttribute(MeshElement::Corner, MeshAttributeName::Tangent, MeshValueType::Vec3, 1);
    auto sign = mesh.AddAttribute(MeshElement::Corner, MeshAttributeName::BinormalSign,
                                  MeshValueType::Float, 1);
    auto color =
        mesh.AddAttribute(MeshElement::Corner, MeshAttributeName::Color, MeshValueType::Vec4, 1);
    auto uv =
        mesh.AddAttribute(MeshElement::Corner, MeshAttributeName::TexCoord, MeshValueType::Vec2, 2);
    auto hard =
        mesh.AddAttribute(MeshElement::Edge, MeshAttributeName::IsHard, MeshValueType::Bool, 1);
    auto part = mesh.AddAttribute(MeshElement::Polygon, MeshAttributeName::PartName,
                                  MeshValueType::Name, 1);
    auto slot = mesh.AddAttribute(MeshElement::PolygonGroup, MeshAttributeName::MaterialSlotName,
                                  MeshValueType::Name, 1);
    auto skin = mesh.AddAttribute(MeshElement::Vertex, MeshAttributeName::SkinWeights,
                                  MeshValueType::BoneInfluences, 1);
    auto cloth = mesh.AddAttribute(MeshElement::Vertex, "SkinWeights-Cloth",
                                   MeshValueType::BoneInfluences, 1);
    auto morph =
        mesh.AddAttribute(MeshElement::Vertex, "Morph-Smile", MeshValueType::SparseVec3, 1);
    auto morphNormal =
        mesh.AddAttribute(MeshElement::Corner, "MorphNormal-Smile", MeshValueType::SparseVec3, 1);
    auto ids = mesh.AddAttribute(MeshElement::Triangle, "SourceIds", MeshValueType::UInt32, 2);
    auto flags = mesh.AddAttribute(MeshElement::Vertex, "Pinned", MeshValueType::Bool, 3);
    auto tags = mesh.AddAttribute(MeshElement::Corner, "Tags", MeshValueType::Name, 2);
    // 엔진 Color가 아닌 코너 Vec4(기본값 0).
    auto tint = mesh.AddAttribute(MeshElement::Corner, "Tint", MeshValueType::Vec4, 1);
    if (!normal || !tangent || !sign || !color || !uv || !hard || !part || !slot || !skin ||
        !cloth || !morph || !morphNormal || !ids || !flags || !tags || !tint) {
        return false;
    }

    for (uint32 corner = 0; corner < corners; ++corner) {
        const float f = static_cast<float>(corner);
        mesh.SetVec3(normal.Value(), 0, corner, Vec3(0.0f, 0.0f, 1.0f));
        mesh.SetVec3(tangent.Value(), 0, corner, Vec3(1.0f, 0.0f, 0.0f));
        mesh.SetFloat(sign.Value(), 0, corner, corner % 2 == 0 ? 1.0f : -1.0f);
        mesh.SetVec4(color.Value(), 0, corner, Vec4(0.25f * f, 0.5f, 0.75f, 1.0f));
        mesh.SetVec2(uv.Value(), 0, corner, Vec2(f / 8.0f, 1.0f - f / 8.0f));
        mesh.SetVec2(uv.Value(), 1, corner, Vec2(f, -f));
        mesh.SetVec4(tint.Value(), 0, corner, Vec4(f, 0.0f, -f, 0.5f));
        if (!mesh.SetNameValue(tags.Value(), 0, corner, SampleNames[corner % 4]) ||
            !mesh.SetNameValue(tags.Value(), 1, corner, corner % 3 == 0 ? "" : "Seam")) {
            return false;
        }
    }

    mesh.BoolValues(hard.Value(), 0)[1] = 1;
    mesh.BoolValues(hard.Value(), 0)[4] = 1;
    for (uint32 polygon = 0; polygon < mesh.Count(MeshElement::Polygon); ++polygon) {
        if (!mesh.SetNameValue(part.Value(), 0, polygon, SampleNames[polygon % 4])) {
            return false;
        }
    }

    if (!mesh.SetNameValue(slot.Value(), 0, 0, "Paint") ||
        !mesh.SetNameValue(slot.Value(), 0, 1, "Glass")) {
        return false;
    }

    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        const BoneInfluence two[] = {{static_cast<uint16>(vertex), 40000},
                                     {static_cast<uint16>(vertex + 1), 25535}};
        const BoneInfluence one[] = {{3, BoneWeightTotal}};
        if (!mesh.SetInfluences(skin.Value(), vertex,
                                vertex % 3 == 2 ? Span<const BoneInfluence>(one)
                                                : Span<const BoneInfluence>(two))) {
            return false;
        }

        if (vertex % 2 == 0 && !mesh.SetInfluences(cloth.Value(), vertex, one)) {
            return false;
        }

        mesh.BoolValues(flags.Value(), vertex % 3)[vertex] = 1;
    }

    if (!mesh.SetSparseValue(morph.Value(), 1, Vec3(1.5f, -0.0f, 0.0f)) ||
        !mesh.SetSparseValue(morph.Value(), 4, Vec3(-0.0f, -0.0f, -0.0f)) ||
        !mesh.SetSparseValue(morphNormal.Value(), 2, Vec3(0.0f, 0.25f, 0.0f))) {
        return false;
    }

    for (uint32 triangle = 0; triangle < mesh.Count(MeshElement::Triangle); ++triangle) {
        mesh.UInt32Values(ids.Value(), 0)[triangle] = 1000 + triangle;
        mesh.UInt32Values(ids.Value(), 1)[triangle] = 0xffff0000u ^ triangle;
    }

    return true;
}

inline bool BuildRichMesh(MeshSource& mesh) noexcept {
    return BuildSharedMesh(mesh) && AddRichAttributes(mesh);
}

inline bool SameFloatBits(Span<const float> left, Span<const float> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || platform::MemoryEquals(left.Data(), right.Data(), left.ByteCount()));
}

template <class T>
bool SameSpan(Span<const T> left, Span<const T> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (!(left[index] == right[index])) {
            return false;
        }
    }

    return true;
}

inline bool SameAttribute(const MeshSource& left, uint32 a, const MeshSource& right,
                          uint32 b) noexcept {
    const MeshAttributeInfo info = left.AttributeInfo(a);
    const MeshAttributeInfo other = right.AttributeInfo(b);
    if (info.element != other.element || info.type != other.type ||
        info.channels != other.channels || !(info.name == other.name)) {
        return false;
    }

    const uint32 count = left.Count(info.element);
    for (uint32 channel = 0; channel < info.channels; ++channel) {
        switch (info.type) {
        case MeshValueType::Float:
        case MeshValueType::Vec2:
        case MeshValueType::Vec3:
        case MeshValueType::Vec4:
            if (!SameFloatBits(left.FloatValues(a, channel), right.FloatValues(b, channel))) {
                return false;
            }

            break;
        case MeshValueType::UInt32:
            if (!SameSpan(left.UInt32Values(a, channel), right.UInt32Values(b, channel))) {
                return false;
            }

            break;
        case MeshValueType::Bool:
            if (!SameSpan(left.BoolValues(a, channel), right.BoolValues(b, channel))) {
                return false;
            }

            break;
        case MeshValueType::Name:
            for (uint32 index = 0; index < count; ++index) {
                if (!(left.NameValue(a, channel, index) == right.NameValue(b, channel, index))) {
                    return false;
                }
            }

            break;
        case MeshValueType::BoneInfluences:
            for (uint32 index = 0; index < count; ++index) {
                if (!SameSpan(left.Influences(a, index), right.Influences(b, index))) {
                    return false;
                }
            }

            break;
        case MeshValueType::SparseVec3: {
            const Span<const MeshSparseVec3> l = left.SparseValues(a);
            const Span<const MeshSparseVec3> r = right.SparseValues(b);
            if (l.Size() != r.Size()) {
                return false;
            }

            for (usize index = 0; index < l.Size(); ++index) {
                if (l[index].index != r[index].index ||
                    !SameFloatBits(Span<const float>(&l[index].value.x, 3),
                                   Span<const float>(&r[index].value.x, 3))) {
                    return false;
                }
            }

            break;
        }
        case MeshValueType::Invalid:
            return false;
        }
    }

    return true;
}

// 두 메시가 같은지 본다(요소 수·위상·위치 비트·속성 — 속성은 (element, 이름)으로 짝짓는다).
inline bool SameMesh(const MeshSource& left, const MeshSource& right) noexcept {
    for (uint32 kind = 1; kind <= MeshElementKindCount; ++kind) {
        if (left.Count(static_cast<MeshElement>(kind)) !=
            right.Count(static_cast<MeshElement>(kind))) {
            return false;
        }
    }

    if (left.Positions().Size() != right.Positions().Size()) {
        return false;
    }

    for (usize vertex = 0; vertex < left.Positions().Size(); ++vertex) {
        if (!SameFloatBits(Span<const float>(&left.Positions()[vertex].x, 3),
                           Span<const float>(&right.Positions()[vertex].x, 3))) {
            return false;
        }
    }

    if (!SameSpan(left.CornerVertices(), right.CornerVertices()) ||
        !SameSpan(left.PolygonCornerEnds(), right.PolygonCornerEnds()) ||
        !SameSpan(left.PolygonCornerList(), right.PolygonCornerList()) ||
        !SameSpan(left.PolygonGroups(), right.PolygonGroups()) ||
        !SameSpan(left.TrianglePolygons(), right.TrianglePolygons()) ||
        left.Edges().Size() != right.Edges().Size() ||
        left.TriangleCorners().Size() != right.TriangleCorners().Size() ||
        left.AttributeCount() != right.AttributeCount()) {
        return false;
    }

    for (usize index = 0; index < left.Edges().Size(); ++index) {
        if (left.Edges()[index].first != right.Edges()[index].first ||
            left.Edges()[index].second != right.Edges()[index].second) {
            return false;
        }
    }

    for (usize index = 0; index < left.TriangleCorners().Size(); ++index) {
        for (uint32 slot = 0; slot < 3; ++slot) {
            if (left.TriangleCorners()[index].corners[slot] !=
                right.TriangleCorners()[index].corners[slot]) {
                return false;
            }
        }
    }

    for (uint32 attribute = 0; attribute < left.AttributeCount(); ++attribute) {
        const MeshAttributeInfo info = left.AttributeInfo(attribute);
        uint32 match = MeshNoAttribute;
        for (uint32 other = 0; other < right.AttributeCount(); ++other) {
            const MeshAttributeInfo candidate = right.AttributeInfo(other);
            if (candidate.element == info.element && candidate.name == info.name) {
                match = other;
            }
        }

        if (match == MeshNoAttribute || !SameAttribute(left, attribute, right, match)) {
            return false;
        }
    }

    return true;
}

// 패키지 하나의 왕복 결과다.
struct MeshPackage {
    explicit MeshPackage(Allocator& allocator) noexcept : bytes(allocator) {
    }

    RootRef<Object> root;
    ObjectHandle package{};
    ObjectHandle asset{};
    Array<uint8> bytes;
};

// source Registry에 id 패키지와 TestMeshAsset을 만들고 fill(asset, bulk)로 채운 뒤 kind·codec으로
// 쓰고 target에 로드한다. 쓰기·로드 실패면 false다(failure가 있으면 로드 진단).
template <class FillFn>
bool WriteAndLoad(asset_test::AssetEnv& env, uint64 id, PackageKind kind, BulkCodec codec,
                  FillFn&& fill, MeshPackage& out, AssetDiagnostic* failure = nullptr) noexcept {
    ObjectRegistry& source = *env.source;
    auto package = NewPackage(source, source.OwnerAccess(), asset_test::FixedId(id), kind);
    if (!package) {
        return false;
    }

    const ObjectHandle asset =
        asset_test::Make<TestMeshAsset>(source, MeshAssetType(), package.Value().Handle(), "");
    TestMeshAsset* pointer = asset_test::Pointer<TestMeshAsset>(source, asset);
    MemoryBulkSource bulk(env.allocator);
    if (pointer == nullptr || !fill(*pointer, bulk)) {
        return false;
    }

    PackageWriteOptions options =
        kind == PackageKind::Cooked ? asset_test::CookedOptions() : asset_test::EditorOptions();
    options.bulkSource = &bulk;
    options.bulkCodec = codec;
    out.bytes = asset_test::Write(env.allocator, source, package.Value().Handle(), options);
    if (out.bytes.IsEmpty()) {
        return false;
    }

    Array<uint8> files[1] = {asset_test::Copy(env.allocator, out.bytes.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    if (!asset_test::LoadAll(env, *env.target, files, kind, nullptr, roots, handles, failure)) {
        return false;
    }

    out.root = Move(roots[0]);
    out.package = handles[0];
    auto loaded = env.target->FindObject(env.target->OwnerAccess(), handles[0], StringView());
    if (!loaded) {
        return false;
    }

    out.asset = loaded.Value();
    return true;
}

inline TestMeshAsset* LoadedAsset(asset_test::AssetEnv& env, const MeshPackage& loaded) noexcept {
    return asset_test::Pointer<TestMeshAsset>(*env.target, loaded.asset);
}

inline Package* LoadedPackage(asset_test::AssetEnv& env, const MeshPackage& loaded) noexcept {
    return asset_test::Pointer<Package>(*env.target, loaded.package);
}

// mesh를 TestMeshAsset::mesh에 저장해 왕복하고 로드한 패키지로 out을 다시 만든다. diagnostic은
// Load의 진단이다. 패키지 쓰기·로드가 실패하면 InvalidArgument 아닌 NotFound를 돌려준다(시험이
// Load 실패와 가른다).
inline Result<void> RoundTripMesh(asset_test::AssetEnv& env, uint64 id, PackageKind kind,
                                  const MeshSource& mesh, MeshSource& out,
                                  MeshDiagnostic* diagnostic = nullptr,
                                  BulkCodec codec = BulkCodec::None) noexcept {
    MeshPackage loaded(env.allocator);
    const bool written = WriteAndLoad(
        env, id, kind, codec,
        [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
            return mesh.Store(bulk, env.names, asset.mesh).IsOk();
        },
        loaded);
    if (!written) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return out.Load(LoadedAsset(env, loaded)->mesh, *LoadedPackage(env, loaded), env.names,
                    diagnostic);
}

// TestMeshAsset::mesh를 mesh로 채운 뒤 edit(data, bulk)로 저장 데이터를 고치고 왕복해 Load한다.
// edit는 Store 결과와 그 bulk(raw byte를 ReadBulk로 읽고 AddCopy로 바꿀 수 있다)를 받는다.
template <class EditFn>
Result<void> LoadEdited(asset_test::AssetEnv& env, uint64 id, const MeshSource& mesh, EditFn&& edit,
                        MeshSource& out, MeshDiagnostic& diagnostic) noexcept {
    MeshPackage loaded(env.allocator);
    const bool written = WriteAndLoad(
        env, id, PackageKind::Editor, BulkCodec::None,
        [&](TestMeshAsset& asset, MemoryBulkSource& bulk) {
            return mesh.Store(bulk, env.names, asset.mesh).IsOk() && edit(asset.mesh, bulk);
        },
        loaded);
    if (!written) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return out.Load(LoadedAsset(env, loaded)->mesh, *LoadedPackage(env, loaded), env.names,
                    &diagnostic);
}

// bulk의 ref byte를 복사해 돌려준다.
inline Array<uint8> BulkCopy(Allocator& allocator, MemoryBulkSource& bulk,
                             const BulkDataRef& ref) noexcept {
    Array<uint8> copy(allocator);
    auto bytes = bulk.ReadBulk(ref);
    if (bytes) {
        (void)copy.TryAppend(bytes.Value());
    }

    return copy;
}

// data에서 (element, 이름)인 레코드 번호다(없으면 MeshNoAttribute).
inline uint32 FindRecord(asset_test::AssetEnv& env, const MeshSourceData& data, MeshElement element,
                         StringView name) noexcept {
    for (usize index = 0; index < data.attributes.Size(); ++index) {
        auto lease = env.names.Acquire(data.attributes[index].name);
        if (lease && data.attributes[index].element == element && lease.Value().View() == name) {
            return static_cast<uint32>(index);
        }
    }

    return MeshNoAttribute;
}

// 메시 검증 진단을 기대값과 대조한다.
inline bool HasIssue(const MeshDiagnostic& diagnostic, AssetDiagnosticCode code,
                     MeshIssue issue) noexcept {
    return diagnostic.code == code && diagnostic.issue == issue;
}

} // namespace rocks::mesh_source_test
