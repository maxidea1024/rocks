#include <Base/Endian.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <GraphicsTypes/VertexFormat.h>
#include <Mesh/SkeletalMesh/SkeletalMeshSourceLod.h>
#include <Mesh/SkeletalMesh/SkeletalMeshValidation.h>
#include <MeshTools/SkinnedMeshPacking.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 스킨 메시 쿠킹 LOD 패킹 시험이다(Docs/Design/MeshAnimationAssets 6.7.1절 6·6.7.2절 1·7): 배치
// 순서(정적 stream → 영향 번호 → 가중치 → 프로필 → morph → index)와 16 byte 정렬, 영향 4·8칸, 번호
// UByte4·UShort4 경계(bone map 256·257개), 가중치 8·16비트, 프로필·morph 이름과 byte(Δ half, w =
// 0), LOD 머티리얼 맵 굽기, 인자 오류, 결정성, 로드 검증 통과.
using namespace rocks;
namespace skinned_mesh_packing_tests_file {

using graphics::VertexFormat;

namespace {

uint32 Number(VertexFormat format) noexcept {
    return graphics::VertexFormatNumber(format);
}

bool Aligned(const MeshRenderStream& stream) noexcept {
    return stream.offset % MeshRenderStreamAlignment == 0;
}

// 정점 vertices개(UV 한 채널, 색 없음, 삼각형 vertices / 3개, 섹션 하나)의 합성 렌더 결과에 영향
// slots칸(정점 v의 첫 칸 = (v % bones, 합), 나머지 (0, 0)), bone map·필요 본 0..bones−1을 채운다.
void Fill(SkinnedMeshBuildResult& built, uint32 vertices, uint32 slots, uint32 bones,
          bool wideWeights) noexcept {
    StaticMeshBuildResult& mesh = built.mesh;
    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        const float x = static_cast<float>(vertex);
        CHECK(mesh.positions.TryPushBack(Vec3(x, 0.0f, 0.0f)));
        CHECK(mesh.normals.TryPushBack(Vec3(0.0f, 0.0f, 1.0f)));
        CHECK(mesh.tangents.TryPushBack(Vec3(1.0f, 0.0f, 0.0f)));
        CHECK(mesh.binormalSigns.TryPushBack(1.0f));
        CHECK(mesh.texCoords.TryPushBack(Vec2(0.5f, x)));
    }

    mesh.texCoordChannels = 1;
    const uint32 triangles = vertices / 3;
    for (uint32 index = 0; index < triangles * 3; ++index) {
        CHECK(mesh.indices.TryPushBack(index));
    }

    MeshBuildSection section;
    section.materialIndex = 0;
    section.triangleCount = triangles;
    section.maxVertex = triangles == 0 ? 0 : triangles * 3 - 1;
    CHECK(mesh.sections.TryPushBack(section));
    built.influencesPerVertex = slots;
    built.weightTotal = wideWeights ? BoneWeightTotal : SkinWeightTotal8;
    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        for (uint32 slot = 0; slot < slots; ++slot) {
            const BoneInfluence influence =
                slot == 0 ? BoneInfluence{static_cast<uint16>(vertex % bones), built.weightTotal}
                          : BoneInfluence{0, 0};
            CHECK(built.influences.TryPushBack(influence));
        }
    }

    for (uint32 bone = 0; bone < bones; ++bone) {
        CHECK(built.boneMap.TryPushBack(static_cast<uint16>(bone)));
        CHECK(built.requiredBones.TryPushBack(static_cast<uint16>(bone)));
    }
}

// 시험 메시의 머티리얼 슬롯 수다(맵 값 [0, 8)만 굽는다).
constexpr uint32 TestSlots = 8;

// half 반올림 경계 값이다: 1 + 2^-11은 1과 1 + 2^-10의 가운데라 짝수(1.0 = 0x3C00)로, 1 + 3·2^-11은
// 1 + 2^-10과 1 + 2^-9의 가운데라 짝수(0x3C02)로 간다(RNE — 내림이면 0x3C01).
constexpr float HalfTieDown = 1.00048828125f;
constexpr float HalfTieUp = 1.00146484375f;
constexpr uint16 HalfOne = 0x3C00;
constexpr uint16 HalfOnePlusTwoUlp = 0x3C02;

Result<void> Pack(const SkinnedMeshBuildResult& built, Array<uint8>& bytes, SkinnedRenderLod& lod,
                  Span<const int32> map = {}, Span<const Name> profiles = {},
                  Span<const Name> morphs = {}) noexcept {
    return PackSkinnedMeshLod(built, MeshBuildSettings{}, Span<const MeshSectionInfo>(), map,
                              TestSlots, MeshColorFormat::Bgra8Unorm, profiles, morphs, bytes, lod);
}

// 패킹 결과가 로드 검증(모양·bulk 값)을 통과하는지다. boneCount는 메시 본 수다.
bool Validates(Allocator& allocator, const SkinnedRenderLod& lod, Span<const uint8> bytes,
               usize boneCount) noexcept {
    SkeletalMeshRenderData data(allocator);
    SkinnedRenderLod copy(allocator);
    copy.vertexCount = lod.vertexCount;
    copy.data.bulkId = 1;
    copy.data.rawBytes = bytes.Size();
    copy.positions = lod.positions;
    copy.tangentBasis = lod.tangentBasis;
    copy.colors = lod.colors;
    copy.influencesPerVertex = lod.influencesPerVertex;
    copy.boneIndices = lod.boneIndices;
    copy.boneWeights = lod.boneWeights;
    copy.indexFormat = lod.indexFormat;
    copy.indices = lod.indices;
    if (!copy.texCoords.TryAppend(lod.texCoords.View()) ||
        !copy.sections.TryAppend(lod.sections.View()) ||
        !copy.boneMap.TryAppend(lod.boneMap.View()) ||
        !copy.requiredBones.TryAppend(lod.requiredBones.View()) ||
        !copy.morphTargets.TryAppend(lod.morphTargets.View()) ||
        !copy.skinWeightProfiles.TryAppend(lod.skinWeightProfiles.View())) {
        return false;
    }

    if (!data.lods.TryPushBack(Move(copy))) {
        return false;
    }

    return ValidateSkeletalMeshRenderData(data, boneCount).IsOk() &&
           ValidateSkinnedRenderLodBulk(data.lods[0], 0, bytes).IsOk();
}

} // namespace

// 기본: 영향 4칸 → UByte4 번호·UByte4Unorm 가중치(원소 하나), UV 다음(색 없음)에 번호 → 가중치 →
// index, 각 16 byte 정렬, bone map·필요 본 복사, 검증 통과.
TEST(SkinnedMeshPacking_DefaultLayout) {
    SystemAllocator allocator;
    SkinnedMeshBuildResult built(allocator);
    Fill(built, 3, MeshCookedInfluencesLow, 2, false);
    Array<uint8> bytes(allocator);
    SkinnedRenderLod lod(allocator);
    REQUIRE(Pack(built, bytes, lod));
    CHECK(lod.vertexCount == 3 && lod.influencesPerVertex == MeshCookedInfluencesLow);
    REQUIRE(lod.texCoords.Size() == 1);
    CHECK(lod.boneIndices.vertexFormat == Number(VertexFormat::UByte4) &&
          lod.boneIndices.byteSize == 12 && Aligned(lod.boneIndices) &&
          lod.boneIndices.offset >= lod.texCoords[0].offset + lod.texCoords[0].byteSize);
    CHECK(lod.boneWeights.vertexFormat == Number(VertexFormat::UByte4Unorm) &&
          lod.boneWeights.byteSize == 12 && Aligned(lod.boneWeights) &&
          lod.boneWeights.offset >= lod.boneIndices.offset + lod.boneIndices.byteSize);
    CHECK(Aligned(lod.indices) &&
          lod.indices.offset >= lod.boneWeights.offset + lod.boneWeights.byteSize &&
          bytes.Size() == lod.indices.offset + lod.indices.byteSize);
    // 정점 1: 번호 (1, 0, 0, 0), 가중치 (255, 0, 0, 0).
    const uint8* indices = bytes.Data() + lod.boneIndices.offset;
    const uint8* weights = bytes.Data() + lod.boneWeights.offset;
    CHECK(indices[4] == 1 && indices[5] == 0 && weights[4] == 255 && weights[5] == 0);
    CHECK(lod.boneMap.Size() == 2 && lod.boneMap[1] == 1 && lod.requiredBones.Size() == 2);
    CHECK(lod.morphTargets.IsEmpty() && lod.skinWeightProfiles.IsEmpty());
    CHECK(lod.sections.Size() == 1 && lod.sections[0].materialIndex == 0);
    CHECK(Validates(allocator, lod, bytes.View(), 2));
}

// 넓은 형식: 영향 8칸이면 원소 둘, 16비트 가중치는 UShort4Unorm, bone map 257개면 UShort4 번호이고
// 256개면 UByte4다.
TEST(SkinnedMeshPacking_WideFormats) {
    SystemAllocator allocator;
    SkinnedMeshBuildResult wide(allocator);
    Fill(wide, 3, MeshCookedInfluencesHigh, MeshBoneIndex8Limit + 1, true);
    // 정점 2의 첫 칸을 본 256으로(16비트 번호에서만 표현된다).
    wide.influences[2 * MeshCookedInfluencesHigh].boneIndex = MeshBoneIndex8Limit;
    Array<uint8> bytes(allocator);
    SkinnedRenderLod lod(allocator);
    REQUIRE(Pack(wide, bytes, lod));
    CHECK(lod.influencesPerVertex == MeshCookedInfluencesHigh);
    CHECK(lod.boneIndices.vertexFormat == Number(VertexFormat::UShort4) &&
          lod.boneIndices.byteSize == 3 * 2 * 8);
    CHECK(lod.boneWeights.vertexFormat == Number(VertexFormat::UShort4Unorm) &&
          lod.boneWeights.byteSize == 3 * 2 * 8);
    const uint8* indices = bytes.Data() + lod.boneIndices.offset;
    const uint8* weights = bytes.Data() + lod.boneWeights.offset;
    CHECK(LoadLittleEndian<uint16>(indices + 2 * 16) == MeshBoneIndex8Limit);
    CHECK(LoadLittleEndian<uint16>(weights + 16) == BoneWeightTotal &&
          LoadLittleEndian<uint16>(weights + 18) == 0);
    CHECK(Validates(allocator, lod, bytes.View(), MeshBoneIndex8Limit + 1));

    // 256개면 UByte4이고 최댓값 255가 byte 0xFF로 들어간다.
    SkinnedMeshBuildResult edge(allocator);
    Fill(edge, 3, MeshCookedInfluencesLow, MeshBoneIndex8Limit, false);
    edge.influences[MeshCookedInfluencesLow].boneIndex = MeshBoneIndex8Limit - 1;
    REQUIRE(Pack(edge, bytes, lod));
    CHECK(lod.boneIndices.vertexFormat == Number(VertexFormat::UByte4) &&
          lod.boneWeights.vertexFormat == Number(VertexFormat::UByte4Unorm));
    CHECK(bytes[lod.boneIndices.offset + MeshCookedInfluencesLow] == 0xFF);
    CHECK(Validates(allocator, lod, bytes.View(), MeshBoneIndex8Limit));

    // 조합: 16비트 번호 + 8비트 가중치(8칸), 8비트 번호 + 16비트 가중치(4칸).
    SkinnedMeshBuildResult wideIndex(allocator);
    Fill(wideIndex, 3, MeshCookedInfluencesHigh, MeshBoneIndex8Limit + 1, false);
    REQUIRE(Pack(wideIndex, bytes, lod));
    CHECK(lod.boneIndices.vertexFormat == Number(VertexFormat::UShort4) &&
          lod.boneWeights.vertexFormat == Number(VertexFormat::UByte4Unorm) &&
          lod.boneWeights.byteSize == 3 * 2 * 4);
    CHECK(Validates(allocator, lod, bytes.View(), MeshBoneIndex8Limit + 1));
    SkinnedMeshBuildResult wideWeight(allocator);
    Fill(wideWeight, 3, MeshCookedInfluencesLow, 2, true);
    REQUIRE(Pack(wideWeight, bytes, lod));
    CHECK(lod.boneIndices.vertexFormat == Number(VertexFormat::UByte4) &&
          lod.boneIndices.byteSize == 3 * 4 &&
          lod.boneWeights.vertexFormat == Number(VertexFormat::UShort4Unorm) &&
          lod.boneWeights.byteSize == 3 * 8);
    CHECK(Validates(allocator, lod, bytes.View(), 2));
}

// 프로필·morph: 가중치 다음에 프로필(번호·가중치, 빌드 순서), 그다음 morph(빌드 순서), 마지막
// index. 이름은 인자 그대로, morph 항목은 (정점 uint32, Δp half4, Δn half4 — w = 0) 20 byte다.
TEST(SkinnedMeshPacking_ProfilesAndMorphs) {
    SystemAllocator allocator;
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    SkinnedMeshBuildResult built(allocator);
    Fill(built, 3, MeshCookedInfluencesLow, 2, false);
    SkinWeightProfileBuild profile(allocator);
    for (uint32 vertex = 0; vertex < 3; ++vertex) {
        for (uint32 slot = 0; slot < MeshCookedInfluencesLow; ++slot) {
            REQUIRE(profile.influences.TryPushBack(slot == 0 ? BoneInfluence{1, SkinWeightTotal8}
                                                             : BoneInfluence{0, 0}));
        }
    }

    REQUIRE(built.profiles.TryPushBack(Move(profile)));
    for (uint32 target = 0; target < 2; ++target) {
        MorphTargetBuild morph(allocator);
        REQUIRE(morph.deltas.TryPushBack(
            MorphTargetDelta{0, Vec3(1.0f, 0.5f, -2.0f), Vec3(HalfTieDown, HalfTieUp, 0.0f)}));
        if (target == 1) {
            REQUIRE(morph.deltas.TryPushBack(
                MorphTargetDelta{2, Vec3(0.0f, 0.0f, 0.25f), Vec3(0.0f, 1.0f, 0.0f)}));
        }

        REQUIRE(built.morphTargets.TryPushBack(Move(morph)));
    }

    const Name profileNames[1] = {names.TryIntern("Cloth").Value()};
    const Name morphNames[2] = {names.TryIntern("Smile").Value(), names.TryIntern("Blink").Value()};
    Array<uint8> bytes(allocator);
    SkinnedRenderLod lod(allocator);
    REQUIRE(
        Pack(built, bytes, lod, {}, Span<const Name>(profileNames), Span<const Name>(morphNames)));
    REQUIRE(lod.skinWeightProfiles.Size() == 1 && lod.morphTargets.Size() == 2);
    const SkinWeightProfileRenderLod& cloth = lod.skinWeightProfiles[0];
    CHECK(cloth.name == profileNames[0] &&
          cloth.boneIndices.vertexFormat == lod.boneIndices.vertexFormat);
    CHECK(cloth.boneIndices.byteSize == lod.boneIndices.byteSize &&
          cloth.boneWeights.byteSize == lod.boneWeights.byteSize);
    CHECK(Aligned(cloth.boneIndices) &&
          cloth.boneIndices.offset >= lod.boneWeights.offset + lod.boneWeights.byteSize &&
          cloth.boneWeights.offset >= cloth.boneIndices.offset + cloth.boneIndices.byteSize);
    CHECK(bytes[cloth.boneIndices.offset] == 1);
    const SkinnedMorphTarget& smile = lod.morphTargets[0];
    const SkinnedMorphTarget& blink = lod.morphTargets[1];
    CHECK(smile.name == morphNames[0] && smile.deltaCount == 1 &&
          smile.deltas.byteSize == SkinnedMorphDeltaBytes &&
          smile.deltas.vertexFormat == MeshRenderStreamNone);
    CHECK(blink.name == morphNames[1] && blink.deltaCount == 2 &&
          blink.deltas.byteSize == 2 * SkinnedMorphDeltaBytes);
    CHECK(Aligned(smile.deltas) &&
          smile.deltas.offset >= cloth.boneWeights.offset + cloth.boneWeights.byteSize &&
          blink.deltas.offset >= smile.deltas.offset + smile.deltas.byteSize &&
          lod.indices.offset >= blink.deltas.offset + blink.deltas.byteSize);
    // blink 항목 1: 정점 2, Δp (0, 0, 0.25, 0), Δn (0, 1, 0, 0).
    const uint8* entry = bytes.Data() + blink.deltas.offset + SkinnedMorphDeltaBytes;
    CHECK(LoadLittleEndian<uint32>(entry) == 2);
    CHECK(LoadLittleEndian<uint16>(entry + 4 + 4) == Float16::FromFloat(0.25f).bits &&
          LoadLittleEndian<uint16>(entry + 4 + 6) == Float16::FromFloat(0.0f).bits);
    CHECK(LoadLittleEndian<uint16>(entry + 12 + 2) == Float16::FromFloat(1.0f).bits &&
          LoadLittleEndian<uint16>(entry + 12 + 6) == 0);
    // smile 항목 0의 Δp x·z.
    const uint8* first = bytes.Data() + smile.deltas.offset;
    CHECK(LoadLittleEndian<uint16>(first + 4) == Float16::FromFloat(1.0f).bits &&
          LoadLittleEndian<uint16>(first + 8) == Float16::FromFloat(-2.0f).bits);
    // Δn의 반올림 경계(RNE): x → 0x3C00, y → 0x3C02, w = 0.
    CHECK(LoadLittleEndian<uint16>(first + 12) == HalfOne &&
          LoadLittleEndian<uint16>(first + 14) == HalfOnePlusTwoUlp &&
          LoadLittleEndian<uint16>(first + 18) == 0);
    CHECK(Validates(allocator, lod, bytes.View(), 2));
}

// LOD 머티리얼 맵: 슬롯 범위 [0, 8) 값만 섹션 materialIndex로 굽고 −1·범위 밖·음수는 그대로다(UE
// IsValidMaterialIndex — 섹션 번호 기준, 맵이 섹션보다 길어도 된다). 인자 오류: 이름 수 불일치,
// 영향 칸 수 4·8 아님, 가중치 합 255·65535 아님, 영향 배열(기본·프로필) 크기 불일치, 번호 ≥ bone
// map, 가중치 > 합.
TEST(SkinnedMeshPacking_MaterialMapAndErrors) {
    SystemAllocator allocator;
    SkinnedMeshBuildResult built(allocator);
    Fill(built, 3, MeshCookedInfluencesLow, 2, false);
    built.mesh.sections[0].materialIndex = 1;
    Array<uint8> bytes(allocator);
    SkinnedRenderLod lod(allocator);
    const int32 keep[2] = {SkeletalMeshNoMaterialRemap, 7};
    REQUIRE(Pack(built, bytes, lod, Span<const int32>(keep)));
    CHECK(lod.sections[0].materialIndex == 1);
    const int32 remap[1] = {3};
    REQUIRE(Pack(built, bytes, lod, Span<const int32>(remap)));
    CHECK(lod.sections[0].materialIndex == 3);

    const int32 outside[1] = {static_cast<int32>(TestSlots)};
    REQUIRE(Pack(built, bytes, lod, Span<const int32>(outside)));
    CHECK(lod.sections[0].materialIndex == 1);
    const int32 negative[1] = {-2};
    REQUIRE(Pack(built, bytes, lod, Span<const int32>(negative)));
    CHECK(lod.sections[0].materialIndex == 1);
    NamePool names(allocator);
    REQUIRE(names.Initialize());
    const Name extra[1] = {names.TryIntern("Extra").Value()};
    CHECK(Pack(built, bytes, lod, {}, Span<const Name>(extra)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Pack(built, bytes, lod, {}, {}, Span<const Name>(extra)).Error() ==
          ErrorCode::InvalidArgument);
    built.influencesPerVertex = 5;
    CHECK(Pack(built, bytes, lod).Error() == ErrorCode::InvalidArgument);
    built.influencesPerVertex = MeshCookedInfluencesLow;
    built.weightTotal = 100;
    CHECK(Pack(built, bytes, lod).Error() == ErrorCode::InvalidArgument);
    built.weightTotal = SkinWeightTotal8;
    built.influences[0].boneIndex = 2;
    CHECK(Pack(built, bytes, lod).Error() == ErrorCode::InvalidArgument);
    built.influences[0] = BoneInfluence{0, SkinWeightTotal8 + 1};
    CHECK(Pack(built, bytes, lod).Error() == ErrorCode::InvalidArgument);
    built.influences[0] = BoneInfluence{0, SkinWeightTotal8};
    REQUIRE(built.profiles.TryPushBack(SkinWeightProfileBuild(allocator)));
    const Name profileName[1] = {extra[0]};
    CHECK(Pack(built, bytes, lod, {}, Span<const Name>(profileName)).Error() ==
          ErrorCode::InvalidArgument);
    built.profiles.Clear();
    (void)built.influences.TryPopBack();
    CHECK(Pack(built, bytes, lod).Error() == ErrorCode::InvalidArgument);
}

// 같은 입력이면 같은 byte다(다시 패킹하면 앞 결과를 지우고 같은 값을 쓴다).
TEST(SkinnedMeshPacking_Deterministic) {
    SystemAllocator allocator;
    SkinnedMeshBuildResult built(allocator);
    Fill(built, 6, MeshCookedInfluencesHigh, 3, true);
    Array<uint8> first(allocator);
    Array<uint8> second(allocator);
    SkinnedRenderLod lod(allocator);
    REQUIRE(Pack(built, first, lod));
    REQUIRE(Pack(built, second, lod));
    CHECK(lod.boneMap.Size() == 3 && lod.sections.Size() == 1);
    CHECK(first.Size() == second.Size() &&
          platform::MemoryEquals(first.Data(), second.Data(), first.Size()));
    CHECK(Validates(allocator, lod, second.View(), 3));
}

} // namespace skinned_mesh_packing_tests_file
