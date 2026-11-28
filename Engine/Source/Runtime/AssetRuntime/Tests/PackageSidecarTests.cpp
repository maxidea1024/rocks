#include "ManifestTestSupport.h"

#include <AssetRuntime/PackageLoad.h>
#include <AssetRuntime/PackageRangeReader.h>

// 스트리밍 bulk(sidecar) 형식 시험이다(Docs/Design/AssetSystem 20.1 3·4·7, 20.4 형식): Sidecar
// 항목만(Bulk 섹션 없음)·섞임·없음, 두 공간의 정렬·증가(4096·64), sidecar 머리·여백·본문·블록 해시,
// 두 번 쓰면 같은 byte, 쓰기 거부(에디터 패키지·출력 없음), 읽기 거부(섹션
// 존재·크기·hashBlockBytes· hashCount·fileBytes·codec·flags·에디터 패키지·옛 판),
// ReadSidecarHeader의 칸마다 손상, Prepare가 sidecar를 풀지 않고 예산에서 뺌, DecodePackageBulk의
// streamed 자리, Package::BulkBytes의 Unsupported, 매니페스트 v3 칸(왕복·0 byte 규칙)과 패키지
// 대조(sidecar 칸·판정 순서).
using namespace rocks;
namespace package_sidecar_tests_file {

using namespace rocks::asset_test;

namespace {

constexpr uint32 SidecarFlag = static_cast<uint32>(BulkEntryFlags::Sidecar);
constexpr usize PixelBytes = 5000;
constexpr usize PartBytes = 300;
constexpr uint64 SceneId = 1900;
// Sidecar 섹션 고정 앞부분의 칸 위치다(PackageFormat.h SidecarSectionFixedBytes 주석).
constexpr usize SectionShaOffset = 8;
constexpr usize SectionBlockBytesOffset = 40;
constexpr usize SectionCountOffset = 44;

// 고정 장면(섞임 — 쿠킹 Zstd) 출력의 SHA-256이다(AssetSidecar_GoldenBytes).
constexpr StringView GoldenPackageDigest =
    "d394b0531f57549dd4331eea8943e3b8576e4e16a06ee012b4fd7d472b404cb3";
constexpr StringView GoldenSidecarDigest =
    "d7a79cdf683f7177c6b075f1ffc9c1e51f09d9f1604c309b63aaa598f2bbd2b1";

// 대표 에셋(pixels bulk)과 하위 객체 Body(data bulk) 하나의 패키지다.
struct Scene {
    explicit Scene(Allocator& allocator) noexcept : pixels(allocator), part(allocator) {
    }

    RootRef<Package> package;
    ObjectHandle asset{};
    ObjectHandle body{};
    Array<uint8> pixels;
    Array<uint8> part;
    TestBulkSource bulk;
};

bool BuildScene(ObjectRegistry& registry, Scene& scene, PackageBulkPlacement pixels,
                PackageBulkPlacement part, uint64 id = SceneId,
                PackageKind kind = PackageKind::Cooked) noexcept {
    auto package = NewPackage(registry, registry.OwnerAccess(), FixedId(id), kind);
    if (!package) {
        return false;
    }

    scene.package = Move(package.Value());
    scene.asset = Make<TestAsset>(registry, TestAssetType(), scene.package.Handle(), "");
    scene.body = Make<TestPart>(registry, TestPartType(), scene.asset, "Body");
    TestAsset* asset = Pointer<TestAsset>(registry, scene.asset);
    TestPart* body = Pointer<TestPart>(registry, scene.body);
    if (asset == nullptr || body == nullptr) {
        return false;
    }

    FillCompressible(scene.pixels, PixelBytes, 3);
    FillCompressible(scene.part, PartBytes, 9);
    asset->pixels = scene.bulk.Add(4, scene.pixels.View(), pixels);
    body->data = scene.bulk.Add(7, scene.part.View(), part);
    return true;
}

// 패키지와 sidecar 출력이다.
struct Written {
    explicit Written(Allocator& allocator) noexcept
        : package(allocator), sidecar(allocator), result(allocator) {
    }

    Array<uint8> package;
    Array<uint8> sidecar;
    PackageWriteResult result;
    ErrorCode error = ErrorCode::None;
};

Written WriteScene(AssetEnv& env, Scene& scene, PackageWriteOptions options,
                   bool withSidecar = true) noexcept {
    Written out(env.allocator);
    MemoryPackageSink sink(env.allocator);
    MemoryPackageSink sidecar(env.allocator);
    options.bulkSource = &scene.bulk;
    options.sidecar = withSidecar ? &sidecar : nullptr;
    auto written = WritePackage(env.allocator, *env.source, env.source->OwnerAccess(),
                                scene.package.Handle(), options, sink);
    if (!written) {
        out.error = written.Error();
        return out;
    }

    out.result = Move(written.Value());
    out.package = sink.Take();
    out.sidecar = sidecar.Take();
    return out;
}

Result<PackageContents> Read(AssetEnv& env, Span<const uint8> file,
                             PackageKind kind = PackageKind::Cooked) noexcept {
    PackageReadOptions options;
    options.expectedKind = kind;
    options.types = &env.types;
    options.engineVersion = TestEngineVersion;
    return ReadPackage(env.allocator, file, options);
}

// file을 읽으면 CorruptPackage / CorruptData다(빈 file은 만들기 실패 — 거부로 치지 않는다).
bool Rejected(AssetEnv& env, const Array<uint8>& file,
              PackageKind kind = PackageKind::Cooked) noexcept {
    if (file.IsEmpty()) {
        return false;
    }

    PackageReadOptions options;
    options.expectedKind = kind;
    options.types = &env.types;
    options.engineVersion = TestEngineVersion;
    AssetDiagnostic failure;
    auto read = ReadPackage(env.allocator, file.View(), options, &failure);
    return !read && read.Error() == ErrorCode::CorruptData &&
           failure.code == AssetDiagnosticCode::CorruptPackage;
}

uint64 AlignTo(uint64 value, uint64 alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

// sidecar byte의 머리·정렬 여백·entry 본문·블록 해시가 쓴 결과·패키지 섹션과 맞다.
bool SidecarMatches(const Written& written, const PackageContents& contents,
                    Span<const Span<const uint8>> raws) noexcept {
    const Span<const uint8> file = written.sidecar.View();
    const SidecarHeaderExpectation expected = ExpectedSidecarHeader(contents);
    bool ok = contents.sidecarPresent && file.Size() == contents.sidecar.fileBytes &&
              written.result.sidecarBytes == file.Size() &&
              written.result.sidecarSha256 == Sha256::Hash(file) &&
              contents.sidecar.sha256 == written.result.sidecarSha256 &&
              contents.sidecar.hashBlockBytes == SidecarHashBlockBytes &&
              expected.entryCount == raws.Size() &&
              ReadSidecarHeader(file.Slice(0, SidecarHeaderBytes), expected).IsOk();
    // 블록 해시: 64 KiB 단위, 마지막은 짧다.
    const uint64 blocks = (file.Size() + SidecarHashBlockBytes - 1) / SidecarHashBlockBytes;
    ok = ok && contents.sidecar.blockHashes.Size() == blocks;
    for (uint64 block = 0; ok && block < blocks; ++block) {
        const uint64 start = block * SidecarHashBlockBytes;
        const uint64 end = start + SidecarHashBlockBytes < file.Size()
                               ? start + SidecarHashBlockBytes
                               : file.Size();
        ok = contents.sidecar.blockHashes[static_cast<usize>(block)] ==
             Sha256::Hash(file.Slice(static_cast<usize>(start), static_cast<usize>(end - start)));
    }

    // entry 본문은 raw 그대로, 그 사이(머리 뒤 포함)는 0이다.
    uint64 cursor = SidecarHeaderBytes;
    usize raw = 0;
    for (const PackageBulkEntry& entry : contents.bulkEntries) {
        if (!ok || (entry.flags & SidecarFlag) == 0) {
            continue;
        }

        for (uint64 at = cursor; ok && at < entry.offset; ++at) {
            ok = file[static_cast<usize>(at)] == 0;
        }

        ok = ok && raw < raws.Size() && entry.codec == BulkCodec::None &&
             entry.storedBytes == entry.rawBytes && entry.crc32 == Crc32(raws[raw]) &&
             SameBytes(
                 file.Slice(static_cast<usize>(entry.offset), static_cast<usize>(entry.rawBytes)),
                 raws[raw]);
        cursor = entry.offset + entry.rawBytes;
        ++raw;
    }

    return ok && raw == raws.Size() && cursor == file.Size();
}

// Sidecar 섹션 byte를 change로 고친 패키지다(섹션을 writer 규칙대로 다시 배치한다).
template <class Change>
Array<uint8> WithSidecarSection(AssetEnv& env, const Array<uint8>& file, Change change) noexcept {
    EditablePackage edit(env.allocator);
    if (!edit.Parse(file.View())) {
        return Array<uint8>(env.allocator);
    }

    EditableSection* section = edit.Find(PackageSectionKind::Sidecar);
    if (section == nullptr) {
        return Array<uint8>(env.allocator);
    }

    change(section->bytes);
    return edit.Build();
}

// BulkIndex 항목을 change로 고친 패키지다(codecOverride가 0이 아니면 모든 항목의 codec 칸).
template <class Change>
Array<uint8> WithBulkIndex(AssetEnv& env, const Array<uint8>& file, Change change,
                           uint32 codecOverride = 0,
                           PackageKind kind = PackageKind::Cooked) noexcept {
    auto contents = Read(env, file.View(), kind);
    EditablePackage edit(env.allocator);
    if (!contents || !edit.Parse(file.View())) {
        return Array<uint8>(env.allocator);
    }

    Array<PackageBulkEntry> entries = CopyEntries(env.allocator, contents.Value().bulkEntries);
    change(entries);
    const Array<uint8> index = BulkIndexBytes(env.allocator, entries.View(),
                                              static_cast<uint32>(entries.Size()), codecOverride);
    if (!edit.Replace(PackageSectionKind::BulkIndex, index.View())) {
        return Array<uint8>(env.allocator);
    }

    return edit.Build();
}

// 헤더 formatVersion을 바꾸고 headerCrc를 다시 맞춘 byte다.
Array<uint8> WithFormatVersion(Allocator& allocator, const Array<uint8>& file,
                               uint16 version) noexcept {
    Array<uint8> copy = Copy(allocator, file.View());
    StoreLittleEndian<uint16>(copy.Data() + 4, version);
    StoreLittleEndian<uint32>(copy.Data() + 60, Crc32(Span<const uint8>(copy.Data(), 60)));
    return copy;
}

} // namespace

// Sidecar 항목만: Bulk 섹션이 없고 BulkIndex·Sidecar 섹션만 있다. 두 entry가 머리 뒤 4096 정렬로
// 이어지고(4096·12288), sidecar 끝 = fileBytes, 머리·여백 0·본문·블록 해시가 맞다. 쓰기 결과의
// sidecarBytes·sidecarSha256이 파일과 섹션과 같다.
TEST(AssetSidecar_SidecarOnlyLayout) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Sidecar));
    Written written = WriteScene(env, scene, CookedOptions());
    REQUIRE(written.error == ErrorCode::None);
    auto contents = Read(env, written.package.View());
    REQUIRE(contents);
    const PackageContents& read = contents.Value();
    CHECK(!read.bulkSectionPresent && read.sidecarPresent && read.bulk.IsEmpty());
    REQUIRE(read.bulkEntries.Size() == 2);
    const uint64 first = AlignTo(SidecarHeaderBytes, PackageDefaultCookedBulkAlignment);
    const uint64 second = AlignTo(first + PixelBytes, PackageDefaultCookedBulkAlignment);
    CHECK(first == 4096 && second == 12288);
    CHECK(read.bulkEntries[0].flags == SidecarFlag && read.bulkEntries[0].offset == first);
    CHECK(read.bulkEntries[1].flags == SidecarFlag && read.bulkEntries[1].offset == second);
    CHECK(read.sidecar.fileBytes == second + PartBytes);
    const Span<const uint8> raws[] = {scene.pixels.View(), scene.part.View()};
    CHECK(SidecarMatches(written, read, raws));
    EditablePackage edit(env.allocator);
    REQUIRE(edit.Parse(written.package.View()));
    CHECK(edit.Find(PackageSectionKind::Bulk) == nullptr);
    CHECK(edit.Find(PackageSectionKind::BulkIndex) != nullptr);
    CHECK(edit.Find(PackageSectionKind::Sidecar) != nullptr);
    CHECK(written.result.fileBytes == written.package.Size());
}

// 섞임(쿠킹 Zstd): 패키지 안 entry는 Zstd로 Bulk 섹션 0에서, sidecar entry는 codec None으로 머리 뒤
// 정렬 자리에서 시작한다(두 공간이 따로 센다). 정렬 64면 두 Sidecar entry가 64·5120이다.
TEST(AssetSidecar_MixedAndAlignment) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package));
    PackageWriteOptions zstd = CookedOptions();
    zstd.bulkCodec = BulkCodec::Zstd;
    Written mixed = WriteScene(env, scene, zstd);
    REQUIRE(mixed.error == ErrorCode::None);
    auto contents = Read(env, mixed.package.View());
    REQUIRE(contents);
    const PackageContents& read = contents.Value();
    CHECK(read.bulkSectionPresent && read.sidecarPresent);
    REQUIRE(read.bulkEntries.Size() == 2);
    CHECK(read.bulkEntries[0].flags == SidecarFlag);
    CHECK(read.bulkEntries[0].codec == BulkCodec::None);
    CHECK(read.bulkEntries[0].offset == PackageDefaultCookedBulkAlignment);
    CHECK(read.bulkEntries[1].flags == 0 && read.bulkEntries[1].codec == BulkCodec::Zstd);
    CHECK(read.bulkEntries[1].offset == 0);
    CHECK(read.bulk.Size() == read.bulkEntries[1].storedBytes);
    const Span<const uint8> pixels[] = {scene.pixels.View()};
    CHECK(SidecarMatches(mixed, read, pixels));

    Scene aligned(env.allocator);
    REQUIRE(BuildScene(*env.source, aligned, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Sidecar, SceneId + 1));
    PackageWriteOptions narrow = CookedOptions();
    narrow.bulkAlignment = 64;
    Written both = WriteScene(env, aligned, narrow);
    REQUIRE(both.error == ErrorCode::None);
    auto narrowRead = Read(env, both.package.View());
    REQUIRE(narrowRead);
    REQUIRE(narrowRead.Value().bulkEntries.Size() == 2);
    CHECK(narrowRead.Value().bulkEntries[0].offset == 64);
    CHECK(narrowRead.Value().bulkEntries[1].offset == AlignTo(64 + PixelBytes, 64));
    const Span<const uint8> raws[] = {aligned.pixels.View(), aligned.part.View()};
    CHECK(SidecarMatches(both, narrowRead.Value(), raws));
}

// 고정 byte(AS 20.4 형식 golden): 섞인 장면(쿠킹 Zstd)의 패키지와 sidecar 파일 SHA-256이 고정
// 값이다. 값이 바뀌면 형식(또는 Zstd 출력)이 바뀐 것이다 — 의도한 변경이면 판 규칙(5.3절)을 따르고
// 여기 값을 함께 고친다.
TEST(AssetSidecar_GoldenBytes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package));
    PackageWriteOptions zstd = CookedOptions();
    zstd.bulkCodec = BulkCodec::Zstd;
    Written written = WriteScene(env, scene, zstd);
    REQUIRE(written.error == ErrorCode::None);
    auto same = [](Span<const uint8> bytes, StringView expected) noexcept {
        char hex[Sha256Digest::HexBytes + 1];
        Sha256::Hash(bytes).ToHex(hex);
        if (StringView(hex, Sha256Digest::HexBytes) == expected) {
            return true;
        }

        test::RecordFailure(hex, __FILE__, __LINE__);
        return false;
    };
    CHECK(same(written.package.View(), GoldenPackageDigest));
    CHECK(same(written.sidecar.View(), GoldenSidecarDigest));
}

// 없음: sidecar 출력을 줘도 Sidecar bulk가 없으면 아무것도 쓰지 않고(결과 0·SHA 0) Sidecar 섹션도
// 없다. 출력 없이 쓴 패키지와 byte가 같다. 두 번 쓰면 패키지·sidecar byte가 같다(결정적).
TEST(AssetSidecar_AbsentAndDeterministic) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Package,
                       PackageBulkPlacement::Package));
    Written with = WriteScene(env, scene, CookedOptions());
    Written without = WriteScene(env, scene, CookedOptions(), false);
    REQUIRE(with.error == ErrorCode::None && without.error == ErrorCode::None);
    CHECK(with.sidecar.IsEmpty() && with.result.sidecarBytes == 0);
    CHECK(with.result.sidecarSha256 == Sha256Digest{});
    CHECK(SameBytes(with.package.View(), without.package.View()));
    auto contents = Read(env, with.package.View());
    REQUIRE(contents);
    CHECK(contents.Value().bulkSectionPresent && !contents.Value().sidecarPresent);
    const SidecarHeaderExpectation none = ExpectedSidecarHeader(contents.Value());
    CHECK(none.entryCount == 0 && none.fileBytes == 0);

    Scene streamed(env.allocator);
    REQUIRE(BuildScene(*env.source, streamed, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package, SceneId + 1));
    Written first = WriteScene(env, streamed, CookedOptions());
    Written second = WriteScene(env, streamed, CookedOptions());
    REQUIRE(first.error == ErrorCode::None && second.error == ErrorCode::None);
    CHECK(SameBytes(first.package.View(), second.package.View()));
    CHECK(SameBytes(first.sidecar.View(), second.sidecar.View()));
    CHECK(first.result.sidecarSha256 == second.result.sidecarSha256);
}

// 쓰기 거부(AS 20.1 1): 에디터 패키지의 Sidecar 배치, 쿠킹인데 sidecar 출력 없음은
// InvalidArgument다. 배치가 모두 Package면 출력이 없어도 된다. rawBytes 0인 bulk는 Sidecar를 골라도
// 패키지 안에 둔다(정렬 64에서도 sidecar를 쓰지 않는다 — fileBytes > 머리 규칙, 20.1 4).
TEST(AssetSidecar_WriterRejections) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene editor(env.allocator);
    REQUIRE(BuildScene(*env.source, editor, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package, SceneId, PackageKind::Editor));
    CHECK(WriteScene(env, editor, EditorOptions()).error == ErrorCode::InvalidArgument);
    CHECK(WriteScene(env, editor, EditorOptions(), false).error == ErrorCode::InvalidArgument);

    Scene cooked(env.allocator);
    REQUIRE(BuildScene(*env.source, cooked, PackageBulkPlacement::Package,
                       PackageBulkPlacement::Sidecar, SceneId + 1));
    CHECK(WriteScene(env, cooked, CookedOptions(), false).error == ErrorCode::InvalidArgument);
    CHECK(WriteScene(env, cooked, CookedOptions()).error == ErrorCode::None);

    Scene empty(env.allocator);
    REQUIRE(BuildScene(*env.source, empty, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package, SceneId + 2));
    TestAsset* asset = Pointer<TestAsset>(*env.source, empty.asset);
    REQUIRE(asset != nullptr);
    asset->pixels = empty.bulk.Add(5, Span<const uint8>(), PackageBulkPlacement::Sidecar);
    PackageWriteOptions narrow = CookedOptions();
    narrow.bulkAlignment = 64;
    Written zero = WriteScene(env, empty, narrow);
    REQUIRE(zero.error == ErrorCode::None);
    CHECK(zero.sidecar.IsEmpty() && zero.result.sidecarBytes == 0);
    auto contents = Read(env, zero.package.View());
    REQUIRE(contents);
    CHECK(!contents.Value().sidecarPresent);
    for (const PackageBulkEntry& entry : contents.Value().bulkEntries) {
        CHECK(entry.flags == 0);
    }
}

// 읽기 거부(AS 20.1 4): 섹션 존재 규칙(Sidecar 항목인데 섹션 없음·섹션인데 Sidecar 항목 없음·패키지
// 안 항목이 없는데 Bulk 섹션), Sidecar 섹션 크기·hashBlockBytes(2의 거듭제곱 아님·4 KiB 미만·1 MiB
// 초과)·hashCount·fileBytes(머리 이하·끝과 다름), Sidecar 항목의 codec Zstd·정렬 아님·fileBytes 밖,
// 모르는 flag bit, 에디터 패키지의 Sidecar flag·섹션, 옛 판(UnsupportedVersion).
TEST(AssetSidecar_ReaderRejections) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package));
    Written mixed = WriteScene(env, scene, CookedOptions());
    REQUIRE(mixed.error == ErrorCode::None);
    REQUIRE(Read(env, mixed.package.View()));
    const usize sectionBytes = SidecarSectionFixedBytes + Sha256Digest::Bytes;

    // 섹션 존재.
    {
        EditablePackage edit(env.allocator);
        REQUIRE(edit.Parse(mixed.package.View()));
        REQUIRE(edit.Remove(PackageSectionKind::Sidecar));
        CHECK(Rejected(env, edit.Build()));
    }

    Scene plain(env.allocator);
    REQUIRE(BuildScene(*env.source, plain, PackageBulkPlacement::Package,
                       PackageBulkPlacement::Package, SceneId + 1));
    Written packaged = WriteScene(env, plain, CookedOptions());
    REQUIRE(packaged.error == ErrorCode::None);
    {
        EditablePackage source(env.allocator);
        EditablePackage edit(env.allocator);
        REQUIRE(source.Parse(mixed.package.View()) && edit.Parse(packaged.package.View()));
        const EditableSection* section = source.Find(PackageSectionKind::Sidecar);
        REQUIRE(section != nullptr);
        REQUIRE(edit.Insert(section->kind, section->flags, section->bytes.View()) != nullptr);
        CHECK(Rejected(env, edit.Build()));
    }

    // Bulk 섹션은 패키지 안 항목이 있을 때만이다: Sidecar 항목만인 패키지에 Bulk 섹션을 넣으면
    // 거부.
    Scene only(env.allocator);
    REQUIRE(BuildScene(*env.source, only, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Sidecar, SceneId + 3));
    Written streamedOnly = WriteScene(env, only, CookedOptions());
    REQUIRE(streamedOnly.error == ErrorCode::None);
    {
        EditablePackage source(env.allocator);
        EditablePackage edit(env.allocator);
        REQUIRE(source.Parse(mixed.package.View()) && edit.Parse(streamedOnly.package.View()));
        const EditableSection* bulk = source.Find(PackageSectionKind::Bulk);
        REQUIRE(bulk != nullptr);
        REQUIRE(edit.Insert(bulk->kind, bulk->flags, bulk->bytes.View()) != nullptr);
        CHECK(Rejected(env, edit.Build()));
    }

    // BulkIndex는 Bulk 또는 Sidecar 섹션이 있을 때만이다: Sidecar 항목만인 패키지에서 Sidecar
    // 섹션을 빼면 전체 읽기와 부분 읽기(색인 — BulkIndex 내용을 읽지 않는다) 모두 거부한다.
    {
        EditablePackage edit(env.allocator);
        REQUIRE(edit.Parse(streamedOnly.package.View()));
        REQUIRE(edit.Remove(PackageSectionKind::Sidecar));
        const Array<uint8> orphan = edit.Build();
        CHECK(Rejected(env, orphan));
        MemoryPackageRangeReader reader(orphan.View());
        PackageReadOptions options;
        options.types = &env.types;
        AssetDiagnostic failure;
        auto index = ReadPackageIndex(env.allocator, reader, options, &failure);
        CHECK(!index && index.Error() == ErrorCode::CorruptData &&
              failure.code == AssetDiagnosticCode::CorruptPackage);
    }

    // Sidecar 섹션 칸.
    auto withSection = [&](auto change) noexcept {
        return WithSidecarSection(env, mixed.package, change);
    };
    CHECK(Rejected(env, withSection([&](Array<uint8>& bytes) { (void)bytes.TryPushBack(0); })));
    CHECK(Rejected(
        env, withSection([&](Array<uint8>& bytes) { (void)bytes.TryResize(sectionBytes - 1); })));
    CHECK(Rejected(env, withSection([&](Array<uint8>& bytes) {
                       (void)bytes.TryResize(SidecarSectionFixedBytes - 1);
                   })));
    const uint32 badBlocks[] = {3 * 1024 * 3, SidecarMinHashBlockBytes / 2,
                                SidecarMaxHashBlockBytes * 2, 0};
    for (uint32 blockBytes : badBlocks) {
        CHECK(Rejected(env, withSection([&](Array<uint8>& bytes) {
                           StoreLittleEndian<uint32>(bytes.Data() + SectionBlockBytesOffset,
                                                     blockBytes);
                       })));
    }

    // 8 KiB 블록이면 hashCount가 2여야 한다(fileBytes 9096) — 1개 그대로는 거부, 2개로 늘리면 섹션
    // 규칙은 통과한다(블록 해시 값은 범위 읽기가 본다).
    CHECK(Rejected(env, withSection([&](Array<uint8>& bytes) {
                       StoreLittleEndian<uint32>(bytes.Data() + SectionBlockBytesOffset,
                                                 SidecarMinHashBlockBytes * 2);
                   })));
    {
        auto contents = Read(env, mixed.package.View());
        REQUIRE(contents);
        const uint64 fileBytes = contents.Value().sidecar.fileBytes;
        const uint32 blockBytes = SidecarMinHashBlockBytes * 2;
        const uint32 count = static_cast<uint32>((fileBytes + blockBytes - 1) / blockBytes);
        REQUIRE(count == 2);
        Array<uint8> rehashed = withSection([&](Array<uint8>& bytes) {
            StoreLittleEndian<uint32>(bytes.Data() + SectionBlockBytesOffset, blockBytes);
            StoreLittleEndian<uint32>(bytes.Data() + SectionCountOffset, count);
            (void)bytes.TryResize(SidecarSectionFixedBytes + Sha256Digest::Bytes * count);
        });
        CHECK(Read(env, rehashed.View()));
        CHECK(Rejected(env, withSection([&](Array<uint8>& bytes) {
                           StoreLittleEndian<uint32>(bytes.Data() + SectionCountOffset, 2);
                       })));
    }

    // fileBytes: 머리 크기 이하, 마지막 entry 끝보다 큼(같은 hashCount), 작음(entry가 밖).
    const uint64 fileBytesValues[] = {SidecarHeaderBytes, 4096 + PixelBytes + 1,
                                      4096 + PixelBytes - 1};
    for (uint64 fileBytes : fileBytesValues) {
        CHECK(Rejected(env, withSection([&](Array<uint8>& bytes) {
                           StoreLittleEndian<uint64>(bytes.Data(), fileBytes);
                       })));
    }

    // 블록 수 올림이 넘치는 fileBytes(2^64 − 1, hashCount 0, 섹션 48 byte)도 거부한다.
    CHECK(Rejected(env, withSection([&](Array<uint8>& bytes) {
                       StoreLittleEndian<uint64>(bytes.Data(), NumericLimits<uint64>::Max());
                       StoreLittleEndian<uint32>(bytes.Data() + SectionCountOffset, 0);
                       (void)bytes.TryResize(SidecarSectionFixedBytes);
                   })));

    // SHA-256 칸은 섹션 규칙이 아니다(매니페스트 대조가 본다) — 바꿔도 읽힌다.
    CHECK(
        Read(env, withSection([&](Array<uint8>& bytes) { bytes[SectionShaOffset] ^= 1; }).View()));

    // Sidecar 항목: codec Zstd(stored = raw 그대로), 정렬 아님, fileBytes 밖, 모르는 flag bit.
    CHECK(Rejected(env, WithBulkIndex(env, mixed.package, [](Array<PackageBulkEntry>& entries) {
                       entries[0].codec = BulkCodec::Zstd;
                   })));
    CHECK(Rejected(env, WithBulkIndex(env, mixed.package, [](Array<PackageBulkEntry>& entries) {
                       entries[0].offset = SidecarHeaderBytes;
                   })));
    CHECK(Rejected(env, WithBulkIndex(env, mixed.package, [](Array<PackageBulkEntry>& entries) {
                       entries[0].offset += PackageDefaultCookedBulkAlignment;
                   })));
    CHECK(Rejected(env, WithBulkIndex(env, mixed.package, [](Array<PackageBulkEntry>& entries) {
                       entries[0].flags |= 4;
                   })));
    CHECK(Rejected(env, WithBulkIndex(env, mixed.package, [](Array<PackageBulkEntry>& entries) {
                       entries[0].flags = 0;
                   })));

    // 에디터 패키지: Sidecar flag(섹션 없음), Sidecar 섹션.
    Scene editor(env.allocator);
    REQUIRE(BuildScene(*env.source, editor, PackageBulkPlacement::Package,
                       PackageBulkPlacement::Package, SceneId + 2, PackageKind::Editor));
    Written editorFile = WriteScene(env, editor, EditorOptions(), false);
    REQUIRE(editorFile.error == ErrorCode::None);
    CHECK(Rejected(env,
                   WithBulkIndex(
                       env, editorFile.package,
                       [](Array<PackageBulkEntry>& entries) { entries[0].flags |= SidecarFlag; }, 0,
                       PackageKind::Editor),
                   PackageKind::Editor));
    {
        EditablePackage source(env.allocator);
        EditablePackage edit(env.allocator);
        REQUIRE(source.Parse(mixed.package.View()) && edit.Parse(editorFile.package.View()));
        const EditableSection* section = source.Find(PackageSectionKind::Sidecar);
        REQUIRE(section != nullptr);
        REQUIRE(edit.Insert(section->kind, section->flags, section->bytes.View()) != nullptr);
        CHECK(Rejected(env, edit.Build(), PackageKind::Editor));
    }

    // 옛 판(2)과 다음 판(4)은 UnsupportedVersion이다.
    const uint16 versions[] = {PackageFormatVersion - 1, PackageFormatVersion + 1};
    for (uint16 version : versions) {
        const Array<uint8> other = WithFormatVersion(env.allocator, mixed.package, version);
        PackageReadOptions options;
        options.types = &env.types;
        AssetDiagnostic failure;
        auto read = ReadPackage(env.allocator, other.View(), options, &failure);
        CHECK(!read && read.Error() == ErrorCode::Unsupported &&
              failure.code == AssetDiagnosticCode::UnsupportedVersion);
    }
}

// sidecar 머리(AS 20.2 1 ReadSidecarHeader): 쓴 머리는 통과하고, 칸마다(magic·판·headerBytes·정렬·
// entryCount·fileBytes·AssetId 두 반쪽·reserved) 바꾸고 CRC를 다시 맞춰도 CorruptData, CRC만 틀려도
// CorruptData다. 기대값이 다르면(다른 패키지의 sidecar) CorruptData, 크기가 64가 아니면
// InvalidArgument다.
TEST(AssetSidecar_HeaderChecks) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Sidecar));
    Written written = WriteScene(env, scene, CookedOptions());
    REQUIRE(written.error == ErrorCode::None);
    auto contents = Read(env, written.package.View());
    REQUIRE(contents);
    const SidecarHeaderExpectation expected = ExpectedSidecarHeader(contents.Value());
    CHECK(expected.bulkAlignment == PackageDefaultCookedBulkAlignment);
    CHECK(expected.entryCount == 2 && expected.asset == FixedId(SceneId));
    CHECK(expected.fileBytes == written.sidecar.Size());
    uint8 header[SidecarHeaderBytes];
    for (usize index = 0; index < SidecarHeaderBytes; ++index) {
        header[index] = written.sidecar[index];
    }

    const Span<const uint8> view(header, SidecarHeaderBytes);
    CHECK(ReadSidecarHeader(view, expected).IsOk());
    CHECK(header[0] == 'R' && header[1] == 'B' && header[2] == 'S' && header[3] == 'C');
    // 바꿀 byte 위치다(magic·판·headerBytes·정렬·entryCount·fileBytes·AssetId high·low·reserved
    // 처음과 끝).
    const usize offsets[] = {0, 3, 4, 6, 8, 12, 16, 24, 32, 40, 59};
    for (usize offset : offsets) {
        uint8 changed[SidecarHeaderBytes];
        for (usize index = 0; index < SidecarHeaderBytes; ++index) {
            changed[index] = header[index];
        }

        changed[offset] ^= 0x10;
        StoreLittleEndian<uint32>(changed + 60, Crc32(Span<const uint8>(changed, 60)));
        const Result<void> checked =
            ReadSidecarHeader(Span<const uint8>(changed, SidecarHeaderBytes), expected);
        CHECK(!checked && checked.Error() == ErrorCode::CorruptData);
    }

    uint8 badCrc[SidecarHeaderBytes];
    for (usize index = 0; index < SidecarHeaderBytes; ++index) {
        badCrc[index] = header[index];
    }

    badCrc[61] ^= 1;
    CHECK(ReadSidecarHeader(Span<const uint8>(badCrc, SidecarHeaderBytes), expected).Error() ==
          ErrorCode::CorruptData);

    SidecarHeaderExpectation other = expected;
    other.asset = FixedId(SceneId + 9);
    CHECK(ReadSidecarHeader(view, other).Error() == ErrorCode::CorruptData);
    other = expected;
    other.entryCount = 1;
    CHECK(ReadSidecarHeader(view, other).Error() == ErrorCode::CorruptData);
    other = expected;
    other.bulkAlignment = 64;
    CHECK(ReadSidecarHeader(view, other).Error() == ErrorCode::CorruptData);
    other = expected;
    other.fileBytes += 1;
    CHECK(ReadSidecarHeader(view, other).Error() == ErrorCode::CorruptData);
    CHECK(ReadSidecarHeader(view.Slice(0, SidecarHeaderBytes - 1), expected).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ReadSidecarHeader(written.sidecar.View(), expected).Error() ==
          ErrorCode::InvalidArgument);
}

// 런타임 로드(AS 20.1 7): DecodePackageBulk는 Sidecar 항목을 풀지 않고 streamed 자리(sidecar 안
// offset·rawBytes·sha256·crc32)를 내며 예산은 패키지 안 raw만 센다(300이면 통과, 299면
// CapacityExceeded). Prepare도 같은 예산이고 sidecar 없이 로드된다. Package::BulkBytes는 Sidecar
// ref에 Unsupported, 패키지 안 ref는 byte다.
TEST(AssetSidecar_LoadSkipsSidecar) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package));
    PackageWriteOptions zstd = CookedOptions();
    zstd.bulkCodec = BulkCodec::Zstd;
    Written written = WriteScene(env, scene, zstd);
    REQUIRE(written.error == ErrorCode::None);
    auto contents = Read(env, written.package.View());
    REQUIRE(contents);
    Array<uint8> raw(env.allocator);
    Array<PackageBulkSlot> slots(env.allocator);
    REQUIRE(DecodePackageBulk(contents.Value(), raw, slots, PartBytes));
    REQUIRE(slots.Size() == 2);
    const PackageBulkEntry& streamed = contents.Value().bulkEntries[0];
    CHECK(slots[0].streamed && slots[0].offset == streamed.offset);
    CHECK(slots[0].rawBytes == PixelBytes && slots[0].crc32 == streamed.crc32);
    CHECK(slots[0].sha256 == Sha256::Hash(scene.pixels.View()));
    CHECK(!slots[1].streamed && slots[1].offset == 0 && slots[1].rawBytes == PartBytes);
    CHECK(raw.Size() == PartBytes && SameBytes(raw.View(), scene.part.View()));
    Array<uint8> tight(env.allocator);
    Array<PackageBulkSlot> tightSlots(env.allocator);
    CHECK(DecodePackageBulk(contents.Value(), tight, tightSlots, PartBytes - 1).Error() ==
          ErrorCode::CapacityExceeded);

    // Prepare 예산.
    auto prepareWith = [&](uint64 budget) noexcept {
        PackageLoadOptions options = LoadOptions(PackageKind::Cooked);
        options.maxBulkRawBytes = budget;
        auto state =
            UniquePtr<PackageLoadState>::TryMake(env.allocator, env.allocator, *env.target,
                                                 env.types, env.target->OwnerAccess(), options);
        if (!state) {
            return state.Error();
        }

        auto prepared =
            state.Value()->Prepare(Copy(env.allocator, written.package.View()), nullptr);
        return prepared ? ErrorCode::None : prepared.Error();
    };
    CHECK(prepareWith(PartBytes) == ErrorCode::None);
    CHECK(prepareWith(PartBytes - 1) == ErrorCode::CapacityExceeded);

    Array<uint8> files[1] = {Copy(env.allocator, written.package.View())};
    RootRef<Object> roots[1];
    ObjectHandle handles[1];
    REQUIRE(LoadAll(env, *env.target, files, PackageKind::Cooked, nullptr, roots, handles));
    Package* package = Pointer<Package>(*env.target, handles[0]);
    REQUIRE(package != nullptr);
    CHECK(package->BulkCount() == 2);
    const ObjectAccessContext access = env.target->OwnerAccess();
    auto asset = env.target->FindObject(access, handles[0], StringView());
    auto body = env.target->FindObject(access, handles[0], "Body");
    REQUIRE(asset && body);
    TestAsset* loaded = Pointer<TestAsset>(*env.target, asset.Value());
    TestPart* part = Pointer<TestPart>(*env.target, body.Value());
    REQUIRE(loaded != nullptr && part != nullptr);
    CHECK(package->BulkBytes(loaded->pixels).Error() == ErrorCode::Unsupported);
    auto bytes = package->BulkBytes(part->data);
    REQUIRE(bytes);
    CHECK(SameBytes(bytes.Value(), scene.part.View()));
}

// 매니페스트 v3(AS 20.1 5): sidecarBytes·sidecarSha256이 왕복한다. sidecarBytes 0인데 SHA-256이 0이
// 아니면 모델 검증·쓰기는 InvalidArgument, 읽기는 CorruptData(둘 다 SidecarSha256 칸·항목 번호)다.
// sidecarBytes가 있으면 SHA-256 값은 칸 규칙이 아니다(패키지 대조가 본다).
TEST(AssetSidecar_ManifestFields) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    CookedManifest manifest(allocator);
    FillManifestHeader(manifest);
    CookedManifestEntry* first = AddEntry(manifest, 10, "Plain");
    CookedManifestEntry* second = AddEntry(manifest, 11, "Streamed");
    REQUIRE(first != nullptr && second != nullptr);
    constexpr uint64 StreamedBytes = 9096;
    second->sidecarBytes = StreamedBytes;
    second->sidecarSha256 = ManifestDigestOf(0x5C);
    Array<uint8> bytes(allocator);
    REQUIRE(WriteCookedManifest(allocator, manifest, bytes));
    auto read = ReadCookedManifest(allocator, bytes.View());
    REQUIRE(read);
    REQUIRE(read.Value().entries.Size() == 2);
    CHECK(read.Value().entries[0].sidecarBytes == 0);
    CHECK(read.Value().entries[0].sidecarSha256 == Sha256Digest{});
    CHECK(read.Value().entries[1].sidecarBytes == StreamedBytes);
    CHECK(read.Value().entries[1].sidecarSha256 == ManifestDigestOf(0x5C));
    // 시험 인코더(검증하지 않음)와 writer가 같은 byte다.
    CHECK(SameBytes(EncodeManifest(allocator, manifest).View(), bytes.View()));

    second->sidecarSha256 = Sha256Digest{};
    CHECK(ValidateCookedManifest(manifest, allocator));
    second->sidecarBytes = 0;
    second->sidecarSha256 = ManifestDigestOf(1);
    AssetDiagnostic model;
    CHECK(ValidateCookedManifest(manifest, allocator, &model).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ModelCorruptAt(model, CookedManifestField::SidecarSha256, 1));
    Array<uint8> rejected(allocator);
    AssetDiagnostic writeFailure;
    CHECK(!WriteCookedManifest(allocator, manifest, rejected, &writeFailure));
    CHECK(ModelCorruptAt(writeFailure, CookedManifestField::SidecarSha256, 1));
    const Array<uint8> encoded = EncodeManifest(allocator, manifest);
    AssetDiagnostic readFailure;
    CHECK(ReadCookedManifest(allocator, encoded.View(), &readFailure).Error() ==
          ErrorCode::CorruptData);
    CHECK(CorruptAt(readFailure, CookedManifestField::SidecarSha256, 1) &&
          readFailure.asset == FixedId(11));

    // 칸 판정 순서: PackageBytes → SidecarSha256 → Flags.
    second->packageBytes = PackageHeaderBytes - 1;
    AssetDiagnostic earlier;
    CHECK(!ValidateCookedManifest(manifest, allocator, &earlier));
    CHECK(ModelCorruptAt(earlier, CookedManifestField::PackageBytes, 1));
    second->packageBytes = PackageHeaderBytes;
    second->flags = 0x80000000u;
    AssetDiagnostic later;
    CHECK(!ValidateCookedManifest(manifest, allocator, &later));
    CHECK(ModelCorruptAt(later, CookedManifestField::SidecarSha256, 1));
}

// 패키지 대조(AS 20.1 5 CheckCookedPackageContents): sidecar 있는 패키지는 그 결과로 만든 항목과
// 맞고 bulkRawBytes는 패키지 안 entry만이다. sidecarBytes 다름·있는 섹션에 0·SHA-256 다름·없는
// 섹션에 sidecarBytes가 각 칸의 CorruptPackage다. 판정 순서는 BulkRawBytes → SidecarBytes →
// SidecarSha256 → PackageProfile이다(둘이 다르면 앞 칸).
TEST(AssetSidecar_PackageCrossCheck) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Allocator& allocator = env.allocator;
    Scene scene(allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package));
    Written written = WriteScene(env, scene, CookedOptions());
    REQUIRE(written.error == ErrorCode::None);
    auto contents = Read(env, written.package.View());
    REQUIRE(contents);
    CookedManifest manifest(allocator);
    FillManifestHeader(manifest);
    CookedManifestEntry* entry = AddEntry(manifest, SceneId, "Streamed");
    REQUIRE(entry != nullptr);
    entry->packageBytes = written.package.Size();
    entry->packageSha256 = Sha256::Hash(written.package.View());
    entry->bulkRawBytes = PartBytes;
    entry->sidecarBytes = written.result.sidecarBytes;
    entry->sidecarSha256 = written.result.sidecarSha256;
    REQUIRE(ValidateCookedManifest(manifest, allocator));
    CHECK(CheckCookedPackage(manifest, *entry, written.package.View(), contents.Value()));

    auto expect = [&](auto&& change, CookedManifestField field) {
        CookedManifest copy(allocator);
        FillManifestHeader(copy);
        CookedManifestEntry* e = AddEntry(copy, SceneId, "Streamed");
        if (e == nullptr) {
            return false;
        }

        e->packageBytes = entry->packageBytes;
        e->packageSha256 = entry->packageSha256;
        e->bulkRawBytes = entry->bulkRawBytes;
        e->sidecarBytes = entry->sidecarBytes;
        e->sidecarSha256 = entry->sidecarSha256;
        change(copy, *e);
        AssetDiagnostic failure;
        return CheckCookedPackageContents(copy, *e, contents.Value(), &failure).Error() ==
                   ErrorCode::CorruptData &&
               CorruptAt(failure, field, CookedManifestNoEntry) && failure.asset == e->asset;
    };
    using F = CookedManifestField;
    // 모든 raw 합(5300)은 bulkRawBytes가 아니다.
    CHECK(expect([](CookedManifest&, CookedManifestEntry& e) { e.bulkRawBytes += PixelBytes; },
                 F::BulkRawBytes));
    CHECK(expect([](CookedManifest&, CookedManifestEntry& e) { e.sidecarBytes += 1; },
                 F::SidecarBytes));
    CHECK(expect(
        [](CookedManifest&, CookedManifestEntry& e) {
            e.sidecarBytes = 0;
            e.sidecarSha256 = Sha256Digest{};
        },
        F::SidecarBytes));
    CHECK(expect([](CookedManifest&, CookedManifestEntry& e) { e.sidecarSha256.bytes[31] ^= 1; },
                 F::SidecarSha256));
    // 판정 순서.
    CHECK(expect(
        [](CookedManifest&, CookedManifestEntry& e) {
            e.bulkRawBytes += 1;
            e.sidecarBytes += 1;
        },
        F::BulkRawBytes));
    CHECK(expect(
        [](CookedManifest&, CookedManifestEntry& e) {
            e.sidecarBytes += 1;
            e.sidecarSha256.bytes[0] ^= 1;
        },
        F::SidecarBytes));
    CHECK(expect(
        [](CookedManifest& m, CookedManifestEntry& e) {
            e.sidecarSha256.bytes[0] ^= 1;
            ++m.cookerVersion;
        },
        F::SidecarSha256));

    // sidecar 없는 패키지에 sidecarBytes가 있으면 SidecarBytes다.
    Scene plain(allocator);
    REQUIRE(BuildScene(*env.source, plain, PackageBulkPlacement::Package,
                       PackageBulkPlacement::Package, SceneId + 1));
    Written packaged = WriteScene(env, plain, CookedOptions());
    REQUIRE(packaged.error == ErrorCode::None);
    auto plainContents = Read(env, packaged.package.View());
    REQUIRE(plainContents);
    CookedManifest other(allocator);
    FillManifestHeader(other);
    CookedManifestEntry* plainEntry = AddEntry(other, SceneId + 1, "Plain");
    REQUIRE(plainEntry != nullptr);
    plainEntry->packageBytes = packaged.package.Size();
    plainEntry->packageSha256 = Sha256::Hash(packaged.package.View());
    plainEntry->bulkRawBytes = PixelBytes + PartBytes;
    CHECK(CheckCookedPackage(other, *plainEntry, packaged.package.View(), plainContents.Value()));
    plainEntry->sidecarBytes = written.result.sidecarBytes;
    plainEntry->sidecarSha256 = written.result.sidecarSha256;
    AssetDiagnostic failure;
    CHECK(!CheckCookedPackageContents(other, *plainEntry, plainContents.Value(), &failure));
    CHECK(CorruptAt(failure, F::SidecarBytes, CookedManifestNoEntry));
}

// 고정 seed 변조(AS 20.4 형식 — 14절 12의 확장): 섞인 sidecar 패키지(Sidecar 항목·Sidecar 섹션·
// Zstd 패키지 bulk)를 4000번 바꾼다(절반은 모든 CRC를 다시 맞춘다). 읽기·Prepare는 정해진 오류만
// 내고, 읽기 거부·Prepare 거부·받아들임이 모두 일어난다. 받아들인 입력의 Sidecar 항목은 늘 쿠킹·
// codec None·Sidecar 섹션 fileBytes 안이다.
TEST(AssetSidecar_FixedSeedMutationFuzz) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Scene scene(env.allocator);
    REQUIRE(BuildScene(*env.source, scene, PackageBulkPlacement::Sidecar,
                       PackageBulkPlacement::Package));
    PackageWriteOptions zstd = CookedOptions();
    zstd.bulkCodec = BulkCodec::Zstd;
    Written valid = WriteScene(env, scene, zstd);
    REQUIRE(valid.error == ErrorCode::None);
    struct XorShift {
        uint64 state;

        uint64 Next() noexcept {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }
    };

    XorShift random{0x51DECA5EEDull};
    constexpr uint32 Iterations = 4000;
    uint32 readFailures = 0;
    uint32 loadFailures = 0;
    uint32 accepted = 0;
    for (uint32 iteration = 0; iteration < Iterations; ++iteration) {
        Array<uint8> bytes = Copy(env.allocator, valid.package.View());
        const uint32 edits = 1 + static_cast<uint32>(random.Next() % 4);
        for (uint32 edit = 0; edit < edits; ++edit) {
            const usize at = static_cast<usize>(random.Next() % bytes.Size());
            bytes[at] = static_cast<uint8>(random.Next());
        }

        if (iteration % 2 == 1) {
            FixAllCrcs(bytes);
        }

        PackageReadOptions read;
        read.types = &env.types;
        AssetDiagnostic readFailure;
        auto contents = ReadPackage(env.allocator, bytes.View(), read, &readFailure);
        if (!contents) {
            ++readFailures;
            CHECK((contents.Error() == ErrorCode::CorruptData &&
                   readFailure.code == AssetDiagnosticCode::CorruptPackage) ||
                  (contents.Error() == ErrorCode::Unsupported &&
                   readFailure.code == AssetDiagnosticCode::UnsupportedVersion));
            continue;
        }

        for (const PackageBulkEntry& entry : contents.Value().bulkEntries) {
            if ((entry.flags & SidecarFlag) != 0) {
                CHECK(contents.Value().header.kind == PackageKind::Cooked &&
                      entry.codec == BulkCodec::None && contents.Value().sidecarPresent &&
                      entry.offset + entry.storedBytes <= contents.Value().sidecar.fileBytes);
            }
        }

        PackageLoadOptions options = LoadOptions(PackageKind::Cooked);
        options.maxBulkRawBytes = 64 * 1024 * 1024;
        PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                               options);
        AssetDiagnostic loadFailure;
        auto prepared = state.Prepare(Move(bytes), &loadFailure);
        if (prepared) {
            ++accepted;
            continue;
        }

        ++loadFailures;
        const ErrorCode error = prepared.Error();
        CHECK(error == ErrorCode::CorruptData || error == ErrorCode::Unsupported ||
              error == ErrorCode::NotFound || error == ErrorCode::TypeMismatch ||
              error == ErrorCode::CapacityExceeded || error == ErrorCode::Overflow);
    }

    CHECK(readFailures + loadFailures + accepted == Iterations);
    CHECK(readFailures > Iterations / 2);
    CHECK(loadFailures > 0 && accepted > 0);
}

} // namespace package_sidecar_tests_file
