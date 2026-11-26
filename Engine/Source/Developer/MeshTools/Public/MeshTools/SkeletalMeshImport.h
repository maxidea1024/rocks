#pragma once

#include <Animation/AnimationDiagnostic.h>
#include <AssetDatabase/AssetImporter.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.h>
#include <MeshSource/MeshSourceTypes.h>
#include <MeshTools/SkeletalMeshImportSpec.h>

// 스킨 메시 임포트 서비스다(Docs/Design/MeshAnimationAssets 6.7.3절 — MA5b3). 파일 형식을 읽는
// 임포터(교환 형식 UX4b, 뒤의 glTF·FBX)가 AssetImporter::Import 안에서 메모리 MeshSource와
// SkeletalMeshImportSpec을 만들어 부르면, 서비스가 Output()으로 받은 SkeletalMesh 객체를 채우고
// 스켈레톤을 만들거나(보조 출력) 기존 스켈레톤에 본을 맞춘다. 정적 메시 ImportStaticMesh와 같은
// 역할이다.
namespace rocks {

// spec으로 output(context.Output이 준 SkeletalMesh)을 채운다(6.7.3절). 먼저 모두 검증하고(LOD 수
// 1–SkeletalMeshMaxLods → LOD마다 원본이나 baked → 메시 본 → 원본마다 ValidateMeshSource(본 수) →
// 섹션 정보 → 슬롯 수 → LOD 머티리얼 맵 → 이름(슬롯 → 소켓(이름 겹침·본 존재를 소켓마다) → LOD 본
// 목록·화면 크기 키 → 프로필(이름 겹침·키를 프로필마다) → minLod 키) → 머티리얼 참조) 스켈레톤을
// 정한 뒤(6.7.3절 2·3 — 보조 출력이거나 판정만) 원본 bulk를 context.AddBulk로 넘기고 필드를 쓰고,
// 마지막에 기존 스켈레톤에 모자란 본을 더한다(EditAsset → MergeBonesIntoSkeleton →
// ValidateSkeleton). 재임포트(context.IsReimport())는 사용자 편집을 보존한다(6.7.3절 5 —
// replaceSettingsOnReimport면 설정은 명세 값). renderData는 비운다. 값 정리는 하지 않는다(빌드 몫).
//
// 오류(진단은 context.Fail): output이 SkeletalMesh가 아님 TypeMismatch, LOD 수·원본 없음·섹션
// 정보·슬롯 수·머티리얼 맵 값(−1 미만)·이름(intern의 원래 오류)·프로파일 키·소켓(이름 겹침, 본이
// 메시 본에 없음)·프로필 이름 겹침 InvalidSetting(detail = 위치), 메시 본 0개 SkeletonMismatch
// (EmptyMeshHierarchy), 메시 본 계층 위반 InvalidSkeletonData(detail = 본), 원본 검증 실패는
// ValidateMeshSource의 코드(detail = LOD — meshFailure가 null이 아니면 세부도 채운다), 머티리얼
// 참조 TypeMismatch(detail = 슬롯 i의 material 2i·overlay 2i+1)·스켈레톤 참조 TypeMismatch(detail =
// 슬롯 수 × 2), 참조 열기·EditAsset 실패는 문맥의 오류, 스켈레톤 병합 실패 SkeletonMismatch·
// InvalidSkeletonData(detail = 메시 본, 기존 스켈레톤 계층 자체의 위반이면 role Skeleton·스켈레톤
// 본), 병합 뒤 ValidateSkeleton 위반은 그 진단(skeletonFailure가 null이 아니면 세부도 채운다), 묶음
// 구성원 스켈레톤에 본이 모자람 SkeletonMismatch(MissingSkeletonBones, detail = 첫 모자란 메시 본),
// 보조 스켈레톤 출력은 ImportSkeleton의 오류, 할당 실패. 실패하면 output은 부분적으로 바뀔 수
// 있다(서비스가 출력을 버린다 — AssetImporter 계약).
ROCKS_MESHTOOLS_API Result<void>
ImportSkeletalMesh(AssetImportContext& context, ObjectHandle output,
                   const SkeletalMeshImportSpec& spec, MeshDiagnostic* meshFailure = nullptr,
                   SkeletonDiagnostic* skeletonFailure = nullptr) noexcept;

} // namespace rocks
