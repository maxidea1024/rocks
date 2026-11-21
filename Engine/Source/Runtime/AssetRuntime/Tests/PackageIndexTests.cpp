#include "AssetFixtureValues.h"
#include "PackageEditing.h"

#include <AssetRuntime/PackageRangeReader.h>
#include <AssetRuntime/PackageTags.h>
#include <Core/FileError.h>
#include <Platform/Process.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <stdio.h>
#include <string.h>

// 패키지 부분 읽기·범위 읽기·Tags 값 스키마 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 13 —
// A2b): 부분 읽기가 전체 읽기와 같은 내용을 주는지, 모든 byte·bit 손상에서 판정 관계(읽은 구간은
// 같은 진단, 건너뛴 구간은 부분 읽기 통과·전체 읽기 거부)가 지켜지는지, 모든 길이의 자르기, 범위
// 읽기의 모든 IO 실패·할당 실패, 범위 읽기 구현의 경계, Tags 스키마의 쓰기·읽기·거부.
using namespace rocks;
namespace package_index_tests_file {

using namespace rocks::asset_test;

namespace {

constexpr uint32 BitsPerByte = 8;

// 풍부한 에디터 장면이다: A(1800) 대표 에셋(Hard → B 대표 에셋·Soft·bulk 둘·EditorOnly Hard)과 하위
// 객체 Part(bulk), B(1801) 대표 에셋. 옵션은 Zstd bulk·커스텀 버전·ImportRecord·Tags(값 스키마)·
// Build 의존이다.
struct RichScene {
    explicit RichScene(Allocator& allocator) noexcept
        : pixels(allocator), data(allocator), importRecord(allocator), tags(allocator),
          file(allocator) {
    }

    RootRef<Package> a;
    RootRef<Package> b;
    Array<uint8> pixels;
    Array<uint8> data;
    TestBulkSource bulk;
    Array<uint8> importRecord;
    Array<uint8> tags;
    Array<uint8> file;
};

const PackageCustomVersion RichCustomVersions[] = {{Guid{0x1DE, 1}, 2}, {Guid{0x1DE, 2}, 7}};
const PackageDependencyEntry RichBuildDependencies[] = {
    {FixedId(1803), PackageDependencyKind::Build, TypeId{}, false}};
const PackageTagInput RichTags[] = {{"Kind", "Hero"},
                                    {"UnrealExchange.FormerPaths", "/Game/Old/B"},
                                    {"UnrealExchange.FormerPaths", "/Game/Old/A"}};

bool MakeRich(AssetEnv& env, RichScene& scene) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    auto a = NewPackage(registry, access, FixedId(1800), PackageKind::Editor);
    auto b = NewPackage(registry, access, FixedId(1801), PackageKind::Editor);
    if (!a || !b) {
        return false;
    }

    scene.a = Move(a.Value());
    scene.b = Move(b.Value());
    const ObjectHandle bAsset = Make<TestAsset>(registry, TestAssetType(), scene.b.Handle(), "");
    const ObjectHandle asset = Make<TestAsset>(registry, TestAssetType(), scene.a.Handle(), "");
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), asset, "Part");
    TestAsset* pointer = Pointer<TestAsset>(registry, asset);
    if (bAsset.IsNull() || part.IsNull() || pointer == nullptr) {
        return false;
    }

    FillCompressible(scene.pixels, 3000, 23);
    FillIncompressible(scene.data, 64, 23);
    pointer->value = 1800;
    pointer->pixels = scene.bulk.Add(1, scene.pixels.View());
    pointer->source = scene.bulk.Add(2, scene.data.View());
    Pointer<TestPart>(registry, part)->index = 5;
    Pointer<TestPart>(registry, part)->data = scene.bulk.Add(3, scene.data.View());
    if (!pointer->label.TryAssign("rich") || !pointer->note.TryAssign("note") ||
        !pointer->soft.TryAssign(FixedId(1802), "Sub") ||
        !registry.SetReference(access, asset, pointer->hard, bAsset) ||
        !registry.SetReference(access, asset, pointer->editorRef, bAsset)) {
        return false;
    }

    const uint8 importerVersion[4] = {3, 0, 0, 0};
    RecordBuilder importRecord(env.allocator, Guid{0x1DE, 0x1EC0}, 1);
    if (!importRecord.AddField(Guid{0x1DE, 0x11}, WireType::U32, importerVersion)) {
        return false;
    }

    auto builtImport = importRecord.Build();
    auto builtTags = WritePackageTags(env.allocator, RichTags);
    if (!builtImport || !builtTags) {
        return false;
    }

    scene.importRecord = Move(builtImport.Value());
    scene.tags = Move(builtTags.Value());
    PackageWriteOptions options = EditorOptions();
    options.bulkSource = &scene.bulk;
    options.bulkCodec = BulkCodec::Zstd;
    options.customVersions = RichCustomVersions;
    options.importRecord = scene.importRecord.View();
    options.tags = scene.tags.View();
    options.extraDependencies = RichBuildDependencies;
    scene.file = Write(env.allocator, registry, scene.a.Handle(), options);
    return !scene.file.IsEmpty();
}

PackageReadOptions EditorRead() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.customVersions = RichCustomVersions;
    return options;
}

bool SameDiagnostic(const AssetDiagnostic& a, const AssetDiagnostic& b) noexcept {
    return a.code == b.code && a.error == b.error && a.section == b.section &&
           a.detail == b.detail && a.asset == b.asset;
}

// 부분 읽기가 전체 읽기와 같은 내용을 주는지다(Objects·bulk 제외).
bool SameIndexContents(const PackageContents& full, const PackageContents& index) noexcept {
    if (!(full.header.assetId == index.header.assetId) ||
        full.header.sectionCount != index.header.sectionCount ||
        !(full.fingerprint.digest == index.fingerprint.digest) || full.readOnly != index.readOnly ||
        full.exports.Size() != index.exports.Size() ||
        full.imports.Size() != index.imports.Size() ||
        full.dependencies.Size() != index.dependencies.Size() ||
        full.summary.types.Size() != index.summary.types.Size() ||
        full.summary.customVersions.Size() != index.summary.customVersions.Size() ||
        full.importRecord.Size() != index.importRecord.Size() ||
        full.tags.Size() != index.tags.Size() ||
        memcmp(full.importRecord.Data(), index.importRecord.Data(), full.importRecord.Size()) !=
            0 ||
        memcmp(full.tags.Data(), index.tags.Data(), full.tags.Size()) != 0 ||
        !index.objects.IsEmpty() || !index.bulk.IsEmpty() || !index.bulkEntries.IsEmpty()) {
        return false;
    }

    for (usize i = 0; i < full.exports.Size(); ++i) {
        if (full.exports[i].name != index.exports[i].name ||
            !(full.exports[i].type == index.exports[i].type) ||
            full.exports[i].outerIndex != index.exports[i].outerIndex) {
            return false;
        }
    }

    for (usize i = 0; i < full.imports.Size(); ++i) {
        if (!(full.imports[i].asset == index.imports[i].asset) ||
            full.imports[i].name != index.imports[i].name) {
            return false;
        }
    }

    for (usize i = 0; i < full.dependencies.Size(); ++i) {
        if (!(full.dependencies[i].asset == index.dependencies[i].asset) ||
            full.dependencies[i].kind != index.dependencies[i].kind) {
            return false;
        }
    }

    return true;
}

// 패키지 파일 안 offset의 손상이 두 읽기에 주는 결과다: Read(헤더·섹션 표·두 읽기가 다 읽는 섹션 —
// 같은 진단으로 거부), Skipped(부분 읽기가 건너뛰는 Objects·Bulk·BulkIndex 내용과 섹션 사이 패딩 —
// 부분 읽기 통과·전체 읽기 거부), Ignored(모르는 비 Required 섹션 내용 — 두 읽기 모두 내용을 보지
// 않아 통과).
enum class FlipRegion {
    Read,
    Skipped,
    Ignored
};

FlipRegion RegionOf(const PackageContents& contents, uint64 offset) noexcept {
    if (offset < PackageHeaderBytes || offset >= contents.header.sectionTableOffset) {
        return FlipRegion::Read;
    }

    for (const PackageSectionEntry& entry : contents.sections) {
        if (offset >= entry.offset && offset < entry.offset + entry.bytes) {
            if (static_cast<uint32>(entry.kind) >
                static_cast<uint32>(PackageSectionKind::BulkIndex)) {
                return FlipRegion::Ignored;
            }

            const bool skipped = entry.kind == PackageSectionKind::Objects ||
                                 entry.kind == PackageSectionKind::Bulk ||
                                 entry.kind == PackageSectionKind::BulkIndex;
            return skipped ? FlipRegion::Skipped : FlipRegion::Read;
        }
    }

    return FlipRegion::Skipped; // 패딩
}

// 손상 시험의 패키지 모양 셋이다: 풍부한 장면, 최소 패키지(대표 에셋 하나 — 선택 섹션 없음), 최소
// 패키지에 모르는 비 Required 섹션(kind 200 — 두 읽기 모두 readOnly)을 더한 것.
constexpr uint32 UnknownSectionKind = 200;

struct FlipShapes {
    explicit FlipShapes(Allocator& allocator) noexcept
        : rich(allocator), minimal(allocator), unknown(allocator), scene(allocator) {
    }

    Array<uint8> rich;
    Array<uint8> minimal;
    Array<uint8> unknown;
    RichScene scene;
    RootRef<Package> minimalPackage;
};

bool MakeShapes(AssetEnv& env, FlipShapes& shapes) noexcept {
    if (!MakeRich(env, shapes.scene)) {
        return false;
    }

    shapes.rich = Copy(env.allocator, shapes.scene.file.View());
    ObjectRegistry& registry = *env.source;
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(1810), PackageKind::Editor);
    if (!made) {
        return false;
    }

    shapes.minimalPackage = Move(made.Value());
    if (Make<TestAsset>(registry, TestAssetType(), shapes.minimalPackage.Handle(), "").IsNull()) {
        return false;
    }

    shapes.minimal =
        Write(env.allocator, registry, shapes.minimalPackage.Handle(), EditorOptions());
    EditablePackage edit(env.allocator);
    if (shapes.minimal.IsEmpty() || !edit.Parse(shapes.minimal.View())) {
        return false;
    }

    EditableSection extra(env.allocator);
    extra.kind = UnknownSectionKind;
    const uint8 payload[] = {1, 2, 3, 4, 5, 6, 7};
    if (!extra.bytes.TryAppend(Span<const uint8>(payload)) ||
        !edit.sections.TryPushBack(Move(extra))) {
        return false;
    }

    shapes.unknown = edit.Build();
    return !shapes.unknown.IsEmpty();
}

// n번째 ReadAt을 실패시키는 범위 읽기다(IO 오류 주입).
class FailingRangeReader final : public PackageRangeReader {
public:
    FailingRangeReader(Span<const uint8> bytes, uint32 failAt) noexcept
        : inner_(bytes), failAt_(failAt) {
    }

    uint64 FileBytes() const noexcept override {
        return inner_.FileBytes();
    }

    Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept override {
        if (++reads_ == failAt_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return inner_.ReadAt(offset, out);
    }

    uint32 Reads() const noexcept {
        return reads_;
    }

private:
    MemoryPackageRangeReader inner_;
    uint32 failAt_;
    uint32 reads_ = 0;
};

// 파일 범위 읽기의 IO 실패를 주입하는 가짜 파일 시스템이다(메모리 byte를 파일로 보인다).
enum class RangeFault {
    None,
    Open,
    Size,
    Seek,
    Read
};

constexpr platform::FileStatus FaultError{platform::FileCode::SystemError, 5};

class RangeFaultHandle final : public platform::FileHandle {
public:
    explicit RangeFaultHandle(Span<const uint8> bytes) noexcept : bytes_(bytes) {
    }

    platform::FileTransfer Read(void* destination, usize bytes) noexcept override {
        if (fault == RangeFault::Read) {
            return {FaultError, 0};
        }

        const usize left = bytes_.Size() - position_;
        const usize count = bytes < left ? bytes : left;
        memcpy(destination, bytes_.Data() + position_, count);
        position_ += count;
        return {platform::FileStatus{}, count};
    }

    platform::FileTransfer Write(const void*, usize) noexcept override {
        return {FaultError, 0};
    }

    platform::FileStatus Seek(int64 offset, platform::FileSeekOrigin,
                              uint64& position) noexcept override {
        if (fault == RangeFault::Seek) {
            return FaultError;
        }

        position_ = static_cast<usize>(offset);
        position = position_;
        return {};
    }

    platform::FileStatus Size(uint64& bytes) noexcept override {
        if (fault == RangeFault::Size) {
            return FaultError;
        }

        bytes = bytes_.Size();
        return {};
    }

    platform::FileStatus Close() noexcept override {
        return {};
    }

    void Destroy() noexcept override {
    }

    RangeFault fault = RangeFault::None;

private:
    Span<const uint8> bytes_;
    usize position_ = 0;
};

class RangeFaultSystem final : public platform::FileSystem {
public:
    explicit RangeFaultSystem(Span<const uint8> bytes) noexcept : handle(bytes) {
    }

    platform::FileStatus Open(const char*, usize, platform::FileOpenMode,
                              platform::FileHandle*& out) noexcept override {
        if (handle.fault == RangeFault::Open) {
            return FaultError;
        }

        out = &handle;
        return {};
    }

    platform::FileStatus Exists(const char*, usize, bool& exists) noexcept override {
        exists = true;
        return {};
    }

    platform::FileStatus Delete(const char*, usize) noexcept override {
        return FaultError;
    }

    RangeFaultHandle handle;
};

} // namespace

// 같은 내용(7.1절 13): 부분 읽기는 헤더·섹션 표·Summary·Exports·ObjectImports·Dependencies·
// ImportRecord·Tags·지문·readOnly를 전체 읽기와 같게 주고, Objects·Bulk·bulk 항목은 비운다. 결과
// view는 소유 버퍼를 가리켜 값을 옮겨도 유효하다. 범위 읽기는 헤더·표·고른 섹션만 한다(읽은 byte
// 수가 파일보다 작다).
TEST(PackageIndex_MatchesFullRead) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RichScene scene(env.allocator);
    REQUIRE(MakeRich(env, scene));
    auto full = ReadPackage(env.allocator, scene.file.View(), EditorRead());
    REQUIRE(full);
    MemoryPackageRangeReader reader(scene.file.View());
    auto index = ReadPackageIndex(env.allocator, reader, EditorRead());
    REQUIRE(index);
    PackageIndexView moved = Move(index.Value());
    CHECK(SameIndexContents(full.Value(), moved.contents));
    usize readBytes = 0;
    for (const Array<uint8>& buffer : moved.buffers) {
        readBytes += buffer.Size();
    }

    CHECK(readBytes < scene.file.Size());
    auto tags = ReadPackageTags(env.allocator, moved.contents.tags);
    REQUIRE(tags);
    CHECK(tags.Value().Size() == 3);
}

// 모든 bit 손상(7.1절 13 판정 관계): 패키지 모양 셋(풍부한 장면·최소 패키지·모르는 비 Required
// 섹션)마다 모든 byte의 모든 bit를 뒤집는다. 읽는 구간(헤더·표·읽는 섹션)의 손상은 두 읽기가 같은
// 진단으로 거부하고, 건너뛰는 구간(Objects·Bulk·BulkIndex 내용과 패딩)의 손상은 부분 읽기가
// 통과하고 전체 읽기가 거부하며, 모르는 섹션 내용의 손상은 둘 다 통과한다(readOnly).
TEST(PackageIndex_EveryBitFlip) {
    AssetEnv env;
    REQUIRE(env.Ready());
    FlipShapes shapes(env.allocator);
    REQUIRE(MakeShapes(env, shapes));
    const Array<uint8>* const files[] = {&shapes.rich, &shapes.minimal, &shapes.unknown};
    uint32 regionFlips[3] = {};
    uint32 mismatches = 0;
    for (const Array<uint8>* file : files) {
        auto original = ReadPackage(env.allocator, file->View(), EditorRead());
        REQUIRE(original);
        CHECK(original.Value().readOnly == (file == &shapes.unknown));
        Array<uint8> bytes = Copy(env.allocator, file->View());
        for (usize offset = 0; offset < bytes.Size(); ++offset) {
            const FlipRegion region = RegionOf(original.Value(), offset);
            for (uint32 bit = 0; bit < BitsPerByte; ++bit) {
                bytes[offset] = static_cast<uint8>(bytes[offset] ^ (1u << bit));
                AssetDiagnostic fullFailure;
                AssetDiagnostic indexFailure;
                auto full = ReadPackage(env.allocator, bytes.View(), EditorRead(), &fullFailure);
                MemoryPackageRangeReader reader(bytes.View());
                auto index = ReadPackageIndex(env.allocator, reader, EditorRead(), &indexFailure);
                bool expected = false;
                switch (region) {
                case FlipRegion::Read:
                    expected = !full && !index && full.Error() == index.Error() &&
                               SameDiagnostic(fullFailure, indexFailure);
                    break;
                case FlipRegion::Skipped:
                    expected = !full && index;
                    break;
                case FlipRegion::Ignored:
                    expected = full && index && index.Value().contents.readOnly;
                    break;
                }

                ++regionFlips[static_cast<uint32>(region)];
                if (!expected && mismatches++ < 5) {
                    printf("  region %u offset %zu bit %u: full %d index %d\n",
                           static_cast<unsigned>(region), offset, bit,
                           static_cast<int>(static_cast<bool>(full)),
                           static_cast<int>(static_cast<bool>(index)));
                }

                bytes[offset] = static_cast<uint8>(bytes[offset] ^ (1u << bit));
            }
        }
    }

    CHECK(mismatches == 0);
    CHECK(regionFlips[0] > 0 && regionFlips[1] > 0 && regionFlips[2] > 0);
}

// 자르기·늘리기(7.1절 13): 모양 셋마다 모든 길이로 자르거나 1 byte 늘린 파일은 두 읽기가 같은
// 진단으로 거부한다(헤더의 fileBytes와 크기 대조, 64 byte 미만은 있는 만큼으로 헤더 판정).
TEST(PackageIndex_TruncateAndExtend) {
    AssetEnv env;
    REQUIRE(env.Ready());
    FlipShapes shapes(env.allocator);
    REQUIRE(MakeShapes(env, shapes));
    const Array<uint8>* const files[] = {&shapes.rich, &shapes.minimal, &shapes.unknown};
    uint32 mismatches = 0;
    for (const Array<uint8>* file : files) {
        for (usize size = 0; size <= file->Size(); ++size) {
            Array<uint8> bytes = Copy(env.allocator, file->View().Slice(0, size));
            if (size == file->Size()) {
                REQUIRE(bytes.TryPushBack(uint8{0}));
            }

            AssetDiagnostic fullFailure;
            AssetDiagnostic indexFailure;
            auto full = ReadPackage(env.allocator, bytes.View(), EditorRead(), &fullFailure);
            MemoryPackageRangeReader reader(bytes.View());
            auto index = ReadPackageIndex(env.allocator, reader, EditorRead(), &indexFailure);
            if (full || index || !SameDiagnostic(fullFailure, indexFailure)) {
                ++mismatches;
            }
        }
    }

    CHECK(mismatches == 0);
}

// 범위 읽기 IO 실패(7.1절 13): 부분 읽기의 n번째 범위 읽기가 실패하면(모든 n) 그 오류(진단
// None)이고, 실패가 없으면 성공한다. 할당 실패도 모든 지점에서 그 오류(진단 None)다.
TEST(PackageIndex_ReadAndAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RichScene scene(env.allocator);
    REQUIRE(MakeRich(env, scene));
    uint32 ioFailures = 0;
    for (uint32 failAt = 1;; ++failAt) {
        FailingRangeReader reader(scene.file.View(), failAt);
        AssetDiagnostic failure;
        failure.code = AssetDiagnosticCode::CorruptPackage;
        auto index = ReadPackageIndex(env.allocator, reader, EditorRead(), &failure);
        if (reader.Reads() < failAt) {
            CHECK(index);
            break;
        }

        CHECK(!index && index.Error() == ErrorCode::Busy);
        CHECK(failure.code == AssetDiagnosticCode::None);
        ++ioFailures;
    }

    CHECK(ioFailures >= 3);
    test::NFailAllocator failing(env.allocator);
    uint32 allocationFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        MemoryPackageRangeReader reader(scene.file.View());
        failing.FailOnAttempt(attempt);
        AssetDiagnostic failure;
        auto index = ReadPackageIndex(failing, reader, EditorRead(), &failure);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(index);
            break;
        }

        CHECK(!index && index.Error() == ErrorCode::OutOfMemory);
        CHECK(failure.code == AssetDiagnosticCode::None);
        ++allocationFailures;
    }

    CHECK(allocationFailures > 0);
}

// 범위 읽기 구현(7.1절 13): 메모리·파일 모두 파일 밖 범위는 InvalidArgument, 빈 범위·끝까지 읽기는
// 성공이고 같은 byte다. 없는 파일 열기는 NotFound다. 파일 구현으로 한 부분 읽기는 메모리와 같다.
TEST(PackageRangeReader_Bounds) {
    AssetEnv env;
    REQUIRE(env.Ready());
    RichScene scene(env.allocator);
    REQUIRE(MakeRich(env, scene));
    char path[96];
    const int count = snprintf(path, sizeof(path), "rocks-package-index-%u.rasset",
                               static_cast<unsigned>(platform::CurrentProcessId()));
    REQUIRE(count > 0);
    const StringView pathView(path, static_cast<usize>(count));
    {
        platform::File file;
        REQUIRE(file.Open(path, platform::FileOpenMode::CreateAlways));
        REQUIRE(file.WriteAll(scene.file.Data(), scene.file.Size()).status);
        REQUIRE(file.Close());
    }

    MemoryPackageRangeReader memory(scene.file.View());
    FilePackageRangeReader file;
    REQUIRE(file.Open(platform::PhysicalFileSystem(), pathView));
    PackageRangeReader* const readers[] = {&memory, &file};
    for (PackageRangeReader* reader : readers) {
        CHECK(reader->FileBytes() == scene.file.Size());
        uint8 last[4];
        CHECK(reader->ReadAt(scene.file.Size() - 4, Span<uint8>(last)));
        CHECK(memcmp(last, scene.file.Data() + scene.file.Size() - 4, 4) == 0);
        CHECK(reader->ReadAt(scene.file.Size(), Span<uint8>()));
        CHECK(reader->ReadAt(scene.file.Size() - 3, Span<uint8>(last)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(reader->ReadAt(scene.file.Size() + 1, Span<uint8>()).Error() ==
              ErrorCode::InvalidArgument);
    }

    auto fromFile = ReadPackageIndex(env.allocator, file, EditorRead());
    auto fromMemory = ReadPackageIndex(env.allocator, memory, EditorRead());
    REQUIRE(fromFile && fromMemory);
    CHECK(SameIndexContents(fromMemory.Value().contents, fromFile.Value().contents));
    FilePackageRangeReader missing;
    CHECK(missing.Open(platform::PhysicalFileSystem(), "rocks-package-index-missing.rasset")
              .Error() == ErrorCode::NotFound);
    CHECK(missing.FileBytes() == 0);
    // 다시 열면 앞 파일을 닫고 새 파일을 쓴다. 실패한 다시 열기는 크기 0이다.
    REQUIRE(file.Open(platform::PhysicalFileSystem(), pathView));
    CHECK(file.FileBytes() == scene.file.Size());
    CHECK(!file.Open(platform::PhysicalFileSystem(), "rocks-package-index-missing.rasset"));
    CHECK(file.FileBytes() == 0);
    CHECK(platform::PhysicalFileSystem().Delete(path, static_cast<usize>(count)));
}

// 파일 범위 읽기의 IO 실패(7.1절 13): 열기·크기 실패는 Open의 그 오류(크기 0), 이동·읽기 실패는
// ReadAt의 그 오류다. 빈 범위는 이동만 한다.
TEST(PackageRangeReader_FileFaults) {
    const uint8 bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const ErrorCode systemError = FileErrorCode(FaultError);
    const RangeFault openFaults[] = {RangeFault::Open, RangeFault::Size};
    for (RangeFault fault : openFaults) {
        RangeFaultSystem files(bytes);
        files.handle.fault = fault;
        FilePackageRangeReader reader;
        CHECK(reader.Open(files, "P").Error() == systemError);
        CHECK(reader.FileBytes() == 0);
    }

    const RangeFault readFaults[] = {RangeFault::Seek, RangeFault::Read};
    for (RangeFault fault : readFaults) {
        RangeFaultSystem files(bytes);
        FilePackageRangeReader reader;
        REQUIRE(reader.Open(files, "P"));
        CHECK(reader.FileBytes() == sizeof(bytes));
        files.handle.fault = fault;
        uint8 out[4];
        CHECK(reader.ReadAt(2, Span<uint8>(out)).Error() == systemError);
        CHECK(static_cast<bool>(reader.ReadAt(2, Span<uint8>())) == (fault == RangeFault::Read));
    }

    RangeFaultSystem files(bytes);
    FilePackageRangeReader reader;
    REQUIRE(reader.Open(files, "P"));
    uint8 out[4];
    REQUIRE(reader.ReadAt(4, Span<uint8>(out)));
    CHECK(memcmp(out, bytes + 4, 4) == 0);
}

// Tags 값 스키마(7.1절 13): (키, 값) 순으로 정렬해 쓰고 그대로 읽는다(목록 값은 같은 키 여러 항목),
// 빈 목록은 빈 byte다. 쓰기 거부: 빈 키·257 byte 키·4 KiB 넘는 값·잘못된 UTF-8·같은 쌍 두 번. 읽기
// 거부: 다른 스키마·더 새 Record·항목 버전 Unsupported, 깨진 Record·순서 위반·같은 쌍·키나 값 필드
// 누락·wire 종류·빈 키·키 길이·잘못된 UTF-8·값 길이·목록 필드 없음·목록 wire·항목 스키마 다름
// InvalidArgument. 할당 실패는 모든 지점에서 그 오류다.
TEST(PackageTags_WriteAndRead) {
    SystemAllocator allocator;
    auto written = WritePackageTags(allocator, RichTags);
    REQUIRE(written);
    auto read = ReadPackageTags(allocator, written.Value().View());
    REQUIRE(read);
    REQUIRE(read.Value().Size() == 3);
    CHECK(read.Value()[0].key.View() == "Kind" && read.Value()[0].value.View() == "Hero");
    CHECK(read.Value()[1].value.View() == "/Game/Old/A");
    CHECK(read.Value()[2].value.View() == "/Game/Old/B");
    auto empty = WritePackageTags(allocator, {});
    REQUIRE(empty);
    CHECK(empty.Value().IsEmpty());
    auto none = ReadPackageTags(allocator, {});
    CHECK(none && none.Value().IsEmpty());
    String longKey(allocator);
    while (longKey.View().ByteCount() <= MaxPackageTagKeyBytes) {
        REQUIRE(longKey.TryAppend("k"));
    }

    String longValue(allocator);
    while (longValue.View().ByteCount() <= PackageMaxStringBytes) {
        REQUIRE(longValue.TryAppend("v"));
    }

    const PackageTagInput invalid[][2] = {
        {{"", "x"}, {"A", "1"}},
        {{longKey.View(), "x"}, {"A", "1"}},
        {{"Key", longValue.View()}, {"A", "1"}},
        {{"K\xff", "x"}, {"A", "1"}},
        {{"Key", "v\xc3"}, {"A", "1"}},
        {{"Dup", "same"}, {"Dup", "same"}},
    };
    for (const auto& pair : invalid) {
        CHECK(WritePackageTags(allocator, pair).Error() == ErrorCode::InvalidArgument);
    }

    // 다른 스키마 Guid의 Record는 Unsupported, 깨진 byte는 InvalidArgument다.
    const uint8 value[4] = {1, 2, 3, 4};
    RecordBuilder other(allocator, Guid{0x7A, 0x6B}, 1);
    REQUIRE(other.AddField(Guid{1, 1}, WireType::U32, value));
    auto otherBytes = other.Build();
    REQUIRE(otherBytes);
    CHECK(ReadPackageTags(allocator, otherBytes.Value().View()).Error() == ErrorCode::Unsupported);
    const uint8 junk[] = {1, 2, 3};
    CHECK(ReadPackageTags(allocator, junk).Error() == ErrorCode::InvalidArgument);
    // 손으로 만든 Record(writer가 만들지 않는 모양): 항목마다 키·값 필드의 유무·wire 종류·byte와
    // 항목 버전, Record마다 목록 필드의 모양과 Record 버전을 고른다.
    struct ItemSpec {
        StringView key;
        StringView text;
        WireType keyWire = WireType::String;
        WireType valueWire = WireType::String;
        bool withKey = true;
        bool withValue = true;
        uint32 version = PackageTagsVersion;
    };
    auto item = [&](const ItemSpec& spec) noexcept {
        RecordBuilder builder(allocator, PackageTagItemSchema, spec.version);
        if (spec.withKey) {
            (void)builder.AddField(
                PackageTagKeyField, spec.keyWire,
                Span<const uint8>(reinterpret_cast<const uint8*>(spec.key.Data()),
                                  spec.key.ByteCount()));
        }

        if (spec.withValue) {
            (void)builder.AddField(
                PackageTagValueField, spec.valueWire,
                Span<const uint8>(reinterpret_cast<const uint8*>(spec.text.Data()),
                                  spec.text.ByteCount()));
        }

        auto built = builder.Build();
        return built ? Move(built.Value()) : Array<uint8>(allocator);
    };
    enum class ListShape {
        List,
        Missing,
        WrongWire
    };
    auto record = [&](Span<const Array<uint8>* const> items, ListShape shape,
                      uint32 version) noexcept {
        RecordListBuilder list(allocator);
        for (const Array<uint8>* one : items) {
            (void)list.AddItem(one->View());
        }

        RecordBuilder builder(allocator, PackageTagsSchema, version);
        if (shape == ListShape::List) {
            (void)list.Finish(builder, PackageTagsListField);
        } else {
            (void)builder.AddField(shape == ListShape::Missing ? Guid{0, 9} : PackageTagsListField,
                                   WireType::U32, value);
        }

        auto built = builder.Build();
        return built ? Move(built.Value()) : Array<uint8>(allocator);
    };
    const Array<uint8> a = item({"A", "1"});
    const Array<uint8> b = item({"B", "1"});
    const Array<uint8> noKey = item({"A", "1", WireType::String, WireType::String, false});
    const Array<uint8> noValue = item({"A", "1", WireType::String, WireType::String, true, false});
    const Array<uint8> keyBytes = item({"A", "1", WireType::Bytes});
    const Array<uint8> valueBytes = item({"A", "1", WireType::String, WireType::Bytes});
    const Array<uint8> emptyKey = item({"", "1"});
    const Array<uint8> longKeyItem = item({longKey.View(), "1"});
    const Array<uint8> badKey = item({"K\xff", "1"});
    const Array<uint8> badValue = item({"A", "v\xc3"});
    const Array<uint8> longValueItem = item({"A", longValue.View()});
    const Array<uint8> newerItem =
        item({"A", "1", WireType::String, WireType::String, true, true, PackageTagsVersion + 1});
    // 항목 스키마가 다른 항목이다.
    RecordBuilder wrongItem(allocator, Guid{0x7A, 0x6C}, 1);
    REQUIRE(wrongItem.AddField(Guid{0, 1}, WireType::U32, value));
    auto wrongItemBytes = wrongItem.Build();
    REQUIRE(wrongItemBytes);
    struct RecordCase {
        const Array<uint8>* first;
        const Array<uint8>* second;
        ListShape shape;
        uint32 version;
        ErrorCode expected;
    };
    const ErrorCode invalidArgument = ErrorCode::InvalidArgument;
    const RecordCase cases[] = {
        {&a, &b, ListShape::List, PackageTagsVersion, ErrorCode::None},
        {&b, &a, ListShape::List, PackageTagsVersion, invalidArgument},
        {&a, &a, ListShape::List, PackageTagsVersion, invalidArgument},
        {&noKey, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&noValue, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&keyBytes, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&valueBytes, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&emptyKey, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&longKeyItem, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&badKey, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&badValue, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&longValueItem, nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&wrongItemBytes.Value(), nullptr, ListShape::List, PackageTagsVersion, invalidArgument},
        {&newerItem, nullptr, ListShape::List, PackageTagsVersion, ErrorCode::Unsupported},
        {&a, nullptr, ListShape::List, PackageTagsVersion + 1, ErrorCode::Unsupported},
        {&a, nullptr, ListShape::Missing, PackageTagsVersion, invalidArgument},
        {&a, nullptr, ListShape::WrongWire, PackageTagsVersion, invalidArgument},
    };
    for (usize index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const RecordCase& one = cases[index];
        const Array<uint8>* const items[] = {one.first, one.second};
        const Array<uint8> bytes =
            record(Span<const Array<uint8>* const>(items, one.second == nullptr ? 1 : 2), one.shape,
                   one.version);
        REQUIRE(!bytes.IsEmpty());
        auto parsed = ReadPackageTags(allocator, bytes.View());
        const ErrorCode actual = parsed ? ErrorCode::None : parsed.Error();
        if (actual != one.expected) {
            printf("  tags case %zu: %u\n", index, static_cast<unsigned>(actual));
        }

        CHECK(actual == one.expected);
    }
}

// Tags 한도(7.1절 14): 태그 수를 늘려 쓰기가 받는 가장 큰 수를 찾는다(값 4 KiB 태그 — Record 값
// 한도, 값 1 byte 태그 — 목록 한도). 그 수로 쓴 것은 그대로 읽히고(쓴 것은 모두 읽힌다 — 읽기 예산
// 포함), 하나 더하면 CapacityExceeded다.
TEST(PackageTags_Limits) {
    SystemAllocator allocator;
    constexpr usize KeyTextBytes = 8;
    const usize maxCount = RecordListLimits{}.maxItems + 1;
    String large(allocator);
    while (large.View().ByteCount() < PackageMaxStringBytes) {
        REQUIRE(large.TryAppend("v"));
    }

    Array<char> keyText(allocator);
    REQUIRE(keyText.TryResize(maxCount * KeyTextBytes));
    for (usize index = 0; index < maxCount; ++index) {
        (void)snprintf(keyText.Data() + index * KeyTextBytes, KeyTextBytes, "K%06zu", index);
    }

    Array<PackageTagInput> tags(allocator);
    REQUIRE(tags.TryReserve(maxCount));
    const StringView values[] = {large.View(), "v"};
    for (StringView value : values) {
        auto writeCount = [&](usize count) noexcept {
            tags.Clear();
            for (usize index = 0; index < count; ++index) {
                (void)tags.TryPushBack(PackageTagInput{
                    StringView(keyText.Data() + index * KeyTextBytes, KeyTextBytes - 1), value});
            }

            return WritePackageTags(allocator, tags.View());
        };
        // 받는 가장 큰 수(low)와 거부하는 가장 작은 수(high)를 이분으로 찾는다.
        usize low = 1;
        usize high = maxCount;
        REQUIRE(writeCount(low));
        auto rejected = writeCount(high);
        REQUIRE(!rejected);
        CHECK(rejected.Error() == ErrorCode::CapacityExceeded);
        while (high - low > 1) {
            const usize middle = low + (high - low) / 2;
            auto written = writeCount(middle);
            if (written) {
                low = middle;
            } else {
                CHECK(written.Error() == ErrorCode::CapacityExceeded);
                high = middle;
            }
        }

        auto written = writeCount(low);
        REQUIRE(written);
        auto read = ReadPackageTags(allocator, written.Value().View());
        REQUIRE(read);
        CHECK(read.Value().Size() == low);
        CHECK(writeCount(low + 1).Error() == ErrorCode::CapacityExceeded);
        printf("  tags limit for %zu byte values: %zu\n", value.ByteCount(), low);
    }
}

TEST(PackageTags_AllocationFailures) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    auto expected = WritePackageTags(system, RichTags);
    REQUIRE(expected);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto written = WritePackageTags(failing, RichTags);
        auto read = written ? ReadPackageTags(failing, written.Value().View())
                            : Result<Array<PackageTag>>::Failure(written.Error());
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            REQUIRE(written && read);
            CHECK(written.Value().Size() == expected.Value().Size() &&
                  memcmp(written.Value().Data(), expected.Value().Data(),
                         expected.Value().Size()) == 0);
            CHECK(read.Value().Size() == 3);
            break;
        }

        CHECK(!read && read.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace package_index_tests_file
