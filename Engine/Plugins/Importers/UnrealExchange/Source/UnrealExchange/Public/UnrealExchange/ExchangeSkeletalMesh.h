#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <MeshTools/SkeletalMeshImportSpec.h>
#include <Object/ObjectRegistry.h>
#include <UnrealExchange/ExchangeMeshSource.h>
#include <UnrealExchange/ExchangeSettings.h>

// 교환 스킨 메시 → 스킨 메시 임포트 명세 변환이다(Docs/Design/UnrealExchange 6.4절 2–6번 — UX7b1).
// A2f 임포터 unreal_skeletal_mesh(UX7b2)가 문서·blob을 검사한 뒤 이 변환으로
// SkeletalMeshImportSpec을 만들어 ImportSkeletalMesh에 넘긴다. 원본 모드는 늘 순수 원본이다(UE 준비
// 원본 없음 — 6.2절).
//
// 사용 예(A2f 임포터 안):
//   ExchangeSkeletalMeshInput input{&asset, blobs.View(), &options->references,
//                                   &context.Registry(), context.Access()};
//   ExchangeSkeletalMesh mesh(allocator);
//   ExchangeMeshFailure failure;
//   if (auto built = mesh.Build(input, failure); !built) { ... }
//   return ImportSkeletalMesh(context, output, mesh.Spec());
namespace rocks {

struct ExchangeSkeletalMeshInput {
    // 스킨 메시 교환 에셋(Ready)과 asset.blobs 순서의 검사한 byte다.
    const ExchangeAsset* asset = nullptr;
    Span<const Span<const uint8>> blobs{};
    // 참조로 걸 대상이다(스켈레톤·슬롯 머티리얼). null이면 참조는 모두 비운다.
    const OrderedMap<String, AssetId>* references = nullptr;
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
};

// 변환 결과다. Spec()은 이 객체와 input.asset이 살아 있는 동안 유효하다. 복사·이동하지 않는다.
class ROCKS_UNREALEXCHANGE_API ExchangeSkeletalMesh {
public:
    explicit ExchangeSkeletalMesh(Allocator& allocator) noexcept;
    ExchangeSkeletalMesh(const ExchangeSkeletalMesh&) = delete;
    ExchangeSkeletalMesh& operator=(const ExchangeSkeletalMesh&) = delete;

    // input으로 명세를 만든다(6.4절 2–6번): 설정 번역(ExchangeSkeletalMeshSettings.h 표)과
    // data.materials 번역 → 본 → LOD마다 원본·생성 LOD 기록 적재와 LOD 설정 → 슬롯·소켓·가중치
    // 프로필 → 에셋 설정. replaceSettingsOnReimport는 참이다. 한 객체에 한 번만 부른다.
    //
    // 경고: 번역의 UnsupportedSetting(detail = 표 행 번호, 모르는 키 ExchangeUnknownSettingDetail,
    // data.materials 슬롯 멤버·data.materials 없음(UX7a2 전 묶음 — 슬롯 없이 들인다)
    // ExchangeSkeletalMeshMaterialsDetail), PerPlatform 프로필 값(detail =
    // SkinWeightProfiles 행), 이름 정규화 UnrealNameNormalized(끝에 하나).
    //
    // 오류(failure.code·detail): 호출 계약(asset·registry 없음, 스킨 메시가 아님, blobs 수, 두 번
    // 부름) InvalidSource / InvalidArgument, 덤프 형식(SourceModels 수와 data.lods 수가 다름 —
    // detail 0, 원본이 있다는 LOD의 기록 없음·LOD 설정 값 종류 — detail = LOD, 본 — detail = 본, 그
    // 밖 — 행 번호) CorruptBundle / CorruptData, 기록 적재 실패는 LoadExchangeMeshSource의
    // 코드(detail = LOD), 번역 값 종류 위반·ImportJson 진단 거부 InvalidSource / CorruptData, 코드
    // 결함·할당 오류는 그 오류(코드 None).
    Result<void> Build(const ExchangeSkeletalMeshInput& input,
                       ExchangeMeshFailure& failure) noexcept;

    const SkeletalMeshImportSpec& Spec() const noexcept {
        return spec_;
    }

    Span<const ExchangeSettingWarning> Warnings() const noexcept {
        return warnings_.View();
    }

    // LOD i의 원본·baked 원본이다(없으면 null — 시험이 본다).
    const MeshSource* LodSource(usize lod) const noexcept;
    const MeshSource* LodBakedSource(usize lod) const noexcept;

private:
    // LOD 하나의 원본 번호와 목록 범위다(LOD 설정 배열 안).
    struct LodParts {
        uint32 source = NumericLimits<uint32>::Max();
        uint32 baked = NumericLimits<uint32>::Max();
        usize materialMapStart = 0;
        usize materialMapCount = 0;
        usize removeStart = 0;
        usize removeCount = 0;
        usize prioritizeStart = 0;
        usize prioritizeCount = 0;
    };

    friend struct ExchangeSkeletalMeshBuilder;

    Allocator* allocator_;
    bool started_ = false;
    JsonDocument translated_;
    JsonDocument translatedData_;
    Array<MeshSource> meshes_;
    Array<LodParts> lodParts_;
    Array<SkeletalMeshImportLod> lods_;
    Array<int32> materialMaps_;
    Array<StringView> boneNames_;
    Array<SkeletonImportBone> bones_;
    Array<StaticMeshImportSlot> slots_;
    Array<SkeletonImportSocket> sockets_;
    Array<SkeletalMeshImportProfile> profiles_;
    Array<ExchangeSettingWarning> warnings_;
    SkeletalMeshImportSpec spec_;
};

} // namespace rocks
