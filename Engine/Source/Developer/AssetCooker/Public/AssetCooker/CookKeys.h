#pragma once

#include <AssetCooker/CookProcessor.h>
#include <AssetCooker/CookTargets.h>
#include <ObjectSerialization/ClassDefaults.h>

// 쿠킹 키 계산이다(Docs/Design/AssetSystem 10.2절 결정 5, "A4c 구현 세부" — A4c1). 패키지를
// 열지 않고 색인으로만 정한다(UE 증분 쿠킹의 TargetDomain 키처럼). build key는 Build SCC, cookKey는
// 대상 사이 Hard SCC로 구한다(9절 너겟 키 — 순환도 결정적 키). 에셋 문제(콘텐츠 해시 실패·Build
// 의존 없음·처리기를 못 고름)는 진단이고 그 에셋과 그것에 기대는 에셋은 키가 없다.
//
// 사용 예:
//   CookKeyRequest request;
//   request.index = &index;
//   request.files = &files;
//   request.types = &types;
//   request.processors = &processors;
//   request.targets = &targets;
//   request.profileSettings = profileHash;
//   CookKeySet keys(allocator);
//   if (auto computed = ComputeCookKeys(allocator, request, keys); !computed) { ... }
namespace rocks {

// cookKey 공통 머리의 쿠커 정체성이다(쿠커 키 배치·의미가 바뀌면 CookerKeyVersion을 바꾼다).
inline constexpr Guid CookerProcessorGuid{0x524f434b434f4f4bull, 0x0000000000000001ull};
inline constexpr Guid CookerKeyVersion{0x524f434b4b455931ull, 0x0000000000000001ull};
// 쿠커 버전이다(매니페스트 cookerVersion — 출력 의미가 바뀌면 올린다).
inline constexpr uint32 CookerVersion = 1;
// SCC 외부 튜플의 역할 이름이다.
inline constexpr StringView CookBuildRole = "Build";
inline constexpr StringView CookHardRole = "Hard";

// 쿠킹 시작 때 만드는 클래스 기본값 표의 한 행이다(RuntimeClassMetadata 'A5c 지문 연결'):
// 등록된 구체 클래스 하나의 레코드 유무와 cooked 지문(Present일 때)·이유(그 밖).
struct CookClassDefaults {
    TypeId type{};
    ClassDefaultsAvailability availability = ClassDefaultsAvailability::Failed;
    Sha256Digest cooked{};
    ClassDefaultsDiagnostic diagnostic{};
};

// 클래스 기본값 표다(TypeId 오름차순 — BuildCookClassDefaults). cookKey와 처리기가 같이 쓴다.
struct CookClassDefaultsTable {
    explicit CookClassDefaultsTable(Allocator& allocator) noexcept : rows(allocator) {
    }

    // type의 행이다(없으면 null — 미등록·클래스가 아님·추상).
    const CookClassDefaults* Find(TypeId type) const noexcept;

    Array<CookClassDefaults> rows;
};

// 등록된 구체 클래스마다 레코드를 만들어 표를 채운다(ForEachClassDefaults — 쿠킹은 모든 레코드를
// 강제로 만들어 실패를 미리 드러낸다, 비용은 구체 클래스 수 × 생성 2회). 레코드 실패는 행으로
// 남긴다(오류가 아니다). 오류: 기억하지 않는 일시 실패(할당 등)·목록 복사의 오류.
ROCKS_ASSETCOOKER_API Result<void> BuildCookClassDefaults(Allocator& allocator, TypeRegistry& types,
                                                          CookClassDefaultsTable& out) noexcept;

struct CookKeyRequest {
    // 색인이다(콘텐츠 해시를 두므로 비const — 색인 소유 스레드에서 부른다).
    AssetIndex* index = nullptr;
    platform::FileSystem* files = nullptr;
    TypeRegistry* types = nullptr;
    const CookProcessorTable* processors = nullptr;
    // A4b 결과다(대상·제외 목록).
    const CookTargetSet* targets = nullptr;
    // 프로파일 출력 설정 SHA-256이다(HashTargetProfile).
    Sha256Digest profileSettings{};
    // 프로젝트 세대 Guid다(9.1절 결정 4 — 없으면 null).
    Guid projectGeneration{};
    uint32 cookerVersion = CookerVersion;
    // 클래스 기본값 표다(A5c — null이면 ComputeCookKeys가 types로 만든다).
    const CookClassDefaultsTable* classDefaults = nullptr;
};

struct CookKey {
    AssetId asset{};
    Sha256Digest contentHash{};
    Sha256Digest buildKey{};
    Sha256Digest cookKey{};
    // 키를 구했는지다(거짓이면 원인 진단이 있거나 기대는 에셋이 키가 없다).
    bool hasKey = false;
};

struct CookKeySet {
    explicit CookKeySet(Allocator& allocator) noexcept : keys(allocator), diagnostics(allocator) {
    }

    // 키 진단은 모두 오류다(경고 없음).
    bool HasErrors() const noexcept {
        return !diagnostics.IsEmpty();
    }

    // 대상과 같은 순서(AssetId 순)다.
    Array<CookKey> keys;
    // 진단(CookDiagnosticBefore 순)이다.
    Array<CookDiagnostic> diagnostics;
    // 타입 표 지문이다(등록 타입 전체의 (TypeId, 종류, 버전) — 진단·보고서용).
    Sha256Digest typeTable{};
};

// 키를 계산한다(out을 비우고 채운다). 진단: 콘텐츠 해시 실패(파일 없음 MissingFile, 그 밖 None —
// error 칸에 원인), Build 의존 없음 MissingDependency·같은 id DuplicateAssetId(asset = 참조자,
// related = 의존), 대표 타입 처리기를 못 고름(미등록 UnsupportedVersion·클래스 아님 TypeMismatch —
// error = 조회 오류 NotFound·TypeMismatch), export 클래스가 기본값 표에 없음(UnsupportedVersion,
// related = AssetId{TypeId}, error NotFound)·레코드 실패(InvalidClassDefaults, related =
// AssetId{TypeId}, error = 레코드 오류 — A5c). cookKey 지역 입력에 export 타입마다 (TypeId, 유무
// 상태, cooked 지문)이 들어간다. 오류: 요청 인자 null InvalidArgument, TypeRegistry
// 오류(종료 뒤 ShuttingDown 등), 처리기 설정 키의 writer 오류, 할당 실패.
ROCKS_ASSETCOOKER_API Result<void>
ComputeCookKeys(Allocator& allocator, const CookKeyRequest& request, CookKeySet& out) noexcept;

// 등록 타입 전체 목록의 지문이다(ListTypeVersions — 개수, 칸마다 TypeId·종류·버전). 오류:
// ListTypeVersions의 오류, 할당 실패.
ROCKS_ASSETCOOKER_API Result<Sha256Digest> HashTypeTable(Allocator& allocator,
                                                         const TypeRegistry& types) noexcept;

} // namespace rocks
