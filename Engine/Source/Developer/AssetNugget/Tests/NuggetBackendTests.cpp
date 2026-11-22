#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"

#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/Endian.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Process.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// 파일 시스템 에셋 너겟 backend 시험이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 7–9, "A3b 구현
// 세부"): 왕복(저장 압축 판단·값 선택·partial·skipData·skipMeta), Exists·Conflict, 손상(위치마다)
// 삭제·교체, 다른 버전은 손대지 않음, echo, Touch·Remove·GC(시각·용량·순서·이름 거름·지난 임시
// 파일·잡힌 잠금), 읽기 전용, IO 실패 주입, 같은 프로세스 경합(hook), 두 프로세스 경합(pipe gate),
// 할당 실패.
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace nugget_backend_tests_file {

using rocks::asset_database_test::MemoryFault;
using rocks::asset_database_test::MemoryFileSystem;
using rocks::asset_database_test::TempProject;

namespace {

constexpr int64 Day = 24ll * 60 * 60 * platform::FileTicksPerSecond;
// 시험 시각의 기준이다(2026-11-22 근처 — 파일 시각으로 설정할 수 있는 범위 안).
constexpr int64 BaseTicks = 639000000000000000ll;

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
        key.bytes[index] = static_cast<uint8>(seed * 7 + index);
    }

    return key;
}

// 결정적 byte 열이다. compressible이면 반복 무늬, 아니면 LCG(압축되지 않는다).
Array<uint8> Bytes(Allocator& allocator, usize size, uint32 seed, bool compressible) noexcept {
    Array<uint8> bytes(allocator);
    CHECK(bytes.TryResize(size));
    uint32 state = seed * 2654435761u + 1;
    for (usize index = 0; index < size; ++index) {
        if (compressible) {
            bytes[index] = static_cast<uint8>((index / 64 + seed) & 0x0f);
        } else {
            state = state * 1664525u + 1013904223u;
            bytes[index] = static_cast<uint8>(state >> 24);
        }
    }

    return bytes;
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

// 루트 아래 key의 항목 경로다.
String EntryPath(Allocator& allocator, StringView root, const Sha256Digest& key) noexcept {
    char hex[Sha256Digest::HexBytes + 1];
    key.ToHex(hex);
    String path(allocator);
    CHECK(path.TryAppend(root) && path.TryAppend("/") && path.TryAppend(StringView(hex, 2)) &&
          path.TryAppend("/") && path.TryAppend(StringView(hex, 64)) && path.TryAppend(".rnug"));
    return path;
}

Array<uint8> ReadAll(Allocator& allocator, platform::FileSystem& files, StringView path) noexcept {
    Array<uint8> bytes(allocator);
    platform::File file;
    uint64 size = 0;
    if (file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting) &&
        file.Size(size) && bytes.TryResize(static_cast<usize>(size))) {
        CHECK(file.ReadExact(bytes.Data(), bytes.Size()).status);
    }

    (void)file.Close();
    return bytes;
}

bool WriteAll(platform::FileSystem& files, StringView path, Span<const uint8> bytes) noexcept {
    platform::File file;
    const bool ok = static_cast<bool>(file.Open(files, path.Data(), path.ByteCount(),
                                                platform::FileOpenMode::CreateAlways)) &&
                    static_cast<bool>(file.WriteAll(bytes.Data(), bytes.Size()).status);
    return static_cast<bool>(file.Close()) && ok;
}

bool Exists(platform::FileSystem& files, StringView path) noexcept {
    bool exists = false;
    return files.Exists(path.Data(), path.ByteCount(), exists) && exists;
}

// tmp 안의 파일 수다.
usize TemporaryCount(Allocator& allocator, platform::FileSystem& files, StringView root) noexcept {
    String directory(allocator);
    CHECK(directory.TryAppend(root) && directory.TryAppend("/tmp"));
    usize count = 0;
    (void)files.EnumerateDirectory(
        directory.View().Data(), directory.View().ByteCount(),
        [](const platform::DirectoryEntry& entry, void* context) noexcept {
            const StringView name(entry.name, entry.nameBytes);
            *static_cast<usize*>(context) +=
                name.EndsWith(".tmp", StringComparison::CaseSensitive) ? 1 : 0;
            return true;
        },
        &count);
    return count;
}

// 값 셋(작은 값·압축되는 큰 값·압축되지 않는 큰 값)의 Put이다.
struct Values {
    explicit Values(Allocator& allocator, uint32 seed = 1) noexcept
        : small(Bytes(allocator, 100, seed, true)), packed(Bytes(allocator, 300000, seed, true)),
          noisy(Bytes(allocator, NuggetStoreCompressBytes + 10, seed, false)),
          list{{0, small.View()}, {4, packed.View()}, {9, noisy.View()}} {
    }

    Array<uint8> small;
    Array<uint8> packed;
    Array<uint8> noisy;
    NuggetPutValue list[3];
};

} // namespace

// 왕복: Stored, Hit(값 byte·digest·Record — 저장 압축은 압축되는 큰 값만 Rcbk), 값 선택·partial·
// 없는 값(Miss)·skipData·skipMeta, 없는 키 Miss(None), bucket만 다르면 Miss(InvalidArgument)·항목
// 유지, 임시 파일 없음, 1 MiB 넘는 값(조각 표).
TEST(NuggetBackend_RoundTrip) {
    SystemAllocator allocator;
    TempProject project("nugget-roundtrip");
    REQUIRE(project.made);
    ManualClock clock;
    NuggetFileSystemBackend backend(allocator, platform::PhysicalFileSystem(), project.Root(),
                                    clock);
    REQUIRE(backend.Open());
    CHECK(!backend.IsReadOnly());
    Values values(allocator);
    const uint8 meta[] = {7, 8};
    NuggetInput input(allocator);
    REQUIRE(input.bucket.TryAssign("scc"));
    input.key = Key(50);
    const NuggetInput inputs[] = {Move(input)};
    NuggetBackendPut put{NuggetBucketTexture, Key(1), values.list, inputs, meta};
    const NuggetPutResult stored = backend.Put(put);
    REQUIRE(stored.status == NuggetStatus::Stored);
    CHECK(stored.error == ErrorCode::None &&
          TemporaryCount(allocator, platform::PhysicalFileSystem(), project.Root()) == 0);

    NuggetGetResult hit = backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)});
    REQUIRE(hit.status == NuggetStatus::Hit && hit.values.Size() == 3);
    CHECK(hit.digest == stored.digest && hit.error == ErrorCode::None &&
          hit.record.meta.Size() == 2 && hit.record.inputs.Size() == 1);
    CHECK(Same(hit.values[0].bytes.View(), values.small.View()) &&
          Same(hit.values[1].bytes.View(), values.packed.View()) &&
          Same(hit.values[2].bytes.View(), values.noisy.View()));
    CHECK(hit.record.values[0].encoding == NuggetEncoding::Identity &&
          hit.record.values[1].encoding == NuggetEncoding::Rcbk &&
          hit.record.values[2].encoding == NuggetEncoding::Identity &&
          hit.record.values[1].storedSize < values.packed.Size());

    // 값 선택·partial·없는 값.
    const uint32 one[] = {9};
    NuggetGetResult chosen = backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1), one});
    REQUIRE(chosen.status == NuggetStatus::Hit && chosen.values.Size() == 1);
    CHECK(chosen.values[0].id == 9 && Same(chosen.values[0].bytes.View(), values.noisy.View()));
    const uint32 withMissing[] = {0, 3};
    CHECK(backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1), withMissing}).status ==
          NuggetStatus::Miss);
    NuggetBackendGet partial{NuggetBucketTexture, Key(1), withMissing};
    partial.partial = true;
    NuggetGetResult part = backend.Get(partial);
    CHECK(part.status == NuggetStatus::Partial && part.values.Size() == 1 &&
          part.values[0].id == 0);
    // 청한 순서대로 돌려준다(이진 검색 — 행 순서와 무관), 모든 행보다 큰 ValueId는 없음.
    const uint32 reversed[] = {9, 0, 10};
    NuggetBackendGet descending{NuggetBucketTexture, Key(1), reversed};
    descending.partial = true;
    NuggetGetResult back = backend.Get(descending);
    CHECK(back.status == NuggetStatus::Partial && back.values.Size() == 2 &&
          back.values[0].id == 9 && back.values[1].id == 0);
    // 같은 ValueId를 두 번 청하면 Miss(InvalidArgument)다 — partial이어도, 없는 값이어도.
    const uint32 twice[] = {9, 0, 9};
    const uint32 absentTwice[] = {3, 3};
    const Span<const uint32> repeats[] = {Span<const uint32>(twice),
                                          Span<const uint32>(absentTwice)};
    const bool partialModes[] = {false, true};
    for (Span<const uint32> ids : repeats) {
        for (bool allowPartial : partialModes) {
            NuggetBackendGet repeated{NuggetBucketTexture, Key(1), ids};
            repeated.partial = allowPartial;
            NuggetGetResult got = backend.Get(repeated);
            CHECK(got.status == NuggetStatus::Miss && got.error == ErrorCode::InvalidArgument &&
                  got.values.IsEmpty());
        }
    }

    CHECK(backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)}).status == NuggetStatus::Hit);
    NuggetBackendGet skip{NuggetBucketTexture, Key(1)};
    skip.skipData = true;
    skip.skipMeta = true;
    NuggetGetResult bare = backend.Get(skip);
    CHECK(bare.status == NuggetStatus::Hit && bare.values.IsEmpty() && bare.record.meta.IsEmpty() &&
          bare.record.values.Size() == 3);

    // 없는 키·다른 bucket.
    NuggetGetResult none = backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(2)});
    CHECK(none.status == NuggetStatus::Miss && none.error == ErrorCode::None);
    NuggetGetResult other = backend.Get(NuggetBackendGet{NuggetBucketShader, Key(1)});
    CHECK(other.status == NuggetStatus::Miss && other.error == ErrorCode::InvalidArgument);
    const String path = EntryPath(allocator, project.Root(), Key(1));
    CHECK(Exists(platform::PhysicalFileSystem(), path.View()));

    // 1 MiB 넘는 값.
    const Array<uint8> large = Bytes(allocator, NuggetChunkBytes * 2 + 3, 5, false);
    const NuggetPutValue big[] = {{0, large.View()}};
    REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketShader, Key(3), big}).status ==
            NuggetStatus::Stored);
    NuggetGetResult bigHit = backend.Get(NuggetBackendGet{NuggetBucketShader, Key(3)});
    REQUIRE(bigHit.status == NuggetStatus::Hit);
    CHECK(Same(bigHit.values[0].bytes.View(), large.View()) &&
          !(bigHit.record.values[0].chunkTableHash == Sha256Digest{}));

    // 정규 형식 위반 Put은 Dropped(InvalidArgument).
    const NuggetPutValue backwards[] = {{2, values.small.View()}, {1, values.small.View()}};
    const NuggetPutResult dropped =
        backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(4), backwards});
    CHECK(dropped.status == NuggetStatus::Dropped && dropped.error == ErrorCode::InvalidArgument);
    CHECK(backend.Put(NuggetBackendPut{"Bad", Key(4), values.list}).status ==
          NuggetStatus::Dropped);
}

// 같은 키: 같은 Record Exists(기존 digest·Touch), 다른 Record Conflict(기존 유지·기존 digest), 임시
// 파일 없음.
TEST(NuggetBackend_ExistsAndConflict) {
    SystemAllocator allocator;
    TempProject project("nugget-exists");
    REQUIRE(project.made);
    ManualClock clock;
    NuggetFileSystemBackend backend(allocator, platform::PhysicalFileSystem(), project.Root(),
                                    clock);
    REQUIRE(backend.Open());
    Values first(allocator, 1);
    const NuggetPutResult stored =
        backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), first.list});
    REQUIRE(stored.status == NuggetStatus::Stored);
    const String path = EntryPath(allocator, project.Root(), Key(1));
    platform::FileInfo before;
    REQUIRE(
        platform::PhysicalFileSystem().Stat(path.View().Data(), path.View().ByteCount(), before));

    clock.now += Day;
    const NuggetPutResult again =
        backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), first.list});
    CHECK(again.status == NuggetStatus::Exists && again.existing == stored.digest &&
          again.digest == stored.digest);
    platform::FileInfo touched;
    REQUIRE(
        platform::PhysicalFileSystem().Stat(path.View().Data(), path.View().ByteCount(), touched));
    CHECK(touched.modifiedTicks == clock.now);

    Values second(allocator, 2);
    const NuggetPutResult conflict =
        backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), second.list});
    CHECK(conflict.status == NuggetStatus::Conflict && conflict.existing == stored.digest &&
          !(conflict.digest == stored.digest));
    NuggetGetResult hit = backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)});
    CHECK(hit.status == NuggetStatus::Hit && hit.digest == stored.digest);
    CHECK(TemporaryCount(allocator, platform::PhysicalFileSystem(), project.Root()) == 0);
}

// 손상: 머리·Record·값·조각 표 위치를 바꾼 항목은 Get이 Corrupt로 지운다. 손상 항목 위의 Put은
// 교체(Stored)다. 다른 버전 항목은 Get Miss(Unsupported)·Put Dropped(Unsupported)이고 지우지
// 않는다. 다른 키의 파일이 그 자리에 있으면(echo key) 손상이다.
TEST(NuggetBackend_CorruptionAndVersions) {
    SystemAllocator allocator;
    TempProject project("nugget-corrupt");
    REQUIRE(project.made);
    ManualClock clock;
    platform::FileSystem& files = platform::PhysicalFileSystem();
    NuggetFileSystemBackend backend(allocator, files, project.Root(), clock);
    REQUIRE(backend.Open());
    Values values(allocator);
    REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values.list}).status ==
            NuggetStatus::Stored);
    const String path = EntryPath(allocator, project.Root(), Key(1));
    const Array<uint8> good = ReadAll(allocator, files, path.View());
    REQUIRE(good.Size() > 1000);
    const usize positions[] = {0, 20, 60, 200, good.Size() / 2, good.Size() - 1};
    for (usize at : positions) {
        Array<uint8> broken(allocator);
        REQUIRE(broken.TryAppend(good.View()));
        broken[at] ^= 0x01;
        REQUIRE(WriteAll(files, path.View(), broken.View()));
        NuggetGetResult got = backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)});
        CHECK(got.status == NuggetStatus::Corrupt && got.error == ErrorCode::InvalidArgument &&
              got.values.IsEmpty() && !Exists(files, path.View()));
    }

    // 손상 항목 위의 Put은 교체한다.
    Array<uint8> broken(allocator);
    REQUIRE(broken.TryAppend(good.View()));
    broken[good.Size() - 1] ^= 0x01;
    REQUIRE(WriteAll(files, path.View(), broken.View()));
    const NuggetPutResult replaced =
        backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values.list});
    CHECK(replaced.status == NuggetStatus::Stored &&
          Same(ReadAll(allocator, files, path.View()).View(), good.View()));

    // 다른 버전(파일 버전 칸): 손대지 않는다.
    Array<uint8> newer(allocator);
    REQUIRE(newer.TryAppend(good.View()));
    StoreLittleEndian<uint32>(newer.Data() + 4, NuggetFileVersion + 1);
    REQUIRE(WriteAll(files, path.View(), newer.View()));
    NuggetGetResult got = backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)});
    CHECK(got.status == NuggetStatus::Miss && got.error == ErrorCode::Unsupported);
    const NuggetPutResult kept =
        backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), values.list});
    CHECK(kept.status == NuggetStatus::Dropped && kept.error == ErrorCode::Unsupported &&
          Same(ReadAll(allocator, files, path.View()).View(), newer.View()));

    // 다른 키의 파일이 이 키 자리에 있다.
    REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(2), values.list}).status ==
            NuggetStatus::Stored);
    const String otherPath = EntryPath(allocator, project.Root(), Key(2));
    REQUIRE(WriteAll(files, path.View(), ReadAll(allocator, files, otherPath.View()).View()));
    CHECK(backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)}).status ==
          NuggetStatus::Corrupt);
    CHECK(!Exists(files, path.View()) && TemporaryCount(allocator, files, project.Root()) == 0);
}

// Touch(지금 시각)·Remove(있음 true·없음 false), Get 적중은 Touch, touch 끈 Get은 시각을 두며, GC는
// unusedDays보다 오래 쓰지 않은 항목을 지우고 남은 합이 maxBytes를 넘으면 오래된 것부터(같은
// 시각이면 경로 순) 지우며, 항목이 아닌 이름은 건드리지 않고, 지난 임시 파일·잡히지 않은 지난
// 잠금은 지우고 젊은 것·잡힌 잠금은 둔다.
TEST(NuggetBackend_TouchRemoveAndCollect) {
    SystemAllocator allocator;
    TempProject project("nugget-collect");
    REQUIRE(project.made);
    ManualClock clock;
    platform::FileSystem& files = platform::PhysicalFileSystem();
    NuggetFileSystemBackend backend(allocator, files, project.Root(), clock);
    REQUIRE(backend.Open());
    const Array<uint8> payload = Bytes(allocator, 1000, 3, false);
    const NuggetPutValue value[] = {{0, payload.View()}};
    for (uint8 seed = 1; seed <= 5; ++seed) {
        REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(seed), value}).status ==
                NuggetStatus::Stored);
    }

    auto modified = [&](uint8 seed) noexcept {
        const String path = EntryPath(allocator, project.Root(), Key(seed));
        platform::FileInfo info;
        CHECK(files.Stat(path.View().Data(), path.View().ByteCount(), info));
        return info.modifiedTicks;
    };
    auto setModified = [&](uint8 seed, int64 ticks) noexcept {
        const String path = EntryPath(allocator, project.Root(), Key(seed));
        CHECK(files.SetModifiedTime(path.View().Data(), path.View().ByteCount(), ticks));
    };

    // Touch·Get Touch·touch 끔.
    setModified(1, BaseTicks - 10 * Day);
    clock.now = BaseTicks + Day;
    REQUIRE(backend.Touch(Key(1)));
    CHECK(modified(1) == clock.now);
    CHECK(backend.Touch(Key(9)).Error() == ErrorCode::NotFound);
    setModified(2, BaseTicks - 3 * Day);
    NuggetBackendGet quiet{NuggetBucketTexture, Key(2)};
    quiet.touch = false;
    REQUIRE(backend.Get(quiet).status == NuggetStatus::Hit);
    CHECK(modified(2) == BaseTicks - 3 * Day);
    REQUIRE(backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(2)}).status == NuggetStatus::Hit);
    CHECK(modified(2) == clock.now);

    // Remove.
    auto removed = backend.Remove(Key(5));
    REQUIRE(removed);
    CHECK(removed.Value() && !backend.Remove(Key(5)).Value());

    // GC: 1(지금)·2(지금)·3(20일 전)·4(5일 전 — 같은 시각 둘 아님).
    clock.now = BaseTicks + 30 * Day;
    setModified(1, clock.now);
    setModified(2, clock.now);
    setModified(3, clock.now - 20 * Day);
    setModified(4, clock.now - 5 * Day);
    // 항목이 아닌 이름들(16진이 아닌 두 글자 디렉터리 포함).
    const char* strayDirectories[] = {"/-1", "/AB", "/a_"};
    for (const char* name : strayDirectories) {
        String path(allocator);
        REQUIRE(path.TryAppend(project.Root()) && path.TryAppend(name));
        REQUIRE(files.MakeDirectory(path.View().Data(), path.View().ByteCount()));
    }

    String text(allocator);
    REQUIRE(text.TryAppend(project.Root()) && text.TryAppend("/readme.txt"));
    REQUIRE(WriteAll(files, text.View(), payload.View()));
    String zz(allocator);
    REQUIRE(zz.TryAppend(project.Root()) && zz.TryAppend("/zz"));
    REQUIRE(files.MakeDirectory(zz.View().Data(), zz.View().ByteCount()));
    String stray(allocator);
    REQUIRE(stray.TryAppend(zz.View()) && stray.TryAppend("/stray.rnug"));
    REQUIRE(WriteAll(files, stray.View(), payload.View()));
    // 지난·젊은 임시 파일, 지난 잠금(잡히지 않음·잡힘 — 잠금 파일은 지우지 않는다).
    String oldTemp(allocator);
    String youngTemp(allocator);
    String freeLock(allocator);
    String heldLock(allocator);
    REQUIRE(oldTemp.TryAppend(project.Root()) && oldTemp.TryAppend("/tmp/old.tmp") &&
            youngTemp.TryAppend(project.Root()) && youngTemp.TryAppend("/tmp/young.tmp") &&
            freeLock.TryAppend(project.Root()) && freeLock.TryAppend("/tmp/free.lock") &&
            heldLock.TryAppend(project.Root()) && heldLock.TryAppend("/tmp/held.lock"));
    REQUIRE(WriteAll(files, oldTemp.View(), payload.View()) &&
            WriteAll(files, youngTemp.View(), payload.View()) &&
            WriteAll(files, freeLock.View(), payload.View()));
    // tmp 안의 지난 다른 이름·디렉터리는 건드리지 않는다.
    String note(allocator);
    String sub(allocator);
    REQUIRE(note.TryAppend(project.Root()) && note.TryAppend("/tmp/note.txt") &&
            sub.TryAppend(project.Root()) && sub.TryAppend("/tmp/sub.tmp"));
    REQUIRE(WriteAll(files, note.View(), payload.View()) &&
            files.MakeDirectory(sub.View().Data(), sub.View().ByteCount()));
    platform::FileLock holder;
    REQUIRE(holder.Acquire(heldLock.View().Data(), heldLock.View().ByteCount(), 0));
    const int64 stale = clock.now - NuggetTemporaryLifetimeTicks - 1;
    REQUIRE(files.SetModifiedTime(note.View().Data(), note.View().ByteCount(), stale) &&
            files.SetModifiedTime(oldTemp.View().Data(), oldTemp.View().ByteCount(), stale) &&
            files.SetModifiedTime(freeLock.View().Data(), freeLock.View().ByteCount(), stale) &&
            files.SetModifiedTime(heldLock.View().Data(), heldLock.View().ByteCount(), stale));

    // unusedDays 10: 3(20일)만 지운다.
    auto first = backend.Collect(NuggetCollectOptions{~uint64{0}, 10});
    REQUIRE(first);
    CHECK(first.Value().removed == 1 && first.Value().kept == 3 && first.Value().skipped == 0 &&
          first.Value().temporaries == 1);
    CHECK(!Exists(files, EntryPath(allocator, project.Root(), Key(3)).View()));
    CHECK(Exists(files, text.View()));
    CHECK(Exists(files, stray.View()));
    CHECK(!Exists(files, oldTemp.View()));
    CHECK(Exists(files, youngTemp.View()));
    CHECK(Exists(files, freeLock.View()));
    CHECK(Exists(files, heldLock.View()));
    CHECK(Exists(files, note.View()));
    // Exists는 디렉터리에 거짓이다 — Stat으로 본다.
    platform::FileInfo subInfo;
    CHECK(files.Stat(sub.View().Data(), sub.View().ByteCount(), subInfo) &&
          subInfo.type == platform::FileType::Directory);
    holder.Release();

    // maxBytes: 남은 셋 중 오래된 4부터, 그다음 같은 시각 1·2 중 경로 순 앞의 것.
    const uint64 entryBytes = first.Value().keptBytes / 3;
    auto second = backend.Collect(NuggetCollectOptions{entryBytes, 365});
    REQUIRE(second);
    CHECK(second.Value().removed == 2 && second.Value().kept == 1 &&
          second.Value().keptBytes == entryBytes);
    CHECK(!Exists(files, EntryPath(allocator, project.Root(), Key(4)).View()));
    const String path1 = EntryPath(allocator, project.Root(), Key(1));
    const String path2 = EntryPath(allocator, project.Root(), Key(2));
    const bool firstEarlier = StringView::CompareBytes(path1.View(), path2.View()) < 0;
    CHECK(Exists(files, (firstEarlier ? path2 : path1).View()) &&
          !Exists(files, (firstEarlier ? path1 : path2).View()));
}

// 열기·상태 규칙: 빈 루트 InvalidArgument, 루트 자리가 파일이면 열기 실패, 열기 전 Get·Put·Collect,
// 읽기 전용 루트(tmp를 만들 수 없음 — Get은 되고 Put은 Dropped Unsupported).
TEST(NuggetBackend_OpenAndReadOnly) {
    SystemAllocator allocator;
    TempProject project("nugget-open");
    REQUIRE(project.made);
    ManualClock clock;
    platform::FileSystem& physical = platform::PhysicalFileSystem();
    {
        NuggetFileSystemBackend empty(allocator, physical, "", clock);
        CHECK(empty.Open().Error() == ErrorCode::InvalidArgument);
    }

    String fileRoot(allocator);
    REQUIRE(fileRoot.TryAppend(project.Root()) && fileRoot.TryAppend("/file"));
    const uint8 one[] = {1};
    REQUIRE(WriteAll(physical, fileRoot.View(), one));
    {
        NuggetFileSystemBackend onFile(allocator, physical, fileRoot.View(), clock);
        CHECK(!onFile.Open());
    }

    NuggetFileSystemBackend closed(allocator, physical, project.Root(), clock);
    CHECK(closed.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)}).error ==
          ErrorCode::InvalidArgument);
    const NuggetPutValue value[] = {{0, one}};
    CHECK(closed.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), value}).error ==
          ErrorCode::InvalidArgument);
    CHECK(closed.Collect({}).Error() == ErrorCode::InvalidArgument);

    // 메모리 파일 시스템: 항목을 쓴 뒤 디렉터리를 만들 수 없게 하면 읽기 전용이다.
    MemoryFileSystem memory(allocator, project.Root());
    REQUIRE(memory.Ready());
    {
        NuggetFileSystemBackend writer(allocator, memory, project.Root(), clock);
        REQUIRE(writer.Open());
        REQUIRE(writer.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), value}).status ==
                NuggetStatus::Stored);
    }

    memory.Remove("tmp");
    memory.faults.makeDirectory = platform::FileStatus{platform::FileCode::AccessDenied, 5};
    NuggetFileSystemBackend reader(allocator, memory, project.Root(), clock);
    REQUIRE(reader.Open());
    CHECK(reader.IsReadOnly());
    CHECK(reader.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)}).status == NuggetStatus::Hit);
    const NuggetPutResult dropped =
        reader.Put(NuggetBackendPut{NuggetBucketTexture, Key(2), value});
    CHECK(dropped.status == NuggetStatus::Dropped && dropped.error == ErrorCode::Unsupported);
}

// 시스템 시계는 지금 시각(파일 시각 범위 안)이다. 기록의 RawHash가 틀린 파일(해시가 맞는 머리·
// Record·값 — 쓰는 쪽 결함)은 해제 검증이 잡아 Corrupt로 지운다.
TEST(NuggetBackend_SystemClockAndWrongRawHash) {
    NuggetSystemClock system;
    const int64 now = system.NowTicks();
    CHECK(now > platform::FileTicksSettableAfter && now <= platform::FileTicksSettableLast);

    SystemAllocator allocator;
    TempProject project("nugget-rawhash");
    REQUIRE(project.made);
    ManualClock clock;
    platform::FileSystem& files = platform::PhysicalFileSystem();
    NuggetFileSystemBackend backend(allocator, files, project.Root(), clock);
    REQUIRE(backend.Open());
    const Array<uint8> raw = Bytes(allocator, 64, 1, true);
    NuggetStoredValue value;
    value.id = 0;
    value.rawSize = raw.Size();
    value.rawHash = Sha256::Hash(raw.View());
    value.stored = raw.View();
    // Identity는 ContentHash = RawHash라 행 검사가 막는다 — Rcbk로 RawHash만 틀리게 만든다.
    const Array<uint8> big = Bytes(allocator, 300000, 2, true);
    const NuggetPutValue put[] = {{0, big.View()}};
    REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, Key(1), put}).status ==
            NuggetStatus::Stored);
    const String path = EntryPath(allocator, project.Root(), Key(1));
    NuggetMemoryReader unused(raw.View());
    (void)unused;
    Array<uint8> file = ReadAll(allocator, files, path.View());
    NuggetMemoryReader reader(file.View());
    auto view = OpenNuggetFile(allocator, reader);
    REQUIRE(view && view.Value().record.values[0].encoding == NuggetEncoding::Rcbk);
    Array<uint8> stored(allocator);
    REQUIRE(ReadNuggetValue(view.Value(), reader, 0, stored));
    NuggetStoredValue wrong;
    wrong.id = 0;
    wrong.encoding = NuggetEncoding::Rcbk;
    wrong.rawSize = big.Size();
    wrong.rawHash = Sha256::Hash(raw.View());
    wrong.indexLength = view.Value().record.values[0].indexLength;
    wrong.indexHash = view.Value().record.values[0].indexHash;
    wrong.stored = stored.View();
    const NuggetStoredValue values[] = {wrong};
    auto plan = PlanNuggetFile(allocator, NuggetBucketTexture, Key(1), values, {}, {});
    REQUIRE(plan);
    Array<uint8> broken(allocator);
    REQUIRE(AppendNuggetFile(plan.Value(), values, broken));
    REQUIRE(WriteAll(files, path.View(), broken.View()));
    NuggetGetResult got = backend.Get(NuggetBackendGet{NuggetBucketTexture, Key(1)});
    CHECK(got.status == NuggetStatus::Corrupt && !Exists(files, path.View()));
}

} // namespace nugget_backend_tests_file
