#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <AssetNugget/NuggetCache.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 에셋 너겟 캐시 시험이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 10·11·13–15, "A3c1 구현 세부"):
// 열기 방식(off·rockpile·auto·filesystem·열기 실패와 required), Memory 계층(적중 순서·값 선택·
// partial·SkipData·SkipMeta·64 MiB 규칙·합치기·축출·KeepAlive), 세션 고정(DigestChanged·다시 만든
// Record 고정 보관·캐시 없음·비결정 Put), 소비 입력 대조, 정책, SCC 헬퍼, backend 실패 표지, 할당
// 실패, 동시 호출.
using namespace rocks;

namespace nugget_cache_tests_file {

using rocks::asset_database_test::MemoryFault;
using rocks::asset_database_test::MemoryFileSystem;
using rocks::asset_database_test::TempProject;

namespace {

constexpr int64 BaseTicks = 639000000000000000ll;
constexpr int64 Day = 24ll * 60 * 60 * platform::FileTicksPerSecond;

class ManualClock final : public NuggetClock {
public:
    int64 NowTicks() noexcept override {
        return now;
    }

    int64 now = BaseTicks;
};

Sha256Digest Key(uint8 seed) noexcept {
    Sha256Digest key;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        key.bytes[index] = static_cast<uint8>(seed * 11 + index);
    }

    return key;
}

AssetId Member(uint64 seed) noexcept {
    return AssetId{Guid{0x3000000000000000ull + seed, 0x4000000000000000ull + seed}};
}

// 값 하나의 모양이다.
struct ValueSpec {
    uint32 id;
    usize size;
    uint8 seed;
};

Array<uint8> Pattern(Allocator& allocator, usize size, uint8 seed) noexcept {
    Array<uint8> bytes(allocator);
    CHECK(bytes.TryResize(size));
    for (usize index = 0; index < size; ++index) {
        bytes[index] = static_cast<uint8>(seed + index * 13 + (index >> 9));
    }

    return bytes;
}

Array<NuggetValue> Values(Allocator& allocator, Span<const ValueSpec> specs) noexcept {
    Array<NuggetValue> values(allocator);
    for (const ValueSpec& spec : specs) {
        NuggetValue value(allocator);
        value.id = spec.id;
        value.bytes = Pattern(allocator, spec.size, spec.seed);
        CHECK(values.TryPushBack(Move(value)));
    }

    return values;
}

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

// 값 하나가 spec의 byte와 같은지다.
bool Matches(Allocator& allocator, const NuggetValue& value, const ValueSpec& spec) noexcept {
    const Array<uint8> expected = Pattern(allocator, spec.size, spec.seed);
    return value.id == spec.id && Same(value.bytes.View(), expected.View());
}

// 캐시 하나(실제 디스크 — 프로젝트 임시 루트의 Intermediate/AssetNuggets)다.
struct CacheScene {
    explicit CacheScene(const char* tag, const NuggetCacheLimits& limits = {}) noexcept
        : project(tag), settings(allocator), root(allocator),
          cache(allocator, platform::PhysicalFileSystem(), clock, limits) {
        ready = project.made &&
                static_cast<bool>(settings.localPath.TryAssign(NuggetDefaultLocalPath)) &&
                static_cast<bool>(cache.Open(settings, project.Root())) &&
                static_cast<bool>(ResolveNuggetLocalPath(settings, project.Root(), root));
    }

    NuggetCachePutResult Put(const Sha256Digest& key, Span<const ValueSpec> specs,
                             Span<const NuggetInput> inputs = {},
                             NuggetPolicy policy = NuggetPolicy::Default) noexcept {
        Array<NuggetValue> values = Values(allocator, specs);
        return cache.Put(
            NuggetCachePut{NuggetBucketTexture, key, values.View(), inputs, {}, policy});
    }

    NuggetCacheGetResult Get(const Sha256Digest& key, Span<const uint32> ids = {},
                             NuggetPolicy policy = NuggetPolicy::Default) noexcept {
        return cache.Get(NuggetCacheGet{NuggetBucketTexture, key, ids, policy});
    }

    // 디스크 항목 경로다.
    String Entry(const Sha256Digest& key) noexcept {
        char hex[Sha256Digest::HexBytes + 1];
        key.ToHex(hex);
        String path(allocator);
        CHECK(path.TryAppend(root.View()) && path.TryAppend("/") &&
              path.TryAppend(StringView(hex, 2)) && path.TryAppend("/") &&
              path.TryAppend(StringView(hex, 64)) && path.TryAppend(".rnug"));
        return path;
    }

    bool RemoveEntry(const Sha256Digest& key) noexcept {
        const String path = Entry(key);
        return static_cast<bool>(
            platform::PhysicalFileSystem().Delete(path.View().Data(), path.View().ByteCount()));
    }

    bool HasEntry(const Sha256Digest& key) noexcept {
        const String path = Entry(key);
        bool found = false;
        return platform::PhysicalFileSystem().Exists(path.View().Data(), path.View().ByteCount(),
                                                     found) &&
               found;
    }

    // 캐시를 거치지 않고 디스크에 쓴다(다른 세션의 게시).
    NuggetStatus Publish(const Sha256Digest& key, Span<const ValueSpec> specs,
                         Span<const NuggetInput> inputs = {},
                         Span<const uint8> meta = {}) noexcept {
        NuggetFileSystemBackend other(allocator, platform::PhysicalFileSystem(), root.View(),
                                      clock);
        CHECK(other.Open());
        Array<NuggetValue> values = Values(allocator, specs);
        Array<NuggetPutValue> spans(allocator);
        for (const NuggetValue& value : values) {
            CHECK(spans.TryPushBack(NuggetPutValue{value.id, value.bytes.View()}));
        }

        return other.Put(NuggetBackendPut{NuggetBucketTexture, key, spans.View(), inputs, meta})
            .status;
    }

    SystemAllocator allocator;
    TempProject project;
    ManualClock clock;
    NuggetCacheSettings settings;
    String root;
    NuggetCache cache;
    bool ready = false;
};

NuggetInput Input(Allocator& allocator, const Sha256Digest& key,
                  const Sha256Digest& digest) noexcept {
    NuggetInput input(allocator);
    CHECK(input.bucket.TryAssign(NuggetBucketTexture));
    input.key = key;
    input.digest = digest;
    return input;
}

// 물리 파일 시스템에 넘기되 한 번 hook을 거는 래퍼다: 경로가 suffix로 끝나고 열기 방식이 mode인 첫
// Open 직전에 callback을 부른다(캐시 잠금 밖 backend IO 사이 — 경합 창을 결정적으로 만든다).
// callback 안에서 같은 캐시를 다시 불러도 된다(hook은 부르기 전에 풀린다).
class HookFileSystem final : public platform::FileSystem {
public:
    void Arm(StringView pathSuffix, platform::FileOpenMode openMode, void (*function)(void*),
             void* functionContext) noexcept {
        suffix = pathSuffix;
        mode = openMode;
        callback = function;
        context = functionContext;
    }

    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode openMode,
                              platform::FileHandle*& out) noexcept override {
        if (callback != nullptr && openMode == mode &&
            StringView(path, pathBytes).EndsWith(suffix, StringComparison::CaseSensitive)) {
            auto* function = callback;
            callback = nullptr;
            function(context);
        }

        return inner->Open(path, pathBytes, openMode, out);
    }

    platform::FileStatus Exists(const char* path, usize pathBytes, bool& exists) noexcept override {
        return inner->Exists(path, pathBytes, exists);
    }

    platform::FileStatus Delete(const char* path, usize pathBytes) noexcept override {
        return inner->Delete(path, pathBytes);
    }

    platform::FileStatus Stat(const char* path, usize pathBytes,
                              platform::FileInfo& info) noexcept override {
        return inner->Stat(path, pathBytes, info);
    }

    platform::FileStatus MakeDirectory(const char* path, usize pathBytes) noexcept override {
        return inner->MakeDirectory(path, pathBytes);
    }

    platform::FileStatus DeleteDirectory(const char* path, usize pathBytes) noexcept override {
        return inner->DeleteDirectory(path, pathBytes);
    }

    platform::FileStatus EnumerateDirectory(const char* path, usize pathBytes,
                                            platform::DirectoryVisitor visitor,
                                            void* visitorContext) noexcept override {
        return inner->EnumerateDirectory(path, pathBytes, visitor, visitorContext);
    }

    platform::FileStatus Move(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        return inner->Move(from, fromBytes, to, toBytes, replaceExisting);
    }

    platform::FileStatus Copy(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        return inner->Copy(from, fromBytes, to, toBytes, replaceExisting);
    }

    platform::FileStatus Replace(const char* from, usize fromBytes, const char* to,
                                 usize toBytes) noexcept override {
        return inner->Replace(from, fromBytes, to, toBytes);
    }

    platform::FileStatus SetModifiedTime(const char* path, usize pathBytes,
                                         int64 modifiedTicks) noexcept override {
        return inner->SetModifiedTime(path, pathBytes, modifiedTicks);
    }

    platform::FileStatus SyncDirectory(const char* path, usize pathBytes) noexcept override {
        return inner->SyncDirectory(path, pathBytes);
    }

    platform::FileSystem* inner = &platform::PhysicalFileSystem();
    StringView suffix{};
    platform::FileOpenMode mode = platform::FileOpenMode::ReadExisting;
    void (*callback)(void*) = nullptr;
    void* context = nullptr;
};

} // namespace

// 열기: 열기 전 Get Miss·Put Dropped(InvalidArgument), off(backend 없음 — Miss·Dropped 오류 없음),
// 두 번째 Open InvalidArgument, rockpile Unsupported(열지 않음 — 다시 열 수 있다),
// auto·filesystem은 localPath에 루트를 만든다, 루트 자리가 파일이면 required는 Open 실패·아니면
// off로 내리고 원인을 남긴다, 세대·required 접근자, 절대 localPath.
TEST(NuggetCache_OpenModes) {
    SystemAllocator allocator;
    TempProject project("nugget-cache-open");
    REQUIRE(project.made);
    ManualClock clock;
    platform::FileSystem& files = platform::PhysicalFileSystem();
    NuggetCacheSettings settings(allocator);
    REQUIRE(settings.localPath.TryAssign(NuggetDefaultLocalPath));
    const ValueSpec spec[] = {{0, 100, 1}};
    {
        NuggetCache cache(allocator, files, clock);
        NuggetCacheGetResult before = cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)});
        CHECK(before.status == NuggetStatus::Miss && before.error == ErrorCode::InvalidArgument &&
              !before.failed);
        Array<NuggetValue> values = Values(allocator, spec);
        NuggetCachePutResult put =
            cache.Put(NuggetCachePut{NuggetBucketTexture, Key(1), values.View()});
        CHECK(put.status == NuggetStatus::Dropped && put.error == ErrorCode::InvalidArgument);
        settings.backend = NuggetBackendKind::Rockpile;
        CHECK(cache.Open(settings, project.Root()).Error() == ErrorCode::Unsupported);
        settings.backend = NuggetBackendKind::Off;
        settings.generation = Guid{1, 2};
        REQUIRE(cache.Open(settings, project.Root()));
        CHECK(!cache.IsEnabled() && cache.OpenError() == ErrorCode::None &&
              (cache.ProjectGeneration() == Guid{1, 2}) && !cache.IsRequired());
        CHECK(cache.Open(settings, project.Root()).Error() == ErrorCode::InvalidArgument);
        NuggetCacheGetResult off = cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)});
        CHECK(off.status == NuggetStatus::Miss && off.error == ErrorCode::None);
        values = Values(allocator, spec);
        put = cache.Put(NuggetCachePut{NuggetBucketTexture, Key(1), values.View()});
        CHECK(put.status == NuggetStatus::Dropped && put.error == ErrorCode::None && !put.failed);
        CHECK(cache.Stats().misses == 1 && cache.Stats().dropped == 1);
    }

    const NuggetBackendKind enabled[] = {NuggetBackendKind::Auto, NuggetBackendKind::FileSystem};
    for (NuggetBackendKind kind : enabled) {
        settings.backend = kind;
        NuggetCache cache(allocator, files, clock);
        REQUIRE(cache.Open(settings, project.Root()));
        CHECK(cache.IsEnabled());
        String root(allocator);
        REQUIRE(ResolveNuggetLocalPath(settings, project.Root(), root));
        platform::FileInfo info;
        CHECK(files.Stat(root.View().Data(), root.View().ByteCount(), info) &&
              info.type == platform::FileType::Directory);
    }

    // 루트 자리가 파일.
    String blocked(allocator);
    REQUIRE(blocked.TryAppend(project.Root()) && blocked.TryAppend("/Blocked"));
    {
        platform::File file;
        REQUIRE(file.Open(files, blocked.View().Data(), blocked.View().ByteCount(),
                          platform::FileOpenMode::CreateAlways));
        REQUIRE(file.Close());
    }

    REQUIRE(settings.localPath.TryAssign("Blocked"));
    settings.backend = NuggetBackendKind::FileSystem;
    settings.required = true;
    {
        NuggetCache cache(allocator, files, clock);
        auto opened = cache.Open(settings, project.Root());
        CHECK(!opened && opened.Error() != ErrorCode::None && !cache.IsEnabled());
    }

    settings.required = false;
    {
        NuggetCache cache(allocator, files, clock);
        REQUIRE(cache.Open(settings, project.Root()));
        CHECK(!cache.IsEnabled() && cache.OpenError() != ErrorCode::None);
        CHECK(cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)}).status == NuggetStatus::Miss);
    }

    // required + 성공.
    REQUIRE(settings.localPath.TryAssign("Required/Nuggets"));
    settings.required = true;
    NuggetCache cache(allocator, files, clock);
    REQUIRE(cache.Open(settings, project.Root()));
    CHECK(cache.IsEnabled() && cache.IsRequired());
}

// Memory 계층: Put이 Memory에 넣는다(적중 Memory — 디스크 항목을 지워도), 새 캐시는 Local 적중 뒤
// Memory, 값 선택(청한 순서)·Record에 없는 값(partial이면 Partial·아니면 Memory에서 Miss)·SkipData·
// SkipMeta·같은 값 두 번(backend 판정 Miss InvalidArgument)·다른 bucket(backend 판정), 64 MiB 규칙
// (큰 값은 쥐지 않음 — 그 값을 청하면 Local), 값 합치기(일부를 얻은 뒤 나머지), KeepAlive Touch.
TEST(NuggetCache_MemoryLayer) {
    NuggetCacheLimits limits;
    limits.memoryValueBytes = 4096;
    CacheScene scene("nugget-cache-memory", limits);
    REQUIRE(scene.ready);
    const ValueSpec specs[] = {{1, 300, 1}, {4, 500, 2}, {9, 5000, 3}};
    NuggetCachePutResult put = scene.Put(Key(1), specs);
    REQUIRE(put.status == NuggetStatus::Stored);
    // 큰 값(9)은 쥐지 않으므로 전체 Get은 Local이다.
    NuggetCacheGetResult all = scene.Get(Key(1));
    REQUIRE(all.status == NuggetStatus::Hit && all.values.Size() == 3);
    CHECK(all.source == NuggetSource::Local && all.digest == put.digest &&
          Matches(scene.allocator, all.values[2], specs[2]));
    // 쥔 값만 청하면 Memory다 — 디스크 항목을 지워도.
    REQUIRE(scene.RemoveEntry(Key(1)));
    const uint32 small[] = {4, 1};
    NuggetCacheGetResult held = scene.Get(Key(1), small);
    REQUIRE(held.status == NuggetStatus::Hit && held.values.Size() == 2);
    CHECK(held.source == NuggetSource::Memory && held.digest == put.digest &&
          Matches(scene.allocator, held.values[0], specs[1]) &&
          Matches(scene.allocator, held.values[1], specs[0]) && held.record.values.Size() == 3 &&
          held.record.bucket.View() == NuggetBucketTexture);
    // Record에 없는 값: partial이면 Partial, 아니면 Memory에서 확정 Miss.
    const uint32 withAbsent[] = {1, 7};
    NuggetCacheGetResult absent = scene.Get(Key(1), withAbsent);
    CHECK(absent.status == NuggetStatus::Miss && absent.source == NuggetSource::Memory &&
          absent.values.IsEmpty() && absent.error == ErrorCode::None);
    NuggetCacheGetResult partial =
        scene.Get(Key(1), withAbsent, NuggetPolicy::Default | NuggetPolicy::PartialRecord);
    CHECK(partial.status == NuggetStatus::Partial && partial.source == NuggetSource::Memory &&
          partial.values.Size() == 1 && partial.values[0].id == 1);
    // SkipData(큰 값이 있어도 Memory), SkipMeta.
    NuggetCacheGetResult bare =
        scene.Get(Key(1), {}, NuggetPolicy::Default | NuggetPolicy::SkipData);
    CHECK(bare.status == NuggetStatus::Hit && bare.source == NuggetSource::Memory &&
          bare.values.IsEmpty() && bare.record.values.Size() == 3);
    // 같은 값 두 번은 backend가 판정한다(디스크 항목이 없어 Miss).
    const uint32 twice[] = {1, 1};
    CHECK(scene.Get(Key(1), twice).status == NuggetStatus::Miss);
    // 디스크에 다시 두고 큰 값을 청하면 Local, 다른 bucket은 backend가 Miss(InvalidArgument).
    REQUIRE(scene.Publish(Key(1), specs) == NuggetStatus::Stored);
    const uint32 large[] = {9};
    NuggetCacheGetResult big = scene.Get(Key(1), large);
    CHECK(big.status == NuggetStatus::Hit && big.source == NuggetSource::Local &&
          Matches(scene.allocator, big.values[0], specs[2]));
    NuggetCacheGetResult other = scene.cache.Get(NuggetCacheGet{NuggetBucketShader, Key(1)});
    CHECK(other.status == NuggetStatus::Miss && other.error == ErrorCode::InvalidArgument &&
          other.failed);
    CHECK(scene.Get(Key(1), twice).error == ErrorCode::InvalidArgument);

    // SkipMeta: meta가 있는 Record(다른 세션 게시)를 새 캐시로 얻는다 — Memory 항목은 meta를 쥔다.
    const uint8 meta[] = {5, 6, 7};
    REQUIRE(scene.RemoveEntry(Key(2)) == false);
    {
        NuggetFileSystemBackend direct(scene.allocator, platform::PhysicalFileSystem(),
                                       scene.root.View(), scene.clock);
        REQUIRE(direct.Open());
        const Array<uint8> bytes = Pattern(scene.allocator, 64, 9);
        const NuggetPutValue one[] = {{0, bytes.View()}};
        REQUIRE(direct.Put(NuggetBackendPut{NuggetBucketTexture, Key(2), one, {}, meta}).status ==
                NuggetStatus::Stored);
    }

    NuggetCacheGetResult noMeta =
        scene.Get(Key(2), {}, NuggetPolicy::Default | NuggetPolicy::SkipMeta);
    CHECK(noMeta.status == NuggetStatus::Hit && noMeta.source == NuggetSource::Local &&
          noMeta.record.meta.IsEmpty());
    NuggetCacheGetResult withMeta = scene.Get(Key(2));
    CHECK(withMeta.status == NuggetStatus::Hit && withMeta.source == NuggetSource::Memory &&
          withMeta.record.meta.Size() == 3);
    NuggetCacheGetResult memoryNoMeta =
        scene.Get(Key(2), {}, NuggetPolicy::Default | NuggetPolicy::SkipMeta);
    CHECK(memoryNoMeta.source == NuggetSource::Memory && memoryNoMeta.record.meta.IsEmpty());

    // 합치기: 새 캐시가 값 일부를 Local로 얻고 나머지를 얻으면 둘 다 Memory에 있다.
    const ValueSpec pair[] = {{0, 200, 4}, {1, 300, 5}};
    REQUIRE(scene.Publish(Key(3), pair) == NuggetStatus::Stored);
    NuggetCache fresh(scene.allocator, platform::PhysicalFileSystem(), scene.clock, limits);
    REQUIRE(fresh.Open(scene.settings, scene.project.Root()));
    const uint32 first[] = {0};
    const uint32 second[] = {1};
    CHECK(fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(3), first}).source ==
          NuggetSource::Local);
    CHECK(fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(3), second}).source ==
          NuggetSource::Local);
    REQUIRE(scene.RemoveEntry(Key(3)));
    NuggetCacheGetResult merged = fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(3)});
    CHECK(merged.status == NuggetStatus::Hit && merged.source == NuggetSource::Memory &&
          merged.values.Size() == 2 && Matches(scene.allocator, merged.values[1], pair[1]));
    // 같은 값을 다시 얻어도 합치기는 중복을 만들지 않는다.
    CHECK(fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(3), first}).values.Size() == 1);
    // 가운데 삽입: 0·2를 쥔 뒤 1을 얻는다.
    const ValueSpec triple[] = {{0, 100, 6}, {1, 110, 7}, {2, 120, 8}};
    REQUIRE(scene.Publish(Key(5), triple) == NuggetStatus::Stored);
    const uint32 zero[] = {0};
    const uint32 two[] = {2};
    const uint32 one[] = {1};
    REQUIRE(fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(5), zero}).status ==
            NuggetStatus::Hit);
    REQUIRE(fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(5), two}).status ==
            NuggetStatus::Hit);
    REQUIRE(fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(5), one}).source ==
            NuggetSource::Local);
    REQUIRE(scene.RemoveEntry(Key(5)));
    NuggetCacheGetResult ordered = fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(5)});
    CHECK(ordered.source == NuggetSource::Memory && ordered.values.Size() == 3 &&
          Matches(scene.allocator, ordered.values[1], triple[1]));
    // 새 항목도 큰 값은 쥐지 않는다(Key(1) — 9는 4096 byte보다 크다).
    REQUIRE(scene.Publish(Key(1), specs) == NuggetStatus::Exists);
    REQUIRE(fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)}).source == NuggetSource::Local);
    CHECK(fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(1), small}).source ==
              NuggetSource::Memory &&
          fresh.Get(NuggetCacheGet{NuggetBucketTexture, Key(1), large}).source ==
              NuggetSource::Local);

    // Put의 큰 값이 작은 값보다 앞서도 작은 값만 쥔다(디스크를 지우면 작은 값만 Memory).
    const ValueSpec bigFirst[] = {{0, 5000, 9}, {1, 300, 10}};
    REQUIRE(scene.Put(Key(6), bigFirst).status == NuggetStatus::Stored);
    REQUIRE(scene.RemoveEntry(Key(6)));
    const uint32 later[] = {1};
    NuggetCacheGetResult smallOnly = scene.Get(Key(6), later);
    CHECK(smallOnly.source == NuggetSource::Memory &&
          Matches(scene.allocator, smallOnly.values[0], bigFirst[1]));
    const uint32 earlier[] = {0};
    CHECK(scene.Get(Key(6), earlier).status == NuggetStatus::Miss);

    // KeepAlive: Memory 적중이 디스크 사용 시각을 갱신한다(없으면 갱신하지 않는다).
    REQUIRE(scene.Publish(Key(4), pair) == NuggetStatus::Stored);
    REQUIRE(scene.Get(Key(4)).source == NuggetSource::Local);
    const String entry = scene.Entry(Key(4));
    auto modified = [&]() noexcept {
        platform::FileInfo info;
        CHECK(platform::PhysicalFileSystem().Stat(entry.View().Data(), entry.View().ByteCount(),
                                                  info));
        return info.modifiedTicks;
    };
    const int64 before = modified();
    scene.clock.now += Day;
    CHECK(scene.Get(Key(4)).source == NuggetSource::Memory && modified() == before);
    CHECK(scene.Get(Key(4), {}, NuggetPolicy::Default | NuggetPolicy::KeepAlive).source ==
              NuggetSource::Memory &&
          modified() == scene.clock.now);
    const NuggetCacheStats stats = scene.cache.Stats();
    CHECK(stats.memoryHits >= 6 && stats.localHits >= 4 && stats.partials == 1 &&
          stats.memoryBytes > 0 && stats.pinnedBytes == 0);
}

// 축출: 합이 상한을 넘으면 오래 쓰지 않은 것부터 버린다(Get이 순서를 새로 한다), 상한보다 큰 항목은
// 넣지 않는다, 상한 0은 Memory 없음(고정 보관 제외).
TEST(NuggetCache_Eviction) {
    const usize entryBytes = NuggetMemoryEntryBytes + NuggetValueRowBytes + 1000;
    NuggetCacheLimits limits;
    limits.memoryBytes = entryBytes * 3;
    CacheScene scene("nugget-cache-evict", limits);
    REQUIRE(scene.ready);
    const ValueSpec spec[] = {{0, 1000, 1}};
    for (uint8 seed = 1; seed <= 3; ++seed) {
        REQUIRE(scene.Put(Key(seed), spec).status == NuggetStatus::Stored);
    }

    CHECK(scene.cache.Stats().memoryBytes == entryBytes * 3);
    // 1을 새로 쓰면 다음 축출 대상은 2다.
    CHECK(scene.Get(Key(1)).source == NuggetSource::Memory);
    REQUIRE(scene.Put(Key(4), spec).status == NuggetStatus::Stored);
    CHECK(scene.Get(Key(1)).source == NuggetSource::Memory &&
          scene.Get(Key(3)).source == NuggetSource::Memory &&
          scene.Get(Key(4)).source == NuggetSource::Memory &&
          scene.Get(Key(2)).source == NuggetSource::Local);
    // 상한보다 큰 항목은 넣지 않는다(다른 항목도 버리지 않는다 — 2를 얻으며 1이 나갔다).
    const ValueSpec huge[] = {{0, entryBytes * 4, 2}};
    REQUIRE(scene.Put(Key(5), huge).status == NuggetStatus::Stored);
    CHECK(scene.Get(Key(5)).source == NuggetSource::Local &&
          scene.cache.Stats().memoryBytes == entryBytes * 3);

    NuggetCacheLimits none;
    none.memoryBytes = 0;
    CacheScene empty("nugget-cache-nomemory", none);
    REQUIRE(empty.ready);
    REQUIRE(empty.Put(Key(1), spec).status == NuggetStatus::Stored);
    CHECK(empty.Get(Key(1)).source == NuggetSource::Local &&
          empty.Get(Key(1)).source == NuggetSource::Local && empty.cache.Stats().memoryBytes == 0);
}

// 세션 고정: 고정과 다른 디스크 판은 DigestChanged(값 버림·Memory 항목 지움 — 다시 얻어도 계속),
// 다시 만든 Record의 Put은 Conflict여도 값 전부를 고정 보관(64 MiB 규칙·Memory 상한 예외, 축출
// 없음)하고 고정 digest를 바꾼다(이후 Memory 적중), 고정 보관 상한을 넘으면 캐시 없음(Get Miss·
// Put Dropped — 통계), 비결정 Put(DigestChanged 없이 다른 Record)은 고정·Memory를 바꾸지 않는다.
TEST(NuggetCache_PinsAndRegeneration) {
    NuggetCacheLimits limits;
    limits.memoryBytes = 0;
    limits.memoryValueBytes = 16;
    limits.pinnedBytes = 4000;
    CacheScene scene("nugget-cache-pins", limits);
    REQUIRE(scene.ready);
    const ValueSpec first[] = {{0, 600, 1}};
    const ValueSpec second[] = {{0, 600, 2}};
    const ValueSpec third[] = {{0, 600, 3}};
    NuggetCachePutResult original = scene.Put(Key(1), first);
    REQUIRE(original.status == NuggetStatus::Stored);
    // 다른 세션이 디스크 판을 바꾼다.
    REQUIRE(scene.RemoveEntry(Key(1)));
    REQUIRE(scene.Publish(Key(1), second) == NuggetStatus::Stored);
    NuggetCacheGetResult changed = scene.Get(Key(1));
    CHECK(changed.status == NuggetStatus::DigestChanged && changed.values.IsEmpty() &&
          !(changed.digest == original.digest) && !changed.failed);
    CHECK(scene.Get(Key(1)).status == NuggetStatus::DigestChanged);
    // 다시 만든 Record(third) — 디스크는 second라 Conflict지만 고정 보관한다.
    NuggetCachePutResult regenerated = scene.Put(Key(1), third);
    CHECK(regenerated.status == NuggetStatus::Conflict);
    NuggetCacheGetResult pinned = scene.Get(Key(1));
    CHECK(pinned.status == NuggetStatus::Hit && pinned.source == NuggetSource::Memory &&
          pinned.digest == regenerated.digest &&
          Matches(scene.allocator, pinned.values[0], third[0]));
    NuggetCacheStats stats = scene.cache.Stats();
    CHECK(stats.digestChanged == 2 && stats.pinnedBytes > 0 && stats.memoryBytes == 0 &&
          stats.conflicts == 1);
    // 다시 만든 키의 또 다른 Put(다른 Record)은 고정을 바꾸지 않는다.
    REQUIRE(scene.Put(Key(1), first).status == NuggetStatus::Conflict);
    CHECK(scene.Get(Key(1)).digest == regenerated.digest);

    // 고정 보관 상한 초과 → 캐시 없음.
    const ValueSpec big[] = {{0, 3000, 4}};
    const ValueSpec bigOther[] = {{0, 3000, 5}};
    REQUIRE(scene.Put(Key(2), big).status == NuggetStatus::Stored);
    REQUIRE(scene.RemoveEntry(Key(2)));
    REQUIRE(scene.Publish(Key(2), bigOther) == NuggetStatus::Stored);
    REQUIRE(scene.Get(Key(2)).status == NuggetStatus::DigestChanged);
    CHECK(scene.Put(Key(2), big).status == NuggetStatus::Conflict);
    CHECK(scene.cache.Stats().uncachedKeys == 1);
    NuggetCacheGetResult uncached = scene.Get(Key(2));
    CHECK(uncached.status == NuggetStatus::Miss && uncached.error == ErrorCode::None &&
          uncached.source == NuggetSource::None);
    NuggetCachePutResult refused = scene.Put(Key(2), big);
    CHECK(refused.status == NuggetStatus::Dropped && refused.error == ErrorCode::None &&
          !refused.failed);

    // 비결정 Put: DigestChanged 없이 다른 Record — 디스크는 Conflict, 고정은 그대로.
    NuggetCacheLimits roomy;
    CacheScene plain("nugget-cache-nondeterministic", roomy);
    REQUIRE(plain.ready);
    NuggetCachePutResult kept = plain.Put(Key(3), first);
    REQUIRE(kept.status == NuggetStatus::Stored);
    NuggetCachePutResult drift = plain.Put(Key(3), second);
    CHECK(drift.status == NuggetStatus::Conflict && drift.existing == kept.digest);
    NuggetCacheGetResult stable = plain.Get(Key(3));
    CHECK(stable.source == NuggetSource::Memory && stable.digest == kept.digest);
    // 같은 Record의 Put은 Exists이고 Memory를 그대로 둔다.
    CHECK(plain.Put(Key(3), first).status == NuggetStatus::Exists &&
          plain.Get(Key(3)).source == NuggetSource::Memory);
}

// 소비 입력 대조: 고정된 키를 다른 digest로 소비한 Record는 Miss(InputMismatch — 고정하지 않는다),
// 같은 digest로 소비하면 적중하고 고정한다, 고정 없는 입력은 그 digest로 고정한다(이후 그 키의 다른
// 판은 DigestChanged), Memory 적중도 대조한다.
TEST(NuggetCache_InputMismatch) {
    CacheScene scene("nugget-cache-inputs");
    REQUIRE(scene.ready);
    const ValueSpec base[] = {{0, 100, 1}};
    const ValueSpec derived[] = {{0, 100, 2}};
    NuggetCachePutResult pinned = scene.Put(Key(1), base);
    REQUIRE(pinned.status == NuggetStatus::Stored);
    // Key(2)는 Key(1)의 다른 판을 소비했다.
    const NuggetInput stale[] = {Input(scene.allocator, Key(1), Key(99))};
    REQUIRE(scene.Publish(Key(2), derived, stale) == NuggetStatus::Stored);
    NuggetCacheGetResult mismatch = scene.Get(Key(2));
    CHECK(mismatch.status == NuggetStatus::Miss && mismatch.error == ErrorCode::None &&
          mismatch.values.IsEmpty() && !mismatch.failed);
    CHECK(scene.cache.Stats().inputMismatches == 1);
    // 같은 판을 소비한 Record는 적중한다.
    const NuggetInput fresh[] = {Input(scene.allocator, Key(1), pinned.digest)};
    REQUIRE(scene.Publish(Key(3), derived, fresh) == NuggetStatus::Stored);
    NuggetCacheGetResult matched = scene.Get(Key(3));
    CHECK(matched.status == NuggetStatus::Hit && matched.record.inputs.Size() == 1);
    // 고정 없는 입력(Key(4) — 아직 쓰지 않음)은 그 digest로 고정된다.
    const NuggetInput unpinned[] = {Input(scene.allocator, Key(4), Key(77))};
    REQUIRE(scene.Publish(Key(5), derived, unpinned) == NuggetStatus::Stored);
    REQUIRE(scene.Get(Key(5)).status == NuggetStatus::Hit);
    REQUIRE(scene.Publish(Key(4), base) == NuggetStatus::Stored);
    CHECK(scene.Get(Key(4)).status == NuggetStatus::DigestChanged);
    // Memory 적중도 대조한다: Key(6)을 소비한 Key(7)을 Memory에 둔 뒤 Key(6)의 고정이 바뀌면.
    const ValueSpec six[] = {{0, 100, 6}};
    NuggetCachePutResult sixPut = scene.Put(Key(6), six);
    REQUIRE(sixPut.status == NuggetStatus::Stored);
    const NuggetInput consumed[] = {Input(scene.allocator, Key(6), sixPut.digest)};
    REQUIRE(scene.Put(Key(7), derived, consumed).status == NuggetStatus::Stored);
    CHECK(scene.Get(Key(7)).source == NuggetSource::Memory);
    // Key(6)을 다시 만든다(DigestChanged 경로 — 고정 digest가 바뀐다).
    REQUIRE(scene.RemoveEntry(Key(6)));
    const ValueSpec sixOther[] = {{0, 100, 16}};
    REQUIRE(scene.Publish(Key(6), sixOther) == NuggetStatus::Stored);
    REQUIRE(scene.cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(6)}).status ==
            NuggetStatus::Hit);
    // (Key(6)은 Memory에 있어 DigestChanged가 아니다 — Memory 상한 0인 캐시로 다시 본다.)
    NuggetCacheLimits none;
    none.memoryBytes = 0;
    NuggetCache cold(scene.allocator, platform::PhysicalFileSystem(), scene.clock, none);
    REQUIRE(cold.Open(scene.settings, scene.project.Root()));
    REQUIRE(cold.Get(NuggetCacheGet{NuggetBucketTexture, Key(7)}).status == NuggetStatus::Hit);
    CHECK(cold.Get(NuggetCacheGet{NuggetBucketTexture, Key(6)}).status ==
          NuggetStatus::DigestChanged);
    const ValueSpec sixAgain[] = {{0, 100, 26}};
    Array<NuggetValue> again = Values(scene.allocator, sixAgain);
    REQUIRE(cold.Put(NuggetCachePut{NuggetBucketTexture, Key(6), again.View()}).status ==
            NuggetStatus::Conflict);
    NuggetCacheGetResult afterChange = cold.Get(NuggetCacheGet{NuggetBucketTexture, Key(7)});
    CHECK(afterChange.status == NuggetStatus::Miss && cold.Stats().inputMismatches == 1);
}

// 정책: QueryLocal 없음(QueryRemote만·없음) Miss, Store 없음 Dropped(오류 없음), StoreRemote만은
// 저장(StoreLocal 함의), SkipData는 Memory에 넣지 않는다.
TEST(NuggetCache_Policies) {
    CacheScene scene("nugget-cache-policy");
    REQUIRE(scene.ready);
    const ValueSpec spec[] = {{0, 100, 1}};
    NuggetCachePutResult none = scene.Put(Key(1), spec, {}, NuggetPolicy::Query);
    CHECK(none.status == NuggetStatus::Dropped && none.error == ErrorCode::None &&
          !scene.HasEntry(Key(1)));
    REQUIRE(scene.Put(Key(1), spec, {}, NuggetPolicy::Query | NuggetPolicy::StoreRemote).status ==
            NuggetStatus::Stored);
    CHECK(scene.HasEntry(Key(1)));
    CHECK(scene.Get(Key(1), {}, NuggetPolicy::QueryRemote).status == NuggetStatus::Miss &&
          scene.Get(Key(1), {}, NuggetPolicy::Store).status == NuggetStatus::Miss);
    CHECK(scene.Get(Key(1), {}, NuggetPolicy::QueryLocal).status == NuggetStatus::Hit);
    // SkipData Local 적중은 Memory에 넣지 않는다.
    REQUIRE(scene.Publish(Key(2), spec) == NuggetStatus::Stored);
    CHECK(scene.Get(Key(2), {}, NuggetPolicy::Default | NuggetPolicy::SkipData).source ==
          NuggetSource::Local);
    CHECK(scene.Get(Key(2), {}, NuggetPolicy::Default | NuggetPolicy::SkipData).source ==
          NuggetSource::Local);
    CHECK(scene.Get(Key(2)).source == NuggetSource::Local &&
          scene.Get(Key(2)).source == NuggetSource::Memory);
}

// SCC 헬퍼: PutScc가 meta를 구성원 목록으로 채운다, GetSccMember가 그 구성원 값만(부분 여럿)
// 고른다, 값 없는 구성원은 빈 Hit, 구성원 아님 Miss(NotFound), 4,096 초과
// Dropped(CapacityExceeded·실패 아님), 순번이 구성원 수 이상인 값·잘못된 구성원 목록
// Dropped(InvalidArgument), 잘못된 meta Miss (InvalidArgument), Record 없음 Miss, SkipMeta.
TEST(NuggetCache_Scc) {
    CacheScene scene("nugget-cache-scc");
    REQUIRE(scene.ready);
    const AssetId members[] = {Member(1), Member(2), Member(3)};
    const ValueSpec specs[] = {{NuggetSccValueId(0, 0), 100, 1},
                               {NuggetSccValueId(0, 1), 120, 2},
                               {NuggetSccValueId(2, 0), 140, 3}};
    Array<NuggetValue> values = Values(scene.allocator, specs);
    NuggetCachePut put{NuggetBucketScc, Key(1), values.View()};
    REQUIRE(scene.cache.PutScc(members, put).status == NuggetStatus::Stored);
    NuggetCacheGetResult first = scene.cache.GetSccMember(NuggetBucketScc, Key(1), members[0]);
    REQUIRE(first.status == NuggetStatus::Hit && first.values.Size() == 2);
    CHECK(Matches(scene.allocator, first.values[0], specs[0]) &&
          Matches(scene.allocator, first.values[1], specs[1]) &&
          first.record.meta.Size() == 3 * NuggetSccMemberBytes);
    NuggetCacheGetResult third = scene.cache.GetSccMember(
        NuggetBucketScc, Key(1), members[2], NuggetPolicy::Default | NuggetPolicy::SkipMeta);
    CHECK(third.status == NuggetStatus::Hit && third.values.Size() == 1 &&
          Matches(scene.allocator, third.values[0], specs[2]) && third.record.meta.IsEmpty());
    NuggetCacheGetResult empty = scene.cache.GetSccMember(NuggetBucketScc, Key(1), members[1]);
    CHECK(empty.status == NuggetStatus::Hit && empty.values.IsEmpty() &&
          empty.record.meta.Size() == 3 * NuggetSccMemberBytes);
    NuggetCacheGetResult emptyNoMeta = scene.cache.GetSccMember(
        NuggetBucketScc, Key(1), members[1], NuggetPolicy::Default | NuggetPolicy::SkipMeta);
    CHECK(emptyNoMeta.status == NuggetStatus::Hit && emptyNoMeta.record.meta.IsEmpty());
    NuggetCacheGetResult stranger = scene.cache.GetSccMember(NuggetBucketScc, Key(1), Member(9));
    CHECK(stranger.status == NuggetStatus::Miss && stranger.error == ErrorCode::NotFound &&
          !stranger.failed);
    CHECK(scene.cache.GetSccMember(NuggetBucketScc, Key(2), members[0]).status ==
          NuggetStatus::Miss);

    // 거부.
    Array<AssetId> many(scene.allocator);
    for (usize index = 0; index <= NuggetSccMaxMembers; ++index) {
        REQUIRE(many.TryPushBack(Member(index + 100)));
    }

    values = Values(scene.allocator, specs);
    put.values = values.View();
    NuggetCachePutResult capacity = scene.cache.PutScc(many.View(), put);
    CHECK(capacity.status == NuggetStatus::Dropped &&
          capacity.error == ErrorCode::CapacityExceeded && !capacity.failed);
    const AssetId two[] = {Member(1), Member(2)};
    NuggetCachePutResult outside = scene.cache.PutScc(two, put);
    CHECK(outside.status == NuggetStatus::Dropped && outside.error == ErrorCode::InvalidArgument);
    const AssetId duplicate[] = {Member(1), Member(1), Member(3)};
    CHECK(scene.cache.PutScc(duplicate, put).error == ErrorCode::InvalidArgument);
    // 잘못된 meta(16의 배수 아님).
    REQUIRE(scene.RemoveEntry(Key(3)) == false);
    {
        NuggetFileSystemBackend direct(scene.allocator, platform::PhysicalFileSystem(),
                                       scene.root.View(), scene.clock);
        REQUIRE(direct.Open());
        const Array<uint8> bytes = Pattern(scene.allocator, 50, 1);
        const NuggetPutValue one[] = {{0, bytes.View()}};
        const uint8 broken[] = {1, 2, 3, 4, 5};
        REQUIRE(direct.Put(NuggetBackendPut{NuggetBucketScc, Key(3), one, {}, broken}).status ==
                NuggetStatus::Stored);
    }

    NuggetCacheGetResult badMeta = scene.cache.GetSccMember(NuggetBucketScc, Key(3), members[0]);
    CHECK(badMeta.status == NuggetStatus::Miss && badMeta.error == ErrorCode::InvalidArgument);
}

// backend 실패 표지: IO 실패의 Get Miss·Put Dropped는 failed, 다른 버전(Unsupported)·읽기 전용은
// 실패 아님, 손상(Corrupt — 지움)은 실패 아님.
TEST(NuggetCache_FailureFlags) {
    SystemAllocator allocator;
    TempProject project("nugget-cache-failure");
    REQUIRE(project.made);
    ManualClock clock;
    NuggetCacheSettings settings(allocator);
    REQUIRE(settings.localPath.TryAssign("Nuggets"));
    String root(allocator);
    REQUIRE(ResolveNuggetLocalPath(settings, project.Root(), root));
    MemoryFileSystem memory(allocator, project.Root());
    REQUIRE(memory.Ready());
    NuggetCache cache(allocator, memory, clock);
    REQUIRE(cache.Open(settings, project.Root()));
    const ValueSpec spec[] = {{0, 200, 1}};
    Array<NuggetValue> values = Values(allocator, spec);
    REQUIRE(cache.Put(NuggetCachePut{NuggetBucketTexture, Key(1), values.View()}).status ==
            NuggetStatus::Stored);
    // 쓰기 실패.
    memory.faults.create = platform::FileStatus{platform::FileCode::AccessDenied, 5};
    values = Values(allocator, spec);
    NuggetCachePutResult dropped =
        cache.Put(NuggetCachePut{NuggetBucketTexture, Key(2), values.View()});
    CHECK(dropped.status == NuggetStatus::Dropped && dropped.failed);
    memory.faults.create = {};
    // 읽기 실패(새 캐시 — Memory 없음).
    char hex[Sha256Digest::HexBytes + 1];
    Key(1).ToHex(hex);
    String relative(allocator);
    REQUIRE(relative.TryAppend("Nuggets/") && relative.TryAppend(StringView(hex, 2)) &&
            relative.TryAppend("/") && relative.TryAppend(StringView(hex, 64)) &&
            relative.TryAppend(".rnug"));
    Array<uint8> good(allocator);
    REQUIRE(good.TryAppend(memory.Bytes(relative.View())));
    memory.Remove(relative.View());
    REQUIRE(memory.AddFile(relative.View(), good.View(), MemoryFault::Read));
    NuggetCache second(allocator, memory, clock);
    REQUIRE(second.Open(settings, project.Root()));
    NuggetCacheGetResult failed = second.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)});
    CHECK(failed.status == NuggetStatus::Miss && failed.failed && failed.error != ErrorCode::None);
    // 다른 버전(Unsupported)은 실패가 아니다.
    Array<uint8> newer(allocator);
    REQUIRE(newer.TryAppend(good.View()));
    newer[4] = static_cast<uint8>(newer[4] + 1);
    memory.Remove(relative.View());
    REQUIRE(memory.AddFile(relative.View(), newer.View()));
    NuggetCacheGetResult version = second.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)});
    CHECK(version.status == NuggetStatus::Miss && version.error == ErrorCode::Unsupported &&
          !version.failed);
    // 손상은 Corrupt(실패 아님 — 지운다).
    Array<uint8> broken(allocator);
    REQUIRE(broken.TryAppend(good.View()));
    broken[good.Size() - 1] ^= 1;
    memory.Remove(relative.View());
    REQUIRE(memory.AddFile(relative.View(), broken.View()));
    NuggetCacheGetResult corrupt = second.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)});
    CHECK(corrupt.status == NuggetStatus::Corrupt && !corrupt.failed &&
          !memory.Has(relative.View()) && second.Stats().corrupts == 1);
    // 다른 버전 항목 위 Put은 Dropped(Unsupported — 실패 아님, 지우지 않는다).
    REQUIRE(memory.AddFile(relative.View(), newer.View()));
    values = Values(allocator, spec);
    NuggetCachePutResult unsupported =
        second.Put(NuggetCachePut{NuggetBucketTexture, Key(1), values.View()});
    CHECK(unsupported.status == NuggetStatus::Dropped &&
          unsupported.error == ErrorCode::Unsupported && !unsupported.failed &&
          memory.Has(relative.View()));
    // 일시 경합(재게시 실패 Busy)은 실패가 아니다.
    memory.faults.move = platform::FileStatus{platform::FileCode::AlreadyExists, 80};
    values = Values(allocator, spec);
    NuggetCachePutResult busy =
        second.Put(NuggetCachePut{NuggetBucketTexture, Key(5), values.View()});
    CHECK(busy.status == NuggetStatus::Dropped && busy.error == ErrorCode::Busy && !busy.failed);
    memory.faults.move = {};
    // 한도 위반(Record 1 MiB 초과 — 행 수)은 실패가 아니다.
    const usize rows = NuggetRecordMaxBytes / NuggetValueRowBytes + 1;
    Array<NuggetValue> many(allocator);
    REQUIRE(many.TryReserve(rows));
    for (usize index = 0; index < rows; ++index) {
        NuggetValue value(allocator);
        value.id = static_cast<uint32>(index);
        REQUIRE(value.bytes.TryPushBack(static_cast<uint8>(index)));
        REQUIRE(many.TryPushBack(Move(value)));
    }

    NuggetCachePutResult capacity =
        second.Put(NuggetCachePut{NuggetBucketTexture, Key(6), many.View()});
    CHECK(capacity.status == NuggetStatus::Dropped &&
          capacity.error == ErrorCode::CapacityExceeded && !capacity.failed);
    // 분류 표.
    CHECK(!NuggetCache::IsFailure(ErrorCode::None) &&
          !NuggetCache::IsFailure(ErrorCode::Unsupported) &&
          !NuggetCache::IsFailure(ErrorCode::CapacityExceeded) &&
          !NuggetCache::IsFailure(ErrorCode::Busy) &&
          NuggetCache::IsFailure(ErrorCode::OutOfMemory) &&
          NuggetCache::IsFailure(ErrorCode::InvalidArgument) &&
          NuggetCache::IsFailure(ErrorCode::DependencyFailed));
}

// 잠금 밖 경합 창(hook — backend IO 중 같은 캐시를 다시 부른다): Get의 backend 읽기 사이에 그 키가
// 다시 만든 Record로 고정 보관되면 바깥 Get은 DigestChanged이고 고정 보관 항목은 남는다(EvictKey가
// 건너뜀), 사이에 캐시 없음으로 내려가면 바깥 Get은 Miss, 다시 만든 Record Put 둘이 겹치면 늦은
// 쪽은 고정 보관 항목을 건드리지 않는다, 보통 Put 사이에 DigestChanged가 나면 그 Put은
// Memory·고정을 남기지 않는다.
TEST(NuggetCache_Windows) {
    struct Scene {
        explicit Scene(const char* tag, usize pinnedBytes) noexcept
            : project(tag), settings(allocator), root(allocator),
              cache(allocator, files, clock, Limits(pinnedBytes)) {
            ready = project.made &&
                    static_cast<bool>(settings.localPath.TryAssign(NuggetDefaultLocalPath)) &&
                    static_cast<bool>(cache.Open(settings, project.Root())) &&
                    static_cast<bool>(ResolveNuggetLocalPath(settings, project.Root(), root));
        }

        static NuggetCacheLimits Limits(usize pinnedBytes) noexcept {
            NuggetCacheLimits limits;
            limits.memoryBytes = 0;
            limits.pinnedBytes = pinnedBytes;
            return limits;
        }

        NuggetCachePutResult Put(const Sha256Digest& key, const ValueSpec& spec) noexcept {
            Array<NuggetValue> values = Values(allocator, Span<const ValueSpec>(&spec, 1));
            return cache.Put(NuggetCachePut{NuggetBucketTexture, key, values.View()});
        }

        NuggetCacheGetResult Get(const Sha256Digest& key) noexcept {
            return cache.Get(NuggetCacheGet{NuggetBucketTexture, key});
        }

        // 디스크 판을 spec으로 바꾼다(다른 세션).
        void Replace(const Sha256Digest& key, const ValueSpec& spec) noexcept {
            NuggetFileSystemBackend other(allocator, platform::PhysicalFileSystem(), root.View(),
                                          clock);
            CHECK(other.Open());
            (void)other.Remove(key);
            const Array<uint8> bytes = Pattern(allocator, spec.size, spec.seed);
            const NuggetPutValue one[] = {{spec.id, bytes.View()}};
            CHECK(other.Put(NuggetBackendPut{NuggetBucketTexture, key, one}).status ==
                  NuggetStatus::Stored);
        }

        SystemAllocator allocator;
        TempProject project;
        ManualClock clock;
        HookFileSystem files;
        NuggetCacheSettings settings;
        String root;
        NuggetCache cache;
        bool ready = false;
    };
    struct Inner {
        explicit Inner(Scene& owner) noexcept : scene(&owner), get(owner.allocator) {
        }

        Scene* scene;
        NuggetCachePutResult put;
        NuggetCacheGetResult get;
    };
    const ValueSpec x{0, 200, 1};
    const ValueSpec y{0, 200, 2};
    const ValueSpec z{0, 200, 3};

    // (1) Get 읽기 사이에 다시 만든 Record가 고정 보관된다. 바깥 Get은 Memory 확인(빈 항목)을 지나
    // backend 읽기를 열기 직전이고, 안쪽 Put이 z를 고정 보관한 뒤 바깥 Get이 y를 읽는다.
    {
        Scene scene("nugget-window-regenerate", NuggetPinnedBytes);
        REQUIRE(scene.ready);
        REQUIRE(scene.Put(Key(1), x).status == NuggetStatus::Stored);
        scene.Replace(Key(1), y);
        REQUIRE(scene.Get(Key(1)).status == NuggetStatus::DigestChanged);
        Inner inner(scene);
        scene.files.Arm(
            ".rnug", platform::FileOpenMode::ReadExisting,
            [](void* context) noexcept {
                auto* state = static_cast<Inner*>(context);
                state->put = state->scene->Put(Key(1), ValueSpec{0, 200, 3});
            },
            &inner);
        NuggetCacheGetResult outer = scene.Get(Key(1));
        CHECK(inner.put.status == NuggetStatus::Conflict);
        CHECK(outer.status == NuggetStatus::DigestChanged && outer.values.IsEmpty());
        NuggetCacheGetResult kept = scene.Get(Key(1));
        CHECK(kept.status == NuggetStatus::Hit && kept.source == NuggetSource::Memory &&
              kept.digest == inner.put.digest);
    }

    // (2) Get 읽기 사이에 캐시 없음으로 내려간다(고정 보관 상한 초과).
    {
        Scene scene("nugget-window-uncached", 1000);
        REQUIRE(scene.ready);
        REQUIRE(scene.Put(Key(2), x).status == NuggetStatus::Stored);
        scene.Replace(Key(2), y);
        REQUIRE(scene.Get(Key(2)).status == NuggetStatus::DigestChanged);
        Inner inner(scene);
        scene.files.Arm(
            ".rnug", platform::FileOpenMode::ReadExisting,
            [](void* context) noexcept {
                auto* state = static_cast<Inner*>(context);
                state->put = state->scene->Put(Key(2), ValueSpec{0, 5000, 4});
            },
            &inner);
        NuggetCacheGetResult outer = scene.Get(Key(2));
        CHECK(inner.put.status == NuggetStatus::Conflict && scene.cache.Stats().uncachedKeys == 1);
        CHECK(outer.status == NuggetStatus::Miss && outer.error == ErrorCode::None &&
              outer.values.IsEmpty());
    }

    // (3) 다시 만든 Record Put 둘이 겹친다(늦은 쪽 판정 때 이미 Regenerated — 고정 보관 항목 유지).
    {
        Scene scene("nugget-window-double", NuggetPinnedBytes);
        REQUIRE(scene.ready);
        REQUIRE(scene.Put(Key(3), x).status == NuggetStatus::Stored);
        scene.Replace(Key(3), y);
        REQUIRE(scene.Get(Key(3)).status == NuggetStatus::DigestChanged);
        Inner inner(scene);
        scene.files.Arm(
            ".tmp", platform::FileOpenMode::CreateNew,
            [](void* context) noexcept {
                auto* state = static_cast<Inner*>(context);
                state->put = state->scene->Put(Key(3), ValueSpec{0, 200, 3});
            },
            &inner);
        NuggetCachePutResult outer = scene.Put(Key(3), z);
        CHECK(inner.put.status == NuggetStatus::Conflict &&
              outer.status == NuggetStatus::Conflict && outer.digest == inner.put.digest);
        NuggetCacheGetResult kept = scene.Get(Key(3));
        CHECK(kept.source == NuggetSource::Memory && kept.digest == inner.put.digest);
        CHECK(scene.cache.Stats().pinnedBytes ==
              NuggetMemoryEntryBytes + NuggetValueRowBytes + z.size);
    }

    // (4) 보통 Put 사이에 DigestChanged가 난다 — 그 Put은 Memory·고정을 남기지 않는다.
    {
        Scene scene("nugget-window-changed", NuggetPinnedBytes);
        REQUIRE(scene.ready);
        REQUIRE(scene.Put(Key(4), x).status == NuggetStatus::Stored);
        scene.Replace(Key(4), y);
        Inner inner(scene);
        scene.files.Arm(
            ".tmp", platform::FileOpenMode::CreateNew,
            [](void* context) noexcept {
                auto* state = static_cast<Inner*>(context);
                state->get = state->scene->Get(Key(4));
            },
            &inner);
        NuggetCachePutResult outer = scene.Put(Key(4), x);
        CHECK(inner.get.status == NuggetStatus::DigestChanged &&
              outer.status == NuggetStatus::Conflict);
        // 고정은 Changed 그대로다 — 다음 Put이 다시 만든 Record로 고정 보관한다.
        CHECK(scene.Get(Key(4)).status == NuggetStatus::DigestChanged);
        CHECK(scene.Put(Key(4), z).status == NuggetStatus::Conflict &&
              scene.Get(Key(4)).source == NuggetSource::Memory);
    }
}

// 고정 보관 경계: DigestChanged 뒤 디스크가 고정 판으로 돌아오면 적중을 받아들여 Memory에
// 넣고(고정은 Changed 그대로), 그 뒤 다시 만든 Record Put은 그 항목을 지우고 고정 보관한다, 같은
// 다시 만든 Record를 또 Put해도 고정 보관 항목은 그대로다, 항목 하나가 고정 보관 상한보다 크면 캐시
// 없음.
TEST(NuggetCache_RegenerationEdges) {
    NuggetCacheLimits limits;
    limits.memoryValueBytes = 16;
    limits.pinnedBytes = NuggetMemoryEntryBytes + NuggetValueRowBytes + 300;
    CacheScene scene("nugget-cache-regen-edges", limits);
    REQUIRE(scene.ready);
    const ValueSpec x[] = {{0, 200, 1}};
    const ValueSpec y[] = {{0, 200, 2}};
    const ValueSpec z[] = {{0, 200, 3}};
    NuggetCachePutResult original = scene.Put(Key(1), x);
    REQUIRE(original.status == NuggetStatus::Stored);
    REQUIRE(scene.RemoveEntry(Key(1)));
    REQUIRE(scene.Publish(Key(1), y) == NuggetStatus::Stored);
    REQUIRE(scene.Get(Key(1)).status == NuggetStatus::DigestChanged);
    // 디스크가 고정 판(x)으로 돌아온다.
    REQUIRE(scene.RemoveEntry(Key(1)));
    REQUIRE(scene.Publish(Key(1), x) == NuggetStatus::Stored);
    NuggetCacheGetResult back = scene.Get(Key(1));
    CHECK(back.status == NuggetStatus::Hit && back.digest == original.digest &&
          scene.cache.Stats().memoryBytes > 0);
    // 다시 만든 Record(z) — Memory 항목(x)을 지우고 고정 보관한다.
    NuggetCachePutResult regenerated = scene.Put(Key(1), z);
    CHECK(regenerated.status == NuggetStatus::Conflict);
    const usize pinnedEntry = NuggetMemoryEntryBytes + NuggetValueRowBytes + 200;
    NuggetCacheStats stats = scene.cache.Stats();
    CHECK(stats.memoryBytes == 0 && stats.pinnedBytes == pinnedEntry);
    NuggetCacheGetResult pinned = scene.Get(Key(1));
    CHECK(pinned.source == NuggetSource::Memory && pinned.digest == regenerated.digest);
    // 같은 다시 만든 Record를 또 Put해도 고정 보관 항목은 그대로다(Memory에 다시 넣지 않는다).
    CHECK(scene.Put(Key(1), z).status == NuggetStatus::Conflict);
    stats = scene.cache.Stats();
    CHECK(stats.pinnedBytes == pinnedEntry && stats.memoryBytes == 0);

    // 항목 하나가 상한보다 크다.
    const ValueSpec big[] = {{0, 400, 5}};
    const ValueSpec bigOther[] = {{0, 400, 6}};
    REQUIRE(scene.Put(Key(2), big).status == NuggetStatus::Stored);
    REQUIRE(scene.RemoveEntry(Key(2)));
    REQUIRE(scene.Publish(Key(2), bigOther) == NuggetStatus::Stored);
    REQUIRE(scene.Get(Key(2)).status == NuggetStatus::DigestChanged);
    CHECK(scene.Put(Key(2), big).status == NuggetStatus::Conflict &&
          scene.cache.Stats().uncachedKeys == 1 && scene.Get(Key(2)).status == NuggetStatus::Miss);
}

// 할당 실패(Get): Local 적중의 판정(입력·키 고정)·Memory 넣기, Memory 적중의 복사 — 실패하면 Miss
// (OutOfMemory, failed)이거나 Memory에 넣지 않은 적중이다, 실패 없는 다음 Get은 적중한다.
TEST(NuggetCache_GetAllocationFailures) {
    SystemAllocator system;
    TempProject project("nugget-cache-get-oom");
    REQUIRE(project.made);
    ManualClock clock;
    NuggetCacheSettings settings(system);
    REQUIRE(settings.localPath.TryAssign(NuggetDefaultLocalPath));
    String root(system);
    REQUIRE(ResolveNuggetLocalPath(settings, project.Root(), root));
    {
        NuggetFileSystemBackend direct(system, platform::PhysicalFileSystem(), root.View(), clock);
        REQUIRE(direct.Open());
        const Array<uint8> first = Pattern(system, 300, 1);
        const Array<uint8> second = Pattern(system, 200, 2);
        const NuggetPutValue values[] = {{0, first.View()}, {1, second.View()}};
        const NuggetInput inputs[] = {Input(system, Key(9), Key(8))};
        const uint8 meta[] = {1, 2, 3};
        REQUIRE(direct.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values, inputs, meta})
                    .status == NuggetStatus::Stored);
    }

    test::NFailAllocator failing(system);
    uint32 failures = 0;
    // Local 적중 경로.
    for (uint64 attempt = 1;; ++attempt) {
        NuggetCache cache(failing, platform::PhysicalFileSystem(), clock);
        REQUIRE(cache.Open(settings, project.Root()));
        failing.FailOnAttempt(attempt);
        NuggetCacheGetResult got = cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        NuggetCacheGetResult again = cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)});
        CHECK(again.status == NuggetStatus::Hit && again.values.Size() == 2);
        if (!failed) {
            CHECK(got.status == NuggetStatus::Hit && got.source == NuggetSource::Local &&
                  again.source == NuggetSource::Memory);
            break;
        }

        CHECK(got.status == NuggetStatus::Hit ||
              (got.status == NuggetStatus::Miss && got.error == ErrorCode::OutOfMemory &&
               got.failed));
        ++failures;
    }

    // Memory 적중 경로.
    for (uint64 attempt = 1;; ++attempt) {
        NuggetCache cache(failing, platform::PhysicalFileSystem(), clock);
        REQUIRE(cache.Open(settings, project.Root()));
        REQUIRE(cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)}).source ==
                NuggetSource::Local);
        failing.FailOnAttempt(attempt);
        NuggetCacheGetResult got = cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(got.status == NuggetStatus::Hit && got.source == NuggetSource::Memory &&
                  got.values.Size() == 2 && got.record.meta.Size() == 3 &&
                  got.record.inputs.Size() == 1);
            break;
        }

        CHECK(got.status == NuggetStatus::Miss && got.error == ErrorCode::OutOfMemory &&
              got.failed && got.values.IsEmpty());
        ++failures;
    }

    // 입력 없는 Record(Key(2)) — 첫 고정 할당이 키 고정이다.
    {
        NuggetFileSystemBackend direct(system, platform::PhysicalFileSystem(), root.View(), clock);
        REQUIRE(direct.Open());
        const Array<uint8> first = Pattern(system, 300, 3);
        const Array<uint8> second = Pattern(system, 200, 4);
        const NuggetPutValue values[] = {{0, first.View()}, {1, second.View()}};
        REQUIRE(direct.Put(NuggetBackendPut{NuggetBucketTexture, Key(2), values}).status ==
                NuggetStatus::Stored);
    }

    for (uint64 attempt = 1;; ++attempt) {
        NuggetCache cache(failing, platform::PhysicalFileSystem(), clock);
        REQUIRE(cache.Open(settings, project.Root()));
        failing.FailOnAttempt(attempt);
        NuggetCacheGetResult got = cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(2)});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(got.status == NuggetStatus::Hit);
            break;
        }

        CHECK(got.status == NuggetStatus::Hit ||
              (got.status == NuggetStatus::Miss && got.error == ErrorCode::OutOfMemory &&
               got.failed));
        ++failures;
    }

    // 값 합치기(0을 쥔 뒤 1을 얻는다) — 실패하면 합치지 않은 적중이다.
    const uint32 zero[] = {0};
    const uint32 one[] = {1};
    for (uint64 attempt = 1;; ++attempt) {
        NuggetCache cache(failing, platform::PhysicalFileSystem(), clock);
        REQUIRE(cache.Open(settings, project.Root()));
        REQUIRE(cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(2), zero}).status ==
                NuggetStatus::Hit);
        failing.FailOnAttempt(attempt);
        NuggetCacheGetResult got = cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(2), one});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        NuggetCacheGetResult again = cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(2), one});
        if (!failed) {
            CHECK(got.status == NuggetStatus::Hit && again.source == NuggetSource::Memory);
            break;
        }

        CHECK((got.status == NuggetStatus::Hit &&
               (again.source == NuggetSource::Memory || again.source == NuggetSource::Local)) ||
              (got.status == NuggetStatus::Miss && got.error == ErrorCode::OutOfMemory));
        ++failures;
    }

    CHECK(failures >= 12);
}

// 할당 실패: Get(Local·Memory)·Put·다시 만든 Record Put·GetSccMember·PutScc·Open — 실패하면 Miss·
// Dropped(OutOfMemory, failed)이거나 캐시가 조용히 쥐지 않는다(Memory는 최선 노력), 실패 없는
// 결과는 기준과 같다.
TEST(NuggetCache_AllocationFailures) {
    SystemAllocator system;
    TempProject project("nugget-cache-oom");
    REQUIRE(project.made);
    ManualClock clock;
    NuggetCacheSettings settings(system);
    REQUIRE(settings.localPath.TryAssign(NuggetDefaultLocalPath));
    String root(system);
    REQUIRE(ResolveNuggetLocalPath(settings, project.Root(), root));
    test::NFailAllocator failing(system);
    const ValueSpec spec[] = {{0, 300, 1}, {1, 200, 2}};
    const AssetId members[] = {Member(1)};
    uint32 failures = 0;

    // Open.
    for (uint64 attempt = 1;; ++attempt) {
        NuggetCache cache(failing, platform::PhysicalFileSystem(), clock);
        failing.FailOnAttempt(attempt);
        auto opened = cache.Open(settings, project.Root());
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(opened && cache.IsEnabled());
            break;
        }

        CHECK((!opened && opened.Error() == ErrorCode::OutOfMemory) ||
              (opened && !cache.IsEnabled()));
        ++failures;
    }

    // Put·Get(Local·Memory)·SCC: 매 시도 새 캐시와 새 키.
    for (uint64 attempt = 1;; ++attempt) {
        NuggetCache cache(failing, platform::PhysicalFileSystem(), clock);
        REQUIRE(cache.Open(settings, project.Root()));
        const Sha256Digest key = Key(static_cast<uint8>(attempt % 200 + 1));
        Array<NuggetValue> values = Values(system, spec);
        NuggetCachePut put{NuggetBucketScc, key, values.View()};
        // 이전 시도의 디스크 항목은 지운다(Stored 기준).
        NuggetFileSystemBackend direct(system, platform::PhysicalFileSystem(), root.View(), clock);
        REQUIRE(direct.Open());
        (void)direct.Remove(key);
        failing.FailOnAttempt(attempt);
        NuggetCachePutResult stored = cache.PutScc(members, put);
        NuggetCacheGetResult memoryHit = cache.Get(NuggetCacheGet{NuggetBucketScc, key});
        NuggetCacheGetResult member = cache.GetSccMember(NuggetBucketScc, key, members[0]);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(stored.status == NuggetStatus::Stored && memoryHit.status == NuggetStatus::Hit &&
                  memoryHit.source == NuggetSource::Memory && member.values.Size() == 2);
            break;
        }

        CHECK(stored.status == NuggetStatus::Stored ||
              (stored.status == NuggetStatus::Dropped && stored.failed &&
               stored.error == ErrorCode::OutOfMemory));
        CHECK(memoryHit.status == NuggetStatus::Hit ||
              (memoryHit.status == NuggetStatus::Miss &&
               (memoryHit.error == ErrorCode::OutOfMemory || memoryHit.error == ErrorCode::None)));
        CHECK(member.status == NuggetStatus::Hit || member.status == NuggetStatus::Miss);
        ++failures;
    }

    // DigestChanged 뒤 다시 만든 Record의 Put(고정 보관 경로) — 실패하면 캐시 없음이거나 Dropped.
    for (uint64 attempt = 1;; ++attempt) {
        NuggetCacheLimits limits;
        limits.memoryBytes = 0;
        NuggetCache cache(failing, platform::PhysicalFileSystem(), clock, limits);
        REQUIRE(cache.Open(settings, project.Root()));
        const Sha256Digest key = Key(static_cast<uint8>(attempt % 200 + 1));
        NuggetFileSystemBackend direct(system, platform::PhysicalFileSystem(), root.View(), clock);
        REQUIRE(direct.Open());
        (void)direct.Remove(key);
        Array<NuggetValue> values = Values(system, spec);
        REQUIRE(cache.Put(NuggetCachePut{NuggetBucketTexture, key, values.View()}).status ==
                NuggetStatus::Stored);
        REQUIRE(direct.Remove(key));
        const Array<uint8> other = Pattern(system, 100, 9);
        const NuggetPutValue one[] = {{0, other.View()}};
        REQUIRE(direct.Put(NuggetBackendPut{NuggetBucketTexture, key, one}).status ==
                NuggetStatus::Stored);
        REQUIRE(cache.Get(NuggetCacheGet{NuggetBucketTexture, key}).status ==
                NuggetStatus::DigestChanged);
        values = Values(system, spec);
        failing.FailOnAttempt(attempt);
        NuggetCachePutResult regenerated =
            cache.Put(NuggetCachePut{NuggetBucketTexture, key, values.View()});
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        NuggetCacheGetResult after = cache.Get(NuggetCacheGet{NuggetBucketTexture, key});
        if (!failed) {
            CHECK(regenerated.status == NuggetStatus::Conflict &&
                  after.source == NuggetSource::Memory && after.status == NuggetStatus::Hit);
            break;
        }

        CHECK(regenerated.status == NuggetStatus::Conflict ||
              (regenerated.status == NuggetStatus::Dropped &&
               regenerated.error == ErrorCode::OutOfMemory));
        // 고정 보관에 실패했으면 캐시 없음(Miss)이거나 아직 Changed(DigestChanged)다.
        CHECK(after.status == NuggetStatus::Hit || after.status == NuggetStatus::Miss ||
              after.status == NuggetStatus::DigestChanged);
        ++failures;
    }

    CHECK(failures >= 10);
}

// 동시 호출: 여러 스레드가 같은 키·다른 키를 Get·Put해도 결과가 일관된다(같은 키는 Stored 하나 +
// Exists, 이후 모든 Get이 같은 digest로 적중).
TEST(NuggetCache_Concurrency) {
    CacheScene scene("nugget-cache-threads");
    REQUIRE(scene.ready);
    constexpr usize ThreadCount = 4;
    constexpr uint32 Rounds = 20;
    struct Worker {
        CacheScene* scene = nullptr;
        uint8 seed = 0;
        uint32 stored = 0;
        uint32 exists = 0;
        uint32 hits = 0;
        uint32 bad = 0;
    };
    Worker workers[ThreadCount];
    platform::Thread threads[ThreadCount];
    auto entry = [](void* context) noexcept {
        auto* worker = static_cast<Worker*>(context);
        const ValueSpec shared[] = {{0, 400, 7}};
        for (uint32 round = 0; round < Rounds; ++round) {
            const ValueSpec own[] = {{0, 300, static_cast<uint8>(worker->seed + round)}};
            const Sha256Digest ownKey =
                Key(static_cast<uint8>(100 + worker->seed * Rounds + round));
            NuggetCachePutResult mine = worker->scene->Put(ownKey, own);
            worker->bad += mine.status == NuggetStatus::Stored ? 0 : 1;
            NuggetCachePutResult common = worker->scene->Put(Key(1), shared);
            worker->stored += common.status == NuggetStatus::Stored ? 1 : 0;
            worker->exists += common.status == NuggetStatus::Exists ? 1 : 0;
            NuggetCacheGetResult got = worker->scene->Get(Key(1));
            worker->hits += got.status == NuggetStatus::Hit && got.digest == common.digest ? 1 : 0;
            NuggetCacheGetResult back = worker->scene->Get(ownKey);
            worker->bad += back.status == NuggetStatus::Hit && back.digest == mine.digest ? 0 : 1;
        }
    };
    for (usize index = 0; index < ThreadCount; ++index) {
        workers[index].scene = &scene;
        workers[index].seed = static_cast<uint8>(index);
        REQUIRE(threads[index].Start(entry, &workers[index]) == platform::ThreadStatus::Success);
    }

    uint32 stored = 0;
    uint32 exists = 0;
    for (usize index = 0; index < ThreadCount; ++index) {
        REQUIRE(threads[index].Join() == platform::ThreadStatus::Success);
        CHECK(workers[index].bad == 0 && workers[index].hits == Rounds);
        stored += workers[index].stored;
        exists += workers[index].exists;
    }

    CHECK(stored == 1 && exists == ThreadCount * Rounds - 1);
    const NuggetCacheStats stats = scene.cache.Stats();
    CHECK(stats.conflicts == 0 && stats.dropped == 0 && stats.digestChanged == 0);
}

} // namespace nugget_cache_tests_file
