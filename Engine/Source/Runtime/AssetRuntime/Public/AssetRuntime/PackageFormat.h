#pragma once

#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <Meta/TypeSchema.h>
#include <Object/AssetId.h>

// 패키지 포맷 v1의 상수·열거와 헤더·섹션 표 값이다(Docs/Design/AssetSystem 5.1·5.2절).
// 쓰기는 PackageWriter.h, 읽기·검증은 PackageReader.h가 이 정의를 같이 쓴다.
//
// 물리 배치(모든 정수 little-endian, Guid는 high·low u64):
//   [0, 64)          헤더(PackageHeader — 아래 offset 표)
//   [64, table)      섹션들(kind 오름차순, 비-Bulk 8 정렬, Bulk는 bulkAlignment 정렬)
//   [table, end)     섹션 표 sectionCount × 32 byte + u32 tableCrc
// 섹션 사이 byte는 정렬 패딩(0)뿐이다.
// 헤더 offset: 0 magic "RPKG", 4 formatVersion(u16), 6 headerBytes(u16 = 64), 8 flags(u32 — Editor
// 1· Cooked 2 중 정확히 하나), 12 sectionCount(u32, 1–32), 16 sectionTableOffset(u64, 8 정렬), 24
// fileBytes(u64 = table + 32 × count + 4), 32 assetId(Guid), 48 bulkAlignment(u32), 52 reserved(8
// byte, 0), 60 headerCrc(u32 = CRC32(byte 0–59)).
namespace rocks {

// 패키지 파일의 첫 4 byte다("RPKG").
inline constexpr uint8 PackageMagic[4] = {'R', 'P', 'K', 'G'};

// 이 엔진이 쓰고 읽는 컨테이너 버전이다(AS 5.3절 컨테이너 축).
inline constexpr uint16 PackageFormatVersion = 1;

// 헤더 크기다(모든 포맷 버전에서 고정 — 버전 판정 전에 headerCrc 위치를 알아야 한다).
inline constexpr uint32 PackageHeaderBytes = 64;

// 섹션 표 항목 하나의 크기다({kind, flags, offset, bytes, crc32, schemaVersion}).
inline constexpr uint32 PackageSectionEntryBytes = 32;

// 섹션 수 상한이다.
inline constexpr uint32 PackageMaxSections = 32;

// 파일 크기 상한이다(64 GiB).
inline constexpr uint64 PackageMaxFileBytes = uint64{64} * 1024 * 1024 * 1024;

// 비-Bulk 섹션 하나의 크기 상한이다(64 MiB — graph v2 maxBytes와 같다).
inline constexpr uint64 PackageMaxSectionBytes = uint64{64} * 1024 * 1024;

// export 수 상한이다(graph v2 maxObjects와 같다).
inline constexpr uint32 PackageMaxExports = 4096;

// object import 수 상한이다(graph v2 maxExternalReferences와 같다).
inline constexpr uint32 PackageMaxObjectImports = 65536;

// 패키지 안 문자열(export·import 이름, 프로파일 id) 하나의 byte 상한이다.
inline constexpr uint32 PackageMaxStringBytes = 4096;

// bulkAlignment 범위(2의 거듭제곱)와 기본값이다(에디터 64, 쿠킹은 프로파일 값 — 기본 4096).
inline constexpr uint32 PackageMinBulkAlignment = 8;
inline constexpr uint32 PackageMaxBulkAlignment = 65536;
inline constexpr uint32 PackageEditorBulkAlignment = 64;
inline constexpr uint32 PackageDefaultCookedBulkAlignment = 4096;

// 비-Bulk 섹션과 섹션 표의 정렬이다.
inline constexpr uint32 PackageSectionAlignment = 8;

// 헤더 flags다. 정확히 하나만 켠다(AS 5.1절 검증 1).
enum class PackageKind : uint32 {
    Editor = 1,
    Cooked = 2
};

// 섹션 종류다. 값이 물리 순서다(AS 5.1절 — kind 오름차순 고정).
enum class PackageSectionKind : uint32 {
    Summary = 1,
    Exports = 2,
    ObjectImports = 3,
    Dependencies = 4,
    Objects = 5,
    ImportRecord = 6,
    Tags = 7,
    Bulk = 8,
    BulkIndex = 9
};

// 알려진 섹션 종류의 수다(1..이 값).
inline constexpr uint32 PackageKnownSectionKinds = 9;

// 섹션 표 flags 비트다. 정의되지 않은 비트는 CorruptPackage다.
enum class PackageSectionFlags : uint32 {
    None = 0,
    // 이 섹션을 모르는 reader는 패키지를 해석할 수 없다.
    Required = 1,
    // 섹션 표의 crc32 칸은 0이고 entry별 CRC로 검증한다(Bulk).
    PerEntryChecksum = 2
};

inline constexpr uint32 PackageSectionFlagsMask = 3;

// 이 엔진이 쓰고 읽는 섹션 schemaVersion이다(모든 알려진 섹션이 1 — AS 5.3절 섹션 축).
inline constexpr uint32 PackageSectionSchemaVersion = 1;

// Exports 항목 flags다.
enum class PackageExportFlags : uint32 {
    None = 0,
    // 대표 에셋(export 1)만 가진다.
    Primary = 1,
    // 객체 단위 EditorOnly(쿠킹 패키지에 없다). v1은 이 비트를 쓰지 않는다 — 객체를 EditorOnly로
    // 표시하는 클래스 지정자가 아직 없어 writer는 0을 쓰고 reader는 켜진 비트를 CorruptPackage로
    // 거부한다(구현에서 정한 점, AS 5.2절).
    EditorOnly = 2
};

// Dependencies 항목 종류다(AS 6.2절). 정렬 키 (assetId, kind)의 kind 순서도 이 값이다.
enum class PackageDependencyKind : uint32 {
    Hard = 1,
    Soft = 2,
    EditorOnly = 3,
    Build = 4,
    ImportOwner = 5
};

// bulk entry 저장 codec이다(AS 5.2절 BulkIndex). Zstd entry의 저장 byte는 Zstd 블록
// 컨테이너(RCBK — Compression/CompressedBlocks.h)이고 블록마다 압축이 줄이지 못하면 그 블록은 raw로
// 담긴다. 0 byte entry는 언제나 None이다.
enum class BulkCodec : uint32 {
    None = 0,
    Zstd = 1
};

// writer가 쓰는 Zstd bulk 블록 크기(log2 — 256 KiB)다. 압축 메모리 피크가 블록 하나다(AS 5.5절 3).
// 블록 크기는 컨테이너 헤더에 기록되므로 reader는 이 값에 기대지 않는다(바꾸면 쿠킹 byte가 바뀐다).
inline constexpr uint32 PackageBulkBlockSizeLog2 = 18;

// BulkIndex 항목 flags다.
enum class BulkEntryFlags : uint32 {
    None = 0,
    // 모든 참조 위치가 EditorOnly 필드다(쿠킹 패키지에서는 0이어야 한다).
    EditorOnly = 1
};

// 헤더 값이다(검증을 지난 것 — PackageReader가 만든다).
struct PackageHeader {
    uint16 formatVersion = PackageFormatVersion;
    PackageKind kind = PackageKind::Cooked;
    uint32 sectionCount = 0;
    uint64 sectionTableOffset = 0;
    uint64 fileBytes = 0;
    AssetId assetId{};
    uint32 bulkAlignment = PackageDefaultCookedBulkAlignment;
};

// 섹션 표 항목 하나다.
struct PackageSectionEntry {
    PackageSectionKind kind = PackageSectionKind::Summary;
    uint32 flags = 0;
    uint64 offset = 0;
    uint64 bytes = 0;
    uint32 crc32 = 0;
    uint32 schemaVersion = PackageSectionSchemaVersion;
};

// 섹션 종류별로 고정된 표 flags다(AS 5.2절 표). 모르는 종류는 0이다.
constexpr uint32 PackageSectionRequiredFlags(PackageSectionKind kind) noexcept {
    switch (kind) {
    case PackageSectionKind::Summary:
    case PackageSectionKind::Exports:
    case PackageSectionKind::ObjectImports:
    case PackageSectionKind::Dependencies:
    case PackageSectionKind::Objects:
    case PackageSectionKind::BulkIndex:
        return static_cast<uint32>(PackageSectionFlags::Required);
    case PackageSectionKind::Bulk:
        return static_cast<uint32>(PackageSectionFlags::Required) |
               static_cast<uint32>(PackageSectionFlags::PerEntryChecksum);
    case PackageSectionKind::ImportRecord:
    case PackageSectionKind::Tags:
        return 0;
    }

    return 0;
}

// kind가 알려진 섹션 종류인지다.
constexpr bool IsKnownPackageSection(uint32 kind) noexcept {
    return kind >= 1 && kind <= PackageKnownSectionKinds;
}

// 압축을 푼 bulk entry 하나의 위치다(raw byte 버퍼 안 offset — Package가 소유한 버퍼,
// DecodePackageBulk).
struct PackageBulkSlot {
    uint64 offset = 0;
    uint64 rawBytes = 0;
    Sha256Digest sha256{};
};

// 패키지 지문이다(AS 5.5절 — 헤더 + 섹션 표 byte의 SHA-256). 저장의 외부 변경 검출과 AssetIndex
// 캐시가 쓴다. 섹션 CRC가 표에 있으므로 표의 SHA-256이 내용 변화를 대표한다(CRC32 충돌 수준).
struct PackageFingerprint {
    Sha256Digest digest{};

    friend constexpr bool operator==(const PackageFingerprint&,
                                     const PackageFingerprint&) noexcept = default;
};

} // namespace rocks
