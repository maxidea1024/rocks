#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/AssetDiagnostic.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Time/DateTime.h>
#include <Object/AssetId.h>
#include <Platform/File.h>

// 경로 별칭 표다(Docs/Design/AssetSystem.ko.md 4.3절, 7.1절 17 A2e1 — UE ObjectRedirector
// 대응이지만 패키지가 아니라 사용자 데이터 파일). 이동·이름 변경이 옛 논리 경로 → AssetId를 남겨
// 설정·스크립트· 명령의 텍스트 경로가 계속 풀리게 한다(참조는 AssetId라 별칭이 필요 없다). 파일은
// `Config/AssetAliases.json`(버전 관리 대상, 도구가 다시 쓰는 엄격 JSON — 항목은 경로 키 순).
//
// 사용 예:
//   AssetAliasTable aliases(allocator, layout);
//   if (auto loaded = aliases.Load(files, &failure); !loaded) { ... }
//   const usize at = aliases.Find(path.Key());
//   if (at != AssetAliasNone) { id = aliases.At(at).asset; }
//
// 스레드: 한 스레드에서 쓴다.
namespace rocks {

// 별칭 표 파일 이름(Config 아래)·형식 버전이다.
inline constexpr StringView AssetAliasFileName = "AssetAliases.json";
inline constexpr uint32 AssetAliasFormatVersion = 1;
// 만료 일수의 기본값·범위다.
inline constexpr uint32 AssetAliasDefaultDays = 90;
inline constexpr uint32 AssetAliasMaxDays = 36500;
// 항목 수·파일 크기 상한이다(신뢰하지 않는 입력 — 작은 실수가 큰 할당을 부르지 않게).
inline constexpr usize MaxAssetAliases = usize{1} << 20;
inline constexpr uint64 MaxAssetAliasFileBytes = uint64{64} << 20;
// Find가 찾지 못한 결과다.
inline constexpr usize AssetAliasNone = NumericLimits<usize>::Max();
// 진단 detail의 "항목이 아님"(헤더 칸 위반)이다.
inline constexpr uint64 AssetAliasNoEntry = NumericLimits<uint64>::Max();

// 별칭 표 위반 칸이다(진단 section).
enum class AssetAliasField : uint32 {
    // 파일 크기 상한 초과.
    File,
    // JSON 문법(detail = (줄 << 32) | 열).
    Json,
    // 최상위가 객체가 아니거나 모르는 키·빠진 키.
    Root,
    Version,
    Aliases,
    // 항목이 객체가 아니거나 모르는 키·빠진 키.
    Entry,
    Path,
    Asset,
    Created,
    Days,
    // 같은 경로 키가 이미 있다.
    DuplicatePath
};

// 별칭 하나다.
struct AssetAlias {
    explicit AssetAlias(Allocator& allocator) noexcept : path(allocator), key(allocator) {
    }

    AssetAlias(AssetAlias&&) noexcept = default;
    AssetAlias& operator=(AssetAlias&&) noexcept = default;

    // 옛 논리 경로(표기 그대로)와 비교 키다.
    String path;
    String key;
    AssetId asset{};
    // 만든 시각(UTC)과 만료 일수다.
    DateTime created{};
    uint32 days = AssetAliasDefaultDays;
};

// 더하거나 바꿀 별칭이다(Write — 같은 경로 키가 있으면 바꾼다).
struct AssetAliasInput {
    StringView path;
    AssetId asset{};
    DateTime created{};
    uint32 days = AssetAliasDefaultDays;
};

// prune의 텍스트 검색 상한이다(검색 대상이 크면 멈추고 아무것도 지우지 않는다).
struct AssetAliasSearchLimits {
    usize maxFiles = 65536;
    uint64 maxTotalBytes = uint64{1} << 30;
    uint64 maxFileBytes = uint64{64} << 20;
};

// prune이 찾은 쓰인 곳 하나다(만료 항목이 그 텍스트에 나온다).
struct AssetAliasUse {
    explicit AssetAliasUse(Allocator& allocator) noexcept : alias(allocator), file(allocator) {
    }

    AssetAliasUse(AssetAliasUse&&) noexcept = default;
    AssetAliasUse& operator=(AssetAliasUse&&) noexcept = default;

    // 그 항목의 옛 경로와 프로젝트 루트 상대 파일 경로(표시 이름 — 제어 문자 escape)·줄(1부터)이다.
    String alias;
    String file;
    uint64 line = 0;
};

// prune 결과다.
struct AssetAliasPruneReport {
    explicit AssetAliasPruneReport(Allocator& allocator) noexcept : uses(allocator) {
    }

    // 지운 항목 수, 만료됐지만 쓰여서 남긴 항목 수, 만료되지 않아 남긴 항목 수다.
    usize removed = 0;
    usize inUse = 0;
    usize kept = 0;
    // 검색 상한에 닿아 멈췄다(아무것도 지우지 않았다).
    bool truncated = false;
    // 교체는 됐고 디렉터리 동기화를 확인하지 못했다.
    bool notDurable = false;
    // 쓰인 곳이다(파일 순회 순서).
    Array<AssetAliasUse> uses;
};

class AssetAliasTable {
public:
    // layout은 빌린다(표보다 오래 산다). maxEntries는 항목 수 상한이다(읽기·쓰기 — 시험이 줄여
    // 경계를 본다, 기본 MaxAssetAliases).
    AssetAliasTable(Allocator& allocator, const ProjectLayout& layout,
                    usize maxEntries = MaxAssetAliases) noexcept;

    AssetAliasTable(const AssetAliasTable&) = delete;
    AssetAliasTable& operator=(const AssetAliasTable&) = delete;

    // 파일을 읽는다(없으면 빈 표 — 다시 부르면 다시 읽는다). 검사: version 1, 최상위·항목 키
    // 정확히(모르는 키·빠진 키 없음), path는 논리 경로 규칙, asset은 36자 소문자 non-null,
    // created는 ISO 8601, days 1–AssetAliasMaxDays, 경로 키 중복 없음, 항목 수·파일 크기 상한.
    // 정렬이 어긋난 파일은 받는다(다음 쓰기가 정렬한다). 실패하면 표는 비고 읽지 않은 상태다.
    // 오류(failure 진단): 위반 InvalidArgument(코드 CorruptAliasTable, section = AssetAliasField,
    // detail = 항목 번호 또는 AssetAliasNoEntry — JSON 문법은 줄·열), 파일 크기 상한
    // CapacityExceeded(같은 진단, 칸 File), 읽기 IO(진단 None과 그 오류), 할당 실패.
    Result<void> Load(platform::FileSystem& files, AssetDiagnostic* failure = nullptr) noexcept;

    bool IsLoaded() const noexcept {
        return loaded_;
    }

    // 항목 수와 index번째 항목(경로 키 byte 순)이다.
    usize Count() const noexcept {
        return entries_.Size();
    }

    const AssetAlias& At(usize index) const noexcept {
        return entries_[index];
    }

    // 경로 키의 항목 위치다(없으면 AssetAliasNone).
    usize Find(StringView key) const noexcept;

    // 표에서 removeKeys를 빼고 add를 더하거나 바꾼(같은 키) 새 표를 원자 교체로 쓰고(빈 표도 쓴다),
    // 성공하면 메모리 표를 바꾼다. 쓰기 전 파일 상태를 기억해 Restore가 되돌린다. 쓰기 직전에
    // 파일을 다시 읽어 마지막 Load·Write 때와 다르면(병합·손 편집 — 있음·없음 포함) 쓰지 않는다
    // (ExternalModification / StaleHandle — 다시 Load한 뒤 다시 한다, 패키지 저장 5.5절 1과 같은
    // 원칙). 읽지 않은 표(Load 전)는 InvalidArgument. 오류(failure 진단): 잠금이 아니면
    // InvalidArgument, 외부 변경 ExternalModification / StaleHandle, 다시 읽기 IO(진단 None), add의
    // 경로 규칙 위반 InvalidPath / InvalidArgument, add의 null id·days 범위 밖 InvalidArgument,
    // 항목 수 상한 CapacityExceeded, 쓰기 IO(진단 None과 그 오류 — 파일·메모리 표 그대로), 할당
    // 실패. notDurable은 교체 뒤 동기화를 확인하지 못했는지다.
    Result<void> Write(platform::FileSystem& files, const AssetDatabaseLock& lock,
                       Span<const StringView> removeKeys, Span<const AssetAliasInput> add,
                       bool& notDurable, AssetDiagnostic* failure = nullptr) noexcept;

    // 마지막 Write 전 상태(파일 byte·없음, 메모리 표)로 되돌린다 — 이동·삭제의 OS 단계가 실패했을
    // 때 쓴다. 되돌릴 것이 없으면 아무것도 하지 않는다. 마지막 Write 뒤 파일이 바뀌었으면 되돌리지
    // 않는다(StaleHandle). 오류: 잠금이 아니면 InvalidArgument, 외부 변경 StaleHandle, 다시 읽기·
    // 쓰기·지우기 IO(그때 파일은 마지막 Write의 것이다), 할당 실패.
    Result<void> Restore(platform::FileSystem& files, const AssetDatabaseLock& lock) noexcept;

    // 만료 항목(created + days ≤ nowTicks)을 고르고 텍스트 참조 검색(Config 아래 일반 파일 —
    // 링크·별칭 표 자신·임시 파일 제외, limits)으로 쓰인 곳을 찾아, 쓰이지 않은 만료 항목만 지운다
    // (Write). 대조는 경로 텍스트의 ASCII 대소문자 무시 일치이고 뒤가 구성 요소
    // 경계(`.`·`"`·`'`·공백·줄끝·`,`·`)`·`]`·파일 끝)여야 한다. 검색이 상한에 닿으면 아무것도
    // 지우지 않는다(report.truncated). 오류: 읽지 않은 표 InvalidArgument, Write의 오류, Config
    // 열거·파일 읽기 IO(진단 None과 그 오류 — 표 그대로), 할당 실패.
    Result<void> Prune(platform::FileSystem& files, const AssetDatabaseLock& lock, int64 nowTicks,
                       const AssetAliasSearchLimits& limits, AssetAliasPruneReport& report,
                       AssetDiagnostic* failure = nullptr) noexcept;

private:
    // 파일이 마지막 Load·Write 때(currentBytes_·currentExisted_)와 같은지다.
    Result<bool> Unchanged(platform::FileSystem& files) const noexcept;
    Result<void> EncodeTo(const Array<AssetAlias>& entries, Array<uint8>& out) const noexcept;

    Allocator* allocator_;
    const ProjectLayout* layout_;
    usize maxEntries_;
    Array<AssetAlias> entries_;
    // 지금 파일 byte(Load가 읽었거나 Write가 쓴 것)와 파일이 있는지다.
    Array<uint8> currentBytes_;
    bool currentExisted_ = false;
    // 마지막 Write 전의 파일 byte(있었으면)와 메모리 표다(Restore).
    Array<uint8> previousBytes_;
    Array<AssetAlias> previousEntries_;
    bool previousExisted_ = false;
    bool restorable_ = false;
    bool loaded_ = false;
};

// 별칭이 만료되는 시각(tick)이다(created + days 일 — 넘치면 최댓값).
int64 AssetAliasExpiryTicks(const AssetAlias& alias) noexcept;

} // namespace rocks
