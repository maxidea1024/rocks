#pragma once

#include <AssetCooker/CookRootLock.h>
#include <AssetCooker/CookRun.h>
#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/ProjectLayout.h>
#include <AssetRuntime/CookedManifest.h>
#include <Core/Text/StringBuilder.h>

// 쿠킹 게시와 재사용 판정이다(Docs/Design/AssetSystem 10절 1–3, 10.2절 결정 8·10, "A4d 구현
// 세부" — A4d1). 게시본은 `<Saved>/Cooked/<프로파일>/`의 `CookedManifest.rman`과 그것이 가리키는
// `Packages/<a>/<A>-<h>.rpkg`(이름에 내용 SHA-256)다. 패키지는 `.staging`에 쓴 뒤 덮어쓰지 않고
// 옮기고, 매니페스트는 마지막에 원자 교체한다 — 도중에 실패해도 게시본은 그대로다.
//
// 사용 예:
//   CookPublisher publisher(allocator, config);
//   if (auto begun = publisher.Begin(cookLock); !begun) { ... }
//   request.output = &publisher;                 // CookPackages가 Put을 부른다
//   publisher.Reuse(existingEntry, target, path); // 재사용 대상
//   if (!errors) { publisher.Finish(partial ? &existing : nullptr, targets.removed.View()); }
namespace rocks {

inline constexpr StringView CookedDirectoryName = "Cooked";
inline constexpr StringView CookStagingDirectoryName = ".staging";
inline constexpr StringView CookPackagesDirectoryName = "Packages";
// 비에셋 산출물(셰이더 라이브러리) 디렉터리다(AssetSystem 10.3절 4항 — AppendCookedArtifactPath).
inline constexpr StringView CookShadersDirectoryName = "Shaders";
// 패키지 파일 대조를 흘려 읽는 조각 크기다.
inline constexpr usize CookHashChunkBytes = 64 * 1024;

// 쿠킹 루트(`<savedDirectory>/Cooked/<profileId>` — 프로젝트는 layout.SavedDirectory(), 엔진 전용은
// `Engine/Saved`)를 out 뒤에 붙인다. 오류: 할당 실패.
ROCKS_ASSETCOOKER_API Result<void>
AppendCookedRoot(StringBuilderBase& out, StringView savedDirectory, StringView profileId) noexcept;

struct CookPublishConfig {
    platform::FileSystem* files = nullptr;
    // Saved 디렉터리다(쿠킹 루트 = `<savedDirectory>/Cooked/<profileId>` — 10.3.1절 A4f4 2번).
    StringView savedDirectory{};
    // 매니페스트 머리 값이다(프로파일 id·출력 설정 SHA-256·CookSettings SHA-256·쿠커 버전·쿠킹한
    // 엔진 버전 — 엔진 버전은 진단용).
    StringView profileId{};
    Sha256Digest profileSettings{};
    Sha256Digest cookSettings{};
    uint32 cookerVersion = CookerVersion;
    EngineVersion cookedBy{};
    // 매니페스트 backend 표다(TargetProfiles MakeCookedManifestBackends — 10.3.1절 A4f2 8번).
    // Finish가 복사한다(Finish까지 살아야 한다). 셰이더 없는 프로필이면 비어 있다.
    Span<const CookedManifestBackend> backends{};
};

// 게시기다(CookPackages의 출력 sink). 한 쿠킹에 한 번 쓴다: Begin → Put·Reuse → Finish. 한
// 스레드에서 쓴다.
class ROCKS_ASSETCOOKER_API CookPublisher final : public CookOutputSink {
public:
    CookPublisher(Allocator& allocator, const CookPublishConfig& config) noexcept;

    CookPublisher(const CookPublisher&) = delete;
    CookPublisher& operator=(const CookPublisher&) = delete;

    // 쿠킹 루트·`.staging`·`Packages`·`Shaders`를 만들고 `.staging` 안의 고아 파일을 지운다(못
    // 지우면 경고).
    // 오류: 설정 null·빈 칸·두 번째 호출 InvalidArgument, lock이 이 쿠킹 루트의 잡은 잠금이 아니면
    // InvalidArgument(에셋 DB 잠금은 CookProject가 본다 — 10.3.1절 A4f4 2번), 디렉터리 만들기·열거
    // IO 오류, 할당 실패.
    Result<void> Begin(const CookRootLock& lock) noexcept;

    // 쿠킹 패키지 하나를 `.staging/<대상 파일 이름>.tmp`에 쓰고 `Packages/<a>/<A>-<h>.rpkg`로
    // 덮어쓰지 않고 옮긴다. 대상이 있으면 크기·SHA-256을 대조해 같으면 임시 파일을 지우고, 다르면
    // 경고 CorruptEntry 뒤 교체한다. 매니페스트 항목을 더한다. 오류: Begin 전 InvalidArgument, 파일
    // IO 오류, 할당 실패.
    Result<void> Put(const CookedPackage& package) noexcept override;

    // 재사용 항목을 더한다: byte에서 나오는 칸(SHA-256·크기·bulk raw·대표 타입·의존·cookKey)은
    // existing에서, byte 밖 칸(경로 힌트·flags·bundle)은 이번 판정(target·pathHint)에서 온다(결정
    // 8). 오류: Begin 전 InvalidArgument, 할당 실패.
    Result<void> Reuse(const CookedManifestEntry& existing, const CookTarget& target,
                       StringView pathHint) noexcept;

    // 단계가 staging에 쓴 artifact 파일을 게시한다(10.3.1절 A4f3 4번): 파일을 흘려 읽어
    // 크기·SHA-256을 직접 계산하고, `Shaders/<이름>-<형식>-<hash>.rshl`로 덮어쓰지 않고 옮긴다.
    // 같은 이름이 있으면 크기·SHA-256을 대조해 같으면 staging 파일을 지우고, 다르면 경고
    // CorruptEntry(asset null) 뒤 교체한다. 성공하면 placed가 추가한 항목을 가리킨다(다음
    // PutArtifact·ReuseArtifact까지 유효). 오류: Begin 전 InvalidArgument, 파일 0
    // byte·CookedArtifactMaxBytes 초과·칸 문법 위반 InvalidArgument(staging 파일은 지운다), 파일 IO
    // 오류, 할당 실패.
    Result<void> PutArtifact(CookArtifactKind kind, StringView name, StringView formatId,
                             const Sha256Digest& cookKey, StringView stagingFile,
                             const CookedManifestArtifact** placed = nullptr) noexcept;

    // 재사용 artifact 항목을 그대로 더한다(10.3절 6항). 오류: Begin 전 InvalidArgument, 할당 실패.
    Result<void> ReuseArtifact(const CookedManifestArtifact& artifact) noexcept;

    // 게시된 artifact 파일의 크기·SHA-256이 항목과 같은지 본다(재사용 후보 판정 — 파일이 없거나
    // 읽을 수 없으면 거짓). 오류: Begin 전 InvalidArgument, 할당 실패.
    Result<bool> ArtifactFileMatches(const CookedManifestArtifact& artifact) noexcept;

    // 이번 쿠킹의 artifact 항목(PutArtifact·ReuseArtifact 순 — Finish 전)이다.
    Span<const CookedManifestArtifact> Artifacts() const noexcept {
        return artifacts_.View();
    }

    // 게시를 끝낸다: 바뀐 디렉터리를 안쪽부터 동기화하고(NotDurable 경고) 매니페스트를 만들어 원자
    // 교체한다. artifact 표는 이번 항목과(부분 쿠킹이면) merge의 같은 키가 없는 항목이다. merge가
    // 있으면 부분 쿠킹이다 — merge의 항목 중 이번 항목도 removed도 아닌 것을
    // 더하고, CheckCookedManifestMerge가 다르면 오류 진단 IncompatibleCook을 남기고 게시하지
    // 않는다. merge가 없으면 전체 쿠킹이다 — 게시 뒤 새 매니페스트가 가리키지 않는 `Packages/`·
    // `Shaders/` 파일을 지운다(못 지우면 경고). 매니페스트 검증 위반은 그 진단 코드로 오류 진단을
    // 남기고 게시하지 않는다. 오류: Begin 전·두 번째 호출 InvalidArgument, 동기화·쓰기·교체·열거 IO
    // 오류, 할당 실패.
    Result<void> Finish(const CookedManifest* merge, Span<const AssetId> removed) noexcept;

    // 쿠킹 루트 경로다(Begin 뒤).
    StringView Root() const noexcept {
        return root_.View();
    }

    // 게시 진단(경고·오류 — CookDiagnosticBefore 순은 Finish 뒤)이다.
    Span<const CookDiagnostic> Diagnostics() const noexcept {
        return diagnostics_.View();
    }

    bool HasErrors() const noexcept;

    // 새로 쓴 패키지 byte 합(같은 내용이 이미 있던 것은 빼고)·게시 여부다.
    uint64 BytesWritten() const noexcept {
        return bytesWritten_;
    }

    bool Published() const noexcept {
        return published_;
    }

private:
    Result<void> AddEntry(const CookedPackage& package) noexcept;
    // temp를 target으로 덮어쓰지 않고 옮긴다(target 디렉터리를 만들고, 같은 이름이 있으면 크기·
    // SHA-256 대조 — 같으면 temp를 지우고 wrote 거짓, 다르면 CorruptEntry 경고 뒤 교체). 실패하면
    // temp를 지운다.
    Result<void> PlaceStaged(StringView temp, StringView target, usize directoryBytes, uint64 bytes,
                             const Sha256Digest& digest, AssetId asset, bool& wrote) noexcept;
    Result<void> RememberDirectory(StringView directory) noexcept;
    Result<void> SyncDirectories() noexcept;
    Result<void> RemoveUnreferenced(const CookedManifest& manifest) noexcept;
    Result<void> Warn(AssetDiagnosticCode code, AssetId asset, ErrorCode error) noexcept;
    Result<void> Refuse(AssetDiagnosticCode code, AssetId asset, AssetId related,
                        ErrorCode error) noexcept;

    Allocator* allocator_;
    CookPublishConfig config_;
    String root_;
    Array<CookedManifestEntry> entries_;
    Array<CookedManifestArtifact> artifacts_;
    // 동기화할 디렉터리(전체 경로, 중복 없음)다.
    Array<String> directories_;
    Array<CookDiagnostic> diagnostics_;
    uint64 bytesWritten_ = 0;
    bool begun_ = false;
    bool finished_ = false;
    bool published_ = false;
};

struct CookReuseRequest {
    platform::FileSystem* files = nullptr;
    // 쿠킹 루트(AppendCookedRoot)다.
    StringView cookedRoot{};
    // 기존 매니페스트다(없으면 null — 재사용 없음).
    const CookedManifest* existing = nullptr;
    const CookTargetSet* targets = nullptr;
    const CookKeySet* keys = nullptr;
    // 이번 쿠킹의 매니페스트 머리 값이다(기존 머리의 이 셋이 다르면 재사용 없음).
    StringView profileId{};
    Sha256Digest profileSettings{};
    uint32 cookerVersion = CookerVersion;
};

struct CookReusePlan {
    explicit CookReusePlan(Allocator& allocator) noexcept : reuse(allocator), entries(allocator) {
    }

    // 대상마다 재사용 표지(CookRunRequest::reuse)와 재사용할 기존 항목(아니면 null)이다.
    Array<uint8> reuse;
    Array<const CookedManifestEntry*> entries;
    usize count = 0;
};

// 재사용 판정(결정 8): 기존 머리의 프로파일 id·출력 설정·쿠커 버전이 이번과 같을 때, 키가 있는
// 대상의 기존 항목 cookKey가 같고 그 파일이 있으며 크기·SHA-256이 같으면 재사용한다(파일이 없거나
// 읽을 수 없거나 다르면 다시 쿠킹 — 시나리오 27). 오류: 요청 인자 null·키 수가 대상 수와 다름
// InvalidArgument, 할당 실패.
ROCKS_ASSETCOOKER_API Result<void>
PlanCookReuse(Allocator& allocator, const CookReuseRequest& request, CookReusePlan& out) noexcept;

} // namespace rocks
