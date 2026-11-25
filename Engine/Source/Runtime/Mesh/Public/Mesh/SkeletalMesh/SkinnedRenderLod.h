#pragma once

#include <AssetRuntime/BulkData.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Mesh/Common/MeshRenderSection.h>
#include <Mesh/Common/MeshRenderStream.h>
#include <Mesh/SkeletalMesh/SkinWeightProfileRenderLod.h>
#include <Mesh/SkeletalMesh/SkinnedMorphTarget.h>
#include <MeshSource/MeshSourceTypes.h>

// 스킨 메시 쿠킹 LOD 하나다(Docs/Design/MeshAnimationAssets 6.3절·6.7.1절 6, UE
// FSkeletalMeshLODRenderData). 정적 메시 LOD(StaticMeshRenderLod)의 필드에 영향·bone map·필요 본·
// morph target·가중치 프로필을 더한다. stream은 LOD 덩어리(data) 안 구간이다(5.3절과 같다).
namespace rocks {

// 영향 번호 형식을 UByte4로 둘 수 있는 bone map 크기 상한이다(그보다 크면 UShort4 — 6.3절,
// RenderBase 10절 skinned 행).
inline constexpr uint32 MeshBoneIndex8Limit = 256;

// 쿠킹 정점당 영향 칸 수다(영향 4개 = 원소 하나, 8개 = 둘 — 6.3절, UE MAX_INFLUENCES_PER_STREAM·
// EXTRA_BONE_INFLUENCES).
inline constexpr uint32 MeshCookedInfluencesLow = 4;
inline constexpr uint32 MeshCookedInfluencesHigh = 8;

// 8비트 쿠킹 가중치의 합이다(UByte4Unorm — 16비트는 MeshSource BoneWeightTotal 65535).
inline constexpr uint16 SkinWeightTotal8 = 255;

// 정점마다 influencesPerVertex / 4개 원소(영향 4개 = 원소 하나)를 잇는다. 영향 번호는 boneMap 안
// 위치, 가중치는 내림차순이고 빈 칸은 (0, 0)이다. 섹션 materialIndex는 원본 LOD의 lodMaterialMap을
// 적용한 값이다.
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct SkinnedRenderLod {
    ROCKS_GENERATED_BODY()

    explicit SkinnedRenderLod(Allocator& allocator) noexcept
        : texCoords(allocator), sections(allocator), boneMap(allocator), requiredBones(allocator),
          morphTargets(allocator), skinWeightProfiles(allocator) {
    }

    // 쿠킹 프로파일의 화면 크기와 LOD 전환 여유다(UE ScreenSize·LODHysteresis).
    ROCKS_PROPERTY() float screenSize = 0.0f;
    ROCKS_PROPERTY() float lodHysteresis = 0.0f;
    ROCKS_PROPERTY() uint32 vertexCount = 0;
    // 이 LOD의 덩어리다(쿠킹 패키지 bulk — 4 KiB 정렬).
    ROCKS_PROPERTY() BulkDataRef data;
    ROCKS_PROPERTY() MeshRenderStream positions;
    ROCKS_PROPERTY() MeshRenderStream tangentBasis;
    ROCKS_PROPERTY() Array<MeshRenderStream> texCoords;
    ROCKS_PROPERTY() MeshRenderStream colors;
    // 정점당 영향 칸 수(MeshCookedInfluencesLow 또는 MeshCookedInfluencesHigh)다.
    ROCKS_PROPERTY() uint32 influencesPerVertex = 0;
    // 영향 번호(UByte4 — bone map MeshBoneIndex8Limit개 이하, 넘으면 UShort4)와 가중치(UByte4Unorm
    // 합 255, UShort4Unorm 합 65535)다.
    ROCKS_PROPERTY() MeshRenderStream boneIndices;
    ROCKS_PROPERTY() MeshRenderStream boneWeights;
    ROCKS_PROPERTY() uint32 indexFormat = 0;
    ROCKS_PROPERTY() MeshRenderStream indices;
    ROCKS_PROPERTY() Array<MeshRenderSection> sections;
    // LOD 통합 bone map(메시 본 번호 오름차순 — RenderBase 결정 R18)과 필요 본 목록(오름차순)이다.
    ROCKS_PROPERTY() Array<uint16> boneMap;
    ROCKS_PROPERTY() Array<uint16> requiredBones;
    ROCKS_PROPERTY() Array<SkinnedMorphTarget> morphTargets;
    ROCKS_PROPERTY() Array<SkinWeightProfileRenderLod> skinWeightProfiles;
};

} // namespace rocks
