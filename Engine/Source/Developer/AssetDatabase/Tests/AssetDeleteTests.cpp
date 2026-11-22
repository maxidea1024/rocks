#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "AssetDatabaseTestSupport.h"
#include "AssetIndexCompare.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetAliasTable.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>

// 삭제 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 17 A2e3a): 집합의 간선 표(관찰자 묶음 적용 —
// 열기·편집·닫기, 쌍별 합산, 할당 실패에도 묶음을 잃지 않음), 닫기(들어오는 참조 Busy·Discard·
// 초기화 안 Busy), 색인 빼기(removed), 서비스 삭제(색인 항목·열린 패키지·저장하지 않은 새 패키지·
// 디스크 참조자 거부와 force·메모리 참조 Busy·별칭 지우기·모든 거부와 실패 — 파일·별칭·색인
// 그대로, 지우기 실패의 별칭 되돌리기·되돌리기 실패 경고·NotDurable·할당 실패), 실제 디스크 삭제.
using namespace rocks;
namespace asset_delete_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;

namespace {

constexpr StringView AliasFile = "Config/AssetAliases.json";

PackageReadOptions ReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.engineVersion = TestEngineVersion;
    return options;
}

int64 StartTicks() noexcept {
    auto parsed = TryParseIso8601("2026-11-22T00:00:00Z");
    CHECK(parsed);
    return parsed ? parsed.Value().Ticks() : 0;
}

Span<const uint8> BytesOf(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

// 삭제 장면이다: 루트는 디스크 임시 디렉터리(잠금), 파일은 그 루트에 놓인 메모리 파일 시스템 —
// Content에 Base·Derived·Dir/Hero(bulk — Base Hard·Derived EditorOnly)·Villain(Hero Hard 둘)·
// Leaf·Solo(CycleB — 아무도 참조하지 않는다), Config, 잡은 잠금.
struct DeleteScene {
    DeleteScene(AssetEnv& env, Sources& sources, const char* tag) noexcept
        : root(tag), layout(env.allocator), files(env.allocator, Initialized(layout, root)),
          hooks(env.allocator, env.types), clock(StartTicks()) {
        ready = root.made && !layout.Root().IsEmpty() && files.Ready() &&
                files.AddDirectory("Content") && files.AddDirectory("Config") &&
                files.AddDirectory("Content/Dir") &&
                files.AddFile("Content/Base.rasset", sources.base.View()) &&
                files.AddFile("Content/Derived.rasset", sources.derived.View()) &&
                files.AddFile("Content/Dir/Hero.rasset", sources.hero.View()) &&
                files.AddFile("Content/Villain.rasset", sources.villain.View()) &&
                files.AddFile("Content/Leaf.rasset", sources.leaf.View()) &&
                files.AddFile("Content/Solo.rasset", sources.cycleB.View()) &&
                static_cast<bool>(lock.Acquire(layout, 0));
    }

    static StringView Initialized(ProjectLayout& layout, const TempProject& project) noexcept {
        return layout.Initialize(project.Root()) ? layout.Root() : project.Root();
    }

    AssetServiceConfig Config(AssetEnv& env) noexcept {
        AssetServiceConfig config;
        config.packages.types = &env.types;
        config.packages.files = &files;
        config.packages.hooks = &hooks;
        config.packages.read.engineVersion = TestEngineVersion;
        config.packages.registryContext = ContextId{0x44454c45ull, 1};
        config.packages.registryOptions.names = &env.names;
        config.clock = &clock;
        return config;
    }

    StringView AliasText() const noexcept {
        const Span<const uint8> bytes = files.Bytes(AliasFile);
        return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    }

    bool Put(StringView relative, Span<const uint8> bytes,
             MemoryFault fault = MemoryFault::None) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, bytes, fault);
    }

    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetTypeHookTable hooks;
    ManualAssetClock clock;
    AssetDatabaseLock lock;
    bool ready = false;
};

bool Matches(AssetEnv& env, DeleteScene& scene, const AssetIndex& index) noexcept {
    AssetIndex fresh(env.allocator, scene.layout);
    return fresh.Rebuild(scene.files, ReadOptions()) && SameIndex(fresh, index);
}

Result<ObjectHandle> CreateUnsaved(AssetEnv& env, EditorPackageSet& set, StringView path,
                                   uint64 id) noexcept {
    auto parsed = ParseLogicalPath(env.allocator, path);
    if (!parsed) {
        return Result<ObjectHandle>::Failure(parsed.Error());
    }

    return set.Create(parsed.Value(), IdOf<TestAsset>(), FixedId(id));
}

// 별칭 둘(Leaf ← /Game/Old/Leaf, Base ← /Game/Old/Base)을 쓴다.
bool WriteAliases(DeleteScene& scene, AssetService& service) noexcept {
    auto aliases = service.Aliases();
    auto created = DateTime::TryFromTicks(scene.clock.now);
    if (!aliases || !created) {
        return false;
    }

    const AssetAliasInput adds[] = {{"/Game/Old/Leaf", FixedId(LeafId), created.Value(), 90},
                                    {"/Game/Old/Base", FixedId(BaseId), created.Value(), 90}};
    bool notDurable = false;
    return static_cast<bool>(aliases.Value()->Write(scene.files, scene.lock, {}, adds, notDurable));
}

usize Incoming(EditorPackageSet& set, uint64 id, AssetId* first = nullptr) noexcept {
    auto incoming = set.IncomingReferences(FixedId(id), first);
    CHECK(incoming);
    return incoming ? incoming.Value() : 0;
}

// 대표 에셋의 Hard 참조를 바꾼다(target이 null이면 끊는다).
bool SetHard(EditorPackageSet& set, uint64 owner, ObjectHandle target, bool typed) noexcept {
    const ObjectHandle representative = RepresentativeOf(set, owner);
    TestAsset* value =
        representative.IsNull() ? nullptr : Pointer<TestAsset>(set.Registry(), representative);
    if (value == nullptr) {
        return false;
    }

    return typed ? static_cast<bool>(set.Registry().SetReference(set.Access(), representative,
                                                                 value->typed, target))
                 : static_cast<bool>(set.Registry().SetReference(set.Access(), representative,
                                                                 value->hard, target));
}

// 초기화 안에서 닫기를 부른다(Busy여야 한다).
class CloseInside final : public EditorAssetInitializer {
public:
    CloseInside(EditorPackageSet& set, AssetId target) noexcept : set_(&set), target_(target) {
    }

    Result<void> Initialize(ConstructionContext&, ObjectHandle) noexcept override {
        AssetDiagnostic failure;
        auto closed = set_->Close(target_, &failure);
        error = closed ? ErrorCode::None : closed.Error();
        return Result<void>::Success();
    }

    ErrorCode error = ErrorCode::None;

private:
    EditorPackageSet* set_;
    AssetId target_;
};

// 자기 자신을 Soft로 가리키는 패키지(Selfish — 자기 의존은 기록되지 않아 참조자가 없다)다.
constexpr uint64 SelfishId = 0xe30;

Array<uint8> SelfReferencing(AssetEnv& env, RootRef<Package>& package) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectHandle asset = MakeAsset<TestAsset>(registry, SelfishId, package);
    TestAsset* value = asset.IsNull() ? nullptr : Pointer<TestAsset>(registry, asset);
    if (value == nullptr || !value->soft.TryAssign(FixedId(SelfishId), "")) {
        return Array<uint8>(env.allocator);
    }

    return Write(env.allocator, registry, package.Handle(), EditorOptions());
}

} // namespace

// 간선 표와 닫기: 열기(Villain → Hero 다중도 2·Hero → Base·Derived), 편집(끊기·다시 잇기 — 묶음
// 안 합산), 닫기(들어오는 참조 ReferencedBy / Busy·관련 id, 닫으면 나가는 간선 반납·Find 없음·
// 다시 닫기 NotFound), 저장하지 않은 새 패키지 닫기, 초기화 안 닫기 Busy, Initialize 전
// InvalidArgument, 열리지 않은 id는 0.
TEST(AssetDelete_EdgeTableAndClose) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    REQUIRE(project.files.AddFile("Base.rasset", sources.base.View()));
    REQUIRE(project.files.AddFile("Derived.rasset", sources.derived.View()));
    REQUIRE(project.files.AddFile("Hero.rasset", sources.hero.View()));
    REQUIRE(project.files.AddFile("Villain.rasset", sources.villain.View()));
    REQUIRE(project.files.AddFile("Leaf.rasset", sources.leaf.View()));
    REQUIRE(project.Rebuild());
    EditorPackageSet set(env.allocator, Config(env, project));
    AssetDiagnostic failure;
    CHECK(set.SyncReferences().Error() == ErrorCode::InvalidArgument);
    CHECK(set.IncomingReferences(FixedId(HeroId)).Error() == ErrorCode::InvalidArgument);
    CHECK(set.Close(FixedId(HeroId), &failure).Error() == ErrorCode::InvalidArgument);
    REQUIRE(set.Initialize());
    REQUIRE(set.SyncReferences());
    CHECK(Incoming(set, HeroId) == 0);

    REQUIRE(set.Open(FixedId(VillainId)));
    AssetId first;
    CHECK(Incoming(set, HeroId, &first) == 2 && first == FixedId(VillainId));
    CHECK(Incoming(set, BaseId, &first) == 1 && first == FixedId(HeroId));
    CHECK(Incoming(set, DerivedId, &first) == 1 && first == FixedId(HeroId));
    CHECK(Incoming(set, VillainId, &first) == 0 && first.IsNull());
    CHECK(Incoming(set, LeafId) == 0);

    // 편집: 끊고 다시 잇기를 한 묶음에(같은 쌍 — 합산), 하나만 끊기.
    const ObjectHandle hero = RepresentativeOf(set, HeroId);
    REQUIRE(SetHard(set, VillainId, ObjectHandle{}, false));
    REQUIRE(SetHard(set, VillainId, hero, false));
    REQUIRE(SetHard(set, VillainId, ObjectHandle{}, true));
    CHECK(Incoming(set, HeroId) == 1);
    REQUIRE(SetHard(set, VillainId, hero, true));
    CHECK(Incoming(set, HeroId) == 2);

    // 닫기: 참조되는 Hero는 Busy, Villain을 닫으면 Hero로 들어오는 간선이 없다.
    failure = AssetDiagnostic{};
    auto busy = set.Close(FixedId(HeroId), &failure);
    CHECK(!busy && busy.Error() == ErrorCode::Busy &&
          failure.code == AssetDiagnosticCode::ReferencedBy &&
          failure.related == FixedId(VillainId) && set.Find(FixedId(HeroId)) != nullptr);
    REQUIRE(set.Close(FixedId(VillainId)));
    CHECK(set.Find(FixedId(VillainId)) == nullptr && set.Count() == 3 &&
          Incoming(set, HeroId) == 0);
    REQUIRE(set.Close(FixedId(HeroId)));
    CHECK(Incoming(set, BaseId) == 0 && set.Count() == 2);
    failure = AssetDiagnostic{};
    auto again = set.Close(FixedId(HeroId), &failure);
    CHECK(!again && again.Error() == ErrorCode::NotFound &&
          failure.code == AssetDiagnosticCode::None);
    CHECK(set.Close(FixedId(HeroId)).Error() == ErrorCode::NotFound);
    // 닫은 패키지는 다시 열린다. 동기화 전에 끊으면 열기(Commit +2)와 편집(Edit −2)이 한 묶음에서
    // 합쳐져 0이다(표에 없는 쌍), 다시 이으면 1.
    REQUIRE(set.Open(FixedId(VillainId)));
    REQUIRE(SetHard(set, VillainId, ObjectHandle{}, false));
    REQUIRE(SetHard(set, VillainId, ObjectHandle{}, true));
    CHECK(Incoming(set, HeroId) == 0);
    REQUIRE(SetHard(set, VillainId, RepresentativeOf(set, HeroId), false));
    CHECK(Incoming(set, HeroId) == 1);

    // 저장하지 않은 새 패키지(Base·Hero를 참조 — Hero의 두 번째 참조자)를 닫는다.
    REQUIRE(CreateUnsaved(env, set, "/Game/Fresh", 0xe01));
    REQUIRE(SetHard(set, 0xe01, RepresentativeOf(set, BaseId), false));
    REQUIRE(SetHard(set, 0xe01, RepresentativeOf(set, HeroId), true));
    CHECK(Incoming(set, BaseId) == 2);
    CHECK(Incoming(set, HeroId, &first) == 2 && first == FixedId(VillainId));
    REQUIRE(set.Close(FixedId(0xe01)));
    CHECK(set.Find(FixedId(0xe01)) == nullptr && Incoming(set, BaseId) == 1 &&
          Incoming(set, HeroId) == 1);

    // 소유 스레드가 아니면 WrongThread(TakeChanges — 묶음을 꺼내지 않는다).
    struct Foreign {
        EditorPackageSet* set;
        ErrorCode error = ErrorCode::None;
    } foreign{&set};
    {
        platform::Thread thread;
        auto entry = [](void* context) noexcept {
            auto* state = static_cast<Foreign*>(context);
            auto synced = state->set->SyncReferences();
            state->error = synced ? ErrorCode::None : synced.Error();
        };
        REQUIRE(thread.Start(entry, &foreign) == platform::ThreadStatus::Success);
        REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    }

    CHECK(foreign.error == ErrorCode::WrongThread && Incoming(set, HeroId) == 1);

    // 초기화 안의 닫기는 Busy다.
    CloseInside inside(set, FixedId(LeafId));
    auto path = ParseLogicalPath(env.allocator, "/Game/Inner");
    REQUIRE(path);
    REQUIRE(set.Create(path.Value(), IdOf<TestAsset>(), FixedId(0xe02), nullptr, &inside));
    CHECK(inside.error == ErrorCode::Busy);
}

// 간선 표의 할당 실패: 묶음을 꺼낸 뒤 적용이 실패하면 쥔 채 다음 호출이 다시 적용한다(다중도를
// 잃지 않는다). 닫기의 할당 실패(경로·Discard)는 아무것도 바꾸지 않는다.
TEST(AssetDelete_EdgeTableAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    REQUIRE(project.files.AddFile("Base.rasset", sources.base.View()));
    REQUIRE(project.files.AddFile("Derived.rasset", sources.derived.View()));
    REQUIRE(project.files.AddFile("Hero.rasset", sources.hero.View()));
    REQUIRE(project.files.AddFile("Villain.rasset", sources.villain.View()));
    REQUIRE(project.Rebuild());
    TaggedFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        EditorPackageSet set(failing, Config(env, project));
        REQUIRE(set.Initialize());
        REQUIRE(set.Open(FixedId(VillainId)));
        failing.FailOnAttempt(attempt);
        auto synced = set.SyncReferences();
        const bool failed = failing.Failed();
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(synced);
            CHECK(Incoming(set, HeroId) == 2);
            break;
        }

        CHECK(!synced && synced.Error() == ErrorCode::OutOfMemory);
        // 다음 호출이 쥔 묶음을 적용한다.
        CHECK(Incoming(set, HeroId) == 2 && Incoming(set, BaseId) == 1);
        ++failures;
    }

    CHECK(failures > 0);

    // 열기·만들기는 시작할 때 쌓인 묶음을 적용한다 — 그 할당 실패는 아무것도 열거나 만들지 않는다.
    REQUIRE(project.files.AddFile("Leaf.rasset", sources.leaf.View()));
    REQUIRE(project.Rebuild());
    const bool creates[] = {false, true};
    for (bool create : creates) {
        failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            EditorPackageSet set(failing, Config(env, project));
            REQUIRE(set.Initialize());
            REQUIRE(set.Open(FixedId(VillainId)));
            failing.FailOnAttempt(attempt);
            Result<ObjectHandle> made =
                create ? CreateUnsaved(env, set, "/Game/Made", 0xe40) : set.Open(FixedId(LeafId));
            const bool failed = failing.Failed();
            const uint32 tag = failing.FailedTag();
            failing.DisableFailure();
            const uint64 madeId = create ? 0xe40 : LeafId;
            if (!failed || (made && tag == GcReferenceTableTag)) {
                REQUIRE(made);
                CHECK(Incoming(set, HeroId) == 2);
                if (!failed) {
                    break;
                }

                continue;
            }

            CHECK(!made && made.Error() == ErrorCode::OutOfMemory &&
                  set.Find(FixedId(madeId)) == nullptr && Incoming(set, HeroId) == 2);
            ++failures;
        }

        CHECK(failures > 0);
    }

    failures = 0;
    uint32 tolerated = 0;
    for (uint64 attempt = 1;; ++attempt) {
        EditorPackageSet set(failing, Config(env, project));
        REQUIRE(set.Initialize());
        REQUIRE(set.Open(FixedId(VillainId)));
        AssetDiagnostic failure;
        failing.FailOnAttempt(attempt);
        // 닫기 안의 동기화(열기 묶음 적용)도 실패 지점이다.
        auto closed = set.Close(FixedId(VillainId), &failure);
        const bool failed = failing.Failed();
        const uint32 tag = failing.FailedTag();
        failing.DisableFailure();
        if (!failed || (closed && tag == GcReferenceTableTag)) {
            REQUIRE(closed);
            CHECK(set.Find(FixedId(VillainId)) == nullptr && Incoming(set, HeroId) == 0);
            if (!failed) {
                break;
            }

            ++tolerated;
            continue;
        }

        CHECK(!closed && closed.Error() == ErrorCode::OutOfMemory &&
              failure.code == AssetDiagnosticCode::None);
        CHECK(set.Find(FixedId(VillainId)) != nullptr && Incoming(set, HeroId) == 2);
        ++failures;
    }

    CHECK(failures > 0 && tolerated <= 1);
}

// 색인 빼기(removed): 파일·디렉터리(그 아래 항목·진단 — 손상 파일·열거 실패)·없는 이름을 지우기
// 전에 준비하고 지운 뒤 확정하면 새 검색과 같다(읽지 않는다 — 지우기 전 준비가 항목을 다시 더하지
// 않는다). 이름 규칙 위반은 InvalidArgument, 할당 실패는 OutOfMemory이고 색인 그대로.
TEST(AssetDelete_IndexRemoved) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    DeleteScene scene(env, sources, "delete-index");
    REQUIRE(scene.ready);
    const uint8 junk[] = {1, 2, 3};
    REQUIRE(scene.files.AddFile("Content/Dir/Junk.rasset", Span<const uint8>(junk, 3)));
    REQUIRE(scene.files.AddDirectory("Content/Dir/Broken", false, MemoryFault::Enumerate));
    AssetIndex index(env.allocator, scene.layout);
    REQUIRE(index.Rebuild(scene.files, ReadOptions()));
    auto removeAndCommit = [&](StringView name, bool directory) noexcept {
        AssetIndexRefreshPaths paths;
        paths.removed = Span<const StringView>(&name, 1);
        AssetIndexRefresh plan(env.allocator);
        if (!index.PrepareRefreshPaths(scene.files, paths, ReadOptions(), plan)) {
            return false;
        }

        String relative(env.allocator);
        if (!relative.TryAppend("Content/") || !relative.TryAppend(name)) {
            return false;
        }

        if (directory) {
            // 메모리 파일 시스템의 디렉터리 지우기: 그 아래 노드를 먼저 뺀다.
            for (usize at = scene.files.NodeCount(); at-- > 0;) {
                const StringView node = scene.files.NodeName(at);
                if (node.StartsWith(relative.View(), StringComparison::CaseSensitive) &&
                    node.ByteCount() > relative.View().ByteCount() &&
                    node.Data()[relative.View().ByteCount()] == '/') {
                    String copy(env.allocator);
                    if (!copy.TryAssign(node)) {
                        return false;
                    }

                    scene.files.Remove(copy.View());
                }
            }
        }

        scene.files.Remove(relative.View());
        index.CommitRefresh(plan);
        return Matches(env, scene, index);
    };

    CHECK(removeAndCommit("Leaf.rasset", false));
    CHECK(removeAndCommit("Dir", true));
    bool underDir = false;
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        underDir = underDir ||
                   diagnostic.fileName.View().StartsWith("Dir/", StringComparison::CaseSensitive);
    }

    CHECK(!underDir);
    CHECK(removeAndCommit("Gone.rasset", false));

    AssetIndex before(env.allocator, scene.layout);
    REQUIRE(before.Rebuild(scene.files, ReadOptions()));
    const StringView bad[] = {"", "/Base.rasset", "A/../B"};
    for (StringView name : bad) {
        AssetIndexRefreshPaths paths;
        paths.removed = Span<const StringView>(&name, 1);
        auto rejected = index.RefreshPaths(scene.files, paths, ReadOptions());
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument &&
              SameIndex(before, index));
    }

    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex attempted(failing, scene.layout);
        REQUIRE(attempted.Rebuild(scene.files, ReadOptions()));
        const StringView name = "Base.rasset";
        AssetIndexRefreshPaths paths;
        paths.removed = Span<const StringView>(&name, 1);
        AssetIndexRefresh plan(failing);
        failing.FailOnAttempt(attempt);
        auto prepared = attempted.PrepareRefreshPaths(scene.files, paths, ReadOptions(), plan);
        const bool triggered = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!triggered) {
            REQUIRE(prepared);
            break;
        }

        CHECK(!prepared && prepared.Error() == ErrorCode::OutOfMemory &&
              SameIndex(before, attempted));
        ++failures;
    }

    CHECK(failures > 0);
}

// 서비스 삭제: 닫힌 패키지(별칭 하나 지움 — 경고 AliasRemoved·다른 에셋 별칭은 남음), id로 고른
// 패키지, 열린 패키지(닫힘), 저장하지 않은 새 패키지(경로·id — 집합에서만), 디스크 참조자(거부 —
// 목록·related, force — 지우고 목록, 참조자는 MissingDependency), 메모리 참조(force여도 Busy),
// NotDurable 경고(경고 목록 있고 없음). 매번 색인은 새 검색과 같다.
TEST(AssetDelete_DeletesPackages) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    DeleteScene scene(env, sources, "delete-service");
    REQUIRE(scene.ready);
    RootRef<Package> selfPackage;
    const Array<uint8> selfish = SelfReferencing(env, selfPackage);
    REQUIRE(!selfish.IsEmpty());
    REQUIRE(scene.files.AddFile("Content/Selfish.rasset", selfish.View()));
    // Derived는 읽기 전용(모르는 커스텀 버전)이다 — 삭제는 내용을 잃는 쓰기가 아니라 허용한다.
    PackageWriteOptions readOnly = EditorOptions();
    readOnly.customVersions = UnknownCustomVersions;
    const Array<uint8> derived =
        Write(env.allocator, *env.source, sources.packages[1].Handle(), readOnly);
    REQUIRE(!derived.IsEmpty() && scene.Put("Content/Derived.rasset", derived.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    REQUIRE(WriteAliases(scene, service));
    AssetDeleteResult result(env.allocator);
    Array<EditorWarning> warnings(env.allocator);

    // 닫힌 패키지와 그 별칭.
    REQUIRE(service.Delete(scene.lock, "/Game/Leaf", false, result, &warnings));
    CHECK(result.id == FixedId(LeafId) && result.path.View() == "/Game/Leaf" &&
          result.aliases == 1 && result.referencerIds.IsEmpty());
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorWarningKind::AliasRemoved &&
          warnings[0].Text() == "/Game/Old/Leaf" &&
          warnings[0].diagnostic.asset == FixedId(LeafId));
    CHECK(!scene.files.Has("Content/Leaf.rasset") && Matches(env, scene, service.Index()));
    {
        auto aliases = service.Aliases();
        REQUIRE(aliases);
        CHECK(aliases.Value()->Count() == 1 && aliases.Value()->At(0).asset == FixedId(BaseId));
    }

    // id로 고른 열린 패키지(별칭 없음 — 별칭 표를 쓰지 않는다).
    warnings.Clear();
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Solo", AssetInfoOptions{}, out));
    String aliasBefore(env.allocator);
    REQUIRE(aliasBefore.TryAssign(scene.AliasText()));
    char solo[40] = {};
    {
        FixedStringBuilder text{Span<char>(solo)};
        REQUIRE(text.TryAppendFormat("{}", FixedId(CycleBId)));
    }

    REQUIRE(service.Delete(scene.lock, StringView(solo), false, result, &warnings));
    CHECK(result.path.View() == "/Game/Solo" && result.aliases == 0 && warnings.IsEmpty() &&
          service.Packages().Find(FixedId(CycleBId)) == nullptr &&
          !scene.files.Has("Content/Solo.rasset") && scene.AliasText() == aliasBefore.View() &&
          Matches(env, scene, service.Index()));

    // 자기 자신만 참조하는 패키지는 참조자가 없다.
    REQUIRE(service.Delete(scene.lock, "/Game/Selfish", false, result, &warnings));
    CHECK(result.referencerIds.IsEmpty() && !scene.files.Has("Content/Selfish.rasset"));

    // 저장하지 않은 새 패키지(경로 — 열린 저장 패키지와 앞의 다른 새 패키지를 지나서, id).
    REQUIRE(service.Info("/Game/Base", AssetInfoOptions{}, out));
    REQUIRE(CreateUnsaved(env, service.Packages(), "/Game/New/One", 0xe10));
    REQUIRE(CreateUnsaved(env, service.Packages(), "/Game/New/Two", 0xe11));
    REQUIRE(CreateUnsaved(env, service.Packages(), "/Game/New/Three", 0xe12));
    const uint32 deletes = scene.files.deletes;
    REQUIRE(service.Delete(scene.lock, "/Game/new/two", false, result, &warnings));
    CHECK(result.id == FixedId(0xe11) && result.path.View() == "/Game/New/Two" &&
          service.Packages().Find(FixedId(0xe11)) == nullptr);
    char one[40] = {};
    {
        FixedStringBuilder text{Span<char>(one)};
        REQUIRE(text.TryAppendFormat("{}", FixedId(0xe10)));
    }

    REQUIRE(service.Delete(scene.lock, StringView(one), false, result, &warnings));
    REQUIRE(service.Delete(scene.lock, "/Game/New/Three", false, result, &warnings));
    CHECK(service.Packages().Find(FixedId(0xe10)) == nullptr &&
          service.Packages().Find(FixedId(0xe12)) == nullptr && scene.files.deletes == deletes &&
          warnings.IsEmpty());

    // 디스크 참조자: Villain이 Hero를 참조한다 — 거부(목록·related), 파일 그대로.
    AssetDiagnostic failure;
    auto refused = service.Delete(scene.lock, "/Game/Dir/Hero", false, result, &warnings, &failure);
    CHECK(!refused && refused.Error() == ErrorCode::InvalidArgument &&
          failure.code == AssetDiagnosticCode::ReferencedBy && failure.asset == FixedId(HeroId) &&
          failure.related == FixedId(VillainId));
    REQUIRE(result.referencerIds.Size() == 1);
    CHECK(result.referencerIds[0] == FixedId(VillainId) &&
          result.referencerPaths[0].View() == "/Game/Villain" &&
          result.path.View() == "/Game/Dir/Hero" && scene.files.Has("Content/Dir/Hero.rasset"));

    // 메모리 참조: 열린 Villain이 Hero를 참조하면 force여도 Busy.
    REQUIRE(service.Info("/Game/Villain", AssetInfoOptions{}, out));
    failure = AssetDiagnostic{};
    auto busy = service.Delete(scene.lock, "/Game/Dir/Hero", true, result, &warnings, &failure);
    CHECK(!busy && busy.Error() == ErrorCode::Busy &&
          failure.code == AssetDiagnosticCode::ReferencedBy &&
          failure.related == FixedId(VillainId) && scene.files.Has("Content/Dir/Hero.rasset"));

    // Villain을 닫으면 force로 지운다(참조자는 다음 점검에서 MissingDependency).
    REQUIRE(service.Packages().Close(FixedId(VillainId)));
    REQUIRE(service.Delete(scene.lock, "/Game/Dir/Hero", true, result, &warnings));
    CHECK(result.referencerIds.Size() == 1 && result.referencerIds[0] == FixedId(VillainId) &&
          !scene.files.Has("Content/Dir/Hero.rasset") &&
          service.Packages().Find(FixedId(HeroId)) == nullptr &&
          Matches(env, scene, service.Index()));
    Array<AssetIndexDiagnostic> verified(env.allocator);
    REQUIRE(service.Index().Verify(nullptr, verified));
    bool missing = false;
    for (const AssetIndexDiagnostic& diagnostic : verified) {
        missing = missing || (diagnostic.code == AssetDiagnosticCode::MissingDependency &&
                              diagnostic.relatedAsset == FixedId(HeroId));
    }

    CHECK(missing);

    // NotDurable(별칭이 있는 Base — Base는 아직 Hero 없이 참조자가 없다)는 성공 경고다.
    warnings.Clear();
    scene.files.faults.replace = MemoryNotDurable;
    REQUIRE(service.Delete(scene.lock, "/Game/Base", false, result, &warnings));
    scene.files.faults.replace = {};
    REQUIRE(warnings.Size() == 2);
    CHECK(warnings[0].kind == EditorWarningKind::AliasRemoved &&
          warnings[1].kind == EditorWarningKind::NotDurable);
    // 경고 목록 없이도(별칭 없는 Derived — 열린 읽기 전용 패키지).
    const EditorPackageInfo* readOnlyInfo = service.Packages().Find(FixedId(DerivedId));
    REQUIRE(readOnlyInfo != nullptr);
    CHECK(readOnlyInfo->readOnly);
    REQUIRE(service.Delete(scene.lock, "/Game/Derived", false, result));
    CHECK(!scene.files.Has("Content/Derived.rasset") && Matches(env, scene, service.Index()) &&
          service.Packages().Find(FixedId(DerivedId)) == nullptr);

    // 낡은 색인(파일을 밖에서 지웠다): 바라는 상태라 성공이고 색인을 확정한다.
    scene.files.Remove("Content/Villain.rasset");
    REQUIRE(service.Delete(scene.lock, "/Game/Villain", false, result));
    CHECK(Matches(env, scene, service.Index()));
}

// 임포트 소유자와 저장하지 않은 Soft 편집: ImportOwner 의존만 있는 참조자(Leaf — Solo가 소유한
// 출력)는 소유자 삭제를 막고(ReferencedBy) force는 지운다. 열린 패키지의 저장하지 않은 Soft 참조는
// 강한 간선이 아니고 디스크 참조자도 아니라 삭제를 막지 않는다(7.1절 17 A2e3a 한계 — 저장된 상태만
// 본다).
TEST(AssetDelete_ImportOwnerAndSoftEdits) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    DeleteScene scene(env, sources, "delete-owner");
    REQUIRE(scene.ready);
    ImportRecordInput record;
    record.owner = FixedId(CycleBId);
    auto recordBytes = WriteImportRecord(env.allocator, FixedId(LeafId), record);
    REQUIRE(recordBytes);
    const PackageDependencyEntry owner[] = {
        {FixedId(CycleBId), PackageDependencyKind::ImportOwner, TypeId{}, false}};
    PackageWriteOptions options = EditorOptions();
    options.importRecord = recordBytes.Value().View();
    options.extraDependencies = owner;
    const Array<uint8> output =
        Write(env.allocator, *env.source, sources.packages[4].Handle(), options);
    REQUIRE(!output.IsEmpty() && scene.Put("Content/Leaf.rasset", output.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    AssetDeleteResult result(env.allocator);
    AssetDiagnostic failure;
    auto refused = service.Delete(scene.lock, "/Game/Solo", false, result, nullptr, &failure);
    CHECK(!refused && failure.code == AssetDiagnosticCode::ReferencedBy &&
          failure.related == FixedId(LeafId) && result.referencerIds.Size() == 1 &&
          scene.files.Has("Content/Solo.rasset"));
    REQUIRE(service.Delete(scene.lock, "/Game/Solo", true, result));
    CHECK(result.referencerIds.Size() == 1 && !scene.files.Has("Content/Solo.rasset") &&
          Matches(env, scene, service.Index()));

    // 열린 Villain이 Leaf를 Soft로 가리키게 고친다(저장하지 않음) — Leaf는 지워진다.
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Villain", AssetInfoOptions{}, out));
    REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
    const ObjectHandle villain = RepresentativeOf(service.Packages(), VillainId);
    TestAsset* value =
        villain.IsNull() ? nullptr : Pointer<TestAsset>(service.Packages().Registry(), villain);
    REQUIRE(value != nullptr && value->soft.TryAssign(FixedId(LeafId), ""));
    REQUIRE(service.Packages().MarkDirty(FixedId(VillainId)));
    REQUIRE(service.Delete(scene.lock, "/Game/Leaf", false, result));
    CHECK(result.referencerIds.IsEmpty() && !scene.files.Has("Content/Leaf.rasset") &&
          service.Packages().Find(FixedId(LeafId)) == nullptr);
}

// 거부와 실패: 파일·별칭 파일 byte·색인이 그대로다. Initialize 전·잠금 아님·경로·id 텍스트 형식·
// null id·없음(경로·id·별칭 경로는 따르지 않음)·같은 경로 키 둘·같은 id 파일 둘(id·경로)·별칭 표
// 손상·별칭 쓰기 실패·경고 자리 할당 실패. 지우기 실패는 별칭을 되돌리고(닫은 패키지는 닫힌 채 —
// 다시 열린다), 되돌리기 실패는 경고 AliasNotRestored(목록 없으면 실패만).
TEST(AssetDelete_RejectsAndRestores) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    DeleteScene scene(env, sources, "delete-reject");
    REQUIRE(scene.ready);
    REQUIRE(scene.files.AddDirectory("Content/Split") && scene.files.AddDirectory("Content/split"));
    REQUIRE(scene.files.AddFile("Content/Split/Dup.rasset", sources.cycleA.View()));
    REQUIRE(scene.files.AddFile("Content/split/dup.rasset", sources.derived.View()));
    REQUIRE(scene.files.AddFile("Content/Twin.rasset", sources.leaf.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    AssetDeleteResult result(env.allocator);
    AssetDiagnostic failure;
    CHECK(service.Delete(scene.lock, "/Game/Leaf", false, result, nullptr, &failure).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(service.Initialize());
    REQUIRE(WriteAliases(scene, service));
    String aliasText(env.allocator);
    REQUIRE(aliasText.TryAssign(scene.AliasText()));
    auto unchanged = [&]() noexcept {
        return scene.files.Has("Content/Leaf.rasset") && scene.files.Has("Content/Base.rasset") &&
               scene.AliasText() == aliasText.View() && Matches(env, scene, service.Index());
    };
    auto rejects = [&](StringView target, ErrorCode error, AssetDiagnosticCode code) noexcept {
        failure = AssetDiagnostic{};
        auto deleted = service.Delete(scene.lock, target, true, result, nullptr, &failure);
        return !deleted && deleted.Error() == error && failure.code == code && unchanged();
    };

    AssetDatabaseLock other;
    CHECK(service.Delete(other, "/Game/Leaf", false, result).Error() == ErrorCode::InvalidArgument);
    CHECK(rejects("/Game/Bad Name", ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("not-an-id", ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("00000000-0000-0000-0000-000000000000", ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Nowhere", ErrorCode::NotFound, AssetDiagnosticCode::None));
    CHECK(rejects("5e570000-a55e-7000-0000-00000000dead", ErrorCode::NotFound,
                  AssetDiagnosticCode::None));
    // 별칭 경로는 따르지 않는다.
    CHECK(rejects("/Game/Old/Leaf", ErrorCode::NotFound, AssetDiagnosticCode::None));
    CHECK(rejects("/Game/Split/Dup", ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicatePath));
    char leafId[40] = {};
    {
        FixedStringBuilder text{Span<char>(leafId)};
        REQUIRE(text.TryAppendFormat("{}", FixedId(LeafId)));
    }

    CHECK(rejects(StringView(leafId), ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicateAssetId));
    CHECK(rejects("/Game/Twin", ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicateAssetId) &&
          failure.asset == FixedId(LeafId));
    scene.files.Remove("Content/Twin.rasset");
    AssetIndex& index = service.Index();
    {
        AssetIndexRefreshPaths paths;
        const StringView twin = "Twin.rasset";
        paths.files = Span<const StringView>(&twin, 1);
        REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    }

    CHECK(unchanged());

    // 열려 있지만 색인에서 빠진 저장 패키지(파일이 밖에서 지워지고 색인을 갱신했다)는 id로도 찾지
    // 않는다(색인이 권위 — NotFound).
    String opened(env.allocator);
    REQUIRE(service.Info("/Game/Solo", AssetInfoOptions{}, opened));
    scene.files.Remove("Content/Solo.rasset");
    {
        AssetIndexRefreshPaths paths;
        const StringView solo = "Solo.rasset";
        paths.files = Span<const StringView>(&solo, 1);
        REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    }

    char soloId[40] = {};
    {
        FixedStringBuilder text{Span<char>(soloId)};
        REQUIRE(text.TryAppendFormat("{}", FixedId(CycleBId)));
    }

    failure = AssetDiagnostic{};
    auto gone = service.Delete(scene.lock, StringView(soloId), false, result, nullptr, &failure);
    CHECK(!gone && gone.Error() == ErrorCode::NotFound &&
          service.Packages().Find(FixedId(CycleBId)) != nullptr);
    REQUIRE(scene.files.AddFile("Content/Solo.rasset", sources.cycleB.View()));
    {
        AssetIndexRefreshPaths paths;
        const StringView solo = "Solo.rasset";
        paths.files = Span<const StringView>(&solo, 1);
        REQUIRE(index.RefreshPaths(scene.files, paths, ReadOptions()));
    }

    CHECK(unchanged());

    // 별칭 표 손상.
    {
        AssetService broken(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(scene.Put(AliasFile, BytesOf("{")));
        REQUIRE(broken.Initialize());
        failure = AssetDiagnostic{};
        auto corrupt = broken.Delete(scene.lock, "/Game/Leaf", false, result, nullptr, &failure);
        CHECK(!corrupt && failure.code == AssetDiagnosticCode::CorruptAliasTable &&
              scene.files.Has("Content/Leaf.rasset"));
        REQUIRE(scene.Put(AliasFile, BytesOf(aliasText.View())));
    }

    // 별칭 쓰기 실패.
    scene.files.faults.create = MemoryError;
    failure = AssetDiagnostic{};
    auto writeFailed = service.Delete(scene.lock, "/Game/Leaf", false, result, nullptr, &failure);
    scene.files.faults.create = {};
    CHECK(!writeFailed && unchanged());

    // 경고 자리 할당 실패(바꾸기 전).
    {
        test::NFailAllocator warningAllocator(env.allocator);
        Array<EditorWarning> failingWarnings(warningAllocator);
        warningAllocator.FailOnAttempt(1);
        auto noRoom = service.Delete(scene.lock, "/Game/Leaf", false, result, &failingWarnings);
        warningAllocator.DisableFailure();
        CHECK(!noRoom && noRoom.Error() == ErrorCode::OutOfMemory && unchanged());
    }

    // 지우기 실패: 별칭을 되돌린다. 열려 있던 Leaf는 닫힌 채이고 다시 열린다.
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
    Array<EditorWarning> warnings(env.allocator);
    scene.files.faults.remove = MemoryBusy;
    failure = AssetDiagnostic{};
    auto removeFailed =
        service.Delete(scene.lock, "/Game/Leaf", false, result, &warnings, &failure);
    scene.files.faults.remove = {};
    CHECK(!removeFailed && removeFailed.Error() == ErrorCode::Busy &&
          failure.code == AssetDiagnosticCode::None && warnings.IsEmpty() && unchanged() &&
          service.Packages().Find(FixedId(LeafId)) == nullptr);
    REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));

    // 되돌리기 실패(별칭 쓰기는 성공, 되돌리기의 쓰기가 실패): AliasNotRestored, 경고 목록이
    // 없으면 실패만.
    const bool listedModes[] = {true, false};
    for (bool listed : listedModes) {
        warnings.Clear();
        scene.files.faults.remove = MemoryBusy;
        scene.files.faults.failWriteCall = scene.files.WriteCalls() + 2;
        auto notRestored =
            service.Delete(scene.lock, "/Game/Leaf", false, result, listed ? &warnings : nullptr);
        scene.files.faults.remove = {};
        scene.files.faults.failWriteCall = 0;
        CHECK(!notRestored && notRestored.Error() == ErrorCode::Busy);
        CHECK(listed ? warnings.Size() == 1 &&
                           warnings[0].kind == EditorWarningKind::AliasNotRestored &&
                           warnings[0].diagnostic.asset == FixedId(LeafId)
                     : warnings.IsEmpty());
        CHECK(scene.files.Has("Content/Leaf.rasset") && scene.AliasText() != aliasText.View());
        // 별칭 파일을 되돌리고 서비스 표를 다시 읽힌다(외부 변경 판정 — 다음 쓰기 전에 읽는다).
        REQUIRE(scene.Put(AliasFile, BytesOf(aliasText.View())));
        auto aliases = service.Aliases();
        REQUIRE(aliases && aliases.Value()->Load(scene.files));
    }

    CHECK(unchanged());
}

// 할당 실패: 별칭이 있는 열린 패키지, 디스크 참조자가 있는 force 삭제, 저장하지 않은 새 패키지 —
// 모든 할당 실패는 OutOfMemory이고 파일·별칭 파일·색인·집합이 그대로다(Discard의 GC 표 할당
// 실패는 Registry가 삼킨다 — 성공으로 본다). 실패가 없어지면 끝까지 지운다.
TEST(AssetDelete_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    struct DeleteCase {
        const char* tag;
        StringView target;
        bool force;
        bool open;
        StringView unsaved;
        const char* file;
        usize aliases;
        const Array<uint8>* bytes;
    };
    const DeleteCase cases[] = {
        {"delete-oom-open", "/Game/Leaf", false, true, {}, "Content/Leaf.rasset", 1, &sources.leaf},
        {"delete-oom-force",
         "/Game/Dir/Hero",
         true,
         false,
         {},
         "Content/Dir/Hero.rasset",
         0,
         &sources.hero},
        {"delete-oom-edges",
         "/Game/Villain",
         false,
         true,
         {},
         "Content/Villain.rasset",
         0,
         &sources.villain},
        {"delete-oom-unsaved", "/Game/New/Thing", false, false, "/Game/New/Thing", nullptr, 0,
         nullptr},
    };
    for (const DeleteCase& testCase : cases) {
        DeleteScene scene(env, sources, testCase.tag);
        REQUIRE(scene.ready);
        {
            AssetService prepared(env.allocator, scene.layout, scene.Config(env));
            REQUIRE(prepared.Initialize());
            REQUIRE(WriteAliases(scene, prepared));
        }

        String aliasBefore(env.allocator);
        REQUIRE(aliasBefore.TryAssign(scene.AliasText()));
        TaggedFailAllocator failing(env.allocator);
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            AssetService service(failing, scene.layout, scene.Config(env));
            REQUIRE(service.Initialize());
            String out(env.allocator);
            if (testCase.open) {
                REQUIRE(service.Info(testCase.target, AssetInfoOptions{}, out));
            }

            if (!testCase.unsaved.IsEmpty()) {
                REQUIRE(CreateUnsaved(env, service.Packages(), testCase.unsaved, 0xe20));
            }

            AssetDeleteResult result(failing);
            Array<EditorWarning> warnings(env.allocator);
            failing.FailOnAttempt(attempt);
            auto deleted =
                service.Delete(scene.lock, testCase.target, testCase.force, result, &warnings);
            const bool failed = failing.Failed();
            const uint32 tag = failing.FailedTag();
            failing.DisableFailure();
            if (!failed || (deleted && tag == GcReferenceTableTag)) {
                REQUIRE(deleted);
                CHECK(result.aliases == testCase.aliases && warnings.Size() == testCase.aliases &&
                      Matches(env, scene, service.Index()));
                CHECK(testCase.file == nullptr || !scene.files.Has(testCase.file));
                if (!failed) {
                    break;
                }

                // GC 표 실패를 삼킨 성공은 지웠다 — 장면을 되돌리고 다음 시도로.
                REQUIRE(testCase.file == nullptr ||
                        scene.Put(testCase.file, testCase.bytes->View()));
                REQUIRE(scene.Put(AliasFile, BytesOf(aliasBefore.View())));
                continue;
            }

            CHECK(!deleted && deleted.Error() == ErrorCode::OutOfMemory && warnings.IsEmpty());
            CHECK(scene.AliasText() == aliasBefore.View() && Matches(env, scene, service.Index()));
            CHECK(testCase.file == nullptr || scene.files.Has(testCase.file));
            ++failures;
        }

        CHECK(failures > 0);
    }
}

// 실제 디스크: bulk를 읽어 캐시 핸들이 열린 패키지를 지운다(닫기가 핸들을 닫지 않으면 Windows는
// 파일을 지우지 못한다).
TEST(AssetDelete_RealDisk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("delete-disk");
    REQUIRE(project.made);
    REQUIRE(WriteContentFile(env.allocator, project, "Dir/Hero.rasset", sources.hero.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Base.rasset", sources.base.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Derived.rasset", sources.derived.View()));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(layout, 0));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &platform::PhysicalFileSystem();
    config.packages.hooks = &hooks;
    config.packages.read.engineVersion = TestEngineVersion;
    config.packages.registryContext = ContextId{0x44454c44ull, 1};
    config.packages.registryOptions.names = &env.names;
    AssetService service(env.allocator, layout, config);
    REQUIRE(service.Initialize());
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Dir/Hero", AssetInfoOptions{}, out));
    const EditorPackageInfo* hero = service.Packages().Find(FixedId(HeroId));
    REQUIRE(hero != nullptr && hero->store != nullptr);
    TestAsset* value = AssetOf(service.Packages(), hero->package);
    REQUIRE(value != nullptr);
    auto read = hero->store->Read(value->pixels);
    REQUIRE(read && read.Value().Size() == PixelBytes);
    AssetDeleteResult result(env.allocator);
    REQUIRE(service.Delete(lock, "/Game/Dir/Hero", false, result));
    String path(env.allocator);
    REQUIRE(path.TryAppend(project.Root()) && path.TryAppend("/Content/Dir/Hero.rasset"));
    bool exists = true;
    REQUIRE(
        platform::PhysicalFileSystem().Exists(path.View().Data(), path.View().ByteCount(), exists));
    CHECK(!exists && service.Packages().Find(FixedId(HeroId)) == nullptr);
}

} // namespace asset_delete_tests_file
