#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "../../../Runtime/AssetRuntime/Tests/PackageEditing.h"
#include "AssetDatabaseTestSupport.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/EditorPackageSet.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/FileError.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>
#include <string.h>

// 에디터 패키지 저장 시험이다(Docs/Design/AssetSystem.ko.md 5.5절, 7.1절 15 — A2c2b): 기존 패키지
// 저장·다시 열기(지연 bulk의 Rebind 포함)·같은 byte 다시 저장, 새 패키지 만들기·첫 저장(디렉터리
// 만들기)과 만들기·첫 저장의 거부, 판정 순서의 거부(열리지 않음·잠금·읽기 전용·미해결 import·
// dropUnresolved), 외부 변경, 모든 IO 고장(만들기·n번째 쓰기·쓰기 손상·Flush·Close·교체·
// NotDurable·디렉터리)과 임시 파일 정리·고아 정리, dirty·세대, 타입 hook(Tags 다시 만들기·Build
// 의존·오류), ImportRecord·ImportOwner 보존, 모든 할당 실패, 색인 Refresh, 실제 디스크.
using namespace rocks;
namespace editor_package_save_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;

namespace {

constexpr int32 EditedValue = 42;
constexpr uint64 FreshId = 2850;
// 기록 종류(소유·참조)와 저장 대상(기존·새 패키지)의 반복 값이다.
constexpr bool OwnedCases[] = {true, false};
constexpr bool FreshCases[] = {false, true};
// 초기화가 하위 객체를 만드는지의 반복 값이다.
constexpr bool PartCases[] = {false, true};
// 이름 규칙(ValidateNameComponent)에 어긋나는 하위 객체 이름이다.
constexpr StringView InvalidPartName = "bad/name";

// 저장 집합 설정이다(hook 표를 넘긴다 — 저장에 필수).
EditorPackageSetConfig SaveConfig(AssetEnv& env, Project& project,
                                  const AssetTypeHookTable& hooks) noexcept {
    EditorPackageSetConfig config = Config(env, project);
    config.hooks = &hooks;
    return config;
}

// 저장 장면이다: 루트는 디스크 임시 디렉터리(잠금 파일 — 이 프로젝트의 잠금), Content는 메모리의
// Base·Derived·Hero·Villain·Leaf 파일, 색인, 빈 hook 표로 연 집합, 잡은 잠금.
struct SaveScene {
    SaveScene(AssetEnv& env, Sources& sources) noexcept
        : root("save-scene"), project(env.allocator, root.Root()), hooks(env.allocator, env.types),
          set(env.allocator, SaveConfig(env, project, hooks)) {
        MemoryFileSystem& files = project.files;
        ready = project.ready && files.Ready() &&
                files.AddFile("Base.rasset", sources.base.View()) &&
                files.AddFile("Derived.rasset", sources.derived.View()) &&
                files.AddFile("Hero.rasset", sources.hero.View()) &&
                files.AddFile("Villain.rasset", sources.villain.View()) &&
                files.AddFile("Leaf.rasset", sources.leaf.View()) && project.Rebuild() &&
                root.made && static_cast<bool>(lock.Acquire(project.layout, 0)) &&
                static_cast<bool>(set.Initialize());
    }

    // 같은 프로젝트·빈 hook 표의 다른 집합 설정이다.
    EditorPackageSetConfig Config(AssetEnv& env) noexcept {
        return SaveConfig(env, project, hooks);
    }

    TempProject root;
    Project project;
    AssetTypeHookTable hooks;
    AssetDatabaseLock lock;
    EditorPackageSet set;
    bool ready = false;
};

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.IsEmpty() || memcmp(left.Data(), right.Data(), left.Size()) == 0);
}

// Content에 남은 임시 파일(".tmp")이 있는지다.
bool HasTemporary(const MemoryFileSystem& files) noexcept {
    for (usize index = 0; index < files.NodeCount(); ++index) {
        if (files.NodeName(index).EndsWith(".tmp")) {
            return true;
        }
    }

    return false;
}

// 저장한 byte를 다른 집합으로 다시 연다(같은 프로젝트 — 색인은 다시 만든다).
struct Reopened {
    Reopened(AssetEnv& env, Project& project) noexcept : set(env.allocator, Config(env, project)) {
        ready = project.Rebuild() && static_cast<bool>(set.Initialize());
    }

    EditorPackageSet set;
    bool ready = false;
};

} // namespace

namespace {

// hook 시험의 상태다(hook은 함수 포인터라 전역으로 넘긴다).
struct HookState {
    EditorPackageSet* set = nullptr;
    bool markDirty = false;
    bool failTags = false;
    bool failBuild = false;
    // 4 MiB를 넘는 태그(값 4000 byte × 1100)를 낸다(Tags 쓰기 CapacityExceeded).
    bool bigTags = false;
};

constexpr usize BigTagCount = 1100;
constexpr usize BigTagValueBytes = 4000;

HookState hookState;

Result<void> TestTags(const AssetTypeHookContext&, AssetTagSink& sink) noexcept {
    if (hookState.failTags) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (hookState.markDirty && hookState.set != nullptr) {
        (void)hookState.set->MarkDirty(FixedId(HeroId));
    }

    if (hookState.bigTags) {
        static char value[BigTagValueBytes];
        memset(value, 'v', sizeof(value));
        for (usize index = 0; index < BigTagCount; ++index) {
            char key[8];
            const int count = snprintf(key, sizeof(key), "K%04u", static_cast<unsigned>(index));
            if (auto added = sink.Add(StringView(key, static_cast<usize>(count)),
                                      StringView(value, sizeof(value)));
                !added) {
                return added;
            }
        }
    }

    return sink.Add("Kind", "Hero");
}

Result<void> TestBuild(const AssetTypeHookContext&, AssetDependencySink& sink) noexcept {
    if (hookState.failBuild) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return sink.Add(FixedId(LeafId));
}

} // namespace

// 기존 패키지 저장(5.5절, 7.1절 15): 값을 바꾸고 MarkDirty한 Hero를 저장하면 교체 한 번·임시 파일
// 없음·경고 없음, dirty 해제(세대 그대로), 정보 지문 = 새 파일 = 색인 항목. 저장 전에 읽지 않은
// bulk는 새 파일에서 읽힌다(Rebind). 바뀌지 않은 패키지를 다시 저장하면 같은 byte다(결정적). 다른
// 집합으로 다시 열면 값·bulk·Hard 참조가 그대로다. 저장 전에 읽은 bulk span은 저장 뒤에도 유효하다.
TEST(EditorPackageSave_RoundTrip) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    EditorPackageSet& set = scene.set;
    MemoryFileSystem& files = scene.project.files;
    auto hero = set.Open(FixedId(HeroId));
    REQUIRE(hero);
    TestAsset* asset = AssetOf(set, hero.Value());
    REQUIRE(asset != nullptr);
    asset->value = EditedValue;
    REQUIRE(set.MarkDirty(FixedId(HeroId)));
    const EditorPackageInfo* info = set.Find(FixedId(HeroId));
    REQUIRE(info != nullptr);
    CHECK(info->dirty && info->generation == 1);
    const PackageFingerprint before = info->fingerprint;
    Array<EditorWarning> warnings(env.allocator);
    AssetDiagnostic failure;
    REQUIRE(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, &warnings, &failure));
    CHECK(warnings.IsEmpty());
    CHECK(!info->dirty && info->generation == 1 && !info->unsaved);
    CHECK(!(info->fingerprint == before));
    CHECK(files.replaces == 1 && !HasTemporary(files));
    auto entry = scene.project.index.FindById(FixedId(HeroId));
    REQUIRE(entry);
    CHECK(entry.Value()->fingerprint == info->fingerprint);
    CHECK(entry.Value()->fileName.View() == "Hero.rasset");

    // 저장 전에 읽지 않은 bulk는 새 파일에서 읽힌다.
    Package* package = Pointer<Package>(set.Registry(), hero.Value());
    REQUIRE(package != nullptr);
    auto pixels = package->BulkBytes(asset->pixels);
    REQUIRE(pixels);
    CHECK(SameBytes(pixels.Value(), sources.pixels.View()));

    // 바뀌지 않은 패키지를 다시 저장하면 같은 byte다.
    const Array<uint8> first = [&]() noexcept {
        Array<uint8> copy(env.allocator);
        (void)copy.TryAppend(files.Bytes("Hero.rasset"));
        return copy;
    }();
    REQUIRE(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}));
    CHECK(SameBytes(files.Bytes("Hero.rasset"), first.View()));
    CHECK(SameBytes(pixels.Value(), sources.pixels.View()));

    Reopened again(env, scene.project);
    REQUIRE(again.ready);
    auto reopened = again.set.Open(FixedId(HeroId));
    REQUIRE(reopened);
    TestAsset* loaded = AssetOf(again.set, reopened.Value());
    REQUIRE(loaded != nullptr);
    CHECK(loaded->value == EditedValue);
    CHECK(loaded->hard.Handle() == RepresentativeOf(again.set, BaseId));
    Package* loadedPackage = Pointer<Package>(again.set.Registry(), reopened.Value());
    REQUIRE(loadedPackage != nullptr);
    auto loadedPixels = loadedPackage->BulkBytes(loaded->pixels);
    REQUIRE(loadedPixels);
    CHECK(SameBytes(loadedPixels.Value(), sources.pixels.View()));
    CHECK(loaded->pixels.SameContent(asset->pixels));
}

// 새 패키지(7.1절 15): Create는 dirty·세대 1·파일 없음·논리 경로를 가진 정보를 만들고, 저장하지
// 않은 bulk와 열린 패키지 참조를 담아 저장하면 부모 디렉터리를 만들어 NFC 이름 파일을 쓰고 색인에
// 넣는다. 다시 열면 값·bulk·참조가 그대로다. 만들기 거부: type 0·Initialize 전 InvalidArgument,
// 색인의 같은 경로·저장하지 않은 새 패키지의 같은 경로 DuplicatePath, 열린 id·색인의 id
// DuplicateAssetId, 에셋 클래스가 아닌 type은 Registry 오류(아무것도 남지 않는다).
TEST(EditorPackageSave_NewPackage) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    EditorPackageSet& set = scene.set;
    REQUIRE(set.Open(FixedId(BaseId)));
    auto path = ParseLogicalPath(env.allocator, "/Game/Fresh/Thing");
    REQUIRE(path);
    auto made = set.Create(path.Value(), TestAssetType(), FixedId(FreshId));
    REQUIRE(made);
    const EditorPackageInfo* info = set.Find(FixedId(FreshId));
    REQUIRE(info != nullptr);
    CHECK(info->unsaved && info->dirty && info->generation == 1 && info->fileName.IsEmpty());
    CHECK(info->logicalPath.View() == "/Game/Fresh/Thing" && info->package == made.Value());
    TestAsset* asset = AssetOf(set, made.Value());
    REQUIRE(asset != nullptr);
    asset->value = EditedValue;
    Array<uint8> raw(env.allocator);
    REQUIRE(raw.TryAppend(sources.pixels.View()));
    auto bulk = info->store->AddUnsaved(Move(raw));
    REQUIRE(bulk);
    asset->pixels = bulk.Value();
    REQUIRE(set.Registry().SetReference(
        set.Access(), set.Registry().FindChild(set.Access(), made.Value(), "").Value(), asset->hard,
        RepresentativeOf(set, BaseId)));
    REQUIRE(set.Save(FixedId(FreshId), scene.lock, EditorSaveOptions{}));
    CHECK(!info->unsaved && !info->dirty && info->fileName.View() == "Fresh/Thing.rasset");
    CHECK(info->logicalPath.IsEmpty());
    CHECK(scene.project.files.Has("Fresh") && scene.project.files.Has("Fresh/Thing.rasset"));
    auto entry = scene.project.index.FindByPath(path.Value());
    REQUIRE(entry);
    CHECK(entry.Value()->id == FixedId(FreshId) && entry.Value()->fingerprint == info->fingerprint);

    Reopened again(env, scene.project);
    REQUIRE(again.ready);
    auto reopened = again.set.Open(path.Value());
    REQUIRE(reopened);
    TestAsset* loaded = AssetOf(again.set, reopened.Value());
    REQUIRE(loaded != nullptr);
    CHECK(loaded->value == EditedValue);
    CHECK(loaded->hard.Handle() == RepresentativeOf(again.set, BaseId));
    Package* loadedPackage = Pointer<Package>(again.set.Registry(), reopened.Value());
    REQUIRE(loadedPackage != nullptr);
    auto pixels = loadedPackage->BulkBytes(loaded->pixels);
    REQUIRE(pixels);
    CHECK(SameBytes(pixels.Value(), sources.pixels.View()));

    // 만들기 거부.
    const usize count = set.Count();
    AssetDiagnostic failure;
    auto other = ParseLogicalPath(env.allocator, "/Game/Fresh/Other");
    auto hero = ParseLogicalPath(env.allocator, "/Game/Hero");
    REQUIRE(other && hero);
    CHECK(set.Create(other.Value(), TypeId{}, {}, &failure).Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::None);
    CHECK(set.Create(hero.Value(), TestAssetType(), {}, &failure).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicatePath);
    REQUIRE(set.Create(other.Value(), TestAssetType()));
    CHECK(set.Create(other.Value(), TestAssetType(), {}, &failure).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicatePath);
    auto third = ParseLogicalPath(env.allocator, "/Game/Fresh/Third");
    REQUIRE(third);
    CHECK(set.Create(third.Value(), TestAssetType(), FixedId(BaseId), &failure).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicateAssetId);
    CHECK(set.Create(third.Value(), TestAssetType(), FixedId(VillainId), &failure).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicateAssetId);
    CHECK(!set.Create(third.Value(), TestPartType(), {}, &failure));
    CHECK(failure.code == AssetDiagnosticCode::None && failure.error != ErrorCode::None);
    CHECK(set.Count() == count + 1);

    EditorPackageSet idle(env.allocator, scene.Config(env));
    CHECK(idle.Create(third.Value(), TestAssetType()).Error() == ErrorCode::InvalidArgument);
}

// 새 패키지 첫 저장의 거부(7.1절 15): 만든 뒤 대상 파일이 생겼거나 색인에 같은 경로 키(대소문자만
// 다른 파일)가 생겼으면 DuplicatePath / AlreadyExists, 절대 경로가 OS 상한을 넘으면 InvalidPath /
// CapacityExceeded, 디렉터리를 만들 수 없으면 진단 None과 그 오류다. 거부는 아무것도 쓰지 않고
// 패키지는 저장하지 않은 채 남는다(고장을 걷으면 저장된다). 색인에 같은 경로 키·id가 둘이어도
// (AlreadyExists) 같은 거부이고, 대상 Stat 실패는 진단 None, 열린 것 사이에 없는 id는 NotFound다.
namespace {

// 대표 에셋 값을 쓰고(선택) 하위 객체를 만들거나 지정한 오류로 실패하는 초기화다.
class ValueInitializer final : public EditorAssetInitializer {
public:
    Result<void> Initialize(ConstructionContext& transaction,
                            ObjectHandle asset) noexcept override {
        ++calls;
        seen = asset;
        auto owner = transaction.GetForWrite<TestAsset>(asset);
        if (!owner) {
            return Result<void>::Failure(owner.Error());
        }

        owner.Value()->value = EditedValue;
        stagedId = owner.Value()->Id();
        if (makePart) {
            auto part = transaction.Create<TestPart>(TestPartType(), asset, "Part");
            if (!part) {
                return Result<void>::Failure(part.Error());
            }
        }

        if (nested != nullptr) {
            // 초기화 안의 만들기·저장은 Busy다(재진입 거부 — 리뷰 권장).
            nestedCreate = nested->Create(*nestedPath, TestAssetType()).Error();
            nestedSave = nested->Save(FixedId(BaseId), *nestedLock, EditorSaveOptions{}).Error();
        }

        if (openDuring != nullptr) {
            // 초기화 중 같은 집합의 다른 패키지를 연다(참조 해석과 같다).
            if (auto opened = openDuring->Open(openId); !opened) {
                return Result<void>::Failure(opened.Error());
            }
        }

        if (swallow) {
            // 실패한 Create의 오류를 삼킨다(계약 위반 — 트랜잭션이 오류로 고정되어 Commit이
            // 실패한다).
            (void)transaction.Create<TestPart>(TestPartType(), asset, InvalidPartName);
        }

        if (error != ErrorCode::None) {
            return Result<void>::Failure(error);
        }

        return Result<void>::Success();
    }

    ErrorCode error = ErrorCode::None;
    bool makePart = false;
    bool swallow = false;
    EditorPackageSet* openDuring = nullptr;
    AssetId openId{};
    EditorPackageSet* nested = nullptr;
    const LogicalPath* nestedPath = nullptr;
    const AssetDatabaseLock* nestedLock = nullptr;
    ErrorCode nestedCreate = ErrorCode::None;
    ErrorCode nestedSave = ErrorCode::None;
    uint32 calls = 0;
    ObjectHandle seen{};
    AssetId stagedId{};
};

} // namespace

// 만들기의 공개 전 초기화(7.1절 15 A2c3): 초기화는 공개 전 대표 에셋(Id()가 이미 패키지 id)에 한 번
// 불리고 그 값·하위 객체가 공개된다. 초기화가 실패하면(값을 쓴 뒤·하위 객체를 만든 뒤라도) 진단
// None과 그 오류이고 아무것도 남지 않는다(집합·Registry 객체 수 그대로, 같은 경로·id로 다시 만들 수
// 있다). 초기화가 실패한 트랜잭션 연산의 오류를 삼키고 성공을 돌려주면 공개가 그 오류로 실패하고
// 역시 아무것도 남지 않는다. 초기화가 같은 집합의 다른 패키지를 열어도(참조 해석) 새 패키지가
// 집합에 들어간다. 초기화 안의 Create·Save는 Busy다(재진입 거부). 초기화 없는 만들기도 대표 에셋
// Id()가 패키지 id다.
TEST(EditorPackageSave_CreateInitializer) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    EditorPackageSet& set = scene.set;
    auto path = ParseLogicalPath(env.allocator, "/Game/Fresh/Inited");
    auto plain = ParseLogicalPath(env.allocator, "/Game/Fresh/Plain");
    REQUIRE(path && plain);
    const usize count = set.Count();
    const uint64 live = set.Registry().Stats().liveObjects;
    const ErrorCode errors[] = {ErrorCode::NotFound, ErrorCode::OutOfMemory};
    for (ErrorCode error : errors) {
        for (bool makePart : PartCases) {
            ValueInitializer failing;
            failing.error = error;
            failing.makePart = makePart;
            AssetDiagnostic failure;
            auto made =
                set.Create(path.Value(), TestAssetType(), FixedId(FreshId), &failure, &failing);
            CHECK(!made && made.Error() == error);
            CHECK(failure.code == AssetDiagnosticCode::None && failure.error == error);
            CHECK(failing.calls == 1 && failing.stagedId == FixedId(FreshId));
            CHECK(set.Count() == count && set.Find(FixedId(FreshId)) == nullptr);
            CHECK(set.Registry().Stats().liveObjects == live);
        }
    }

    // 초기화가 트랜잭션 오류를 삼키면 공개(Commit)가 그 오류로 실패하고 아무것도 남지 않는다.
    {
        ValueInitializer swallowing;
        swallowing.swallow = true;
        AssetDiagnostic failure;
        auto made =
            set.Create(path.Value(), TestAssetType(), FixedId(FreshId), &failure, &swallowing);
        CHECK(!made && made.Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::None &&
              failure.error == ErrorCode::InvalidArgument);
        CHECK(set.Count() == count && set.Registry().Stats().liveObjects == live);
    }

    ValueInitializer initializer;
    initializer.makePart = true;
    initializer.openDuring = &set;
    initializer.openId = FixedId(LeafId);
    auto nestedPath = ParseLogicalPath(env.allocator, "/Game/Fresh/Nested");
    REQUIRE(nestedPath);
    REQUIRE(set.Open(FixedId(BaseId)));
    initializer.nested = &set;
    initializer.nestedPath = &nestedPath.Value();
    initializer.nestedLock = &scene.lock;
    auto made = set.Create(path.Value(), TestAssetType(), FixedId(FreshId), nullptr, &initializer);
    REQUIRE(made);
    CHECK(initializer.calls == 1);
    CHECK(initializer.nestedCreate == ErrorCode::Busy && initializer.nestedSave == ErrorCode::Busy);
    TestAsset* asset = AssetOf(set, made.Value());
    REQUIRE(asset != nullptr);
    CHECK(asset->value == EditedValue && asset->Id() == FixedId(FreshId));
    auto representative = set.Registry().FindChild(set.Access(), made.Value(), "");
    REQUIRE(representative && representative.Value() == initializer.seen);
    CHECK(set.Registry().FindChild(set.Access(), initializer.seen, "Part"));
    const EditorPackageInfo* info = set.Find(FixedId(FreshId));
    REQUIRE(info != nullptr);
    CHECK(info->unsaved && info->dirty && info->package == made.Value() && info->store != nullptr);
    CHECK(set.Find(FixedId(LeafId)) != nullptr);
    REQUIRE(set.Save(FixedId(FreshId), scene.lock, EditorSaveOptions{}));

    auto other = set.Create(plain.Value(), TestAssetType());
    REQUIRE(other);
    TestAsset* plainAsset = AssetOf(set, other.Value());
    REQUIRE(plainAsset != nullptr);
    CHECK(!plainAsset->Id().IsNull() && set.Find(plainAsset->Id()) != nullptr);
}

TEST(EditorPackageSave_NewPackageRefusals) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    EditorPackageSet& set = scene.set;
    MemoryFileSystem& files = scene.project.files;
    auto taken = ParseLogicalPath(env.allocator, "/Game/Taken");
    auto keyed = ParseLogicalPath(env.allocator, "/Game/Keyed");
    auto nested = ParseLogicalPath(env.allocator, "/Game/Deep/Nested");
    REQUIRE(taken && keyed && nested);
    auto takenMade = set.Create(taken.Value(), TestAssetType());
    auto keyedMade = set.Create(keyed.Value(), TestAssetType());
    auto nestedMade = set.Create(nested.Value(), TestAssetType());
    REQUIRE(takenMade && keyedMade && nestedMade);
    const AssetId takenId = set.At(0).package == takenMade.Value() ? set.At(0).id : AssetId{};
    (void)takenId;
    auto idOf = [&](ObjectHandle handle) noexcept {
        for (usize index = 0; index < set.Count(); ++index) {
            if (set.At(index).package == handle) {
                return set.At(index).id;
            }
        }

        return AssetId{};
    };

    // 대상 파일이 생겼다.
    REQUIRE(files.AddFile("Taken.rasset", sources.leaf.View()));
    AssetDiagnostic failure;
    CHECK(set.Save(idOf(takenMade.Value()), scene.lock, EditorSaveOptions{}, nullptr, &failure)
              .Error() == ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicatePath);
    // 색인에 같은 경로 키(대소문자만 다른 파일)가 생겼다.
    REQUIRE(files.AddFile("keyed.RASSET", sources.leaf.View()));
    REQUIRE(scene.project.Rebuild());
    CHECK(set.Save(idOf(keyedMade.Value()), scene.lock, EditorSaveOptions{}, nullptr, &failure)
              .Error() == ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicatePath);
    // 디렉터리를 만들 수 없다.
    files.faults.makeDirectory = MemoryError;
    CHECK(set.Save(idOf(nestedMade.Value()), scene.lock, EditorSaveOptions{}, nullptr, &failure)
              .Error() == FileErrorCode(MemoryError));
    CHECK(failure.code == AssetDiagnosticCode::None);
    CHECK(!files.Has("Deep") && files.replaces == 0 && !HasTemporary(files));
    files.faults.makeDirectory = {};
    REQUIRE(set.Save(idOf(nestedMade.Value()), scene.lock, EditorSaveOptions{}));
    CHECK(files.Has("Deep/Nested.rasset"));

    // OS 경로 상한: 루트가 긴 프로젝트(메모리 — 디스크를 보지 않는다).
    String root(env.allocator);
    REQUIRE(root.TryAppend(MemoryRoot));
    REQUIRE(root.TryAppend("/"));
    const usize fixed = 9 + 3 + EditorPackageExtension.ByteCount() + TemporaryFileSuffixBytes;
    while (root.View().ByteCount() + fixed <= MaxNativePathBytes()) {
        REQUIRE(root.TryAppend("r"));
    }

    ProjectLayout longLayout(env.allocator);
    REQUIRE(longLayout.Initialize(root.View()));
    MemoryFileSystem longFiles(env.allocator, longLayout.ContentDirectory());
    REQUIRE(longFiles.Ready());
    AssetIndex longIndex(env.allocator, longLayout);
    REQUIRE(longIndex.Rebuild(longFiles, PackageReadOptions{}));
    EditorPackageSetConfig config = Config(env, scene.project);
    config.files = &longFiles;
    config.layout = &longLayout;
    config.index = &longIndex;
    EditorPackageSet longSet(env.allocator, config);
    REQUIRE(longSet.Initialize());
    auto big = ParseLogicalPath(env.allocator, "/Game/Big");
    REQUIRE(big);
    CHECK(longSet.Create(big.Value(), TestAssetType(), {}, &failure).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(failure.code == AssetDiagnosticCode::InvalidPath);
    CHECK(longSet.Count() == 0);

    // 색인의 같은 경로 키 둘(AlreadyExists)은 만들기·첫 저장 모두 DuplicatePath, 색인의 같은 id
    // 둘은 DuplicateAssetId, 대상 Stat 실패는 진단 None과 그 오류, 열린 것 사이에 없는 id는
    // MarkDirty·Save NotFound(진단 자리 없이도).
    auto pair = ParseLogicalPath(env.allocator, "/Game/Pair");
    auto later = ParseLogicalPath(env.allocator, "/Game/Later");
    auto statted = ParseLogicalPath(env.allocator, "/Game/Statted");
    auto fourth = ParseLogicalPath(env.allocator, "/Game/Fourth");
    REQUIRE(pair && later && statted && fourth);
    auto laterMade = set.Create(later.Value(), TestAssetType());
    auto stattedMade = set.Create(statted.Value(), TestAssetType());
    REQUIRE(laterMade && stattedMade);
    REQUIRE(files.AddFile("Pair.rasset", sources.cycleA.View()));
    REQUIRE(files.AddFile("pair.RASSET", sources.cycleB.View()));
    REQUIRE(files.AddFile("later.RASSET", sources.cycleA.View()));
    REQUIRE(files.AddFile("LATER.rasset", sources.cycleB.View()));
    REQUIRE(files.AddFile("Copy.rasset", sources.villain.View()));
    REQUIRE(scene.project.Rebuild());
    CHECK(set.Create(pair.Value(), TestAssetType(), {}, &failure).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicatePath);
    CHECK(set.Save(idOf(laterMade.Value()), scene.lock, EditorSaveOptions{}, nullptr, &failure)
              .Error() == ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicatePath);
    CHECK(set.Create(fourth.Value(), TestAssetType(), FixedId(VillainId), &failure).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(failure.code == AssetDiagnosticCode::DuplicateAssetId);
    files.faults.stat = MemoryBusy;
    files.faults.statPath = "Statted.rasset";
    CHECK(set.Save(idOf(stattedMade.Value()), scene.lock, EditorSaveOptions{}, nullptr, &failure)
              .Error() == FileErrorCode(MemoryBusy));
    CHECK(failure.code == AssetDiagnosticCode::None);
    files.faults.stat = {};
    files.faults.statPath = nullptr;
    CHECK(set.MarkDirty(FixedId(1)).Error() == ErrorCode::NotFound);
    CHECK(set.Save(FixedId(1), scene.lock, EditorSaveOptions{}).Error() == ErrorCode::NotFound);
}

// 판정 순서의 거부(7.1절 15): Initialize 전·잠금을 잡지 않음 InvalidArgument, 열리지 않음
// NotFound, 읽기 전용(모르는 커스텀 버전) NewerVersion / Unsupported, 미해결 import가 있으면
// MissingDependency / InvalidArgument(related = 대상). dropUnresolved면 null로 저장하고
// DroppedUnresolvedImport 경고(related·import 위치)를 남기며 미해결 표가 빈다 — 다시 열면 참조가
// null이고 경고·미해결이 없으며 Base 의존도 없다. 거부는 아무것도 쓰지 않는다.
TEST(EditorPackageSave_Refusals) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    MemoryFileSystem& files = scene.project.files;
    AssetDiagnostic failure;
    {
        EditorPackageSet idle(env.allocator, scene.Config(env));
        CHECK(idle.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, nullptr, &failure)
                  .Error() == ErrorCode::InvalidArgument);
    }

    EditorPackageSet& set = scene.set;
    AssetDatabaseLock unheld;
    REQUIRE(set.Open(FixedId(LeafId)));
    CHECK(set.Save(FixedId(LeafId), unheld, EditorSaveOptions{}, nullptr, &failure).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, nullptr, &failure).Error() ==
          ErrorCode::NotFound);
    CHECK(failure.code == AssetDiagnosticCode::None);
    CHECK(set.Save(FixedId(9999), scene.lock, EditorSaveOptions{}).Error() == ErrorCode::NotFound);

    // hook 표 없는 집합·다른 프로젝트의 잠금은 InvalidArgument다(아무것도 쓰지 않는다).
    {
        EditorPackageSet noHooks(env.allocator, Config(env, scene.project));
        REQUIRE(noHooks.Initialize());
        REQUIRE(noHooks.Open(FixedId(LeafId)));
        CHECK(noHooks.Save(FixedId(LeafId), scene.lock, EditorSaveOptions{}, nullptr, &failure)
                  .Error() == ErrorCode::InvalidArgument);
        TempProject elsewhere("save-elsewhere");
        REQUIRE(elsewhere.made);
        ProjectLayout elsewhereLayout(env.allocator);
        REQUIRE(elsewhereLayout.Initialize(elsewhere.Root()));
        AssetDatabaseLock foreign;
        REQUIRE(foreign.Acquire(elsewhereLayout, 0));
        CHECK(set.Save(FixedId(LeafId), foreign, EditorSaveOptions{}, nullptr, &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::None);
        CHECK(files.replaces == 0);
    }

    // 편집이 대표 에셋을 Discard한 패키지: hook이 있으면 그 찾기 오류(NotFound), 없으면 writer가
    // 거부한다. 아무것도 쓰지 않는다.
    {
        AssetTypeHookTable hooks(env.allocator, env.types);
        auto registration = hooks.Register(TestAssetType(), AssetTypeHooks{&TestTags, nullptr});
        REQUIRE(registration);
        EditorPackageSetConfig config = Config(env, scene.project);
        config.hooks = &hooks;
        for (const bool withHooks : OwnedCases) {
            EditorPackageSet hollow(env.allocator, withHooks ? config : scene.Config(env));
            REQUIRE(hollow.Initialize());
            auto villain = hollow.Open(FixedId(VillainId));
            REQUIRE(villain);
            auto representative = hollow.Registry().FindChild(hollow.Access(), villain.Value(), "");
            REQUIRE(representative);
            REQUIRE(hollow.Registry().Discard(hollow.Access(), representative.Value()));
            auto saved =
                hollow.Save(FixedId(VillainId), scene.lock, EditorSaveOptions{}, nullptr, &failure);
            REQUIRE(!saved);
            if (withHooks) {
                CHECK(saved.Error() == ErrorCode::NotFound &&
                      failure.code == AssetDiagnosticCode::None);
            }

            CHECK(files.replaces == 0 && !HasTemporary(files));
        }
    }

    // 읽기 전용: 모르는 커스텀 버전으로 쓴 Leaf.
    PackageWriteOptions options = EditorOptions();
    options.customVersions = UnknownCustomVersions;
    const Array<uint8> readOnly =
        Write(env.allocator, *env.source, sources.packages[4].Handle(), options);
    REQUIRE(!readOnly.IsEmpty());
    files.Remove("Leaf.rasset");
    REQUIRE(files.AddFile("Leaf.rasset", readOnly.View()));
    REQUIRE(scene.project.Rebuild());
    EditorPackageSet other(env.allocator, scene.Config(env));
    REQUIRE(other.Initialize());
    REQUIRE(other.Open(FixedId(LeafId)));
    CHECK(other.Save(FixedId(LeafId), scene.lock, EditorSaveOptions{}, nullptr, &failure).Error() ==
          ErrorCode::Unsupported);
    CHECK(failure.code == AssetDiagnosticCode::NewerVersion);

    // 미해결 import: Base가 없는 프로젝트의 Hero.
    files.Remove("Base.rasset");
    REQUIRE(scene.project.Rebuild());
    EditorPackageSet missing(env.allocator, scene.Config(env));
    REQUIRE(missing.Initialize());
    REQUIRE(missing.Open(FixedId(HeroId)));
    CHECK(missing.Find(FixedId(HeroId))->unresolved.Size() == 1);
    CHECK(
        missing.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, nullptr, &failure).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::MissingDependency &&
          failure.related == FixedId(BaseId));
    CHECK(files.replaces == 0 && !HasTemporary(files));

    Array<EditorWarning> warnings(env.allocator);
    EditorSaveOptions drop;
    drop.dropUnresolved = true;
    REQUIRE(missing.Save(FixedId(HeroId), scene.lock, drop, &warnings));
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorWarningKind::DroppedUnresolvedImport);
    CHECK(warnings[0].diagnostic.asset == FixedId(HeroId));
    CHECK(warnings[0].diagnostic.related == FixedId(BaseId));
    CHECK(warnings[0].diagnostic.detail == 0);
    CHECK(missing.Find(FixedId(HeroId))->unresolved.IsEmpty());

    Reopened again(env, scene.project);
    REQUIRE(again.ready);
    Array<EditorWarning> reopenWarnings(env.allocator);
    auto hero = again.set.Open(FixedId(HeroId), &reopenWarnings);
    REQUIRE(hero);
    CHECK(reopenWarnings.IsEmpty());
    TestAsset* asset = AssetOf(again.set, hero.Value());
    REQUIRE(asset != nullptr);
    CHECK(asset->hard.Handle().IsNull());
    CHECK(asset->editorRef.Handle() == RepresentativeOf(again.set, DerivedId));
    auto entry = scene.project.index.FindById(FixedId(HeroId));
    REQUIRE(entry);
    for (const PackageDependencyEntry& dependency : entry.Value()->dependencies) {
        CHECK(!(dependency.asset == FixedId(BaseId)));
    }
}

// 외부 변경(5.5절 1): 연 뒤 대상이 다른 패키지로 바뀌었거나·지워졌거나·헤더가 깨졌으면
// ExternalModification / StaleHandle, 대상 열기·읽기의 IO 실패는 진단 None과 그 오류다. 아무것도
// 쓰지 않고 정보(지문·dirty)는 그대로다.
TEST(EditorPackageSave_ExternalModification) {
    enum class Change {
        Replaced,
        Removed,
        Corrupt,
        Busy,
        ReadFault
    };
    const Change changes[] = {Change::Replaced, Change::Removed, Change::Corrupt, Change::Busy,
                              Change::ReadFault};
    for (Change change : changes) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        SaveScene scene(env, sources);
        REQUIRE(scene.ready);
        MemoryFileSystem& files = scene.project.files;
        EditorPackageSet& set = scene.set;
        REQUIRE(set.Open(FixedId(LeafId)));
        REQUIRE(set.MarkDirty(FixedId(LeafId)));
        const PackageFingerprint before = set.Find(FixedId(LeafId))->fingerprint;
        Array<uint8> corrupt(env.allocator);
        REQUIRE(corrupt.TryAppend(sources.leaf.View()));
        corrupt[0] = static_cast<uint8>(corrupt[0] ^ 0xff);
        files.Remove("Leaf.rasset");
        switch (change) {
        case Change::Replaced:
            REQUIRE(files.AddFile("Leaf.rasset", sources.villain.View()));
            break;
        case Change::Removed:
            break;
        case Change::Corrupt:
            REQUIRE(files.AddFile("Leaf.rasset", corrupt.View()));
            break;
        case Change::Busy:
            REQUIRE(files.AddFile("Leaf.rasset", sources.leaf.View(), MemoryFault::OpenBusy));
            break;
        case Change::ReadFault:
            REQUIRE(files.AddFile("Leaf.rasset", sources.leaf.View(), MemoryFault::Read));
            break;
        }

        AssetDiagnostic failure;
        auto saved = set.Save(FixedId(LeafId), scene.lock, EditorSaveOptions{}, nullptr, &failure);
        REQUIRE(!saved);
        if (change == Change::Busy || change == Change::ReadFault) {
            CHECK(failure.code == AssetDiagnosticCode::None);
            CHECK(saved.Error() ==
                  FileErrorCode(change == Change::Busy ? MemoryBusy : MemoryError));
        } else {
            CHECK(failure.code == AssetDiagnosticCode::ExternalModification);
            CHECK(saved.Error() == ErrorCode::StaleHandle);
        }

        CHECK(failure.asset == FixedId(LeafId));
        CHECK(files.replaces == 0 && !HasTemporary(files));
        CHECK(set.Find(FixedId(LeafId))->fingerprint == before && set.Find(FixedId(LeafId))->dirty);
    }
}

namespace {

// 저장 IO 고장 하나다.
enum class SaveFault {
    Create,
    Write,
    CorruptWrite,
    Flush,
    Close,
    Replace,
    ReplaceAndDelete,
    ReopenTemp
};

// 고장 없는 저장의 쓰기 호출 수다(n번째 쓰기 고장을 모두 훑는다).
// clean은 그 저장의 결과 byte다(덮어쓰는 자리의 쓰기 손상은 결과가 이것과 같아야 한다).
uint32 CountSaveWrites(AssetEnv& env, Sources& sources, Array<uint8>& clean) noexcept {
    SaveScene scene(env, sources);
    if (!scene.ready || !scene.set.Open(FixedId(HeroId)) || !scene.set.MarkDirty(FixedId(HeroId))) {
        return 0;
    }

    const uint32 before = scene.project.files.WriteCalls();
    if (!scene.set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}) ||
        !clean.TryAppend(scene.project.files.Bytes("Hero.rasset"))) {
        return 0;
    }

    return scene.project.files.WriteCalls() - before;
}

} // namespace

// 저장 IO 고장(5.5절 2–7): 임시 파일 만들기·n번째 쓰기(모든 n)·Flush·Close·검증용 다시 열기·교체
// 실패는 진단 None과
// 그 오류, 쓰기가 byte를 망가뜨리면(모든 n) 재검증이 그 진단(CorruptPackage)으로 거부한다(나중에
// 덮어쓰는 헤더 자리 하나만 저장되고 결과는 깨끗한 저장과 같다). 어느
// 경우에도 대상 byte·정보(지문·dirty)는 그대로이고 임시 파일은 지워진다. 교체와 지우기가 모두
// 실패하면 임시 파일이 남고, 다음 저장이 그 고아를 지운다. 고아를 지울 수 없으면 저장은 성공하고
// OrphanNotDeleted 경고다. NotDurable 교체는 성공 + NotDurable 경고다.
TEST(EditorPackageSave_IoFailures) {
    AssetEnv probeEnv;
    REQUIRE(probeEnv.Ready());
    Sources probeSources(probeEnv.allocator);
    REQUIRE(MakeSources(probeEnv, probeSources));
    Array<uint8> clean(probeEnv.allocator);
    const uint32 writes = CountSaveWrites(probeEnv, probeSources, clean);
    REQUIRE(writes > 0);
    struct Case {
        SaveFault fault;
        uint32 call;
    };
    Array<Case> cases(probeEnv.allocator);
    const SaveFault single[] = {
        SaveFault::Create,           SaveFault::Flush,     SaveFault::Close, SaveFault::Replace,
        SaveFault::ReplaceAndDelete, SaveFault::ReopenTemp};
    for (SaveFault fault : single) {
        REQUIRE(cases.TryPushBack(Case{fault, 0}));
    }

    for (uint32 call = 1; call <= writes; ++call) {
        REQUIRE(cases.TryPushBack(Case{SaveFault::Write, call}));
        REQUIRE(cases.TryPushBack(Case{SaveFault::CorruptWrite, call}));
    }

    uint32 overwritten = 0;
    for (const Case& one : cases) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        SaveScene scene(env, sources);
        REQUIRE(scene.ready);
        MemoryFileSystem& files = scene.project.files;
        EditorPackageSet& set = scene.set;
        REQUIRE(set.Open(FixedId(HeroId)));
        REQUIRE(set.MarkDirty(FixedId(HeroId)));
        const PackageFingerprint before = set.Find(FixedId(HeroId))->fingerprint;
        const uint32 base = files.WriteCalls();
        switch (one.fault) {
        case SaveFault::Create:
            files.faults.create = MemoryBusy;
            break;
        case SaveFault::Write:
            files.faults.failWriteCall = base + one.call;
            break;
        case SaveFault::CorruptWrite:
            files.faults.corruptWriteCall = base + one.call;
            break;
        case SaveFault::Flush:
            files.faults.flush = MemoryError;
            break;
        case SaveFault::Close:
            files.faults.close = MemoryError;
            break;
        case SaveFault::Replace:
            files.faults.replace = MemoryBusy;
            break;
        case SaveFault::ReplaceAndDelete:
            files.faults.replace = MemoryBusy;
            files.faults.remove = MemoryError;
            break;
        case SaveFault::ReopenTemp:
            files.faults.openRead = MemoryBusy;
            files.faults.readSuffix = ".tmp";
            break;
        }

        AssetDiagnostic failure;
        auto saved = set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, nullptr, &failure);
        if (saved) {
            // 나중에 덮어쓰는 자리(헤더 자리의 0)의 손상만 저장되고, 결과는 깨끗한 저장과 같다.
            CHECK(one.fault == SaveFault::CorruptWrite);
            CHECK(SameBytes(files.Bytes("Hero.rasset"), clean.View()));
            if (one.fault != SaveFault::CorruptWrite) {
                printf("  fault %u call %u: saved\n", static_cast<unsigned>(one.fault), one.call);
            }

            ++overwritten;
            continue;
        }

        if (one.fault == SaveFault::CorruptWrite) {
            CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
        } else {
            CHECK(failure.code == AssetDiagnosticCode::None);
            const platform::FileStatus expected =
                one.fault == SaveFault::Create || one.fault == SaveFault::Replace ||
                        one.fault == SaveFault::ReplaceAndDelete ||
                        one.fault == SaveFault::ReopenTemp
                    ? MemoryBusy
                    : MemoryError;
            CHECK(saved.Error() == FileErrorCode(expected));
        }

        CHECK(SameBytes(files.Bytes("Hero.rasset"), sources.hero.View()));
        CHECK(set.Find(FixedId(HeroId))->fingerprint == before && set.Find(FixedId(HeroId))->dirty);
        CHECK(files.replaces == 0);
        CHECK(HasTemporary(files) == (one.fault == SaveFault::ReplaceAndDelete));
        if (one.fault != SaveFault::ReplaceAndDelete) {
            continue;
        }

        // 남은 고아는 다음 저장이 지운다. 지울 수 없으면 OrphanNotDeleted 경고.
        files.faults.replace = {};
        Array<EditorWarning> warnings(env.allocator);
        REQUIRE(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, &warnings));
        REQUIRE(warnings.Size() == 1);
        CHECK(warnings[0].kind == EditorWarningKind::OrphanNotDeleted);
        CHECK(warnings[0].diagnostic.error == FileErrorCode(MemoryError));
        CHECK(HasTemporary(files));
        // 경고 표 없이도 같고, 지우기 실패가 둘이어도 경고는 첫 오류 하나다.
        REQUIRE(files.AddFile("Second.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp",
                              sources.leaf.View()));
        REQUIRE(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}));
        warnings.Clear();
        REQUIRE(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, &warnings));
        CHECK(warnings.Size() == 1);
        files.faults.remove = {};
        warnings.Clear();
        REQUIRE(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, &warnings));
        CHECK(warnings.IsEmpty() && !HasTemporary(files));
    }

    CHECK(overwritten == 1);

    // NotDurable: 교체는 보이므로 성공 + 경고.
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    REQUIRE(scene.set.Open(FixedId(HeroId)));
    scene.project.files.faults.replace = MemoryNotDurable;
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(scene.set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, &warnings));
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorWarningKind::NotDurable);
    CHECK(warnings[0].diagnostic.code == AssetDiagnosticCode::NotDurable);
    CHECK(scene.project.files.replaces == 1 && !HasTemporary(scene.project.files));

    // 디렉터리 열거 실패: 고아 정리를 못 하면 경고.
    scene.project.files.faults.replace = {};
    REQUIRE(scene.project.files.AddDirectory("Sub"));
    warnings.Clear();
    scene.project.files.Remove("");
    REQUIRE(scene.project.files.AddDirectory("", false, MemoryFault::Enumerate));
    REQUIRE(scene.set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, &warnings));
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorWarningKind::OrphanNotDeleted);
    CHECK(warnings[0].diagnostic.error == FileErrorCode(MemoryError));
}

// dirty와 hook(7.1절 5·15): MarkDirty는 열리지 않은 id에 NotFound, 세대를 올린다. Tags는 저장마다
// hook으로 다시 만든다 — 파일에 있던 다른 태그는 남지 않고 hook 태그만 남으며, hook이 없으면 Tags가
// 빈다. Build 의존은 hook이 낸다. 저장 중(hook 안) MarkDirty가 오면 세대가 바뀌어 dirty가 남는다.
// hook 오류(태그·Build)와 태그가 Record 한도를 넘는 것(CapacityExceeded)은 진단 None과 그 오류이고
// 아무것도 쓰지 않는다.
TEST(EditorPackageSave_DirtyAndHooks) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    // Hero 파일에 옛 태그를 넣는다.
    const PackageTagInput oldTags[] = {{"Old", "x"}};
    auto tagBytes = WritePackageTags(env.allocator, oldTags);
    REQUIRE(tagBytes);
    PackageWriteOptions options = EditorOptions();
    options.bulkSource = &sources.bulk;
    options.tags = tagBytes.Value().View();
    const Array<uint8> tagged =
        Write(env.allocator, *env.source, sources.packages[2].Handle(), options);
    REQUIRE(!tagged.IsEmpty());
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    MemoryFileSystem& files = scene.project.files;
    files.Remove("Hero.rasset");
    REQUIRE(files.AddFile("Hero.rasset", tagged.View()));
    REQUIRE(scene.project.Rebuild());
    REQUIRE(scene.project.index.FindById(FixedId(HeroId)).Value()->tags.Size() == 1);

    AssetTypeHookTable hooks(env.allocator, env.types);
    auto registration = hooks.Register(TestAssetType(), AssetTypeHooks{&TestTags, &TestBuild});
    REQUIRE(registration);
    EditorPackageSetConfig config = Config(env, scene.project);
    config.hooks = &hooks;
    EditorPackageSet set(env.allocator, config);
    REQUIRE(set.Initialize());
    hookState = HookState{};
    hookState.set = &set;
    CHECK(set.MarkDirty(FixedId(HeroId)).Error() == ErrorCode::NotFound);
    REQUIRE(set.Open(FixedId(HeroId)));
    REQUIRE(set.MarkDirty(FixedId(HeroId)));
    REQUIRE(set.MarkDirty(FixedId(HeroId)));
    CHECK(set.Find(FixedId(HeroId))->generation == 2);

    // hook 오류는 아무것도 쓰지 않는다.
    AssetDiagnostic failure;
    hookState.failTags = true;
    CHECK(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, nullptr, &failure).Error() ==
          ErrorCode::Busy);
    CHECK(failure.code == AssetDiagnosticCode::None);
    hookState.failTags = false;
    hookState.failBuild = true;
    CHECK(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, nullptr, &failure).Error() ==
          ErrorCode::Unsupported);
    hookState.failBuild = false;
    hookState.bigTags = true;
    CHECK(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, nullptr, &failure).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(failure.code == AssetDiagnosticCode::None);
    hookState.bigTags = false;
    CHECK(files.replaces == 0 && set.Find(FixedId(HeroId))->dirty);

    // hook 안의 MarkDirty: 세대가 바뀌어 dirty가 남는다.
    hookState.markDirty = true;
    REQUIRE(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}));
    CHECK(set.Find(FixedId(HeroId))->dirty && set.Find(FixedId(HeroId))->generation == 3);
    hookState.markDirty = false;
    REQUIRE(set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}));
    CHECK(!set.Find(FixedId(HeroId))->dirty);
    auto entry = scene.project.index.FindById(FixedId(HeroId));
    REQUIRE(entry);
    REQUIRE(entry.Value()->tags.Size() == 1);
    CHECK(entry.Value()->tags[0].key.View() == "Kind" &&
          entry.Value()->tags[0].value.View() == "Hero");
    bool build = false;
    for (const PackageDependencyEntry& dependency : entry.Value()->dependencies) {
        build = build || (dependency.asset == FixedId(LeafId) &&
                          dependency.kind == PackageDependencyKind::Build);
    }

    CHECK(build);

    // 빈 hook 표면 Tags가 빈다(옛 태그는 옮기지 않는다).
    EditorPackageSet plain(env.allocator, scene.Config(env));
    REQUIRE(plain.Initialize());
    REQUIRE(plain.Open(FixedId(HeroId)));
    REQUIRE(plain.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}));
    CHECK(scene.project.index.FindById(FixedId(HeroId)).Value()->tags.IsEmpty());
    hookState = HookState{};
}

// TypeRegistry 종료(7.1절 15 — 리뷰 필수): 호출자의 TypeRegistry가 닫힌 뒤 hook 있는 저장은 대표
// 에셋 타입 조회가 ShuttingDown이라 진단 None과 그 오류이고 아무것도 쓰지 않는다(단언하지 않는다
// — 로드의 같은 조회와 같다, 5.7절 19).
TEST(EditorPackageSave_TypeRegistryClosed) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    REQUIRE(scene.set.Open(FixedId(HeroId)));
    REQUIRE(scene.set.MarkDirty(FixedId(HeroId)));
    (void)env.types.Shutdown();
    AssetDiagnostic failure;
    CHECK(scene.set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}, nullptr, &failure)
              .Error() == ErrorCode::ShuttingDown);
    CHECK(failure.code == AssetDiagnosticCode::None && failure.asset == FixedId(HeroId));
    CHECK(scene.project.files.replaces == 0 && !HasTemporary(scene.project.files));
    CHECK(scene.set.Find(FixedId(HeroId))->dirty);
}

// ImportRecord 보존(7.1절 15): 소유 기록을 가진 Base와 참조 기록 + ImportOwner를 가진 Base를
// 저장하면 ImportRecord byte와 ImportOwner 의존이 그대로이고 색인 요약이 같다.
TEST(EditorPackageSave_ImportRecordPreserved) {
    for (const bool owned : OwnedCases) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        const ImportSourceInput recordSources[] = {{"source", "", "Base.png", Sha256Digest{}, 1}};
        const ImportOutputInput recordOutputs[] = {{"", FixedId(BaseId), IdOf<BaseGameAsset>()}};
        ImportRecordInput input;
        input.owner = FixedId(owned ? BaseId : DerivedId);
        if (owned) {
            input.importerId = Guid{0x494d50, 3};
            input.importerName = "BaseImporter";
            input.importerVersion = 2;
            input.sources = recordSources;
            input.outputs = recordOutputs;
        }

        auto record = WriteImportRecord(env.allocator, FixedId(BaseId), input);
        REQUIRE(record);
        const PackageDependencyEntry owner[] = {
            {FixedId(DerivedId), PackageDependencyKind::ImportOwner, TypeId{}, false}};
        PackageWriteOptions options = EditorOptions();
        options.importRecord = record.Value().View();
        if (!owned) {
            options.extraDependencies = owner;
        }

        const Array<uint8> base =
            Write(env.allocator, *env.source, sources.packages[0].Handle(), options);
        REQUIRE(!base.IsEmpty());
        SaveScene scene(env, sources);
        REQUIRE(scene.ready);
        scene.project.files.Remove("Base.rasset");
        REQUIRE(scene.project.files.AddFile("Base.rasset", base.View()));
        REQUIRE(scene.project.Rebuild());
        EditorPackageSet& set = scene.set;
        REQUIRE(set.Open(FixedId(BaseId)));
        REQUIRE(set.MarkDirty(FixedId(BaseId)));
        REQUIRE(set.Save(FixedId(BaseId), scene.lock, EditorSaveOptions{}));
        CHECK(scene.project.files.replaces == 1);
        auto entry = scene.project.index.FindById(FixedId(BaseId));
        REQUIRE(entry);
        CHECK(entry.Value()->importRecord.present);
        CHECK(entry.Value()->importRecord.owner == input.owner);
        usize owners = 0;
        for (const PackageDependencyEntry& dependency : entry.Value()->dependencies) {
            owners += dependency.kind == PackageDependencyKind::ImportOwner ? 1 : 0;
        }

        CHECK(owners == (owned ? 0u : 1u));
        Reopened again(env, scene.project);
        REQUIRE(again.ready);
        REQUIRE(again.set.Open(FixedId(BaseId)));
        CHECK(
            SameBytes(again.set.Find(FixedId(BaseId))->importRecord.View(), record.Value().View()));
    }
}

// 저장 임시 파일 모양의 고아 이름이다(guid 36자 소문자).
constexpr const char* OrphanName = "Hero.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp";

// 할당 실패(7.1절 15): 집합 allocator와 색인 allocator의 할당을 저장 중에 하나씩 실패시키면 Save는
// OutOfMemory다(hook·경고 표·고아 정리까지 지나는 장면). 교체 전의 실패는 대상 byte·정보가
// 그대로이고 임시 파일이 없다. 교체 뒤의 실패(Rebind·Refresh)는 대상이 새 byte이고 정보 지문이
// 그것을 가리키며 dirty는 지워진다(파일이 메모리와 같다). 고아 정리의 할당 실패는 저장 성공이고
// 고아가 남는다(경고는 잡아 둔 자리에 — OrphanNotDeleted / OutOfMemory). 기존 패키지(bulk)와 새
// 패키지를 모두 훑는다.
TEST(EditorPackageSave_AllocationFailures) {
    for (const bool fresh : FreshCases) {
        AssetEnv env;
        REQUIRE(env.Ready());
        Sources sources(env.allocator);
        REQUIRE(MakeSources(env, sources));
        AssetTypeHookTable hooks(env.allocator, env.types);
        auto registration = hooks.Register(TestAssetType(), AssetTypeHooks{&TestTags, &TestBuild});
        REQUIRE(registration);
        hookState = HookState{};
        uint32 failures = 0;
        uint32 orphanKept = 0;
        for (uint64 attempt = 1;; ++attempt) {
            TempProject root("save-alloc");
            REQUIRE(root.made);
            Project project(env.allocator, root.Root());
            REQUIRE(project.ready && project.files.Ready());
            REQUIRE(project.files.AddFile("Base.rasset", sources.base.View()));
            REQUIRE(project.files.AddFile("Derived.rasset", sources.derived.View()));
            REQUIRE(project.files.AddFile("Hero.rasset", sources.hero.View()));
            REQUIRE(project.files.AddDirectory("Fresh"));
            REQUIRE(project.files.AddFile(OrphanName, sources.leaf.View()));
            REQUIRE(project.files.AddFile(
                "Fresh/Made.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp", sources.leaf.View()));
            AssetDatabaseLock lock;
            REQUIRE(lock.Acquire(project.layout, 0));
            TaggedFailAllocator failing(env.allocator);
            AssetIndex index(failing, project.layout);
            REQUIRE(index.Rebuild(project.files, PackageReadOptions{}));
            EditorPackageSetConfig config = Config(env, project);
            config.index = &index;
            config.hooks = &hooks;
            EditorPackageSet set(failing, config);
            REQUIRE(set.Initialize());
            AssetId id = FixedId(HeroId);
            if (fresh) {
                auto path = ParseLogicalPath(env.allocator, "/Game/Fresh/Made");
                REQUIRE(path);
                REQUIRE(set.Create(path.Value(), TestAssetType(), FixedId(FreshId)));
                id = FixedId(FreshId);
            } else {
                REQUIRE(set.Open(id));
                REQUIRE(set.MarkDirty(id));
            }

            const StringView orphan =
                fresh ? "Fresh/Made.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp" : OrphanName;
            const PackageFingerprint before = set.Find(id)->fingerprint;
            Array<EditorWarning> warnings(failing);
            AssetDiagnostic failure;
            failing.FailOnAttempt(attempt);
            auto saved = set.Save(id, lock, EditorSaveOptions{}, &warnings, &failure);
            const bool failed = failing.Failed();
            failing.DisableFailure();
            if (!failed) {
                REQUIRE(saved);
                CHECK(!set.Find(id)->dirty && warnings.IsEmpty());
                CHECK(!project.files.Has(orphan));
                break;
            }

            const StringView name = fresh ? "Fresh/Made.rasset" : "Hero.rasset";
            if (saved) {
                // 고아 정리의 할당 실패: 저장은 끝났고 고아가 남는다.
                CHECK(project.files.Has(orphan) && !set.Find(id)->dirty);
                CHECK(warnings.Size() == 1 &&
                      warnings[0].kind == EditorWarningKind::OrphanNotDeleted &&
                      warnings[0].diagnostic.error == ErrorCode::OutOfMemory);
                ++orphanKept;
                continue;
            }

            CHECK(saved.Error() == ErrorCode::OutOfMemory);
            CHECK(failure.code == AssetDiagnosticCode::None &&
                  failure.error == ErrorCode::OutOfMemory);
            // 교체 뒤의 일은 교체 전에 준비하므로 실패는 언제나 교체 전이다: 대상·정보·색인 그대로,
            // 임시 파일 없음(고아는 정리 전이라 남는다).
            CHECK(project.files.replaces == 0 && project.files.Has(orphan));
            CHECK(set.Find(id)->dirty && set.Find(id)->fingerprint == before);
            CHECK(fresh ? !project.files.Has(name)
                        : SameBytes(project.files.Bytes(name), sources.hero.View()));
            auto entry = index.FindById(FixedId(HeroId));
            CHECK(entry && (fresh || entry.Value()->fingerprint == before));
            ++failures;
        }

        CHECK(failures > 0 && orphanKept > 0);
        hookState = HookState{};
    }
}

// 만들기의 할당 실패(7.1절 15): Create의 할당을 하나씩 실패시키면 OutOfMemory(진단 None)이고 집합에
// 아무것도 남지 않는다. 실패한 Create가 늘려 둔 표 용량이 다음 시도의 할당 순서를 바꾸지 않게
// 시도마다 같은 상태의 새 집합(Base 열림 + 저장하지 않은 새 패키지 하나)에서 만든다.
TEST(EditorPackageSave_CreateAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    REQUIRE(project.files.AddFile("Base.rasset", sources.base.View()));
    REQUIRE(project.Rebuild());
    auto other = ParseLogicalPath(env.allocator, "/Game/Unsaved/Other");
    auto path = ParseLogicalPath(env.allocator, "/Game/Made/Thing");
    REQUIRE(other && path);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        TaggedFailAllocator failing(env.allocator);
        EditorPackageSet set(failing, Config(env, project));
        REQUIRE(set.Initialize());
        REQUIRE(set.Open(FixedId(BaseId)));
        REQUIRE(set.Create(other.Value(), TestAssetType()));
        AssetDiagnostic failure;
        failing.FailOnAttempt(attempt);
        auto made = set.Create(path.Value(), TestAssetType(), FixedId(FreshId), &failure);
        const bool failed = failing.Failed();
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(made);
            CHECK(set.Count() == 3 && set.Find(FixedId(FreshId)) != nullptr);
            break;
        }

        CHECK(!made && made.Error() == ErrorCode::OutOfMemory);
        CHECK(failure.code == AssetDiagnosticCode::None);
        CHECK(set.Count() == 2 && set.Find(FixedId(FreshId)) == nullptr);
        ++failures;
    }

    CHECK(failures > 0);
}

// 고아 임시 파일 모양(5.5절 8): 저장은 대상 디렉터리의 정확히 "<이름>.rasset.<36자 소문자
// guid>.tmp" 모양인 일반 파일만 지운다(확장자 ".rasset"은 대소문자 무시). 다른 디렉터리, 대문자
// guid, ".TMP", guid 앞 '.' 없음, ".rasset"이 아닌 이름, 빈 이름, 짧은 이름, 하이픈 자리·16진이
// 아닌 문자, 디렉터리·링크, UTF-8이 아닌 이름은 남긴다. 경고 표 없이 저장해도 같다.
TEST(EditorPackageSave_OrphanShapes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    SaveScene scene(env, sources);
    REQUIRE(scene.ready);
    MemoryFileSystem& files = scene.project.files;
    const StringView removed[] = {
        "Hero.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp",
        "Other.RASSET.89abcdef-0123-4567-89ab-cdef01234567.tmp",
    };
    const StringView kept[] = {
        "Sub/Hero.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp",
        "Hero.rasset.0123ABCD-4567-89ab-cdef-0123456789ab.tmp",
        "Hero.rasset.0123abcd-4567-89ab-cdef-0123456789ab.TMP",
        "Hero.rasset-0123abcd-4567-89ab-cdef-0123456789ab.tmp",
        "Hero.raset.0123abcd-4567-89ab-cdef-0123456789ab.tmp",
        ".rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp",
        "x.tmp",
        "Hero.rasset.0123abcd4-567-89ab-cdef-0123456789ab.tmp",
        "Hero.rasset.0123abcg-4567-89ab-cdef-0123456789ab.tmp",
        "Hero.rasset.0123abcd-4567-89ab-cdef-0123456789a!.tmp",
        "Link.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp",
        "\xff.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp",
    };
    REQUIRE(files.AddDirectory("Sub"));
    REQUIRE(files.AddDirectory("Dir.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp"));
    for (StringView name : removed) {
        REQUIRE(files.AddFile(name, sources.leaf.View()));
    }

    for (StringView name : kept) {
        if (name.Contains("/", StringComparison::CaseSensitive) && !name.StartsWith("Sub/")) {
            continue;
        }

        REQUIRE(files.AddFile(name, sources.leaf.View(), MemoryFault::None,
                              platform::FileType::File, name.StartsWith("Link.")));
    }

    REQUIRE(scene.set.Open(FixedId(HeroId)));
    REQUIRE(scene.set.Save(FixedId(HeroId), scene.lock, EditorSaveOptions{}));
    for (StringView name : removed) {
        CHECK(!files.Has(name));
    }

    for (StringView name : kept) {
        if (name.Contains("/", StringComparison::CaseSensitive) && !name.StartsWith("Sub/")) {
            continue;
        }

        CHECK(files.Has(name));
    }

    CHECK(files.Has("Dir.rasset.0123abcd-4567-89ab-cdef-0123456789ab.tmp"));
}

// 색인 Refresh(7.1절 15): 바뀐 파일은 항목을 바꾸고, 새 파일은 더하고, 없어진 파일은 빼며, 같은
// id 진단·표시는 전체에서 다시 정한다(둘이 되면 둘 다, 하나가 없어지면 풀린다). 이름 규칙 위반은
// InvalidPath 진단, 링크·디렉터리는 색인하지 않고, Stat 실패는 진단 None과 그 오류다. 같은 경로
// 키 진단(DuplicatePath)도 다시 정한다. `.rasset`이 아닌 이름은 InvalidArgument(색인 그대로), 할당
// 실패(긴 Content 경로·Stat 진단 포함)는 색인을 그대로 둔다(준비·확정 — 준비는 색인을 바꾸지
// 않는다).
TEST(AssetIndex_Refresh) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    Project project(env.allocator);
    REQUIRE(project.ready && project.files.Ready());
    MemoryFileSystem& files = project.files;
    REQUIRE(files.AddFile("Base.rasset", sources.base.View()));
    REQUIRE(files.AddFile("Leaf.rasset", sources.leaf.View()));
    REQUIRE(project.Rebuild());
    AssetIndex& index = project.index;
    const PackageReadOptions read{};
    REQUIRE(index.Count() == 2);

    // 바뀐 파일.
    files.Remove("Leaf.rasset");
    REQUIRE(files.AddFile("Leaf.rasset", sources.villain.View()));
    REQUIRE(index.Refresh(files, "Leaf.rasset", read));
    REQUIRE(index.Count() == 2);
    CHECK(index.FindById(FixedId(VillainId)) && !index.FindById(FixedId(LeafId)));

    // 새 파일(같은 id — 둘 다 DuplicateAssetId), 하나를 지우면 풀린다.
    REQUIRE(files.AddFile("Copy.rasset", sources.villain.View()));
    REQUIRE(index.Refresh(files, "Copy.rasset", read));
    REQUIRE(index.Count() == 3);
    CHECK(index.FindById(FixedId(VillainId)).Error() == ErrorCode::AlreadyExists);
    usize duplicates = 0;
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        duplicates += diagnostic.code == AssetDiagnosticCode::DuplicateAssetId ? 1 : 0;
    }

    CHECK(duplicates == 2);
    files.Remove("Copy.rasset");
    REQUIRE(index.Refresh(files, "Copy.rasset", read));
    CHECK(index.Count() == 2 && index.Diagnostics().IsEmpty());
    CHECK(index.FindById(FixedId(VillainId)));

    // 이름 규칙 위반·링크·디렉터리·Stat 실패.
    REQUIRE(files.AddFile("Bad#Name.rasset", sources.leaf.View()));
    REQUIRE(index.Refresh(files, "Bad#Name.rasset", read));
    REQUIRE(index.Diagnostics().Size() == 1);
    CHECK(index.Diagnostics()[0].code == AssetDiagnosticCode::InvalidPath);
    REQUIRE(files.AddFile("Link.rasset", sources.leaf.View(), MemoryFault::None,
                          platform::FileType::File, true));
    REQUIRE(files.AddDirectory("Dir.rasset"));
    REQUIRE(index.Refresh(files, "Link.rasset", read));
    REQUIRE(index.Refresh(files, "Dir.rasset", read));
    CHECK(index.Count() == 2 && index.Diagnostics().Size() == 1);
    files.faults.stat = MemoryError;
    files.faults.statPath = "Base.rasset";
    REQUIRE(index.Refresh(files, "Base.rasset", read));
    files.faults.stat = {};
    CHECK(index.Count() == 1);
    REQUIRE(index.Diagnostics().Size() == 2);
    CHECK(index.Diagnostics()[1].fileName.View() == "Base.rasset");
    CHECK(index.Diagnostics()[1].code == AssetDiagnosticCode::None &&
          index.Diagnostics()[1].error == FileErrorCode(MemoryError));
    REQUIRE(index.Refresh(files, "Base.rasset", read));
    CHECK(index.Count() == 2 && index.Diagnostics().Size() == 1);

    // 인자.
    CHECK(index.Refresh(files, "Base.txt", read).Error() == ErrorCode::InvalidArgument);
    CHECK(index.Refresh(files, ".rasset", read).Error() == ErrorCode::InvalidArgument);
    CHECK(index.Count() == 2);

    // 할당 실패: 색인 그대로.
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex fresh(failing, project.layout);
        REQUIRE(fresh.Rebuild(files, read));
        failing.FailOnAttempt(attempt);
        const Result<void> result = fresh.Refresh(files, "Leaf.rasset", read);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(result);
            CHECK(fresh.Count() == 2);
            break;
        }

        CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
        CHECK(fresh.Count() == 2 && fresh.FindById(FixedId(VillainId)));
        ++failures;
    }

    CHECK(failures > 0);

    // 같은 경로 키 진단: 둘이면 둘 다 DuplicatePath, 하나를 지우고 Refresh하면 풀린다.
    REQUIRE(files.AddFile("leaf.RASSET", sources.cycleA.View()));
    REQUIRE(project.Rebuild());
    usize paths = 0;
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        paths += diagnostic.code == AssetDiagnosticCode::DuplicatePath ? 1 : 0;
    }

    CHECK(paths == 2);
    files.Remove("leaf.RASSET");
    REQUIRE(index.Refresh(files, "leaf.RASSET", read));
    for (const AssetIndexDiagnostic& diagnostic : index.Diagnostics()) {
        CHECK(diagnostic.code != AssetDiagnosticCode::DuplicatePath);
    }

    // 긴 Content 경로(경로 임시 저장소 512 byte를 넘는다)와 Stat 진단의 할당 실패.
    String root(env.allocator);
    REQUIRE(root.TryAppend(MemoryRoot));
    for (uint32 level = 0; level < 8; ++level) {
        REQUIRE(root.TryAppend("/Root-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-0123"));
    }

    ProjectLayout longLayout(env.allocator);
    REQUIRE(longLayout.Initialize(root.View()));
    MemoryFileSystem longFiles(env.allocator, longLayout.ContentDirectory());
    REQUIRE(longFiles.Ready());
    REQUIRE(longFiles.AddFile("Leaf.rasset", sources.leaf.View()));
    REQUIRE(longFiles.AddFile("Base.rasset", sources.base.View()));
    const bool statFaults[] = {false, true};
    for (bool statFault : statFaults) {
        uint32 longFailures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            AssetIndex fresh(failing, longLayout);
            REQUIRE(fresh.Rebuild(longFiles, read));
            longFiles.faults.stat = statFault ? MemoryError : platform::FileStatus{};
            longFiles.faults.statPath = statFault ? "Base.rasset" : nullptr;
            failing.FailOnAttempt(attempt);
            const Result<void> result = fresh.Refresh(longFiles, "Base.rasset", read);
            const bool failed = failing.Attempts() >= attempt;
            failing.DisableFailure();
            longFiles.faults.stat = {};
            longFiles.faults.statPath = nullptr;
            if (!failed) {
                REQUIRE(result);
                CHECK(fresh.Count() == (statFault ? 1u : 2u));
                CHECK(fresh.Diagnostics().Size() == (statFault ? 1u : 0u));
                break;
            }

            CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
            CHECK(fresh.Count() == 2 && fresh.Diagnostics().IsEmpty());
            ++longFailures;
        }

        CHECK(longFailures > 0);
    }
}

// 실제 디스크(5.5절): 물리 파일 시스템에서 기존 패키지를 고쳐 저장하고 새 패키지를 새 디렉터리에
// 만들어 저장한 뒤 다시 연다. 임시 파일이 남지 않는다.
TEST(EditorPackageSave_Disk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    TempProject project("editor-save");
    REQUIRE(project.made);
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    REQUIRE(WriteContentFile(env.allocator, project, "Base.rasset", sources.base.View()));
    REQUIRE(WriteContentFile(env.allocator, project, "Derived.rasset", sources.derived.View()));
    REQUIRE(
        WriteContentFile(env.allocator, project, "Characters/Hero.rasset", sources.hero.View()));
    AssetIndex index(env.allocator, layout);
    REQUIRE(index.Rebuild(platform::PhysicalFileSystem(), PackageReadOptions{}));
    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(layout, 0));
    EditorPackageSetConfig config;
    config.types = &env.types;
    config.files = &platform::PhysicalFileSystem();
    config.layout = &layout;
    config.index = &index;
    config.registryContext = ContextId{0x4550534554ull, 3};
    config.registryOptions.names = &env.names;
    config.writeEngineVersion = TestEngineVersion;
    AssetTypeHookTable hooks(env.allocator, env.types);
    config.hooks = &hooks;
    {
        EditorPackageSet set(env.allocator, config);
        REQUIRE(set.Initialize());
        auto hero = set.Open(FixedId(HeroId));
        REQUIRE(hero);
        TestAsset* asset = AssetOf(set, hero.Value());
        REQUIRE(asset != nullptr);
        asset->value = EditedValue;
        REQUIRE(set.MarkDirty(FixedId(HeroId)));
        REQUIRE(set.Save(FixedId(HeroId), lock, EditorSaveOptions{}));
        auto path = ParseLogicalPath(env.allocator, "/Game/Made/New");
        REQUIRE(path);
        REQUIRE(set.Create(path.Value(), TestAssetType(), FixedId(FreshId)));
        REQUIRE(set.Save(FixedId(FreshId), lock, EditorSaveOptions{}));
    }

    REQUIRE(index.Rebuild(platform::PhysicalFileSystem(), PackageReadOptions{}));
    CHECK(index.Count() == 4 && index.Diagnostics().IsEmpty());
    EditorPackageSet set(env.allocator, config);
    REQUIRE(set.Initialize());
    auto hero = set.Open(FixedId(HeroId));
    REQUIRE(hero);
    TestAsset* asset = AssetOf(set, hero.Value());
    REQUIRE(asset != nullptr);
    CHECK(asset->value == EditedValue);
    Package* package = Pointer<Package>(set.Registry(), hero.Value());
    REQUIRE(package != nullptr);
    auto pixels = package->BulkBytes(asset->pixels);
    REQUIRE(pixels);
    CHECK(SameBytes(pixels.Value(), sources.pixels.View()));
    CHECK(set.Open(FixedId(FreshId)));
}

} // namespace editor_package_save_tests_file
