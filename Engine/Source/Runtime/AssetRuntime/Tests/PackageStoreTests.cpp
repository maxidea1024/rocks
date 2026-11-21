#include "StoreTestSupport.h"

#include <AssetRuntime/PackageLoad.h>
#include <Core/FileError.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>

// 저장 기반 시험이다(Docs/Design/AssetSystem.ko.md 5.5절 5, 7.1절 15 — A2c1): 재검증과 전체 읽기의
// 판정 일치(모든 bit 손상·자르기·IO·할당), 편집 읽기(Bulk 내용만 뺌)의 판정 관계, 지문 읽기, entry
// 하나 해제, 파일 sink, 지연 로드(Defer)와 저장소 연결, 저장하지 않은 bulk id 거부, 내용 비교.
using namespace rocks;
namespace package_store_tests_file {

using namespace rocks::asset_test;

namespace {

constexpr uint32 BitsPerByte = 8;
// 재검증 조각 크기들이다(1 byte·홀수·한 번에 다).
constexpr usize ScratchSizes[] = {1, 13, 65536};

// 손상 구간이다: 편집 읽기가 읽는 곳(Read — 전체 읽기와 같은 진단), 읽지 않는 곳(Skipped —
// Bulk 내용·섹션 사이 패딩: 편집 읽기 통과, 전체 읽기 거부), 두 읽기 모두 보지 않는 곳(Ignored —
// 모르는 섹션 내용).
enum class EditRegion {
    Read,
    Skipped,
    Ignored
};

EditRegion RegionOf(const PackageContents& contents, uint64 offset) noexcept {
    if (offset < PackageHeaderBytes || offset >= contents.header.sectionTableOffset) {
        return EditRegion::Read;
    }

    for (const PackageSectionEntry& entry : contents.sections) {
        if (offset >= entry.offset && offset < entry.offset + entry.bytes) {
            if (static_cast<uint32>(entry.kind) >
                static_cast<uint32>(PackageSectionKind::BulkIndex)) {
                return EditRegion::Ignored;
            }

            return entry.kind == PackageSectionKind::Bulk ? EditRegion::Skipped : EditRegion::Read;
        }
    }

    return EditRegion::Skipped;
}

} // namespace

// 재검증 = 전체 읽기(5.5절 5, 7.1절 15): 패키지 모양 셋(Zstd·None bulk·모르는 섹션)의 모든 byte
// 모든 bit 손상에서 VerifyPackageFile은 ReadPackage와 같은 성패·같은 진단이다(조각 13 byte로
// 훑는다). 손상 없는 파일은 모든 조각 크기에서 통과한다.
TEST(PackageVerify_EveryBitFlipMatchesFullRead) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2400));
    const Array<uint8>* const files[] = {&scene.zstd, &scene.raw, &scene.unknown};
    uint32 mismatches = 0;
    uint32 failures = 0;
    for (const Array<uint8>* file : files) {
        for (usize scratchBytes : ScratchSizes) {
            Array<uint8> scratch(env.allocator);
            REQUIRE(scratch.TryResize(scratchBytes));
            MemoryPackageRangeReader reader(file->View());
            CHECK(VerifyPackageFile(env.allocator, reader, scratch.View()));
        }

        Array<uint8> scratch(env.allocator);
        REQUIRE(scratch.TryResize(13));
        Array<uint8> bytes = Copy(env.allocator, file->View());
        for (usize offset = 0; offset < bytes.Size(); ++offset) {
            for (uint32 bit = 0; bit < BitsPerByte; ++bit) {
                bytes[offset] = static_cast<uint8>(bytes[offset] ^ (1u << bit));
                AssetDiagnostic fullFailure;
                AssetDiagnostic verifyFailure;
                auto full =
                    ReadPackage(env.allocator, bytes.View(), EditorReadOptions(), &fullFailure);
                MemoryPackageRangeReader reader(bytes.View());
                auto verified =
                    VerifyPackageFile(env.allocator, reader, scratch.View(), &verifyFailure);
                const bool same = static_cast<bool>(full) == static_cast<bool>(verified) &&
                                  (full || (full.Error() == verified.Error() &&
                                            SameFailure(fullFailure, verifyFailure)));
                failures += full ? 0 : 1;
                if (!same && mismatches++ < 5) {
                    printf("  offset %zu bit %u: full %d (%u %u) verify %d (%u %u)\n", offset, bit,
                           static_cast<int>(static_cast<bool>(full)),
                           static_cast<unsigned>(fullFailure.code), fullFailure.section,
                           static_cast<int>(static_cast<bool>(verified)),
                           static_cast<unsigned>(verifyFailure.code), verifyFailure.section);
                }

                bytes[offset] = static_cast<uint8>(bytes[offset] ^ (1u << bit));
            }
        }
    }

    CHECK(mismatches == 0);
    CHECK(failures > 0);
}

// 재검증의 자르기·늘리기·IO·할당(5.5절 5): 모든 길이로 자른 파일과 1 byte 늘린 파일은 전체 읽기와
// 같은 진단이다. n번째 범위 읽기 실패(모든 n)는 그 오류·진단 None, 할당 실패(모든 지점)는 그
// 오류·진단 None이다. 빈 scratch는 InvalidArgument(진단 없음)다.
TEST(PackageVerify_TruncationIoAndAllocation) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2401));
    Array<uint8> scratch(env.allocator);
    REQUIRE(scratch.TryResize(64));
    uint32 mismatches = 0;
    for (usize size = 0; size <= scene.zstd.Size(); ++size) {
        Array<uint8> bytes = Copy(env.allocator, scene.zstd.View().Slice(0, size));
        if (size == scene.zstd.Size()) {
            REQUIRE(bytes.TryPushBack(uint8{0}));
        }

        AssetDiagnostic fullFailure;
        AssetDiagnostic verifyFailure;
        auto full = ReadPackage(env.allocator, bytes.View(), EditorReadOptions(), &fullFailure);
        MemoryPackageRangeReader reader(bytes.View());
        auto verified = VerifyPackageFile(env.allocator, reader, scratch.View(), &verifyFailure);
        if (full || verified || !SameFailure(fullFailure, verifyFailure)) {
            ++mismatches;
        }
    }

    CHECK(mismatches == 0);
    {
        MemoryPackageRangeReader reader(scene.zstd.View());
        AssetDiagnostic failure;
        CHECK(VerifyPackageFile(env.allocator, reader, Span<uint8>(), &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::None);
    }

    uint32 ioFailures = 0;
    for (uint32 failAt = 1;; ++failAt) {
        FailingPackageReader reader(scene.zstd.View(), failAt);
        AssetDiagnostic failure;
        failure.code = AssetDiagnosticCode::CorruptPackage;
        auto verified = VerifyPackageFile(env.allocator, reader, scratch.View(), &failure);
        if (reader.Reads() < failAt) {
            CHECK(verified);
            break;
        }

        CHECK(!verified && verified.Error() == ErrorCode::Busy);
        CHECK(failure.code == AssetDiagnosticCode::None);
        ++ioFailures;
    }

    CHECK(ioFailures > 5);
    test::NFailAllocator failing(env.allocator);
    uint32 allocationFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        MemoryPackageRangeReader reader(scene.zstd.View());
        failing.FailOnAttempt(attempt);
        AssetDiagnostic failure;
        auto verified = VerifyPackageFile(failing, reader, scratch.View(), &failure);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(verified);
            break;
        }

        CHECK(!verified && verified.Error() == ErrorCode::OutOfMemory);
        CHECK(failure.code == AssetDiagnosticCode::None);
        ++allocationFailures;
    }

    CHECK(allocationFailures > 0);
}

// 편집 읽기(7.1절 15): 내용은 전체 읽기와 같고(bulk span만 비고 bulkEntries는 같다) 모든 bit
// 손상에서 읽는 구간은 같은 진단, Bulk 내용·섹션 사이 패딩은 편집 읽기 통과·전체 읽기 거부, 모르는
// 섹션 내용은 둘 다 통과다. 모든 길이 자르기는 같은 진단이다.
TEST(PackageEditRead_RelationToFullRead) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2402));
    const Array<uint8>* const files[] = {&scene.zstd, &scene.raw, &scene.unknown};
    uint32 mismatches = 0;
    uint32 regions[3] = {};
    for (const Array<uint8>* file : files) {
        auto original = ReadPackage(env.allocator, file->View(), EditorReadOptions());
        REQUIRE(original);
        {
            MemoryPackageRangeReader reader(file->View());
            auto edit = ReadPackageForEdit(env.allocator, reader, EditorReadOptions());
            REQUIRE(edit);
            const PackageContents& contents = edit.Value().contents;
            CHECK(contents.bulk.IsEmpty());
            CHECK(contents.bulkEntries.Size() == original.Value().bulkEntries.Size());
            CHECK(contents.objects.Size() == original.Value().objects.Size() &&
                  memcmp(contents.objects.Data(), original.Value().objects.Data(),
                         contents.objects.Size()) == 0);
            CHECK(contents.fingerprint.digest == original.Value().fingerprint.digest);
            CHECK(contents.readOnly == original.Value().readOnly);
        }

        Array<uint8> bytes = Copy(env.allocator, file->View());
        for (usize offset = 0; offset < bytes.Size(); ++offset) {
            const EditRegion region = RegionOf(original.Value(), offset);
            for (uint32 bit = 0; bit < BitsPerByte; ++bit) {
                bytes[offset] = static_cast<uint8>(bytes[offset] ^ (1u << bit));
                AssetDiagnostic fullFailure;
                AssetDiagnostic editFailure;
                auto full =
                    ReadPackage(env.allocator, bytes.View(), EditorReadOptions(), &fullFailure);
                MemoryPackageRangeReader reader(bytes.View());
                auto edit =
                    ReadPackageForEdit(env.allocator, reader, EditorReadOptions(), &editFailure);
                bool expected = false;
                switch (region) {
                case EditRegion::Read:
                    expected = !full && !edit && full.Error() == edit.Error() &&
                               SameFailure(fullFailure, editFailure);
                    break;
                case EditRegion::Skipped:
                    expected = !full && edit;
                    break;
                case EditRegion::Ignored:
                    expected = full && edit;
                    break;
                }

                ++regions[static_cast<uint32>(region)];
                if (!expected && mismatches++ < 5) {
                    printf("  region %u offset %zu bit %u\n", static_cast<unsigned>(region), offset,
                           bit);
                }

                bytes[offset] = static_cast<uint8>(bytes[offset] ^ (1u << bit));
            }
        }

        for (usize size = 0; size < file->Size(); ++size) {
            Array<uint8> cut = Copy(env.allocator, file->View().Slice(0, size));
            AssetDiagnostic fullFailure;
            AssetDiagnostic editFailure;
            auto full = ReadPackage(env.allocator, cut.View(), EditorReadOptions(), &fullFailure);
            MemoryPackageRangeReader reader(cut.View());
            auto edit =
                ReadPackageForEdit(env.allocator, reader, EditorReadOptions(), &editFailure);
            mismatches += (!full && !edit && SameFailure(fullFailure, editFailure)) ? 0 : 1;
        }
    }

    CHECK(mismatches == 0);
    CHECK(regions[0] > 0 && regions[1] > 0 && regions[2] > 0);
}

// 지문 읽기(5.5절 1, 7.1절 15): ReadPackage의 지문과 같다. 헤더 손상은 그 진단, 섹션 표 CRC 손상은
// CorruptPackage, 64 byte 미만 파일은 헤더 진단, n번째 범위 읽기 실패는 그 오류·진단 None, 할당
// 실패는 그 오류다.
TEST(PackageFingerprint_Read) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2403));
    auto full = ReadPackage(env.allocator, scene.zstd.View(), EditorReadOptions());
    REQUIRE(full);
    MemoryPackageRangeReader reader(scene.zstd.View());
    auto fingerprint = ReadPackageFingerprint(env.allocator, reader);
    REQUIRE(fingerprint);
    CHECK(fingerprint.Value() == full.Value().fingerprint);
    // 헤더 손상(magic)·표 CRC 손상(표 첫 byte)·짧은 파일.
    Array<uint8> header = Copy(env.allocator, scene.zstd.View());
    header[0] = static_cast<uint8>(header[0] ^ 1);
    Array<uint8> table = Copy(env.allocator, scene.zstd.View());
    const usize tableOffset = static_cast<usize>(full.Value().header.sectionTableOffset);
    table[tableOffset] = static_cast<uint8>(table[tableOffset] ^ 1);
    Array<uint8> shortFile = Copy(env.allocator, scene.zstd.View().Slice(0, 40));
    const Array<uint8>* const broken[] = {&header, &table, &shortFile};
    for (const Array<uint8>* bytes : broken) {
        AssetDiagnostic expected;
        AssetDiagnostic actual;
        CHECK(!ReadPackage(env.allocator, bytes->View(), EditorReadOptions(), &expected));
        MemoryPackageRangeReader brokenReader(bytes->View());
        CHECK(!ReadPackageFingerprint(env.allocator, brokenReader, &actual));
        CHECK(SameFailure(expected, actual));
    }

    for (uint32 failAt = 1; failAt <= 2; ++failAt) {
        FailingPackageReader failingReader(scene.zstd.View(), failAt);
        AssetDiagnostic failure;
        CHECK(ReadPackageFingerprint(env.allocator, failingReader, &failure).Error() ==
              ErrorCode::Busy);
        CHECK(failure.code == AssetDiagnosticCode::None);
    }

    test::NFailAllocator failing(env.allocator);
    failing.FailOnAttempt(1);
    MemoryPackageRangeReader again(scene.zstd.View());
    CHECK(ReadPackageFingerprint(failing, again).Error() == ErrorCode::OutOfMemory);
}

// entry 하나 해제(7.1절 15): None·Zstd entry를 풀어 SHA-256까지 확인한다. 저장·raw 크기 불일치,
// SHA-256 불일치, 깨진 컨테이너는 InvalidArgument, 할당 실패는 그 오류다.
TEST(PackageBulkEntry_Decode) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2404));
    const Array<uint8>* const files[] = {&scene.zstd, &scene.raw};
    for (const Array<uint8>* file : files) {
        auto contents = ReadPackage(env.allocator, file->View(), EditorReadOptions());
        REQUIRE(contents);
        for (const PackageBulkEntry& entry : contents.Value().bulkEntries) {
            const Span<const uint8> stored = contents.Value().bulk.Slice(
                static_cast<usize>(entry.offset), static_cast<usize>(entry.storedBytes));
            Array<uint8> raw(env.allocator);
            REQUIRE(raw.TryResize(static_cast<usize>(entry.rawBytes)));
            REQUIRE(DecodePackageBulkEntry(entry, stored, raw.View(), env.allocator));
            CHECK(Sha256::Hash(raw.View()) == entry.sha256);
            // 크기 불일치.
            CHECK(DecodePackageBulkEntry(entry, stored.Slice(0, stored.Size() - 1), raw.View(),
                                         env.allocator)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(DecodePackageBulkEntry(entry, stored, raw.View().Slice(0, raw.Size() - 1),
                                         env.allocator)
                      .Error() == ErrorCode::InvalidArgument);
            // SHA-256 불일치.
            PackageBulkEntry wrongHash = entry;
            wrongHash.sha256.bytes[0] = static_cast<uint8>(wrongHash.sha256.bytes[0] ^ 1);
            CHECK(DecodePackageBulkEntry(wrongHash, stored, raw.View(), env.allocator).Error() ==
                  ErrorCode::InvalidArgument);
            if (entry.codec == BulkCodec::Zstd) {
                Array<uint8> corrupt = Copy(env.allocator, stored);
                corrupt[0] = static_cast<uint8>(corrupt[0] ^ 1);
                CHECK(DecodePackageBulkEntry(entry, corrupt.View(), raw.View(), env.allocator)
                          .Error() == ErrorCode::InvalidArgument);
                test::NFailAllocator failing(env.allocator);
                uint32 failures = 0;
                for (uint64 attempt = 1;; ++attempt) {
                    failing.FailOnAttempt(attempt);
                    auto decoded = DecodePackageBulkEntry(entry, stored, raw.View(), failing);
                    const bool failed = failing.Attempts() >= attempt;
                    failing.DisableFailure();
                    if (!failed) {
                        CHECK(decoded);
                        break;
                    }

                    CHECK(decoded.Error() == ErrorCode::OutOfMemory);
                    ++failures;
                }

                CHECK(failures > 0);
            }
        }
    }
}

// 파일 sink(7.1절 15): WritePackage가 파일 sink로 쓴 byte는 메모리 sink와 같다. 쓴 끝보다 먼
// WriteAt은 InvalidArgument, 빈 Append는 무동작, 쓰기·이동 실패는 그 파일 오류다.
TEST(FilePackageSink_WritesSameBytes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2405));
    MemoryPackageFiles files(env.allocator);
    PackageWriteOptions options = EditorOptions();
    options.bulkSource = &scene.bulk;
    options.bulkCodec = BulkCodec::Zstd;
    {
        platform::File file;
        REQUIRE(file.Open(files, "Out.rasset", platform::FileOpenMode::CreateNew));
        FilePackageSink sink(file);
        auto written = WritePackage(env.allocator, *env.source, env.source->OwnerAccess(),
                                    scene.package.Handle(), options, sink);
        REQUIRE(written);
        CHECK(sink.Written() == written.Value().fileBytes);
        CHECK(sink.Append(Span<const uint8>()));
        const uint8 one[] = {1};
        CHECK(sink.WriteAt(sink.Written(), Span<const uint8>(one)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(sink.WriteAt(sink.Written() + 1, Span<const uint8>()).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(sink.WriteAt(0, Span<const uint8>()));
        REQUIRE(file.Close());
    }

    const Array<uint8>* stored = files.Get("Out.rasset");
    REQUIRE(stored != nullptr);
    CHECK(stored->Size() == scene.zstd.Size() &&
          memcmp(stored->Data(), scene.zstd.Data(), scene.zstd.Size()) == 0);
    const ErrorCode fileError = FileErrorCode(FakeFileError);
    const FileFault faults[] = {FileFault::Write, FileFault::Seek};
    for (FileFault fault : faults) {
        platform::File file;
        REQUIRE(file.Open(files, "Fault.rasset", platform::FileOpenMode::CreateAlways));
        FilePackageSink sink(file);
        const uint8 bytes[] = {1, 2, 3, 4};
        REQUIRE(sink.Append(Span<const uint8>(bytes)));
        files.fault = fault;
        if (fault == FileFault::Write) {
            CHECK(sink.Append(Span<const uint8>(bytes)).Error() == fileError);
            CHECK(sink.WriteAt(0, Span<const uint8>(bytes)).Error() == fileError);
        } else {
            CHECK(sink.WriteAt(0, Span<const uint8>(bytes)).Error() == fileError);
        }

        files.fault = FileFault::None;
        REQUIRE(file.Close());
    }

    // 둘째 이동(끝으로 돌아가기)만 실패한다.
    platform::File file;
    REQUIRE(file.Open(files, "Seek.rasset", platform::FileOpenMode::CreateAlways));
    FilePackageSink sink(file);
    const uint8 bytes[] = {1, 2, 3, 4};
    REQUIRE(sink.Append(Span<const uint8>(bytes)));
    files.seeks = 0;
    files.failSeekAt = 2;
    CHECK(sink.WriteAt(0, Span<const uint8>(bytes)).Error() == fileError);
    files.failSeekAt = 0;
    REQUIRE(file.Close());
}

// 지연 로드(7.1절 15): Defer + 범위 읽기 Prepare + 저장소를 건 상태는 bulk를 풀지 않고 Package가
// 저장소로 지연 읽기를 한다(같은 byte). 범위 읽기 Prepare는 Defer 전용, 두 번째 Prepare는
// InvalidArgument, SetBulkStore는 Decode·Prepare 전·null·인스턴스 뒤 InvalidArgument다. Defer를
// file Prepare로 열면 bulk 없이 열린다(BulkBytes NotFound). Bulk 내용 손상은 열기를 통과하고 그
// entry의 지연 읽기가 CorruptPackage다.
TEST(PackageLoad_DeferredBulk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2406));
    MemoryPackageFiles files(env.allocator);
    REQUIRE(files.Set("Scene.rasset", scene.zstd.View()));
    PackageFileCache cache(env.allocator, files);
    PackageLoadOptions decode = LoadOptions(PackageKind::Editor);
    PackageLoadOptions defer = decode;
    defer.bulk = PackageBulkMode::Defer;
    ObjectRegistry& registry = *env.target;
    const ObjectAccessContext access = registry.OwnerAccess();
    {
        PackageLoadState state(env.allocator, registry, env.types, access, decode);
        MemoryPackageRangeReader reader(scene.zstd.View());
        CHECK(state.Prepare(reader).Error() == ErrorCode::InvalidArgument);
    }

    auto fileReader = UniquePtr<FilePackageRangeReader>::TryMake(env.allocator);
    REQUIRE(fileReader);
    REQUIRE(fileReader.Value()->Open(files, "Scene.rasset"));
    PackageLoadState state(env.allocator, registry, env.types, access, defer);
    UniquePtr<PackageBulkStore> store;
    CHECK(state.SetBulkStore(store).Error() == ErrorCode::InvalidArgument);
    REQUIRE(state.Prepare(*fileReader.Value()));
    CHECK(state.Prepare(*fileReader.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(state.SetBulkStore(store).Error() == ErrorCode::InvalidArgument);
    auto made = UniquePtr<FilePackageBulkStore>::TryMake(env.allocator, env.allocator, cache);
    REQUIRE(made);
    const PackageContents& contents = state.Contents();
    uint64 bulkOffset = 0;
    for (const PackageSectionEntry& entry : contents.sections) {
        bulkOffset = entry.kind == PackageSectionKind::Bulk ? entry.offset : bulkOffset;
    }

    REQUIRE(made.Value()->Attach("Scene.rasset", contents.fingerprint, state.Id(), bulkOffset,
                                 contents.bulkEntries.View()));
    UniquePtr<PackageBulkStore> attached(Move(made.Value()));
    REQUIRE(state.SetBulkStore(attached));
    CHECK(attached.Get() == nullptr);
    ConstructionContext transaction(registry, access);
    PackageLoadState* states[] = {&state};
    ObjectHandle packages[1];
    REQUIRE(InstantiatePackages(transaction, states, nullptr, packages));
    RootRef<Object> roots[1];
    REQUIRE(transaction.Commit(packages, roots));
    UniquePtr<PackageBulkStore> again;
    CHECK(state.SetBulkStore(again).Error() == ErrorCode::InvalidArgument);
    Package* package = Pointer<Package>(registry, packages[0]);
    REQUIRE(package != nullptr && package->BulkStore() != nullptr);
    auto representative = registry.FindChild(access, packages[0], "");
    REQUIRE(representative);
    TestAsset* asset = Pointer<TestAsset>(registry, representative.Value());
    REQUIRE(asset != nullptr);
    const uint32 opensBefore = files.opens;
    auto pixels = package->BulkBytes(asset->pixels);
    REQUIRE(pixels);
    CHECK(pixels.Value().Size() == scene.pixels.Size() &&
          memcmp(pixels.Value().Data(), scene.pixels.Data(), scene.pixels.Size()) == 0);
    CHECK(files.opens == opensBefore + 1);
    auto again2 = package->BulkBytes(asset->pixels);
    CHECK(again2 && again2.Value().Data() == pixels.Value().Data());
    CHECK(asset->pixels.SameContent(asset->pixels));
    BulkDataRef other = asset->pixels;
    other.bulkId = 7;
    CHECK(asset->pixels.SameContent(other) && !(asset->pixels == other));
    other.rawBytes += 1;
    CHECK(!asset->pixels.SameContent(other));
}

// 손으로 만든 섹션(7.1절 15 — CRC는 맞고 내용만 다른 경우): BulkIndex 배치 위반(entry offset
// 어긋남), 더 새 schemaVersion의 BulkIndex(Required 섹션이라 NewerVersion), bulk 없는 패키지에서
// 전체 읽기·편집 읽기·재검증이 같은 판정·같은 진단이다.
TEST(PackageVerify_CraftedSections) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2410));
    auto contents = ReadPackage(env.allocator, scene.raw.View(), EditorReadOptions());
    REQUIRE(contents);
    Array<PackageBulkEntry> entries(env.allocator);
    REQUIRE(entries.TryAppend(contents.Value().bulkEntries.View()));
    entries[1].offset += contents.Value().header.bulkAlignment;
    EditablePackage layout(env.allocator);
    REQUIRE(layout.Parse(scene.raw.View()));
    REQUIRE(layout.Replace(
        PackageSectionKind::BulkIndex,
        BulkIndexBytes(env.allocator, entries.View(), static_cast<uint32>(entries.Size())).View()));
    const Array<uint8> badLayout = layout.Build();
    EditablePackage newer(env.allocator);
    REQUIRE(newer.Parse(scene.raw.View()));
    newer.Find(PackageSectionKind::BulkIndex)->schemaVersion += 1;
    const Array<uint8> newerIndex = newer.Build();
    // bulk 없는 패키지.
    ObjectRegistry& registry = *env.source;
    auto plain = NewPackage(registry, registry.OwnerAccess(), FixedId(2411), PackageKind::Editor);
    REQUIRE(plain);
    REQUIRE(!Make<TestAsset>(registry, TestAssetType(), plain.Value().Handle(), "").IsNull());
    const Array<uint8> noBulk =
        Write(env.allocator, registry, plain.Value().Handle(), EditorOptions());
    REQUIRE(!badLayout.IsEmpty() && !newerIndex.IsEmpty() && !noBulk.IsEmpty());
    // 더 새 BulkIndex는 Required 섹션이라 NewerVersion이다(5.3절).
    struct Case {
        const Array<uint8>* file;
        bool valid;
        AssetDiagnosticCode code;
    };
    const Case cases[] = {{&badLayout, false, AssetDiagnosticCode::CorruptPackage},
                          {&newerIndex, false, AssetDiagnosticCode::NewerVersion},
                          {&noBulk, true, AssetDiagnosticCode::None}};
    Array<uint8> scratch(env.allocator);
    REQUIRE(scratch.TryResize(32));
    for (const Case& one : cases) {
        AssetDiagnostic fullFailure;
        AssetDiagnostic editFailure;
        AssetDiagnostic verifyFailure;
        auto full = ReadPackage(env.allocator, one.file->View(), EditorReadOptions(), &fullFailure);
        MemoryPackageRangeReader editReader(one.file->View());
        auto edit =
            ReadPackageForEdit(env.allocator, editReader, EditorReadOptions(), &editFailure);
        MemoryPackageRangeReader verifyReader(one.file->View());
        auto verified =
            VerifyPackageFile(env.allocator, verifyReader, scratch.View(), &verifyFailure);
        CHECK(static_cast<bool>(full) == one.valid);
        CHECK(static_cast<bool>(edit) == one.valid && static_cast<bool>(verified) == one.valid);
        if (!one.valid) {
            CHECK(SameFailure(fullFailure, editFailure) && SameFailure(fullFailure, verifyFailure));
            CHECK(fullFailure.code == one.code);
            CHECK(fullFailure.section == static_cast<uint32>(PackageSectionKind::BulkIndex));
        }
    }
}

// 지연 로드의 실패(7.1절 15): 범위 읽기 Prepare의 판정 실패는 그 진단이다. Decode 상태에는 저장소를
// 걸 수 없다(InvalidArgument). BulkDataRef의 bulkId에 저장하지 않은 id 표지(최상위 bit)가 선
// 패키지는 CorruptPackage(BulkIndex, detail = 그 id)다.
TEST(PackageLoad_DeferredFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    StoreScene scene(env.allocator);
    REQUIRE(MakeStoreScene(env, scene, 2412));
    PackageLoadOptions decode = LoadOptions(PackageKind::Editor);
    PackageLoadOptions defer = decode;
    defer.bulk = PackageBulkMode::Defer;
    ObjectRegistry& registry = *env.target;
    const ObjectAccessContext access = registry.OwnerAccess();
    {
        Array<uint8> corrupt = Copy(env.allocator, scene.zstd.View());
        corrupt[0] = static_cast<uint8>(corrupt[0] ^ 1);
        PackageLoadState state(env.allocator, registry, env.types, access, defer);
        MemoryPackageRangeReader reader(corrupt.View());
        AssetDiagnostic failure;
        CHECK(!state.Prepare(reader, &failure));
        CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
    }

    {
        PackageLoadState state(env.allocator, registry, env.types, access, decode);
        REQUIRE(state.Prepare(Copy(env.allocator, scene.zstd.View())));
        MemoryPackageFiles files(env.allocator);
        PackageFileCache cache(env.allocator, files);
        auto made = UniquePtr<FilePackageBulkStore>::TryMake(env.allocator, env.allocator, cache);
        REQUIRE(made);
        UniquePtr<PackageBulkStore> store(Move(made.Value()));
        CHECK(state.SetBulkStore(store).Error() == ErrorCode::InvalidArgument);
        CHECK(store.Get() != nullptr);
    }

    const Guid bulkIdField = FieldIdOf(env.types, GeneratedType<BulkDataRef>::Id(), "bulkId");
    REQUIRE(!(bulkIdField == Guid{}));
    const uint32 unsavedId = UnsavedBulkIdBit | 1u;
    const Array<uint8> tagged = WithObjectsU32(env.allocator, scene.raw, bulkIdField, unsavedId);
    REQUIRE(!tagged.IsEmpty());
    PackageLoadState state(env.allocator, registry, env.types, access, decode);
    AssetDiagnostic failure;
    CHECK(!state.Prepare(Copy(env.allocator, tagged.View()), &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
    CHECK(failure.section == static_cast<uint32>(PackageSectionKind::BulkIndex));
    CHECK(failure.detail == unsavedId);
}

} // namespace package_store_tests_file
