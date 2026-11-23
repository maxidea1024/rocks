#include "CookTestSupport.h"

#include <AssetCooker/CookKeys.h>
#include <Core/FileError.h>
#include <TestSupport/NFailAllocator.h>

// 쿠킹 키 시험이다(Docs/Design/AssetSystem.ko.md 10.2절 결정 5·6, "A4c 구현 세부" — A4c1): 처리기
// 표(등록 거부·가장 가까운 base·fallback·조회 오류), 키 기본(대상 순·콘텐츠 해시·서로 다름·결정성·
// 타입 표 지문), 입력마다 바뀌는 키(프로파일·프로젝트 세대·쿠커 버전·처리기 정체성·설정 키·Build
// 원본 내용·Hard 의존 내용·Hard 순환·Build 순환·PrimaryAsset·제외 Soft·경로·타입 표 — 바뀌지 않아야
// 할 키는 그대로), 실패(콘텐츠 해시·Build 의존 없음·같은 id·처리기 없음·종류 다름 — 기대는 에셋 키
// 없음), 요청 거부, 할당 실패.
using namespace rocks;

namespace cook_keys_tests_file {

using namespace rocks::cook_test;

namespace {

// 시험 처리기다(정체성·설정 키를 바꿔 가며 쓴다).
class TestProcessor final : public CookProcessor {
public:
    explicit TestProcessor(NuggetProcessorId id) noexcept : identity(id) {
    }

    NuggetProcessorId Identity() const noexcept override {
        return identity;
    }

    void WriteSettingsKey(NuggetKeyWriter& writer,
                          const AssetIndexEntry& entry) const noexcept override {
        (void)entry;
        if (failWriter) {
            writer.Fail(ErrorCode::InvalidArgument);
        } else if (writeSettings) {
            writer.WriteU32(settings);
        }
    }

    NuggetProcessorId identity;
    bool writeSettings = false;
    uint32 settings = 0;
    bool failWriter = false;
};

// 설정 키를 재정의하지 않는 처리기다(기본 WriteSettingsKey — 아무것도 쓰지 않는다).
class PlainProcessor final : public CookProcessor {
public:
    NuggetProcessorId Identity() const noexcept override {
        return NuggetProcessorId{Guid{0xFA11, 1}, Guid{0xFA11, 2}};
    }
};

constexpr NuggetProcessorId FallbackId{Guid{0xFA11, 1}, Guid{0xFA11, 2}};

// 키 그래프다. L(Maps) Hard → A(Base) Hard → A2(Derived), P(Maps) Soft → X(Never)·bundle Ui =
// A2(태그 — PrimaryAsset일 때만 따른다), T(Arrays) Build → B(Sources — 대상 아님), M1 ↔ M2 Hard
// 순환, N1(대상) ↔ N2(대상 아님) Build 순환.
constexpr uint64 LId = 10;
constexpr uint64 PId = 12;
constexpr uint64 AId = 20;
constexpr uint64 A2Id = 21;
constexpr uint64 TId = 30;
constexpr uint64 BId = 41;
constexpr uint64 M1Id = 50;
constexpr uint64 M2Id = 51;
constexpr uint64 N1Id = 60;
constexpr uint64 N2Id = 61;
constexpr uint64 XId = 70;
const Dep LDeps[] = {{AId, K::Hard}};
const Dep ADeps[] = {{A2Id, K::Hard}};
const Dep PDeps[] = {{XId, K::Soft}};
const Dep TDeps[] = {{BId, K::Build}};
const Dep M1Deps[] = {{M2Id, K::Hard}};
const Dep M2Deps[] = {{M1Id, K::Hard}};
const Dep N1Deps[] = {{N2Id, K::Build}};
const Dep N2Deps[] = {{N1Id, K::Build}};
const Node KeyNodes[] = {{"Maps/L.rasset", LId, NodeType::Test, LDeps},
                         {"Maps/P.rasset", PId, NodeType::Test, PDeps},
                         {"Props/A.rasset", AId, NodeType::Base, ADeps},
                         {"Props/A2.rasset", A2Id, NodeType::Derived},
                         {"Arrays/T.rasset", TId, NodeType::Test, TDeps},
                         {"Sources/B.rasset", BId},
                         {"Cycle/M1.rasset", M1Id, NodeType::Test, M1Deps},
                         {"Cycle/M2.rasset", M2Id, NodeType::Test, M2Deps},
                         {"Cycle/N1.rasset", N1Id, NodeType::Test, N1Deps},
                         {"Cycle/N2.rasset", N2Id, NodeType::Test, N2Deps},
                         {"Never/X.rasset", XId}};
// 대상(전체 쿠킹 기본 설정)이다.
const uint64 KeyTargets[] = {LId, PId, AId, A2Id, TId, M1Id, M2Id, N1Id};

bool SetKeySettings(CookSettings& settings) noexcept {
    const StringView always[] = {"/Game/Maps/", "/Game/Arrays/", "/Game/Cycle/M1",
                                 "/Game/Cycle/N1"};
    for (StringView path : always) {
        if (!Push(settings.alwaysCook, path)) {
            return false;
        }
    }

    return true;
}

const uint64 KeyTextIds[] = {A2Id};

// 장면 하나: 그래프·기본 설정·fallback 처리기 표.
struct KeyScene {
    explicit KeyScene(AssetEnv& env) noexcept
        : ids(env.allocator, KeyTextIds), scene(env), table(env.allocator, env.types, fallback),
          targets(env.allocator) {
        Node nodes[sizeof(KeyNodes) / sizeof(KeyNodes[0])];
        const PackageTagInput bundle[] = {{"Bundle.Ui", ids(A2Id)}};
        for (usize index = 0; index < sizeof(KeyNodes) / sizeof(KeyNodes[0]); ++index) {
            nodes[index] = KeyNodes[index];
            if (nodes[index].id == PId) {
                nodes[index].tags = bundle;
            }
        }

        ready = ids.ready && scene.Build(nodes) && SetKeySettings(scene.settings) && Retarget();
    }

    // 지금 설정으로 대상을 다시 계산한다(오류 진단 없음을 요구한다).
    bool Retarget() noexcept {
        return ComputeCookTargets(scene.env.allocator, scene.Request(), targets) &&
               !targets.HasErrors();
    }

    CookKeyRequest Request() noexcept {
        CookKeyRequest request;
        request.index = &scene.index;
        request.files = &scene.files;
        request.types = &scene.env.types;
        request.processors = &table;
        request.targets = &targets;
        request.profileSettings = Sha256::Hash(Span<const uint8>());
        return request;
    }

    Result<void> Compute(CookKeySet& out) noexcept {
        return ComputeCookKeys(scene.env.allocator, Request(), out);
    }

    IdTexts ids;
    CookScene scene;
    PlainProcessor fallback;
    CookProcessorTable table;
    CookTargetSet targets;
    bool ready = false;
};

const CookKey* KeyOf(const CookKeySet& set, uint64 id) noexcept {
    for (const CookKey& key : set.keys) {
        if (key.asset == FixedId(id)) {
            return &key;
        }
    }

    return nullptr;
}

// 두 결과에서 id의 build key·cookKey가 같은지다(둘 다 키가 있어야 한다).
bool SameBuild(const CookKeySet& left, const CookKeySet& right, uint64 id) noexcept {
    const CookKey* a = KeyOf(left, id);
    const CookKey* b = KeyOf(right, id);
    return a != nullptr && b != nullptr && a->hasKey && b->hasKey && a->buildKey == b->buildKey;
}

bool SameCook(const CookKeySet& left, const CookKeySet& right, uint64 id) noexcept {
    const CookKey* a = KeyOf(left, id);
    const CookKey* b = KeyOf(right, id);
    return a != nullptr && b != nullptr && a->hasKey && b->hasKey && a->cookKey == b->cookKey;
}

// ids의 키가 모두 바뀌었는지(changed)·그대로인지 본다.
bool CookChanged(const CookKeySet& left, const CookKeySet& right, Span<const uint64> ids,
                 bool changed) noexcept {
    for (uint64 id : ids) {
        const CookKey* a = KeyOf(left, id);
        const CookKey* b = KeyOf(right, id);
        if (a == nullptr || b == nullptr || !a->hasKey || !b->hasKey ||
            (a->cookKey == b->cookKey) == changed) {
            return false;
        }
    }

    return true;
}

bool BuildChanged(const CookKeySet& left, const CookKeySet& right, Span<const uint64> ids,
                  bool changed) noexcept {
    for (uint64 id : ids) {
        const CookKey* a = KeyOf(left, id);
        const CookKey* b = KeyOf(right, id);
        if (a == nullptr || b == nullptr || !a->hasKey || !b->hasKey ||
            (a->buildKey == b->buildKey) == changed) {
            return false;
        }
    }

    return true;
}

// 처음 계산한 결과를 기준으로 둔다.
bool Baseline(KeyScene& scene, CookKeySet& out) noexcept {
    return scene.Compute(out) && out.diagnostics.IsEmpty() &&
           out.keys.Size() == scene.targets.targets.Size();
}

Span<const char> Chars(const char* text) noexcept {
    usize count = 0;
    while (text[count] != 0) {
        ++count;
    }

    return Span<const char>(text, count);
}

} // namespace

// 처리기 표: 0 type·같은 type 등록 거부, 정확한 type·가장 가까운 base(Derived → Base 처리기)·등록
// 없음은 fallback, 미등록 type NotFound, 0 type InvalidArgument, 등록 할당 실패.
TEST(CookProcessorTable_RegisterAndFind) {
    AssetEnv env;
    REQUIRE(env.Ready());
    TestProcessor fallback(FallbackId);
    TestProcessor baseProcessor(NuggetProcessorId{Guid{0xBA5E, 1}, Guid{0xBA5E, 2}});
    TestProcessor testProcessor(NuggetProcessorId{Guid{0x7E57, 1}, Guid{0x7E57, 2}});
    CookProcessorTable table(env.allocator, env.types, fallback);
    CHECK(table.Register(TypeId{}, baseProcessor).Error() == ErrorCode::InvalidArgument);
    REQUIRE(table.Register(IdOf<BaseGameAsset>(), baseProcessor));
    CHECK(table.Register(IdOf<BaseGameAsset>(), testProcessor).Error() == ErrorCode::AlreadyExists);
    CHECK(table.Count() == 1);
    auto base = table.Find(IdOf<BaseGameAsset>());
    auto derived = table.Find(IdOf<DerivedGameAsset>());
    auto test = table.Find(TestAssetType());
    REQUIRE(base && derived && test);
    CHECK(base.Value() == &baseProcessor && derived.Value() == &baseProcessor);
    CHECK(test.Value() == &fallback);
    REQUIRE(table.Register(IdOf<DerivedGameAsset>(), testProcessor));
    auto nearest = table.Find(IdOf<DerivedGameAsset>());
    REQUIRE(nearest);
    CHECK(nearest.Value() == &testProcessor);
    CHECK(table.Find(Guid{0x404, 0x404}).Error() == ErrorCode::NotFound);
    CHECK(table.Find(TypeId{}).Error() == ErrorCode::InvalidArgument);

    test::NFailAllocator failing(env.allocator);
    CookProcessorTable fresh(failing, env.types, fallback);
    failing.FailOnAttempt(1);
    CHECK(fresh.Register(TestAssetType(), testProcessor).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    CHECK(fresh.Count() == 0);
}

// 키 기본: 대상 순으로 모두 키가 있고 콘텐츠 해시는 파일 전체 SHA-256, 키는 서로 다르며(build
// key도) 다시 계산하면 같다. 타입 표 지문은 HashTypeTable과 같다. Build로만 닿는 에셋(B·N2)은 키
// 목록에 없다. out은 비우고 채운다.
TEST(CookKeys_BasicAndDeterministic) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyScene scene(env);
    REQUIRE(scene.ready);
    REQUIRE(SameTargets(scene.targets, KeyTargets));
    CookKeySet keys(env.allocator);
    REQUIRE(keys.diagnostics.TryPushBack(CookDiagnostic{}));
    REQUIRE(Baseline(scene, keys));
    CHECK(!keys.HasErrors());
    for (usize index = 0; index < keys.keys.Size(); ++index) {
        CHECK(keys.keys[index].asset == FixedId(KeyTargets[index]) && keys.keys[index].hasKey);
        for (usize other = 0; other < index; ++other) {
            CHECK(!(keys.keys[index].cookKey == keys.keys[other].cookKey));
            CHECK(!(keys.keys[index].buildKey == keys.keys[other].buildKey));
        }
    }

    CHECK(KeyOf(keys, BId) == nullptr && KeyOf(keys, N2Id) == nullptr);
    CHECK(KeyOf(keys, LId)->contentHash == Sha256::Hash(scene.scene.files.Bytes("Maps/L.rasset")));
    auto typeTable = HashTypeTable(env.allocator, env.types);
    REQUIRE(typeTable);
    CHECK(keys.typeTable == typeTable.Value());

    CookKeySet again(env.allocator);
    REQUIRE(Baseline(scene, again));
    CHECK(CookChanged(keys, again, KeyTargets, false) &&
          BuildChanged(keys, again, KeyTargets, false));
}

// 입력마다: 바뀌어야 할 키만 바뀐다.
TEST(CookKeys_InputsChangeKeys) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyScene scene(env);
    REQUIRE(scene.ready);
    CookKeySet base(env.allocator);
    REQUIRE(Baseline(scene, base));
    CookKeySet changed(env.allocator);

    // 프로파일 출력 설정: 모든 cookKey, build key는 그대로.
    CookKeyRequest request = scene.Request();
    request.profileSettings =
        Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("p"), 1));
    REQUIRE(ComputeCookKeys(env.allocator, request, changed));
    CHECK(CookChanged(base, changed, KeyTargets, true) &&
          BuildChanged(base, changed, KeyTargets, false));

    // 프로젝트 세대: 둘 다.
    request = scene.Request();
    request.projectGeneration = Guid{0x6E6, 1};
    REQUIRE(ComputeCookKeys(env.allocator, request, changed));
    CHECK(CookChanged(base, changed, KeyTargets, true) &&
          BuildChanged(base, changed, KeyTargets, true));

    // 쿠커 버전: cookKey만.
    request = scene.Request();
    request.cookerVersion = CookerVersion + 1;
    REQUIRE(ComputeCookKeys(env.allocator, request, changed));
    CHECK(CookChanged(base, changed, KeyTargets, true) &&
          BuildChanged(base, changed, KeyTargets, false));

    // 처리기 정체성: Base 처리기 → A(Base)·A2(Derived)의 build·cook, Hard로 기대는 L의 cook.
    // 나머지는 그대로.
    TestProcessor baseProcessor(NuggetProcessorId{Guid{0xBA5E, 1}, Guid{0xBA5E, 2}});
    REQUIRE(scene.table.Register(IdOf<BaseGameAsset>(), baseProcessor));
    REQUIRE(scene.Compute(changed));
    const uint64 baseTyped[] = {AId, A2Id};
    const uint64 hardOnBase[] = {LId};
    const uint64 untouched[] = {PId, TId, M1Id, M2Id, N1Id};
    CHECK(BuildChanged(base, changed, baseTyped, true) &&
          CookChanged(base, changed, baseTyped, true));
    CHECK(BuildChanged(base, changed, hardOnBase, false) &&
          CookChanged(base, changed, hardOnBase, true));
    CHECK(BuildChanged(base, changed, untouched, false) &&
          CookChanged(base, changed, untouched, false));

    // 처리기 버전 Guid만 바꿔도 같다.
    CookKeySet versioned(env.allocator);
    baseProcessor.identity.version = Guid{0xBA5E, 3};
    REQUIRE(scene.Compute(versioned));
    CHECK(BuildChanged(changed, versioned, baseTyped, true));

    // 설정 키: 쓰기 시작·값이 바뀌면 그 처리기 에셋의 build key가 바뀐다.
    CookKeySet settings(env.allocator);
    baseProcessor.writeSettings = true;
    REQUIRE(scene.Compute(settings));
    CHECK(BuildChanged(versioned, settings, baseTyped, true));
    CookKeySet settings2(env.allocator);
    baseProcessor.settings = 7;
    REQUIRE(scene.Compute(settings2));
    CHECK(BuildChanged(settings, settings2, baseTyped, true) &&
          BuildChanged(settings, settings2, untouched, false));
}

// 내용: Build 원본(B)만 바꾸면 그것을 Build로 쓰는 T의 build·cook이 바뀌고 나머지는 그대로다. Hard
// 의존(A2) 내용이 바뀌면 A2·A·L의 cook이 바뀌고 A·L의 build는 그대로다. Hard 순환(M1 ↔ M2)은 한쪽
// 내용만 바뀌어도 둘 다(시나리오 28), Build 순환(N1 ↔ N2)은 대상 아닌 N2 내용으로 N1의 build·cook이
// 바뀐다. 경로만 바뀌면(같은 byte) cook만 바뀐다.
TEST(CookKeys_ContentAndCycles) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyScene scene(env);
    REQUIRE(scene.ready);
    CookKeySet base(env.allocator);
    REQUIRE(Baseline(scene, base));
    const PackageTagInput revision[] = {{"Rev", "2"}};

    const Node b2{"Sources/B.rasset", BId, NodeType::Test, {}, revision};
    REQUIRE(scene.scene.Replace(b2));
    REQUIRE(scene.Retarget());
    CookKeySet build(env.allocator);
    REQUIRE(Baseline(scene, build));
    const uint64 array[] = {TId};
    const uint64 others[] = {LId, PId, AId, A2Id, M1Id, M2Id, N1Id};
    CHECK(BuildChanged(base, build, array, true) && CookChanged(base, build, array, true));
    CHECK(BuildChanged(base, build, others, false) && CookChanged(base, build, others, false));

    const Node a2{"Props/A2.rasset", A2Id, NodeType::Derived, {}, revision};
    REQUIRE(scene.scene.Replace(a2));
    REQUIRE(scene.Retarget());
    CookKeySet hard(env.allocator);
    REQUIRE(Baseline(scene, hard));
    const uint64 hardChain[] = {A2Id, AId, LId};
    const uint64 hardHolders[] = {AId, LId};
    const uint64 a2Only[] = {A2Id};
    CHECK(CookChanged(build, hard, hardChain, true) &&
          BuildChanged(build, hard, hardHolders, false) && BuildChanged(build, hard, a2Only, true));

    const Node m2{"Cycle/M2.rasset", M2Id, NodeType::Test, M2Deps, revision};
    REQUIRE(scene.scene.Replace(m2));
    REQUIRE(scene.Retarget());
    CookKeySet cycle(env.allocator);
    REQUIRE(Baseline(scene, cycle));
    const uint64 hardCycle[] = {M1Id, M2Id};
    const uint64 m1Only[] = {M1Id};
    CHECK(CookChanged(hard, cycle, hardCycle, true) && BuildChanged(hard, cycle, m1Only, false));

    const Node n2{"Cycle/N2.rasset", N2Id, NodeType::Test, N2Deps, revision};
    REQUIRE(scene.scene.Replace(n2));
    REQUIRE(scene.Retarget());
    CookKeySet buildCycle(env.allocator);
    REQUIRE(Baseline(scene, buildCycle));
    const uint64 n1[] = {N1Id};
    CHECK(BuildChanged(cycle, buildCycle, n1, true) && CookChanged(cycle, buildCycle, n1, true));

    // 경로: 같은 byte를 다른 경로로 옮기면 cook만 바뀐다.
    const Span<const uint8> tBytes = scene.scene.files.Bytes("Arrays/T.rasset");
    Array<uint8> copy(env.allocator);
    REQUIRE(copy.TryAppend(tBytes));
    scene.scene.files.Remove("Arrays/T.rasset");
    REQUIRE(scene.scene.files.AddFile("Arrays/T2.rasset", copy.View()));
    REQUIRE(scene.scene.index.Rebuild(scene.scene.files, PackageReadOptions{}));
    REQUIRE(scene.Retarget());
    CookKeySet moved(env.allocator);
    REQUIRE(Baseline(scene, moved));
    CHECK(CookChanged(buildCycle, moved, array, true) &&
          BuildChanged(buildCycle, moved, array, false));
}

// 판정: PrimaryAsset이 되면 그 에셋 cook만(P는 bundle Ui = A2도 키에 든다), Soft 설정은 Hard
// 그래프에 들어가지 않고(P 그대로) 제외된 Soft 의존이 생기면 P cook이 바뀐다. 타입 표(타입을 하나
// 더 등록)가 바뀌면 모든 cook, build는 그대로다.
TEST(CookKeys_DecisionsAndTypeTable) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyScene scene(env);
    REQUIRE(scene.ready);
    CookKeySet base(env.allocator);
    REQUIRE(Baseline(scene, base));

    const StringView maps[] = {"/Game/Maps/"};
    REQUIRE(AddRule(scene.scene.settings, "TestAsset", maps));
    REQUIRE(scene.Retarget());
    CookKeySet primary(env.allocator);
    REQUIRE(Baseline(scene, primary));
    const uint64 mapIds[] = {LId, PId};
    const uint64 rest[] = {AId, A2Id, TId, M1Id, M2Id, N1Id};
    CHECK(CookChanged(base, primary, mapIds, true) && CookChanged(base, primary, rest, false));
    const CookTarget* pTarget = TargetOf(scene.targets, PId);
    REQUIRE(pTarget != nullptr && pTarget->primary && pTarget->bundles.Size() == 1);

    // Soft 설정만(제외 없음): X가 대상이 되지만 P의 키는 그대로.
    scene.scene.settings.primaryAssets.Clear();
    scene.scene.settings.cookSoftReferences = true;
    REQUIRE(scene.Retarget());
    CookKeySet soft(env.allocator);
    REQUIRE(Baseline(scene, soft));
    const uint64 p[] = {PId};
    CHECK(CookChanged(base, soft, p, true) == false && SameCook(base, soft, PId));
    // X가 NeverCook이면 P의 제외 Soft 의존이 생긴다.
    REQUIRE(Push(scene.scene.settings.neverCook, "/Game/Never/"));
    REQUIRE(scene.Retarget());
    REQUIRE(scene.targets.excluded.Size() == 1);
    CookKeySet excluded(env.allocator);
    REQUIRE(Baseline(scene, excluded));
    CHECK(CookChanged(soft, excluded, p, true) && SameBuild(soft, excluded, PId));

    // 타입 표: enum 하나를 더 등록하면 모든 cook이 바뀌고 build는 그대로다.
    scene.scene.settings.neverCook.Clear();
    scene.scene.settings.cookSoftReferences = false;
    REQUIRE(scene.Retarget());
    static const char Marker = 0;
    const EnumeratorSchema values[] = {{Chars("One"), {}, {}, 0, 0}};
    const EnumSchema extra{Guid{0xE47A, 1},
                           Chars("cook_keys::Extra"),
                           ValueKind::UInt8,
                           0u,
                           Span<const EnumeratorSchema>(values, 1),
                           {}};
    auto token = env.types.RegisterEnum(extra, RuntimeEnumOps{&Marker, {}});
    REQUIRE(token);
    CookKeySet typed(env.allocator);
    REQUIRE(Baseline(scene, typed));
    CHECK(!(typed.typeTable == base.typeTable));
    CHECK(CookChanged(base, typed, KeyTargets, true) &&
          BuildChanged(base, typed, KeyTargets, false));
    REQUIRE(env.types.Unregister(token.Value()));
}

// 실패: 콘텐츠 해시(파일 없음 MissingFile·읽기 실패 None — error 칸), Build 의존 없음
// MissingDependency·같은 id DuplicateAssetId(asset = 참조자), 처리기 없음(미등록 →
// UnsupportedVersion (error NotFound)·종류 다름 → TypeMismatch(error TypeMismatch)). 원인 에셋과
// 그것에 Hard·Build로 기대는 에셋은 키가 없고 진단은 원인에만 있다.
TEST(CookKeys_Failures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyScene scene(env);
    REQUIRE(scene.ready);
    const Dep yDeps[] = {{999, K::Build}};
    const Dep wDeps[] = {{100, K::Build}};
    const Dep hDeps[] = {{XId, K::Hard}, {XId, K::Build}};
    const Node extra[] = {{"Arrays/Y.rasset", 31, NodeType::Test, yDeps},
                          {"Arrays/W.rasset", 32, NodeType::Test, wDeps},
                          {"Arrays/H.rasset", 33, NodeType::Test, hDeps},
                          {"Dup/D1.rasset", 100},
                          {"Dup/D2.rasset", 100}};
    for (const Node& node : extra) {
        Array<uint8> bytes = GraphPackage(env, *env.source, node);
        if (node.id == 100 && scene.scene.files.Has("Dup/D1.rasset")) {
            bytes.Clear();
            REQUIRE(bytes.TryAppend(scene.scene.files.Bytes("Dup/D1.rasset")));
        }

        REQUIRE(!bytes.IsEmpty() && scene.scene.AddParents(node.file) &&
                scene.scene.files.AddFile(node.file, bytes.View()));
    }

    REQUIRE(scene.scene.index.Rebuild(scene.scene.files, PackageReadOptions{}));
    // X는 NeverCook이다 — H의 Hard 참조는 A4b 오류이고, X는 Build로만 영역에 든다(대상 밖 Hard
    // 간선은 키 그래프에서 빠진다).
    REQUIRE(Push(scene.scene.settings.neverCook, "/Game/Never/"));
    REQUIRE(ComputeCookTargets(env.allocator, scene.scene.Request(), scene.targets));
    REQUIRE(scene.targets.HasErrors() && TargetOf(scene.targets, XId) == nullptr);
    // 색인 뒤 파일이 사라지고(A2) 읽기가 실패한다(B — Build 원본).
    scene.scene.files.Remove("Props/A2.rasset");
    Array<uint8> bBytes(env.allocator);
    REQUIRE(bBytes.TryAppend(scene.scene.files.Bytes("Sources/B.rasset")));
    scene.scene.files.Remove("Sources/B.rasset");
    REQUIRE(scene.scene.files.AddFile("Sources/B.rasset", bBytes.View(), MemoryFault::Read));

    CookKeySet keys(env.allocator);
    REQUIRE(scene.Compute(keys));
    const ExpectedDiagnostic diagnostics[] = {
        {C::MissingFile, false, A2Id, 0, 0, ErrorCode::NotFound},
        {C::None, false, BId, 0, 0, FileErrorCode(asset_database_test::MemoryError)},
        {C::MissingDependency, false, 31, 999, 0},
        {C::DuplicateAssetId, false, 32, 100, 0}};
    CHECK(SameDiagnostics(keys.diagnostics, diagnostics));
    CHECK(keys.HasErrors());
    const uint64 noKey[] = {LId, AId, A2Id, TId, 31, 32};
    const uint64 withKey[] = {PId, M1Id, M2Id, N1Id, 33};
    for (uint64 id : noKey) {
        CHECK(KeyOf(keys, id) != nullptr && !KeyOf(keys, id)->hasKey);
    }

    for (uint64 id : withKey) {
        CHECK(KeyOf(keys, id) != nullptr && KeyOf(keys, id)->hasKey);
    }

    // 처리기를 못 고름: 빈 Registry 표는 모든 노드가 UnsupportedVersion(NotFound), TestAsset id가
    // enum인 Registry 표는 TestAsset 노드가 TypeMismatch(Base·Derived는 NotFound).
    scene.scene.files.Remove("Sources/B.rasset");
    REQUIRE(scene.scene.files.AddFile("Sources/B.rasset", bBytes.View()));
    TypeRegistry empty(env.allocator);
    CookProcessorTable emptyTable(env.allocator, empty, scene.fallback);
    CookKeyRequest request = scene.Request();
    request.processors = &emptyTable;
    REQUIRE(ComputeCookKeys(env.allocator, request, keys));
    usize notFound = 0;
    for (const CookDiagnostic& diagnostic : keys.diagnostics) {
        notFound +=
            diagnostic.code == C::UnsupportedVersion && diagnostic.error == ErrorCode::NotFound;
    }

    CHECK(notFound >= 10);
    for (const CookKey& key : keys.keys) {
        CHECK(!key.hasKey);
    }

    TypeRegistry clashing(env.allocator);
    static const char Marker = 0;
    const EnumeratorSchema values[] = {{Chars("One"), {}, {}, 0, 0}};
    const EnumSchema asEnum{TestAssetType(),
                            Chars("cook_keys::Clash"),
                            ValueKind::UInt8,
                            0u,
                            Span<const EnumeratorSchema>(values, 1),
                            {}};
    REQUIRE(clashing.RegisterEnum(asEnum, RuntimeEnumOps{&Marker, {}}));
    CookProcessorTable clashTable(env.allocator, clashing, scene.fallback);
    request.processors = &clashTable;
    REQUIRE(ComputeCookKeys(env.allocator, request, keys));
    bool sawMismatch = false;
    for (const CookDiagnostic& diagnostic : keys.diagnostics) {
        sawMismatch = sawMismatch ||
                      (diagnostic.asset == FixedId(LId) && diagnostic.code == C::TypeMismatch &&
                       diagnostic.error == ErrorCode::TypeMismatch);
    }

    CHECK(sawMismatch);
    REQUIRE(clashing.Shutdown());
}

// 요청 거부: 인자 null InvalidArgument(out 비움), 다른 색인의 대상 InvalidArgument, 종료한
// TypeRegistry(요청·처리기 표) ShuttingDown, 처리기 설정 키의 writer 오류는 그대로.
TEST(CookKeys_Rejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyScene scene(env);
    REQUIRE(scene.ready);
    CookKeySet keys(env.allocator);
    REQUIRE(keys.keys.TryPushBack(CookKey{}));
    CookKeyRequest request = scene.Request();
    request.index = nullptr;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::InvalidArgument);
    CHECK(keys.keys.IsEmpty());
    request = scene.Request();
    request.files = nullptr;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::InvalidArgument);
    request = scene.Request();
    request.types = nullptr;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::InvalidArgument);
    request = scene.Request();
    request.processors = nullptr;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::InvalidArgument);
    request = scene.Request();
    request.targets = nullptr;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::InvalidArgument);

    CookTargetSet foreign(env.allocator);
    REQUIRE(foreign.targets.TryEmplaceBack(env.allocator));
    foreign.targets[0].id = FixedId(999);
    request = scene.Request();
    request.targets = &foreign;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::InvalidArgument);

    TypeRegistry closed(env.allocator);
    REQUIRE(closed.Shutdown());
    request = scene.Request();
    request.types = &closed;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::ShuttingDown);
    CookProcessorTable closedTable(env.allocator, closed, scene.fallback);
    request = scene.Request();
    request.processors = &closedTable;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::ShuttingDown);

    TestProcessor failing(FallbackId);
    failing.failWriter = true;
    CookProcessorTable failingTable(env.allocator, env.types, failing);
    request = scene.Request();
    request.processors = &failingTable;
    CHECK(ComputeCookKeys(env.allocator, request, keys).Error() == ErrorCode::InvalidArgument);
}

// 할당 실패: 실패 장면(진단 있음·Build·Hard 순환)의 모든 할당 지점에서 OutOfMemory이고, 지점을 다
// 지나면 기준과 키·진단이 같다.
TEST(CookKeys_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    KeyScene scene(env);
    REQUIRE(scene.ready);
    const Dep yDeps[] = {{999, K::Build}};
    const Node missing{"Arrays/Y.rasset", 31, NodeType::Test, yDeps};
    Array<uint8> bytes = GraphPackage(env, *env.source, missing);
    REQUIRE(!bytes.IsEmpty() && scene.scene.files.AddFile(missing.file, bytes.View()));
    REQUIRE(scene.scene.index.Rebuild(scene.scene.files, PackageReadOptions{}));
    REQUIRE(scene.Retarget());
    CookKeySet baseline(env.allocator);
    REQUIRE(scene.Compute(baseline));
    REQUIRE(baseline.diagnostics.Size() == 1);

    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        // 콘텐츠 해시는 색인에 남으므로 매번 다시 계산하게 시각을 옮긴다.
        ++scene.scene.files.modifiedShift;
        CookKeySet out(failing);
        failing.FailOnAttempt(attempt);
        auto computed = ComputeCookKeys(failing, scene.Request(), out);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(computed);
            REQUIRE(out.keys.Size() == baseline.keys.Size());
            for (usize index = 0; index < out.keys.Size(); ++index) {
                CHECK(out.keys[index].hasKey == baseline.keys[index].hasKey &&
                      out.keys[index].cookKey == baseline.keys[index].cookKey);
            }

            CHECK(out.diagnostics.Size() == baseline.diagnostics.Size());
            break;
        }

        CHECK(computed.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures >= 10);

    // 색인 allocator의 실패(콘텐츠 해시의 경로·읽기 버퍼)도 OutOfMemory로 돌아온다.
    test::NFailAllocator indexFailing(env.allocator);
    AssetIndex failingIndex(indexFailing, scene.scene.layout);
    REQUIRE(failingIndex.Rebuild(scene.scene.files, PackageReadOptions{}));
    CookTargetRequest targetRequest = scene.scene.Request();
    targetRequest.index = &failingIndex;
    CookTargetSet targets(env.allocator);
    REQUIRE(ComputeCookTargets(env.allocator, targetRequest, targets));
    uint32 indexFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        ++scene.scene.files.modifiedShift;
        CookKeyRequest request = scene.Request();
        request.index = &failingIndex;
        request.targets = &targets;
        CookKeySet out(env.allocator);
        indexFailing.FailOnAttempt(attempt);
        auto computed = ComputeCookKeys(env.allocator, request, out);
        const bool failed = indexFailing.Attempts() >= attempt;
        indexFailing.DisableFailure();
        if (!failed) {
            REQUIRE(computed);
            CHECK(out.keys.Size() == baseline.keys.Size());
            break;
        }

        CHECK(computed.Error() == ErrorCode::OutOfMemory);
        ++indexFailures;
    }

    CHECK(indexFailures >= 2);
}

} // namespace cook_keys_tests_file
