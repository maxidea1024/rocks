#include "AssetIndexCompare.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetIndex.h>
#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/LogicalPath.h>
#include <Core/FileError.h>

// 색인 콘텐츠 해시 시험이다(Docs/Design/AssetSystem 10.2절 "A4b 구현 세부"): 처음 계산은 파일
// 전체 SHA-256(조각 경계를 넘는 큰 파일·빈 파일 포함)이고 Stat(크기·수정 시각)이 같으면 열지 않고
// 저장된 값을 준다. 크기만·시각만 달라도 다시 계산한다. 읽기 전후 Stat이 다르면(크기만·시각만)
// Busy이고 해시를 두지 않는다. 오류: 없는 id NotFound, 같은 id AlreadyExists, Stat·열기·읽기·두
// 번째 Stat 실패는 그 파일 오류, 할당 실패는 OutOfMemory(해시를 두지 않는다). 색인 캐시가 네 칸을
// 보존하고 캐시를 쓴 색인은 Stat 대조로만 그 값을 믿는다.
using namespace rocks;
namespace asset_index_content_hash_tests_file {

using namespace rocks::editor_set_test;

namespace {

// 조각(64 KiB) 여러 개와 끝 조각이 생기는 큰 파일 크기다.
constexpr usize LargeBytes = 3 * 64 * 1024 + 17;

// Stat 결과를 바꾸고 n번째 Stat을 고장 내는 전달 파일 시스템이다(읽기 전후 Stat 대조 시험).
class StatHookFileSystem final : public platform::FileSystem {
public:
    explicit StatHookFileSystem(MemoryFileSystem& memory) noexcept : inner(memory) {
    }

    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode openMode,
                              platform::FileHandle*& out) noexcept override {
        return inner.Open(path, pathBytes, openMode, out);
    }

    platform::FileStatus FileExists(const char* path, usize pathBytes,
                                    bool& exists) noexcept override {
        return inner.FileExists(path, pathBytes, exists);
    }

    platform::FileStatus Delete(const char* path, usize pathBytes) noexcept override {
        return inner.Delete(path, pathBytes);
    }

    platform::FileStatus Stat(const char* path, usize pathBytes,
                              platform::FileInfo& info) noexcept override {
        ++calls;
        if (calls == failCall) {
            return failStatus;
        }

        const platform::FileStatus status = inner.Stat(path, pathBytes, info);
        info.size += alwaysSize;
        if (calls == changeCall) {
            info.size += changeSize;
            info.modifiedTicks += changeTicks;
        }

        return status;
    }

    platform::FileStatus MakeDirectory(const char* path, usize pathBytes) noexcept override {
        return inner.MakeDirectory(path, pathBytes);
    }

    platform::FileStatus DeleteDirectory(const char* path, usize pathBytes) noexcept override {
        return inner.DeleteDirectory(path, pathBytes);
    }

    platform::FileStatus EnumerateDirectory(const char* path, usize pathBytes,
                                            platform::DirectoryVisitor visitor,
                                            void* visitorContext) noexcept override {
        return inner.EnumerateDirectory(path, pathBytes, visitor, visitorContext);
    }

    platform::FileStatus Move(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        return inner.Move(from, fromBytes, to, toBytes, replaceExisting);
    }

    platform::FileStatus Copy(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        return inner.Copy(from, fromBytes, to, toBytes, replaceExisting);
    }

    platform::FileStatus Replace(const char* from, usize fromBytes, const char* to,
                                 usize toBytes) noexcept override {
        return inner.Replace(from, fromBytes, to, toBytes);
    }

    platform::FileStatus SetModifiedTime(const char* path, usize pathBytes,
                                         int64 modifiedTicks) noexcept override {
        return inner.SetModifiedTime(path, pathBytes, modifiedTicks);
    }

    platform::FileStatus SyncDirectory(const char* path, usize pathBytes) noexcept override {
        return inner.SyncDirectory(path, pathBytes);
    }

    // 다음 시험 단계를 위해 호출 수와 주입을 지운다.
    void Reset() noexcept {
        calls = 0;
        failCall = 0;
        changeCall = 0;
        changeSize = 0;
        changeTicks = 0;
        alwaysSize = 0;
    }

    MemoryFileSystem& inner;
    uint32 calls = 0;
    // 이 번호(1부터)의 Stat이 failStatus로 실패한다(0은 끔).
    uint32 failCall = 0;
    platform::FileStatus failStatus = MemoryError;
    // 이 번호(1부터)의 Stat 결과에 크기·시각 차이를 더한다(0은 끔).
    uint32 changeCall = 0;
    uint64 changeSize = 0;
    int64 changeTicks = 0;
    // 모든 Stat 결과의 크기에 더한다(시각은 그대로 — 크기만 다른 Stat).
    uint64 alwaysSize = 0;
};

// 색인 장면이다: 메모리 Content에 Hero·Villain·Leaf와 Leaf와 같은 id의 Copy.
struct HashScene {
    HashScene(AssetEnv& env, Allocator& indexAllocator) noexcept
        : sources(env.allocator), layout(env.allocator), files(env.allocator, Initialized(layout)),
          hook(files), index(indexAllocator, layout) {
        ready = MakeSources(env, sources) && files.Ready() &&
                files.AddFile("Hero.rasset", sources.hero.View()) &&
                files.AddFile("Villain.rasset", sources.villain.View()) &&
                files.AddFile("Leaf.rasset", sources.leaf.View()) &&
                files.AddFile("Copy.rasset", sources.leaf.View()) &&
                static_cast<bool>(index.Rebuild(files, ReadOptions()));
    }

    static StringView Initialized(ProjectLayout& layout) noexcept {
        return layout.Initialize(MemoryRoot) ? layout.ContentDirectory() : StringView(MemoryRoot);
    }

    static PackageReadOptions ReadOptions() noexcept {
        PackageReadOptions options;
        options.expectedKind = PackageKind::Editor;
        options.engineVersion = TestEngineVersion;
        return options;
    }

    // relative 파일 내용을 bytes로 바꾼다(색인은 다시 만들지 않는다).
    bool ReplaceFile(StringView relative, Span<const uint8> bytes,
                     MemoryFault fault = MemoryFault::None) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, bytes, fault);
    }

    // relative 파일의 Stat이다(실패하면 기본값).
    platform::FileInfo StatOf(Allocator& allocator, StringView relative) noexcept {
        platform::FileInfo info;
        String path(allocator);
        const bool built = path.TryAppend(layout.ContentDirectory()) && path.TryAppend("/") &&
                           path.TryAppend(relative);
        CHECK(built && files.Stat(path.View().Data(), path.View().ByteCount(), info));
        return info;
    }

    const AssetIndexEntry& Entry(uint64 id) const noexcept {
        auto found = index.FindById(FixedId(id));
        CHECK(found);
        return *found.Value();
    }

    Sources sources;
    ProjectLayout layout;
    MemoryFileSystem files;
    StatHookFileSystem hook;
    AssetIndex index;
    bool ready = false;
};

Sha256Digest HashOf(Span<const uint8> bytes) noexcept {
    return Sha256::Hash(bytes);
}

} // namespace

// 계산·재사용·무효화: 처음은 전체 SHA-256과 그때의 Stat 값(크기·시각)을 두고, Stat이 같으면 열지
// 않는다. 시각만(modifiedShift)·크기만(Stat 결과의 크기만 조작) 달라도 다시 읽는다. 내용이 바뀌면
// 새 해시이고, 큰 파일(조각 경계를 넘음)·빈 파일도 전체 해시다.
TEST(AssetIndex_ContentHashComputesAndReuses) {
    AssetEnv env;
    REQUIRE(env.Ready());
    HashScene scene(env, env.allocator);
    REQUIRE(scene.ready);
    CHECK(!scene.Entry(HeroId).hasContentHash);

    const uint32 opens = scene.files.opens;
    auto hero = scene.index.ContentHash(scene.files, FixedId(HeroId));
    REQUIRE(hero);
    CHECK(hero.Value() == HashOf(scene.sources.hero.View()));
    CHECK(scene.files.opens == opens + 1);
    const platform::FileInfo info = scene.StatOf(env.allocator, "Hero.rasset");
    CHECK(scene.Entry(HeroId).hasContentHash && scene.Entry(HeroId).contentHash == hero.Value());
    CHECK(scene.Entry(HeroId).contentHashBytes == scene.sources.hero.Size());
    CHECK(scene.Entry(HeroId).contentHashTicks == info.modifiedTicks);

    // Stat이 같으면 열지 않는다.
    auto again = scene.index.ContentHash(scene.files, FixedId(HeroId));
    REQUIRE(again);
    CHECK(again.Value() == hero.Value() && scene.files.opens == opens + 1);

    // 시각만 바뀌면 다시 읽는다(같은 값, 새 시각).
    scene.files.modifiedShift = 5;
    auto shifted = scene.index.ContentHash(scene.files, FixedId(HeroId));
    REQUIRE(shifted);
    CHECK(shifted.Value() == hero.Value() && scene.files.opens == opens + 2);
    CHECK(scene.Entry(HeroId).contentHashTicks == info.modifiedTicks + 5);

    // 크기만 바뀌어 보여도(시각 같음) 다시 읽고 그 Stat 크기를 둔다. 돌아오면 또 다시 읽는다.
    scene.hook.alwaysSize = 1;
    auto sized = scene.index.ContentHash(scene.hook, FixedId(HeroId));
    REQUIRE(sized);
    CHECK(sized.Value() == hero.Value() && scene.files.opens == opens + 3);
    CHECK(scene.Entry(HeroId).contentHashBytes == scene.sources.hero.Size() + 1);
    scene.hook.Reset();
    auto restored = scene.index.ContentHash(scene.hook, FixedId(HeroId));
    REQUIRE(restored);
    CHECK(scene.files.opens == opens + 4);
    CHECK(scene.Entry(HeroId).contentHashBytes == scene.sources.hero.Size());

    // 내용이 바뀌면 새 해시다.
    const uint8 small[] = {1, 2, 3};
    REQUIRE(scene.ReplaceFile("Hero.rasset", Span<const uint8>(small, sizeof(small))));
    auto replaced = scene.index.ContentHash(scene.files, FixedId(HeroId));
    REQUIRE(replaced);
    CHECK(replaced.Value() == HashOf(Span<const uint8>(small, sizeof(small))));
    CHECK(scene.Entry(HeroId).contentHashBytes == sizeof(small));

    // 큰 파일(조각 셋 + 끝 조각).
    Array<uint8> large(env.allocator);
    REQUIRE(large.TryResize(LargeBytes));
    for (usize index = 0; index < large.Size(); ++index) {
        large[index] = static_cast<uint8>(index * 31 + 7);
    }

    REQUIRE(scene.ReplaceFile("Villain.rasset", large.View()));
    auto big = scene.index.ContentHash(scene.files, FixedId(VillainId));
    REQUIRE(big);
    CHECK(big.Value() == HashOf(large.View()));
    CHECK(scene.Entry(VillainId).contentHashBytes == LargeBytes);

    // 빈 파일.
    REQUIRE(scene.ReplaceFile("Villain.rasset", Span<const uint8>()));
    auto empty = scene.index.ContentHash(scene.files, FixedId(VillainId));
    REQUIRE(empty);
    CHECK(empty.Value() == HashOf(Span<const uint8>()));
    CHECK(scene.Entry(VillainId).contentHashBytes == 0);
}

// 거부: 없는 id NotFound, 같은 id AlreadyExists, 첫 Stat 실패·파일 없음·열기 실패·읽기 실패·두 번째
// Stat 실패는 그 파일 오류, 읽는 동안 크기만·시각만 바뀌면 Busy다. 어느 경우도 해시를 두지 않는다.
TEST(AssetIndex_ContentHashRejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    HashScene scene(env, env.allocator);
    REQUIRE(scene.ready);
    CHECK(scene.index.ContentHash(scene.files, FixedId(NowhereId)).Error() == ErrorCode::NotFound);
    CHECK(scene.index.ContentHash(scene.files, AssetId{}).Error() == ErrorCode::NotFound);
    CHECK(scene.index.ContentHash(scene.files, FixedId(LeafId)).Error() ==
          ErrorCode::AlreadyExists);

    const ErrorCode systemError = FileErrorCode(MemoryError);
    struct Case {
        uint32 failCall;
        uint32 changeCall;
        uint64 changeSize;
        int64 changeTicks;
        ErrorCode expected;
    };
    const Case cases[] = {{1, 0, 0, 0, systemError},
                          {2, 0, 0, 0, systemError},
                          {0, 2, 1, 0, ErrorCode::Busy},
                          {0, 2, 0, 1, ErrorCode::Busy}};
    for (const Case& item : cases) {
        scene.hook.Reset();
        scene.hook.failCall = item.failCall;
        scene.hook.changeCall = item.changeCall;
        scene.hook.changeSize = item.changeSize;
        scene.hook.changeTicks = item.changeTicks;
        CHECK(scene.index.ContentHash(scene.hook, FixedId(VillainId)).Error() == item.expected);
        CHECK(!scene.Entry(VillainId).hasContentHash);
    }

    scene.hook.Reset();
    REQUIRE(scene.ReplaceFile("Villain.rasset", scene.sources.villain.View(),
                              MemoryFault::OpenMissing));
    CHECK(scene.index.ContentHash(scene.files, FixedId(VillainId)).Error() ==
          FileErrorCode(MemoryMissing));
    REQUIRE(scene.ReplaceFile("Villain.rasset", scene.sources.villain.View(), MemoryFault::Read));
    CHECK(scene.index.ContentHash(scene.files, FixedId(VillainId)).Error() == systemError);
    scene.files.Remove("Villain.rasset");
    CHECK(scene.index.ContentHash(scene.files, FixedId(VillainId)).Error() ==
          FileErrorCode(MemoryMissing));
    CHECK(!scene.Entry(VillainId).hasContentHash);

    // 고장을 걷으면 계산한다.
    REQUIRE(scene.files.AddFile("Villain.rasset", scene.sources.villain.View()));
    auto computed = scene.index.ContentHash(scene.hook, FixedId(VillainId));
    REQUIRE(computed);
    CHECK(computed.Value() == HashOf(scene.sources.villain.View()));
}

// 캐시 보존: 색인 캐시가 네 칸을 담고 캐시를 쓴 색인은 같은 값을 가진다(SameIndex). Stat이 같으면
// 열지 않고, 캐시 뒤 시각만 바뀐 파일(같은 지문 — 캐시 항목은 재사용된다)은 다시 읽는다.
TEST(AssetIndex_ContentHashCachePersists) {
    AssetEnv env;
    REQUIRE(env.Ready());
    HashScene scene(env, env.allocator);
    REQUIRE(scene.ready);
    auto hero = scene.index.ContentHash(scene.files, FixedId(HeroId));
    auto villain = scene.index.ContentHash(scene.files, FixedId(VillainId));
    REQUIRE(hero && villain);
    const AssetIndexCacheKey key = MakeAssetIndexCacheKey(HashScene::ReadOptions());
    Array<uint8> bytes(env.allocator);
    REQUIRE(EncodeAssetIndexCache(scene.index, key, bytes));
    AssetIndexCache cache(env.allocator);
    auto parsed = cache.Parse(bytes.View(), key);
    REQUIRE(parsed && parsed.Value());

    AssetIndex reused(env.allocator, scene.layout);
    REQUIRE(reused.Rebuild(scene.files, HashScene::ReadOptions(), &cache));
    CHECK(SameIndex(scene.index, reused));
    auto heroEntry = reused.FindById(FixedId(HeroId));
    REQUIRE(heroEntry);
    CHECK(heroEntry.Value()->hasContentHash && heroEntry.Value()->contentHash == hero.Value());
    auto leafEntry = reused.FindByPath(ParseLogicalPath(env.allocator, "/Game/Leaf").Value());
    REQUIRE(leafEntry);
    CHECK(!leafEntry.Value()->hasContentHash);

    const uint32 opens = scene.files.opens;
    auto cached = reused.ContentHash(scene.files, FixedId(HeroId));
    REQUIRE(cached);
    CHECK(cached.Value() == hero.Value() && scene.files.opens == opens);

    // 캐시 뒤 시각이 바뀐 파일: 캐시 항목은 지문으로 재사용되지만 해시는 Stat 대조로 다시 읽는다.
    scene.files.modifiedShift = 9;
    AssetIndex stale(env.allocator, scene.layout);
    REQUIRE(stale.Rebuild(scene.files, HashScene::ReadOptions(), &cache));
    auto staleEntry = stale.FindById(FixedId(VillainId));
    REQUIRE(staleEntry);
    CHECK(staleEntry.Value()->hasContentHash &&
          staleEntry.Value()->contentHashTicks != staleEntry.Value()->modifiedTicks);
    const uint32 before = scene.files.opens;
    auto reread = stale.ContentHash(scene.files, FixedId(VillainId));
    REQUIRE(reread);
    CHECK(reread.Value() == villain.Value() && scene.files.opens == before + 1);
}

// 할당 실패: 경로·읽기 버퍼의 모든 할당 지점에서 OutOfMemory이고 해시를 두지 않는다. 지점을 다
// 지나면 계산한다.
TEST(AssetIndex_ContentHashAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    TaggedFailAllocator failing(env.allocator);
    HashScene scene(env, failing);
    REQUIRE(scene.ready);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto hashed = scene.index.ContentHash(scene.files, FixedId(HeroId));
        const bool failed = failing.Failed();
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(hashed);
            CHECK(hashed.Value() == HashOf(scene.sources.hero.View()));
            break;
        }

        CHECK(hashed.Error() == ErrorCode::OutOfMemory);
        CHECK(!scene.Entry(HeroId).hasContentHash);
        ++failures;
    }

    CHECK(failures >= 2);
}

} // namespace asset_index_content_hash_tests_file
