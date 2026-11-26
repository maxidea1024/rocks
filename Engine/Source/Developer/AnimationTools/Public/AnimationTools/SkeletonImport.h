#pragma once

#include <Animation/Skeleton.h>
#include <AssetDatabase/AssetImporter.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/StringView.h>

// 스켈레톤 임포트 서비스다(Docs/Design/MeshAnimationAssets 6.6절·10절 — MA4b). 파일 형식을 읽는
// 임포터(교환 형식 UX, 뒤의 glTF·FBX)가 AssetImporter::Import 안에서 SkeletonImportSpec을 만들어
// 부르면, 서비스가 Output()으로 받은 Skeleton을 채운다. 정적 메시 ImportStaticMesh와 같은 역할이다.
namespace rocks {

// 본 하나다(본 번호 순 — parentIndex는 앞 본 번호, 루트는 NoParentBone).
struct SkeletonImportBone {
    StringView name{};
    int32 parentIndex = NoParentBone;
    Transform referencePose = Transform::Identity();
};

// 가상 본 하나다(이름은 "VB " 접두사로 시작한다 — 6.2절).
struct SkeletonImportVirtualBone {
    StringView sourceBone{};
    StringView targetBone{};
    StringView name{};
};

// 소켓 하나다(BoneSocket 필드 — 이름은 문자열).
struct SkeletonImportSocket {
    StringView name{};
    StringView boneName{};
    Vec3d relativeLocation{};
    Rotator<double> relativeRotation{};
    Vec3d relativeScale = Vec3d(1.0, 1.0, 1.0);
    bool forceAlwaysAnimated = true;
};

// 커브 정보 하나다(SkeletonCurveInfo 필드 — 연결 본은 문자열 목록, 호출 동안 산다).
struct SkeletonImportCurve {
    StringView name{};
    bool isMaterial = false;
    bool isMorphTarget = false;
    Span<const StringView> linkedBones{};
    uint8 maxLod = NumericLimits<uint8>::Max();
};

// 스켈레톤 임포트 명세다(6.6절 2). span은 ImportSkeleton이 끝날 때까지 산다.
//
// 사용 예(임포터 Import 안):
//   const SkeletonImportBone bones[] = {{"root", NoParentBone}, {"spine", 0}};
//   SkeletonImportSpec spec;
//   spec.bones = Span<const SkeletonImportBone>(bones);
//   auto output = context.Output({}, TypeIdOf<Skeleton>());
//   if (!output) { return Result<void>::Failure(output.Error()); }
//   return ImportSkeleton(context, output.Value(), spec);
struct SkeletonImportSpec {
    Span<const SkeletonImportBone> bones{};
    // 비면 모두 Animation(UE 새 본 기본), 아니면 bones와 같은 길이다.
    Span<const BoneTranslationRetarget> retargetModes{};
    Span<const SkeletonImportVirtualBone> virtualBones{};
    Span<const SkeletonImportSocket> sockets{};
    Span<const SkeletonImportCurve> curves{};
    // 재임포트에서 소켓·가상 본·커브와 명세에 있는 본의 리타깃 방식을 명세 값으로 바꾼다(교환
    // 경로처럼 원본 쪽이 정본일 때 — retargetModes가 비면 명세 본은 Animation). 거짓이면 사용자
    // 편집을 보존하고 명세의 새 항목만 더한다. 본은 어느 쪽이든 지우지 않는다(6.6절 4).
    bool replaceOnReimport = false;
};

// spec으로 output(context.Output이 준 Skeleton)을 채운다(6.6절 3·4). 새 임포트는 명세 값으로 쓰고
// 보존 필드(슬롯 묶음·리타깃 원본·호환 스켈레톤)를 비운다. 재임포트(context.IsReimport())는 명세
// 본을 기존 계층에 병합하고(MergeBonesIntoSkeleton — 기존 본의 번호·pose 유지, 본을 지우지 않음),
// 기존 항목을 두고 명세의 새 항목만 더한다: 소켓은 이름, 가상 본은 이름이나 (원본, 대상) 쌍(UE
// AddNewVirtualBone), 커브는 이름이 같으면 머티리얼·morph 표시만 OR로 합친다(UE
// AccumulateCurveMetaData). replaceOnReimport면 명세 값이다. 기준 pose는 저장 전에
// 정규화하고(NormalizeBoneReferencePose) 마지막에 ValidateSkeleton으로 본다.
//
// 오류(진단은 context.Fail, 이 순서): output이 Skeleton이 아님 TypeMismatch, 본 0개
// SkeletonMismatch (EmptyMeshHierarchy — UE도 본 없는 스켈레톤을 만들지 않는다), retargetModes 길이
// InvalidSetting (detail = 길이), 이름 intern 실패 InvalidSetting(detail = 그 목록 안 위치 —
// 본·가상 본·소켓·커브 순, 커브 연결 본은 커브 번호, intern의 원래 오류), 병합의
// 진단(SkeletonMismatch·InvalidSkeletonData), 검증 위반 InvalidSkeletonData(detail = 진단 index,
// 없으면 0 — failure가 null이 아니면 세부도 채운다), 할당 실패(진단 코드 없이 원래 오류). 실패하면
// output은 부분적으로 바뀔 수 있다(서비스가 출력을 버린다 — AssetImporter 계약).
ROCKS_ANIMATIONTOOLS_API Result<void>
ImportSkeleton(AssetImportContext& context, ObjectHandle output, const SkeletonImportSpec& spec,
               SkeletonDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
