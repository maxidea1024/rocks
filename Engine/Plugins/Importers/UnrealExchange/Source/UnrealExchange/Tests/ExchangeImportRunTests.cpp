#include "ExchangeTestSupport.h"

#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeImportRun.h>

// Unreal 교환 임포트 실행 시험이다(Docs/Design/UnrealExchange.ko.md 10.5절 7·8번): 종류별 임포터
// 표, 단위 순서와 참조 id 표, 새로 만듦·재임포트, Hard 순환 단위 실패, 임포터 없는 클래스 건너뜀과
// 의존자(텍스처는 경고, 그 밖은 실패), 임포터 실패의 진단 코드, 계획 실패의 결과 코드, 범위 밖
// 선택, dry run, 할당 실패.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_import_run_tests_file {

namespace {

bool ReadSynthetic(Allocator& allocator, Span<const SyntheticAsset> assets, ExchangeBundle& bundle,
                   ExchangeDiagnostics& diagnostics) noexcept {
    MemoryFiles files(allocator);
    return WriteSyntheticBundle(allocator, files, assets) &&
           ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics);
}

AssetId MadeId(uint32 asset) noexcept {
    return AssetId{Guid{0xF00, asset + 1ull}};
}

AssetId ExistingId(uint64 low) noexcept {
    return AssetId{Guid{0xE1, low}};
}

// 부른 순서와 요청을 기록하는 시험 임포터다(서비스에 쓰지 않는다). 새 에셋은 MadeId, 재임포트는
// 기존 id를 돌려준다. failPath 에셋은 진단을 더하고 failWith로 실패한다(진단 없이 실패하려면
// failQuietly, 오류 앞에 경고·다른 에셋 진단을 섞으려면 noisy). breakContract는 계약 위반 id를
// 돌려준다(1 = null id, 2 = 재임포트인데 다른 id).
class RecordingKind final : public ExchangeKindImporter {
public:
    explicit RecordingKind(Allocator& allocator) noexcept
        : calls(allocator), existing(allocator), paths(allocator), resolvedSeen(allocator) {
    }

    Result<AssetId> Import(const ExchangeKindRequest& request) noexcept override {
        if (!calls.TryPushBack(request.asset) || !existing.TryPushBack(request.existing) ||
            !paths.TryPushBack(request.path) || !resolvedSeen.TryResize(request.resolved.Size())) {
            return Result<AssetId>::Failure(ErrorCode::OutOfMemory);
        }

        for (usize index = 0; index < request.resolved.Size(); ++index) {
            resolvedSeen[index] = request.resolved[index];
        }

        optionsSeen = request.options;
        const ExchangeAsset& asset = request.bundle->Assets()[request.asset];
        if (asset.objectPath == failPath) {
            if (noisy) {
                ExchangeDiagnosticInput warning;
                warning.code = AssetDiagnosticCode::UnknownField;
                warning.asset = request.asset;
                ExchangeDiagnosticInput other;
                other.code = AssetDiagnosticCode::InvalidSource;
                other.error = failWith;
                other.asset = request.asset + 1;
                if (auto added = request.diagnostics->TryAdd(warning); !added) {
                    return Result<AssetId>::Failure(added.Error());
                }

                if (auto added = request.diagnostics->TryAdd(other); !added) {
                    return Result<AssetId>::Failure(added.Error());
                }
            }

            if (!failQuietly) {
                ExchangeDiagnosticInput input;
                input.code = AssetDiagnosticCode::InvalidSource;
                input.error = failWith;
                input.asset = request.asset;
                if (auto added = request.diagnostics->TryAdd(input); !added) {
                    return Result<AssetId>::Failure(added.Error());
                }
            }

            return Result<AssetId>::Failure(failWith);
        }

        if (breakContract == 1) {
            return Result<AssetId>::Success(AssetId{});
        }

        if (breakContract == 2) {
            return Result<AssetId>::Success(MadeId(request.asset));
        }

        return Result<AssetId>::Success(request.existing.IsNull() ? MadeId(request.asset)
                                                                  : request.existing);
    }

    Array<uint32> calls;
    Array<AssetId> existing;
    Array<StringView> paths;
    // 마지막 호출이 받은 참조 id 표다.
    Array<AssetId> resolvedSeen;
    const ExchangeImportOptions* optionsSeen = nullptr;
    StringView failPath;
    ErrorCode failWith = ErrorCode::InvalidArgument;
    bool failQuietly = false;
    bool noisy = false;
    uint32 breakContract = 0;
};

// objectPath → 기존 에셋 표의 조회 대역이다.
class TableLookup final : public ExchangeExistingAssets {
public:
    struct Row {
        StringView objectPath;
        uint64 id = 0;
        StringView path;
    };

    explicit TableLookup(Span<const Row> rows) noexcept : rows_(rows) {
    }

    Result<ExchangeExistingMatch> Find(StringView objectPath,
                                       Span<const StringView> formerPaths) const noexcept override {
        (void)formerPaths;
        ExchangeExistingMatch match;
        for (const Row& row : rows_) {
            if (row.objectPath == objectPath) {
                match.state = ExchangeExistingState::Found;
                match.id = ExistingId(row.id);
                match.path = row.path;
            }
        }

        return Result<ExchangeExistingMatch>::Success(match);
    }

    Result<bool> PathTaken(StringView logicalPath) const noexcept override {
        (void)logicalPath;
        return Result<bool>::Success(false);
    }

private:
    Span<const Row> rows_;
};

// 실행 시험 묶음: 클립 → 스켈레톤, 머티리얼 → 텍스처, 서로 가리키는 메시 둘, 레벨(범위 밖).
// 클립은 묶음에 없는 텍스처(Hard)와 묶음 안 텍스처(Soft)도 가리키고, 텍스처는 자기 자신을
// 가리킨다 — 실행의 의존 검사에서 빠지는 경우들이다.
const SyntheticDependency ClipDependencies[] = {{"/Game/B/SKEL.SKEL", "Skeleton"},
                                                {"/Game/D/T.T", "Texture2D", true},
                                                {"/Game/Ext/E.E", "Texture2D"}};
const SyntheticDependency TextureDependencies[] = {{"/Game/D/T.T", "Texture2D"}};
// 머티리얼은 범위 밖 대상(레벨)도 Hard로 가리킨다(순서·전파에서 빠진다).
const SyntheticDependency MaterialDependencies[] = {{"/Game/D/T.T", "Texture2D"},
                                                    {"/Game/L/L.L", "World"}};
const SyntheticDependency FirstMeshDependencies[] = {{"/Game/E/SM2.SM2", "StaticMesh"}};
const SyntheticDependency SecondMeshDependencies[] = {{"/Game/E/SM1.SM1", "StaticMesh"}};
const SyntheticAsset RunAssets[] = {
    {"/Game/A/Clip.Clip", "AnimSequence", Span<const SyntheticDependency>(ClipDependencies)},
    {"/Game/B/SKEL.SKEL", "Skeleton", {}},
    {"/Game/C/M.M", "Material", Span<const SyntheticDependency>(MaterialDependencies)},
    {"/Game/D/T.T", "Texture2D", Span<const SyntheticDependency>(TextureDependencies)},
    {"/Game/E/SM1.SM1", "StaticMesh", Span<const SyntheticDependency>(FirstMeshDependencies)},
    {"/Game/E/SM2.SM2", "StaticMesh", Span<const SyntheticDependency>(SecondMeshDependencies)},
    {"/Game/L/L.L", "World", {}},
};
constexpr uint32 Clip = 0;
constexpr uint32 Skeleton = 1;
constexpr uint32 Material = 2;
constexpr uint32 Texture = 3;
constexpr uint32 FirstMesh = 4;
constexpr uint32 SecondMesh = 5;
constexpr uint32 Level = 6;

// 묶음·계획·임포터 하나를 가진 실행 장면이다.
struct RunScene {
    explicit RunScene(Allocator& allocator) noexcept
        : bundle(allocator), diagnostics(allocator), plan(allocator), kind(allocator),
          result(allocator) {
    }

    bool Plan(const ExchangeExistingAssets* existing = nullptr) noexcept {
        ExchangeOrderOptions options;
        options.existing = existing;
        return static_cast<bool>(PlanExchangeImport(bundle, options, plan, diagnostics));
    }

    // classes의 임포터를 kind로 등록하고 실행한다.
    Result<void> Run(Span<const ExchangeClass> classes, bool dryRun = false) noexcept {
        for (const ExchangeClass type : classes) {
            if (auto registered = kinds.Register(type, kind); !registered) {
                return registered;
            }
        }

        ExchangeRunInput input;
        input.bundle = &bundle;
        input.plan = &plan;
        input.kinds = &kinds;
        input.options = &options;
        return RunExchangeImport(input, dryRun, result, diagnostics);
    }

    usize Codes(AssetDiagnosticCode code, uint32 asset) const noexcept {
        usize count = 0;
        for (const ExchangeDiagnostic& item : diagnostics.Items()) {
            count += item.code == code && item.asset == asset ? 1 : 0;
        }

        return count;
    }

    usize Issues(ExchangeIssue issue, uint32 asset,
                 uint32 related = ExchangeNoIndex) const noexcept {
        usize count = 0;
        for (const ExchangeDiagnostic& item : diagnostics.Items()) {
            count += item.issue == issue && item.asset == asset && item.related == related ? 1 : 0;
        }

        return count;
    }

    ExchangeBundle bundle;
    ExchangeDiagnostics diagnostics;
    ExchangeImportPlan plan;
    ExchangeKindTable kinds;
    RecordingKind kind;
    ExchangeImportOptions options;
    ExchangeImportResult result;
};

const ExchangeClass AllRunClasses[] = {ExchangeClass::AnimSequence, ExchangeClass::Skeleton,
                                       ExchangeClass::Material, ExchangeClass::Texture2D,
                                       ExchangeClass::StaticMesh};

} // namespace

// 종류별 임포터 표: 범위 밖 클래스 거부, 같은 클래스 둘, 없는 클래스.
TEST(UnrealExchange_KindTable) {
    SystemAllocator allocator;
    {
        RecordingKind kind(allocator);
        ExchangeKindTable table;
        CHECK(table.Find(ExchangeClass::Texture2D) == nullptr &&
              table.Find(ExchangeClass::Unsupported) == nullptr);
        CHECK(table.Register(ExchangeClass::Unsupported, kind).Error() ==
              ErrorCode::InvalidArgument);
        REQUIRE(table.Register(ExchangeClass::Texture2D, kind));
        CHECK(table.Register(ExchangeClass::Texture2D, kind).Error() == ErrorCode::AlreadyExists);
        CHECK(table.Find(ExchangeClass::Texture2D) == &kind &&
              table.Find(ExchangeClass::TextureCube) == nullptr);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 순서와 참조: 깊이 0 단위(스켈레톤·텍스처·메시 순환)부터, 의존자는 앞서 만든 id를 받는다. 순환
// 단위는 둘 다 HardCycle, 범위 밖 선택은 끝에. 옵션이 그대로 전달된다.
TEST(UnrealExchange_RunOrderAndReferences) {
    SystemAllocator allocator;
    {
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        scene.options.meshNormals = ExchangeMeshNormals::Rocks;
        REQUIRE(scene.Run(Span<const ExchangeClass>(AllRunClasses)));
        const uint32 calls[] = {Skeleton, Texture, Clip, Material};
        REQUIRE(scene.kind.calls.Size() == 4);
        for (usize index = 0; index < 4; ++index) {
            CHECK(scene.kind.calls[index] == calls[index] && scene.kind.existing[index].IsNull() &&
                  scene.kind.paths[index] == scene.plan.MappedPath(calls[index]));
        }

        // 마지막 호출(머티리얼)이 본 표: 앞서 만든 셋, 순환 메시는 없음.
        CHECK(scene.kind.resolvedSeen[Skeleton] == MadeId(Skeleton) &&
              scene.kind.resolvedSeen[Texture] == MadeId(Texture) &&
              scene.kind.resolvedSeen[Clip] == MadeId(Clip) &&
              scene.kind.resolvedSeen[FirstMesh].IsNull());
        CHECK(scene.kind.optionsSeen == &scene.options);

        const ExchangeImportResult& result = scene.result;
        CHECK(result.Outcome(Clip).state == ExchangeOutcomeState::Created &&
              result.Outcome(Clip).id == MadeId(Clip));
        CHECK(result.Outcome(FirstMesh).state == ExchangeOutcomeState::Failed &&
              result.Outcome(FirstMesh).failure == AssetDiagnosticCode::UnsupportedCycle &&
              result.Outcome(FirstMesh).error == ErrorCode::CycleDetected &&
              result.Outcome(SecondMesh).failure == AssetDiagnosticCode::UnsupportedCycle);
        CHECK(scene.Issues(ExchangeIssue::HardCycle, FirstMesh) == 1 &&
              scene.Issues(ExchangeIssue::HardCycle, SecondMesh) == 1);
        CHECK(result.Outcome(Level).state == ExchangeOutcomeState::Unsupported);
        REQUIRE(result.Order().Size() == 7);
        CHECK(result.Order()[6] == Level);
        CHECK(result.Count(ExchangeOutcomeState::Created) == 4 &&
              result.Count(ExchangeOutcomeState::Failed) == 2 &&
              result.Count(ExchangeOutcomeState::Unsupported) == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 임포터 실패: 그 에셋의 첫 오류 진단 코드로 실패하고 다음으로 넘어간다. 스켈레톤 의존자는 실패
// (FailedDependency), 진단 없이 실패하면 코드 None과 원본 오류.
TEST(UnrealExchange_RunImporterFailure) {
    SystemAllocator allocator;
    {
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        scene.kind.failPath = "/Game/B/SKEL.SKEL";
        REQUIRE(scene.Run(Span<const ExchangeClass>(AllRunClasses)));
        const ExchangeImportResult& result = scene.result;
        CHECK(result.Outcome(Skeleton).state == ExchangeOutcomeState::Failed &&
              result.Outcome(Skeleton).failure == AssetDiagnosticCode::InvalidSource &&
              result.Outcome(Skeleton).error == ErrorCode::InvalidArgument);
        CHECK(result.Outcome(Clip).state == ExchangeOutcomeState::Failed &&
              result.Outcome(Clip).failure == AssetDiagnosticCode::MissingDependency &&
              result.Outcome(Clip).error == ErrorCode::NotFound);
        CHECK(scene.Issues(ExchangeIssue::FailedDependency, Clip, Skeleton) == 1);
        CHECK(result.Outcome(Material).state == ExchangeOutcomeState::Created);
        // 클립은 부르지 않았다.
        CHECK(scene.kind.calls.Size() == 3);
    }

    {
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        scene.kind.failPath = "/Game/D/T.T";
        scene.kind.failQuietly = true;
        scene.kind.failWith = ErrorCode::Busy;
        REQUIRE(scene.Run(Span<const ExchangeClass>(AllRunClasses)));
        const ExchangeAssetOutcome& texture = scene.result.Outcome(Texture);
        CHECK(texture.state == ExchangeOutcomeState::Failed &&
              texture.failure == AssetDiagnosticCode::None && texture.error == ErrorCode::Busy);
        // 실행 중 실패한 대상은 텍스처라도 의존자를 실패시킨다(9.3절 — 비워 두는 대체는 묶음에
        // 없거나 건너뛴 대상만).
        CHECK(scene.result.Outcome(Material).state == ExchangeOutcomeState::Failed &&
              scene.result.Outcome(Material).failure == AssetDiagnosticCode::MissingDependency &&
              scene.Issues(ExchangeIssue::FailedDependency, Material, Texture) == 1 &&
              scene.Codes(AssetDiagnosticCode::MissingUnrealDependency, Material) == 0);
    }

    {
        // 오류 앞의 경고·다른 에셋 진단은 결과 코드가 아니다(그 에셋의 첫 오류).
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        scene.kind.failPath = "/Game/B/SKEL.SKEL";
        scene.kind.noisy = true;
        REQUIRE(scene.Run(Span<const ExchangeClass>(AllRunClasses)));
        CHECK(scene.result.Outcome(Skeleton).failure == AssetDiagnosticCode::InvalidSource);
    }

    {
        // 실행 중 실패한 대상은 기존 에셋이 있어도 옛 버전에 잇지 않는다.
        const TableLookup::Row rows[] = {{"/Game/B/SKEL.SKEL", 7, "/Game/Rocks/Skeleton"}};
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan(&lookup));
        scene.kind.failPath = "/Game/B/SKEL.SKEL";
        REQUIRE(scene.Run(Span<const ExchangeClass>(AllRunClasses)));
        CHECK(scene.result.Outcome(Clip).state == ExchangeOutcomeState::Failed &&
              scene.result.Outcome(Clip).failure == AssetDiagnosticCode::MissingDependency &&
              scene.Issues(ExchangeIssue::FailedDependency, Clip, Skeleton) == 1);
    }

    {
        // 임포터 계약 위반: null id, 재임포트인데 다른 id — 그 에셋의 실패(InvalidArgument).
        const TableLookup::Row rows[] = {{"/Game/B/SKEL.SKEL", 7, "/Game/Rocks/Skeleton"}};
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        for (uint32 mode = 1; mode <= 2; ++mode) {
            RunScene scene(allocator);
            REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                                  scene.diagnostics));
            REQUIRE(scene.Plan(&lookup));
            scene.kind.breakContract = mode;
            const ExchangeClass skeletonOnly[] = {ExchangeClass::Skeleton};
            REQUIRE(scene.Run(Span<const ExchangeClass>(skeletonOnly)));
            const ExchangeAssetOutcome& skeleton = scene.result.Outcome(Skeleton);
            CHECK(skeleton.state == ExchangeOutcomeState::Failed &&
                  skeleton.failure == AssetDiagnosticCode::None &&
                  skeleton.error == ErrorCode::InvalidArgument && skeleton.id.IsNull());
        }
    }

    {
        // 할당 실패만 실행 전체를 멈춘다.
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        scene.kind.failPath = "/Game/B/SKEL.SKEL";
        scene.kind.failWith = ErrorCode::OutOfMemory;
        CHECK(scene.Run(Span<const ExchangeClass>(AllRunClasses)).Error() ==
              ErrorCode::OutOfMemory);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 임포터 없는 클래스: 건너뛰고(UnsupportedAsset NoKindImporter 경고) 그 의존자는 텍스처면 경고,
// 스켈레톤이면 실패. 기존 에셋이 있으면 건너뛴 대상의 id로 이어진다.
TEST(UnrealExchange_RunWithoutKindImporter) {
    SystemAllocator allocator;
    const ExchangeClass onlyDependents[] = {ExchangeClass::AnimSequence, ExchangeClass::Material};
    {
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        REQUIRE(scene.Run(Span<const ExchangeClass>(onlyDependents)));
        const ExchangeImportResult& result = scene.result;
        CHECK(result.Outcome(Skeleton).state == ExchangeOutcomeState::Skipped &&
              result.Outcome(Texture).state == ExchangeOutcomeState::Skipped);
        CHECK(scene.Issues(ExchangeIssue::NoKindImporter, Skeleton) == 1 &&
              scene.Issues(ExchangeIssue::NoKindImporter, Texture) == 1);
        // 읽기의 범위 밖 클래스(레벨)와 같은 코드다 — 세부 규칙으로 고른다.
        const ExchangeDiagnostic* skipped = nullptr;
        for (const ExchangeDiagnostic& item : scene.diagnostics.Items()) {
            if (item.issue == ExchangeIssue::NoKindImporter && item.asset == Skeleton) {
                skipped = &item;
            }
        }

        REQUIRE(skipped != nullptr);
        CHECK(skipped->code == AssetDiagnosticCode::UnsupportedAsset &&
              skipped->severity == ExchangeSeverity::Warning && skipped->text.View() == "Skeleton");
        CHECK(result.Outcome(Clip).state == ExchangeOutcomeState::Failed &&
              result.Outcome(Clip).failure == AssetDiagnosticCode::MissingDependency);
        CHECK(result.Outcome(Material).state == ExchangeOutcomeState::Created);
        CHECK(result.Count(ExchangeOutcomeState::Skipped) == 2);
    }

    {
        // 기존 스켈레톤이 있으면 클립이 그 id를 받고, 기존 텍스처가 있는 머티리얼은 재임포트한다.
        const TableLookup::Row rows[] = {{"/Game/B/SKEL.SKEL", 7, "/Game/Rocks/Skeleton"},
                                         {"/Game/C/M.M", 8, "/Game/Rocks/Material"}};
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan(&lookup));
        REQUIRE(scene.Run(Span<const ExchangeClass>(onlyDependents)));
        const ExchangeImportResult& result = scene.result;
        CHECK(result.Outcome(Skeleton).state == ExchangeOutcomeState::Skipped);
        CHECK(result.Outcome(Clip).state == ExchangeOutcomeState::Created);
        REQUIRE(scene.kind.calls.Size() == 2 && scene.kind.calls[0] == Clip);
        CHECK(result.Outcome(Material).state == ExchangeOutcomeState::Reimported &&
              result.Outcome(Material).id == ExistingId(8));
        CHECK(scene.kind.existing[1] == ExistingId(8) &&
              scene.kind.paths[1] == "/Game/Rocks/Material");
        CHECK(result.Count(ExchangeOutcomeState::Reimported) == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 클립이 기존 스켈레톤 id를 받는다(첫 호출의 표).
TEST(UnrealExchange_RunResolvesExistingTarget) {
    SystemAllocator allocator;
    {
        const TableLookup::Row rows[] = {{"/Game/B/SKEL.SKEL", 7, "/Game/Rocks/Skeleton"}};
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan(&lookup));
        const ExchangeClass clipOnly[] = {ExchangeClass::AnimSequence};
        REQUIRE(scene.Run(Span<const ExchangeClass>(clipOnly)));
        REQUIRE(scene.kind.calls.Size() == 1);
        CHECK(scene.kind.resolvedSeen[Skeleton] == ExistingId(7));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 계획 실패의 결과: 경로 겹침은 그 코드, 순환 단위·실패 의존자, dry run(임포터를 부르지 않음).
TEST(UnrealExchange_RunPlanFailuresAndDryRun) {
    SystemAllocator allocator;
    {
        const SyntheticDependency onSkeleton[] = {{"/Game/Ext/SKEL.SKEL", "Skeleton"}};
        const SyntheticDependency onCollided[] = {{"/Game/X/Tex.Tex", "Texture2D"}};
        const SyntheticAsset assets[] = {
            {"/Game/A/Clip.Clip", "AnimSequence", Span<const SyntheticDependency>(onSkeleton)},
            {"/Game/B/M.M", "Material", Span<const SyntheticDependency>(onCollided)},
            {"/Game/X/Tex.Tex", "Texture2D", {}},
            {"/Game/x/tex.tex", "Texture2D", {}},
        };
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        REQUIRE(scene.Run(Span<const ExchangeClass>(AllRunClasses)));
        const ExchangeImportResult& result = scene.result;
        CHECK(result.Outcome(0).failure == AssetDiagnosticCode::MissingDependency &&
              result.Outcome(0).error == ErrorCode::NotFound);
        CHECK(result.Outcome(1).failure == AssetDiagnosticCode::MissingDependency);
        CHECK(result.Outcome(2).failure == AssetDiagnosticCode::UnrealPathCollision &&
              result.Outcome(2).error == ErrorCode::AlreadyExists);
        CHECK(scene.kind.calls.IsEmpty());
    }

    {
        const TableLookup::Row rows[] = {{"/Game/C/M.M", 8, "/Game/Rocks/Material"}};
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan(&lookup));
        REQUIRE(scene.Run({}, true));
        const ExchangeImportResult& result = scene.result;
        CHECK(scene.kind.calls.IsEmpty());
        CHECK(result.Outcome(Skeleton).state == ExchangeOutcomeState::PlannedCreate &&
              result.Outcome(Skeleton).id.IsNull());
        CHECK(result.Outcome(Material).state == ExchangeOutcomeState::PlannedReimport &&
              result.Outcome(Material).id == ExistingId(8));
        CHECK(result.Outcome(FirstMesh).failure == AssetDiagnosticCode::UnsupportedCycle &&
              result.Outcome(Level).state == ExchangeOutcomeState::Unsupported);
        CHECK(result.Count(ExchangeOutcomeState::PlannedCreate) == 3 &&
              result.Count(ExchangeOutcomeState::PlannedReimport) == 1);
    }

    {
        // 경로 길이 초과로 실패한 에셋의 결과 코드.
        constexpr StringView LongPath =
            "/Game/D/Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/Long.Long";
        const SyntheticAsset assets[] = {{LongPath, "Texture2D", {}}};
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        REQUIRE(scene.Run({}, true));
        CHECK(scene.result.Outcome(0).failure == AssetDiagnosticCode::UnrealPathTooLong &&
              scene.result.Outcome(0).error == ErrorCode::CapacityExceeded);
    }

    {
        // 읽기에 실패한 에셋의 결과 코드(CorruptBundle → InvalidArgument).
        const SyntheticAsset assets[] = {{"/Game/A/T.T", "Texture2D", {}, "[]"}};
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        REQUIRE(scene.Run({}, true));
        CHECK(scene.result.Outcome(0).failure == AssetDiagnosticCode::CorruptBundle &&
              scene.result.Outcome(0).error == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당 실패: 실행의 모든 할당 지점(결과·참조 표·순서·진단)에서 OutOfMemory이고 새는 것이 없다.
TEST(UnrealExchange_RunAllocationFailure) {
    SystemAllocator allocator;
    {
        RunScene scene(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(RunAssets), scene.bundle,
                              scene.diagnostics));
        REQUIRE(scene.Plan());
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            failing.FailOnAttempt(attempt);
            ExchangeDiagnostics diagnostics(failing);
            ExchangeImportResult result(failing);
            ExchangeKindTable kinds;
            RecordingKind kind(allocator);
            REQUIRE(kinds.Register(ExchangeClass::Skeleton, kind));
            REQUIRE(kinds.Register(ExchangeClass::Material, kind));
            ExchangeImportOptions options;
            ExchangeRunInput input;
            input.bundle = &scene.bundle;
            input.plan = &scene.plan;
            input.kinds = &kinds;
            input.options = &options;
            auto ran = RunExchangeImport(input, false, result, diagnostics);
            if (!ran) {
                CHECK(ran.Error() == ErrorCode::OutOfMemory);
                ++failures;
                continue;
            }

            CHECK(result.Outcome(Skeleton).state == ExchangeOutcomeState::Created);
            break;
        }

        CHECK(failures > 3);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_import_run_tests_file
