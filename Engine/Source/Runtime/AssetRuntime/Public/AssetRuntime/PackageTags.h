#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>

// 패키지 Tags 섹션의 값 스키마다(Docs/Design/AssetSystem 7절, 7.1절 2·13 — A2b). UE
// FAssetData의 태그처럼 문자열 키 → 문자열 값이고, 목록 값(예: Unreal 교환의 옛 경로 목록)은 같은
// 키의 항목 여러 개다. 패키지 형식 층(PackageWriter·Reader)은 Tags를 "필드 id 오름차순 Record"로만
// 검사하고, 이 스키마(PackageTagsSchema)는 색인·에디터가 읽고 쓴다 — 다른 스키마 Guid의 Tags도
// 형식으로는 유효하다.
//
// 배치: Record(PackageTagsSchema, v1){ 필드 PackageTagsListField = RecordList[
// Record(PackageTagItemSchema, v1){ 키 String, 값 String } ] }. 항목은 (키, 값) UTF-8 byte 순 엄격
// 오름차순(같은 쌍 금지)이다.
//
// 사용 예:
//   const PackageTagInput tags[] = {{"UnrealExchange.FormerPaths", "/Game/Old/A"},
//                                   {"UnrealExchange.FormerPaths", "/Game/Old/B"}};
//   auto bytes = WritePackageTags(allocator, tags);      // PackageWriteOptions::tags
//   auto read = ReadPackageTags(allocator, contents.tags);
namespace rocks {

inline constexpr Guid PackageTagsSchema{0x5250414b54414753ull, 0x0000000000000001ull};
inline constexpr Guid PackageTagItemSchema{0x5250414b54414753ull, 0x0000000000000002ull};
inline constexpr uint32 PackageTagsVersion = 1;
inline constexpr Guid PackageTagsListField{0, 1};
inline constexpr Guid PackageTagKeyField{0, 1};
inline constexpr Guid PackageTagValueField{0, 2};
// 키의 byte 상한이다(값은 패키지 문자열 상한 PackageMaxStringBytes).
inline constexpr usize MaxPackageTagKeyBytes = 256;
// 에셋 bundle 구성원 태그의 키 접두다(`Bundle.<이름>` = 구성원 AssetId 표준 문자열, 구성원마다 태그
// 하나 — Docs/Design/AssetSystem 10.2절 결정 9). 에디터 패키지 저장이 쓰고(AssetDatabase) 쿠커가
// PrimaryAsset의 것만 읽는다(AssetCooker).
inline constexpr StringView AssetBundleTagPrefix = "Bundle.";

// 쓰기 입력 한 쌍이다(호출 동안만 빌린다).
struct PackageTagInput {
    StringView key;
    StringView value;
};

// 읽은 한 쌍이다(소유).
struct PackageTag {
    explicit PackageTag(Allocator& allocator) noexcept : key(allocator), value(allocator) {
    }

    String key;
    String value;
};

// tags를 (키, 값) 순으로 정렬해 Tags Record byte로 쓴다. 비어 있으면 빈 byte(Tags 섹션 없음)다.
// 오류: 빈 키·키 256 byte 초과·값이 패키지 문자열 상한 초과·잘못된 UTF-8·같은 (키, 값) 쌍 두 번은
// InvalidArgument, 목록 전체가 Record 기본 한도(값 하나 RecordLimits::maxValueBytes)를 넘으면
// CapacityExceeded(ReadPackageTags가 같은 기본 한도로 읽으므로 쓴 것은 모두 읽힌다), 할당 실패.
ROCKS_ASSETRUNTIME_API Result<Array<uint8>>
WritePackageTags(Allocator& allocator, Span<const PackageTagInput> tags) noexcept;

// Tags Record byte를 읽는다. 빈 byte는 빈 목록이다. 오류: 스키마 Guid가 PackageTagsSchema가
// 아니거나 Record·항목 버전이 PackageTagsVersion 밖이면 Unsupported(다른 스키마·더 새 Tags),
// 형식·순서·크기 위반은 InvalidArgument, 할당 실패.
ROCKS_ASSETRUNTIME_API Result<Array<PackageTag>> ReadPackageTags(Allocator& allocator,
                                                                 Span<const uint8> bytes) noexcept;

} // namespace rocks
