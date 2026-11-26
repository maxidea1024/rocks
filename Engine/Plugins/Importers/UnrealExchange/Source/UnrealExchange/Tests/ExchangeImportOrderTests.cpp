#include "ExchangeTestSupport.h"

#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// Unreal 교환 임포트 순서 시험이다(Docs/Design/UnrealExchange 9.1·9.3절, 10.4절 13–15번):
// fixture 순서(깊이·SCC·redirect·누락 경고·이름 바꿈), Build 순환, Hard 순환 허용, 누락 의존 분류와
// 기존 에셋 조회, 깊이 상한, 실패 전파, 경로 겹침·길이, 잘못된 root, 결정성, 할당 실패.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_import_order_tests_file {

// 합성 묶음을 읽고 순서를 계산한다.
bool PlanSynthetic(Allocator& allocator, Span<const SyntheticAsset> assets,
                   const ExchangeOrderOptions& options, ExchangeBundle& bundle,
                   ExchangeImportPlan& plan, ExchangeDiagnostics& diagnostics,
                   Span<const SyntheticRedirect> redirects = {}) noexcept {
    MemoryFiles files(allocator);
    return WriteSyntheticBundle(allocator, files, assets, redirects) &&
           ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics) &&
           PlanExchangeImport(bundle, options, plan, diagnostics);
}

// asset이 속한 단위다.
const ExchangeImportUnit& UnitOfAsset(const ExchangeImportPlan& plan, uint32 asset) noexcept {
    return plan.Units()[plan.UnitOf(asset)];
}

// (code, issue, asset, related) 진단이 있는지 본다.
bool HasDiagnostic(const ExchangeDiagnostics& diagnostics, AssetDiagnosticCode code,
                   ExchangeIssue issue, uint32 asset, uint32 related = ExchangeNoIndex) noexcept {
    for (const ExchangeDiagnostic& item : diagnostics.Items()) {
        if (item.code == code && item.issue == issue && item.asset == asset &&
            item.related == related) {
            return true;
        }
    }

    return false;
}

void AppendSuffix(StringView prefix, StringBuilderBase& out) noexcept {
    const Sha256Digest digest = Sha256::Hash(
        Span<const uint8>(reinterpret_cast<const uint8*>(prefix.Data()), prefix.ByteCount()));
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    (void)out.TryAppendAscii('-');
    (void)out.TryAppend(StringView(hex, UnrealPathHashChars));
}

// fixture 순서: 깊이 0(스켈레톤·함수·텍스처 둘) → 머티리얼 → 인스턴스(Build) → 스킨 메시·Hard 순환
// 메시 둘(redirect로 인스턴스를 가리킴). 모두 Ready, 진단은 이름 바꿈 정보와 누락 텍스처
// 경고뿐이다.
TEST(UnrealExchange_PlanFixture) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        diagnostics.Clear();
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        REQUIRE(plan.Units().Size() == 8);
        const uint32 expectedFirst[] = {0, 2, 8, 9, 5, 4, 1, 6};
        const uint32 expectedDepth[] = {0, 0, 0, 0, 1, 2, 3, 3};
        for (usize index = 0; index < plan.Units().Size(); ++index) {
            const ExchangeImportUnit& unit = plan.Units()[index];
            CHECK(plan.Members(unit)[0] == expectedFirst[index]);
            CHECK(unit.depth == expectedDepth[index]);
            // 메시 둘(SM_Rock ↔ SM_Rock_Collision)은 Hard 순환이라 실패한다(10.5절 8번 — UXR11 전).
            if (index == 7) {
                CHECK(unit.state == ExchangeUnitState::Failed &&
                      unit.failure == AssetDiagnosticCode::UnsupportedCycle &&
                      unit.failureAsset == 6);
            } else {
                CHECK(unit.state == ExchangeUnitState::Ready &&
                      unit.failure == AssetDiagnosticCode::None &&
                      unit.failureAsset == ExchangeNoIndex);
            }

            for (const uint32 member : plan.Members(unit)) {
                CHECK(plan.UnitOf(member) == index);
            }
        }

        const ExchangeImportUnit& meshes = plan.Units()[7];
        REQUIRE(meshes.memberCount == 2);
        CHECK(plan.Members(meshes)[1] == 7);
        CHECK(plan.UnitOf(3) == ExchangeNoIndex && plan.MappedPath(3).IsEmpty());
        CHECK(plan.UnitOf(1000) == ExchangeNoIndex && plan.MappedPath(1000).IsEmpty() &&
              !plan.Renamed(1000));
        CHECK(plan.MappedPath(9) == StringView("/Game/Unreal/Game/Textures/T_Rock"));
        CHECK(!plan.Renamed(9));
        StringBuilder<256> shared(allocator);
        REQUIRE(shared.TryAppend("/Game/Unreal/Game/My_Props"));
        AppendSuffix("/Game/My Props", shared);
        REQUIRE(shared.TryAppend("/U_Shared"));
        AppendSuffix("/Game/My Props/_Shared", shared);
        CHECK(plan.MappedPath(8) == shared.View() && plan.Renamed(8));
        REQUIRE(diagnostics.Items().Size() == 4);
        CHECK(diagnostics.Items()[2].issue == ExchangeIssue::HardCycle &&
              diagnostics.Items()[2].asset == 6 && diagnostics.Items()[3].asset == 7 &&
              diagnostics.Items()[3].code == AssetDiagnosticCode::UnsupportedCycle);
        CHECK(diagnostics.Items()[0].code == AssetDiagnosticCode::UnrealPathRenamed &&
              diagnostics.Items()[0].severity == ExchangeSeverity::Info &&
              diagnostics.Items()[0].asset == 8 &&
              diagnostics.Items()[0].text.View() == shared.View());
        CHECK(diagnostics.Items()[1].code == AssetDiagnosticCode::MissingUnrealDependency &&
              diagnostics.Items()[1].severity == ExchangeSeverity::Warning &&
              diagnostics.Items()[1].asset == 5 &&
              diagnostics.Items()[1].text.View() ==
                  StringView("/Game/Textures/T_Missing.T_Missing"));

        // 다른 root, 같은 결과 순서. 두 번 계산해도 같다.
        ExchangeOrderOptions options;
        options.root = "/Game/Imported";
        ExchangeImportPlan again(allocator);
        ExchangeDiagnostics quiet(allocator);
        REQUIRE(PlanExchangeImport(bundle, options, again, quiet));
        CHECK(again.MappedPath(9) == StringView("/Game/Imported/Game/Textures/T_Rock"));
        REQUIRE(again.Units().Size() == plan.Units().Size());
        for (usize index = 0; index < plan.Units().Size(); ++index) {
            CHECK(again.Units()[index].firstMember == plan.Units()[index].firstMember ||
                  plan.Members(plan.Units()[index])[0] == again.Members(again.Units()[index])[0]);
        }

        plan.Clear();
        CHECK(plan.Units().IsEmpty() && plan.MappedPath(0).IsEmpty());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Build 참조가 낀 순환(인스턴스 부모 사슬, 함수 자기 호출, 머티리얼↔함수)은 UnsupportedCycle이고,
// 그것을 Hard로 참조하는 단위는 MissingDependency다. Hard만의 순환·자기 참조는 허용한다.
TEST(UnrealExchange_PlanCycles) {
    SystemAllocator allocator;
    {
        const SyntheticDependency toB[] = {{"/Game/M/MI_B.MI_B", "MaterialInstanceConstant"}};
        const SyntheticDependency toA[] = {{"/Game/M/MI_A.MI_A", "MaterialInstanceConstant"}};
        const SyntheticDependency self[] = {{"/Game/F/MF.MF", "MaterialFunction"}};
        const SyntheticDependency selfMesh[] = {{"/Game/S/Self.Self", "StaticMesh"}};
        const SyntheticAsset assets[] = {
            {"/Game/F/MF.MF", "MaterialFunction", Span<const SyntheticDependency>(self)},
            {"/Game/M/MI_A.MI_A", "MaterialInstanceConstant", Span<const SyntheticDependency>(toB)},
            {"/Game/M/MI_B.MI_B", "MaterialInstanceConstant", Span<const SyntheticDependency>(toA)},
            {"/Game/S/SM.SM", "StaticMesh", Span<const SyntheticDependency>(toA)},
            {"/Game/S/Self.Self", "StaticMesh", Span<const SyntheticDependency>(selfMesh)},
        };
        ExchangeBundle bundle(allocator);
        ExchangeImportPlan plan(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(PlanSynthetic(allocator, Span<const SyntheticAsset>(assets), {}, bundle, plan,
                              diagnostics));
        CHECK(UnitOfAsset(plan, 0).state == ExchangeUnitState::Failed &&
              UnitOfAsset(plan, 0).failure == AssetDiagnosticCode::UnsupportedCycle);
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::UnsupportedCycle,
                            ExchangeIssue::BuildCycle, 0));
        const ExchangeImportUnit& pair = UnitOfAsset(plan, 1);
        CHECK(pair.memberCount == 2 && plan.UnitOf(2) == plan.UnitOf(1));
        CHECK(pair.state == ExchangeUnitState::Failed &&
              pair.failure == AssetDiagnosticCode::UnsupportedCycle && pair.failureAsset == 1);
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::UnsupportedCycle,
                            ExchangeIssue::BuildCycle, 1));
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::UnsupportedCycle,
                            ExchangeIssue::BuildCycle, 2));
        const ExchangeImportUnit& dependent = UnitOfAsset(plan, 3);
        CHECK(dependent.state == ExchangeUnitState::Failed &&
              dependent.failure == AssetDiagnosticCode::MissingDependency &&
              dependent.failureAsset == 1 && dependent.depth == 1);
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::MissingDependency,
                            ExchangeIssue::FailedDependency, 3, 1));
        CHECK(UnitOfAsset(plan, 4).state == ExchangeUnitState::Ready &&
              UnitOfAsset(plan, 4).depth == 0);
    }

    {
        // Hard 순환 셋(메시)과 머티리얼↔텍스처 Hard 순환은 한 단위로 묶이고 다중 에셋 원자 저장
        // (UXR11) 전까지 HardCycle로 실패한다. 머티리얼↔함수는 Build 순환.
        const SyntheticDependency meshB[] = {{"/Game/S/B.B", "StaticMesh"}};
        const SyntheticDependency meshC[] = {{"/Game/S/C.C", "StaticMesh"}};
        const SyntheticDependency meshA[] = {{"/Game/S/A.A", "StaticMesh"}};
        const SyntheticDependency toTexture[] = {{"/Game/T/T.T", "Texture2D"}};
        const SyntheticDependency toMaterial[] = {{"/Game/M/M.M", "Material"}};
        const SyntheticDependency toFunction[] = {{"/Game/F/F.F", "MaterialFunction"}};
        const SyntheticDependency toCycleMaterial[] = {{"/Game/M/N.N", "Material"}};
        const SyntheticDependency toCollection[] = {{"/Game/P/P.P", "MaterialParameterCollection"}};
        const SyntheticAsset assets[] = {
            {"/Game/F/F.F", "MaterialFunction", Span<const SyntheticDependency>(toCycleMaterial)},
            {"/Game/M/M.M", "Material", Span<const SyntheticDependency>(toTexture)},
            {"/Game/M/N.N", "Material", Span<const SyntheticDependency>(toFunction)},
            {"/Game/M/Q.Q", "Material", Span<const SyntheticDependency>(toCollection)},
            {"/Game/P/P.P", "MaterialParameterCollection", {}},
            {"/Game/S/A.A", "StaticMesh", Span<const SyntheticDependency>(meshB)},
            {"/Game/S/B.B", "StaticMesh", Span<const SyntheticDependency>(meshC)},
            {"/Game/S/C.C", "StaticMesh", Span<const SyntheticDependency>(meshA)},
            {"/Game/T/T.T", "Texture2D", Span<const SyntheticDependency>(toMaterial)},
        };
        ExchangeBundle bundle(allocator);
        ExchangeImportPlan plan(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(PlanSynthetic(allocator, Span<const SyntheticAsset>(assets), {}, bundle, plan,
                              diagnostics));
        CHECK(UnitOfAsset(plan, 0).state == ExchangeUnitState::Failed &&
              UnitOfAsset(plan, 0).memberCount == 2 && plan.UnitOf(0) == plan.UnitOf(2));
        CHECK(UnitOfAsset(plan, 1).state == ExchangeUnitState::Failed &&
              UnitOfAsset(plan, 1).failure == AssetDiagnosticCode::UnsupportedCycle &&
              UnitOfAsset(plan, 1).memberCount == 2 && plan.UnitOf(1) == plan.UnitOf(8));
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::UnsupportedCycle,
                            ExchangeIssue::HardCycle, 8));
        CHECK(UnitOfAsset(plan, 3).state == ExchangeUnitState::Ready &&
              UnitOfAsset(plan, 3).depth == 1);
        CHECK(UnitOfAsset(plan, 5).state == ExchangeUnitState::Failed &&
              UnitOfAsset(plan, 5).memberCount == 3 && plan.UnitOf(6) == plan.UnitOf(5) &&
              plan.UnitOf(7) == plan.UnitOf(5));
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 7);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 묶음·기존 에셋 조회에 있는 objectPath 목록이다.
class ListedAssets final : public ExchangeExistingAssets {
public:
    explicit ListedAssets(Span<const StringView> paths) noexcept : paths_(paths) {
    }

    Result<ExchangeExistingMatch> Find(StringView objectPath,
                                       Span<const StringView> formerPaths) const noexcept override {
        (void)formerPaths;
        ExchangeExistingMatch match;
        for (const StringView& path : paths_) {
            if (path == objectPath) {
                match.state = ExchangeExistingState::Found;
                match.id = AssetId{Guid{1, 1}};
                match.path = "/Game/Existing";
            }
        }

        return Result<ExchangeExistingMatch>::Success(match);
    }

    Result<bool> PathTaken(StringView logicalPath) const noexcept override {
        (void)logicalPath;
        return Result<bool>::Success(false);
    }

private:
    Span<const StringView> paths_;
};

// 묶음에 없는 Hard 대상: 텍스처·머티리얼·인스턴스는 경고, 그 밖(스켈레톤·함수 등)은 오류(단위
// 실패와 전파), 범위 밖 클래스·Soft는 무시, 기존 에셋 조회에 있으면(redirect를 푼 경로) 진단 없음.
TEST(UnrealExchange_PlanMissingDependencies) {
    SystemAllocator allocator;
    const SyntheticDependency meshDependencies[] = {
        {"/Game/Ext/MI.MI", "MaterialInstanceConstant"},
        {"/Game/Ext/Phys.Phys", "PhysicalMaterial"},
        {"/Game/Ext/SKEL.SKEL", "Skeleton"},
        {"/Game/Ext/T.T", "TextureCube"},
        {"/Game/Ext/Z.Z", "Skeleton", true},
        {"/Game/Old/F.F", "MaterialFunction"},
    };
    const SyntheticDependency onMesh[] = {{"/Game/A/SK.SK", "SkeletalMesh"}};
    const SyntheticAsset assets[] = {
        {"/Game/A/SK.SK", "SkeletalMesh", Span<const SyntheticDependency>(meshDependencies)},
        {"/Game/B/Anim.Anim", "AnimSequence", Span<const SyntheticDependency>(onMesh)},
    };
    const SyntheticRedirect redirects[] = {{"/Game/Old/F.F", "/Game/New/F.F"}};
    {
        ExchangeBundle bundle(allocator);
        ExchangeImportPlan plan(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(PlanSynthetic(allocator, Span<const SyntheticAsset>(assets), {}, bundle, plan,
                              diagnostics, Span<const SyntheticRedirect>(redirects)));
        CHECK(diagnostics.Count(AssetDiagnosticCode::MissingUnrealDependency) == 2);
        CHECK(CountIssues(diagnostics, AssetDiagnosticCode::MissingDependency,
                          ExchangeIssue::MissingTarget) == 2);
        // 함수 의존은 redirect를 푼 경로로 보고한다.
        bool redirected = false;
        for (const ExchangeDiagnostic& item : diagnostics.Items()) {
            redirected = redirected || item.text.View() == StringView("/Game/New/F.F");
        }

        CHECK(redirected);
        const ExchangeImportUnit& mesh = UnitOfAsset(plan, 0);
        CHECK(mesh.state == ExchangeUnitState::Failed &&
              mesh.failure == AssetDiagnosticCode::MissingDependency &&
              mesh.failureAsset == ExchangeNoIndex);
        const ExchangeImportUnit& clip = UnitOfAsset(plan, 1);
        CHECK(clip.state == ExchangeUnitState::Failed && clip.failureAsset == 0);
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::MissingDependency,
                            ExchangeIssue::FailedDependency, 1, 0));
    }

    {
        const StringView existing[] = {"/Game/Ext/SKEL.SKEL", "/Game/New/F.F", "/Game/Ext/T.T"};
        ListedAssets lookup{Span<const StringView>(existing)};
        ExchangeOrderOptions options;
        options.existing = &lookup;
        ExchangeBundle bundle(allocator);
        ExchangeImportPlan plan(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(PlanSynthetic(allocator, Span<const SyntheticAsset>(assets), options, bundle, plan,
                              diagnostics, Span<const SyntheticRedirect>(redirects)));
        CHECK(diagnostics.Count(AssetDiagnosticCode::MissingDependency) == 0);
        CHECK(diagnostics.Count(AssetDiagnosticCode::MissingUnrealDependency) == 1);
        CHECK(UnitOfAsset(plan, 0).state == ExchangeUnitState::Ready);
        CHECK(UnitOfAsset(plan, 1).state == ExchangeUnitState::Ready &&
              UnitOfAsset(plan, 1).depth == 1);
    }

    {
        // 묶음 안 범위 밖 에셋을 가리키는 의존은 간선이 아니다.
        const SyntheticDependency onWorld[] = {{"/Game/L/Level.Level", "World"}};
        const SyntheticAsset withWorld[] = {
            {"/Game/A/SM.SM", "StaticMesh", Span<const SyntheticDependency>(onWorld)},
            {"/Game/L/Level.Level", "World", {}},
        };
        ExchangeBundle bundle(allocator);
        ExchangeImportPlan plan(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(PlanSynthetic(allocator, Span<const SyntheticAsset>(withWorld), {}, bundle, plan,
                              diagnostics));
        CHECK(plan.Units().Size() == 1 && UnitOfAsset(plan, 0).state == ExchangeUnitState::Ready);
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 메시 사슬 묶음을 만든다(i번이 i + 1번을 Hard로 참조). cycle이면 마지막이 0번을 참조한다.
bool WriteChain(Allocator& allocator, MemoryFiles& files, uint32 length, bool cycle) noexcept {
    Array<String> names(allocator);
    for (uint32 index = 0; index < length; ++index) {
        StringBuilder<64> name(allocator);
        String copy(allocator);
        if (!name.TryAppendFormat("/Game/C/A{:06}.A", index) || !copy.TryAssign(name.View()) ||
            !names.TryPushBack(Move(copy))) {
            return false;
        }
    }

    Array<SyntheticDependency> links(allocator);
    Array<SyntheticAsset> assets(allocator);
    if (!links.TryResize(length)) {
        return false;
    }

    for (uint32 index = 0; index < length; ++index) {
        const bool last = index + 1 == length;
        links[index] = SyntheticDependency{names[last ? 0 : index + 1].View(), "StaticMesh"};
        const bool linked = !last || cycle;
        if (!assets.TryPushBack(
                SyntheticAsset{names[index].View(), "StaticMesh",
                               linked ? Span<const SyntheticDependency>(&links[index], 1)
                                      : Span<const SyntheticDependency>()})) {
            return false;
        }
    }

    return WriteSyntheticBundle(allocator, files, assets.View());
}

// 깊이 상한: 깊이 256까지 받고 257은 UnsupportedCycle(DepthLimit). 긴 사슬·큰 순환도 재귀 없이
// 끝난다.
TEST(UnrealExchange_PlanDepthLimit) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(WriteChain(allocator, files, UnrealImportMaxDepth + 1, false));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics));
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        CHECK(UnitOfAsset(plan, 0).depth == UnrealImportMaxDepth &&
              UnitOfAsset(plan, 0).state == ExchangeUnitState::Ready);
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 0);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(WriteChain(allocator, files, UnrealImportMaxDepth + 2, false));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics));
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        const ExchangeImportUnit& top = UnitOfAsset(plan, 0);
        CHECK(top.state == ExchangeUnitState::Failed &&
              top.failure == AssetDiagnosticCode::UnsupportedCycle &&
              top.depth == UnrealImportMaxDepth + 1);
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::UnsupportedCycle,
                            ExchangeIssue::DepthLimit, 0));
        CHECK(UnitOfAsset(plan, 1).state == ExchangeUnitState::Ready);
        CHECK(diagnostics.Count(ExchangeSeverity::Error) == 1);
    }

    constexpr uint32 LongChain = 2000;
    {
        MemoryFiles files(allocator);
        REQUIRE(WriteChain(allocator, files, LongChain, false));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics));
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        REQUIRE(plan.Units().Size() == LongChain);
        uint32 failed = 0;
        for (const ExchangeImportUnit& unit : plan.Units()) {
            failed += unit.state == ExchangeUnitState::Failed ? 1 : 0;
        }

        CHECK(failed == LongChain - UnrealImportMaxDepth - 1);
        CHECK(diagnostics.Items().Size() + diagnostics.Dropped() == failed);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(WriteChain(allocator, files, LongChain, true));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, SyntheticDirectory, {}, bundle, diagnostics));
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        REQUIRE(plan.Units().Size() == 1);
        CHECK(plan.Units()[0].memberCount == LongChain &&
              plan.Units()[0].state == ExchangeUnitState::Failed &&
              plan.Units()[0].failure == AssetDiagnosticCode::UnsupportedCycle);
        const Span<const uint32> members = plan.Members(plan.Units()[0]);
        for (uint32 index = 0; index < LongChain; ++index) {
            CHECK(members[index] == index);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 플러그인 error 진단(10.4절 20번): 그 에셋은 InvalidSource(ExporterError, text = 진단 code)로
// 실패하고 Hard 의존자(인스턴스 → 머티리얼, 메시 → 인스턴스)는 FailedDependency로 실패한다.
TEST(UnrealExchange_ExporterErrorFailsAsset) {
    SystemAllocator allocator;
    MemoryFiles files(allocator);
    REQUIRE(LoadFixture(allocator, files));
    StringBuilder<256> path(allocator);
    REQUIRE(FixtureFile("assets/Game/Materials/M_Base.json", path));
    REQUIRE(files.ReplaceOnce(path.View(), "\"diagnostics\": []",
                              "\"diagnostics\": [{\"code\": \"MeshSourceUnavailable\", "
                              "\"severity\": \"warning\", \"message\": \"w\"}, "
                              "{\"code\": \"SourceReadFailed\", \"severity\": \"error\", "
                              "\"message\": \"e\"}]"));
    ExchangeBundle bundle(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
    CHECK(bundle.Assets()[5].state == ExchangeAssetState::Failed);
    CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::InvalidSource,
                        ExchangeIssue::ExporterError, 5));
    bool named = false;
    for (const ExchangeDiagnostic& item : diagnostics.Items()) {
        named = named || (item.issue == ExchangeIssue::ExporterError &&
                          item.text.View() == StringView("SourceReadFailed"));
    }

    CHECK(named);
    diagnostics.Clear();
    ExchangeImportPlan plan(allocator);
    REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
    CHECK(UnitOfAsset(plan, 4).state == ExchangeUnitState::Failed &&
          UnitOfAsset(plan, 4).failure == AssetDiagnosticCode::MissingDependency);
    CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::MissingDependency,
                        ExchangeIssue::FailedDependency, 4, 5));
}

// 실패 전파: 읽기 실패 에셋의 단위는 CorruptBundle(새 진단 없음), 그것을 참조하는 단위는
// MissingDependency. 단위 구성원의 경로 실패는 단위 전체를 실패시키고 나머지는 FailedMember다.
TEST(UnrealExchange_PlanFailurePropagation) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        StringBuilder<256> path(allocator);
        REQUIRE(FixtureFile("assets/Game/Materials/M_Base.json", path));
        REQUIRE(files.PutText(path.View(), "{"));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        diagnostics.Clear();
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        const ExchangeImportUnit& material = UnitOfAsset(plan, 5);
        CHECK(material.state == ExchangeUnitState::Failed &&
              material.failure == AssetDiagnosticCode::CorruptBundle &&
              material.failureAsset == 5 && material.depth == 0);
        // 인스턴스 → 머티리얼, 스킨 메시·메시 → 인스턴스로 전파된다.
        const uint32 blocked[] = {4, 1, 6, 7};
        for (const uint32 asset : blocked) {
            CHECK(UnitOfAsset(plan, asset).state == ExchangeUnitState::Failed);
            CHECK(UnitOfAsset(plan, asset).failure == AssetDiagnosticCode::MissingDependency);
        }

        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::MissingDependency,
                            ExchangeIssue::FailedDependency, 4, 5));
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::MissingDependency,
                            ExchangeIssue::FailedDependency, 6, 4));
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::MissingDependency,
                            ExchangeIssue::FailedMember, 7, 6));
        CHECK(UnitOfAsset(plan, 9).state == ExchangeUnitState::Ready);
        CHECK(UnitOfAsset(plan, 2).state == ExchangeUnitState::Ready);
    }

    {
        StringBuilder<512> longPath(allocator);
        REQUIRE(longPath.TryAppend("/Game/P"));
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(longPath.TryAppendAscii('/'));
            REQUIRE(longPath.TryAppendRepeated(static_cast<char>('a' + index), 60));
        }

        REQUIRE(longPath.TryAppend("/A.A"));
        const SyntheticDependency toShort[] = {{"/Game/Q/B.B", "StaticMesh"}};
        const SyntheticDependency toLong[] = {{longPath.View(), "StaticMesh"}};
        const SyntheticAsset assets[] = {
            {longPath.View(), "StaticMesh", Span<const SyntheticDependency>(toShort)},
            {"/Game/Q/B.B", "StaticMesh", Span<const SyntheticDependency>(toLong)},
        };
        ExchangeBundle bundle(allocator);
        ExchangeImportPlan plan(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(PlanSynthetic(allocator, Span<const SyntheticAsset>(assets), {}, bundle, plan,
                              diagnostics));
        REQUIRE(plan.Units().Size() == 1);
        const ExchangeImportUnit& unit = plan.Units()[0];
        CHECK(unit.state == ExchangeUnitState::Failed &&
              unit.failure == AssetDiagnosticCode::UnrealPathTooLong && unit.failureAsset == 0);
        CHECK(plan.MappedPath(0).IsEmpty() && !plan.MappedPath(1).IsEmpty());
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::UnrealPathTooLong,
                            ExchangeIssue::PathTooLong, 0));
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::MissingDependency,
                            ExchangeIssue::FailedMember, 1, 0));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 경로 겹침: 대소문자만 다른 두 에셋은 모두 UnrealPathCollision(서로 관련), 정리 뒤 다른 이름(해시
// 접미)은 겹치지 않는다. 잘못된 root는 InvalidPath 오류이고 결과는 비어 있다.
TEST(UnrealExchange_PlanPathCollisionsAndRoot) {
    SystemAllocator allocator;
    {
        const SyntheticAsset assets[] = {
            {"/Game/A/My Rock.My Rock", "StaticMesh", {}},
            {"/Game/A/My_Rock.My_Rock", "StaticMesh", {}},
            {"/Game/A/Rock.Rock", "StaticMesh", {}},
            {"/Game/A/rock.rock", "StaticMesh", {}},
            {"/Game/A/\xC3\x89t\xC3\xA9.x", "Texture2D", {}},
            {"/Game/A/\xC3\xA9T\xC3\x89.x", "Texture2D", {}},
        };
        ExchangeBundle bundle(allocator);
        ExchangeImportPlan plan(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(PlanSynthetic(allocator, Span<const SyntheticAsset>(assets), {}, bundle, plan,
                              diagnostics));
        CHECK(UnitOfAsset(plan, 0).state == ExchangeUnitState::Ready);
        CHECK(UnitOfAsset(plan, 1).state == ExchangeUnitState::Ready);
        CHECK(UnitOfAsset(plan, 2).failure == AssetDiagnosticCode::UnrealPathCollision);
        CHECK(UnitOfAsset(plan, 3).failure == AssetDiagnosticCode::UnrealPathCollision);
        CHECK(UnitOfAsset(plan, 4).failure == AssetDiagnosticCode::UnrealPathCollision);
        CHECK(UnitOfAsset(plan, 5).failure == AssetDiagnosticCode::UnrealPathCollision);
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::UnrealPathCollision,
                            ExchangeIssue::PathCollision, 2, 3));
        CHECK(HasDiagnostic(diagnostics, AssetDiagnosticCode::UnrealPathCollision,
                            ExchangeIssue::PathCollision, 3, 2));
        CHECK(diagnostics.Count(AssetDiagnosticCode::UnrealPathCollision) == 4);
        CHECK(diagnostics.Count(AssetDiagnosticCode::UnrealPathRenamed) == 1);
    }

    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        ExchangeImportPlan plan(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, plan, diagnostics));
        ExchangeOrderOptions options;
        options.root = "/Game/Bad Root";
        CHECK(PlanExchangeImport(bundle, options, plan, diagnostics).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(plan.Units().IsEmpty() && plan.MappedPath(0).IsEmpty());
        const ExchangeDiagnostic* error = diagnostics.Find(AssetDiagnosticCode::InvalidPath);
        REQUIRE(error != nullptr);
        CHECK(error->issue == ExchangeIssue::InvalidRoot &&
              error->text.View() == StringView("/Game/Bad Root"));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당 실패: 실패는 그 오류이고 결과는 비어 있으며, 끝내 성공하면 실패 없는 결과와 같다.
TEST(UnrealExchange_PlanAllocationFailures) {
    SystemAllocator allocator;
    {
        MemoryFiles files(allocator);
        REQUIRE(LoadFixture(allocator, files));
        ExchangeBundle bundle(allocator);
        ExchangeDiagnostics diagnostics(allocator);
        REQUIRE(ReadExchangeBundle(files, FixtureDirectory, {}, bundle, diagnostics));
        ExchangeImportPlan expected(allocator);
        REQUIRE(PlanExchangeImport(bundle, {}, expected, diagnostics));
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            test::NFailAllocator failing(allocator);
            ExchangeImportPlan plan(failing);
            ExchangeDiagnostics quiet(allocator);
            failing.FailOnAttempt(attempt);
            auto planned = PlanExchangeImport(bundle, {}, plan, quiet);
            if (!planned) {
                CHECK(planned.Error() == ErrorCode::OutOfMemory && plan.Units().IsEmpty());
                ++failures;
                continue;
            }

            REQUIRE(plan.Units().Size() == expected.Units().Size());
            for (usize index = 0; index < plan.Units().Size(); ++index) {
                CHECK(plan.Members(plan.Units()[index])[0] ==
                      expected.Members(expected.Units()[index])[0]);
            }

            for (uint32 asset = 0; asset < bundle.Assets().Size(); ++asset) {
                CHECK(plan.MappedPath(asset) == expected.MappedPath(asset));
            }

            if (failing.Attempts() < attempt) {
                break;
            }
        }

        CHECK(failures > 10);

        // 진단 목록의 할당 실패도 그 오류다.
        test::NFailAllocator failing(allocator);
        ExchangeDiagnostics noisy(failing);
        ExchangeImportPlan plan(allocator);
        failing.FailOnAttempt(1);
        CHECK(PlanExchangeImport(bundle, {}, plan, noisy).Error() == ErrorCode::OutOfMemory);
        CHECK(plan.Units().IsEmpty());
        failing.DisableFailure();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace exchange_import_order_tests_file
