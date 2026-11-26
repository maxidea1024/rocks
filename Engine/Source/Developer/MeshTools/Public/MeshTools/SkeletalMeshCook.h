#pragma once

#include <AssetCooker/CookProcessor.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.h>
#include <MeshTools/SkeletalMeshNuggetHeader.h>
#include <MeshTools/SkinnedMeshPacking.h>

// 스킨 메시 쿠킹 처리기다(Docs/Design/MeshAnimationAssets 6.7.2절 — MA5b2). SkeletalMesh의 쿠킹
// 패키지를 만든다: 본 계층 검사 → LOD 입력 고르기(생성 LOD·baked 원본) → 너겟 조회(bucket mesh, SCC
// Record) → 미적중이면 원본 로드·빌드(BuildSkinnedMeshLod)·패킹(PackSkinnedMeshLod)·Put →
// renderData·inverse-bind와 대상 프로파일 값 채우기 → 쿠킹 패키지 쓰기(bulk 정렬 4 KiB 하한) →
// 에디터 객체 되돌리기(성공·실패 모두). 스켈레톤은 읽지 않는다(Build 의존 없음 — 6.7.2절 3).
//
// 사용 예(호스트 — 명령 동안 처리기를 쥔다):
//   SkeletalMeshCookProcessor skinCook;
//   context.cookProcessors = {&RegisterSkeletalMeshCookProcessors, &skinCook};
namespace rocks {

// 스킨 패킹·LOD 입력 규칙의 버전이다(StaticMeshBuildVersion과 같은 역할 — 바뀌면 올린다. 너겟 키에
// 들어간다).
inline constexpr uint32 SkinnedMeshBuildVersion = 1;

// 쿠킹 패키지 bulk 정렬의 하한이다(LOD 덩어리 시작 4 KiB — 정적 메시와 같다. inverse-bind bulk의
// 16 byte 요구도 이것으로 맞는다). 프로파일 정렬이 더 크면 그것을 쓴다.
inline constexpr uint32 SkeletalMeshStreamingBulkAlignment = 4096;

// 처리기 정체성이다(출력 배치 — 머리·renderData 채우기 — 가 바뀌면 버전 Guid를 바꾼다).
inline constexpr NuggetProcessorId SkeletalMeshCookProcessorId{
    Guid{0x524f434b534b4d43ull, 0x0000000000000001ull},
    Guid{0x524f434b534b4d56ull, 0x0000000000000001ull}};

// 너겟 키 머리의 정체성이다(너겟 값 — 머리·inverse-bind·LOD byte — 의 뜻이 바뀌면 버전을 올린다).
inline constexpr NuggetProcessorId SkeletalMeshNuggetProcessorId{
    Guid{0x524f434b534b4d43ull, 0x0000000000000001ull},
    Guid{0x524f434b534b4d4eull, 0x0000000000000001ull}};

// 스킨 메시 쿠킹 처리기다(TypeIdOf<SkeletalMesh>()에 등록). 한 스레드에서 쓴다(쿠킹 실행기와 같다).
// Cook 오류: SkeletalMesh가 아님 TypeMismatch, 원본 LOD 수가 SkeletalMeshMaxLods를 넘음
// InvalidSetting(InvalidArgument), 본 계층 위반 InvalidSkeletonData(detail = 본 —
// ValidateBoneHierarchy의 오류), 쓸 LOD가 없음 InvalidSource(InvalidArgument), 원본 로드 실패
// InvalidSource(그 오류), 빌드 실패는 그 진단 코드와 오류(detail = 쿠킹 LOD), 패킹 실패(진단 None,
// detail = 쿠킹 LOD — 크기 넘침 CapacityExceeded), 너겟 머리 상한(프로필·morph 65535개, 본 목록
// MeshMaxBones) 초과 InvalidSetting(InvalidArgument), verify 불일치
// NuggetMismatch(InvalidArgument), required 너겟 캐시 실패(진단 None — 원인 오류), Registry Name
// pool·bulk 원본 없음 InvalidArgument(진단 None), 이름 pool 오류, 쓰기 오류, 할당 실패 OutOfMemory.
class ROCKS_MESHTOOLS_API SkeletalMeshCookProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return SkeletalMeshCookProcessorId;
    }

    Result<void> Cook(CookContext& context) const noexcept override;

    // 빌드한 LOD 수다(시험 — 너겟 적중이면 오르지 않는다).
    uint64 Builds() const noexcept {
        return builds_;
    }

    // 마지막 Cook이 만든 SCC key다(시험 — 너겟 캐시가 없었으면 앞 값 그대로).
    const Sha256Digest& LastNuggetKey() const noexcept {
        return lastKey_;
    }

private:
    mutable uint64 builds_ = 0;
    mutable Sha256Digest lastKey_{};
};

// 쿠킹 처리기 등록이다(AssetCookProcessorSetup — state = SkeletalMeshCookProcessor*, 명령 동안
// 산다). 오류: null state InvalidArgument, CookProcessorTable::Register의 오류.
ROCKS_MESHTOOLS_API Result<void>
RegisterSkeletalMeshCookProcessors(void* state, CookProcessorTable& table) noexcept;

} // namespace rocks
