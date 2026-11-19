#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/PackageFormat.h>
#include <Core/Build/EngineVersion.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

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

// Summary 섹션 값이다(AS 5.2·5.3절). 에디터 패키지는 savedBy·compatibleWith, 쿠킹 패키지는
// profileId· profileSettings·cookerVersion을 가진다(다른 종류의 칸은 기본값).
struct PackageSummary {
    explicit PackageSummary(Allocator& allocator) noexcept
        : types(allocator), customVersions(allocator), profileId(allocator) {
    }

    Array<PackageTypeVersion> types;
    Array<PackageCustomVersion> customVersions;
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
          dependencies(allocator), bulkEntries(allocator) {
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
    // UnsupportedVersion을 낸다(AS 5.7절 18). options.types가 null이면 보지 않으므로 거짓이고, 쿠킹은
    // 그런 패키지를 UnsupportedVersion으로 거부하므로 언제나 거짓이다.
    bool unavailableExportType = false;
    // 헤더 + 섹션 표의 SHA-256이다(AS 5.5절 지문).
    PackageFingerprint fingerprint{};
};

// 헤더 64 byte를 검증한다(AS 5.1절 검증 1·2와 3의 헤더 부분). fileBytes는 Stat 크기다(헤더의
// fileBytes와 같아야 한다). AssetIndex처럼 범위만 읽는 도구가 먼저 부른다.
//
// 오류(진단 코드 / ErrorCode): 크기 < 64·magic·headerCrc·flags(Editor·Cooked 중 정확히 하나가
// 아님)는 CorruptPackage / InvalidArgument, formatVersion이 현재보다 높으면 에디터
// NewerVersion·쿠킹 UnsupportedVersion / Unsupported, 낮으면 UnsupportedVersion / Unsupported(v1이
// 첫 버전이다), headerBytes· bulkAlignment·reserved·sectionCount(1–32)·sectionTableOffset
// 정렬·fileBytes 식·Stat 크기 불일치·64 GiB 초과는 CorruptPackage / InvalidArgument다.
Result<PackageHeader> ReadPackageHeader(Span<const uint8> headerBytes, uint64 fileBytes,
                                        AssetDiagnostic* failure = nullptr) noexcept;

// 패키지 파일 전체를 검증하고 해석한다(AS 5.1절 검증 순서 1–4, 5.2절 섹션 규칙, 5.3절 버전 축).
// Objects graph의 내용 검증·Dependencies 대조·bulk 압축 해제는 PackageLoadState(PackageLoad.h)가
// 한다.
//
// 인자: allocator(결과 배열·문자열), file(패키지 byte 전체 — 결과가 빌린다), options.
// 오류: ReadPackageHeader의 오류, 섹션 표·섹션 내용 위반은 CorruptPackage / InvalidArgument, 버전
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
Result<PackageContents> ReadPackage(Allocator& allocator, Span<const uint8> file,
                                    const PackageReadOptions& options,
                                    AssetDiagnostic* failure = nullptr) noexcept;

// contents의 bulk entry를 모두 풀어 raw byte를 out 끝에 bulkId 순으로 이어 붙이고, entry별 위치를
// slots에 채운다(rawBytes와 SHA-256 확인). raw 합이 maxRawBytes를 넘으면 CapacityExceeded다.
//
// Zstd entry는 블록 컨테이너(RCBK)이며 헤더의 rawSize를 BulkIndex rawBytes와 먼저 대조한다(맞지
// 않으면 해제하지 않는다).
//
// 오류: 컨테이너 형식·Zstd가 아닌 codec·rawSize 불일치·블록 해제 실패·SHA-256 불일치는
// CorruptPackage / InvalidArgument(진단 detail = bulkId), 할당 실패는 OutOfMemory다. 실패하면
// out·slots는 호출 전 길이로 돌아간다.
Result<void> DecodePackageBulk(const PackageContents& contents, Array<uint8>& out,
                               Array<PackageBulkSlot>& slots, uint64 maxRawBytes,
                               AssetDiagnostic* failure = nullptr) noexcept;

// Guid의 정렬 순서다((high, low) 부호 없는 수치 순 — AS 4.2절).
constexpr bool GuidLess(const Guid& left, const Guid& right) noexcept {
    return left.high != right.high ? left.high < right.high : left.low < right.low;
}

} // namespace rocks
