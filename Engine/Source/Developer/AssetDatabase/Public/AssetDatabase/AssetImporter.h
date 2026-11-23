#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/BulkData.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Meta/TypeSchema.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeLease.h>

// 임포터 인터페이스와 표다(Docs/Design/AssetSystem 7.1절 18 — A2f1a; UE UFactory·
// FReimportHandler 대응). 임포터는 원본 byte와 옵션 값에서 출력 에셋 객체를 채운다. 패키지 만들기·
// 기록·저장은 서비스가 한다(AssetService::Import — A2f1b). 모듈이 표에 등록하고 등록 해제 토큰을
// 쥔다.
namespace rocks {

class TypeRegistry;

// 원본이 하나뿐인 임포터의 역할 이름이다(ImportRecord sources[].role — AS 5.4절).
inline constexpr StringView DefaultImportSourceRole = "source";
// 기본 확장자 하나의 byte 상한과 임포터 하나의 확장자 수 상한이다.
inline constexpr usize MaxImporterExtensionBytes = 16;
inline constexpr usize MaxImporterExtensions = 32;

// 명령 예약 이름이다(13절 — 옵션 필드 이름이 될 수 없다, 대소문자 무시).
inline constexpr StringView ImportReservedNames[] = {
    "source", "importer", "project", "wait",       "unset",  "erase",       "force",
    "help",   "cube",     "array",   "cube-array", "volume", "cube-layout", "volume-tile"};

// 임포터의 고정 정보다(Info()가 돌려주는 값은 등록부터 해제까지 바뀌지 않는다).
struct AssetImporterInfo {
    // 기록·재임포트가 임포터를 찾는 id(ImportRecord importerId)다. null이 아니다.
    Guid id{};
    // `--importer=<이름>`과 기록 importerName이다: 1–MaxImporterNameBytes byte의 `[A-Za-z0-9_]`.
    StringView name{};
    // 기록 importerVersion이다(재임포트가 기록보다 낮으면 거부, 높으면 ImporterOutdated 상태).
    uint32 version = 0;
    // 주 출력 타입(추상 아닌 등록 에셋 클래스)이다. 설정 대입의 대상이다.
    TypeId outputType{};
    // 이 임포터가 기본이 되는 원본 확장자(점 없음, 1–MaxImporterExtensionBytes byte의
    // `[a-z0-9]`)다. 비면 `--importer=`로만 고른다.
    Span<const StringView> extensions{};
    // 원본 역할 목록(ImportRecord 역할 규칙, 유일)이다. 비면 원본 하나 DefaultImportSourceRole.
    Span<const StringView> roles{};
    // 옵션 struct(등록 ROCKS_STRUCT)다. 0이면 옵션 없음. 필드 이름은 outputType의 노출 속성
    // 이름·ImportReservedNames와 겹칠 수 없다(대소문자 무시 — 옵션과 설정을 첫 이름으로 나눈다).
    TypeId optionsType{};
};

// 임포트 원본 하나다(문맥이 준다 — 수명은 Import 호출 동안). path는 표시용 OS 경로다.
struct AssetImportSource {
    StringView role{};
    StringView path{};
    Span<const uint8> bytes{};
};

// 임포터가 받는 문맥이다(서비스가 구현한다 — AssetService::Import·Reimport). 소유 스레드에서
// Import 호출 동안만 쓴다.
class AssetImportContext {
public:
    // 원본들이다(임포터 roles 순 — 원본 하나면 역할 DefaultImportSourceRole).
    virtual Span<const AssetImportSource> Sources() const noexcept = 0;
    // 옵션 struct 값(optionsType의 C++ 값 — 읽기 전용)이다. 옵션이 없으면 null.
    virtual const void* Options() const noexcept = 0;
    // 재임포트인지다(출력이 기존 에셋 — 제자리 갱신).
    virtual bool IsReimport() const noexcept = 0;
    // 출력 객체의 Registry와 접근 문맥이다(필드 대입 — Pointer·SetReference).
    virtual ObjectRegistry& Registry() noexcept = 0;
    virtual const ObjectAccessContext& Access() const noexcept = 0;

    // key 출력의 대표 에셋이다(빈 key = 주 출력, 그 밖은 `<path>_<key>`). 새 임포트는 새 패키지
    // (저장하지 않은 — 끝에 서비스가 저장한다), 재임포트는 기록의 같은 key 출력을 연 것이다. 같은
    // key를 두 번 부르면 InvalidArgument다. 오류(진단은 서비스가 실패에 쓴다): key 규칙
    // (ImportRecord outputKey — 1–MaxImportOutputKeyBytes byte의 `[A-Za-z0-9_]`, 주 출력은 빈 key)·
    // 개수 MaxImportOutputs·type(추상 아닌 에셋 클래스, 주 출력은 Info().outputType) 위반
    // InvalidArgument, 경로 규칙 InvalidPath / InvalidArgument, 점유(색인·디스크·저장하지 않은 새
    // 패키지·별칭) DuplicatePath / AlreadyExists, 패키지 만들기·열기의 오류, 할당 실패.
    virtual Result<ObjectHandle> Output(StringView key, TypeId type) noexcept = 0;
    // output(Output이 준 대표 에셋)의 패키지에 저장하지 않은 bulk를 더한다(BulkDataRef 필드에
    // 넣는다). 오류: Output이 주지 않은 객체 InvalidArgument, 할당 실패.
    virtual Result<BulkDataRef> AddBulk(ObjectHandle output, Array<uint8>&& raw) noexcept = 0;
    // 경고 하나를 남긴다(AssetDiagnosticIsWarning인 코드만 — 아니면 InvalidArgument). asset은 주
    // 출력, detail은 임포터가 정한다. 오류: 할당 실패.
    virtual Result<void> Warn(AssetDiagnosticCode code, uint64 detail = 0) noexcept = 0;
    // 실패 진단을 남기고 Failure(error)를 돌려준다(임포터가 `return context.Fail(…)`로 쓴다 —
    // 서비스 실패의 진단이 된다).
    virtual Result<void> Fail(AssetDiagnosticCode code, ErrorCode error,
                              uint64 detail = 0) noexcept = 0;

protected:
    ~AssetImportContext() = default;
};

// 임포터다. IO를 하지 않고(원본 byte는 문맥이 준다) 결정적이어야 한다(같은 원본·옵션 → 같은 출력).
// 임포트가 정하는 필드는 매번 모두 쓴다(재임포트는 제자리 갱신이라 쓰지 않은 옛 값이 남는다).
// 실패는 그 오류를 돌려주고 출력은 버려진다(기존 에셋 보존).
class AssetImporter {
public:
    virtual ~AssetImporter() = default;

    virtual const AssetImporterInfo& Info() const noexcept = 0;
    virtual Result<void> Import(AssetImportContext& context) noexcept = 0;
};

class AssetImporterTable;

// 등록 해제 토큰이다(소멸하면 해제 — 이동만 된다). 표보다 먼저 사라져야 한다.
class ROCKS_ASSETDATABASE_API AssetImporterRegistration {
public:
    AssetImporterRegistration() noexcept = default;
    AssetImporterRegistration(AssetImporterRegistration&& other) noexcept;
    AssetImporterRegistration& operator=(AssetImporterRegistration&& other) noexcept;
    AssetImporterRegistration(const AssetImporterRegistration&) = delete;
    AssetImporterRegistration& operator=(const AssetImporterRegistration&) = delete;
    ~AssetImporterRegistration();

    void Release() noexcept;

    bool IsActive() const noexcept {
        return table_ != nullptr;
    }

private:
    friend class AssetImporterTable;

    AssetImporterTable* table_ = nullptr;
    AssetImporter* importer_ = nullptr;
};

// 등록 임포터 표다. 소유 스레드에서만 쓴다.
class ROCKS_ASSETDATABASE_API AssetImporterTable {
public:
    AssetImporterTable(Allocator& allocator, TypeRegistry& types) noexcept;

    AssetImporterTable(const AssetImporterTable&) = delete;
    AssetImporterTable& operator=(const AssetImporterTable&) = delete;

    // importer를 등록한다(importer는 토큰보다 오래 살아야 한다). 확인: Info 규칙(위), 같은 id·같은
    // 이름(대소문자 무시)·같은 기본 확장자가 이미 있으면 AlreadyExists(확장자마다 기본 하나 —
    // 결정적, UE ImportPriority 경합 대신). 오류: 규칙 위반 InvalidArgument(옵션 필드 이름 충돌
    // 포함), outputType·optionsType 조회 오류(없으면 NotFound, 종류·추상이 맞지 않으면
    // InvalidArgument), AlreadyExists, 할당 실패.
    //
    // 사용 예(모듈 시작):
    //   auto registered = table.Register(textureImporter);
    //   if (!registered) { return registered.Error(); }
    //   registration_ = rocks::Move(registered.Value());
    Result<AssetImporterRegistration> Register(AssetImporter& importer) noexcept;

    // id·이름(대소문자 무시)·기본 확장자(점 없음, ASCII 대소문자 무시)로 찾는다. 없으면 null.
    AssetImporter* FindById(Guid id) const noexcept;
    AssetImporter* FindByName(StringView name) const noexcept;
    AssetImporter* FindByExtension(StringView extension) const noexcept;

    // 등록 importer의 주 출력·옵션 타입 lease다(등록 동안 쥔다 — 그 타입 등록이 풀리지 않는다).
    // 옵션이 없거나 등록되지 않은 importer면 null이다.
    const TypeLease* OutputType(const AssetImporter& importer) const noexcept;
    const TypeLease* OptionsType(const AssetImporter& importer) const noexcept;

    usize Count() const noexcept {
        return importers_.Size();
    }

private:
    friend class AssetImporterRegistration;

    // 등록 하나다(타입 lease는 등록 동안 쥔다).
    struct Entry {
        AssetImporter* importer = nullptr;
        TypeLease output;
        TypeLease options;
    };

    Result<void> Validate(const AssetImporterInfo& info, Entry& entry) noexcept;
    const Entry* EntryOf(const AssetImporter& importer) const noexcept;
    void Remove(AssetImporter* importer) noexcept;

    TypeRegistry* types_;
    Array<Entry> importers_;
};

} // namespace rocks
