#include "StoreTestSupport.h"

#include <AssetRuntime/PackageLoad.h>
#include <Core/FileError.h>
#include <TestSupport/NFailAllocator.h>

// 파일 bulk 저장소·핸들 캐시 시험이다(Docs/Design/AssetSystem 6.1절, 7.1절 15 — A2c1): 지연
// 읽기(처음 읽을 때만 열기·같은 span), ref 판정, 지연 읽기의 손상·외부 변경·IO·할당 실패, 캐시
// 상한(LRU)·지문 대조, 저장하지 않은 bulk·복사, 저장 뒤 다시 잇기(옛 ref 유지·빠진 id·이미 준 span
// 유효·잘못된 표 거부), Detach, Package에 잇기.
using namespace rocks;
namespace bulk_store_tests_file {

using namespace rocks::asset_test;

namespace {

constexpr StringView ScenePath = "Content/Scene.rasset";

// 파일의 BulkIndex·Bulk 섹션 offset·지문이다.
struct FileBulk {
    explicit FileBulk(Allocator& allocator) noexcept : entries(allocator) {
    }

    Array<PackageBulkEntry> entries;
    uint64 bulkOffset = 0;
    PackageFingerprint fingerprint{};
    AssetId asset{};
};

bool ReadFileBulk(Allocator& allocator, Span<const uint8> file, FileBulk& out) noexcept {
    auto contents = ReadPackage(allocator, file, EditorReadOptions());
    if (!contents) {
        return false;
    }

    for (const PackageSectionEntry& entry : contents.Value().sections) {
        out.bulkOffset = entry.kind == PackageSectionKind::Bulk ? entry.offset : out.bulkOffset;
    }

    out.fingerprint = contents.Value().fingerprint;
    out.asset = contents.Value().header.assetId;
    return static_cast<bool>(out.entries.TryAppend(contents.Value().bulkEntries.View()));
}

BulkDataRef RefOf(const PackageBulkEntry& entry) noexcept {
    BulkDataRef ref;
    ref.bulkId = entry.bulkId;
    ref.rawBytes = entry.rawBytes;
    ref.sha256 = entry.sha256;
    return ref;
}

// 장면 파일에 이은 저장소다.
struct AttachedStore {
    AttachedStore(AssetEnv& env, Span<const uint8> file) noexcept
        : files(env.allocator), cache(env.allocator, files), store(env.allocator, cache),
          bulk(env.allocator) {
        ready = files.Set(ScenePath, file) && ReadFileBulk(env.allocator, file, bulk) &&
                store.Attach(ScenePath, bulk.fingerprint, bulk.asset, bulk.bulkOffset,
                             bulk.entries.View());
    }

    MemoryPackageFiles files;
    PackageFileCache cache;
    FilePackageBulkStore store;
    FileBulk bulk;
    bool ready = false;
};

} // namespace

// 지연 읽기(7.1절 15): 이은 뒤 처음 읽을 때만 파일을 열고(핸들은 캐시에 남는다) 같은 ref는 같은
// span이다. 내용은 장면 원본과 같다(Zstd·None). 판정: 빈 ref InvalidArgument, 없는 id NotFound,
// 크기·SHA-256 다른 ref TypeMismatch. 다시 Attach·빈 경로 Attach는 InvalidArgument. 메모리 id와
// 파일 id는 처음에 같다(FileBulkId).
TEST(FilePackageBulkStore_LazyRead) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2500));
    const Array<uint8>* const files[] = {&scene.zstd, &scene.raw};
    for (const Array<uint8>* file : files) {
        AttachedStore attached(env, file->View());
        REQUIRE(attached.ready);
        FilePackageBulkStore& store = attached.store;
        CHECK(store.IsAttached() && store.Count() == 3);
        CHECK(attached.files.opens == 0);
        const Span<const uint8> expected[] = {scene.pixels.View(), scene.data.View(),
                                              scene.data.View()};
        for (usize index = 0; index < 3; ++index) {
            const BulkDataRef ref = RefOf(attached.bulk.entries[index]);
            auto bytes = store.Read(ref);
            REQUIRE(bytes);
            CHECK(bytes.Value().Size() == expected[index].Size() &&
                  memcmp(bytes.Value().Data(), expected[index].Data(), expected[index].Size()) ==
                      0);
            auto again = store.ReadBulk(ref);
            CHECK(again && again.Value().Data() == bytes.Value().Data());
            auto fileId = store.FileBulkId(ref);
            CHECK(fileId && fileId.Value() == ref.bulkId);
        }

        CHECK(attached.files.opens == 1 && attached.cache.OpenCount() == 1);
        BulkDataRef wrong = RefOf(attached.bulk.entries[0]);
        CHECK(store.Read(BulkDataRef{}).Error() == ErrorCode::InvalidArgument);
        wrong.bulkId = 9;
        CHECK(store.Read(wrong).Error() == ErrorCode::NotFound);
        CHECK(store.FileBulkId(wrong).Error() == ErrorCode::NotFound);
        wrong.bulkId = 1;
        wrong.rawBytes += 1;
        CHECK(store.Read(wrong).Error() == ErrorCode::TypeMismatch);
        wrong = RefOf(attached.bulk.entries[0]);
        wrong.sha256.bytes[3] = static_cast<uint8>(wrong.sha256.bytes[3] ^ 1);
        CHECK(store.Read(wrong).Error() == ErrorCode::TypeMismatch);
        CHECK(store
                  .Attach(ScenePath, attached.bulk.fingerprint, attached.bulk.asset,
                          attached.bulk.bulkOffset, attached.bulk.entries.View())
                  .Error() == ErrorCode::InvalidArgument);
        FilePackageBulkStore empty(env.allocator, attached.cache);
        CHECK(empty.Attach("", attached.bulk.fingerprint, attached.bulk.asset, 0, {}).Error() ==
              ErrorCode::InvalidArgument);
    }
}

// 지연 읽기 실패(7.1절 15): 저장 byte 손상(CRC)·풀기 실패(Zstd)·SHA-256 불일치는 그 entry만
// CorruptPackage / InvalidArgument(칸 Bulk, detail = 파일 id, 에셋 id)이고 다른 entry는 읽힌다.
// 처음 열 때 파일 지문이 로드 때와 다르면 ExternalModification / StaleHandle이다. 이미 연 핸들은
// 교체 뒤에도 이전 내용을 읽는다(POSIX 의미 rename). 파일 없음 NotFound, 열기·크기·읽기 IO 실패는
// 진단 None과 그 오류다.
TEST(FilePackageBulkStore_LazyFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2501));
    const ErrorCode fileError = FileErrorCode(FakeFileError);
    // 저장 byte 손상: 첫 entry 첫 byte.
    const Array<uint8>* const files[] = {&scene.zstd, &scene.raw};
    for (const Array<uint8>* file : files) {
        AttachedStore attached(env, file->View());
        REQUIRE(attached.ready);
        Array<uint8> corrupt = Copy(env.allocator, file->View());
        const usize at = static_cast<usize>(attached.bulk.bulkOffset);
        corrupt[at] = static_cast<uint8>(corrupt[at] ^ 1);
        // 지문은 섹션 표 CRC만 보므로 Bulk 내용 손상은 지문을 바꾸지 않는다.
        REQUIRE(attached.files.Set(ScenePath, corrupt.View()));
        AssetDiagnostic failure;
        CHECK(attached.store.Read(RefOf(attached.bulk.entries[0]), &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
        CHECK(failure.section == static_cast<uint32>(PackageSectionKind::Bulk));
        CHECK(failure.detail == 1 && failure.asset == attached.bulk.asset);
        CHECK(attached.store.ReadBulk(RefOf(attached.bulk.entries[0])).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(attached.store.Read(RefOf(attached.bulk.entries[1])));
    }

    // CRC는 맞고 SHA-256이 다른 entry 표(Attach에 준 표가 다르다): 풀기·SHA 확인에서 거부.
    {
        AttachedStore attached(env, scene.zstd.View());
        REQUIRE(attached.ready);
        FilePackageBulkStore other(env.allocator, attached.cache);
        Array<PackageBulkEntry> entries(env.allocator);
        REQUIRE(entries.TryAppend(attached.bulk.entries.View()));
        entries[0].sha256.bytes[0] = static_cast<uint8>(entries[0].sha256.bytes[0] ^ 1);
        REQUIRE(other.Attach(ScenePath, attached.bulk.fingerprint, attached.bulk.asset,
                             attached.bulk.bulkOffset, entries.View()));
        AssetDiagnostic failure;
        CHECK(other.Read(RefOf(entries[0]), &failure).Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
    }

    // 외부 변경: 처음 열 때 지문이 다르다.
    {
        AttachedStore attached(env, scene.zstd.View());
        REQUIRE(attached.ready);
        REQUIRE(attached.files.Set(ScenePath, scene.raw.View()));
        AssetDiagnostic failure;
        CHECK(attached.store.Read(RefOf(attached.bulk.entries[0]), &failure).Error() ==
              ErrorCode::StaleHandle);
        CHECK(failure.code == AssetDiagnosticCode::ExternalModification);
        CHECK(failure.asset == attached.bulk.asset && failure.detail == 1);
        CHECK(attached.cache.OpenCount() == 0);
    }

    // 이미 연 핸들은 교체 뒤에도 이전 내용이다.
    {
        AttachedStore attached(env, scene.zstd.View());
        REQUIRE(attached.ready);
        REQUIRE(attached.store.Read(RefOf(attached.bulk.entries[0])));
        REQUIRE(attached.files.Set(ScenePath, scene.raw.View()));
        auto later = attached.store.Read(RefOf(attached.bulk.entries[1]));
        CHECK(later && later.Value().Size() == scene.data.Size());
    }

    // IO: 파일 없음, 열기·크기 실패, 지문 읽기·저장 byte 읽기 실패.
    {
        AttachedStore attached(env, scene.zstd.View());
        REQUIRE(attached.ready);
        REQUIRE(attached.files.Delete(ScenePath.Data(), ScenePath.ByteCount()));
        AssetDiagnostic failure;
        CHECK(attached.store.Read(RefOf(attached.bulk.entries[0]), &failure).Error() ==
              ErrorCode::NotFound);
        CHECK(failure.code == AssetDiagnosticCode::None);
        CHECK(attached.store.ReadBulk(RefOf(attached.bulk.entries[0])).Error() ==
              ErrorCode::NotFound);
    }

    const FileFault faults[] = {FileFault::Open, FileFault::Size, FileFault::Read};
    for (FileFault fault : faults) {
        AttachedStore attached(env, scene.zstd.View());
        REQUIRE(attached.ready);
        attached.files.fault = fault;
        AssetDiagnostic failure;
        CHECK(attached.store.Read(RefOf(attached.bulk.entries[0]), &failure).Error() == fileError);
        CHECK(failure.code == AssetDiagnosticCode::None);
    }

    // 지문은 읽고 저장 byte 읽기만 실패(셋째 읽기부터 — 헤더·표 다음).
    {
        AttachedStore attached(env, scene.zstd.View());
        REQUIRE(attached.ready);
        attached.files.reads = 0;
        attached.files.failReadAt = 3;
        AssetDiagnostic failure;
        CHECK(attached.store.Read(RefOf(attached.bulk.entries[0]), &failure).Error() == fileError);
        CHECK(failure.code == AssetDiagnosticCode::None && failure.detail == 1);
    }
}

// 지연 읽기의 할당 실패(7.1절 15): 캐시 항목·지문 표·저장 byte·raw·풀기의 모든 할당 지점에서 그
// 오류이고, 실패 뒤 다시 읽으면 같은 byte다.
TEST(FilePackageBulkStore_ReadAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2502));
    MemoryPackageFiles files(env.allocator);
    REQUIRE(files.Set(ScenePath, scene.zstd.View()));
    FileBulk bulk(env.allocator);
    REQUIRE(ReadFileBulk(env.allocator, scene.zstd.View(), bulk));
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        PackageFileCache cache(failing, files);
        FilePackageBulkStore store(failing, cache);
        REQUIRE(store.Attach(ScenePath, bulk.fingerprint, bulk.asset, bulk.bulkOffset,
                             bulk.entries.View()));
        failing.FailOnAttempt(attempt);
        AssetDiagnostic failure;
        auto read = store.Read(RefOf(bulk.entries[0]), &failure);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(read);
            CHECK(read.Value().Size() == scene.pixels.Size());
            break;
        }

        CHECK(!read && read.Error() == ErrorCode::OutOfMemory);
        auto again = store.Read(RefOf(bulk.entries[0]));
        CHECK(again && again.Value().Size() == scene.pixels.Size());
        ++failures;
    }

    CHECK(failures > 0);
    // Attach의 할당 실패: 저장소는 비고 다시 Attach할 수 있다.
    failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        PackageFileCache cache(env.allocator, files);
        FilePackageBulkStore store(failing, cache);
        failing.FailOnAttempt(attempt);
        auto attached = store.Attach(ScenePath, bulk.fingerprint, bulk.asset, bulk.bulkOffset,
                                     bulk.entries.View());
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(attached);
            break;
        }

        CHECK(attached.Error() == ErrorCode::OutOfMemory);
        CHECK(!store.IsAttached() && store.Count() == 0);
        CHECK(store.Attach(ScenePath, bulk.fingerprint, bulk.asset, bulk.bulkOffset,
                           bulk.entries.View()));
        ++failures;
    }

    CHECK(failures > 0);
}

// 핸들 캐시(7.1절 15): 상한 2에서 셋째 파일을 열면 가장 오래 쓰지 않은 핸들을 닫는다(다시 쓰면 다시
// 연다). 캐시된 핸들의 지문과 다른 지문으로 찾으면 ExternalModification이고 그 핸들을 닫는다. 상한
// 0은 InvalidArgument, 없는 경로 Close는 무동작이다.
TEST(PackageFileCache_LeastRecentlyUsed) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2503));
    MemoryPackageFiles files(env.allocator);
    const StringView paths[] = {"A.rasset", "B.rasset", "C.rasset"};
    for (StringView path : paths) {
        REQUIRE(files.Set(path, scene.zstd.View()));
    }

    FileBulk bulk(env.allocator);
    REQUIRE(ReadFileBulk(env.allocator, scene.zstd.View(), bulk));
    PackageFileCache cache(env.allocator, files, 2);
    REQUIRE(cache.Acquire(paths[0], bulk.fingerprint));
    REQUIRE(cache.Acquire(paths[1], bulk.fingerprint));
    REQUIRE(cache.Acquire(paths[0], bulk.fingerprint));
    CHECK(files.opens == 2 && cache.OpenCount() == 2);
    REQUIRE(cache.Acquire(paths[2], bulk.fingerprint));
    CHECK(files.opens == 3 && cache.OpenCount() == 2);
    // B가 닫혔다(A는 방금 썼다).
    REQUIRE(cache.Acquire(paths[0], bulk.fingerprint));
    CHECK(files.opens == 3);
    REQUIRE(cache.Acquire(paths[1], bulk.fingerprint));
    CHECK(files.opens == 4);
    // 첫 항목이 더 오래된 경우: A(앞)·B(뒤)에서 B를 쓰고 C를 열면 A가 닫힌다.
    {
        PackageFileCache ordered(env.allocator, files, 2);
        REQUIRE(ordered.Acquire(paths[0], bulk.fingerprint));
        REQUIRE(ordered.Acquire(paths[1], bulk.fingerprint));
        REQUIRE(ordered.Acquire(paths[1], bulk.fingerprint));
        const uint32 before = files.opens;
        REQUIRE(ordered.Acquire(paths[2], bulk.fingerprint));
        REQUIRE(ordered.Acquire(paths[1], bulk.fingerprint));
        CHECK(files.opens == before + 1);
        REQUIRE(ordered.Acquire(paths[0], bulk.fingerprint));
        CHECK(files.opens == before + 2);
    }
    PackageFingerprint other = bulk.fingerprint;
    other.digest.bytes[0] = static_cast<uint8>(other.digest.bytes[0] ^ 1);
    AssetDiagnostic failure;
    CHECK(cache.Acquire(paths[1], other, &failure).Error() == ErrorCode::StaleHandle);
    CHECK(failure.code == AssetDiagnosticCode::ExternalModification);
    CHECK(cache.OpenCount() == 1);
    cache.Close("Missing.rasset");
    CHECK(cache.OpenCount() == 1);
    PackageFileCache none(env.allocator, files, 0);
    CHECK(none.Acquire(paths[0], bulk.fingerprint).Error() == ErrorCode::InvalidArgument);
    // 손상된 헤더는 그 진단이다.
    Array<uint8> broken = Copy(env.allocator, scene.zstd.View());
    broken[0] = static_cast<uint8>(broken[0] ^ 1);
    REQUIRE(files.Set("Broken.rasset", broken.View()));
    AssetDiagnostic header;
    CHECK(!cache.Acquire("Broken.rasset", bulk.fingerprint, &header));
    CHECK(header.code == AssetDiagnosticCode::CorruptPackage);
}

// 저장하지 않은 bulk와 복사(7.1절 15): AddUnsaved는 최상위 bit id를 차례로 주고 SHA-256을 계산한다.
// 다른 저장소에서 CopyFrom한 bulk는 새 저장하지 않은 ref다(원본 오류는 그대로). 할당 실패는 그
// 오류다.
TEST(FilePackageBulkStore_UnsavedAndCopy) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2504));
    AttachedStore attached(env, scene.zstd.View());
    REQUIRE(attached.ready);
    FilePackageBulkStore fresh(env.allocator, attached.cache);
    auto first = fresh.AddUnsaved(Copy(env.allocator, scene.data.View()));
    auto second = fresh.AddUnsaved(Copy(env.allocator, scene.pixels.View()));
    REQUIRE(first && second);
    CHECK(first.Value().bulkId == (UnsavedBulkIdBit | 1u));
    CHECK(second.Value().bulkId == (UnsavedBulkIdBit | 2u));
    CHECK(first.Value().sha256 == Sha256::Hash(scene.data.View()));
    CHECK(first.Value().rawBytes == scene.data.Size());
    auto bytes = fresh.Read(second.Value());
    CHECK(bytes && bytes.Value().Size() == scene.pixels.Size());
    auto fileId = fresh.FileBulkId(first.Value());
    CHECK(fileId && fileId.Value() == 0);
    CHECK(!fresh.IsAttached());
    // 저장하지 않은 bulk가 있는 저장소는 다시 Attach할 수 없다(새 패키지는 저장 뒤 Rebind로
    // 잇는다).
    CHECK(fresh
              .Attach(ScenePath, attached.bulk.fingerprint, attached.bulk.asset,
                      attached.bulk.bulkOffset, attached.bulk.entries.View())
              .Error() == ErrorCode::InvalidArgument);
    auto copied = fresh.CopyFrom(attached.store, RefOf(attached.bulk.entries[0]));
    REQUIRE(copied);
    CHECK(copied.Value().bulkId == (UnsavedBulkIdBit | 3u));
    CHECK(copied.Value().SameContent(RefOf(attached.bulk.entries[0])));
    BulkDataRef missing = RefOf(attached.bulk.entries[0]);
    missing.bulkId = 99;
    CHECK(fresh.CopyFrom(attached.store, missing).Error() == ErrorCode::NotFound);
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        FilePackageBulkStore store(failing, attached.cache);
        failing.FailOnAttempt(attempt);
        auto added = store.AddUnsaved(Copy(env.allocator, scene.data.View()));
        auto copy = added ? store.CopyFrom(attached.store, RefOf(attached.bulk.entries[1]))
                          : Result<BulkDataRef>::Failure(added.Error());
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(copy);
            break;
        }

        CHECK(copy.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

// 저장 뒤 다시 잇기(5.5절 7, 7.1절 15): 저장소를 bulk source로 새 파일을 쓰고 재매핑 표로
// Rebind하면 옛 ref(파일 id·저장하지 않은 id 모두)가 새 파일 entry를 읽고(FileBulkId가 새 id), 저장
// 결과에 없는 id는 표에서 빠지지만(NotFound) 이미 준 span은 유효하다. 잘못된 표(없는 옛 id·0·범위
// 밖 새 id· 내용 다른 entry)와 빈 경로는 InvalidArgument이고 저장소를 바꾸지 않는다. 할당 실패도
// 바꾸지 않는다.
TEST(FilePackageBulkStore_RebindAfterSave) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2505));
    AttachedStore attached(env, scene.zstd.View());
    REQUIRE(attached.ready);
    FilePackageBulkStore& store = attached.store;
    ObjectRegistry& registry = *env.source;
    TestAsset* asset = Pointer<TestAsset>(registry, scene.asset);
    REQUIRE(asset != nullptr);
    // 객체 필드를 저장소 id로: pixels는 파일 1, source는 새 저장하지 않은 bulk(파일 2는 빠진다),
    // Part data는 파일 3.
    const BulkDataRef oldSource = RefOf(attached.bulk.entries[1]);
    auto oldSpan = store.Read(oldSource);
    REQUIRE(oldSpan);
    Array<uint8> replacement(env.allocator);
    FillIncompressible(replacement, 80, 77);
    auto unsaved = store.AddUnsaved(Copy(env.allocator, replacement.View()));
    REQUIRE(unsaved);
    asset->pixels = RefOf(attached.bulk.entries[0]);
    asset->source = unsaved.Value();
    auto part = registry.FindChild(registry.OwnerAccess(), scene.asset, "Part");
    REQUIRE(part);
    Pointer<TestPart>(registry, part.Value())->data = BulkDataRef{};
    PackageWriteOptions options = EditorOptions();
    options.bulkSource = &store;
    options.bulkCodec = BulkCodec::Zstd;
    MemoryPackageSink sink(env.allocator);
    auto written = WritePackage(env.allocator, registry, registry.OwnerAccess(),
                                scene.package.Handle(), options, sink);
    REQUIRE(written);
    const Array<uint8> saved = sink.Take();
    REQUIRE(attached.files.Set("Content/Saved.rasset", saved.View()));
    FileBulk newBulk(env.allocator);
    REQUIRE(ReadFileBulk(env.allocator, saved.View(), newBulk));
    // 잘못된 표들.
    REQUIRE(written.Value().bulkRemap.Size() == 2);
    const BulkRemapEntry good[] = {written.Value().bulkRemap[0], written.Value().bulkRemap[1]};
    BulkRemapEntry missingOld[] = {{12345, 1}};
    BulkRemapEntry zeroNew[] = {{good[0].oldId, 0}};
    BulkRemapEntry outOfRange[] = {{good[0].oldId, 9}};
    BulkRemapEntry crossed[] = {{good[0].oldId, good[1].newId}};
    Span<const BulkRemapEntry> bad[] = {missingOld, zeroNew, outOfRange, crossed};
    for (Span<const BulkRemapEntry> remap : bad) {
        CHECK(store
                  .Rebind("Content/Saved.rasset", newBulk.fingerprint, newBulk.bulkOffset,
                          newBulk.entries.View(), remap)
                  .Error() == ErrorCode::InvalidArgument);
    }

    CHECK(store.Rebind("", newBulk.fingerprint, newBulk.bulkOffset, newBulk.entries.View(), good)
              .Error() == ErrorCode::InvalidArgument);
    // 손으로 만든 entry 표: id가 자리와 다름·크기 다름·SHA-256 다름.
    for (uint32 variant = 0; variant < 3; ++variant) {
        Array<PackageBulkEntry> crafted(env.allocator);
        REQUIRE(crafted.TryAppend(newBulk.entries.View()));
        const usize at = good[0].newId - 1;
        if (variant == 0) {
            crafted[at].bulkId += 5;
        } else if (variant == 1) {
            crafted[at].rawBytes += 1;
        } else {
            crafted[at].sha256.bytes[0] = static_cast<uint8>(crafted[at].sha256.bytes[0] ^ 1);
        }

        CHECK(store
                  .Rebind("Content/Saved.rasset", newBulk.fingerprint, newBulk.bulkOffset,
                          crafted.View(), good)
                  .Error() == ErrorCode::InvalidArgument);
    }
    auto stillOld = store.FileBulkId(oldSource);
    CHECK(stillOld && stillOld.Value() == 2);
    // 할당 실패는 바꾸지 않는다.
    test::NFailAllocator failing(env.allocator);
    {
        FilePackageBulkStore probe(failing, attached.cache);
        REQUIRE(probe.Attach(ScenePath, attached.bulk.fingerprint, attached.bulk.asset,
                             attached.bulk.bulkOffset, attached.bulk.entries.View()));
        const BulkRemapEntry identity[] = {{1, 1}, {2, 2}, {3, 3}};
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            failing.FailOnAttempt(attempt);
            auto rebound =
                probe.Rebind(ScenePath, attached.bulk.fingerprint, attached.bulk.bulkOffset,
                             attached.bulk.entries.View(), identity);
            const bool failed = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (!failed) {
                CHECK(rebound);
                break;
            }

            CHECK(rebound.Error() == ErrorCode::OutOfMemory);
            CHECK(probe.Count() == 3);
            ++failures;
        }

        CHECK(failures > 0);
    }

    REQUIRE(store.Rebind("Content/Saved.rasset", newBulk.fingerprint, newBulk.bulkOffset,
                         newBulk.entries.View(), good));
    CHECK(store.Count() == 2);
    const BulkDataRef refs[] = {asset->pixels, asset->source};
    for (const BulkDataRef& ref : refs) {
        auto bytes = store.Read(ref);
        REQUIRE(bytes);
        auto fileId = store.FileBulkId(ref);
        REQUIRE(fileId);
        CHECK(fileId.Value() >= 1 && fileId.Value() <= 2);
        CHECK(newBulk.entries[fileId.Value() - 1].sha256 == ref.sha256);
    }

    CHECK(store.Read(oldSource).Error() == ErrorCode::NotFound);
    CHECK(store.Read(RefOf(attached.bulk.entries[2])).Error() == ErrorCode::NotFound);
    // 읽지 않은 채 빠진 entry(Part data)는 Detach가 건너뛴다.
    REQUIRE(store.Detach());
    CHECK(!store.IsAttached());
    CHECK(oldSpan.Value().Size() == scene.data.Size() &&
          memcmp(oldSpan.Value().Data(), scene.data.Data(), scene.data.Size()) == 0);
    // 파일에 이은 적 없는 저장소(새 패키지 — 저장하지 않은 bulk만)도 저장 뒤 Rebind로 잇는다.
    {
        FilePackageBulkStore newOnly(env.allocator, attached.cache);
        auto first = newOnly.AddUnsaved(Copy(env.allocator, replacement.View()));
        REQUIRE(first);
        const BulkRemapEntry remap[] = {{first.Value().bulkId, good[1].newId}};
        REQUIRE(newOnly.Rebind("Content/Saved.rasset", newBulk.fingerprint, newBulk.bulkOffset,
                               newBulk.entries.View(), remap));
        CHECK(newOnly.IsAttached());
        auto fileId = newOnly.FileBulkId(first.Value());
        CHECK(fileId && fileId.Value() == good[1].newId);
    }

    // 새 파일만 남아도 아직 읽지 않은 entry를 새 파일에서 읽는다.
    REQUIRE(attached.files.Delete(ScenePath.Data(), ScenePath.ByteCount()));
    FilePackageBulkStore reread(env.allocator, attached.cache);
    REQUIRE(reread.Attach("Content/Saved.rasset", newBulk.fingerprint, newBulk.asset,
                          newBulk.bulkOffset, newBulk.entries.View()));
    CHECK(reread.Read(RefOf(newBulk.entries[0])));
}

// Detach(7.1절 6·15): 아직 읽지 않은 entry를 모두 읽어 두고 파일과의 연결을 끊는다 — 뒤에 파일을
// 지워도 읽힌다. 손상 entry가 있으면 그 오류이고 연결은 그대로다. 두 번째 Detach는 무동작이다.
TEST(FilePackageBulkStore_Detach) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2506));
    {
        AttachedStore attached(env, scene.zstd.View());
        REQUIRE(attached.ready);
        REQUIRE(attached.store.Read(RefOf(attached.bulk.entries[0])));
        REQUIRE(attached.store.Detach());
        CHECK(!attached.store.IsAttached() && attached.cache.OpenCount() == 0);
        REQUIRE(attached.files.Delete(ScenePath.Data(), ScenePath.ByteCount()));
        for (const PackageBulkEntry& entry : attached.bulk.entries) {
            CHECK(attached.store.Read(RefOf(entry)));
        }

        CHECK(attached.store.Detach());
    }

    AttachedStore attached(env, scene.raw.View());
    REQUIRE(attached.ready);
    Array<uint8> corrupt = Copy(env.allocator, scene.raw.View());
    const usize at = static_cast<usize>(attached.bulk.bulkOffset + attached.bulk.entries[2].offset);
    corrupt[at] = static_cast<uint8>(corrupt[at] ^ 1);
    REQUIRE(attached.files.Set(ScenePath, corrupt.View()));
    AssetDiagnostic failure;
    CHECK(attached.store.Detach(&failure).Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage && failure.detail == 3);
    CHECK(attached.store.IsAttached());
}

// Package에 잇기(7.1절 15): 새 패키지에 저장소를 이으면 BulkBytes가 저장소에 묻는다(진단 포함).
// null 저장소·이미 저장소가 있는 패키지·로드한 bulk가 있는 패키지는 InvalidArgument이고 저장소는
// 호출자에게 남는다.
TEST(Package_AttachBulkStore) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2507));
    MemoryPackageFiles files(env.allocator);
    PackageFileCache cache(env.allocator, files);
    ObjectRegistry& registry = *env.source;
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(2508), PackageKind::Editor);
    REQUIRE(made);
    Package* package = Pointer<Package>(registry, made.Value().Handle());
    REQUIRE(package != nullptr);
    UniquePtr<PackageBulkStore> none;
    CHECK(AttachBulkStore(*package, none).Error() == ErrorCode::InvalidArgument);
    auto store = UniquePtr<FilePackageBulkStore>::TryMake(env.allocator, env.allocator, cache);
    REQUIRE(store);
    auto added = store.Value()->AddUnsaved(Copy(env.allocator, scene.data.View()));
    REQUIRE(added);
    UniquePtr<PackageBulkStore> owned(Move(store.Value()));
    REQUIRE(AttachBulkStore(*package, owned));
    CHECK(owned.Get() == nullptr && package->BulkStore() != nullptr);
    auto bytes = package->BulkBytes(added.Value());
    CHECK(bytes && bytes.Value().Size() == scene.data.Size());
    AssetDiagnostic failure;
    CHECK(package->BulkBytes(BulkDataRef{}, &failure).Error() == ErrorCode::InvalidArgument);
    auto second = UniquePtr<FilePackageBulkStore>::TryMake(env.allocator, env.allocator, cache);
    REQUIRE(second);
    UniquePtr<PackageBulkStore> another(Move(second.Value()));
    CHECK(AttachBulkStore(*package, another).Error() == ErrorCode::InvalidArgument);
    CHECK(another.Get() != nullptr);
    // 로드한 bulk가 있는 패키지(Decode).
    Array<uint8> file = Copy(env.allocator, scene.zstd.View());
    Array<uint8> files2[] = {Move(file)};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, *env.target, files2, PackageKind::Editor, nullptr, roots, handles));
    Package* loaded = Pointer<Package>(*env.target, handles[0]);
    REQUIRE(loaded != nullptr && loaded->BulkCount() == 3);
    CHECK(AttachBulkStore(*loaded, another).Error() == ErrorCode::InvalidArgument);
}

// 다시 Attach(A2c1 리뷰 권장): 모든 bulk를 뺀 저장(빈 재매핑) → Detach 뒤 다시 Attach하면 이미 준
// span의 raw는 남는다 — Attach의 모든 할당 실패에서도 그 span이 유효하고 저장소는 비어 있다.
TEST(FilePackageBulkStore_ReattachKeepsSpans) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2509));
    MemoryPackageFiles files(env.allocator);
    REQUIRE(files.Set(ScenePath, scene.zstd.View()));
    FileBulk bulk(env.allocator);
    REQUIRE(ReadFileBulk(env.allocator, scene.zstd.View(), bulk));
    test::NFailAllocator failing(env.allocator);
    PackageFileCache cache(env.allocator, files);
    FilePackageBulkStore store(failing, cache);
    REQUIRE(store.Attach(ScenePath, bulk.fingerprint, bulk.asset, bulk.bulkOffset,
                         bulk.entries.View()));
    auto held = store.Read(RefOf(bulk.entries[0]));
    REQUIRE(held);
    REQUIRE(store.Rebind(ScenePath, bulk.fingerprint, bulk.bulkOffset, bulk.entries.View(), {}));
    CHECK(store.Count() == 0);
    REQUIRE(store.Detach());
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto attached = store.Attach(ScenePath, bulk.fingerprint, bulk.asset, bulk.bulkOffset,
                                     bulk.entries.View());
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        CHECK(memcmp(held.Value().Data(), scene.pixels.Data(), scene.pixels.Size()) == 0);
        if (!failed) {
            REQUIRE(attached);
            break;
        }

        CHECK(attached.Error() == ErrorCode::OutOfMemory);
        CHECK(!store.IsAttached() && store.Count() == 0);
        ++failures;
    }

    CHECK(failures > 0);
    auto again = store.Read(RefOf(bulk.entries[0]));
    CHECK(again && again.Value().Size() == scene.pixels.Size());
}

// 핸들 정리(A2c1 리뷰 권장): 저장소가 소멸하면 자기 경로의 캐시 핸들을 닫는다. CloseUnder는 그
// 디렉터리 아래(구성 요소 경계) 핸들만 닫는다.
TEST(PackageFileCache_CloseOnStoreDestructionAndUnder) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2510));
    MemoryPackageFiles files(env.allocator);
    const StringView paths[] = {"Content/A/One.rasset", "Content/A/Sub/Two.rasset",
                                "Content/AB/Three.rasset"};
    for (StringView path : paths) {
        REQUIRE(files.Set(path, scene.zstd.View()));
    }

    FileBulk bulk(env.allocator);
    REQUIRE(ReadFileBulk(env.allocator, scene.zstd.View(), bulk));
    PackageFileCache cache(env.allocator, files);
    {
        FilePackageBulkStore store(env.allocator, cache);
        REQUIRE(store.Attach(paths[0], bulk.fingerprint, bulk.asset, bulk.bulkOffset,
                             bulk.entries.View()));
        REQUIRE(store.Read(RefOf(bulk.entries[0])));
        CHECK(cache.OpenCount() == 1);
    }

    CHECK(cache.OpenCount() == 0);
    for (StringView path : paths) {
        REQUIRE(cache.Acquire(path, bulk.fingerprint));
    }

    cache.CloseUnder("Content/A");
    CHECK(cache.OpenCount() == 1);
    cache.CloseUnder("Content/AB/Three.rasset");
    CHECK(cache.OpenCount() == 1);
    cache.CloseUnder("Other");
    CHECK(cache.OpenCount() == 1);
    cache.CloseUnder("Content");
    CHECK(cache.OpenCount() == 0);
}

// 손상된 표의 큰 할당(A2c1 리뷰 권장): Zstd entry의 rawBytes가 컨테이너와 다르면(아주 큼) raw를
// 잡기 전에 CorruptPackage다(할당 실패가 아니다).
TEST(FilePackageBulkStore_HugeRawBytesRejectedBeforeAllocation) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2511));
    AttachedStore attached(env, scene.zstd.View());
    REQUIRE(attached.ready);
    Array<PackageBulkEntry> entries(env.allocator);
    REQUIRE(entries.TryAppend(attached.bulk.entries.View()));
    REQUIRE(entries[0].codec == BulkCodec::Zstd);
    entries[0].rawBytes = uint64{1} << 44;
    FilePackageBulkStore store(env.allocator, attached.cache);
    REQUIRE(store.Attach(ScenePath, attached.bulk.fingerprint, attached.bulk.asset,
                         attached.bulk.bulkOffset, entries.View()));
    AssetDiagnostic failure;
    CHECK(store.Read(RefOf(entries[0]), &failure).Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
}

// 2단계 다시 잇기(7.1절 15 A2c2b): PrepareRebind는 Rebind와 같은 검사(잘못된 표·빈 경로
// InvalidArgument)를 하고 저장소를 바꾸지 않으며(실패한 plan은 준비되지 않음), 모든 할당 실패도
// 저장소를 바꾸지 않는다. CommitRebind 뒤 옛 ref는 새 파일 entry를 읽고 재매핑에 없는 id(빠진 파일
// entry·저장하지 않은 bulk)는 NotFound이며(이미 준 span은 유효) plan은 준비되지 않은 상태다. 실패한
// 준비 뒤 같은 plan을 다시 준비할 수 있다.
TEST(FilePackageBulkStore_TwoPhaseRebind) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2506));
    AttachedStore attached(env, scene.zstd.View());
    REQUIRE(attached.ready);
    FilePackageBulkStore& store = attached.store;
    ObjectRegistry& registry = *env.source;
    TestAsset* asset = Pointer<TestAsset>(registry, scene.asset);
    REQUIRE(asset != nullptr);
    asset->pixels = RefOf(attached.bulk.entries[0]);
    asset->source = RefOf(attached.bulk.entries[1]);
    auto part = registry.FindChild(registry.OwnerAccess(), scene.asset, "Part");
    REQUIRE(part);
    Pointer<TestPart>(registry, part.Value())->data = BulkDataRef{};
    PackageWriteOptions options = EditorOptions();
    options.bulkSource = &store;
    MemoryPackageSink sink(env.allocator);
    auto written = WritePackage(env.allocator, registry, registry.OwnerAccess(),
                                scene.package.Handle(), options, sink);
    REQUIRE(written);
    const Array<uint8> saved = sink.Take();
    REQUIRE(attached.files.Set("Content/Saved.rasset", saved.View()));
    FileBulk newBulk(env.allocator);
    REQUIRE(ReadFileBulk(env.allocator, saved.View(), newBulk));
    const Span<const BulkRemapEntry> remap = written.Value().bulkRemap.View();

    // 잘못된 준비는 저장소를 바꾸지 않고 plan은 준비되지 않는다.
    BulkRebindPlan plan(env.allocator);
    const BulkRemapEntry missing[] = {{12345, 1}};
    CHECK(store
              .PrepareRebind(plan, "Content/Saved.rasset", newBulk.fingerprint, newBulk.bulkOffset,
                             newBulk.entries.View(), missing)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(!plan.IsReady());
    CHECK(store
              .PrepareRebind(plan, "", newBulk.fingerprint, newBulk.bulkOffset,
                             newBulk.entries.View(), remap)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(!plan.IsReady());
    auto before = store.FileBulkId(RefOf(attached.bulk.entries[2]));
    CHECK(before && before.Value() == 3);

    // 준비의 모든 할당 실패도 바꾸지 않는다.
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        BulkRebindPlan probe(failing);
        failing.FailOnAttempt(attempt);
        auto prepared = store.PrepareRebind(probe, "Content/Saved.rasset", newBulk.fingerprint,
                                            newBulk.bulkOffset, newBulk.entries.View(), remap);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(prepared && probe.IsReady());
            break;
        }

        CHECK(prepared.Error() == ErrorCode::OutOfMemory && !probe.IsReady());
        CHECK(store.Count() == 3);
        ++failures;
    }

    CHECK(failures > 0);

    // 준비 → 확정(그 사이 저장소를 바꾸지 않는다 — 계약). 준비 전에 더한 저장하지 않은 bulk는
    // 재매핑에 없으니 표에서 빠진다(저장 결과에 없는 id와 같다 — 이미 준 span은 유효).
    Array<uint8> extra(env.allocator);
    FillIncompressible(extra, 40, 9);
    auto added = store.AddUnsaved(Copy(env.allocator, extra.View()));
    REQUIRE(added);
    auto addedBytes = store.Read(added.Value());
    REQUIRE(addedBytes);
    REQUIRE(store.PrepareRebind(plan, "Content/Saved.rasset", newBulk.fingerprint,
                                newBulk.bulkOffset, newBulk.entries.View(), remap));
    CHECK(plan.IsReady() && store.Count() == 4);
    store.CommitRebind(plan);
    CHECK(!plan.IsReady());
    CHECK(store.Count() == 2);
    auto pixels = store.Read(asset->pixels);
    REQUIRE(pixels);
    CHECK(pixels.Value().Size() == scene.pixels.Size() &&
          memcmp(pixels.Value().Data(), scene.pixels.Data(), scene.pixels.Size()) == 0);
    CHECK(store.Read(RefOf(attached.bulk.entries[2])).Error() == ErrorCode::NotFound);
    CHECK(store.Read(added.Value()).Error() == ErrorCode::NotFound);
    CHECK(addedBytes.Value().Size() == extra.Size());
    auto fileId = store.FileBulkId(asset->source);
    CHECK(fileId && fileId.Value() >= 1);
}

} // namespace bulk_store_tests_file
