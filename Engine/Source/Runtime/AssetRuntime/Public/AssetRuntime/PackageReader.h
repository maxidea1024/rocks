#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/PackageFormat.h>
#include <Core/Build/EngineVersion.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Object/ClassDefaults.h>

namespace rocks {

class TypeRegistry;

// Exports 항목이다(AS 5.2절). name은 입력 byte를 빌린다(PackageContents 수명).
struct PackageExportEntry {
    StringView name{};
    TypeId type{};
    // 0 = 패키지, 1..자기 번호 − 1 = export.
    uint32 outerIndex = 0;
    uint32 flags = 0;
};

// ObjectImports 항목이다(AS 5.2절). outerImportIndex 0은 "그 패키지의 대표 에셋이 Outer"이고, 빈
// name + outerImportIndex 0은 대표 에셋 자신이다. name은 입력 byte를 빌린다.
struct PackageImportEntry {
    AssetId asset{};
    uint32 outerImportIndex = 0;
    StringView name{};
    TypeId type{};
};

// Dependencies 항목이다(AS 5.2절). expectedType은 Hard 의존이면 저장 때 대상 대표 에셋의 타입, 그
// 밖은 0이다(구현에서 정한 점 — Soft 대상의 타입은 로드 요청의 expectedType이 판정한다).
struct PackageDependencyEntry {
    AssetId asset{};
    PackageDependencyKind kind = PackageDependencyKind::Hard;
    TypeId expectedType{};
    bool required = false;
};

// BulkIndex 항목이다(AS 5.2절). offset은 Bulk 섹션 시작 기준이다.
struct PackageBulkEntry {
    uint32 bulkId = 0;
    uint32 flags = 0;
    uint64 offset = 0;
    uint64 storedBytes = 0;
    uint64 rawBytes = 0;
    BulkCodec codec = BulkCodec::None;
    uint32 crc32 = 0;
    Sha256Digest sha256{};
};

// Summary 타입 스키마 버전 표 항목이다(TypeId 오름차순 — Guid (high, low) 부호 없는 수치 순).
struct PackageTypeVersion {
    TypeId type{};
    uint32 version = 0;
};

// Summary 커스텀 버전 표 항목이다(id 오름차순).
struct PackageCustomVersion {
    Guid id{};
    uint32 version = 0;
};

// Summary 클래스 기본값 목록 항목이다(A5c — 레코드 있는 export 클래스의 저장 당시 지문, TypeId
// 오름차순). 지문 구성은 패키지 종류를 따른다(에디터 Editor, 쿠킹 Cooked).
struct PackageClassDefaults {
    TypeId type{};
    Sha256Digest fingerprint{};
};

// 에디터 로드에서 지금 레코드를 만들 수 없는 export 클래스다(A5c — 레코드 실패, 이 패키지는 그
// 타입을 고치기 전까지 저장할 수 없다).
struct PackageUnavailableDefaults {
    TypeId type{};
    ClassDefaultsReason reason = ClassDefaultsReason::None;
};

// Summary 섹션 값이다(AS 5.2·5.3절). 에디터 패키지는 savedBy·compatibleWith, 쿠킹 패키지는
// profileId· profileSettings·cookerVersion을 가진다(다른 종류의 칸은 기본값).
struct PackageSummary {
    explicit PackageSummary(Allocator& allocator) noexcept
        : types(allocator), customVersions(allocator), classDefaults(allocator),
          profileId(allocator) {
    }

    Array<PackageTypeVersion> types;
    Array<PackageCustomVersion> customVersions;
    // 클래스 기본값 목록이다(A5c — 행 ⊆ types의 타입, 레코드 없는 타입은 행이 없다).
    Array<PackageClassDefaults> classDefaults;
    EngineVersion savedBy{};
    EngineVersion compatibleWith{};
    String profileId;
    Sha256Digest profileSettings{};
    uint32 cookerVersion = 0;
};

// 읽기 옵션이다.
struct PackageReadOptions {
    // 기대하는 패키지 종류다. 헤더 종류가 다르면 쿠킹 기대는 UnsupportedVersion(다시 쿠킹), 에디터
    // 기대는 CorruptPackage다(에디터 도구는 쿠킹 패키지를 열지 않는다).
    PackageKind expectedKind = PackageKind::Cooked;
    // 타입 스키마 버전 판정에 쓰는 등록 저장소다(null이면 타입 축을 보지 않는다 — 헤더·표만 보는
    // 도구). 쿠킹: 모든 타입의 등록 버전이 Summary와 같아야 하고 미등록·불일치는
    // UnsupportedVersion이다. 에디터: 낮으면 통과(마이그레이션은 그래프 로드), 높거나 미등록이면
    // 읽기 전용(readOnly)이다.
    TypeRegistry* types = nullptr;
    // 현재 커스텀 버전 표다(id 오름차순). 쿠킹은 Summary 표와 정확히 같아야 하고(아니면
    // UnsupportedVersion) 에디터는 높거나 모르는 id면 읽기 전용이다.
    Span<const PackageCustomVersion> customVersions{};
    // 현재 엔진 버전이다(에디터 패키지 compatibleWith 판정 — 주·부가 더 새면 NewerVersion). 빈
    // 값이면 엔진 축을 보지 않는다(시험·도구가 고정 값을 줄 때는 그 값).
    EngineVersion engineVersion{};
};

// 검증을 마친 패키지 내용이다. 섹션 span·이름은 ReadPackage에 준 file byte를 빌리므로 그 byte가 이
// 값보다 오래 살아야 한다. 이동 전용이다.
struct PackageContents {
    explicit PackageContents(Allocator& allocator) noexcept
        : sections(allocator), summary(allocator), exports(allocator), imports(allocator),
          dependencies(allocator), bulkEntries(allocator), changedDefaults(allocator),
          unavailableDefaults(allocator) {
    }

    PackageContents(PackageContents&&) noexcept = default;
    PackageContents& operator=(PackageContents&&) noexcept = default;
    PackageContents(const PackageContents&) = delete;
    PackageContents& operator=(const PackageContents&) = delete;

    PackageHeader header{};
    Array<PackageSectionEntry> sections;
    PackageSummary summary;
    Array<PackageExportEntry> exports;
    Array<PackageImportEntry> imports;
    Array<PackageDependencyEntry> dependencies;
    // graph v2 byte다(Objects 섹션).
    Span<const uint8> objects{};
    // 에디터 패키지의 ImportRecord·Tags Record byte다(없으면 빈 span).
    Span<const uint8> importRecord{};
    Span<const uint8> tags{};
    // Bulk 섹션 byte와 항목이다(없으면 비어 있다). 항목의 저장 CRC는 이미 확인했다(raw SHA-256은
    // DecodePackageBulk가 압축을 풀며 확인한다).
    Span<const uint8> bulk{};
    Array<PackageBulkEntry> bulkEntries;
    // 에디터 패키지를 읽기 전용으로 열어야 하는지다(더 새 타입·커스텀 버전, 건너뛴 Required가 아닌
    // 모르는 섹션·더 새 섹션 — AS 5.3절). 쿠킹 패키지는 언제나 거짓이다.
    bool readOnly = false;
    // 에디터 패키지의 Summary 타입 표(= export 타입 집합)에 현재 클래스가 없는 타입(미등록, 또는
    // 클래스가 아닌 타입 id)이 있는지다(readOnly도 참이다). PackageLoadState::Prepare는 이 판정으로
    // UnsupportedVersion을 낸다(AS 5.7절 18). options.types가 null이면 보지 않으므로 거짓이고,
    // 쿠킹은 그런 패키지를 UnsupportedVersion으로 거부하므로 언제나 거짓이다.
    bool unavailableExportType = false;
    // 에디터 패키지에서 저장 당시와 지금의 클래스 기본값이 다른 export 클래스다(A5c — 지문이
    // 다르거나 행 유무와 지금 레코드 유무가 다르다, TypeId 오름차순). 쿠킹은 그런 패키지를
    // UnsupportedVersion으로 거부하므로 언제나 비어 있다. options.types가 null이면 보지 않는다.
    Array<TypeId> changedDefaults;
    // 에디터 패키지에서 지금 레코드를 만들 수 없는 export 클래스다(A5c — 레코드 실패, TypeId
    // 오름차순). changedDefaults와 겹치지 않는다.
    Array<PackageUnavailableDefaults> unavailableDefaults;
    // 헤더 + 섹션 표의 SHA-256이다(AS 5.5절 지문).
    PackageFingerprint fingerprint{};
};

// 헤더 64 byte를 검증한다(AS 5.1절 검증 1·2와 3의 헤더 부분). fileBytes는 Stat 크기다(헤더의
// fileBytes와 같아야 한다). AssetIndex처럼 범위만 읽는 도구가 먼저 부른다.
//
// 오류(진단 코드 / ErrorCode): 크기 < 64·magic·headerCrc·flags(Editor·Cooked 중 정확히 하나가
// 아님)는 CorruptPackage / CorruptData, formatVersion이 현재보다 높으면 에디터
// NewerVersion·쿠킹 UnsupportedVersion / Unsupported, 낮으면 UnsupportedVersion /
// Unsupported(하위호환 없음 — v2는 A5c 차이 형식·클래스 기본값 목록), headerBytes·
// bulkAlignment·reserved·sectionCount(1–32)·sectionTableOffset 정렬·fileBytes 식·Stat 크기
// 불일치·64 GiB 초과는 CorruptPackage / CorruptData다(읽은 데이터 손상 — AssetSystem 19장).
ROCKS_ASSETRUNTIME_API Result<PackageHeader>
ReadPackageHeader(Span<const uint8> headerBytes, uint64 fileBytes,
                  AssetDiagnostic* failure = nullptr) noexcept;

// 패키지 파일 전체를 검증하고 해석한다(AS 5.1절 검증 순서 1–4, 5.2절 섹션 규칙, 5.3절 버전 축).
// Objects graph의 내용 검증·Dependencies 대조·bulk 압축 해제는 PackageLoadState(PackageLoad.h)가
// 한다.
//
// 인자: allocator(결과 배열·문자열), file(패키지 byte 전체 — 결과가 빌린다), options.
// 오류: ReadPackageHeader의 오류, 섹션 표·섹션 내용 위반은 CorruptPackage / CorruptData, 버전
// 축은 UnsupportedVersion·NewerVersion / Unsupported, 할당 실패와 options.types 조회의 버전 판정
// 밖 실패(종료된 TypeRegistry의 ShuttingDown 등)는 그 오류(진단 코드 None)다. 실패하면
// failure(있으면)에 분류와 섹션·위치를 채운다.
//
// 사용 예:
//   PackageReadOptions options;
//   options.types = &types;
//   AssetDiagnostic failure;
//   auto contents = ReadPackage(allocator, bytes.View(), options, &failure);
//   if (!contents) {
//       Log::Warning(LogAsset, "{Code}", AssetDiagnosticName(failure.code));
//   }
ROCKS_ASSETRUNTIME_API Result<PackageContents>
ReadPackage(Allocator& allocator, Span<const uint8> file, const PackageReadOptions& options,
            AssetDiagnostic* failure = nullptr) noexcept;

// 부분 읽기 결과다(AS 7.1절 13 — A2b). contents의 view(이름·ImportRecord·Tags 등)는 buffers의 소유
// byte를 가리키므로 이 값을 옮겨도 유효하다. Objects·Bulk·BulkIndex는 읽지 않아 contents.objects·
// bulk·bulkEntries는 비어 있다.
struct PackageIndexView {
    explicit PackageIndexView(Allocator& allocator) noexcept
        : buffers(allocator), contents(allocator) {
    }

    PackageIndexView(PackageIndexView&&) noexcept = default;
    PackageIndexView& operator=(PackageIndexView&&) noexcept = default;
    PackageIndexView(const PackageIndexView&) = delete;
    PackageIndexView& operator=(const PackageIndexView&) = delete;

    Array<Array<uint8>> buffers;
    PackageContents contents;
};

// 범위 읽기로 헤더·섹션 표와 Summary·Exports·ObjectImports·Dependencies·ImportRecord·Tags만 읽어
// ReadPackage와 같은 판정을 한다(AS 7.1절 13). 건너뛰는 것: Objects·Bulk·BulkIndex 섹션의 내용·CRC,
// 섹션 사이 패딩 byte 0 검사, bulk 대조(표의 위치·정렬·겹침·flags·schemaVersion 판정은 한다).
// 그래서 헤더·표·읽은 섹션의 손상은 ReadPackage와 같은 진단이고, 건너뛴 구간의 손상은 여기서는
// 통과하고 열기(ReadPackage)에서 거부된다. 오류: ReadPackage와 같고, 범위 읽기의 IO 오류는 그
// 오류(진단 None)다.
//
// 사용 예(색인 검색):
//   FilePackageRangeReader reader;
//   reader.Open(files, path);
//   auto view = ReadPackageIndex(allocator, reader, options, &diagnostic);
//   if (view) { /* view.Value().contents.exports[0].type — 대표 타입 */ }
class PackageRangeReader;
ROCKS_ASSETRUNTIME_API Result<PackageIndexView>
ReadPackageIndex(Allocator& allocator, PackageRangeReader& reader,
                 const PackageReadOptions& options, AssetDiagnostic* failure = nullptr) noexcept;

// 에디터 지연 열기의 읽기다(AS 7.1절 15 — A2c1): Bulk 섹션 내용만 빼고 전체 읽기와 같은 판정을
// 한다(BulkIndex는 읽어 배치를 Bulk 섹션 크기와 대조한다 — entry 사이 패딩·저장 CRC는 지연 읽기가
// entry마다 본다). 섹션 사이 패딩은 읽지 않는다. 결과는 ReadPackageIndex처럼 소유 버퍼를 가리킨다
// (contents.bulk는 비고 bulkEntries는 채운다). 오류는 ReadPackageIndex와 같다.
ROCKS_ASSETRUNTIME_API Result<PackageIndexView>
ReadPackageForEdit(Allocator& allocator, PackageRangeReader& reader,
                   const PackageReadOptions& options, AssetDiagnostic* failure = nullptr) noexcept;

// 패키지 파일의 지문(헤더 + 섹션 표 + tableCrc의 SHA-256 — PackageContents::fingerprint와 같다)을
// 범위 읽기로 얻는다(AS 5.5절 1, 7.1절 15 — 저장의 외부 변경 검출·지연 읽기의 지문 대조). 헤더
// 판정과 섹션 표 CRC만 본다. 오류: ReadPackageHeader의 진단, 표 CRC 위반 CorruptPackage /
// CorruptData, 범위 읽기 오류는 진단 None과 그 오류, 할당 실패.
ROCKS_ASSETRUNTIME_API Result<PackageFingerprint>
ReadPackageFingerprint(Allocator& allocator, PackageRangeReader& reader,
                       AssetDiagnostic* failure = nullptr) noexcept;

// 저장한 패키지 파일의 재검증이다(AS 5.5절 5, 7.1절 15 — A2c1). 에디터 패키지로 헤더·섹션 표·
// 모든 섹션 CRC·섹션 사이 패딩 0·BulkIndex 배치·entry 사이 패딩 0·entry 저장 CRC를 scratch 크기
// 조각으로 훑는다(버전 축은 보지 않는다, 압축 해제·SHA-256·그래프 해석은 하지 않는다). 같은
// 손상이면 전체 읽기(ReadPackage)와 같은 진단이다. 목적은 짧은 쓰기·배치 오류·sink 버그다 — CRC는
// writer가 자기 출력으로 계산하므로 writer가 잘못된 내용을 올바른 CRC로 쓴 경우는 잡지 못한다.
//
// 오류: 빈 scratch InvalidArgument(진단 없음), 판정 위반은 ReadPackage와 같은 진단, 범위 읽기
// 오류는 그 오류(진단 None), 할당 실패(작은 섹션과 BulkIndex만 할당한다).
ROCKS_ASSETRUNTIME_API Result<void> VerifyPackageFile(Allocator& allocator,
                                                      PackageRangeReader& reader,
                                                      Span<uint8> scratch,
                                                      AssetDiagnostic* failure = nullptr) noexcept;

// bulk entry 하나의 저장 byte(stored — entry.storedBytes)를 raw(entry.rawBytes)에 풀고 SHA-256을
// 확인한다(DecodePackageBulk와 에디터 지연 읽기가 같이 쓴다). 오류: stored·raw 크기가 entry와
// 다르면 InvalidArgument(호출자 인자), 컨테이너 형식·rawSize 불일치·해제 실패·SHA-256 불일치는
// CorruptData, 할당 실패.
ROCKS_ASSETRUNTIME_API Result<void> DecodePackageBulkEntry(const PackageBulkEntry& entry,
                                                           Span<const uint8> stored,
                                                           Span<uint8> raw,
                                                           Allocator& allocator) noexcept;

// 위와 같되 out을 entry.rawBytes로 키워 쓴다. Zstd entry는 out을 키우기 전에 컨테이너 헤더의
// rawSize를 entry와 대조한다(손상된 표로 큰 할당을 하지 않는다). 오류는 위와 같다.
ROCKS_ASSETRUNTIME_API Result<void> DecodePackageBulkEntry(const PackageBulkEntry& entry,
                                                           Span<const uint8> stored,
                                                           Array<uint8>& out,
                                                           Allocator& allocator) noexcept;

// contents의 bulk entry를 모두 풀어 raw byte를 out 끝에 bulkId 순으로 이어 붙이고, entry별 위치를
// slots에 채운다(rawBytes와 SHA-256 확인). raw 합이 maxRawBytes를 넘으면 CapacityExceeded다.
//
// Zstd entry는 블록 컨테이너(RCBK)이며 헤더의 rawSize를 BulkIndex rawBytes와 먼저 대조한다(맞지
// 않으면 해제하지 않는다).
//
// 오류: 컨테이너 형식·Zstd가 아닌 codec·rawSize 불일치·블록 해제 실패·SHA-256 불일치는
// CorruptPackage / CorruptData(진단 detail = bulkId), 할당 실패는 OutOfMemory다. 실패하면
// out·slots는 호출 전 길이로 돌아간다.
ROCKS_ASSETRUNTIME_API Result<void>
DecodePackageBulk(const PackageContents& contents, Array<uint8>& out, Array<PackageBulkSlot>& slots,
                  uint64 maxRawBytes, AssetDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
