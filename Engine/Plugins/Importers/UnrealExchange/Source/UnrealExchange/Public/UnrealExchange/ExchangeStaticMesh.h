#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <MeshTools/StaticMeshImport.h>
#include <Object/ObjectRegistry.h>
#include <UnrealExchange/ExchangeMeshSource.h>
#include <UnrealExchange/ExchangeSettings.h>
#include <UnrealExchange/UnrealExchangeImportOptions.h>

// 교환 정적 메시 → 정적 메시 임포트 명세 변환이다(Docs/Design/UnrealExchange 5.5절 1–9번 — UX3b1).
// A2f 임포터 unreal_static_mesh(UX3b2)가 문서·blob을 검사한 뒤 이 변환으로 StaticMeshImportSpec을
// 만들어 ImportStaticMesh에 넘긴다. 변환은 출력 객체를 만들지 않는다(명세·경고만).
//
// 사용 예(A2f 임포터 안):
//   ExchangeStaticMeshInput input;
//   input.asset = &asset;
//   input.blobs = blobs.View();
//   input.normals = options->meshNormals;
//   input.references = &options->references;
//   input.registry = &context.Registry();
//   input.access = context.Access();
//   ExchangeStaticMesh mesh(allocator);
//   ExchangeMeshFailure failure;
//   if (auto built = mesh.Build(input, failure); !built) {
//       return context.Fail(failure.code, built.Error(), failure.detail);
//   }
//   for (const ExchangeSettingWarning& warning : mesh.Warnings()) {
//       context.Warn(warning.code, warning.detail);
//   }
//   return ImportStaticMesh(context, output, mesh.Spec());
namespace rocks {

struct ExchangeStaticMeshInput {
    // 정적 메시 교환 에셋(Ready — 문서를 읽은 것)과 asset.blobs 순서의 검사한 byte다.
    const ExchangeAsset* asset = nullptr;
    Span<const Span<const uint8>> blobs{};
    // LOD 원본 선택이다(5.5절 2번).
    ExchangeMeshNormals normals = ExchangeMeshNormals::Unreal;
    // 참조로 걸 대상이다(objectPath → AssetId — 슬롯 머티리얼·충돌 메시·거리장 대체 메시). null이면
    // 참조는 모두 비운다.
    const OrderedMap<String, AssetId>* references = nullptr;
    // 값 struct ImportJson의 문맥(타입 표·Name pool — 출력이 쓸 Registry)이다.
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
};

// 변환 결과다. Spec()은 이 객체와 input.asset(문서 문자열을 가리킨다)이 살아 있는 동안 유효하다.
// 복사·이동하지 않는다(명세가 안의 배열을 가리킨다).
class ROCKS_UNREALEXCHANGE_API ExchangeStaticMesh {
public:
    explicit ExchangeStaticMesh(Allocator& allocator) noexcept;
    ExchangeStaticMesh(const ExchangeStaticMesh&) = delete;
    ExchangeStaticMesh& operator=(const ExchangeStaticMesh&) = delete;

    // input으로 명세를 만든다(5.5절): 설정 번역(ExchangeStaticMeshSettings.h 표) → LOD마다 모드에
    // 맞는 기록 적재(LoadExchangeMeshSource)와 빌드·축소·화면 크기 → 슬롯·섹션 정보·소켓 → 충돌 →
    // 에셋 설정. replaceSettingsOnReimport는 참이다(원본 쪽이 설정의 정본 — MeshAnimationAssets
    // 5.5.3절). 한 객체에 한 번만 부른다(실패한 뒤에도 — 두 번째 호출은 InvalidArgument).
    //
    // 경고(Warnings — 순서대로 context.Warn에 넘긴다): 설정 번역의 UnsupportedSetting(detail = 표
    // 행 번호, 모르는 키 ExchangeUnknownSettingDetail), LOD 0이 아닌 거리장 대체 메시(detail =
    // SourceModels 행), 버린 충돌 형상·값 UnsupportedCollisionShape(detail =
    // UnsupportedCollisionFeature), 이름 정규화 UnrealNameNormalized(detail = 바뀐 이름 수 — 끝에
    // 하나).
    //
    // 오류(failure.code·detail): 호출 계약(asset·registry 없음, 정적 메시가 아님, blobs 수)
    // InvalidSource / InvalidArgument, 덤프 형식(SourceModels 수와 data.lods 수가 다름 — detail =
    // 0, 원본이 있다는 LOD의 기록·preparedBuild 없음, 값 종류, 섹션 키 중복, 공유 소켓, 충돌 형상
    // 형식·CollisionEnabled·CollisionTraceFlag 이름) CorruptBundle / CorruptData(LOD 문제는 detail
    // = LOD, 충돌 형상은 그 종류 배열의 번호(BodySetup 자체는 0), 그 밖은 표 행 번호), 기록 적재
    // 실패는 LoadExchangeMeshSource의 코드(detail = LOD), 설정 번역의 값 종류 위반과 ImportJson
    // 진단 거부 InvalidSource / CorruptData(detail = LOD 또는 행), 행 표 결함·진단 없는 ImportJson
    // 실패· 타입 표·이름 pool·writer·할당 오류는 그 오류(코드 None). 실패하면 결과를 버린다.
    Result<void> Build(const ExchangeStaticMeshInput& input, ExchangeMeshFailure& failure) noexcept;

    const StaticMeshImportSpec& Spec() const noexcept {
        return spec_;
    }

    Span<const ExchangeSettingWarning> Warnings() const noexcept {
        return warnings_.View();
    }

    // LOD i의 원본·baked 원본이다(없으면 null — 시험이 본다).
    const MeshSource* LodSource(usize lod) const noexcept;
    const MeshSource* LodBakedSource(usize lod) const noexcept;

private:
    struct LodMeshes {
        uint32 source = NumericLimits<uint32>::Max();
        uint32 baked = NumericLimits<uint32>::Max();
    };

    friend struct ExchangeStaticMeshBuilder;

    Allocator* allocator_;
    // Build를 시작했다(결과가 실패여도 다시 부를 수 없다).
    bool started_ = false;
    JsonDocument translated_;
    Array<MeshSource> meshes_;
    Array<LodMeshes> lodMeshes_;
    Array<StaticMeshImportLod> lods_;
    Array<StaticMeshImportSlot> slots_;
    Array<MeshSectionInfoEntry> sections_;
    Array<StaticMeshImportSocket> sockets_;
    SimpleCollision collision_;
    Array<ConvexGeometry> convexes_;
    Array<ExchangeSettingWarning> warnings_;
    StaticMeshImportSpec spec_;
};

} // namespace rocks
