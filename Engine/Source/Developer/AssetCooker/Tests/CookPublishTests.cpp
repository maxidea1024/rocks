#include "../../../Runtime/AssetRuntime/Tests/AssetTestSupport.h"
#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <AssetCooker/CookPublish.h>
#include <Core/FileError.h>
#include <TestSupport/NFailAllocator.h>
#include <string.h>

// 쿠킹 게시·재사용 판정 시험이다(Docs/Design/AssetSystem.ko.md 10절 1–3, 10.2절 결정 8·10, "A4d
// 구현 세부" — A4d1): 전체 게시(staging → no-replace 이동·매니페스트 원자 교체·디렉터리 동기화
// 안쪽부터·
// `.staging` 고아 정리·가리키지 않는 패키지 정리·모양 밖 파일 유지), 같은 이름 파일(같은 내용은
// 쓰지 않음·손상은 CorruptEntry 뒤 교체 — 시나리오 27), 부분 병합(새 항목·재사용 항목의 byte 밖 칸
// 재계산·사라진 항목 빼기·남은 항목 유지·IncompatibleCook — 게시본 불변), 실패 시 게시본
// 불변(Finish 없음·매니페스트 검증 위반), 거부와 IO 고장(경고·오류), 재사용 판정(cookKey·파일
// 크기·SHA-256·머리 대조), 할당 실패.
using namespace rocks;

namespace cook_publish_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;

namespace {

constexpr StringView Profile = "Windows";
constexpr StringView CookedPrefix = "Saved/Cooked/Windows/";
// 합성 패키지 byte 길이의 기준이다(매니페스트 최소 크기 PackageHeaderBytes보다 길다).
constexpr usize FakeBytes = 96;

Sha256Digest DigestOf(const char* text) noexcept {
    return Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text)));
}

// 합성 쿠킹 패키지다(byte는 id·revision으로 만든 무늬 — 게시기는 내용을 해석하지 않는다).
struct FakePackage {
    FakePackage(Allocator& allocator, uint64 id, uint32 revision, StringView pathText) noexcept
        : bytes(allocator), path(allocator), dependencies(allocator), bundles(allocator) {
        asset = FixedId(id);
        ready = bytes.TryResize(FakeBytes + id % 7) && path.TryAssign(pathText);
        for (usize index = 0; index < bytes.Size(); ++index) {
            bytes[index] = static_cast<uint8>(id * 31 + revision * 7 + index);
        }

        sha256 = Sha256::Hash(bytes.View());
        cookKey = Sha256::Hash(Span<const uint8>(bytes.Data(), 8));
    }

    CookedPackage View() const noexcept {
        CookedPackage package;
        package.asset = asset;
        package.type = TestAssetType();
        package.pathHint = path.View();
        package.flags = flags;
        package.dependencies = dependencies.View();
        package.bundles = bundles.View();
        package.bytes = bytes.View();
        package.sha256 = sha256;
        package.bulkRawBytes = 3;
        package.cookKey = cookKey;
        return package;
    }

    AssetId asset{};
    Array<uint8> bytes;
    String path;
    Array<CookedManifestDependency> dependencies;
    Array<CookTargetBundle> bundles;
    Sha256Digest sha256{};
    Sha256Digest cookKey{};
    uint32 flags = 0;
    bool ready = false;
};

// 게시 장면이다: 디스크 임시 루트(잠금)와 그 루트의 메모리 파일 시스템.
struct PublishScene {
    PublishScene(AssetEnv& sceneEnv, const char* tag) noexcept
        : env(sceneEnv), root(tag), layout(env.allocator),
          files(env.allocator, Initialized(layout, root)) {
        ready = root.made && files.Ready() && static_cast<bool>(lock.Acquire(layout, 0));
    }

    static StringView Initialized(ProjectLayout& layout, const TempProject& root) noexcept {
        return layout.Initialize(root.Root()) ? layout.Root() : root.Root();
    }

    CookPublishConfig Config() noexcept {
        CookPublishConfig config;
        config.files = &files;
        config.layout = &layout;
        config.profileId = Profile;
        config.profileSettings = DigestOf("profile");
        config.cookSettings = DigestOf("settings");
        config.cookedBy = TestEngineVersion;
        return config;
    }

    // 쿠킹 루트 기준 상대 경로(rest)를 메모리 파일 시스템 상대 이름으로 만든다.
    String Relative(StringView rest) noexcept {
        String name(env.allocator);
        CHECK(name.TryAssign(CookedPrefix) && name.TryAppend(rest));
        return name;
    }

    String PackageName(const FakePackage& package) noexcept {
        StringBuilder<256> path(env.allocator);
        CHECK(AppendCookedPackagePath(path, package.asset, package.sha256));
        return Relative(path.View());
    }

    // relative 파일을 부모 디렉터리와 함께 둔다.
    bool Place(StringView relative, Span<const uint8> bytes) noexcept {
        for (usize at = 0; at < relative.ByteCount(); ++at) {
            if (relative.Data()[at] == '/') {
                const StringView parent(relative.Data(), at);
                if (!files.Has(parent) && !files.AddDirectory(parent)) {
                    return false;
                }
            }
        }

        files.Remove(relative);
        return files.AddFile(relative, bytes);
    }

    Result<CookedManifest> Manifest() noexcept {
        const String name = Relative(CookedManifestFileName);
        return ReadCookedManifest(env.allocator, files.Bytes(name.View()));
    }

    AssetEnv& env;
    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetDatabaseLock lock;
    bool ready = false;
};

bool HasWarning(const CookPublisher& publisher, AssetDiagnosticCode code) noexcept {
    for (const CookDiagnostic& diagnostic : publisher.Diagnostics()) {
        if (diagnostic.warning && diagnostic.code == code) {
            return true;
        }
    }

    return false;
}

// 같은 byte인지 본다.
bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// package를 PrimaryAsset으로 하고 member를 Soft 의존·bundle Ui 구성원으로 둔다(bundle 구성원은 그
// 항목의 의존이어야 한다 — AS 10.1절 5).
bool AddBundle(Allocator& allocator, FakePackage& package, AssetId member) noexcept {
    CookTargetBundle bundle(allocator);
    package.flags = static_cast<uint32>(CookedManifestFlags::PrimaryAsset);
    return package.dependencies.TryPushBack(
               CookedManifestDependency{member, PackageDependencyKind::Soft}) &&
           bundle.name.TryAssign("Ui") && bundle.members.TryPushBack(member) &&
           package.bundles.TryPushBack(Move(bundle));
}

} // namespace

// 전체 게시: 패키지는 내용 해시 이름으로 옮겨지고 매니페스트(머리·AssetId 순 항목·칸·bundle)가 원자
// 교체된다. `.staging`의 고아는 Begin이 지우고, 게시 뒤 새 매니페스트가 가리키지 않는
// `Packages/<a>/` 파일은 지운다(모양 밖 파일·링크는 둔다). 바뀐 디렉터리는 동기화한다. 임시 파일은
// 남지 않는다.
TEST(CookPublish_FullCookAndCleanup) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PublishScene scene(env, "publish-full");
    REQUIRE(scene.ready);
    const uint8 junk[] = {1, 2, 3};
    const String orphan = scene.Relative(".staging/left.tmp");
    const String stale = scene.Relative("Packages/zz/Old-0.rpkg");
    const String stray = scene.Relative("Packages/readme.txt");
    const String link = scene.Relative("Packages/zz/Link.rpkg");
    REQUIRE(scene.Place(orphan.View(), junk) && scene.Place(stale.View(), junk) &&
            scene.Place(stray.View(), junk));
    REQUIRE(
        scene.files.AddFile(link.View(), junk, MemoryFault::None, platform::FileType::File, true));

    FakePackage a(env.allocator, 30, 1, "/Game/A");
    FakePackage b(env.allocator, 10, 1, "/Game/B");
    FakePackage c(env.allocator, 20, 1, "/Game/C");
    REQUIRE(a.ready && b.ready && c.ready);
    REQUIRE(
        a.dependencies.TryPushBack(CookedManifestDependency{b.asset, PackageDependencyKind::Hard}));
    REQUIRE(a.dependencies.TryPushBack(
        CookedManifestDependency{FixedId(999), PackageDependencyKind::Soft}));
    REQUIRE(AddBundle(env.allocator, c, b.asset));
    CookPublisher publisher(env.allocator, scene.Config());
    REQUIRE(publisher.Begin(scene.lock));
    CHECK(!scene.files.Has(orphan.View()));
    StringBuilder<256> expectedRoot(env.allocator);
    REQUIRE(AppendCookedRoot(expectedRoot, scene.layout, Profile));
    CHECK(publisher.Root() == expectedRoot.View());
    REQUIRE(publisher.Put(a.View()) && publisher.Put(b.View()) && publisher.Put(c.View()));
    const uint32 syncsBefore = scene.files.syncs;
    REQUIRE(publisher.Finish(nullptr, {}));
    CHECK(publisher.Published() && !publisher.HasErrors() && publisher.Diagnostics().IsEmpty());
    CHECK(publisher.BytesWritten() == a.bytes.Size() + b.bytes.Size() + c.bytes.Size());
    CHECK(scene.files.syncs > syncsBefore);

    auto manifest = scene.Manifest();
    REQUIRE(manifest);
    CHECK(manifest.Value().profileId.View() == Profile &&
          manifest.Value().profileSettings == DigestOf("profile") &&
          manifest.Value().cookSettings == DigestOf("settings") &&
          manifest.Value().cookerVersion == CookerVersion);
    REQUIRE(manifest.Value().entries.Size() == 3);
    CHECK(manifest.Value().entries[0].asset == b.asset &&
          manifest.Value().entries[1].asset == c.asset &&
          manifest.Value().entries[2].asset == a.asset);
    const CookedManifestEntry* aEntry = manifest.Value().Find(a.asset);
    REQUIRE(aEntry != nullptr);
    CHECK(aEntry->pathHint.View() == "/Game/A" && aEntry->packageSha256 == a.sha256 &&
          aEntry->packageBytes == a.bytes.Size() && aEntry->bulkRawBytes == 3 &&
          aEntry->cookKey == a.cookKey && aEntry->dependencies.Size() == 2);
    const FakePackage* all[] = {&a, &b, &c};
    for (const FakePackage* package : all) {
        CHECK(Same(scene.files.Bytes(scene.PackageName(*package).View()), package->bytes.View()));
    }

    CHECK(!scene.files.Has(stale.View()) && scene.files.Has(stray.View()) &&
          scene.files.Has(link.View()));
    const CookedManifestEntry* cEntry = manifest.Value().Find(c.asset);
    REQUIRE(cEntry != nullptr);
    CHECK(cEntry->flags == static_cast<uint32>(CookedManifestFlags::PrimaryAsset) &&
          cEntry->bundles.Size() == 1 && cEntry->bundles[0].name.View() == "Ui");
    for (usize index = 0; index < scene.files.NodeCount(); ++index) {
        CHECK(!scene.files.NodeName(index).EndsWith(".tmp", StringComparison::CaseSensitive));
    }
}

// 첫 쿠킹(쿠킹 루트까지 없음): 새로 만든 디렉터리의 부모(프로젝트 루트·Saved·Saved/Cooked·쿠킹
// 루트)와 shard·Packages가 안쪽부터 동기화되고, 모두 매니페스트 교체보다 먼저다(10절 게시 2). 두
// 번째 쿠킹은 이미 있는 상위 디렉터리를 동기화하지 않는다. 첫 쿠킹의 모든 할당 지점은
// OutOfMemory다.
TEST(CookPublish_FirstCookSyncOrder) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PublishScene scene(env, "publish-first");
    REQUIRE(scene.ready);
    REQUIRE(!scene.files.Has("Saved"));
    FakePackage a(env.allocator, 10, 1, "/Game/A");
    REQUIRE(a.ready);
    scene.files.recordJournal = true;
    {
        CookPublisher publisher(env.allocator, scene.Config());
        REQUIRE(publisher.Begin(scene.lock) && publisher.Put(a.View()) &&
                publisher.Finish(nullptr, {}));
    }

    const auto position = [&](StringView line) noexcept {
        for (usize index = 0; index < scene.files.journal.Size(); ++index) {
            if (scene.files.journal[index].View() == line) {
                return index;
            }
        }

        return scene.files.journal.Size();
    };
    const usize replaced = position("replace Saved/Cooked/Windows/CookedManifest.rman");
    REQUIRE(replaced < scene.files.journal.Size());
    const StringView synced[] = {"sync ", "sync Saved", "sync Saved/Cooked",
                                 "sync Saved/Cooked/Windows", "sync Saved/Cooked/Windows/Packages"};
    for (StringView line : synced) {
        CHECK(position(line) < replaced);
    }

    // 안쪽부터: Packages가 루트보다, 루트가 Saved보다 먼저다.
    CHECK(position("sync Saved/Cooked/Windows/Packages") < position("sync Saved/Cooked/Windows"));
    CHECK(position("sync Saved/Cooked/Windows") < position("sync Saved"));

    scene.files.journal.Clear();
    FakePackage b(env.allocator, 20, 1, "/Game/B");
    REQUIRE(b.ready);
    {
        CookPublisher publisher(env.allocator, scene.Config());
        REQUIRE(publisher.Begin(scene.lock) && publisher.Put(a.View()) && publisher.Put(b.View()) &&
                publisher.Finish(nullptr, {}));
    }

    CHECK(position("sync Saved") == scene.files.journal.Size() &&
          position("sync Saved/Cooked/Windows/Packages") < scene.files.journal.Size());

    // 첫 쿠킹의 할당 실패: 모든 지점 OutOfMemory(시도마다 루트가 없는 새 장면).
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        PublishScene fresh(env, "publish-first-oom");
        REQUIRE(fresh.ready);
        CookPublisher publisher(failing, fresh.Config());
        failing.FailOnAttempt(attempt);
        Result<void> step = publisher.Begin(fresh.lock);
        if (step) {
            step = publisher.Put(a.View());
        }

        if (step) {
            step = publisher.Finish(nullptr, {});
        }

        const bool hit = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!hit) {
            REQUIRE(step && publisher.Published());
            break;
        }

        CHECK(step.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures >= 10);
}

// 같은 이름 파일: 같은 내용이면 쓰지 않고(쓴 byte에 넣지 않음, 경고 없음), 손상됐으면 경고
// CorruptEntry 뒤 새 byte로 교체한다(시나리오 27). 크기는 같고 내용만 다른 손상·크기가 다른
// 손상·읽을 수 없는 파일(error 칸에 읽기 오류)을 모두 잡는다.
TEST(CookPublish_ExistingFiles) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PublishScene scene(env, "publish-existing");
    REQUIRE(scene.ready);
    FakePackage same(env.allocator, 10, 1, "/Game/Same");
    FakePackage broken(env.allocator, 20, 1, "/Game/Broken");
    REQUIRE(same.ready && broken.ready);
    Array<uint8> damaged(env.allocator);
    REQUIRE(damaged.TryAppend(broken.bytes.View()));
    damaged[5] ^= 0xFF;
    FakePackage shorter(env.allocator, 30, 1, "/Game/Shorter");
    FakePackage unreadable(env.allocator, 40, 1, "/Game/Unreadable");
    REQUIRE(shorter.ready && unreadable.ready);
    REQUIRE(scene.Place(scene.PackageName(same).View(), same.bytes.View()));
    REQUIRE(scene.Place(scene.PackageName(broken).View(), damaged.View()));
    REQUIRE(scene.Place(scene.PackageName(shorter).View(),
                        Span<const uint8>(shorter.bytes.Data(), shorter.bytes.Size() - 1)));
    const String unreadableName = scene.PackageName(unreadable);
    REQUIRE(scene.Place(unreadableName.View(), unreadable.bytes.View()));
    scene.files.Remove(unreadableName.View());
    REQUIRE(scene.files.AddFile(unreadableName.View(), unreadable.bytes.View(), MemoryFault::Read));
    CookPublisher publisher(env.allocator, scene.Config());
    REQUIRE(publisher.Begin(scene.lock));
    REQUIRE(publisher.Put(same.View()) && publisher.Put(broken.View()) &&
            publisher.Put(shorter.View()) && publisher.Put(unreadable.View()));
    REQUIRE(publisher.Finish(nullptr, {}));
    CHECK(publisher.BytesWritten() ==
          broken.bytes.Size() + shorter.bytes.Size() + unreadable.bytes.Size());
    CHECK(HasWarning(publisher, AssetDiagnosticCode::CorruptEntry) && !publisher.HasErrors());
    REQUIRE(publisher.Diagnostics().Size() == 3);
    CHECK(publisher.Diagnostics()[0].asset == broken.asset &&
          publisher.Diagnostics()[0].error == ErrorCode::None);
    CHECK(publisher.Diagnostics()[2].asset == unreadable.asset &&
          publisher.Diagnostics()[2].error == FileErrorCode(MemoryError));
    CHECK(Same(scene.files.Bytes(scene.PackageName(shorter).View()), shorter.bytes.View()));
    CHECK(Same(scene.files.Bytes(scene.PackageName(broken).View()), broken.bytes.View()));
    CHECK(Same(scene.files.Bytes(scene.PackageName(same).View()), same.bytes.View()));
}

// 부분 병합: 새 항목(B 다시 쿠킹)·재사용 항목(A — byte 칸은 기존, 경로 힌트·flags·bundle은 이번
// 판정)·사라진 항목(C) 빼기·남은 항목(D) 유지. 부분 쿠킹은 가리키지 않는 파일을 지우지 않는다(옛 B
// 파일이 남는다). CookSettings가 다르면 IncompatibleCook 오류로 게시하지 않고 게시본은 그대로다.
TEST(CookPublish_PartialMerge) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PublishScene scene(env, "publish-partial");
    REQUIRE(scene.ready);
    FakePackage a(env.allocator, 10, 1, "/Game/A");
    FakePackage b(env.allocator, 20, 1, "/Game/B");
    FakePackage c(env.allocator, 30, 1, "/Game/C");
    FakePackage d(env.allocator, 40, 1, "/Game/D");
    REQUIRE(a.ready && b.ready && c.ready && d.ready);
    // bundle 구성원은 그 항목의 의존이어야 한다(AS 10.1절 5 — AssetBundles는 Soft 참조 필드다).
    REQUIRE(
        a.dependencies.TryPushBack(CookedManifestDependency{d.asset, PackageDependencyKind::Soft}));
    {
        CookPublisher first(env.allocator, scene.Config());
        REQUIRE(first.Begin(scene.lock));
        REQUIRE(first.Put(a.View()) && first.Put(b.View()) && first.Put(c.View()) &&
                first.Put(d.View()));
        REQUIRE(first.Finish(nullptr, {}));
    }

    auto existing = scene.Manifest();
    REQUIRE(existing && existing.Value().entries.Size() == 4);
    const String oldB = scene.PackageName(b);
    FakePackage b2(env.allocator, 20, 2, "/Game/B");
    REQUIRE(b2.ready);
    CookTarget aTarget(env.allocator);
    aTarget.id = a.asset;
    aTarget.primary = true;
    CookTargetBundle bundle(env.allocator);
    REQUIRE(bundle.name.TryAssign("Ui") && bundle.members.TryPushBack(d.asset));
    REQUIRE(aTarget.bundles.TryPushBack(Move(bundle)));
    const AssetId removed[] = {c.asset};
    CookPublisher partial(env.allocator, scene.Config());
    REQUIRE(partial.Begin(scene.lock));
    REQUIRE(partial.Put(b2.View()));
    REQUIRE(partial.Reuse(*existing.Value().Find(a.asset), aTarget, "/Game/Moved/A"));
    REQUIRE(partial.Finish(&existing.Value(), removed));
    CHECK(partial.Published() && !partial.HasErrors());
    auto merged = scene.Manifest();
    REQUIRE(merged);
    REQUIRE(merged.Value().entries.Size() == 3);
    const CookedManifestEntry* aEntry = merged.Value().Find(a.asset);
    const CookedManifestEntry* bEntry = merged.Value().Find(b.asset);
    REQUIRE(aEntry != nullptr && bEntry != nullptr && merged.Value().Find(d.asset) != nullptr);
    CHECK(merged.Value().Find(c.asset) == nullptr);
    CHECK(aEntry->pathHint.View() == "/Game/Moved/A" &&
          aEntry->flags == static_cast<uint32>(CookedManifestFlags::PrimaryAsset) &&
          aEntry->bundles.Size() == 1 && aEntry->bundles[0].members.Size() == 1 &&
          aEntry->packageSha256 == a.sha256 && aEntry->cookKey == a.cookKey);
    CHECK(bEntry->packageSha256 == b2.sha256);
    // PrimaryAsset이 아닌 재사용 대상은 flags 0·bundle 없음이다(이번 판정).
    CookTarget plain(env.allocator);
    plain.id = d.asset;
    CookPublisher again(env.allocator, scene.Config());
    REQUIRE(again.Begin(scene.lock));
    REQUIRE(again.Reuse(*merged.Value().Find(d.asset), plain, "/Game/D"));
    REQUIRE(again.Finish(&merged.Value(), {}));
    auto plainMerged = scene.Manifest();
    REQUIRE(plainMerged && plainMerged.Value().Find(d.asset) != nullptr);
    CHECK(plainMerged.Value().Find(d.asset)->flags == 0 &&
          plainMerged.Value().Find(d.asset)->bundles.IsEmpty());
    CHECK(scene.files.Has(oldB.View()) && scene.files.Has(scene.PackageName(c).View()));

    // 다른 CookSettings: IncompatibleCook 오류, 게시하지 않음(매니페스트 byte 그대로).
    merged = scene.Manifest();
    REQUIRE(merged);
    const String manifestName = scene.Relative(CookedManifestFileName);
    Array<uint8> before(env.allocator);
    REQUIRE(before.TryAppend(scene.files.Bytes(manifestName.View())));
    CookPublishConfig other = scene.Config();
    other.cookSettings = DigestOf("other");
    CookPublisher incompatible(env.allocator, other);
    REQUIRE(incompatible.Begin(scene.lock));
    REQUIRE(incompatible.Put(b.View()));
    REQUIRE(incompatible.Finish(&merged.Value(), {}));
    CHECK(!incompatible.Published() && incompatible.HasErrors());
    REQUIRE(incompatible.Diagnostics().Size() == 1);
    CHECK(incompatible.Diagnostics()[0].code == AssetDiagnosticCode::IncompatibleCook);
    CHECK(Same(scene.files.Bytes(manifestName.View()), before.View()));
}

// 실패 시 게시본 불변: Finish 없이 끝나면 매니페스트는 그대로이고 옮긴 새 패키지는 기존 파일을 덮지
// 않는다. 매니페스트 검증 위반(Hard 대상이 매니페스트에 없음)은 그 진단 코드로 오류를 남기고
// 게시하지 않는다.
TEST(CookPublish_FailureKeepsPublished) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PublishScene scene(env, "publish-failure");
    REQUIRE(scene.ready);
    FakePackage a(env.allocator, 10, 1, "/Game/A");
    REQUIRE(a.ready);
    {
        CookPublisher first(env.allocator, scene.Config());
        REQUIRE(first.Begin(scene.lock) && first.Put(a.View()) && first.Finish(nullptr, {}));
    }

    const String manifestName = scene.Relative(CookedManifestFileName);
    Array<uint8> before(env.allocator);
    REQUIRE(before.TryAppend(scene.files.Bytes(manifestName.View())));
    FakePackage a2(env.allocator, 10, 2, "/Game/A");
    REQUIRE(a2.ready);
    {
        CookPublisher abandoned(env.allocator, scene.Config());
        REQUIRE(abandoned.Begin(scene.lock) && abandoned.Put(a2.View()));
    }

    CHECK(Same(scene.files.Bytes(manifestName.View()), before.View()));
    CHECK(Same(scene.files.Bytes(scene.PackageName(a).View()), a.bytes.View()));

    FakePackage dangling(env.allocator, 20, 1, "/Game/Dangling");
    REQUIRE(dangling.ready);
    REQUIRE(dangling.dependencies.TryPushBack(
        CookedManifestDependency{FixedId(777), PackageDependencyKind::Hard}));
    CookPublisher invalid(env.allocator, scene.Config());
    REQUIRE(invalid.Begin(scene.lock) && invalid.Put(dangling.View()));
    REQUIRE(invalid.Finish(nullptr, {}));
    CHECK(!invalid.Published() && invalid.HasErrors());
    REQUIRE(invalid.Diagnostics().Size() == 1);
    CHECK(invalid.Diagnostics()[0].code == AssetDiagnosticCode::CorruptPackage &&
          invalid.Diagnostics()[0].asset == dangling.asset &&
          invalid.Diagnostics()[0].related == FixedId(777));
    CHECK(Same(scene.files.Bytes(manifestName.View()), before.View()));
}

// 거부와 IO: 설정 null·잠금 아님·두 번째 Begin·Begin 전 Put·Reuse·Finish·두 번째 Finish는
// InvalidArgument. IO 고장: 디렉터리 만들기·임시 파일 만들기·옮기기(AlreadyExists 밖)·손상
// 교체·동기화 (NotDurable은 경고)·매니페스트 교체 실패는 오류, 고아·정리 삭제 실패는 경고, 열거
// 실패는 오류.
TEST(CookPublish_RejectsAndFaults) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PublishScene scene(env, "publish-rejects");
    REQUIRE(scene.ready);
    FakePackage a(env.allocator, 10, 1, "/Game/A");
    REQUIRE(a.ready);
    CookPublishConfig nullFiles = scene.Config();
    nullFiles.files = nullptr;
    CookPublisher noFiles(env.allocator, nullFiles);
    CHECK(noFiles.Begin(scene.lock).Error() == ErrorCode::InvalidArgument);
    CookPublishConfig nullLayout = scene.Config();
    nullLayout.layout = nullptr;
    CookPublisher noLayout(env.allocator, nullLayout);
    CHECK(noLayout.Begin(scene.lock).Error() == ErrorCode::InvalidArgument);
    AssetDatabaseLock unheld;
    CookPublisher notLocked(env.allocator, scene.Config());
    CHECK(notLocked.Begin(unheld).Error() == ErrorCode::InvalidArgument);
    CookTarget target(env.allocator);
    CHECK(notLocked.Put(a.View()).Error() == ErrorCode::InvalidArgument);
    CHECK(notLocked.Finish(nullptr, {}).Error() == ErrorCode::InvalidArgument);
    CookedManifestEntry entry(env.allocator);
    CHECK(notLocked.Reuse(entry, target, "/Game/A").Error() == ErrorCode::InvalidArgument);
    {
        CookPublisher twice(env.allocator, scene.Config());
        REQUIRE(twice.Begin(scene.lock));
        CHECK(twice.Begin(scene.lock).Error() == ErrorCode::InvalidArgument);
        REQUIRE(twice.Put(a.View()) && twice.Finish(nullptr, {}));
        CHECK(twice.Finish(nullptr, {}).Error() == ErrorCode::InvalidArgument);
        CHECK(twice.Put(a.View()).Error() == ErrorCode::InvalidArgument);
        CHECK(twice.Reuse(entry, target, "/Game/A").Error() == ErrorCode::InvalidArgument);
    }

    const ErrorCode systemError = FileErrorCode(MemoryError);
    const auto fresh = [&](uint32 revision, const char* name) noexcept {
        return FakePackage(env.allocator, 50 + revision, revision, name);
    };

    // 디렉터리 확인(Stat) 실패는 오류다(Begin).
    {
        CookPublisher publisher(env.allocator, scene.Config());
        scene.files.faults.stat = MemoryError;
        CHECK(publisher.Begin(scene.lock).Error() == systemError);
        scene.files.faults.stat = {};
    }

    // 디렉터리 만들기 실패(Begin) — 쿠킹 루트를 지워 다시 만들게 한다.
    {
        PublishScene clean(env, "publish-mkdir");
        REQUIRE(clean.ready);
        clean.files.faults.makeDirectory = MemoryError;
        CookPublisher publisher(env.allocator, clean.Config());
        CHECK(publisher.Begin(clean.lock).Error() == systemError);
    }

    // 임시 파일 만들기 실패.
    {
        FakePackage package = fresh(1, "/Game/Create");
        REQUIRE(package.ready);
        CookPublisher publisher(env.allocator, scene.Config());
        REQUIRE(publisher.Begin(scene.lock));
        scene.files.faults.create = MemoryError;
        CHECK(publisher.Put(package.View()).Error() == systemError);
        scene.files.faults.create = {};
    }

    // 쓰기·Flush·Close 실패와 패키지 디렉터리 만들기 실패 — 임시 파일은 지운다.
    for (int fault = 0; fault < 4; ++fault) {
        FakePackage package = fresh(10 + static_cast<uint32>(fault), "/Game/Write");
        REQUIRE(package.ready);
        if (fault == 3) {
            // 아직 없는 shard 디렉터리가 생기게 high가 다른 id로 둔다(FixedId는 shard를 공유한다).
            package.asset = AssetId{Guid{0x7a00000000000000ull, 3}};
        }

        CookPublisher publisher(env.allocator, scene.Config());
        REQUIRE(publisher.Begin(scene.lock));
        scene.files.faults.failWriteCall = fault == 0 ? scene.files.WriteCalls() + 1 : 0;
        scene.files.faults.flush = fault == 1 ? MemoryError : platform::FileStatus{};
        scene.files.faults.close = fault == 2 ? MemoryError : platform::FileStatus{};
        scene.files.faults.makeDirectory = fault == 3 ? MemoryError : platform::FileStatus{};
        CHECK(!publisher.Put(package.View()));
        scene.files.faults = MemoryFaults{};
        for (usize index = 0; index < scene.files.NodeCount(); ++index) {
            CHECK(!scene.files.NodeName(index).EndsWith(".tmp", StringComparison::CaseSensitive));
        }
    }

    // 옮기기 실패(AlreadyExists 밖) — 임시 파일은 지운다.
    {
        FakePackage package = fresh(2, "/Game/Move");
        REQUIRE(package.ready);
        CookPublisher publisher(env.allocator, scene.Config());
        REQUIRE(publisher.Begin(scene.lock));
        scene.files.faults.move = MemoryError;
        CHECK(publisher.Put(package.View()).Error() == systemError);
        scene.files.faults.move = {};
        for (usize index = 0; index < scene.files.NodeCount(); ++index) {
            CHECK(!scene.files.NodeName(index).EndsWith(".tmp", StringComparison::CaseSensitive));
        }
    }

    // 손상 교체 실패(오류)와 NotDurable 교체(성공).
    {
        FakePackage package = fresh(3, "/Game/Replace");
        REQUIRE(package.ready);
        const uint8 junk[] = {9, 9, 9};
        REQUIRE(scene.Place(scene.PackageName(package).View(), junk));
        CookPublisher publisher(env.allocator, scene.Config());
        REQUIRE(publisher.Begin(scene.lock));
        scene.files.faults.replace = MemoryError;
        CHECK(publisher.Put(package.View()).Error() == systemError);
        scene.files.faults.replace = MemoryNotDurable;
        REQUIRE(publisher.Put(package.View()));
        scene.files.faults.replace = {};
        CHECK(HasWarning(publisher, AssetDiagnosticCode::CorruptEntry));
    }

    // 동기화: NotDurable은 경고, 그 밖은 오류.
    {
        FakePackage package = fresh(4, "/Game/Sync");
        REQUIRE(package.ready);
        CookPublisher durable(env.allocator, scene.Config());
        REQUIRE(durable.Begin(scene.lock) && durable.Put(package.View()));
        scene.files.faults.sync = MemoryNotDurable;
        REQUIRE(durable.Finish(nullptr, {}));
        CHECK(HasWarning(durable, AssetDiagnosticCode::NotDurable) && durable.Published());
        CookPublisher failing(env.allocator, scene.Config());
        REQUIRE(failing.Begin(scene.lock) && failing.Put(package.View()));
        scene.files.faults.sync = MemoryError;
        CHECK(failing.Finish(nullptr, {}).Error() == systemError);
        scene.files.faults.sync = {};
    }

    // 매니페스트 교체: NotDurable은 경고, 그 밖은 오류.
    {
        FakePackage package = fresh(5, "/Game/Manifest");
        REQUIRE(package.ready);
        CookPublisher notDurable(env.allocator, scene.Config());
        REQUIRE(notDurable.Begin(scene.lock) && notDurable.Put(package.View()));
        scene.files.faults.replace = MemoryNotDurable;
        REQUIRE(notDurable.Finish(nullptr, {}));
        CHECK(HasWarning(notDurable, AssetDiagnosticCode::NotDurable));
        CookPublisher failing(env.allocator, scene.Config());
        REQUIRE(failing.Begin(scene.lock) && failing.Put(package.View()));
        scene.files.faults.replace = MemoryError;
        CHECK(failing.Finish(nullptr, {}).Error() == systemError);
        scene.files.faults.replace = {};
    }

    // 삭제 실패(고아·정리)는 경고다.
    {
        const uint8 junk[] = {1};
        const String orphan = scene.Relative(".staging/orphan.tmp");
        const String stale = scene.Relative("Packages/zz/Stale.rpkg");
        REQUIRE(scene.Place(orphan.View(), junk) && scene.Place(stale.View(), junk));
        FakePackage package = fresh(6, "/Game/Delete");
        REQUIRE(package.ready);
        scene.files.faults.remove = MemoryError;
        CookPublisher publisher(env.allocator, scene.Config());
        REQUIRE(publisher.Begin(scene.lock) && publisher.Put(package.View()));
        REQUIRE(publisher.Finish(nullptr, {}));
        scene.files.faults.remove = {};
        usize warnings = 0;
        for (const CookDiagnostic& diagnostic : publisher.Diagnostics()) {
            warnings += diagnostic.warning && diagnostic.code == AssetDiagnosticCode::None &&
                                diagnostic.error == systemError
                            ? 1
                            : 0;
        }

        CHECK(warnings >= 2 && publisher.Published());
        scene.files.Remove(orphan.View());
        scene.files.Remove(stale.View());
    }

    // 열거 실패: `.staging`(Begin)·`Packages`(정리)·`Packages/<a>`(정리)는 오류다.
    const char* const listed[] = {".staging", "Packages", "Packages/zz"};
    for (const char* directory : listed) {
        const String name = scene.Relative(directory);
        const uint8 junk[] = {1};
        const String keep = scene.Relative("Packages/zz/Keep.rpkg");
        REQUIRE(scene.Place(keep.View(), junk));
        scene.files.Remove(name.View());
        REQUIRE(scene.files.AddDirectory(name.View(), false, MemoryFault::Enumerate));
        FakePackage package = fresh(7, "/Game/List");
        REQUIRE(package.ready);
        CookPublisher publisher(env.allocator, scene.Config());
        auto begun = publisher.Begin(scene.lock);
        if (StringView(directory) == ".staging") {
            CHECK(begun.Error() == systemError);
        } else {
            REQUIRE(begun && publisher.Put(package.View()));
            CHECK(publisher.Finish(nullptr, {}).Error() == systemError);
        }

        scene.files.Remove(name.View());
        REQUIRE(scene.files.AddDirectory(name.View()));
    }
}

// 재사용 판정: 머리(프로파일 id·출력 설정·쿠커 버전)가 같고 cookKey가 같으며 파일 크기·SHA-256이
// 같을 때만 재사용한다. 파일 없음·손상·크기만 다름·cookKey 다름·키 없음·매니페스트에 없음은 다시
// 쿠킹, 머리가 다르거나 기존 매니페스트가 없으면 아무것도 재사용하지 않는다. 인자 null·키 수 다름은
// InvalidArgument.
TEST(CookPublish_PlanReuse) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PublishScene scene(env, "publish-reuse");
    REQUIRE(scene.ready);
    FakePackage packages[] = {
        FakePackage(env.allocator, 10, 1, "/Game/A"), FakePackage(env.allocator, 20, 1, "/Game/B"),
        FakePackage(env.allocator, 30, 1, "/Game/C"), FakePackage(env.allocator, 40, 1, "/Game/D"),
        FakePackage(env.allocator, 50, 1, "/Game/E"), FakePackage(env.allocator, 60, 1, "/Game/F")};
    {
        CookPublisher first(env.allocator, scene.Config());
        REQUIRE(first.Begin(scene.lock));
        for (const FakePackage& package : packages) {
            REQUIRE(package.ready && first.Put(package.View()));
        }

        REQUIRE(first.Finish(nullptr, {}));
    }

    auto existing = scene.Manifest();
    REQUIRE(existing);
    // B: 파일 삭제, C: 손상, D: cookKey 다름, E: 키 없음, F: 크기만 다름, G: 매니페스트에 없음.
    REQUIRE(scene.Place(scene.PackageName(packages[5]).View(),
                        Span<const uint8>(packages[5].bytes.Data(), packages[5].bytes.Size() - 1)));
    scene.files.Remove(scene.PackageName(packages[1]).View());
    Array<uint8> damaged(env.allocator);
    REQUIRE(damaged.TryAppend(packages[2].bytes.View()));
    damaged[0] ^= 1;
    REQUIRE(scene.Place(scene.PackageName(packages[2]).View(), damaged.View()));
    CookTargetSet targets(env.allocator);
    CookKeySet keys(env.allocator);
    for (const FakePackage& package : packages) {
        REQUIRE(targets.targets.TryEmplaceBack(env.allocator));
        targets.targets[targets.targets.Size() - 1].id = package.asset;
        CookKey key;
        key.asset = package.asset;
        key.cookKey = package.cookKey;
        key.hasKey = true;
        REQUIRE(keys.keys.TryPushBack(key));
    }

    keys.keys[3].cookKey = DigestOf("changed");
    keys.keys[4].hasKey = false;
    REQUIRE(targets.targets.TryEmplaceBack(env.allocator));
    targets.targets[targets.targets.Size() - 1].id = FixedId(70);
    CookKey unknown;
    unknown.asset = FixedId(70);
    unknown.hasKey = true;
    REQUIRE(keys.keys.TryPushBack(unknown));
    StringBuilder<256> root(env.allocator);
    REQUIRE(AppendCookedRoot(root, scene.layout, Profile));
    CookReuseRequest request;
    request.files = &scene.files;
    request.cookedRoot = root.View();
    request.existing = &existing.Value();
    request.targets = &targets;
    request.keys = &keys;
    request.profileId = Profile;
    request.profileSettings = DigestOf("profile");
    CookReusePlan plan(env.allocator);
    REQUIRE(PlanCookReuse(env.allocator, request, plan));
    REQUIRE(plan.reuse.Size() == 7 && plan.entries.Size() == 7);
    CHECK(plan.count == 1 && plan.reuse[0] == 1 &&
          plan.entries[0] == existing.Value().Find(packages[0].asset));
    for (usize index = 1; index < 7; ++index) {
        CHECK(plan.reuse[index] == 0 && plan.entries[index] == nullptr);
    }

    // 머리가 다르거나 기존 매니페스트가 없으면 재사용 없음.
    CookReuseRequest changed = request;
    changed.profileId = "Android";
    REQUIRE(PlanCookReuse(env.allocator, changed, plan));
    CHECK(plan.count == 0 && plan.reuse.Size() == 7);
    changed = request;
    changed.profileSettings = DigestOf("other");
    REQUIRE(PlanCookReuse(env.allocator, changed, plan));
    CHECK(plan.count == 0);
    changed = request;
    changed.cookerVersion = CookerVersion + 1;
    REQUIRE(PlanCookReuse(env.allocator, changed, plan));
    CHECK(plan.count == 0);
    changed = request;
    changed.existing = nullptr;
    REQUIRE(PlanCookReuse(env.allocator, changed, plan));
    CHECK(plan.count == 0);

    changed = request;
    changed.files = nullptr;
    CHECK(PlanCookReuse(env.allocator, changed, plan).Error() == ErrorCode::InvalidArgument);
    changed = request;
    changed.targets = nullptr;
    CHECK(PlanCookReuse(env.allocator, changed, plan).Error() == ErrorCode::InvalidArgument);
    changed = request;
    changed.keys = nullptr;
    CHECK(PlanCookReuse(env.allocator, changed, plan).Error() == ErrorCode::InvalidArgument);
    CookKeySet fewer(env.allocator);
    changed = request;
    changed.keys = &fewer;
    CHECK(PlanCookReuse(env.allocator, changed, plan).Error() == ErrorCode::InvalidArgument);

    // 할당 실패: 모든 지점 OutOfMemory, 다 지나면 같은 판정.
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        CookReusePlan failed(failing);
        failing.FailOnAttempt(attempt);
        auto planned = PlanCookReuse(failing, request, failed);
        const bool hit = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!hit) {
            REQUIRE(planned);
            CHECK(failed.count == 1);
            break;
        }

        CHECK(planned.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures >= 3);
}

// 할당 실패: 게시 흐름(Begin·Put·Reuse·Finish 전체·부분 병합·정리)의 모든 지점에서 OutOfMemory이고
// 게시본은 그대로이거나(실패) 기준과 같다(성공). 경고 다섯 자리의 push와 게시하지 않는 오류 진단의
// push도 OutOfMemory다.
TEST(CookPublish_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    PublishScene scene(env, "publish-oom");
    REQUIRE(scene.ready);
    FakePackage a(env.allocator, 10, 1, "/Game/A");
    FakePackage b(env.allocator, 20, 1, "/Game/B");
    FakePackage c(env.allocator, 30, 1, "/Game/C");
    REQUIRE(a.ready && b.ready && c.ready);
    REQUIRE(
        a.dependencies.TryPushBack(CookedManifestDependency{b.asset, PackageDependencyKind::Soft}));
    REQUIRE(AddBundle(env.allocator, c, a.asset));
    {
        CookPublisher first(env.allocator, scene.Config());
        REQUIRE(first.Begin(scene.lock) && first.Put(a.View()) && first.Put(b.View()) &&
                first.Put(c.View()) && first.Finish(nullptr, {}));
    }

    auto existing = scene.Manifest();
    REQUIRE(existing);
    const String manifestName = scene.Relative(CookedManifestFileName);
    Array<uint8> baseline(env.allocator);
    REQUIRE(baseline.TryAppend(scene.files.Bytes(manifestName.View())));
    FakePackage b2(env.allocator, 20, 2, "/Game/B");
    REQUIRE(b2.ready && AddBundle(env.allocator, b2, a.asset));
    CookTarget aTarget(env.allocator);
    aTarget.id = a.asset;
    aTarget.primary = true;
    CookTargetBundle bundle(env.allocator);
    REQUIRE(bundle.name.TryAssign("Ui") && bundle.members.TryPushBack(b.asset));
    REQUIRE(aTarget.bundles.TryPushBack(Move(bundle)));
    const uint8 junk[] = {1};
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        // 매번 같은 시작 상태(게시본 = 첫 게시, 정리할 파일 하나, `.staging` 고아 하나).
        REQUIRE(scene.Place(manifestName.View(), baseline.View()));
        REQUIRE(scene.Place(scene.Relative("Packages/zz/Stale.rpkg").View(), junk));
        REQUIRE(scene.Place(scene.Relative(".staging/orphan.tmp").View(), junk));
        CookPublisher publisher(failing, scene.Config());
        failing.FailOnAttempt(attempt);
        Result<void> step = publisher.Begin(scene.lock);
        if (step) {
            step = publisher.Put(b2.View());
        }

        if (step) {
            step = publisher.Reuse(*existing.Value().Find(a.asset), aTarget, "/Game/A");
        }

        if (step) {
            step = publisher.Finish(&existing.Value(), {});
        }

        const bool hit = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!hit) {
            REQUIRE(step && publisher.Published());
            break;
        }

        CHECK(step.Error() == ErrorCode::OutOfMemory);
        if (!publisher.Published()) {
            CHECK(Same(scene.files.Bytes(manifestName.View()), baseline.View()));
        }

        ++failures;
    }

    CHECK(failures >= 10);

    // 전체 쿠킹 정리 경로의 할당 실패.
    uint32 cleanupFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        REQUIRE(scene.Place(scene.Relative("Packages/zz/Stale.rpkg").View(), junk));
        CookPublisher publisher(failing, scene.Config());
        failing.FailOnAttempt(attempt);
        Result<void> step = publisher.Begin(scene.lock);
        if (step) {
            step = publisher.Put(a.View());
        }

        if (step) {
            step = publisher.Put(b.View());
        }

        if (step) {
            step = publisher.Finish(nullptr, {});
        }

        const bool hit = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!hit) {
            REQUIRE(step);
            CHECK(!scene.files.Has(scene.Relative("Packages/zz/Stale.rpkg").View()));
            break;
        }

        CHECK(step.Error() == ErrorCode::OutOfMemory);
        ++cleanupFailures;
    }

    CHECK(cleanupFailures >= 10);

    // 경고 push의 할당 실패도 OutOfMemory다(경고를 조용히 잃지 않는다). 경고 자리마다 그 경고가 첫
    // 진단인 장면: 0 고아 삭제 실패, 1 CorruptEntry, 2 동기화 NotDurable, 3 매니페스트 NotDurable,
    // 4 정리 삭제 실패.
    const String orphanName = scene.Relative(".staging/orphan.tmp");
    const String staleName = scene.Relative("Packages/zz/Stale.rpkg");
    FakePackage fresh(env.allocator, 60, 1, "/Game/Fresh");
    REQUIRE(fresh.ready);
    const String freshName = scene.PackageName(fresh);
    for (int warning = 0; warning < 5; ++warning) {
        uint32 warned = 0;
        for (uint64 attempt = 1;; ++attempt) {
            scene.files.faults = MemoryFaults{};
            scene.files.Remove(orphanName.View());
            scene.files.Remove(staleName.View());
            scene.files.Remove(freshName.View());
            if (warning == 0) {
                REQUIRE(scene.Place(orphanName.View(), junk));
                scene.files.faults.remove = MemoryError;
            } else if (warning == 1) {
                REQUIRE(scene.Place(freshName.View(), junk));
            } else if (warning == 2) {
                scene.files.faults.sync = MemoryNotDurable;
            } else if (warning == 3) {
                scene.files.faults.replace = MemoryNotDurable;
            } else {
                REQUIRE(scene.Place(staleName.View(), junk));
                scene.files.faults.remove = MemoryError;
            }

            CookPublisher publisher(failing, scene.Config());
            failing.FailOnAttempt(attempt);
            Result<void> step = publisher.Begin(scene.lock);
            if (step) {
                step = publisher.Put(fresh.View());
            }

            if (step) {
                step = publisher.Finish(nullptr, {});
            }

            const bool hit = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!hit) {
                REQUIRE(step);
                CHECK(publisher.Published() && !publisher.Diagnostics().IsEmpty() &&
                      publisher.Diagnostics()[0].warning);
                break;
            }

            CHECK(step.Error() == ErrorCode::OutOfMemory);
            ++warned;
        }

        CHECK(warned >= 3);
    }

    scene.files.faults = MemoryFaults{};

    // 게시하지 않는 오류 진단(IncompatibleCook·검증 위반)의 push도 OutOfMemory로 돌아온다.
    FakePackage dangling(env.allocator, 40, 1, "/Game/Dangling");
    REQUIRE(dangling.ready);
    REQUIRE(dangling.dependencies.TryPushBack(
        CookedManifestDependency{FixedId(777), PackageDependencyKind::Hard}));
    for (int scenario = 0; scenario < 2; ++scenario) {
        CookPublishConfig config = scene.Config();
        if (scenario == 0) {
            config.cookSettings = DigestOf("other");
        }

        uint32 refusals = 0;
        for (uint64 attempt = 1;; ++attempt) {
            CookPublisher publisher(failing, config);
            failing.FailOnAttempt(attempt);
            Result<void> step = publisher.Begin(scene.lock);
            if (step) {
                step = publisher.Put(scenario == 0 ? a.View() : dangling.View());
            }

            if (step) {
                step = publisher.Finish(scenario == 0 ? &existing.Value() : nullptr, {});
            }

            const bool hit = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!hit) {
                REQUIRE(step);
                CHECK(publisher.HasErrors() && !publisher.Published());
                break;
            }

            CHECK(step.Error() == ErrorCode::OutOfMemory);
            ++refusals;
        }

        CHECK(refusals >= 3);
    }
}

} // namespace cook_publish_tests_file
