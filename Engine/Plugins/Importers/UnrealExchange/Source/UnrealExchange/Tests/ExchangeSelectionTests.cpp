#include "ExchangeTestSupport.h"

#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// Unreal 교환 임포트 계획의 선택·기존 에셋 시험이다(Docs/Design/UnrealExchange 10.5절 2–6번):
// --only 닫힘(Hard만, redirect, 범위 밖 직접 선택, 없는 값), 선택과 무관한 경로·겹침(결정성)과 고른
// 에셋만의 진단·단위, 잘못된 선택, 기존 에셋 대상 경로·길이·겹침 제외, 모호함과 전파, 옛 경로
// 전달, 조회 오류, 할당 실패.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_selection_tests_file {

namespace {

// 합성 묶음을 읽는다.
bool ReadSynthetic(Allocator& allocator, Span<const SyntheticAsset> assets, ExchangeBundle& bundle,
                   ExchangeDiagnostics& diagnostics,
                   Span<const SyntheticRedirect> redirects = {}) noexcept {
    MemoryFiles files(allocator);
    return WriteSyntheticBundle(allocator, files, assets, redirects) &&
           ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics);
}

// objectPath → 결과 표로 답하는 조회 대역이다. 받은 옛 경로 목록을 마지막 것만 기록한다.
class TableLookup final : public ExchangeExistingAssets {
public:
    struct Row {
        StringView objectPath;
        ExchangeExistingState state = ExchangeExistingState::None;
        uint64 id = 0;
        StringView path;
    };

    explicit TableLookup(Span<const Row> rows) noexcept : rows_(rows) {
    }

    Result<ExchangeExistingMatch> Find(StringView objectPath,
                                       Span<const StringView> formerPaths) const noexcept override {
        if (failWith != ErrorCode::None) {
            return Result<ExchangeExistingMatch>::Failure(failWith);
        }

        if (objectPath == watched) {
            watchedFormerCount = formerPaths.Size();
            watchedFormerSorted = formerPaths.Size() == 2 && formerPaths[0] == "/Game/Old/A.A" &&
                                  formerPaths[1] == "/Game/Old/B.B";
            watchedCalls += 1;
        }

        ExchangeExistingMatch match;
        for (const Row& row : rows_) {
            if (row.objectPath == objectPath) {
                match.state = row.state;
                // 100 이상은 high가 다르다(id 정렬이 high를 먼저 본다).
                match.id = row.id == 0 ? AssetId{} : AssetId{Guid{0xE1 + row.id / 100, row.id}};
                match.path = row.path;
                for (const StringView former : formerMatches) {
                    match.former = match.former || former == objectPath;
                }
            }
        }

        return Result<ExchangeExistingMatch>::Success(match);
    }

    Result<bool> PathTaken(StringView logicalPath) const noexcept override {
        if (pathFailWith != ErrorCode::None) {
            return Result<bool>::Failure(pathFailWith);
        }

        return Result<bool>::Success(logicalPath == takenPath || logicalPath == otherTakenPath);
    }

    ErrorCode failWith = ErrorCode::None;
    ErrorCode pathFailWith = ErrorCode::None;
    StringView takenPath;
    StringView otherTakenPath;
    // 둘째 단계(옛 경로)로 찾은 것으로 답할 objectPath들이다.
    Span<const StringView> formerMatches;
    StringView watched;
    mutable usize watchedFormerCount = 0;
    mutable bool watchedFormerSorted = false;
    mutable uint32 watchedCalls = 0;

private:
    Span<const Row> rows_;
};

usize CountIssue(const ExchangeDiagnostics& diagnostics, ExchangeIssue issue,
                 uint32 asset) noexcept {
    usize count = 0;
    for (const ExchangeDiagnostic& item : diagnostics.Items()) {
        count += item.issue == issue && item.asset == asset ? 1 : 0;
    }

    return count;
}

// 선택 시험 묶음: 머티리얼 → 텍스처(Hard)·함수(Soft), 메시 → 머티리얼(Hard)·레벨(범위 밖), 따로
// 있는 텍스처, 메시의 옛 경로 redirect.
const SyntheticDependency MaterialDependencies[] = {{"/Game/B/T.T", "Texture2D"},
                                                    {"/Game/C/F.F", "MaterialFunction", true}};
const SyntheticDependency MeshDependencies[] = {{"/Game/A/M.M", "Material"},
                                                {"/Game/L/L.L", "World"}};
const SyntheticAsset SelectionAssets[] = {
    {"/Game/A/M.M", "Material", Span<const SyntheticDependency>(MaterialDependencies)},
    {"/Game/B/T.T", "Texture2D", {}},
    {"/Game/C/F.F", "MaterialFunction", {}},
    {"/Game/D/SM.SM", "StaticMesh", Span<const SyntheticDependency>(MeshDependencies)},
    {"/Game/L/L.L", "World", {}},
    {"/Game/Z/Z.Z", "Texture2D", {}},
};
const SyntheticRedirect SelectionRedirects[] = {{"/Game/Old/SM.SM", "/Game/D/SM.SM"}};

bool SameSelection(const Array<uint8>& selection, Span<const uint8> expected) noexcept {
    if (selection.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (selection[index] != expected[index]) {
            return false;
        }
    }

    return true;
}

} // namespace

// --only 닫힘: 빈 목록은 전부, redirect 옛 경로, Hard만 따라감(Soft·범위 밖 대상 제외), 범위 밖
// 에셋 직접 선택, 중복 값, 없는 값(NotFound·번호·빈 결과).
TEST(UnrealExchange_SelectClosure) {
    SystemAllocator allocator;
    {
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(SelectionAssets), bundle,
                              diagnostics, Span<const SyntheticRedirect>(SelectionRedirects)));
        Array<uint8> selection(allocator);
        REQUIRE(SelectExchangeClosure(bundle, {}, selection));
        const uint8 all[] = {1, 1, 1, 1, 1, 1};
        CHECK(SameSelection(selection, Span<const uint8>(all)));

        const StringView oldMesh[] = {"/Game/Old/SM.SM"};
        REQUIRE(SelectExchangeClosure(bundle, Span<const StringView>(oldMesh), selection));
        const uint8 meshClosure[] = {1, 1, 0, 1, 0, 0};
        CHECK(SameSelection(selection, Span<const uint8>(meshClosure)));

        const StringView level[] = {"/Game/L/L.L", "/Game/L/L.L"};
        REQUIRE(SelectExchangeClosure(bundle, Span<const StringView>(level), selection));
        const uint8 levelOnly[] = {0, 0, 0, 0, 1, 0};
        CHECK(SameSelection(selection, Span<const uint8>(levelOnly)));

        const StringView missingValue[] = {"/Game/B/T.T", "/Game/Nope.Nope"};
        uint32 missing = ExchangeNoIndex;
        auto selected = SelectExchangeClosure(bundle, Span<const StringView>(missingValue),
                                              selection, &missing);
        CHECK(selected.Error() == ErrorCode::NotFound && missing == 1 && selection.IsEmpty());
        CHECK(SelectExchangeClosure(bundle, Span<const StringView>(missingValue), selection)
                  .Error() == ErrorCode::NotFound);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 읽기에 실패한 에셋은 고르지만 의존을 따라가지 않는다(의존 목록이 없다).
TEST(UnrealExchange_SelectClosureFailedAsset) {
    SystemAllocator allocator;
    {
        const SyntheticDependency onTexture[] = {{"/Game/B/T.T", "Texture2D"}};
        const SyntheticAsset assets[] = {
            {"/Game/A/M.M", "Material", Span<const SyntheticDependency>(onTexture), "[]"},
            {"/Game/B/T.T", "Texture2D", {}},
        };
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), bundle, diagnostics));
        REQUIRE(bundle.Assets()[0].state == ExchangeAssetState::Failed);
        Array<uint8> selection(allocator);
        const StringView only[] = {"/Game/A/M.M"};
        REQUIRE(SelectExchangeClosure(bundle, Span<const StringView>(only), selection));
        const uint8 expected[] = {1, 0};
        CHECK(SameSelection(selection, Span<const uint8>(expected)));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 선택한 계획: 고르지 않은 에셋은 단위가 없고 진단이 없지만 대응 경로는 전체 계획과 같다. 고르지
// 않은 에셋과 겹치는 고른 에셋은 실패한다(진단은 고른 쪽만). 크기가 다르거나 닫힘이 아닌 선택은
// InvalidArgument.
TEST(UnrealExchange_PlanSelection) {
    SystemAllocator allocator;
    {
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(SelectionAssets), bundle,
                              diagnostics, Span<const SyntheticRedirect>(SelectionRedirects)));
        ExchangeImportPlan full(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, full, diagnostics));

        Array<uint8> selection(allocator);
        const StringView oldMesh[] = {"/Game/Old/SM.SM"};
        REQUIRE(SelectExchangeClosure(bundle, Span<const StringView>(oldMesh), selection));
        ExchangeOrderOptions options;
        options.selection = selection.View();
        ExchangeImportPlan plan(allocator);
        ExchangeDiagnostics planned(allocator);
        REQUIRE(PlanExchangeImport(bundle, options, plan, planned));
        CHECK(plan.Units().Size() == 3);
        for (uint32 asset = 0; asset < bundle.Assets().Size(); ++asset) {
            CHECK(plan.MappedPath(asset) == full.MappedPath(asset));
            CHECK(plan.Selected(asset) == (selection[asset] != 0));
            const bool inUnit = selection[asset] != 0 && asset != 4;
            CHECK((plan.UnitOf(asset) != ExchangeNoIndex) == inUnit);
        }

        CHECK(full.Selected(2) && !plan.Selected(2) && !plan.Selected(ExchangeNoIndex));
        CHECK(planned.Items().IsEmpty());

        // 크기가 다른 선택.
        const uint8 shortSelection[] = {1, 1};
        options.selection = Span<const uint8>(shortSelection);
        CHECK(PlanExchangeImport(bundle, options, plan, planned).Error() ==
                  ErrorCode::InvalidArgument &&
              plan.Units().IsEmpty());
        // 닫힘이 아닌 선택(메시만 — 머티리얼이 빠졌다).
        const uint8 meshOnly[] = {0, 0, 0, 1, 0, 0};
        options.selection = Span<const uint8>(meshOnly);
        CHECK(PlanExchangeImport(bundle, options, plan, planned).Error() ==
                  ErrorCode::InvalidArgument &&
              plan.Units().IsEmpty());
    }

    {
        // 대소문자만 다른 두 경로(겹침)와 이름이 바뀌는 경로: 고른 것만 진단.
        const SyntheticAsset assets[] = {
            {"/Game/My Props/T.T", "Texture2D", {}},
            {"/Game/X/Tex.Tex", "Texture2D", {}},
            {"/Game/x/tex.tex", "Texture2D", {}},
        };
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), bundle, diagnostics));
        const uint8 selection[] = {0, 1, 0};
        ExchangeOrderOptions options;
        options.selection = Span<const uint8>(selection);
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, options, plan, diagnostics));
        REQUIRE(plan.Units().Size() == 1);
        CHECK(plan.Units()[0].state == ExchangeUnitState::Failed &&
              plan.Units()[0].failure == AssetDiagnosticCode::UnrealPathCollision);
        CHECK(diagnostics.Items().Size() == 1 && diagnostics.Items()[0].asset == 1 &&
              diagnostics.Items()[0].related == 2);
        CHECK(plan.Renamed(0) && CountIssue(diagnostics, ExchangeIssue::Renamed, 0) == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 기존 에셋: 찾으면 대상 경로는 그 경로(대응 경로는 그대로 보임), 길이 초과·겹침에서 빠진다.
// 모호하면 UnrealPathCollision(AmbiguousExisting)과 전파, 묶음에 없는 대상이 모호하면 텍스처라도
// MissingDependency. 옛 경로 목록 전달, 조회 오류.
TEST(UnrealExchange_PlanExisting) {
    SystemAllocator allocator;
    {
        // 256 byte를 넘는 대응 경로를 만드는 긴 경로다(구성 요소는 64 byte 안).
        constexpr StringView LongPath =
            "/Game/D/Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/Long.Long";
        const SyntheticDependency onSkeleton[] = {{"/Game/B/SKEL.SKEL", "Skeleton"}};
        const SyntheticDependency onExternal[] = {{"/Game/Ext/T.T", "Texture2D"}};
        const SyntheticDependency onDuplicate[] = {{"/Game/E/Dup.Dup", "Texture2D"}};
        const SyntheticAsset assets[] = {
            {"/Game/A/Clip.Clip", "AnimSequence", Span<const SyntheticDependency>(onSkeleton)},
            {"/Game/B/SKEL.SKEL", "Skeleton", {}},
            {"/Game/C/SM.SM", "StaticMesh", Span<const SyntheticDependency>(onExternal)},
            {LongPath, "Texture2D", {}},
            {"/Game/E/Dup.Dup", "Texture2D", {}},
            {"/Game/F/User.User", "Material", Span<const SyntheticDependency>(onDuplicate)},
            {"/Game/G/Same.Same", "Texture2D", {}},
            {"/Game/g/same.same", "Texture2D", {}},
        };
        const SyntheticRedirect redirects[] = {{"/Game/Old/A.A", "/Game/B/SKEL.SKEL"},
                                               {"/Game/Old/B.B", "/Game/B/SKEL.SKEL"},
                                               {"/Game/Old/C.C", "/Game/Z/Other.Other"}};
        const TableLookup::Row rows[] = {
            {"/Game/B/SKEL.SKEL", ExchangeExistingState::Found, 7, "/Game/Rocks/Skeleton"},
            {"/Game/Ext/T.T", ExchangeExistingState::Ambiguous, 0, {}},
            {LongPath, ExchangeExistingState::Found, 9, "/Game/Kept"},
            {"/Game/E/Dup.Dup", ExchangeExistingState::Ambiguous, 0, {}},
            {"/Game/G/Same.Same", ExchangeExistingState::Found, 11, "/Game/SameKept"},
        };
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), bundle, diagnostics,
                              Span<const SyntheticRedirect>(redirects)));
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        lookup.watched = "/Game/B/SKEL.SKEL";
        ExchangeOrderOptions options;
        options.existing = &lookup;
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, options, plan, diagnostics));

        // 옛 경로는 그 objectPath로 이어지는 redirect들(from 순)이다.
        CHECK(lookup.watchedCalls == 1 && lookup.watchedFormerCount == 2 &&
              lookup.watchedFormerSorted);
        const ExchangeExistingMatch skeleton = plan.Existing(1);
        CHECK(skeleton.state == ExchangeExistingState::Found &&
              skeleton.id == (AssetId{Guid{0xE1, 7}}) && skeleton.path == "/Game/Rocks/Skeleton");
        CHECK(plan.TargetPath(1) == "/Game/Rocks/Skeleton" &&
              plan.MappedPath(1) == "/Game/Unreal/Game/B/SKEL");
        CHECK(plan.Existing(0).state == ExchangeExistingState::None &&
              plan.TargetPath(0) == plan.MappedPath(0));
        CHECK(plan.Units()[plan.UnitOf(0)].state == ExchangeUnitState::Ready);

        // 묶음에 없는 모호한 텍스처 대상: 경고가 아니라 실패.
        const ExchangeImportUnit& mesh = plan.Units()[plan.UnitOf(2)];
        CHECK(mesh.state == ExchangeUnitState::Failed &&
              mesh.failure == AssetDiagnosticCode::MissingDependency);
        CHECK(CountIssue(diagnostics, ExchangeIssue::AmbiguousExisting, 2) == 1 &&
              diagnostics.Count(AssetDiagnosticCode::MissingUnrealDependency) == 0);

        // 긴 경로: 기존 에셋이 있어 길이 초과로 실패하지 않는다.
        CHECK(plan.MappedPath(3).IsEmpty() && plan.TargetPath(3) == "/Game/Kept");
        CHECK(plan.Units()[plan.UnitOf(3)].state == ExchangeUnitState::Ready &&
              diagnostics.Count(AssetDiagnosticCode::UnrealPathTooLong) == 0);

        // 모호한 기존 에셋과 그 의존자.
        const ExchangeImportUnit& duplicate = plan.Units()[plan.UnitOf(4)];
        CHECK(duplicate.state == ExchangeUnitState::Failed &&
              duplicate.failure == AssetDiagnosticCode::UnrealPathCollision &&
              duplicate.failureAsset == 4 &&
              plan.Existing(4).state == ExchangeExistingState::Ambiguous &&
              plan.Existing(4).path.IsEmpty());
        CHECK(CountIssue(diagnostics, ExchangeIssue::AmbiguousExisting, 4) == 1);
        const ExchangeImportUnit& user = plan.Units()[plan.UnitOf(5)];
        CHECK(user.state == ExchangeUnitState::Failed &&
              user.failure == AssetDiagnosticCode::MissingDependency && user.failureAsset == 4);

        // 기존 에셋을 쓰는 쪽은 겹침에서 빠진다.
        CHECK(plan.Units()[plan.UnitOf(6)].state == ExchangeUnitState::Ready &&
              plan.Units()[plan.UnitOf(7)].state == ExchangeUnitState::Ready &&
              diagnostics.Count(AssetDiagnosticCode::UnrealPathCollision) == 2);

        // 범위 밖 번호.
        CHECK(plan.Existing(ExchangeNoIndex).state == ExchangeExistingState::None);

        // 조회 오류는 계획 전체의 오류다.
        lookup.failWith = ErrorCode::Busy;
        CHECK(PlanExchangeImport(bundle, options, plan, diagnostics).Error() == ErrorCode::Busy &&
              plan.Units().IsEmpty());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 기존 에셋이 있는 묶음에서 의존 대상 조회 오류(묶음에 없는 대상만 조회 오류를 낸다).
TEST(UnrealExchange_PlanExistingTargetLookupError) {
    SystemAllocator allocator;
    {
        // 첫 조회(에셋)는 성공하고 두 번째(묶음에 없는 대상)에서 실패하는 대역이다.
        class SecondFails final : public ExchangeExistingAssets {
        public:
            Result<ExchangeExistingMatch>
            Find(StringView objectPath,
                 Span<const StringView> formerPaths) const noexcept override {
                (void)objectPath;
                (void)formerPaths;
                calls += 1;
                if (calls == 2) {
                    return Result<ExchangeExistingMatch>::Failure(ErrorCode::Busy);
                }

                return Result<ExchangeExistingMatch>::Success(ExchangeExistingMatch{});
            }

            Result<bool> PathTaken(StringView logicalPath) const noexcept override {
                (void)logicalPath;
                return Result<bool>::Success(false);
            }

            mutable uint32 calls = 0;
        };

        const SyntheticDependency onExternal[] = {{"/Game/Ext/T.T", "Texture2D"}};
        const SyntheticAsset assets[] = {
            {"/Game/A/M.M", "Material", Span<const SyntheticDependency>(onExternal)}};
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), bundle, diagnostics));
        SecondFails lookup;
        ExchangeOrderOptions options;
        options.existing = &lookup;
        ExchangeImportPlan plan(allocator);
        CHECK(PlanExchangeImport(bundle, options, plan, diagnostics).Error() == ErrorCode::Busy);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당 실패: 선택과 기존 에셋 조회를 쓰는 계획의 모든 할당 지점에서 오류이고 새는 것이 없다.
TEST(UnrealExchange_SelectionAllocationFailure) {
    SystemAllocator allocator;
    {
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(SelectionAssets), bundle,
                              diagnostics, Span<const SyntheticRedirect>(SelectionRedirects)));
        const TableLookup::Row rows[] = {
            {"/Game/D/SM.SM", ExchangeExistingState::Found, 3, "/Game/Kept"}};
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        const StringView oldMesh[] = {"/Game/Old/SM.SM"};
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            failing.FailOnAttempt(attempt);
            Array<uint8> selection(failing);
            auto selected =
                SelectExchangeClosure(bundle, Span<const StringView>(oldMesh), selection);
            if (!selected) {
                CHECK(selected.Error() == ErrorCode::OutOfMemory && selection.IsEmpty());
                ++failures;
                continue;
            }

            ExchangeOrderOptions options;
            options.existing = &lookup;
            options.selection = selection.View();
            ExchangeImportPlan plan(failing);
            ExchangeDiagnostics quiet(allocator);
            auto planned = PlanExchangeImport(bundle, options, plan, quiet);
            if (!planned) {
                CHECK(planned.Error() == ErrorCode::OutOfMemory && plan.Units().IsEmpty());
                ++failures;
                continue;
            }

            CHECK(plan.TargetPath(3) == "/Game/Kept");
            break;
        }

        CHECK(failures > 3);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 묶음 에셋 둘 이상이 같은 기존 에셋을 찾으면: 첫 단계로 찾은 하나만 재임포트하고 둘째 단계로 찾은
// 쪽은 새 에셋, 첫 단계가 없거나 둘이면 모두 모호함. 새 에셋 경로가 기존 에셋에 다시 임포트하는
// 경로와 겹치면 새 에셋만 실패, 다른 Rocks 에셋이 쓰는 경로면 PathTaken. 고르지 않은 에셋은 진단이
// 없다. 점유 조회 오류는 계획의 오류다.
TEST(UnrealExchange_PlanExistingConflicts) {
    SystemAllocator allocator;
    {
        constexpr StringView LongPath =
            "/Game/L/Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/"
            "Directory0123456789012345678901234567890123456789/Long.Long";
        const SyntheticDependency onAmbiguous[] = {{"/Game/Ext/X.X", "Texture2D"}};
        const SyntheticAsset assets[] = {
            {"/Game/A/A.A", "Texture2D", {}}, // 0: 첫 단계 — 재임포트
            {"/Game/B/B.B", "Texture2D", {}}, // 1: 둘째 단계, 같은 id — 새 에셋
            {"/Game/C/C.C", "Texture2D", {}}, // 2·3: 둘 다 둘째 단계 — 모호
            {"/Game/D/D.D", "Texture2D", {}},
            {"/Game/E/E.E", "Texture2D", {}}, // 4·5: 둘 다 첫 단계 — 모호
            {"/Game/F/F.F", "Texture2D", {}},
            {"/Game/G/G.G", "Texture2D", {}}, // 6: 새 에셋, 7의 기존 경로와 겹침
            {"/Game/H/H.H", "Texture2D", {}}, // 7: 기존 /Game/Unreal/Game/G/g
            {"/Game/I/I.I", "Texture2D", {}}, // 8: 대응 경로를 다른 Rocks 에셋이 씀
            {LongPath, "Texture2D", {}},      // 9: 고르지 않은 길이 초과
            {"/Game/M/M.M", "Texture2D", {}}, // 10: 고르지 않은 모호함
            {"/Game/N/N.N", "Texture2D", {}}, // 11: 기존 /Game/Unreal/Game/P/p(겹친 새 에셋보다 앞)
            {"/Game/P/P.P", "Texture2D", {}}, // 12: 새 에셋, 11의 기존 경로와 겹침
            {"/Game/Q/Q.Q", "Texture2D", {}}, // 13: 고르지 않은 점유 경로
            {"/Game/R/R.R", "Material", Span<const SyntheticDependency>(onAmbiguous)}, // 14
            {"/Game/S/S.S", "Texture2D", {}}, // 15: high가 다른 기존 id
        };
        const TableLookup::Row rows[] = {
            {"/Game/A/A.A", ExchangeExistingState::Found, 5, "/Game/X"},
            {"/Game/B/B.B", ExchangeExistingState::Found, 5, "/Game/X"},
            {"/Game/C/C.C", ExchangeExistingState::Found, 6, "/Game/Y"},
            {"/Game/D/D.D", ExchangeExistingState::Found, 6, "/Game/Y"},
            {"/Game/E/E.E", ExchangeExistingState::Found, 7, "/Game/Z"},
            {"/Game/F/F.F", ExchangeExistingState::Found, 7, "/Game/Z"},
            {"/Game/H/H.H", ExchangeExistingState::Found, 9, "/Game/Unreal/Game/G/g"},
            {"/Game/M/M.M", ExchangeExistingState::Ambiguous, 0, {}},
            {"/Game/N/N.N", ExchangeExistingState::Found, 10, "/Game/Unreal/Game/P/p"},
            {"/Game/Ext/X.X", ExchangeExistingState::Ambiguous, 0, {}},
            {"/Game/S/S.S", ExchangeExistingState::Found, 150, "/Game/HighKept"},
        };
        const StringView formerMatches[] = {"/Game/B/B.B", "/Game/C/C.C", "/Game/D/D.D"};
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), bundle, diagnostics));
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        lookup.formerMatches = Span<const StringView>(formerMatches);
        lookup.takenPath = "/Game/Unreal/Game/I/I";
        lookup.otherTakenPath = "/Game/Unreal/Game/Q/Q";
        const uint8 selection[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1};
        ExchangeOrderOptions options;
        options.existing = &lookup;
        options.selection = Span<const uint8>(selection);
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, options, plan, diagnostics));
        auto unitOf = [&](uint32 asset) noexcept -> const ExchangeImportUnit& {
            return plan.Units()[plan.UnitOf(asset)];
        };

        CHECK(plan.Existing(0).state == ExchangeExistingState::Found &&
              plan.TargetPath(0) == "/Game/X" && unitOf(0).state == ExchangeUnitState::Ready);
        CHECK(plan.Existing(1).state == ExchangeExistingState::None &&
              plan.TargetPath(1) == "/Game/Unreal/Game/B/B" &&
              unitOf(1).state == ExchangeUnitState::Ready);
        const uint32 ambiguous[] = {2, 3, 4, 5};
        for (const uint32 asset : ambiguous) {
            CHECK(plan.Existing(asset).state == ExchangeExistingState::Ambiguous &&
                  unitOf(asset).failure == AssetDiagnosticCode::UnrealPathCollision &&
                  CountIssue(diagnostics, ExchangeIssue::AmbiguousExisting, asset) == 1);
        }

        CHECK(unitOf(6).failure == AssetDiagnosticCode::UnrealPathCollision &&
              unitOf(7).state == ExchangeUnitState::Ready);
        bool related = false;
        for (const ExchangeDiagnostic& item : diagnostics.Items()) {
            related = related || (item.issue == ExchangeIssue::PathCollision && item.asset == 6 &&
                                  item.related == 7);
        }

        CHECK(related && CountIssue(diagnostics, ExchangeIssue::PathCollision, 7) == 0);
        CHECK(unitOf(8).failure == AssetDiagnosticCode::UnrealPathCollision &&
              CountIssue(diagnostics, ExchangeIssue::PathTaken, 8) == 1);
        CHECK(unitOf(12).failure == AssetDiagnosticCode::UnrealPathCollision &&
              unitOf(11).state == ExchangeUnitState::Ready);
        CHECK(unitOf(14).failure == AssetDiagnosticCode::MissingDependency &&
              CountIssue(diagnostics, ExchangeIssue::AmbiguousExisting, 14) == 1);
        CHECK(plan.Existing(15).state == ExchangeExistingState::Found &&
              plan.TargetPath(15) == "/Game/HighKept");
        // 고르지 않은 에셋: 단위·진단 없음.
        CHECK(plan.UnitOf(9) == ExchangeNoIndex && plan.UnitOf(10) == ExchangeNoIndex &&
              CountIssue(diagnostics, ExchangeIssue::PathTooLong, 9) == 0 &&
              CountIssue(diagnostics, ExchangeIssue::AmbiguousExisting, 10) == 0 &&
              plan.UnitOf(13) == ExchangeNoIndex &&
              CountIssue(diagnostics, ExchangeIssue::PathTaken, 13) == 0);

        const usize unitCount = plan.Units().Size();
        lookup.pathFailWith = ErrorCode::Busy;
        CHECK(PlanExchangeImport(bundle, options, plan, diagnostics).Error() == ErrorCode::Busy);
        lookup.pathFailWith = ErrorCode::None;

        // 진단 할당 실패: 진단을 내는 모든 자리에서 OutOfMemory이고, 그 뒤 같은 결과다.
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            failing.FailOnAttempt(attempt);
            ExchangeDiagnostics noisy(failing);
            ExchangeImportPlan again(allocator);
            auto planned = PlanExchangeImport(bundle, options, again, noisy);
            if (!planned) {
                CHECK(planned.Error() == ErrorCode::OutOfMemory);
                ++failures;
                continue;
            }

            CHECK(again.Units().Size() == unitCount);
            break;
        }

        CHECK(failures > 3);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// --only 계획은 고른 에셋마다 전체 계획과 같은 대상 경로·기존 에셋·단위 상태·실패다(기존 에셋
// 조회를 켜고, 고르지 않은 모호한 에셋이 있어도).
TEST(UnrealExchange_PlanSelectionMatchesFull) {
    SystemAllocator allocator;
    {
        const SyntheticDependency onTexture[] = {{"/Game/B/T.T", "Texture2D"}};
        const SyntheticDependency onExternal[] = {{"/Game/Ext/SKEL.SKEL", "Skeleton"}};
        const SyntheticAsset assets[] = {
            {"/Game/A/M.M", "Material", Span<const SyntheticDependency>(onTexture)},
            {"/Game/B/T.T", "Texture2D", {}},
            {"/Game/C/Clip.Clip", "AnimSequence", Span<const SyntheticDependency>(onExternal)},
            {"/Game/D/Dup.Dup", "Texture2D", {}},
            {"/Game/E/Old.Old", "Texture2D", {}},
        };
        const TableLookup::Row rows[] = {
            {"/Game/B/T.T", ExchangeExistingState::Found, 4, "/Game/Kept/T"},
            {"/Game/D/Dup.Dup", ExchangeExistingState::Ambiguous, 0, {}},
            {"/Game/E/Old.Old", ExchangeExistingState::Found, 4, "/Game/Kept/T"},
        };
        const StringView formerMatches[] = {"/Game/E/Old.Old"};
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), bundle, diagnostics));
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        lookup.formerMatches = Span<const StringView>(formerMatches);
        ExchangeOrderOptions options;
        options.existing = &lookup;
        ExchangeImportPlan full(allocator);
        REQUIRE(PlanExchangeImport(bundle, options, full, diagnostics));
        const StringView onlyValues[] = {"/Game/A/M.M", "/Game/C/Clip.Clip", "/Game/E/Old.Old"};
        for (const StringView only : onlyValues) {
            Array<uint8> selection(allocator);
            REQUIRE(SelectExchangeClosure(bundle, Span<const StringView>(&only, 1), selection));
            options.selection = selection.View();
            ExchangeImportPlan plan(allocator);
            ExchangeDiagnostics quiet(allocator);
            REQUIRE(PlanExchangeImport(bundle, options, plan, quiet));
            for (uint32 asset = 0; asset < bundle.Assets().Size(); ++asset) {
                if (selection[asset] == 0) {
                    continue;
                }

                const ExchangeImportUnit& mine = plan.Units()[plan.UnitOf(asset)];
                const ExchangeImportUnit& whole = full.Units()[full.UnitOf(asset)];
                CHECK(plan.TargetPath(asset) == full.TargetPath(asset));
                CHECK(plan.Existing(asset).state == full.Existing(asset).state &&
                      plan.Existing(asset).id == full.Existing(asset).id);
                CHECK(mine.state == whole.state && mine.failure == whole.failure &&
                      mine.depth == whole.depth);
            }
        }

        // 둘째 단계로 같은 에셋을 찾은 Old는 새 에셋이다(T가 첫 단계).
        CHECK(full.Existing(4).state == ExchangeExistingState::None &&
              full.Existing(1).state == ExchangeExistingState::Found);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 묶음 밖 대상 id(10.6절 4번): 계획이 찾은 묶음 밖 Hard 대상만 의존 번호로 남고, 묶음 안
// 대상·모호한 대상·못 찾은 대상·Soft·범위 밖 클래스·의존 없는 에셋·범위 밖 번호는 null이다. 다시
// 계획하면 새 값이다.
TEST(UnrealExchange_PlanExternalTarget) {
    SystemAllocator allocator;
    {
        const SyntheticDependency onTextures[] = {
            {"/Game/B/T.T", "Texture2D"},
            {"/Game/Ext/Amb.Amb", "Texture2D"},
            {"/Game/Ext/Found.Found", "Texture2D"},
            {"/Game/Ext/Missing.Missing", "Texture2D"},
            {"/Game/Ext/Soft.Soft", "Texture2D", true},
            {"/Game/Ext/World.World", "World"},
        };
        const SyntheticAsset assets[] = {
            {"/Game/A/M.M", "Material", Span<const SyntheticDependency>(onTextures)},
            {"/Game/B/T.T", "Texture2D", {}},
        };
        const TableLookup::Row rows[] = {
            {"/Game/Ext/Amb.Amb", ExchangeExistingState::Ambiguous, 0, {}},
            {"/Game/Ext/Found.Found", ExchangeExistingState::Found, 5, "/Game/Rocks/Found"},
            {"/Game/Ext/Soft.Soft", ExchangeExistingState::Found, 6, "/Game/Rocks/Soft"},
            {"/Game/Ext/World.World", ExchangeExistingState::Found, 8, "/Game/Rocks/World"},
        };
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadSynthetic(allocator, Span<const SyntheticAsset>(assets), bundle, diagnostics));
        TableLookup lookup{Span<const TableLookup::Row>(rows)};
        ExchangeOrderOptions options;
        options.existing = &lookup;
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, options, plan, diagnostics));
        // 모호한 대상은 단위를 실패시키지만 계획의 다른 칸은 그대로다.
        CHECK(plan.ExternalTarget(0, 0).IsNull() && plan.ExternalTarget(0, 1).IsNull());
        CHECK(plan.ExternalTarget(0, 2) == (AssetId{Guid{0xE1, 5}}));
        CHECK(plan.ExternalTarget(0, 3).IsNull() && plan.ExternalTarget(0, 4).IsNull() &&
              plan.ExternalTarget(0, 5).IsNull() && plan.ExternalTarget(0, 6).IsNull());
        CHECK(plan.ExternalTarget(1, 0).IsNull() && plan.ExternalTarget(2, 0).IsNull() &&
              plan.ExternalTarget(ExchangeNoIndex, 0).IsNull());
        CHECK(diagnostics.Count(AssetDiagnosticCode::MissingUnrealDependency) == 1);

        // 조회 없이 다시 계획하면 묶음 밖 대상은 모두 null이다(앞 값을 남기지 않는다).
        ExchangeOrderOptions plain;
        REQUIRE(PlanExchangeImport(bundle, plain, plan, diagnostics));
        CHECK(plan.ExternalTarget(0, 2).IsNull());

        // 고르지 않은 에셋의 의존은 보지 않는다.
        const uint8 onlyTexture[] = {0, 1};
        options.selection = Span<const uint8>(onlyTexture);
        REQUIRE(PlanExchangeImport(bundle, options, plan, diagnostics));
        CHECK(plan.ExternalTarget(0, 2).IsNull());
        plan.Clear();
        CHECK(plan.ExternalTarget(0, 2).IsNull());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_selection_tests_file
