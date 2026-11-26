#pragma once

#include <AssetCooker/CookProcessor.h>
#include <Core/Containers/Span.h>
#include <Mesh/StaticMesh/StaticMesh.h>
#include <MeshTools/StaticMeshNuggetHeader.h>
#include <MeshTools/StaticMeshPacking.h>

// 정적 메시 쿠킹 처리기다(Docs/Design/MeshAnimationAssets 5.5.2절 — MA2b2). StaticMesh의 쿠킹
// 패키지를 만든다: LOD 입력 고르기(생성 LOD·baked 원본) → 너겟 조회(bucket mesh, SCC Record) →
// 미적중이면 원본 로드·빌드(BuildStaticMeshLod)·패킹(PackStaticMeshLod)·Put → renderData와 대상
// 프로파일 값 채우기 → 기본 처리기처럼 쿠킹 패키지 쓰기(bulk 정렬 4 KiB 하한) → 에디터 객체
// 되돌리기(성공·실패 모두).
//
// 사용 예(호스트 — 명령 동안 처리기를 쥔다):
//   StaticMeshCookProcessor meshCook;
//   context.cookProcessors = {&RegisterStaticMeshCookProcessors, &meshCook};
namespace rocks {

// 패킹·LOD 입력 규칙의 버전이다(텍스처 TextureBuildVersion과 같은 역할 — 바뀌면 올린다. 너겟 키에
// 들어간다).
inline constexpr uint32 StaticMeshBuildVersion = 1;

// 쿠킹 패키지 bulk 정렬의 하한이다(LOD 덩어리 시작 4 KiB — 텍스처 스트리밍 덩어리와 같은 렌더 요구,
// TextureStreaming 4.9절). 프로파일 정렬이 더 크면 그것을 쓴다.
inline constexpr uint32 StaticMeshStreamingBulkAlignment = 4096;

// 자동 화면 크기의 LOD 0 값과 거듭제곱 밑이다(UE UE::StaticMesh::Private::Constants
// LOD0ScreenSize·AutoComputeLODPowerBase — StaticMesh.cpp).
inline constexpr float StaticMeshLod0ScreenSize = 2.0f;
inline constexpr float StaticMeshAutoLodPowerBase = 0.75f;

// 처리기 정체성이다(출력 배치 — 머리·renderData 채우기 — 가 바뀌면 버전 Guid를 바꾼다).
inline constexpr NuggetProcessorId StaticMeshCookProcessorId{
    Guid{0x524f434b4d455343ull, 0x0000000000000001ull},
    Guid{0x524f434b4d455356ull, 0x0000000000000001ull}};

// 너겟 키 머리의 정체성이다(너겟 값 — 머리·LOD byte — 의 뜻이 바뀌면 버전을 올린다).
inline constexpr NuggetProcessorId StaticMeshNuggetProcessorId{
    Guid{0x524f434b4d455343ull, 0x0000000000000001ull},
    Guid{0x524f434b4d45534eull, 0x0000000000000001ull}};

// 대상 프로파일의 쿠킹 정점 색 형식이다(graphicsBackends에 OpenGL이 있으면 UByte4Unorm — 5.5.2절
// 2).
//
// 사용 예:
//   const MeshColorFormat color = StaticMeshColorFormat(*context.profile);
ROCKS_MESHTOOLS_API MeshColorFormat StaticMeshColorFormat(const TargetProfile& profile) noexcept;

// 대상 프로파일이 장치를 갖는지다(graphicsBackends가 비면 전용 서버 — renderData의 stream을 비운다,
// 5.5.2절 3).
//
// 사용 예:
//   if (!StaticMeshProfileHasDevice(profile)) { /* 서버 쿠킹 */ }
ROCKS_MESHTOOLS_API bool StaticMeshProfileHasDevice(const TargetProfile& profile) noexcept;

// 자동 LOD 화면 크기를 out에 채운다(UE ResolveSectionInfo — 축소기가 없어 MaxDeviation 0: LOD 0은
// StaticMeshLod0ScreenSize, i는 0.75^i, 앞 LOD보다 크면 앞 값의 절반).
//
// 사용 예:
//   float sizes[StaticMeshMaxLods] = {};
//   StaticMeshAutoScreenSizes(Span<float>(sizes, lodCount));
ROCKS_MESHTOOLS_API void StaticMeshAutoScreenSizes(Span<float> out) noexcept;

// 정적 메시 쿠킹 처리기다(TypeIdOf<StaticMesh>()에 등록). 한 스레드에서 쓴다(쿠킹 실행기와 같다).
// Cook 오류: StaticMesh가 아님 TypeMismatch, 원본 LOD 수가 StaticMeshMaxLods를 넘음 InvalidSetting
// (InvalidArgument), 쓸 LOD가 없음 InvalidSource(InvalidArgument), 원본 로드 실패 InvalidSource(그
// 오류), 빌드 실패는 그 진단 코드와 오류, verify 불일치 NuggetMismatch(InvalidArgument), required
// 너겟 캐시 실패(진단 None — 원인 오류), Registry Name pool·bulk 원본 없음 InvalidArgument(진단
// None), 속성 이름 pool 오류, 쓰기 오류, 할당 실패 OutOfMemory.
class ROCKS_MESHTOOLS_API StaticMeshCookProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return StaticMeshCookProcessorId;
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

// 쿠킹 처리기 등록이다(AssetCookProcessorSetup — state = StaticMeshCookProcessor*, 명령 동안 산다).
// 오류: null state InvalidArgument, CookProcessorTable::Register의 오류.
ROCKS_MESHTOOLS_API Result<void>
RegisterStaticMeshCookProcessors(void* state, CookProcessorTable& table) noexcept;

} // namespace rocks
