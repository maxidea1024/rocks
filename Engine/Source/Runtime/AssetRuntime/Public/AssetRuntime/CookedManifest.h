#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/PackageFormat.h>
#include <AssetRuntime/PackageReader.h>
#include <Base/NumericLimits.h>
#include <Core/Build/EngineVersion.h>
#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// 쿠킹 매니페스트 v1(`CookedManifest.rman`)이다(Docs/Design/AssetSystem 10절, 10.1절 A1b).
// 쿠커(A4)가 모델을 만들어 WriteCookedManifest로 쓰고, 로더(A1c)가 ReadCookedManifest로 읽어
// 패키지를 찾고(Find) 연 패키지를 CheckCookedPackage로 대조한다. 서명 봉투(ContentSigning)는 검증한
// 같은 버퍼의 매니페스트 byte를 reader에 넘긴다.
namespace rocks {

// 매니페스트 magic이다(파일 앞 4 byte).
inline constexpr uint8 CookedManifestMagic[4] = {'R', 'M', 'A', 'N'};

// 이 reader·writer가 쓰는 형식 버전이다(다른 값은 UnsupportedVersion).
inline constexpr uint32 CookedManifestFormatVersion = 1;

// 파일 byte 상한이다.
inline constexpr uint64 CookedManifestMaxFileBytes = uint64{1} * 1024 * 1024 * 1024;

// 항목 수 상한이다.
inline constexpr uint32 CookedManifestMaxEntries = uint32{1} << 20;

// 항목당 bundle 수 상한이다.
inline constexpr uint32 CookedManifestMaxBundles = 256;

// bundle 하나의 구성원 수 상한이다(의존 상한과 같다 — 구성원은 그 항목의 의존 대상이다).
inline constexpr uint32 CookedManifestMaxBundleMembers = 65536;

// 항목당 런타임 의존 수 상한이다.
inline constexpr uint32 CookedManifestMaxDependencies = 65536;

// 경로 힌트(논리 경로 — AS 4.2절) byte 상한이다.
inline constexpr uint32 CookedManifestMaxPathBytes = 256;

// 항목 flags 비트다.
enum class CookedManifestFlags : uint32 {
    None = 0,
    // CookSettings PrimaryAsset 규칙이 고른 에셋이다(bundle을 가질 수 있다).
    PrimaryAsset = 1u << 0
};

// 정의된 flags 비트 전체다.
inline constexpr uint32 CookedManifestFlagsMask = 1;

// 진단의 위반 칸이다(AssetDiagnostic::section에 둔다 — AS 10.1절 2). 패키지 진단의 섹션 번호와
// 겹치지 않게 100부터다.
enum class CookedManifestField : uint32 {
    File = 100,
    Magic,
    FormatVersion,
    Crc,
    EntryCount,
    ProfileId,
    ProfileSettings,
    CookSettings,
    CookerVersion,
    EngineVersion,
    AssetId,
    TypeId,
    PathHint,
    PackageSha256,
    PackageBytes,
    BulkRawBytes,
    CookKey,
    Flags,
    BundleCount,
    Bundle,
    DependencyCount,
    Dependency,
    // 패키지 대조(CheckCookedPackage) 전용 칸이다.
    PackageKind,
    PackageProfile,
    PackageDependencies,
    // bundle 구성원 개수 칸과 구성원(AS 10.1절 5 — A1d 개정)이다.
    BundleMemberCount,
    BundleMember
};

// 헤더 칸 진단의 detail 값이다(항목 번호가 없다).
inline constexpr uint64 CookedManifestNoEntry = NumericLimits<uint64>::Max();

// 런타임 의존 하나다(kind는 Hard·Soft만 — 패키지 PackageDependencyKind와 같은 값).
struct CookedManifestDependency {
    AssetId asset{};
    PackageDependencyKind kind = PackageDependencyKind::Hard;
};

// PrimaryAsset bundle 하나다(AS 10.1절 5 — UE FAssetBundleData의 bundle별 에셋 목록 대응). 그
// bundle을 로드할 때 대표 에셋과 함께 로드할 구성원이고, 구성원은 그 항목의 런타임 의존 대상이다.
struct CookedManifestBundle {
    explicit CookedManifestBundle(Allocator& allocator) noexcept
        : name(allocator), members(allocator) {
    }

    CookedManifestBundle(CookedManifestBundle&&) noexcept = default;
    CookedManifestBundle& operator=(CookedManifestBundle&&) noexcept = default;
    CookedManifestBundle(const CookedManifestBundle&) = delete;
    CookedManifestBundle& operator=(const CookedManifestBundle&) = delete;

    String name;
    // AssetId 엄격 오름차순이다.
    Array<AssetId> members;
};

// 쿠킹한 패키지 하나의 항목이다.
struct CookedManifestEntry {
    explicit CookedManifestEntry(Allocator& allocator) noexcept
        : pathHint(allocator), bundles(allocator), dependencies(allocator) {
    }

    CookedManifestEntry(CookedManifestEntry&&) noexcept = default;
    CookedManifestEntry& operator=(CookedManifestEntry&&) noexcept = default;
    CookedManifestEntry(const CookedManifestEntry&) = delete;
    CookedManifestEntry& operator=(const CookedManifestEntry&) = delete;

    AssetId asset{};
    // 대표 에셋(export 1)의 타입이다.
    TypeId type{};
    // 논리 경로 힌트다(AS 4.2절 — 영속 참조는 언제나 AssetId).
    String pathHint;
    Sha256Digest packageSha256{};
    uint64 packageBytes = 0;
    // 패키지 BulkIndex rawBytes 합이다(로더의 PackageLoadOptions::maxBulkRawBytes — AS 5.7절 11).
    uint64 bulkRawBytes = 0;
    Sha256Digest cookKey{};
    // CookedManifestFlags 비트 합이다.
    uint32 flags = 0;
    // PrimaryAsset bundle이다(이름 byte 엄격 오름차순).
    Array<CookedManifestBundle> bundles;
    // (asset, kind) 엄격 오름차순이다.
    Array<CookedManifestDependency> dependencies;
};

// 매니페스트 모델이다. entries는 AssetId 엄격 오름차순이다.
struct ROCKS_ASSETRUNTIME_API CookedManifest {
    explicit CookedManifest(Allocator& allocator) noexcept
        : profileId(allocator), entries(allocator) {
    }

    CookedManifest(CookedManifest&&) noexcept = default;
    CookedManifest& operator=(CookedManifest&&) noexcept = default;
    CookedManifest(const CookedManifest&) = delete;
    CookedManifest& operator=(const CookedManifest&) = delete;

    String profileId;
    Sha256Digest profileSettings{};
    Sha256Digest cookSettings{};
    uint32 cookerVersion = 0;
    // 진단용이다(병합·로드 판정에 쓰지 않는다 — AS 10절).
    EngineVersion cookedBy{};
    Array<CookedManifestEntry> entries;

    // asset의 항목이다(이진 탐색 — entries가 정렬되어 있어야 한다). 없으면 null이다. 반환 포인터는
    // entries가 바뀌기 전까지 유효하다.
    const CookedManifestEntry* Find(AssetId asset) const noexcept;
};

// 모델을 AS 10.1절 규칙(헤더·항목 칸·항목 사이 규칙)으로 검증한다. scratch는 경로 힌트·bundle 비교
// 키의 임시 저장소다(반환 전에 돌려준다).
//
// 오류: 위반은 CorruptPackage / InvalidArgument(section = CookedManifestField, detail = 항목 번호
// 또는 CookedManifestNoEntry, asset·related), scratch 할당 실패는 진단 None과 그 오류다.
ROCKS_ASSETRUNTIME_API Result<void>
ValidateCookedManifest(const CookedManifest& manifest, Allocator& scratch,
                       AssetDiagnostic* failure = nullptr) noexcept;

// 모델을 검증한 뒤 out을 그 매니페스트 byte로 바꾼다(AS 10.1절 1 — 같은 모델이면 같은 byte). 게시
// (임시 파일·Flush·Replace)는 호출자(쿠커) 몫이다.
//
// 오류: ValidateCookedManifest의 위반은 InvalidArgument(진단은 같다 — 쓰지 않는다), 파일이
// CookedManifestMaxFileBytes를 넘으면 CapacityExceeded, 할당 실패는 그 오류다. 실패하면 out은
// 그대로다.
//
// 사용 예:
//   Array<uint8> bytes(allocator);
//   if (auto written = WriteCookedManifest(allocator, manifest, bytes, &failure); !written) { ... }
ROCKS_ASSETRUNTIME_API Result<void>
WriteCookedManifest(Allocator& allocator, const CookedManifest& manifest, Array<uint8>& out,
                    AssetDiagnostic* failure = nullptr) noexcept;

// 매니페스트 byte를 검증하고 모델로 푼다(AS 10.1절 2의 순서). 결과는 bytes를 빌리지 않는다.
//
// 오류: formatVersion이 1이 아니면 UnsupportedVersion / Unsupported, 그 밖의 위반(magic·CRC·칸·항목
// 사이 규칙·남는 byte)은 CorruptPackage / InvalidArgument, 할당 실패는 진단 None과 그 오류다.
//
// 사용 예:
//   auto manifest = ReadCookedManifest(allocator, bytes.View(), &failure);
//   const CookedManifestEntry* entry = manifest ? manifest.Value().Find(id) : nullptr;
ROCKS_ASSETRUNTIME_API Result<CookedManifest>
ReadCookedManifest(Allocator& allocator, Span<const uint8> bytes,
                   AssetDiagnostic* failure = nullptr) noexcept;

// 로더 시작 대조다: 매니페스트 profileId가 실행 프로파일과 다르면 IncompatibleCook / Unsupported
// (section = ProfileId)다.
ROCKS_ASSETRUNTIME_API Result<void>
CheckCookedManifestProfile(const CookedManifest& manifest, StringView runtimeProfile,
                           AssetDiagnostic* failure = nullptr) noexcept;

// 부분 쿠킹 병합 대조다: profileId·profileSettings·cookSettings·cookerVersion이 모두 같아야 하고,
// 아니면 IncompatibleCook / Unsupported(section = 처음 다른 칸)다.
ROCKS_ASSETRUNTIME_API Result<void>
CheckCookedManifestMerge(const CookedManifest& existing, const CookedManifest& incoming,
                         AssetDiagnostic* failure = nullptr) noexcept;

// 연 패키지를 항목과 대조한다(AS 10.1절 8): CheckCookedPackageFile(file 길이·SHA-256)을 먼저 하고
// 그다음 CheckCookedPackageContents(contents — file의 ReadPackage 결과)를 한다.
//
// 오류: 다르면 CorruptPackage / InvalidArgument(section = CookedManifestField, detail =
// CookedManifestNoEntry, asset = entry.asset, related = 의존 칸이면 처음 다른 자리의 대상)다.
ROCKS_ASSETRUNTIME_API Result<void> CheckCookedPackage(const CookedManifest& manifest,
                                                       const CookedManifestEntry& entry,
                                                       Span<const uint8> file,
                                                       const PackageContents& contents,
                                                       AssetDiagnostic* failure = nullptr) noexcept;

// 파일 쪽 대조다(AS 11.6절 3 — 로더가 해석 전에 부른다): file 길이 = packageBytes(PackageBytes),
// SHA-256 = packageSha256(PackageSha256). 오류는 CheckCookedPackage와 같다.
Result<void> CheckCookedPackageFile(const CookedManifestEntry& entry, Span<const uint8> file,
                                    AssetDiagnostic* failure = nullptr) noexcept;

// 내용 쪽 대조다(AS 11.6절 3 — 로더가 Prepare 뒤에 부른다): contents의 종류(PackageKind)·
// assetId(AssetId)·대표 에셋 타입(TypeId)·BulkIndex rawBytes 합(BulkRawBytes — 합이 u64를 넘으면
// 불일치)·Summary 프로파일 칸(PackageProfile)·Dependencies(PackageDependencies — (asset, kind) 순서
// 배열이 같아야 한다)를 manifest·entry와 본다. 오류는 CheckCookedPackage와 같다.
Result<void> CheckCookedPackageContents(const CookedManifest& manifest,
                                        const CookedManifestEntry& entry,
                                        const PackageContents& contents,
                                        AssetDiagnostic* failure = nullptr) noexcept;

// 쿠킹 루트 기준 패키지 파일 경로(AS 4.2절 `Packages/<a>/<A>-<h>.rpkg`)를 out 뒤에 붙인다.
//
// 오류: out의 할당 실패는 그 오류다(실패하면 out 길이는 그대로가 아닐 수 있다 — 호출자가 버린다).
ROCKS_ASSETRUNTIME_API Result<void>
AppendCookedPackagePath(StringBuilderBase& out, AssetId asset,
                        const Sha256Digest& packageSha256) noexcept;

} // namespace rocks
