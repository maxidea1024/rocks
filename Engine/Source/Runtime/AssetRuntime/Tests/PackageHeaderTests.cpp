#include "PackageEditing.h"

// 헤더·섹션 표의 필드 하나하나를 바꿔 정해진 진단이 나오는지
// 본다(Docs/Design/AssetSystem 5.1절 검증 순서 1–4, 5.3절 버전 축, 14절 12). 변조마다 CRC를
// 다시 맞춰 CRC가 아니라 그 필드의 규칙에 걸리게 한다.
using namespace rocks;
namespace package_header_tests_file {

using namespace rocks::asset_test;

namespace {

// 하위 객체·import·Soft·bulk가 있는 쿠킹·에디터 패키지 한 쌍이다.
struct Sample {
    explicit Sample(Allocator& allocator) noexcept
        : cooked(allocator), editor(allocator), pixels(allocator) {
    }

    Array<uint8> cooked;
    Array<uint8> editor;
    Array<uint8> pixels;
    TestBulkSource bulk;
};

bool MakeSample(AssetEnv& env, Sample& sample) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    auto other = NewPackage(registry, access, FixedId(501), PackageKind::Editor);
    auto package = NewPackage(registry, access, FixedId(500), PackageKind::Editor);
    if (!other || !package) {
        return false;
    }

    const ObjectHandle target =
        Make<TestAsset>(registry, TestAssetType(), other.Value().Handle(), "");
    const ObjectHandle asset =
        Make<TestAsset>(registry, TestAssetType(), package.Value().Handle(), "");
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), asset, "Part");
    TestAsset* pointer = Pointer<TestAsset>(registry, asset);
    if (target.IsNull() || part.IsNull() || pointer == nullptr) {
        return false;
    }

    FillCompressible(sample.pixels, 5000, 2);
    pointer->pixels = sample.bulk.Add(1, sample.pixels.View());
    pointer->source = sample.bulk.Add(2, sample.pixels.View().Slice(0, 100));
    if (!pointer->soft.TryAssign(FixedId(502), "Sub") ||
        !registry.SetReference(access, asset, pointer->hard, target) ||
        !pointer->note.TryAssign("note")) {
        return false;
    }

    PackageWriteOptions cooked = CookedOptions();
    cooked.bulkSource = &sample.bulk;
    cooked.bulkCodec = BulkCodec::Zstd;
    PackageWriteOptions editor = EditorOptions();
    editor.bulkSource = &sample.bulk;
    sample.cooked = Write(env.allocator, registry, package.Value().Handle(), cooked);
    sample.editor = Write(env.allocator, registry, package.Value().Handle(), editor);
    return !sample.cooked.IsEmpty() && !sample.editor.IsEmpty();
}

struct Expect {
    AssetEnv& env;

    // bytes 읽기가 code·error로 실패하는지 본다.
    bool Fails(const Array<uint8>& bytes, AssetDiagnosticCode code, ErrorCode error,
               PackageKind kind = PackageKind::Cooked, bool withTypes = false) const noexcept {
        PackageReadOptions options;
        options.expectedKind = kind;
        options.engineVersion = TestEngineVersion;
        options.types = withTypes ? &env.types : nullptr;
        AssetDiagnostic failure;
        auto read = ReadPackage(env.allocator, bytes.View(), options, &failure);
        return !read && read.Error() == error && failure.code == code && failure.error == error;
    }

    bool Corrupt(const Array<uint8>& bytes, PackageKind kind = PackageKind::Cooked) const noexcept {
        return Fails(bytes, AssetDiagnosticCode::CorruptPackage, ErrorCode::InvalidArgument, kind);
    }

    // 읽기가 성공하고 읽기 전용 여부가 readOnly인지 본다.
    bool Opens(const Array<uint8>& bytes, PackageKind kind, bool readOnly,
               bool withTypes = false) const noexcept {
        PackageReadOptions options;
        options.expectedKind = kind;
        options.engineVersion = TestEngineVersion;
        options.types = withTypes ? &env.types : nullptr;
        auto read = ReadPackage(env.allocator, bytes.View(), options);
        return read && read.Value().readOnly == readOnly;
    }
};

Array<uint8> WithU32(const Array<uint8>& source, usize offset, uint32 value, Allocator& allocator,
                     bool fixHeader = true) noexcept {
    Array<uint8> bytes = Copy(allocator, source.View());
    StoreLittleEndian<uint32>(bytes.Data() + offset, value);
    if (fixHeader) {
        FixHeaderCrc(bytes);
    }

    return bytes;
}

Array<uint8> WithU64(const Array<uint8>& source, usize offset, uint64 value,
                     Allocator& allocator) noexcept {
    Array<uint8> bytes = Copy(allocator, source.View());
    StoreLittleEndian<uint64>(bytes.Data() + offset, value);
    FixHeaderCrc(bytes);
    return bytes;
}

Array<uint8> WithU16(const Array<uint8>& source, usize offset, uint16 value,
                     Allocator& allocator) noexcept {
    Array<uint8> bytes = Copy(allocator, source.View());
    StoreLittleEndian<uint16>(bytes.Data() + offset, value);
    FixHeaderCrc(bytes);
    return bytes;
}

} // namespace

// 헤더 64 byte의 모든 칸(AS 5.1절 표): 크기·magic·headerCrc·flags·formatVersion·headerBytes·
// sectionCount·sectionTableOffset·fileBytes·assetId·bulkAlignment·reserved와 기대 종류.
TEST(AssetPackage_HeaderFields) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sample sample(env.allocator);
    REQUIRE(MakeSample(env, sample));
    Allocator& allocator = env.allocator;
    const Expect expect{env};
    const Array<uint8>& valid = sample.cooked;
    REQUIRE(expect.Opens(valid, PackageKind::Cooked, false, true));
    REQUIRE(expect.Opens(sample.editor, PackageKind::Editor, false, true));

    // 크기 < 64: 헤더 byte가 모자라거나 Stat 크기가 64보다 작다.
    AssetDiagnostic failure;
    CHECK(!ReadPackageHeader(valid.View().Slice(0, 63), valid.Size(), &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
    CHECK(!ReadPackageHeader(valid.View().Slice(0, 64), 63, &failure));
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage);
    CHECK(ReadPackageHeader(valid.View().Slice(0, 64), valid.Size()));
    {
        Array<uint8> empty(allocator);
        CHECK(expect.Corrupt(empty));
    }

    // magic: 네 byte 각각.
    for (usize index = 0; index < 4; ++index) {
        Array<uint8> bytes = Copy(allocator, valid.View());
        bytes[index] ^= 0x20;
        FixHeaderCrc(bytes);
        CHECK(expect.Corrupt(bytes));
    }

    // headerCrc 자체와, 덮는 범위(0–59) 안의 byte.
    {
        Array<uint8> bytes = Copy(allocator, valid.View());
        bytes[60] ^= 1;
        CHECK(expect.Corrupt(bytes));
        Array<uint8> body = Copy(allocator, valid.View());
        body[59] ^= 1;
        CHECK(expect.Corrupt(body));
    }

    // flags: 0, 둘 다, 정의되지 않은 비트(버전 판정 전에 본다 — formatVersion을 같이 망가뜨려도
    // CorruptPackage).
    const uint32 badFlags[] = {0, 3, 4, 0x80000002u};
    for (uint32 flags : badFlags) {
        CHECK(expect.Corrupt(WithU32(valid, 8, flags, allocator)));
        Array<uint8> both = WithU32(valid, 8, flags, allocator);
        StoreLittleEndian<uint16>(both.Data() + 4, PackageFormatVersion + 1);
        FixHeaderCrc(both);
        CHECK(expect.Corrupt(both));
    }

    // formatVersion: 더 새 것은 쿠킹 UnsupportedVersion·에디터 NewerVersion, 더 옛 것은
    // UnsupportedVersion(v1이 첫 버전이다). 최댓값도 같다.
    CHECK(expect.Fails(WithU16(valid, 4, PackageFormatVersion + 1, allocator),
                       AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported));
    CHECK(expect.Fails(WithU16(valid, 4, NumericLimits<uint16>::Max(), allocator),
                       AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported));
    CHECK(expect.Fails(WithU16(sample.editor, 4, PackageFormatVersion + 1, allocator),
                       AssetDiagnosticCode::NewerVersion, ErrorCode::Unsupported,
                       PackageKind::Editor));
    CHECK(expect.Fails(WithU16(sample.editor, 4, 0, allocator),
                       AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported,
                       PackageKind::Editor));

    // headerBytes ≠ 64.
    CHECK(expect.Corrupt(WithU16(valid, 6, 65, allocator)));
    CHECK(expect.Corrupt(WithU16(valid, 6, 0, allocator)));

    // sectionCount: 0, 33(상한 32 초과), 실제보다 하나 적거나 많음(fileBytes 식이 깨진다).
    const uint32 sectionCount = LoadLittleEndian<uint32>(valid.Data() + 12);
    const uint32 badCounts[] = {0, PackageMaxSections + 1, sectionCount - 1, sectionCount + 1};
    for (uint32 count : badCounts) {
        CHECK(expect.Corrupt(WithU32(valid, 12, count, allocator)));
    }

    // sectionTableOffset: 헤더 안(56), 8 정렬 아님, 64 GiB 초과, 실제와 다름.
    const uint64 table = LoadLittleEndian<uint64>(valid.Data() + 16);
    const uint64 badTables[] = {56, table + 4, PackageMaxFileBytes + 8, table + 8, table - 8};
    for (uint64 offset : badTables) {
        CHECK(expect.Corrupt(WithU64(valid, 16, offset, allocator)));
    }

    // fileBytes: 식과 다름, Stat 크기와 다름(잘림·덧붙음), 64 GiB 초과.
    CHECK(expect.Corrupt(WithU64(valid, 24, valid.Size() + 1, allocator)));
    CHECK(expect.Corrupt(WithU64(valid, 24, PackageMaxFileBytes + 1, allocator)));
    {
        Array<uint8> longer = Copy(allocator, valid.View());
        REQUIRE(longer.TryPushBack(0));
        CHECK(expect.Corrupt(longer));
        Array<uint8> shorter = Copy(allocator, valid.View());
        REQUIRE(shorter.TryResize(shorter.Size() - 4));
        CHECK(expect.Corrupt(shorter));
    }

    // assetId: null은 손상이다(바뀐 id는 CRC를 맞추면 그 id로 읽힌다 — 정체성은 header다).
    {
        Array<uint8> bytes = WithU64(valid, 32, 0, allocator);
        StoreLittleEndian<uint64>(bytes.Data() + 40, 0);
        FixHeaderCrc(bytes);
        CHECK(expect.Corrupt(bytes));
        Array<uint8> renamed = WithU64(valid, 40, 0x1234, allocator);
        auto read = ReadPackage(allocator, renamed.View(), PackageReadOptions{});
        // import·의존이 자기 id를 가리키지 않으므로 그대로 읽힌다.
        REQUIRE(read);
        CHECK(read.Value().header.assetId.value.low == 0x1234);
    }

    // bulkAlignment: 0, 2의 거듭제곱 아님, 8 미만, 65536 초과. 경계(8·65536)는 받는다 — 다른 정렬로
    // 쓴 패키지(아래 BulkAlignmentBounds)가 확인한다.
    const uint32 badAlignments[] = {0, 48, 4, 131072, 0x80000000u};
    for (uint32 alignment : badAlignments) {
        CHECK(expect.Corrupt(WithU32(valid, 48, alignment, allocator)));
    }

    // reserved 8 byte: 하나라도 0이 아니면 손상이다.
    for (usize index = 52; index < 60; ++index) {
        Array<uint8> bytes = Copy(allocator, valid.View());
        bytes[index] = 1;
        FixHeaderCrc(bytes);
        CHECK(expect.Corrupt(bytes));
    }

    // 기대 종류: 쿠킹 기대 + 에디터 패키지 = UnsupportedVersion, 에디터 기대 + 쿠킹 =
    // CorruptPackage.
    CHECK(expect.Fails(sample.editor, AssetDiagnosticCode::UnsupportedVersion,
                       ErrorCode::Unsupported));
    CHECK(expect.Corrupt(valid, PackageKind::Editor));
}

// bulkAlignment 경계(8·65536)와 에디터 기본값 64·쿠킹 기본값 4096로 쓴 패키지가 읽히고 bulk
// entry offset이 그 정렬이다.
TEST(AssetPackage_BulkAlignmentBounds) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ObjectRegistry& registry = *env.source;
    auto package = NewPackage(registry, registry.OwnerAccess(), FixedId(510), PackageKind::Cooked);
    REQUIRE(package);
    const ObjectHandle asset =
        Make<BulkAsset>(registry, IdOf<BulkAsset>(), package.Value().Handle(), "");
    BulkAsset* pointer = Pointer<BulkAsset>(registry, asset);
    REQUIRE(pointer != nullptr);
    Array<uint8> first(env.allocator);
    Array<uint8> second(env.allocator);
    FillIncompressible(first, 13, 1);
    FillIncompressible(second, 77, 2);
    TestBulkSource bulk;
    pointer->compressible = bulk.Add(1, first.View());
    pointer->incompressible = bulk.Add(2, second.View());
    const uint32 alignments[] = {PackageMinBulkAlignment, PackageMaxBulkAlignment, 0};
    for (uint32 alignment : alignments) {
        PackageWriteOptions options = CookedOptions();
        options.bulkSource = &bulk;
        options.bulkAlignment = alignment;
        Array<uint8> bytes = Write(env.allocator, registry, package.Value().Handle(), options);
        REQUIRE(!bytes.IsEmpty());
        auto contents = ReadPackage(env.allocator, bytes.View(), PackageReadOptions{});
        REQUIRE(contents);
        const uint32 expected = alignment == 0 ? PackageDefaultCookedBulkAlignment : alignment;
        CHECK(contents.Value().header.bulkAlignment == expected);
        REQUIRE(contents.Value().bulkEntries.Size() == 2);
        CHECK(contents.Value().bulkEntries[1].offset ==
              expected * ((13 + expected - 1) / expected));
        usize row = 0;
        uint64 offset = 0;
        uint64 size = 0;
        REQUIRE(FindSection(bytes.View(), PackageSectionKind::Bulk, row, offset, size));
        CHECK(offset % expected == 0);
    }
}

// 섹션 표(AS 5.1절 검증 3·4): tableCrc, kind 순서·중복, flags 비트, offset·bytes 범위, 정렬·패딩,
// 빈 섹션, 비-Bulk 64 MiB 상한, PerEntryChecksum의 CRC 칸, 필수 섹션·Bulk 짝·쿠킹 금지 섹션, 모르는
// kind(Required 여부 × 종류), 아는 kind의 flags, schemaVersion(종류 × 높낮이 × Required), 섹션 CRC.
TEST(AssetPackage_SectionTableFields) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sample sample(env.allocator);
    REQUIRE(MakeSample(env, sample));
    Allocator& allocator = env.allocator;
    const Expect expect{env};
    const Array<uint8>& valid = sample.cooked;
    const usize table = static_cast<usize>(LoadLittleEndian<uint64>(valid.Data() + 16));
    const uint32 count = LoadLittleEndian<uint32>(valid.Data() + 12);
    auto row = [&](uint32 index) noexcept { return table + PackageSectionEntryBytes * index; };
    auto tableEdit = [&](const Array<uint8>& source, usize at, uint64 value, bool wide) noexcept {
        Array<uint8> bytes = Copy(allocator, source.View());
        if (wide) {
            StoreLittleEndian<uint64>(bytes.Data() + at, value);
        } else {
            StoreLittleEndian<uint32>(bytes.Data() + at, static_cast<uint32>(value));
        }

        FixTableCrc(bytes);
        return bytes;
    };

    // tableCrc: 표 byte 하나·CRC 칸 자체.
    {
        Array<uint8> bytes = Copy(allocator, valid.View());
        bytes[row(0) + 28] ^= 1;
        CHECK(expect.Corrupt(bytes));
        Array<uint8> crc = Copy(allocator, valid.View());
        crc[crc.Size() - 4] ^= 1;
        CHECK(expect.Corrupt(crc));
    }

    // kind: 중복(두 번째를 첫 번째와 같게), 역순.
    CHECK(expect.Corrupt(
        tableEdit(valid, row(1), LoadLittleEndian<uint32>(valid.Data() + row(0)), false)));
    CHECK(expect.Corrupt(tableEdit(valid, row(1), 0, false)));

    // flags: 정의되지 않은 비트(4·0x80000000).
    CHECK(expect.Corrupt(tableEdit(valid, row(0) + 4, 1 | 4, false)));
    CHECK(expect.Corrupt(tableEdit(valid, row(0) + 4, 1 | 0x80000000u, false)));

    // offset: 헤더 안, 섹션 표 너머, 정렬 어긋남(앞 섹션 끝 정렬 값이 아님 — 빈 틈).
    CHECK(expect.Corrupt(tableEdit(valid, row(0) + 8, 0, true)));
    CHECK(expect.Corrupt(tableEdit(valid, row(0) + 8, table + 8, true)));
    const uint64 firstOffset = LoadLittleEndian<uint64>(valid.Data() + row(1) + 8);
    CHECK(expect.Corrupt(tableEdit(valid, row(1) + 8, firstOffset + 8, true)));
    CHECK(expect.Corrupt(tableEdit(valid, row(1) + 8, firstOffset + 1, true)));

    // bytes: 섹션 표를 넘음, 0(비-Bulk), 한 byte 짧음(다음 offset이 정렬 값과 달라진다 — 또는 CRC).
    CHECK(expect.Corrupt(tableEdit(valid, row(0) + 16, table, true)));
    CHECK(expect.Corrupt(tableEdit(valid, row(0) + 16, NumericLimits<uint64>::Max(), true)));
    CHECK(expect.Corrupt(tableEdit(valid, row(0) + 16, 0, true)));

    // 마지막 섹션 끝과 섹션 표 사이 패딩의 0이 아닌 byte, 표 offset이 마지막 끝 정렬 값이 아님.
    {
        const uint64 lastOffset = LoadLittleEndian<uint64>(valid.Data() + row(count - 1) + 8);
        const uint64 lastBytes = LoadLittleEndian<uint64>(valid.Data() + row(count - 1) + 16);
        const usize end = static_cast<usize>(lastOffset + lastBytes);
        if (end < table) {
            Array<uint8> bytes = Copy(allocator, valid.View());
            bytes[end] = 7;
            CHECK(expect.Corrupt(bytes));
        }

        // 마지막 섹션을 8 byte 줄이면(CRC는 맞춤) 표 offset이 정렬 값이 아니게 된다.
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(valid.View()));
        Array<uint8> rebuilt = edit.Build();
        REQUIRE(SameBytes(rebuilt.View(), valid.View()));
        Array<uint8> shifted = Copy(allocator, valid.View());
        StoreLittleEndian<uint64>(shifted.Data() + row(count - 1) + 16, lastBytes - 8);
        FixTableCrc(shifted);
        CHECK(expect.Corrupt(shifted));
    }

    // PerEntryChecksum(Bulk)의 표 CRC 칸은 0이어야 한다.
    {
        for (uint32 index = 0; index < count; ++index) {
            if (LoadLittleEndian<uint32>(valid.Data() + row(index)) ==
                static_cast<uint32>(PackageSectionKind::Bulk)) {
                CHECK(expect.Corrupt(tableEdit(valid, row(index) + 24, 1, false)));
            }
        }
    }

    // 필수 섹션 생략(Summary·Exports·Objects), Bulk·BulkIndex 짝 깨짐, 쿠킹의 ImportRecord·Tags.
    const PackageSectionKind required[] = {PackageSectionKind::Summary, PackageSectionKind::Exports,
                                           PackageSectionKind::Objects, PackageSectionKind::Bulk,
                                           PackageSectionKind::BulkIndex};
    for (PackageSectionKind kind : required) {
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(valid.View()));
        REQUIRE(edit.Remove(kind));
        CHECK(expect.Corrupt(edit.Build()));
    }

    {
        Array<uint8> recordBytes(allocator);
        RecordBuilder record(allocator, Guid{1, 1}, 1);
        auto built = record.Build();
        REQUIRE(built);
        const PackageSectionKind editorOnly[] = {PackageSectionKind::ImportRecord,
                                                 PackageSectionKind::Tags};
        for (PackageSectionKind kind : editorOnly) {
            EditablePackage edit(allocator);
            REQUIRE(edit.Parse(valid.View()));
            REQUIRE(edit.Insert(static_cast<uint32>(kind), 0, built.Value().View()) != nullptr);
            CHECK(expect.Corrupt(edit.Build()));
            // 에디터 패키지에는 둘 수 있다(내용은 Record envelope만 본다).
            EditablePackage editor(allocator);
            REQUIRE(editor.Parse(sample.editor.View()));
            REQUIRE(editor.Insert(static_cast<uint32>(kind), 0, built.Value().View()) != nullptr);
            CHECK(expect.Opens(editor.Build(), PackageKind::Editor, false));
            // Record가 아닌 내용은 손상이다.
            EditablePackage bad(allocator);
            REQUIRE(bad.Parse(sample.editor.View()));
            const uint8 junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            REQUIRE(bad.Insert(static_cast<uint32>(kind), 0, Span<const uint8>(junk, 8)) !=
                    nullptr);
            CHECK(expect.Corrupt(bad.Build(), PackageKind::Editor));
        }
    }

    // 모르는 kind(10): Required면 쿠킹 UnsupportedVersion·에디터 NewerVersion, 아니면 쿠킹
    // UnsupportedVersion·에디터 읽기 전용(건너뜀).
    {
        const uint8 payload[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        const uint32 unknown = PackageKnownSectionKinds + 1;
        const uint32 requiredFlag = static_cast<uint32>(PackageSectionFlags::Required);
        const uint32 flagsValues[] = {requiredFlag, 0u};
        for (uint32 flags : flagsValues) {
            EditablePackage cooked(allocator);
            REQUIRE(cooked.Parse(valid.View()));
            REQUIRE(cooked.Insert(unknown, flags, Span<const uint8>(payload, 8)) != nullptr);
            CHECK(expect.Fails(cooked.Build(), AssetDiagnosticCode::UnsupportedVersion,
                               ErrorCode::Unsupported));
            EditablePackage editor(allocator);
            REQUIRE(editor.Parse(sample.editor.View()));
            REQUIRE(editor.Insert(unknown, flags, Span<const uint8>(payload, 8)) != nullptr);
            if (flags != 0) {
                CHECK(expect.Fails(editor.Build(), AssetDiagnosticCode::NewerVersion,
                                   ErrorCode::Unsupported, PackageKind::Editor));
            } else {
                CHECK(expect.Opens(editor.Build(), PackageKind::Editor, true));
            }
        }
    }

    // 아는 kind의 flags가 표와 다름(Summary의 Required 없음, Bulk의 PerEntryChecksum 없음).
    for (uint32 index = 0; index < count; ++index) {
        const uint32 kind = LoadLittleEndian<uint32>(valid.Data() + row(index));
        const uint32 flags = LoadLittleEndian<uint32>(valid.Data() + row(index) + 4);
        const uint32 changed =
            kind == static_cast<uint32>(PackageSectionKind::Bulk) ? 1u : (flags ^ 1u);
        Array<uint8> bytes = tableEdit(valid, row(index) + 4, changed, false);
        if (kind == static_cast<uint32>(PackageSectionKind::Bulk)) {
            // PerEntryChecksum을 빼면 표 CRC 칸(0)이 섹션 CRC와 달라 그 검사에도 걸린다.
            CHECK(expect.Corrupt(bytes));
        } else {
            CHECK(expect.Corrupt(bytes));
        }
    }

    // schemaVersion: 쿠킹은 정확히 일치(높든 낮든 UnsupportedVersion), 에디터는 낮으면
    // UnsupportedVersion(v1이 첫 버전), 높으면 Required는 NewerVersion·아니면 읽기 전용.
    for (uint32 index = 0; index < count; ++index) {
        CHECK(expect.Fails(tableEdit(valid, row(index) + 28, 2, false),
                           AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported));
        CHECK(expect.Fails(tableEdit(valid, row(index) + 28, 0, false),
                           AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported));
    }

    {
        const usize editorTable =
            static_cast<usize>(LoadLittleEndian<uint64>(sample.editor.Data() + 16));
        const uint32 editorCount = LoadLittleEndian<uint32>(sample.editor.Data() + 12);
        for (uint32 index = 0; index < editorCount; ++index) {
            const usize at = editorTable + PackageSectionEntryBytes * index;
            CHECK(expect.Fails(tableEdit(sample.editor, at + 28, 0, false),
                               AssetDiagnosticCode::UnsupportedVersion, ErrorCode::Unsupported,
                               PackageKind::Editor));
            CHECK(expect.Fails(tableEdit(sample.editor, at + 28, 2, false),
                               AssetDiagnosticCode::NewerVersion, ErrorCode::Unsupported,
                               PackageKind::Editor));
        }

        RecordBuilder record(allocator, Guid{1, 1}, 1);
        auto built = record.Build();
        REQUIRE(built);
        EditablePackage editor(allocator);
        REQUIRE(editor.Parse(sample.editor.View()));
        EditableSection* tags =
            editor.Insert(static_cast<uint32>(PackageSectionKind::Tags), 0, built.Value().View());
        REQUIRE(tags != nullptr);
        tags->schemaVersion = 2;
        CHECK(expect.Opens(editor.Build(), PackageKind::Editor, true));
    }

    // 섹션 CRC: 비-Bulk 섹션마다 내용 byte 하나.
    for (uint32 index = 0; index < count; ++index) {
        if (LoadLittleEndian<uint32>(valid.Data() + row(index)) ==
            static_cast<uint32>(PackageSectionKind::Bulk)) {
            continue;
        }

        Array<uint8> bytes = Copy(allocator, valid.View());
        bytes[static_cast<usize>(LoadLittleEndian<uint64>(valid.Data() + row(index) + 8))] ^= 0x40;
        CHECK(expect.Corrupt(bytes));
    }
}

// 비-Bulk 섹션 상한(64 MiB): 표의 bytes가 상한을 넘으면 내용을 보기 전에 손상이다. 모르는
// 비-Required 섹션을 상한 + 8 byte로 넣은 에디터 패키지로 본다(같은 크기 이하는 읽기 전용으로
// 열린다).
TEST(AssetPackage_SectionSizeLimit) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sample sample(env.allocator);
    REQUIRE(MakeSample(env, sample));
    const Expect expect{env};
    Array<uint8> big(env.allocator);
    REQUIRE(big.TryResize(static_cast<usize>(PackageMaxSectionBytes) + 8));
    EditablePackage edit(env.allocator);
    REQUIRE(edit.Parse(sample.editor.View()));
    REQUIRE(edit.Insert(PackageKnownSectionKinds + 1, 0, big.View()) != nullptr);
    CHECK(expect.Corrupt(edit.Build(), PackageKind::Editor));
    REQUIRE(edit.sections.Size() > 0);
    for (EditableSection& section : edit.sections) {
        if (section.kind == PackageKnownSectionKinds + 1) {
            REQUIRE(section.bytes.TryResize(static_cast<usize>(PackageMaxSectionBytes)));
        }
    }

    CHECK(expect.Opens(edit.Build(), PackageKind::Editor, true));
}
} // namespace package_header_tests_file
