#include "../../../Runtime/AssetRuntime/Tests/AssetTestSupport.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <AssetCooker/AssetCooker.module.generated.h>
#include <AssetCooker/CookTargets.h>
#include <AssetRuntime/PackageFormat.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/Text/StringBuilder.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 쿠킹 대상 계산 시험이다(Docs/Design/AssetSystem.ko.md 10.2절 결정 4, "A4b 구현 세부"): 전체
// 쿠킹(alwaysCook·PrimaryAsset 규칙·파생 타입·디렉터리 판정·Hard 폐포·Soft 설정·따라가지 않는
// 간선·bundle 태그·PrimaryAsset이 아닌 에셋의 태그), 뿌리 진단(없는 경로 경고·같은 경로·같은 id·
// 마운트), NeverCook(직접 지정 경고·규칙은 조용히·Hard·bundle 오류·Soft 제외·마운트 전체),
// 누락·같은 id 의존과 깨진 bundle 태그, 부분 쿠킹(인자 경로·역방향 Build 고정점·기존 매니페스트의
// 사라진 항목), 요청 거부, 결정적 순서, 할당 실패.
using namespace rocks;

namespace cook_targets_tests_file {

using namespace rocks::asset_test;
using rocks::asset_database_test::MemoryFileSystem;
using rocks::asset_database_test::MemoryRoot;
using C = AssetDiagnosticCode;
using K = PackageDependencyKind;

namespace {

enum class NodeType {
    Test,
    Base,
    Derived
};

struct Dep {
    uint64 id;
    PackageDependencyKind kind;
};

// 그래프 노드 하나다(Content 상대 파일 이름, id, 대표 타입, 의존, 태그). 같은 id 노드는 앞 노드의
// byte를 그대로 쓴다(같은 id 파일).
struct Node {
    StringView file;
    uint64 id;
    NodeType type = NodeType::Test;
    Span<const Dep> deps{};
    Span<const PackageTagInput> tags{};
};

// id 패키지 byte를 만든다: 대표 에셋 하나와 태그로 쓰고, Dependencies 섹션을 deps로 바꾼다(색인은
// 의존 섹션만 읽으므로 import 없이 어떤 그래프든 만든다 — Hard의 expectedType은 TestAsset).
Array<uint8> GraphPackage(AssetEnv& env, const Node& node) noexcept {
    Array<uint8> none(env.allocator);
    ObjectRegistry& registry = *env.source;
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(node.id), PackageKind::Editor);
    if (!made) {
        return none;
    }

    RootRef<Package> package = Move(made.Value());
    ObjectHandle asset{};
    if (node.type == NodeType::Test) {
        asset = Make<TestAsset>(registry, IdOf<TestAsset>(), package.Handle(), "");
    } else if (node.type == NodeType::Base) {
        asset = Make<BaseGameAsset>(registry, IdOf<BaseGameAsset>(), package.Handle(), "");
    } else {
        asset = Make<DerivedGameAsset>(registry, IdOf<DerivedGameAsset>(), package.Handle(), "");
    }

    auto tags = WritePackageTags(env.allocator, node.tags);
    if (asset.IsNull() || !tags) {
        return none;
    }

    PackageWriteOptions options = EditorOptions();
    options.tags = tags.Value().View();
    Array<uint8> bytes = Write(env.allocator, registry, package.Handle(), options);
    if (bytes.IsEmpty() || node.deps.IsEmpty()) {
        return bytes;
    }

    // (asset, kind) 순으로 정렬한다(읽기 규칙).
    Array<PackageDependencyEntry> entries(env.allocator);
    for (const Dep& dep : node.deps) {
        const PackageDependencyEntry entry{FixedId(dep.id), dep.kind,
                                           dep.kind == K::Hard ? TestAssetType() : TypeId{}, false};
        usize at = 0;
        while (at < entries.Size() && package_detail::DependencyLess(entries[at], entry)) {
            ++at;
        }

        if (!entries.TryInsert(entry, at)) {
            return none;
        }
    }

    EditablePackage edit(env.allocator);
    if (!edit.Parse(bytes.View())) {
        return none;
    }

    (void)edit.Remove(PackageSectionKind::Dependencies);
    const Array<uint8> section =
        DependenciesBytes(env.allocator, entries.View(), static_cast<uint32>(entries.Size()));
    if (edit.Insert(static_cast<uint32>(PackageSectionKind::Dependencies),
                    PackageSectionRequiredFlags(PackageSectionKind::Dependencies),
                    section.View()) == nullptr) {
        return none;
    }

    return edit.Build();
}

// 표준 문자열 id 표다(만든 뒤 자라지 않는다 — 태그 값 StringView가 가리킨다).
struct IdTexts {
    IdTexts(Allocator& allocator, Span<const uint64> ids) noexcept : texts(allocator) {
        ready = static_cast<bool>(texts.TryReserve(ids.Size()));
        for (uint64 id : ids) {
            auto text = TryFormat(allocator, "{}", FixedId(id));
            ready = ready && text && texts.TryPushBack(Move(text.Value()));
        }

        keys = ids;
    }

    StringView operator()(uint64 id) const noexcept {
        for (usize index = 0; index < keys.Size(); ++index) {
            if (keys[index] == id) {
                return texts[index].View();
            }
        }

        CHECK(false);
        return {};
    }

    Array<String> texts;
    Span<const uint64> keys;
    bool ready = false;
};

struct CookScene {
    explicit CookScene(AssetEnv& sceneEnv) noexcept
        : env(sceneEnv), layout(env.allocator), files(env.allocator, Initialized(layout)),
          index(env.allocator, layout), settings(env.allocator) {
        registered = static_cast<bool>(generated::AssetCooker::RegisterModuleTypes(env.modules));
    }

    static StringView Initialized(ProjectLayout& layout) noexcept {
        return layout.Initialize(MemoryRoot) ? layout.ContentDirectory() : StringView(MemoryRoot);
    }

    // 노드를 파일로 쓰고(부모 디렉터리를 만든다) 색인을 만든다.
    bool Build(Span<const Node> nodes) noexcept {
        if (!registered || !files.Ready()) {
            return false;
        }

        Array<Array<uint8>> made(env.allocator);
        for (usize at = 0; at < nodes.Size(); ++at) {
            const Node& node = nodes[at];
            Array<uint8> bytes(env.allocator);
            for (usize before = 0; before < at; ++before) {
                if (nodes[before].id == node.id && bytes.IsEmpty()) {
                    (void)bytes.TryAppend(made[before].View());
                }
            }

            if (bytes.IsEmpty()) {
                bytes = GraphPackage(env, node);
            }

            if (bytes.IsEmpty() || !AddParents(node.file) ||
                !files.AddFile(node.file, bytes.View()) || !made.TryPushBack(Move(bytes))) {
                return false;
            }
        }

        return static_cast<bool>(index.Rebuild(files, PackageReadOptions{}));
    }

    bool AddParents(StringView file) noexcept {
        for (usize at = 0; at < file.ByteCount(); ++at) {
            if (file.Data()[at] != '/') {
                continue;
            }

            const StringView parent(file.Data(), at);
            if (!files.Has(parent) && !files.AddDirectory(parent)) {
                return false;
            }
        }

        return true;
    }

    CookTargetRequest Request(Span<const StringView> paths = {},
                              const CookedManifest* existing = nullptr) noexcept {
        CookTargetRequest request;
        request.index = &index;
        request.types = &env.types;
        request.settings = &settings;
        request.paths = paths;
        request.existing = existing;
        return request;
    }

    void ResetSettings() noexcept {
        settings.alwaysCook.Clear();
        settings.neverCook.Clear();
        settings.primaryAssets.Clear();
        settings.cookSoftReferences = false;
    }

    AssetEnv& env;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetIndex index;
    CookSettings settings;
    bool registered = false;
};

bool Push(Array<String>& list, StringView text) noexcept {
    String value(list.GetAllocator());
    return value.TryAssign(text) && list.TryPushBack(Move(value));
}

bool AddRule(CookSettings& settings, StringView type, Span<const StringView> directories) noexcept {
    PrimaryAssetRule rule(settings.primaryAssets.GetAllocator());
    if (!rule.type.TryAssign(type)) {
        return false;
    }

    for (StringView directory : directories) {
        if (!Push(rule.directories, directory)) {
            return false;
        }
    }

    return static_cast<bool>(settings.primaryAssets.TryPushBack(Move(rule)));
}

AssetId IdOrNull(uint64 id) noexcept {
    return id == 0 ? AssetId{} : FixedId(id);
}

bool SameTargets(const CookTargetSet& set, Span<const uint64> ids) noexcept {
    if (set.targets.Size() != ids.Size()) {
        return false;
    }

    for (usize index = 0; index < ids.Size(); ++index) {
        if (!(set.targets[index].id == FixedId(ids[index]))) {
            return false;
        }
    }

    return true;
}

bool SameIds(const Array<AssetId>& list, Span<const uint64> ids) noexcept {
    if (list.Size() != ids.Size()) {
        return false;
    }

    for (usize index = 0; index < ids.Size(); ++index) {
        if (!(list[index] == FixedId(ids[index]))) {
            return false;
        }
    }

    return true;
}

const CookTarget* TargetOf(const CookTargetSet& set, uint64 id) noexcept {
    for (const CookTarget& target : set.targets) {
        if (target.id == FixedId(id)) {
            return &target;
        }
    }

    return nullptr;
}

// 기대 진단이다(asset·related 0은 null).
struct ExpectedDiagnostic {
    AssetDiagnosticCode code;
    bool warning;
    uint64 asset;
    uint64 related;
    uint32 setting;
};

// 시험 쪽 순서다((asset, code, related, setting, 오류 먼저) — 구현과 따로 정의한다).
bool DiagnosticBefore(const ExpectedDiagnostic& left, const ExpectedDiagnostic& right) noexcept {
    const AssetId leftAsset = IdOrNull(left.asset);
    const AssetId rightAsset = IdOrNull(right.asset);
    if (!(leftAsset == rightAsset)) {
        return leftAsset.value.high != rightAsset.value.high
                   ? leftAsset.value.high < rightAsset.value.high
                   : leftAsset.value.low < rightAsset.value.low;
    }

    if (left.code != right.code) {
        return static_cast<uint32>(left.code) < static_cast<uint32>(right.code);
    }

    if (left.related != right.related) {
        return left.related < right.related;
    }

    if (left.setting != right.setting) {
        return left.setting < right.setting;
    }

    return !left.warning && right.warning;
}

// diagnostics가 expected(순서 무관 — 시험 쪽 순서로 정렬해 대조)와 같은지 본다.
bool SameDiagnostics(const Array<CookDiagnostic>& diagnostics,
                     Span<const ExpectedDiagnostic> expected) noexcept {
    if (diagnostics.Size() != expected.Size()) {
        return false;
    }

    Array<ExpectedDiagnostic> sorted(diagnostics.GetAllocator());
    for (const ExpectedDiagnostic& item : expected) {
        usize at = 0;
        while (at < sorted.Size() && !DiagnosticBefore(item, sorted[at])) {
            ++at;
        }

        if (!sorted.TryInsert(item, at)) {
            return false;
        }
    }

    for (usize index = 0; index < sorted.Size(); ++index) {
        const CookDiagnostic& actual = diagnostics[index];
        const ExpectedDiagnostic& item = sorted[index];
        if (actual.code != item.code || actual.warning != item.warning ||
            !(actual.asset == IdOrNull(item.asset)) ||
            !(actual.related == IdOrNull(item.related)) || actual.setting != item.setting) {
            return false;
        }
    }

    return true;
}

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
// 색인에 없는 항목은 사라진 항목(같은 id 항목은 색인에 있음), 이번에 다시 쿠킹하지 않는 남은 항목의
// Hard 의존이 사라졌으면 오류 MissingDependency(다시 쿠킹하는 R의 옛 의존·색인에 있는 Hard·Soft는
// 아님, 사라진 항목끼리는 보지 않음). 전체 쿠킹은 기존 매니페스트를 보지 않는다.
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
    const ManifestRow rows[] = {{100, {{150, K::Hard}}, 1},
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
    const uint64 expected[] = {ArenaId, A2Id, U1Id, 120, 121, 122, 130, 131, 132};
    CHECK(SameTargets(out, expected));
    const CookTarget* arena = TargetOf(out, ArenaId);
    REQUIRE(arena != nullptr && arena->primary && arena->bundles.Size() == 1);
    CHECK(!TargetOf(out, 130)->primary);
    const ExpectedDiagnostic diagnostics[] = {
        {C::InvalidPath, false, 0, 0, 1},           {C::NeverCookReferenced, true, 72, 0, 2},
        {C::DuplicateAssetId, false, A2Id, 100, 0}, {C::DuplicateAssetId, false, A2Id, 100, 0},
        {C::MissingDependency, false, 100, 150, 0}, {C::MissingDependency, false, 140, 150, 0}};
    CHECK(SameDiagnostics(out.diagnostics, diagnostics));
    const uint64 removed[] = {150, 151};
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
