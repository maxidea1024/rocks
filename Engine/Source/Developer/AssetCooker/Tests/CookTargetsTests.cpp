#include "CookTestSupport.h"

#include <TestSupport/NFailAllocator.h>

// 쿠킹 대상 계산 시험이다(Docs/Design/AssetSystem 10.2절 결정 4, "A4b 구현 세부"): 전체
// 쿠킹(alwaysCook·PrimaryAsset 규칙·파생 타입·디렉터리 판정·Hard 폐포·Soft 설정·따라가지 않는
// 간선·bundle 태그·PrimaryAsset이 아닌 에셋의 태그), 뿌리 진단(없는 경로 경고·같은 경로·같은 id·
// 마운트), NeverCook(직접 지정 경고·규칙은 조용히·Hard·bundle 오류·Soft 제외·마운트 전체),
// 누락·같은 id 의존과 깨진 bundle 태그, 부분 쿠킹(인자 경로·역방향 Build 고정점·기존 매니페스트의
// 사라진 항목), 요청 거부, 결정적 순서, 할당 실패.
using namespace rocks;

namespace cook_targets_tests_file {

using namespace rocks::cook_test;

namespace {

// 전체 쿠킹 그래프다(FullCook·할당 실패 공용). L(Maps/Level)은 Hard → A, Soft → S, EditorOnly →
// E, Build → B, ImportOwner → I, bundle Ui = U1·U2, Audio = U3. A(Base) → A2(Derived) Hard, S → S2
// Hard. X(Always)는 PrimaryAsset이 아니면서 bundle 태그를 가지고(→ Z, 따라가지 않는다) A2를 Build로
// 의존한다. D1·D2는 같은 id(A2를 Build로 의존)다 — 전체 쿠킹 설정은 닿지 않고 부분 쿠킹의 역방향
// Build가 닿는다.
constexpr uint64 LevelId = 10;
constexpr uint64 ArenaId = 11;
constexpr uint64 AId = 20;
constexpr uint64 A2Id = 21;
constexpr uint64 SId = 30;
constexpr uint64 S2Id = 31;
constexpr uint64 EId = 40;
constexpr uint64 BId = 41;
constexpr uint64 IId = 42;
constexpr uint64 U1Id = 50;
constexpr uint64 U2Id = 51;
constexpr uint64 U3Id = 52;
constexpr uint64 XId = 60;
constexpr uint64 ZId = 80;
constexpr uint64 DupId = 100;
const uint64 FullTextIds[] = {U1Id, U2Id, U3Id, ZId};
const Dep LevelDeps[] = {
    {AId, K::Hard}, {SId, K::Soft}, {EId, K::EditorOnly}, {BId, K::Build}, {IId, K::ImportOwner}};
const Dep ADeps[] = {{A2Id, K::Hard}};
const Dep SDeps[] = {{S2Id, K::Hard}};
const Dep BuildA2Deps[] = {{A2Id, K::Build}};

bool BuildFullScene(CookScene& scene, const IdTexts& ids) noexcept {
    const PackageTagInput levelTags[] = {{"Bundle.Ui", ids(U1Id)},
                                         {"Bundle.Ui", ids(U2Id)},
                                         {"Bundle.Audio", ids(U3Id)},
                                         {"Kind", "Map"}};
    const PackageTagInput xTags[] = {{"Bundle.Ignored", ids(ZId)}};
    const Node nodes[] = {{"Maps/Level.rasset", LevelId, NodeType::Test, LevelDeps, levelTags},
                          {"Maps/Arena.rasset", ArenaId},
                          {"Props/A.rasset", AId, NodeType::Base, ADeps},
                          {"Props/A2.rasset", A2Id, NodeType::Derived},
                          {"Props/S.rasset", SId, NodeType::Test, SDeps},
                          {"Props/S2.rasset", S2Id},
                          {"Props/E.rasset", EId},
                          {"Props/B.rasset", BId},
                          {"Props/I.rasset", IId},
                          {"UI/U1.rasset", U1Id},
                          {"UI/U2.rasset", U2Id},
                          {"UI/U3.rasset", U3Id},
                          {"Always/X.rasset", XId, NodeType::Test, BuildA2Deps, xTags},
                          {"Unused/Z.rasset", ZId},
                          {"Dup/D1.rasset", DupId, NodeType::Test, BuildA2Deps},
                          {"Dup/D2.rasset", DupId, NodeType::Test, BuildA2Deps}};
    return scene.Build(nodes);
}

bool SetFullSettings(CookSettings& settings) noexcept {
    const StringView maps[] = {"/Game/Maps/"};
    return Push(settings.alwaysCook, "/Game/Always/X") && AddRule(settings, "TestAsset", maps);
}

} // namespace

// 전체 쿠킹: 뿌리 = alwaysCook ∪ PrimaryAsset 규칙(디렉터리 아래·타입 — 파생 포함), Hard 폐포와
// PrimaryAsset bundle 구성원. Soft는 설정일 때만, EditorOnly·Build·ImportOwner는 따라가지 않는다.
// PrimaryAsset이 아닌 에셋의 bundle 태그는 무시한다. bundle은 이름 순·구성원 AssetId 순이다. 규칙에
// 맞는 에셋은 폐포로 들어와도 PrimaryAsset이고, 규칙 밖 에셋은 bundle을 따라가지 않는다. out은
// 비우고 채운다.
TEST(CookTargets_FullCook) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene scene(env);
    IdTexts ids(env.allocator, FullTextIds);
    REQUIRE(ids.ready);
    REQUIRE(BuildFullScene(scene, ids));
    REQUIRE(SetFullSettings(scene.settings));
    CookTargetSet out(env.allocator);
    REQUIRE(out.diagnostics.TryPushBack(CookDiagnostic{}));
    REQUIRE(out.removed.TryPushBack(FixedId(1)));
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 expected[] = {LevelId, ArenaId, AId, A2Id, U1Id, U2Id, U3Id, XId};
    CHECK(SameTargets(out, expected));
    CHECK(out.diagnostics.IsEmpty() && out.excluded.IsEmpty() && out.removed.IsEmpty());
    CHECK(!out.HasErrors());
    const CookTarget* level = TargetOf(out, LevelId);
    REQUIRE(level != nullptr);
    CHECK(level->primary && TargetOf(out, ArenaId)->primary);
    CHECK(!TargetOf(out, AId)->primary && !TargetOf(out, XId)->primary);
    CHECK(TargetOf(out, ArenaId)->bundles.IsEmpty() && TargetOf(out, XId)->bundles.IsEmpty());
    REQUIRE(level->bundles.Size() == 2);
    CHECK(level->bundles[0].name.View() == "Audio" && level->bundles[1].name.View() == "Ui");
    const uint64 audio[] = {U3Id};
    const uint64 ui[] = {U1Id, U2Id};
    CHECK(SameIds(level->bundles[0].members, audio) && SameIds(level->bundles[1].members, ui));

    // Soft 설정: S와 그 Hard 폐포가 더해진다.
    scene.settings.cookSoftReferences = true;
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 withSoft[] = {LevelId, ArenaId, AId, A2Id, SId, S2Id, U1Id, U2Id, U3Id, XId};
    CHECK(SameTargets(out, withSoft) && out.diagnostics.IsEmpty());

    // 파생 타입 규칙: BaseGameAsset 규칙이 A(Base)·A2(Derived)를 고른다(Props의 TestAsset은 아님).
    scene.ResetSettings();
    const StringView props[] = {"/Game/Props/"};
    REQUIRE(AddRule(scene.settings, "BaseGameAsset", props));
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 derived[] = {AId, A2Id};
    CHECK(SameTargets(out, derived) && out.targets[0].primary && out.targets[1].primary);

    // 규칙 밖 뿌리(alwaysCook L)는 PrimaryAsset이 아니다 — bundle을 따라가지 않는다. 폐포로 들어온
    // A·A2는 규칙에 맞아 PrimaryAsset이다.
    REQUIRE(Push(scene.settings.alwaysCook, "/Game/Maps/Level"));
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 levelOnly[] = {LevelId, AId, A2Id};
    CHECK(SameTargets(out, levelOnly));
    CHECK(!out.targets[0].primary && out.targets[0].bundles.IsEmpty());
    CHECK(out.targets[1].primary && out.targets[2].primary);

    // 디렉터리 판정은 구성 요소 경계다(/Game/U는 /Game/UI 아래가 아니다).
    scene.ResetSettings();
    const StringView partial[] = {"/Game/U/", "/Game/UI/"};
    REQUIRE(AddRule(scene.settings, "TestAsset", partial));
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 uiOnly[] = {U1Id, U2Id, U3Id};
    CHECK(SameTargets(out, uiOnly));
}

// 뿌리 진단: 디렉터리는 재귀, 아무것도 가리키지 않는 alwaysCook 에셋·디렉터리는 경고 InvalidPath
// (setting = 순번), 같은 경로 키 파일 둘은 오류 DuplicatePath, 같은 id 파일은 오류 DuplicateAssetId
// (alwaysCook 경로·디렉터리 목록·PrimaryAsset 규칙 각각). 이미 넣은 뿌리를 다시 가리켜도 한 번이다.
// 마운트 디렉터리는 모든 항목이다.
TEST(CookTargets_Roots) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene scene(env);
    const Node nodes[] = {{"Always/X.rasset", 60},       {"Always/Sub/Y.rasset", 61},
                          {"Case/P.rasset", 110},        {"Case/p.RASSET", 111},
                          {"Dup/D1.rasset", 100},        {"Dup/D2.rasset", 100},
                          {"Maps/Level.rasset", LevelId}};
    REQUIRE(scene.Build(nodes));
    const StringView always[] = {"/Game/Always/", "/Game/Nope",   "/Game/Nope/",
                                 "/Game/Case/P",  "/Game/Dup/D1", "/Game/Always/X"};
    for (StringView path : always) {
        REQUIRE(Push(scene.settings.alwaysCook, path));
    }

    const StringView dup[] = {"/Game/Dup/"};
    REQUIRE(AddRule(scene.settings, "TestAsset", dup));
    CookTargetSet out(env.allocator);
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 expected[] = {60, 61};
    CHECK(SameTargets(out, expected));
    const ExpectedDiagnostic diagnostics[] = {{C::InvalidPath, true, 0, 0, 1},
                                              {C::InvalidPath, true, 0, 0, 2},
                                              {C::DuplicatePath, false, 0, 0, 3},
                                              {C::DuplicateAssetId, false, 100, 0, 4},
                                              {C::DuplicateAssetId, false, 100, 0, 0}};
    CHECK(SameDiagnostics(out.diagnostics, diagnostics));
    CHECK(out.HasErrors());

    // 마운트: 모든 항목(같은 id 파일은 각각 진단, NeverCook 디렉터리는 각각 경고).
    scene.ResetSettings();
    REQUIRE(Push(scene.settings.alwaysCook, "/Game/"));
    REQUIRE(Push(scene.settings.neverCook, "/Game/Case/"));
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 all[] = {LevelId, 60, 61};
    CHECK(SameTargets(out, all));
    const ExpectedDiagnostic mount[] = {{C::DuplicateAssetId, false, 100, 0, 0},
                                        {C::DuplicateAssetId, false, 100, 0, 0},
                                        {C::NeverCookReferenced, true, 110, 0, 0},
                                        {C::NeverCookReferenced, true, 111, 0, 0}};
    CHECK(SameDiagnostics(out.diagnostics, mount));
}

// NeverCook: alwaysCook가 직접(에셋·디렉터리) 가리키면 경고 NeverCookReferenced(setting = 순번)
// 하고 뺀다. PrimaryAsset 규칙으로만 닿으면 진단 없이 뺀다. Hard·bundle 의존은 오류(asset =
// 참조자), Soft 의존은 설정일 때 제외 목록(진단 없음, AssetId 순), 설정이 아니면 아무것도 아니다.
// 마운트 NeverCook은 모든 뿌리를 뺀다(경고만 — 오류 아님).
TEST(CookTargets_NeverCook) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene scene(env);
    const uint64 textIds[] = {70};
    IdTexts ids(env.allocator, textIds);
    REQUIRE(ids.ready);
    const Dep hardNever[] = {{70, K::Hard}};
    const Dep softNever[] = {{70, K::Soft}, {71, K::Soft}};
    const PackageTagInput bundleNever[] = {{"Bundle.X", ids(70)}};
    const Node nodes[] = {{"Never/N.rasset", 70},
                          {"Never/N2.rasset", 71},
                          {"Refs/H.rasset", 90, NodeType::Test, hardNever},
                          {"Refs/SoftN.rasset", 91, NodeType::Test, softNever},
                          {"Bundles/BN.rasset", 92, NodeType::Test, {}, bundleNever},
                          {"Maps/Level.rasset", LevelId}};
    REQUIRE(scene.Build(nodes));
    const StringView always[] = {"/Game/Never/N", "/Game/Refs/", "/Game/Never/"};
    for (StringView path : always) {
        REQUIRE(Push(scene.settings.alwaysCook, path));
    }

    REQUIRE(Push(scene.settings.neverCook, "/Game/Never/"));
    const StringView bundles[] = {"/Game/Bundles/", "/Game/Never/"};
    REQUIRE(AddRule(scene.settings, "TestAsset", bundles));
    CookTargetSet out(env.allocator);
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 expected[] = {90, 91, 92};
    CHECK(SameTargets(out, expected));
    const ExpectedDiagnostic diagnostics[] = {{C::NeverCookReferenced, true, 70, 0, 0},
                                              {C::NeverCookReferenced, true, 70, 0, 2},
                                              {C::NeverCookReferenced, true, 71, 0, 2},
                                              {C::NeverCookReferenced, false, 90, 70, 0},
                                              {C::NeverCookReferenced, false, 92, 70, 0}};
    CHECK(SameDiagnostics(out.diagnostics, diagnostics));
    CHECK(out.excluded.IsEmpty());

    // Soft 설정: Soft로만 닿은 NeverCook 에셋은 제외 목록(진단 없음).
    scene.settings.cookSoftReferences = true;
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    CHECK(SameTargets(out, expected) && SameDiagnostics(out.diagnostics, diagnostics));
    const uint64 excluded[] = {70, 71};
    CHECK(SameIds(out.excluded, excluded));

    // 마운트 NeverCook: 직접 지정한 에셋은 경고하고 뺀다(대상 없음).
    scene.ResetSettings();
    REQUIRE(Push(scene.settings.neverCook, "/Game/"));
    REQUIRE(Push(scene.settings.alwaysCook, "/Game/Refs/H"));
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    CHECK(out.targets.IsEmpty());
    const ExpectedDiagnostic mount[] = {{C::NeverCookReferenced, true, 90, 0, 0}};
    CHECK(SameDiagnostics(out.diagnostics, mount));
    CHECK(!out.HasErrors());
}

// EditorOnly 에셋(대표 타입이 EditorOnly 클래스 — 결정 4, A4e2b)은 NeverCook과 같은 규칙이다:
// alwaysCook이 직접 가리키면 경고 NeverCookReferenced(디렉터리 아래 둘 모두), Hard·bundle 참조는
// 오류, Soft 설정에서 Soft로만 닿으면 제외 목록, PrimaryAsset 규칙으로만 닿으면 진단 없이 뺀다.
// 대표 타입이 요청 TypeRegistry에 없으면(로드되지 않은 모듈) EditorOnly로 판정하지 않고 대상에
// 둔다(키 단계가 UnsupportedVersion을 낸다), 그 밖의 조회 오류(ShuttingDown)는 함수 오류다.
TEST(CookTargets_EditorOnlyAssets) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene scene(env);
    const uint64 textIds[] = {70};
    IdTexts ids(env.allocator, textIds);
    REQUIRE(ids.ready);
    const Dep hardTool[] = {{70, K::Hard}};
    const Dep softTool[] = {{70, K::Soft}, {71, K::Soft}};
    const PackageTagInput bundleTool[] = {{"Bundle.X", ids(70)}};
    const Node nodes[] = {{"Editor/E.rasset", 70, NodeType::EditorTool},
                          {"Editor/E2.rasset", 71, NodeType::EditorTool},
                          {"Refs/H.rasset", 90, NodeType::Test, hardTool},
                          {"Refs/SoftE.rasset", 91, NodeType::Test, softTool},
                          {"Bundles/BE.rasset", 92, NodeType::Test, {}, bundleTool},
                          {"Maps/Level.rasset", LevelId}};
    REQUIRE(scene.Build(nodes));
    const StringView always[] = {"/Game/Editor/E", "/Game/Refs/", "/Game/Editor/"};
    for (StringView path : always) {
        REQUIRE(Push(scene.settings.alwaysCook, path));
    }

    const StringView bundles[] = {"/Game/Bundles/"};
    REQUIRE(AddRule(scene.settings, "TestAsset", bundles));
    CookTargetSet out(env.allocator);
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 expected[] = {90, 91, 92};
    CHECK(SameTargets(out, expected));
    const ExpectedDiagnostic diagnostics[] = {{C::NeverCookReferenced, true, 70, 0, 0},
                                              {C::NeverCookReferenced, true, 70, 0, 2},
                                              {C::NeverCookReferenced, true, 71, 0, 2},
                                              {C::NeverCookReferenced, false, 90, 70, 0},
                                              {C::NeverCookReferenced, false, 92, 70, 0}};
    CHECK(SameDiagnostics(out.diagnostics, diagnostics));
    scene.settings.cookSoftReferences = true;
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 excluded[] = {70, 71};
    CHECK(SameIds(out.excluded, excluded));

    // PrimaryAsset 규칙으로만 닿은 EditorOnly 에셋은 진단 없이 빠진다.
    scene.ResetSettings();
    const StringView editor[] = {"/Game/Editor/"};
    REQUIRE(AddRule(scene.settings, "EditorToolAsset", editor));
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    CHECK(out.targets.IsEmpty() && out.diagnostics.IsEmpty());

    // 요청 TypeRegistry에 대표 타입이 없으면 판정하지 않고 대상에 둔다.
    scene.ResetSettings();
    REQUIRE(Push(scene.settings.alwaysCook, "/Game/Editor/E"));
    SystemAllocator allocator;
    TypeRegistry runtimeOnly(allocator);
    ModuleRegistration modules(runtimeOnly, allocator);
    REQUIRE(RegisterAssetRuntimeTypes(modules));
    CookTargetRequest missing = scene.Request();
    missing.types = &runtimeOnly;
    REQUIRE(ComputeCookTargets(env.allocator, missing, out));
    const uint64 unjudged[] = {70};
    CHECK(SameTargets(out, unjudged) && out.diagnostics.IsEmpty());

    // 닫힌 TypeRegistry의 조회 오류는 함수 오류다.
    REQUIRE(modules.Unregister());
    REQUIRE(runtimeOnly.Shutdown());
    CHECK(ComputeCookTargets(env.allocator, missing, out).Error() == ErrorCode::ShuttingDown);
}

// 누락·같은 id 의존과 bundle 태그: Hard·bundle 누락은 오류·Soft 누락은 경고 MissingDependency(같은
// 의존의 Hard·Soft 누락은 둘 다 — 오류가 먼저), 같은
// id 의존(Hard·Soft·bundle)은 오류 DuplicateAssetId, bundle 태그 값이 AssetId가 아니면(문자열·빈
// 이름·null·대문자) 오류 CorruptPackage. bundle 목록은 올바른 태그만 담는다. Soft 설정이 아니면
// Soft 의존은 보지 않는다.
TEST(CookTargets_MissingAndDuplicates) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene scene(env);
    const uint64 textIds[] = {LevelId, 100, 997};
    IdTexts ids(env.allocator, textIds);
    REQUIRE(ids.ready);
    // 대문자 표기(TryParse가 거부한다)다.
    char upperText[AssetId::TextBytes] = {};
    const StringView lower = ids(LevelId);
    REQUIRE(lower.ByteCount() == AssetId::TextBytes);
    for (usize at = 0; at < lower.ByteCount(); ++at) {
        const char c = lower.Data()[at];
        upperText[at] = c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c;
    }

    const StringView upper(upperText, AssetId::TextBytes);

    const Dep missing[] = {{999, K::Hard}, {999, K::Soft}, {998, K::Soft}};
    const Dep hardDup[] = {{100, K::Hard}};
    const Dep softDup[] = {{100, K::Soft}};
    const PackageTagInput bundleTags[] = {
        {"Bundle.", ids(LevelId)},   {"Bundle.Bad", "nope"},
        {"Bundle.Dup", ids(100)},    {"Bundle.Null", "00000000-0000-0000-0000-000000000000"},
        {"Bundle.Ok", ids(LevelId)}, {"Bundle.Upper", upper},
        {"Bundle.Y", ids(997)}};
    const Node nodes[] = {{"Refs/Miss.rasset", 93, NodeType::Test, missing},
                          {"Refs/HD.rasset", 95, NodeType::Test, hardDup},
                          {"Refs/SD.rasset", 96, NodeType::Test, softDup},
                          {"Dup/D1.rasset", 100},
                          {"Dup/D2.rasset", 100},
                          {"Bundles/BM.rasset", 94, NodeType::Test, {}, bundleTags},
                          {"Maps/Level.rasset", LevelId}};
    REQUIRE(scene.Build(nodes));
    REQUIRE(Push(scene.settings.alwaysCook, "/Game/Refs/"));
    const StringView bundles[] = {"/Game/Bundles/"};
    REQUIRE(AddRule(scene.settings, "TestAsset", bundles));
    scene.settings.cookSoftReferences = true;
    CookTargetSet out(env.allocator);
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    const uint64 expected[] = {LevelId, 93, 94, 95, 96};
    CHECK(SameTargets(out, expected));
    const ExpectedDiagnostic diagnostics[] = {
        {C::MissingDependency, false, 93, 999, 0}, {C::MissingDependency, true, 93, 999, 0},
        {C::MissingDependency, true, 93, 998, 0},  {C::DuplicateAssetId, false, 95, 100, 0},
        {C::DuplicateAssetId, false, 96, 100, 0},  {C::MissingDependency, false, 94, 997, 0},
        {C::DuplicateAssetId, false, 94, 100, 0},  {C::CorruptPackage, false, 94, 0, 0},
        {C::CorruptPackage, false, 94, 0, 0},      {C::CorruptPackage, false, 94, 0, 0},
        {C::CorruptPackage, false, 94, 0, 0}};
    CHECK(SameDiagnostics(out.diagnostics, diagnostics));
    const CookTarget* bm = TargetOf(out, 94);
    REQUIRE(bm != nullptr && bm->primary);
    REQUIRE(bm->bundles.Size() == 3);
    CHECK(bm->bundles[0].name.View() == "Dup" && bm->bundles[1].name.View() == "Ok" &&
          bm->bundles[2].name.View() == "Y");
    const uint64 dupMembers[] = {100};
    const uint64 okMembers[] = {LevelId};
    const uint64 yMembers[] = {997};
    CHECK(SameIds(bm->bundles[0].members, dupMembers) &&
          SameIds(bm->bundles[1].members, okMembers) && SameIds(bm->bundles[2].members, yMembers));

    // Soft 설정이 아니면 Soft 누락·같은 id Soft는 보지 않는다.
    scene.settings.cookSoftReferences = false;
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(), out));
    CHECK(SameTargets(out, expected));
    const ExpectedDiagnostic hardOnly[] = {
        {C::MissingDependency, false, 93, 999, 0}, {C::DuplicateAssetId, false, 95, 100, 0},
        {C::MissingDependency, false, 94, 997, 0}, {C::DuplicateAssetId, false, 94, 100, 0},
        {C::CorruptPackage, false, 94, 0, 0},      {C::CorruptPackage, false, 94, 0, 0},
        {C::CorruptPackage, false, 94, 0, 0},      {C::CorruptPackage, false, 94, 0, 0}};
    CHECK(SameDiagnostics(out.diagnostics, hardOnly));
}

// 부분 쿠킹: 인자 경로가 뿌리다(없는 경로는 오류 InvalidPath, NeverCook은 경고). 인자 뿌리와 역방향
// 으로 더한 에셋을 Build로 의존하는 에셋을 고정점까지 더하고(더한 에셋도 Hard 폐포, NeverCook
// 참조자는 조용히 빼고, 같은 id 참조자는 오류 DuplicateAssetId) Hard 폐포로만 들어온 에셋(Arena)의
// Build 참조자(AR)는 더하지 않는다. 폐포로 먼저 들어온 에셋(Q)이 뒤에 역방향으로 닿으면 그
// 참조자(QR)도 더한다. PrimaryAsset 규칙에 맞는 폐포 에셋은 bundle을 따른다. 기존 매니페스트:
// 색인에 고유하게 있고 NeverCook이 아닌 항목(Keep)은 뿌리(키를 다시 계산 — 10절, 옛 의존으로 오류
// 내지 않음), 색인에 없거나 NeverCook이 된 항목(NB)은 사라진 항목, 같은 id 항목은 오류
// DuplicateAssetId이고 다시 쿠킹하지 않는 그 항목의 Hard 의존이 사라졌으면 오류
// MissingDependency(다시 쿠킹하는 R의 옛 의존은 아님, 사라진 항목끼리는 보지 않음). 전체 쿠킹은
// 기존 매니페스트를 보지 않는다.
TEST(CookTargets_Partial) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene scene(env);
    const uint64 textIds[] = {U1Id};
    IdTexts ids(env.allocator, textIds);
    REQUIRE(ids.ready);
    const Dep rDeps[] = {{A2Id, K::Build}, {122, K::Hard}};
    const Dep r2Deps[] = {{120, K::Build}};
    const Dep buildA2[] = {{A2Id, K::Build}};
    const Dep pDeps[] = {{ArenaId, K::Hard}, {131, K::Hard}};
    const Dep qDeps[] = {{130, K::Build}};
    const Dep qrDeps[] = {{131, K::Build}};
    const Dep arDeps[] = {{ArenaId, K::Build}};
    const PackageTagInput arenaTags[] = {{"Bundle.Ui", ids(U1Id)}};
    const Node nodes[] = {{"Props/A2.rasset", A2Id, NodeType::Derived},
                          {"Build/R.rasset", 120, NodeType::Test, rDeps},
                          {"Build/R2.rasset", 121, NodeType::Test, r2Deps},
                          {"Build/H2.rasset", 122},
                          {"Never/NB.rasset", 72, NodeType::Test, buildA2},
                          {"Dup/D1.rasset", 100, NodeType::Test, buildA2},
                          {"Dup/D2.rasset", 100, NodeType::Test, buildA2},
                          {"Props/P.rasset", 130, NodeType::Test, pDeps},
                          {"Maps/Arena.rasset", ArenaId, NodeType::Test, {}, arenaTags},
                          {"UI/U1.rasset", U1Id},
                          {"Props/Keep.rasset", 140},
                          {"Props/Q.rasset", 131, NodeType::Test, qDeps},
                          {"Build/QR.rasset", 132, NodeType::Test, qrDeps},
                          {"Build/AR.rasset", 133, NodeType::Test, arDeps}};
    REQUIRE(scene.Build(nodes));
    REQUIRE(Push(scene.settings.neverCook, "/Game/Never/"));
    const StringView maps[] = {"/Game/Maps/"};
    REQUIRE(AddRule(scene.settings, "TestAsset", maps));

    CookedManifest existing(env.allocator);
    struct ManifestRow {
        uint64 asset;
        Dep deps[3];
        usize count;
    };
    const ManifestRow rows[] = {{72, {}, 0},
                                {100, {{A2Id, K::Hard}, {150, K::Hard}, {151, K::Soft}}, 3},
                                {120, {{150, K::Hard}}, 1},
                                {140, {{A2Id, K::Hard}, {150, K::Hard}, {151, K::Soft}}, 3},
                                {150, {{151, K::Hard}}, 1},
                                {151, {}, 0}};
    for (const ManifestRow& row : rows) {
        CookedManifestEntry entry(env.allocator);
        entry.asset = FixedId(row.asset);
        for (usize at = 0; at < row.count; ++at) {
            REQUIRE(entry.dependencies.TryPushBack(
                CookedManifestDependency{FixedId(row.deps[at].id), row.deps[at].kind}));
        }

        REQUIRE(existing.entries.TryPushBack(Move(entry)));
    }

    const StringView paths[] = {"/Game/Props/A2", "/Game/Nope", "/Game/Never/NB", "/Game/Props/P"};
    CookTargetSet out(env.allocator);
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(paths, &existing), out));
    // 기존 항목 중 색인에 고유하게 있고 NeverCook이 아닌 Keep(140)도 대상이다(키를 다시 계산 —
    // 10절).
    const uint64 expected[] = {ArenaId, A2Id, U1Id, 120, 121, 122, 130, 131, 132, 140};
    CHECK(SameTargets(out, expected));
    const CookTarget* arena = TargetOf(out, ArenaId);
    REQUIRE(arena != nullptr && arena->primary && arena->bundles.Size() == 1);
    CHECK(!TargetOf(out, 130)->primary);
    const ExpectedDiagnostic diagnostics[] = {
        {C::InvalidPath, false, 0, 0, 1},           {C::NeverCookReferenced, true, 72, 0, 2},
        {C::DuplicateAssetId, false, A2Id, 100, 0}, {C::DuplicateAssetId, false, A2Id, 100, 0},
        {C::MissingDependency, false, 100, 150, 0}, {C::DuplicateAssetId, false, 100, 0, 0}};
    CHECK(SameDiagnostics(out.diagnostics, diagnostics));
    // 색인에 없는 150·151과 NeverCook이 된 72는 사라진 항목이다.
    const uint64 removed[] = {72, 150, 151};
    CHECK(SameIds(out.removed, removed));

    // 디렉터리 인자: 그 아래 전부와 역방향 Build(이미 대상인 참조자는 한 번).
    const StringView directory[] = {"/Game/Build/"};
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(directory), out));
    const uint64 build[] = {120, 121, 122, 132, 133};
    CHECK(SameTargets(out, build) && out.diagnostics.IsEmpty() && out.removed.IsEmpty());

    // 전체 쿠킹은 기존 매니페스트를 보지 않는다.
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request({}, &existing), out));
    CHECK(out.removed.IsEmpty());
    const uint64 primaryOnly[] = {ArenaId, U1Id};
    CHECK(SameTargets(out, primaryOnly) && out.diagnostics.IsEmpty());
}

// 요청 거부: 색인·TypeRegistry·설정이 null이면 InvalidArgument(out은 비운다). 검증되지 않은 설정 —
// 형식이 틀린 alwaysCook·neverCook·PrimaryAsset 디렉터리·인자 경로, 등록되지 않은 PrimaryAsset
// 타입 — 도 InvalidArgument다. 종료한 TypeRegistry의 조회 오류는 ShuttingDown 그대로다.
TEST(CookTargets_Rejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene scene(env);
    const Node nodes[] = {{"Maps/Level.rasset", LevelId}};
    REQUIRE(scene.Build(nodes));
    CookTargetSet out(env.allocator);
    REQUIRE(out.diagnostics.TryPushBack(CookDiagnostic{}));
    CookTargetRequest request = scene.Request();
    request.index = nullptr;
    CHECK(ComputeCookTargets(env.allocator, request, out).Error() == ErrorCode::InvalidArgument);
    CHECK(out.diagnostics.IsEmpty());
    request = scene.Request();
    request.types = nullptr;
    CHECK(ComputeCookTargets(env.allocator, request, out).Error() == ErrorCode::InvalidArgument);
    request = scene.Request();
    request.settings = nullptr;
    CHECK(ComputeCookTargets(env.allocator, request, out).Error() == ErrorCode::InvalidArgument);

    const StringView bad = "Game/Level";
    REQUIRE(Push(scene.settings.alwaysCook, bad));
    CHECK(ComputeCookTargets(env.allocator, scene.Request(), out).Error() ==
          ErrorCode::InvalidArgument);
    scene.ResetSettings();
    REQUIRE(Push(scene.settings.neverCook, bad));
    CHECK(ComputeCookTargets(env.allocator, scene.Request(), out).Error() ==
          ErrorCode::InvalidArgument);
    scene.ResetSettings();
    const StringView badDirectories[] = {bad};
    REQUIRE(AddRule(scene.settings, "TestAsset", badDirectories));
    CHECK(ComputeCookTargets(env.allocator, scene.Request(), out).Error() ==
          ErrorCode::InvalidArgument);
    scene.ResetSettings();
    const StringView maps[] = {"/Game/Maps/"};
    REQUIRE(AddRule(scene.settings, "NoSuchType", maps));
    CHECK(ComputeCookTargets(env.allocator, scene.Request(), out).Error() ==
          ErrorCode::InvalidArgument);
    // 종료한 TypeRegistry는 그 오류(ShuttingDown) 그대로다.
    TypeRegistry closed(env.allocator);
    REQUIRE(closed.Shutdown());
    request = scene.Request();
    request.types = &closed;
    CHECK(ComputeCookTargets(env.allocator, request, out).Error() == ErrorCode::ShuttingDown);
    scene.ResetSettings();
    const StringView badPaths[] = {bad};
    CHECK(ComputeCookTargets(env.allocator, scene.Request(badPaths), out).Error() ==
          ErrorCode::InvalidArgument);
    const StringView valid[] = {"/Game/Maps/Level"};
    REQUIRE(ComputeCookTargets(env.allocator, scene.Request(valid), out));
    const uint64 level[] = {LevelId};
    CHECK(SameTargets(out, level));
}

// 할당 실패: 전체 쿠킹(bundle·Soft·PrimaryAsset·제외)과 부분 쿠킹(역방향 Build 참조자·같은 id
// 참조자·기존 매니페스트·진단)의
// 모든 할당 지점에서 OutOfMemory이고, 지점을 다 지나면 기준 결과와 같다.
TEST(CookTargets_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene scene(env);
    IdTexts ids(env.allocator, FullTextIds);
    REQUIRE(ids.ready);
    REQUIRE(BuildFullScene(scene, ids));
    REQUIRE(SetFullSettings(scene.settings));
    REQUIRE(Push(scene.settings.neverCook, "/Game/Props/S"));
    REQUIRE(Push(scene.settings.alwaysCook, "/Game/Nope/"));
    scene.settings.cookSoftReferences = true;

    CookedManifest existing(env.allocator);
    CookedManifestEntry kept(env.allocator);
    kept.asset = FixedId(AId);
    REQUIRE(kept.dependencies.TryPushBack(CookedManifestDependency{FixedId(150), K::Hard}));
    REQUIRE(existing.entries.TryPushBack(Move(kept)));
    CookedManifestEntry gone(env.allocator);
    gone.asset = FixedId(150);
    REQUIRE(existing.entries.TryPushBack(Move(gone)));
    const StringView paths[] = {"/Game/Maps/", "/Game/Props/A2"};

    for (int pass = 0; pass < 2; ++pass) {
        const CookTargetRequest request =
            pass == 0 ? scene.Request() : scene.Request(paths, &existing);
        CookTargetSet baseline(env.allocator);
        REQUIRE(ComputeCookTargets(env.allocator, request, baseline));
        REQUIRE(!baseline.targets.IsEmpty() && !baseline.diagnostics.IsEmpty());
        test::NFailAllocator failing(env.allocator);
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            CookTargetSet out(failing);
            failing.FailOnAttempt(attempt);
            auto computed = ComputeCookTargets(failing, request, out);
            const bool failed = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!failed) {
                REQUIRE(computed);
                REQUIRE(out.targets.Size() == baseline.targets.Size());
                for (usize index = 0; index < out.targets.Size(); ++index) {
                    CHECK(out.targets[index].id == baseline.targets[index].id &&
                          out.targets[index].primary == baseline.targets[index].primary &&
                          out.targets[index].bundles.Size() ==
                              baseline.targets[index].bundles.Size());
                }

                CHECK(out.excluded.Size() == baseline.excluded.Size() &&
                      out.removed.Size() == baseline.removed.Size() &&
                      out.diagnostics.Size() == baseline.diagnostics.Size());
                break;
            }

            CHECK(computed.Error() == ErrorCode::OutOfMemory);
            ++failures;
        }

        CHECK(failures >= 10);
    }
}

} // namespace cook_targets_tests_file
