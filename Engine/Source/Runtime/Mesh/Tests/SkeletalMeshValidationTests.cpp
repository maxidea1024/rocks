#include <Base/Endian.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <GraphicsTypes/IndexFormat.h>
#include <GraphicsTypes/VertexFormat.h>
#include <Mesh/SkeletalMesh/SkeletalMeshValidation.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 스킨 메시 쿠킹 렌더 데이터 로드 검증 시험이다(Docs/Design/MeshAnimationAssets 6.7.1절 7): 정상
// 배치(영향 4·8칸, 8·16비트 번호 경계 256·257, 16비트 가중치, 프로필·morph, 서버 쿠킹), 스킨
// 규칙마다 하나씩 어기기, bulk 값(영향 번호·가중치 합·morph 정점·index), 메시 검증(본
// 계층·inverse-bind 크기), 확장 bounds.
using namespace rocks;
namespace skeletal_mesh_validation_tests_file {

using graphics::IndexFormat;
using graphics::VertexFormat;

uint32 Number(VertexFormat format) noexcept {
    return graphics::VertexFormatNumber(format);
}

uint64 AlignUp(uint64 value) noexcept {
    return (value + MeshRenderStreamAlignment - 1) & ~(MeshRenderStreamAlignment - 1);
}

// 배치 선택이다.
struct Shape {
    uint32 vertices = 4;
    uint32 indexCount = 6;
    uint32 influences = 4;
    usize boneMapSize = 2;
    bool wideWeights = false;
    uint32 profiles = 0;
    uint32 morphDeltas[2] = {0, 0};
    uint32 morphTargets = 0;
};

// 정상 스킨 LOD 하나의 배치를 만든다(UV 1채널 half, 색 없음, 16비트 index). 구간은 배치 순서로
// 16 byte 정렬해 잇는다.
void Layout(SkinnedRenderLod& lod, const Shape& shape) noexcept {
    lod.vertexCount = shape.vertices;
    lod.influencesPerVertex = shape.influences;
    uint64 offset = 0;
    auto place = [&](MeshRenderStream& stream, uint32 format, uint64 bytes) noexcept {
        offset = AlignUp(offset);
        stream.vertexFormat = format;
        stream.offset = offset;
        stream.byteSize = bytes;
        offset += bytes;
    };
    const uint64 vertices = shape.vertices;
    place(lod.positions, Number(VertexFormat::Float3), vertices * 12);
    place(lod.tangentBasis, Number(VertexFormat::Byte4Snorm), vertices * 8);
    lod.texCoords.Clear();
    MeshRenderStream uv;
    place(uv, Number(VertexFormat::Half2), vertices * 4);
    (void)lod.texCoords.TryPushBack(uv);
    lod.colors = MeshRenderStream{};
    const bool wideIndex = shape.boneMapSize > MeshBoneIndex8Limit;
    const uint32 indexFormat = Number(wideIndex ? VertexFormat::UShort4 : VertexFormat::UByte4);
    const uint32 weightFormat =
        Number(shape.wideWeights ? VertexFormat::UShort4Unorm : VertexFormat::UByte4Unorm);
    const uint64 elements = shape.influences / 4;
    const uint64 indexBytes = vertices * elements * (wideIndex ? 8 : 4);
    const uint64 weightBytes = vertices * elements * (shape.wideWeights ? 8 : 4);
    place(lod.boneIndices, indexFormat, indexBytes);
    place(lod.boneWeights, weightFormat, weightBytes);
    lod.skinWeightProfiles.Clear();
    for (uint32 profile = 0; profile < shape.profiles; ++profile) {
        SkinWeightProfileRenderLod value;
        place(value.boneIndices, indexFormat, indexBytes);
        place(value.boneWeights, weightFormat, weightBytes);
        (void)lod.skinWeightProfiles.TryPushBack(value);
    }

    lod.morphTargets.Clear();
    for (uint32 target = 0; target < shape.morphTargets; ++target) {
        SkinnedMorphTarget value;
        value.deltaCount = shape.morphDeltas[target];
        place(value.deltas, MeshRenderStreamNone,
              uint64{value.deltaCount} * SkinnedMorphDeltaBytes);
        (void)lod.morphTargets.TryPushBack(value);
    }

    lod.indexFormat = graphics::IndexFormatNumber(IndexFormat::UInt16);
    offset = AlignUp(offset);
    lod.indices.vertexFormat = MeshRenderStreamNone;
    lod.indices.offset = offset;
    lod.indices.byteSize = uint64{shape.indexCount} * 2;
    offset += lod.indices.byteSize;
    lod.data.bulkId = 1;
    lod.data.rawBytes = offset;
    lod.sections.Clear();
    MeshRenderSection section;
    section.triangleCount = shape.indexCount / 3;
    section.maxVertex = shape.vertices == 0 ? 0 : shape.vertices - 1;
    (void)lod.sections.TryPushBack(section);
    lod.boneMap.Clear();
    for (usize bone = 0; bone < shape.boneMapSize; ++bone) {
        (void)lod.boneMap.TryPushBack(static_cast<uint16>(bone));
    }

    lod.requiredBones.Clear();
    (void)lod.requiredBones.TryPushBack(0);
}

struct Fixture {
    Fixture() noexcept : data(allocator) {
        SkinnedRenderLod lod(allocator);
        Layout(lod, Shape{});
        (void)data.lods.TryPushBack(Move(lod));
        data.box = Box<double>(Vec3d(-1.0, -1.0, 0.0), Vec3d(1.0, 1.0, 0.0));
        data.sphereRadius = 1.5;
    }

    SkinnedRenderLod& Lod() noexcept {
        return data.lods[0];
    }

    SystemAllocator allocator;
    SkeletalMeshRenderData data;
};

constexpr usize BoneCount = 300;

bool Fails(const SkeletalMeshRenderData& data, MeshIssue issue, uint32 index, uint64 count = 0,
           usize bones = BoneCount) noexcept {
    MeshDiagnostic failure;
    auto checked = ValidateSkeletalMeshRenderData(data, bones, &failure);
    return !checked && failure.code == AssetDiagnosticCode::CorruptPackage &&
           failure.issue == issue && failure.index == index && failure.count == count;
}

// 정상: 영향 4·8칸, bone map 256(UByte4)·257(UShort4), 16비트 가중치, 프로필 둘, morph 둘(0개
// 포함), 서버 쿠킹(data 없음 — 스킨 stream도 비고 칸 수 0), LOD 0개.
TEST(SkeletalMeshValidation_AcceptsValidLayouts) {
    Fixture fixture;
    CHECK(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));
    Shape shape;
    shape.influences = 8;
    shape.boneMapSize = MeshBoneIndex8Limit;
    shape.wideWeights = true;
    shape.profiles = 2;
    shape.morphTargets = 2;
    shape.morphDeltas[0] = 3;
    Layout(fixture.Lod(), shape);
    CHECK(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));
    shape.boneMapSize = MeshBoneIndex8Limit + 1;
    Layout(fixture.Lod(), shape);
    CHECK(fixture.Lod().boneIndices.vertexFormat == Number(VertexFormat::UShort4));
    CHECK(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));

    SkinnedRenderLod server(fixture.allocator);
    server.vertexCount = 4;
    (void)server.boneMap.TryPushBack(0);
    SkinnedMorphTarget target;
    (void)server.morphTargets.TryPushBack(target);
    MeshRenderSection section;
    section.triangleCount = 2;
    section.maxVertex = 3;
    (void)server.sections.TryPushBack(section);
    REQUIRE(fixture.data.lods.TryPushBack(Move(server)));
    CHECK(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));
    fixture.data.lods.Clear();
    CHECK(ValidateSkeletalMeshRenderData(fixture.data, 0));
}

// 스킨 규칙 어기기: 칸 수, 번호 형식(bone map 크기와 어긋남), 가중치 형식, 스킨 stream 크기, 구간
// 순서, 프로필 형식·크기, morph 형식·크기, bone map·필요 본 순서·범위, 서버 LOD의 스킨 stream, LOD
// 수.
TEST(SkeletalMeshValidation_RejectsSkinnedViolations) {
    Fixture fixture;
    SkinnedRenderLod& lod = fixture.Lod();
    lod.influencesPerVertex = 6;
    CHECK(Fails(fixture.data, MeshIssue::RenderInfluenceLayout, 0));
    Layout(lod, Shape{});
    lod.boneIndices.vertexFormat = Number(VertexFormat::UShort4);
    CHECK(Fails(fixture.data, MeshIssue::RenderInfluenceLayout, 0));
    Layout(lod, Shape{});
    lod.boneWeights.vertexFormat = Number(VertexFormat::Byte4Snorm);
    CHECK(Fails(fixture.data, MeshIssue::RenderInfluenceLayout, 0));
    Layout(lod, Shape{});
    lod.boneWeights.byteSize += 4;
    CHECK(Fails(fixture.data, MeshIssue::RenderStreamSize, 0));
    Layout(lod, Shape{});
    lod.boneWeights.offset = lod.boneIndices.offset;
    CHECK(Fails(fixture.data, MeshIssue::RenderStreamRange, 0));

    Shape profiled;
    profiled.profiles = 2;
    Layout(lod, profiled);
    lod.skinWeightProfiles[1].boneWeights.vertexFormat = Number(VertexFormat::UShort4Unorm);
    CHECK(Fails(fixture.data, MeshIssue::RenderSkinWeightProfile, 0, 1));
    Layout(lod, profiled);
    lod.skinWeightProfiles[0].boneIndices.byteSize -= 4;
    CHECK(Fails(fixture.data, MeshIssue::RenderSkinWeightProfile, 0, 0));

    Shape morphed;
    morphed.morphTargets = 2;
    morphed.morphDeltas[0] = 2;
    morphed.morphDeltas[1] = 1;
    Layout(lod, morphed);
    lod.morphTargets[1].deltas.vertexFormat = Number(VertexFormat::Float1);
    CHECK(Fails(fixture.data, MeshIssue::RenderMorphTarget, 0, 1));
    Layout(lod, morphed);
    lod.morphTargets[0].deltaCount = 3;
    CHECK(Fails(fixture.data, MeshIssue::RenderMorphTarget, 0, 0));

    Layout(lod, Shape{});
    lod.boneMap[1] = 0;
    CHECK(Fails(fixture.data, MeshIssue::RenderBoneMap, 0, 1));
    Layout(lod, Shape{});
    CHECK(Fails(fixture.data, MeshIssue::RenderBoneMap, 0, 1, 1));
    Layout(lod, Shape{});
    lod.requiredBones[0] = 400;
    CHECK(Fails(fixture.data, MeshIssue::RenderBoneMap, 0, 0));

    SkinnedRenderLod server(fixture.allocator);
    server.influencesPerVertex = 4;
    Layout(lod, Shape{});
    REQUIRE(fixture.data.lods.TryPushBack(Move(server)));
    CHECK(Fails(fixture.data, MeshIssue::RenderInfluenceLayout, 1));
    fixture.data.lods[1].influencesPerVertex = 0;
    SkinnedMorphTarget target;
    target.deltas.byteSize = 20;
    REQUIRE(fixture.data.lods[1].morphTargets.TryPushBack(target));
    CHECK(Fails(fixture.data, MeshIssue::RenderMorphTarget, 1, 0));
    fixture.data.lods.Clear();
    for (uint32 index = 0; index <= SkeletalMeshMaxLods; ++index) {
        SkinnedRenderLod extra(fixture.allocator);
        REQUIRE(fixture.data.lods.TryPushBack(Move(extra)));
    }

    CHECK(Fails(fixture.data, MeshIssue::RenderLodCount, MeshNoIndex));
}

// bulk 값: 정상(빈 칸 (0, 0) 포함), 영향 번호 ≥ bone map 크기, 가중치 합, 프로필 값, morph 정점
// 범위· 순서, index 값.
TEST(SkeletalMeshValidation_BulkValues) {
    Fixture fixture;
    Shape shape;
    shape.profiles = 1;
    shape.morphTargets = 1;
    shape.morphDeltas[0] = 2;
    SkinnedRenderLod& lod = fixture.Lod();
    Layout(lod, shape);
    REQUIRE(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));
    Array<uint8> bytes(fixture.allocator);
    REQUIRE(bytes.TryResize(lod.data.rawBytes));
    platform::MemorySet(bytes.Data(), 0, bytes.Size());
    // 정점마다 (번호 1, 가중치 200) + (0, 55) + 빈 칸 둘.
    auto fillInfluences = [&](const MeshRenderStream& indices,
                              const MeshRenderStream& weights) noexcept {
        for (uint32 vertex = 0; vertex < lod.vertexCount; ++vertex) {
            bytes[indices.offset + vertex * 4] = 1;
            bytes[weights.offset + vertex * 4] = 200;
            bytes[weights.offset + vertex * 4 + 1] = 55;
        }
    };
    fillInfluences(lod.boneIndices, lod.boneWeights);
    fillInfluences(lod.skinWeightProfiles[0].boneIndices, lod.skinWeightProfiles[0].boneWeights);
    StoreLittleEndian<uint32>(bytes.Data() + lod.morphTargets[0].deltas.offset, 1);
    StoreLittleEndian<uint32>(bytes.Data() + lod.morphTargets[0].deltas.offset + 20, 3);
    const uint16 triangles[6] = {0, 1, 2, 2, 1, 3};
    for (uint32 at = 0; at < 6; ++at) {
        StoreLittleEndian<uint16>(bytes.Data() + lod.indices.offset + at * 2, triangles[at]);
    }

    auto fails = [&](MeshIssue issue, uint64 count) noexcept {
        MeshDiagnostic failure;
        auto checked = ValidateSkinnedRenderLodBulk(lod, 0, bytes.View(), &failure);
        return !checked && failure.code == AssetDiagnosticCode::CorruptPackage &&
               failure.issue == issue && failure.count == count;
    };
    CHECK(ValidateSkinnedRenderLodBulk(lod, 0, bytes.View()));
    bytes[lod.boneIndices.offset + 4] = 2;
    CHECK(fails(MeshIssue::RenderBoneIndexValue, 4));
    bytes[lod.boneIndices.offset + 4] = 1;
    bytes[lod.boneWeights.offset + 8] = 199;
    CHECK(fails(MeshIssue::RenderSkinWeightSum, 2));
    bytes[lod.boneWeights.offset + 8] = 200;
    bytes[lod.skinWeightProfiles[0].boneIndices.offset + 3] = 9;
    bytes[lod.skinWeightProfiles[0].boneWeights.offset + 3] = 0;
    CHECK(fails(MeshIssue::RenderBoneIndexValue, 3));
    bytes[lod.skinWeightProfiles[0].boneIndices.offset + 3] = 0;
    StoreLittleEndian<uint32>(bytes.Data() + lod.morphTargets[0].deltas.offset + 20, 1);
    CHECK(fails(MeshIssue::RenderMorphVertex, 1));
    StoreLittleEndian<uint32>(bytes.Data() + lod.morphTargets[0].deltas.offset + 20, 4);
    CHECK(fails(MeshIssue::RenderMorphVertex, 1));
    StoreLittleEndian<uint32>(bytes.Data() + lod.morphTargets[0].deltas.offset + 20, 3);
    StoreLittleEndian<uint16>(bytes.Data() + lod.indices.offset + 2, 4);
    CHECK(fails(MeshIssue::RenderIndexValue, 1));
    StoreLittleEndian<uint16>(bytes.Data() + lod.indices.offset + 2, 1);
    CHECK(ValidateSkinnedRenderLodBulk(lod, 0, bytes.View()));
    // 16비트 가중치는 합이 65535다.
    shape.wideWeights = true;
    shape.profiles = 0;
    shape.morphTargets = 0;
    Layout(lod, shape);
    REQUIRE(bytes.TryResize(lod.data.rawBytes));
    platform::MemorySet(bytes.Data(), 0, bytes.Size());
    for (uint32 vertex = 0; vertex < lod.vertexCount; ++vertex) {
        StoreLittleEndian<uint16>(bytes.Data() + lod.boneWeights.offset + vertex * 8, 65535);
    }

    CHECK(ValidateSkinnedRenderLodBulk(lod, 0, bytes.View()));
    StoreLittleEndian<uint16>(bytes.Data() + lod.boneWeights.offset, 255);
    CHECK(fails(MeshIssue::RenderSkinWeightSum, 0));
}

// 메시 검증: 본 계층 위반(InvalidBoneHierarchy, index = 본), inverse-bind 크기(쿠킹 LOD가 있으면 본
// 수 × 64 byte, 없으면 0 — RenderInverseBind).
TEST(SkeletalMeshValidation_MeshBonesAndInverseBind) {
    SystemAllocator allocator;
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    SkeletalMesh mesh(allocator);
    CHECK(ValidateSkeletalMesh(mesh, allocator));
    BoneInfo unnamed(allocator);
    REQUIRE(mesh.bones.bones.TryPushBack(Move(unnamed)));
    REQUIRE(mesh.bones.referencePose.TryPushBack(Transform::Identity()));
    MeshDiagnostic failure;
    CHECK(!ValidateSkeletalMesh(mesh, allocator, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.issue == MeshIssue::InvalidBoneHierarchy && failure.index == 0);
    mesh.bones.bones[0].name = names.TryIntern("root").Value();
    CHECK(ValidateSkeletalMesh(mesh, allocator));
    // 에디터 패키지(LOD 없음)에 inverse-bind가 있으면 손상이다.
    mesh.renderData.inverseBindMatrices.rawBytes = SkinnedInverseBindMatrixBytes;
    CHECK(!ValidateSkeletalMesh(mesh, allocator, &failure));
    CHECK(failure.issue == MeshIssue::RenderInverseBind);
    // 쿠킹 LOD가 있으면 본 수 × 64 byte이고 bulk가 있어야 한다.
    SkinnedRenderLod lod(allocator);
    Layout(lod, Shape{});
    lod.boneMap.Clear();
    (void)lod.boneMap.TryPushBack(0);
    REQUIRE(mesh.renderData.lods.TryPushBack(Move(lod)));
    CHECK(!ValidateSkeletalMesh(mesh, allocator, &failure));
    CHECK(failure.issue == MeshIssue::RenderInverseBind);
    mesh.renderData.inverseBindMatrices.bulkId = 2;
    CHECK(ValidateSkeletalMesh(mesh, allocator));
    mesh.renderData.inverseBindMatrices.rawBytes = 2 * SkinnedInverseBindMatrixBytes;
    CHECK(!ValidateSkeletalMesh(mesh, allocator, &failure));
    CHECK(failure.issue == MeshIssue::RenderInverseBind && failure.index == MeshNoIndex);
    CHECK(names.Shutdown());
}

// 확장 bounds는 정적 메시와 같은 식이다(두 확장이 0이면 그대로, 아니면 상자를 늘리고 반 대각선
// 반지름).
TEST(SkeletalMeshValidation_ExtendedBounds) {
    SystemAllocator allocator;
    SkeletalMesh mesh(allocator);
    mesh.renderData.box = Box<double>(Vec3d(0.0, 0.0, 0.0), Vec3d(2.0, 2.0, 2.0));
    mesh.renderData.sphereRadius = 1.0;
    // UE CalculateExtendedBounds(스킨)는 확장이 0이어도 반지름을 상자 반 대각선(√3)으로 다시
    // 만든다.
    const MeshBounds same = SkeletalMeshExtendedBounds(mesh);
    CHECK(same.box.max.z == 2.0 && same.sphereRadius > 1.732050 && same.sphereRadius < 1.732051);
    mesh.positiveBoundsExtension = Vec3d(0.0, 0.0, 2.0);
    const MeshBounds extended = SkeletalMeshExtendedBounds(mesh);
    CHECK(extended.box.max.z == 4.0 && extended.box.min.z == 0.0);
    CHECK(extended.sphereRadius > 2.449489 && extended.sphereRadius < 2.449490);
}

// 리뷰 반영 보강: 서버 LOD의 morph 항목 수·프로필·영향 stream, 배치 순서(프로필·morph·index),
// bulk의 8칸· 16비트 번호·프로필 합·가중치 0 칸·진단 attribute, 서버 LOD bulk.
TEST(SkeletalMeshValidation_ReviewCases) {
    Fixture fixture;
    SkinnedRenderLod server(fixture.allocator);
    SkinnedMorphTarget target;
    target.deltaCount = 5;
    REQUIRE(server.morphTargets.TryPushBack(target));
    REQUIRE(fixture.data.lods.TryPushBack(Move(server)));
    CHECK(Fails(fixture.data, MeshIssue::RenderMorphTarget, 1, 0));
    SkinnedRenderLod& serverLod = fixture.data.lods[1];
    serverLod.morphTargets.Clear();
    SkinWeightProfileRenderLod profile;
    profile.boneWeights.byteSize = 4;
    REQUIRE(serverLod.skinWeightProfiles.TryPushBack(profile));
    CHECK(Fails(fixture.data, MeshIssue::RenderSkinWeightProfile, 1, 0));
    serverLod.skinWeightProfiles.Clear();
    serverLod.boneIndices.vertexFormat = Number(VertexFormat::UByte4);
    CHECK(Fails(fixture.data, MeshIssue::RenderInfluenceLayout, 1));
    serverLod.boneIndices = MeshRenderStream{};
    serverLod.data.rawBytes = 16;
    CHECK(Fails(fixture.data, MeshIssue::RenderStreamRange, 1));
    serverLod.data.rawBytes = 0;
    CHECK(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));
    // 서버 LOD bulk: 빈 byte만 받는다.
    CHECK(ValidateSkinnedRenderLodBulk(serverLod, 1, {}));
    const uint8 stray[1] = {0};
    CHECK(!ValidateSkinnedRenderLodBulk(serverLod, 1, Span<const uint8>(stray)));
    fixture.data.lods.Clear();

    // 배치 순서: 프로필이 기본 영향 앞, morph가 프로필 앞, index가 morph 앞.
    Shape shape;
    shape.profiles = 1;
    shape.morphTargets = 1;
    shape.morphDeltas[0] = 1;
    SkinnedRenderLod lod(fixture.allocator);
    Layout(lod, shape);
    REQUIRE(fixture.data.lods.TryPushBack(Move(lod)));
    SkinnedRenderLod& placed = fixture.data.lods[0];
    CHECK(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));
    const MeshRenderStream profileIndices = placed.skinWeightProfiles[0].boneIndices;
    placed.skinWeightProfiles[0].boneIndices = placed.boneIndices;
    CHECK(Fails(fixture.data, MeshIssue::RenderStreamRange, 0));
    placed.skinWeightProfiles[0].boneIndices = profileIndices;
    const MeshRenderStream deltas = placed.morphTargets[0].deltas;
    placed.morphTargets[0].deltas.offset = placed.skinWeightProfiles[0].boneIndices.offset;
    CHECK(Fails(fixture.data, MeshIssue::RenderStreamRange, 0));
    placed.morphTargets[0].deltas = deltas;
    const MeshRenderStream indices = placed.indices;
    placed.indices.offset = deltas.offset;
    CHECK(Fails(fixture.data, MeshIssue::RenderStreamRange, 0));
    placed.indices = indices;
    CHECK(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));

    // 8칸·16비트 번호(bone map 257개) bulk: 두 원소를 잇고 번호 ≥ 257은 실패, 가중치 0인 map 안
    // 번호는 된다.
    Shape wide;
    wide.influences = 8;
    wide.boneMapSize = MeshBoneIndex8Limit + 1;
    wide.profiles = 1;
    wide.vertices = 2;
    wide.indexCount = 3;
    Layout(placed, wide);
    placed.sections[0].maxVertex = 1;
    REQUIRE(ValidateSkeletalMeshRenderData(fixture.data, BoneCount));
    Array<uint8> bytes(fixture.allocator);
    REQUIRE(bytes.TryResize(placed.data.rawBytes));
    platform::MemorySet(bytes.Data(), 0, bytes.Size());
    auto fill = [&](const MeshRenderStream& indexStream,
                    const MeshRenderStream& weightStream) noexcept {
        for (uint32 vertex = 0; vertex < 2; ++vertex) {
            // 칸 0: 번호 256 가중치 255, 칸 5(둘째 원소): 번호 3 가중치 0.
            StoreLittleEndian<uint16>(bytes.Data() + indexStream.offset + (vertex * 8 + 0) * 2,
                                      256);
            bytes[weightStream.offset + vertex * 8 + 0] = 255;
            StoreLittleEndian<uint16>(bytes.Data() + indexStream.offset + (vertex * 8 + 5) * 2, 3);
        }
    };
    fill(placed.boneIndices, placed.boneWeights);
    fill(placed.skinWeightProfiles[0].boneIndices, placed.skinWeightProfiles[0].boneWeights);
    const uint16 triangle[3] = {0, 1, 1};
    for (uint32 at = 0; at < 3; ++at) {
        StoreLittleEndian<uint16>(bytes.Data() + placed.indices.offset + at * 2, triangle[at]);
    }

    CHECK(ValidateSkinnedRenderLodBulk(placed, 0, bytes.View()));
    MeshDiagnostic failure;
    StoreLittleEndian<uint16>(bytes.Data() + placed.boneIndices.offset + (8 + 6) * 2, 257);
    bytes[placed.boneWeights.offset + 8 + 6] = 0;
    CHECK(!ValidateSkinnedRenderLodBulk(placed, 0, bytes.View(), &failure));
    CHECK(failure.issue == MeshIssue::RenderBoneIndexValue && failure.count == 14 &&
          failure.attribute == MeshNoAttribute);
    StoreLittleEndian<uint16>(bytes.Data() + placed.boneIndices.offset + (8 + 6) * 2, 0);
    // 프로필 가중치 합 실패는 attribute = 프로필 번호다.
    bytes[placed.skinWeightProfiles[0].boneWeights.offset + 8] = 254;
    CHECK(!ValidateSkinnedRenderLodBulk(placed, 0, bytes.View(), &failure));
    CHECK(failure.issue == MeshIssue::RenderSkinWeightSum && failure.count == 1 &&
          failure.attribute == 0);
}

} // namespace skeletal_mesh_validation_tests_file
