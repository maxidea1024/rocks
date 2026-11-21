#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Build/EngineVersion.h>
#include <Core/Encoding/Sha256.h>
#include <Platform/File.h>

// 색인 캐시다(Docs/Design/AssetSystem.ko.md 7절 캐시, 7.1절 16 A2d1).
// `Intermediate/AssetIndex.cache` 하나에 진단 없이 색인된 파일의 항목과 지문을 둔다. 다음 검색은
// 파일의 범위 읽기 지문이 같으면 그 항목을 다시 쓰고(부분 읽기 생략), 다르면 다시 읽는다. 캐시는
// 정확성에 필요 없다 — 키가 다르거나 손상되었으면 없는 것과 같다. 쓰기는 쓰기 잠금을 쥔 프로세스만
// 한다(원자 교체).
//
// 사용 예(목록 명령 — 읽기만):
//   AssetIndexCache cache(allocator);
//   auto loaded = cache.Load(files, layout, options);        // 없거나 맞지 않으면 false
//   if (!loaded) { return loaded; }                           // 할당 실패만 오류
//   AssetIndex index(allocator, layout);
//   index.Rebuild(files, options, loaded.Value() ? &cache : nullptr);
// 사용 예(쓰기 명령 끝):
//   auto written = WriteAssetIndexCache(files, layout, lock, index, options);
namespace rocks {

// 캐시 파일 이름이다(Intermediate 아래).
inline constexpr StringView AssetIndexCacheFileName = "AssetIndex.cache";
// 형식 버전(헤더·본문 배치)이다.
inline constexpr uint32 AssetIndexCacheFormatVersion = 1;
// 항목 스키마 버전(본문 항목의 칸)이다. AssetIndexEntry·요약 칸이 바뀌면 올린다.
inline constexpr uint32 AssetIndexCacheSchemaVersion = 1;
// 판정 의미 버전이다. 색인 판정 규칙(ReadPackageIndex·ImportRecord·Tags 파서·ImportOwner 대조·
// 논리 경로 규칙)을 바꾸면 올린다 — 도구 빌드 ID는 키가 아니므로(7.1절 16).
inline constexpr uint32 AssetIndexCacheSemanticVersion = 1;
// 캐시 파일 크기 상한이다(신뢰하지 않는 입력의 경계 — 넘으면 읽지 않는다).
inline constexpr uint64 MaxAssetIndexCacheBytes = 1ull << 30;

// 캐시 키다(하나라도 다르면 캐시 없음과 같다). 도구 빌드 ID는 키가 아니다(진단용).
struct AssetIndexCacheKey {
    uint32 formatVersion = AssetIndexCacheFormatVersion;
    uint32 schemaVersion = AssetIndexCacheSchemaVersion;
    uint32 semanticVersion = AssetIndexCacheSemanticVersion;
    // NFC·caseless 키 표의 Unicode 버전 텍스트의 SHA-256이다.
    Sha256Digest unicode{};
    // 호환 엔진 버전(주·부 — NewerVersion 판정이 보는 것)이다. 엔진 축을 끈 실행(빈 엔진 버전)은
    // 정의상 주·부가 0이라(EngineVersion::IsEmpty) 켠 실행과 키가 다르다 — 따로 칸을 두지 않는다.
    uint16 compatibleMajor = 0;
    uint16 compatibleMinor = 0;
    // 읽기 옵션 customVersions 목록(id·버전)의 SHA-256이다.
    Sha256Digest customVersions{};
    // 이름 정규화 파일 시스템인지다(FileSystemNormalizesNames — 비 NFC 이름 판정이 기댄다).
    bool normalizesNames = FileSystemNormalizesNames;

    friend bool operator==(const AssetIndexCacheKey&, const AssetIndexCacheKey&) noexcept = default;
};

// options(색인 읽기 옵션)의 캐시 키다.
AssetIndexCacheKey MakeAssetIndexCacheKey(const PackageReadOptions& options) noexcept;

// 캐시 항목이다(진단 없이 색인된 파일 하나).
struct AssetIndexCacheEntry {
    explicit AssetIndexCacheEntry(Allocator& allocator) noexcept
        : fileName(allocator), dependencies(allocator), tags(allocator), importRecord(allocator) {
    }

    AssetIndexCacheEntry(AssetIndexCacheEntry&&) noexcept = default;
    AssetIndexCacheEntry& operator=(AssetIndexCacheEntry&&) noexcept = default;

    // Content 상대 OS 이름(색인 항목의 fileName과 같은 철자)이다.
    String fileName;
    PackageFingerprint fingerprint{};
    AssetId id{};
    TypeId type{};
    bool readOnly = false;
    Array<PackageDependencyEntry> dependencies;
    Array<PackageTag> tags;
    AssetIndexImportSummary importRecord;
};

class AssetIndexCache {
public:
    explicit AssetIndexCache(Allocator& allocator) noexcept;

    AssetIndexCache(const AssetIndexCache&) = delete;
    AssetIndexCache& operator=(const AssetIndexCache&) = delete;

    // layout의 캐시 파일을 읽어 options의 키와 맞으면 적재한다(true). 파일이 없거나 열 수 없거나
    // 키가 다르거나 형식·SHA-256·경계·의미 검사에 어긋나면 비운 채 false다(캐시 없음 — 오류
    // 아님). 오류: 할당 실패.
    Result<bool> Load(platform::FileSystem& files, const ProjectLayout& layout,
                      const PackageReadOptions& options) noexcept;

    // bytes(캐시 파일 내용)를 key로 검사·적재한다. 결과는 Load와 같다(시험·손상 판정의 단일
    // 지점). 오류: 할당 실패.
    Result<bool> Parse(Span<const uint8> bytes, const AssetIndexCacheKey& key) noexcept;

    // fileName·fingerprint가 같은 항목이다(없으면 null).
    const AssetIndexCacheEntry* Find(StringView fileName,
                                     const PackageFingerprint& fingerprint) const noexcept;

    usize Count() const noexcept {
        return entries_.Size();
    }

    const AssetIndexCacheEntry& At(usize index) const noexcept {
        return entries_[index];
    }

    // 적재한 캐시의 도구 빌드 ID(진단용)다.
    const Sha256Digest& ToolBuildId() const noexcept {
        return toolBuildId_;
    }

private:
    Allocator* allocator_;
    // 파일 이름 byte 순(엄격 증가 — Find 이진 검색)이다.
    Array<AssetIndexCacheEntry> entries_;
    Sha256Digest toolBuildId_{};
};

// index의 항목 가운데 파일 자신의 진단(같은 id·경로 진단 밖)이 없는 것을 key의 캐시 byte로 out에
// 쓴다(파일 이름 순). 오류: 할당 실패.
Result<void> EncodeAssetIndexCache(const AssetIndex& index, const AssetIndexCacheKey& key,
                                   Array<uint8>& out) noexcept;

// index를 layout의 캐시 파일로 쓴다: `Intermediate/AssetIndex.cache.<guid>.tmp`에 쓰고
// Flush·Close한 뒤 원자 교체한다(NotDurable은 성공 — 캐시). lock은 이 프로젝트의 잡은 쓰기
// 잠금이어야 한다. 오류: 잠금이 아니면 InvalidArgument, 디렉터리 만들기·쓰기·교체 IO(임시 파일은
// 지운다), 임시 이름 발급 실패, 할당 실패.
Result<void> WriteAssetIndexCache(platform::FileSystem& files, const ProjectLayout& layout,
                                  const AssetDatabaseLock& lock, const AssetIndex& index,
                                  const PackageReadOptions& options) noexcept;

} // namespace rocks
