#include "PackageEditing.h"

// 섹션 내용 시험이다(Docs/Design/AssetSystem.ko.md 5.2절 표, 5.3절 타입·커스텀 버전 축, 5.7절
// 구현에서 정한 점): Summary Record의 모든 필드, Exports·ObjectImports·Dependencies·BulkIndex 고정
// 배치의 모든 칸과 표 사이 규칙, bulk 해제. 변조마다 섹션을 writer 규칙대로 다시
// 배치(EditablePackage)해 CRC·배치가 아니라 그 내용 규칙에 걸리게 한다.
using namespace rocks;
using namespace rocks::asset_test;
using namespace rocks::package_detail;

namespace {

// 대표 에셋 + 하위 객체 Part, 다른 패키지 Hard 하나·Soft 하나·bulk 둘(Zstd)인 패키지다.
struct Base {
    explicit Base(Allocator& allocator) noexcept
        : cooked(allocator), editor(allocator), pixels(allocator), other(allocator) {
    }

    Array<uint8> cooked;
    Array<uint8> editor;
    Array<uint8> pixels;
    Array<uint8> other;
    TestBulkSource bulk;
};

bool MakeBase(AssetEnv& env, Base& base) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    auto dependency = NewPackage(registry, access, FixedId(601), PackageKind::Editor);
    auto package = NewPackage(registry, access, FixedId(600), PackageKind::Editor);
    if (!dependency || !package) {
        return false;
    }

    const ObjectHandle target =
        Make<TestAsset>(registry, TestAssetType(), dependency.Value().Handle(), "");
    const ObjectHandle asset =
        Make<TestAsset>(registry, TestAssetType(), package.Value().Handle(), "");
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), asset, "Part");
    TestAsset* pointer = Pointer<TestAsset>(registry, asset);
    TestPart* partPointer = Pointer<TestPart>(registry, part);
    if (target.IsNull() || pointer == nullptr || partPointer == nullptr) {
        return false;
    }

    FillCompressible(base.pixels, 6000, 4);
    FillIncompressible(base.other, 300, 4);
    pointer->pixels = base.bulk.Add(1, base.pixels.View());
    partPointer->data = base.bulk.Add(2, base.other.View());
    if (!pointer->soft.TryAssign(FixedId(602), StringView()) ||
        !registry.SetReference(access, asset, pointer->hard, target)) {
        return false;
    }

    PackageWriteOptions cooked = CookedOptions();
    cooked.bulkSource = &base.bulk;
    cooked.bulkCodec = BulkCodec::Zstd;
    PackageWriteOptions editor = EditorOptions();
    editor.bulkSource = &base.bulk;
    base.cooked = Write(env.allocator, registry, package.Value().Handle(), cooked);
    base.editor = Write(env.allocator, registry, package.Value().Handle(), editor);
    return !base.cooked.IsEmpty() && !base.editor.IsEmpty();
}

// 읽기 결과 판정이다.
struct Reader {
    AssetEnv& env;

    AssetDiagnostic Read(const Array<uint8>& bytes, PackageKind kind, bool& ok,
                         bool& readOnly) const noexcept {
        PackageReadOptions options;
        options.expectedKind = kind;
        options.engineVersion = TestEngineVersion;
        options.types = &env.types;
        AssetDiagnostic failure;
        auto read = ReadPackage(env.allocator, bytes.View(), options, &failure);
        ok = read.IsOk();
        readOnly = ok && read.Value().readOnly;
        return failure;
    }

    // section 내용 손상(CorruptPackage, 그 섹션 번호)인지다.
    bool Corrupt(const Array<uint8>& bytes, PackageSectionKind section,
                 PackageKind kind = PackageKind::Cooked) const noexcept {
        bool ok = false;
        bool readOnly = false;
        const AssetDiagnostic failure = Read(bytes, kind, ok, readOnly);
        return !ok && failure.code == AssetDiagnosticCode::CorruptPackage &&
               failure.error == ErrorCode::InvalidArgument &&
               failure.section == static_cast<uint32>(section);
    }

    bool Version(const Array<uint8>& bytes, AssetDiagnosticCode code,
                 PackageKind kind = PackageKind::Cooked) const noexcept {
        bool ok = false;
        bool readOnly = false;
        const AssetDiagnostic failure = Read(bytes, kind, ok, readOnly);
        return !ok && failure.code == code && failure.error == ErrorCode::Unsupported;
    }

    bool Opens(const Array<uint8>& bytes, PackageKind kind, bool expectReadOnly) const noexcept {
        bool ok = false;
        bool readOnly = false;
        (void)Read(bytes, kind, ok, readOnly);
        return ok && readOnly == expectReadOnly;
    }
};

// 섹션 하나를 바꾼 패키지다.
Array<uint8> WithSection(Allocator& allocator, const Array<uint8>& file, PackageSectionKind kind,
                         Span<const uint8> bytes) noexcept {
    EditablePackage edit(allocator);
    if (!edit.Parse(file.View()) || !edit.Replace(kind, bytes)) {
        return Array<uint8>(allocator);
    }

    return edit.Build();
}

} // namespace

// Summary(AS 5.2절, 5.7절 1): 스키마 GUID·버전, 필드 수(에디터 4·쿠킹 5), 버전 표의
// 모양(RecordList· 항목 필드·wire)과 정렬·중복, 엔진 버전 Record(필드·범위·wire)와 compatibleWith
// 규칙, 프로파일 id(wire·빈 값·4 KiB·UTF-8)·설정 SHA-256(wire·크기)·쿠커 버전(wire). 고장 없는
// 재구성은 writer byte와 같다.
TEST(AssetPackage_SummaryFields) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Base base(env.allocator);
    REQUIRE(MakeBase(env, base));
    const Reader reader{env};
    Allocator& allocator = env.allocator;
    auto valid = ReadPackage(allocator, base.cooked.View(), PackageReadOptions{});
    REQUIRE(valid);
    const PackageCustomVersion customs[] = {{Guid{1, 1}, 1}, {Guid{2, 2}, 4}};
    const bool editorValues[] = {false, true};
    for (bool editor : editorValues) {
        const Array<uint8>& file = editor ? base.editor : base.cooked;
        const PackageKind kind = editor ? PackageKind::Editor : PackageKind::Cooked;
        SummaryInput input{valid.Value().summary.types.View(), Span<const PackageCustomVersion>(),
                           editor};
        // 고장 없음 = writer byte.
        {
            usize row = 0;
            uint64 offset = 0;
            uint64 size = 0;
            REQUIRE(FindSection(file.View(), PackageSectionKind::Summary, row, offset, size));
            Array<uint8> same = SummaryBytes(allocator, input, SummaryFault::None);
            CHECK(SameBytes(same.View(), file.View().Slice(static_cast<usize>(offset),
                                                           static_cast<usize>(size))));
        }

        for (uint32 value = 1; value < static_cast<uint32>(SummaryFault::Count); ++value) {
            const SummaryFault fault = static_cast<SummaryFault>(value);
            if ((EditorFault(fault) && !editor) || (CookedFault(fault) && editor)) {
                continue;
            }

            SummaryInput faulty = input;
            if (fault == SummaryFault::CustomsUnsorted) {
                faulty.customs = customs;
            }

            Array<uint8> summary = SummaryBytes(allocator, faulty, fault);
            REQUIRE(!summary.IsEmpty());
            CHECK(reader.Corrupt(
                WithSection(allocator, file, PackageSectionKind::Summary, summary.View()),
                PackageSectionKind::Summary, kind));
            // 항목 고장은 커스텀 버전 표(같은 읽기 함수의 다른 인스턴스)에서도 같은 진단이다.
            if (fault >= SummaryFault::ItemFieldCount && fault <= SummaryFault::ItemVersionWire) {
                SummaryInput custom = input;
                custom.customs = customs;
                custom.faultCustoms = true;
                Array<uint8> bytes = SummaryBytes(allocator, custom, fault);
                REQUIRE(!bytes.IsEmpty());
                CHECK(reader.Corrupt(
                    WithSection(allocator, file, PackageSectionKind::Summary, bytes.View()),
                    PackageSectionKind::Summary, kind));
            }
        }
    }
}

// Summary 타입 표 = export 타입 집합(AS 5.1절 — 모든 export의 typeId가 표에 있고 표에 남는 타입이
// 없다)과 타입 축(5.3절): 쿠킹은 등록 버전과 정확히 같아야 하고(미등록·다름 UnsupportedVersion),
// 에디터는 낮으면 그대로·높거나 미등록이면 읽기 전용이다. 커스텀 버전 축: 쿠킹은 표 전체 일치,
// 에디터는 더 새 것·모르는 id면 읽기 전용, 낮거나 같으면 그대로.
TEST(AssetPackage_TypeAndCustomVersionAxes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Base base(env.allocator);
    REQUIRE(MakeBase(env, base));
    const Reader reader{env};
    Allocator& allocator = env.allocator;
    const bool editorValues[] = {false, true};
    for (bool editor : editorValues) {
        const Array<uint8>& file = editor ? base.editor : base.cooked;
        const PackageKind kind = editor ? PackageKind::Editor : PackageKind::Cooked;
        PackageReadOptions plain;
        plain.expectedKind = kind;
        auto valid = ReadPackage(allocator, file.View(), plain);
        REQUIRE(valid);
        REQUIRE(valid.Value().summary.types.Size() == 2);
        Array<PackageTypeVersion> types = CopyEntries(allocator, valid.Value().summary.types);
        auto summaryWith = [&](Span<const PackageTypeVersion> rows,
                               Span<const PackageCustomVersion> custom) noexcept {
            SummaryInput input{rows, custom, editor};
            Array<uint8> summary = SummaryBytes(allocator, input, SummaryFault::None);
            return WithSection(allocator, file, PackageSectionKind::Summary, summary.View());
        };

        // 표에서 하나를 빼거나(export 타입이 표에 없음) 더하면(남는 타입) 손상이다.
        CHECK(reader.Corrupt(summaryWith(types.View().Slice(0, 1), {}), PackageSectionKind::Summary,
                             kind));
        {
            Array<PackageTypeVersion> extra = CopyEntries(allocator, types);
            REQUIRE(
                extra.TryPushBack(PackageTypeVersion{Guid{NumericLimits<uint64>::Max(), 1}, 1}));
            CHECK(reader.Corrupt(summaryWith(extra.View(), {}), PackageSectionKind::Summary, kind));
        }

        // 버전 더 높음·더 낮음.
        const int32 deltaValues[] = {1, -1};
        for (int32 delta : deltaValues) {
            Array<PackageTypeVersion> changed = CopyEntries(allocator, types);
            changed[0].version =
                static_cast<uint32>(static_cast<int32>(changed[0].version) + delta);
            Array<uint8> bytes = summaryWith(changed.View(), {});
            if (!editor) {
                CHECK(reader.Version(bytes, AssetDiagnosticCode::UnsupportedVersion));
            } else {
                CHECK(reader.Opens(bytes, kind, delta > 0));
            }
        }

        // 커스텀 버전.
        const PackageCustomVersion stored[] = {{Guid{1, 1}, 3}, {Guid{2, 2}, 5}};
        Array<uint8> withCustoms = summaryWith(types.View(), stored);
        PackageReadOptions options;
        options.expectedKind = kind;
        options.engineVersion = TestEngineVersion;
        const PackageCustomVersion same[] = {{Guid{1, 1}, 3}, {Guid{2, 2}, 5}};
        const PackageCustomVersion lower[] = {{Guid{1, 1}, 2}, {Guid{2, 2}, 5}};
        const PackageCustomVersion higher[] = {{Guid{1, 1}, 4}, {Guid{2, 2}, 5}};
        const PackageCustomVersion missing[] = {{Guid{1, 1}, 3}};
        const PackageCustomVersion more[] = {{Guid{1, 1}, 3}, {Guid{2, 2}, 5}, {Guid{3, 3}, 1}};
        const PackageCustomVersion otherId[] = {{Guid{0, 9}, 3}, {Guid{2, 2}, 5}};
        struct Case {
            Span<const PackageCustomVersion> current;
            bool cookedOk;
            bool editorReadOnly;
        };

        const Case cases[] = {{same, true, false},    {lower, false, true}, {higher, false, false},
                              {missing, false, true}, {more, false, false}, {{}, false, true},
                              {otherId, false, true}};
        for (const Case& item : cases) {
            options.customVersions = item.current;
            AssetDiagnostic failure;
            auto read = ReadPackage(allocator, withCustoms.View(), options, &failure);
            if (!editor) {
                CHECK(read.IsOk() == item.cookedOk);
                if (!read) {
                    CHECK(failure.code == AssetDiagnosticCode::UnsupportedVersion);
                }
            } else {
                REQUIRE(read);
                CHECK(read.Value().readOnly == item.editorReadOnly);
            }
        }
    }

    // 미등록 타입: export 타입과 표를 함께 모르는 id로 바꾼다.
    const bool unknownEditorValues[] = {false, true};
    for (bool editor : unknownEditorValues) {
        const Array<uint8>& file = editor ? base.editor : base.cooked;
        const PackageKind kind = editor ? PackageKind::Editor : PackageKind::Cooked;
        PackageReadOptions plain;
        plain.expectedKind = kind;
        auto valid = ReadPackage(allocator, file.View(), plain);
        REQUIRE(valid);
        Array<PackageExportEntry> exports = CopyEntries(allocator, valid.Value().exports);
        const TypeId unknown{NumericLimits<uint64>::Max(), 2};
        exports[1].type = unknown;
        Array<PackageTypeVersion> types(allocator);
        REQUIRE(types.TryPushBack(PackageTypeVersion{exports[0].type, 1}));
        for (const PackageTypeVersion& row : valid.Value().summary.types) {
            if (row.type == exports[0].type) {
                types[0].version = row.version;
            }
        }

        REQUIRE(types.TryPushBack(PackageTypeVersion{unknown, 1}));
        SummaryInput input{types.View(), {}, editor};
        Array<uint8> summary = SummaryBytes(allocator, input, SummaryFault::None);
        Array<uint8> exportBytes =
            ExportsBytes(allocator, exports.View(), static_cast<uint32>(exports.Size()));
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(file.View()));
        REQUIRE(edit.Replace(PackageSectionKind::Summary, summary.View()));
        REQUIRE(edit.Replace(PackageSectionKind::Exports, exportBytes.View()));
        if (!editor) {
            CHECK(reader.Version(edit.Build(), AssetDiagnosticCode::UnsupportedVersion));
        } else {
            CHECK(reader.Opens(edit.Build(), kind, true));
        }
    }
}

// Exports(AS 5.1절 유일성, 5.7절 1·3·4): count(0·상한 초과·실제와 다름), 잘림·남는 byte, 이름 길이,
// flags(정의되지 않은 비트·예약 EditorOnly·Primary 위치), 타입 0, 대표 에셋 규칙(빈 이름·outer 0·
// Primary), 하위 export 규칙(이름·outer 범위), 깊이 우선 순서, 같은 Outer 안 이름 순서·중복.
TEST(AssetPackage_ExportsFields) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Base base(env.allocator);
    REQUIRE(MakeBase(env, base));
    const Reader reader{env};
    Allocator& allocator = env.allocator;
    auto valid = ReadPackage(allocator, base.cooked.View(), PackageReadOptions{});
    REQUIRE(valid);
    const PackageExportEntry rep = valid.Value().exports[0];
    const TypeId part = valid.Value().exports[1].type;
    auto with = [&](Span<const PackageExportEntry> entries, uint32 count) noexcept {
        Array<uint8> bytes = ExportsBytes(allocator, entries, count);
        return WithSection(allocator, base.cooked, PackageSectionKind::Exports, bytes.View());
    };
    auto corrupt = [&](Span<const PackageExportEntry> entries) noexcept {
        return reader.Corrupt(with(entries, static_cast<uint32>(entries.Size())),
                              PackageSectionKind::Exports);
    };

    const PackageExportEntry valid2[] = {rep, {"Part", part, 1, 0}};
    CHECK(reader.Opens(with(valid2, 2), PackageKind::Cooked, false));
    CHECK(reader.Corrupt(with(Span<const PackageExportEntry>(), 0), PackageSectionKind::Exports));
    CHECK(reader.Corrupt(with(valid2, PackageMaxExports + 1), PackageSectionKind::Exports));
    CHECK(reader.Corrupt(with(valid2, 3), PackageSectionKind::Exports));
    CHECK(reader.Corrupt(with(valid2, 1), PackageSectionKind::Exports));
    {
        Array<uint8> bytes = ExportsBytes(allocator, valid2, 2);
        REQUIRE(bytes.TryPushBack(0));
        CHECK(reader.Corrupt(
            WithSection(allocator, base.cooked, PackageSectionKind::Exports, bytes.View()),
            PackageSectionKind::Exports));
    }

    {
        Array<char> name(allocator);
        REQUIRE(name.TryResize(PackageMaxStringBytes + 1));
        for (char& value : name) {
            value = 'n';
        }

        const PackageExportEntry longName[] = {rep,
                                               {StringView(name.Data(), name.Size()), part, 1, 0}};
        CHECK(corrupt(longName));
    }

    const PackageExportEntry flagBit[] = {{rep.name, rep.type, 0, 1 | 4}, valid2[1]};
    const PackageExportEntry editorOnly[] = {rep, {"Part", part, 1, 2}};
    const PackageExportEntry twoPrimary[] = {rep, {"Part", part, 1, 1}};
    const PackageExportEntry noPrimary[] = {{rep.name, rep.type, 0, 0}, valid2[1]};
    const PackageExportEntry namedRep[] = {{"Rep", rep.type, 0, 1}, valid2[1]};
    const PackageExportEntry outerRep[] = {{rep.name, rep.type, 1, 1}, valid2[1]};
    const PackageExportEntry nullType[] = {rep, {"Part", TypeId{}, 1, 0}};
    const PackageExportEntry emptyName[] = {rep, {"", part, 1, 0}};
    const PackageExportEntry outerZero[] = {rep, {"Part", part, 0, 0}};
    const PackageExportEntry outerSelf[] = {rep, {"Part", part, 2, 0}};
    const PackageExportEntry outerAhead[] = {rep, {"A", part, 3, 0}, {"B", part, 1, 0}};
    // 깊이 우선 위반: B(outer 1) 뒤에 A(2)의 자식 X가 온다.
    const PackageExportEntry notDepthFirst[] = {
        rep, {"A", part, 1, 0}, {"B", part, 1, 0}, {"X", part, 2, 0}};
    const PackageExportEntry unordered[] = {rep, {"B", part, 1, 0}, {"A", part, 1, 0}};
    const PackageExportEntry duplicate[] = {rep, {"A", part, 1, 0}, {"A", part, 1, 0}};
    const PackageExportEntry prefixOrder[] = {rep, {"AB", part, 1, 0}, {"A", part, 1, 0}};
    CHECK(corrupt(flagBit));
    CHECK(corrupt(editorOnly));
    CHECK(corrupt(twoPrimary));
    CHECK(corrupt(noPrimary));
    CHECK(corrupt(namedRep));
    CHECK(corrupt(outerRep));
    CHECK(corrupt(nullType));
    CHECK(corrupt(emptyName));
    CHECK(corrupt(outerZero));
    CHECK(corrupt(outerSelf));
    CHECK(corrupt(outerAhead));
    CHECK(corrupt(notDepthFirst));
    CHECK(corrupt(unordered));
    CHECK(corrupt(duplicate));
    CHECK(corrupt(prefixOrder));
    // 깊이 우선이고 이름 순서가 맞는 깊은 사슬은 읽힌다(그래프 대조 전 — 표 규칙만).
    const PackageExportEntry deep[] = {
        rep, {"A", part, 1, 0}, {"X", part, 2, 0}, {"Y", part, 3, 0}, {"B", part, 1, 0}};
    PackageReadOptions plain;
    CHECK(ReadPackage(allocator, with(deep, 5).View(), plain));
}

// ObjectImports(AS 5.1절 유일성, 5.2절): count(0·상한·실제와 다름), 잘림·남는 byte, 에셋 null·자기
// 에셋, 타입 0, outer import(자기보다 앞·같은 에셋·이름 있는 항목)·outer가 있는 항목의 이름, 객체
// 경로 상한, (assetId, 객체 경로) 엄격 오름차순.
TEST(AssetPackage_ObjectImportsFields) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Base base(env.allocator);
    REQUIRE(MakeBase(env, base));
    const Reader reader{env};
    Allocator& allocator = env.allocator;
    auto valid = ReadPackage(allocator, base.cooked.View(), PackageReadOptions{});
    REQUIRE(valid);
    REQUIRE(valid.Value().imports.Size() == 1);
    const PackageImportEntry first = valid.Value().imports[0];
    const TypeId type = first.type;
    auto with = [&](Span<const PackageImportEntry> entries, uint32 count) noexcept {
        Array<uint8> bytes = ImportsBytes(allocator, entries, count);
        return WithSection(allocator, base.cooked, PackageSectionKind::ObjectImports, bytes.View());
    };
    auto corrupt = [&](Span<const PackageImportEntry> entries) noexcept {
        return reader.Corrupt(with(entries, static_cast<uint32>(entries.Size())),
                              PackageSectionKind::ObjectImports);
    };

    const PackageImportEntry one[] = {first};
    CHECK(reader.Opens(with(one, 1), PackageKind::Cooked, false));
    CHECK(reader.Corrupt(with(Span<const PackageImportEntry>(), 0),
                         PackageSectionKind::ObjectImports));
    CHECK(
        reader.Corrupt(with(one, PackageMaxObjectImports + 1), PackageSectionKind::ObjectImports));
    CHECK(reader.Corrupt(with(one, 2), PackageSectionKind::ObjectImports));
    {
        Array<uint8> bytes = ImportsBytes(allocator, one, 1);
        REQUIRE(bytes.TryPushBack(0));
        CHECK(reader.Corrupt(
            WithSection(allocator, base.cooked, PackageSectionKind::ObjectImports, bytes.View()),
            PackageSectionKind::ObjectImports));
    }

    const AssetId a = first.asset;
    const AssetId later{Guid{a.value.high, a.value.low + 1}};
    const PackageImportEntry nullAsset[] = {{AssetId{}, 0, "", type}};
    const PackageImportEntry selfAsset[] = {{FixedId(600), 0, "", type}};
    const PackageImportEntry nullType[] = {{a, 0, "", TypeId{}}};
    const PackageImportEntry outerSelf[] = {{a, 1, "X", type}};
    const PackageImportEntry outerAhead[] = {{a, 2, "X", type}, {a, 0, "Y", type}};
    const PackageImportEntry outerOtherAsset[] = {{a, 0, "Gear", type}, {later, 1, "X", type}};
    const PackageImportEntry outerEmptyName[] = {{a, 0, "", type}, {a, 1, "X", type}};
    const PackageImportEntry innerEmptyName[] = {{a, 0, "Gear", type}, {a, 1, "", type}};
    const PackageImportEntry unorderedAssets[] = {{later, 0, "", type}, {a, 0, "", type}};
    const PackageImportEntry unorderedPaths[] = {{a, 0, "B", type}, {a, 0, "A", type}};
    const PackageImportEntry duplicate[] = {{a, 0, "A", type}, {a, 0, "A", type}};
    // 깊은 경로: 정렬은 객체 경로("Gear" < "Gear.Tooth" < "Head")로 한다.
    const PackageImportEntry nested[] = {
        {a, 0, "", type}, {a, 0, "Gear", type}, {a, 2, "Tooth", type}, {a, 0, "Head", type}};
    CHECK(corrupt(nullAsset));
    CHECK(corrupt(selfAsset));
    CHECK(corrupt(nullType));
    CHECK(corrupt(outerSelf));
    CHECK(corrupt(outerAhead));
    CHECK(corrupt(outerOtherAsset));
    CHECK(corrupt(outerEmptyName));
    CHECK(corrupt(innerEmptyName));
    CHECK(corrupt(unorderedAssets));
    CHECK(corrupt(unorderedPaths));
    CHECK(corrupt(duplicate));
    PackageReadOptions plain;
    CHECK(ReadPackage(allocator, with(nested, 4).View(), plain));
    {
        // 객체 경로 상한(4 KiB): 이름 둘의 합이 넘는다(각 이름은 문자열 상한 안).
        Array<char> name(allocator);
        REQUIRE(name.TryResize(3000));
        for (char& value : name) {
            value = 'q';
        }

        const StringView longName(name.Data(), name.Size());
        const PackageImportEntry tooLong[] = {{a, 0, longName, type}, {a, 1, longName, type}};
        CHECK(corrupt(tooLong));
    }
}

// Dependencies(AS 5.2절, 5.7절 5): count(0·크기 상한·실제와 다름), 잘림·남는 byte, kind 범위(0·6),
// flags 정의되지 않은 비트, 에셋 null·자기 에셋, 쿠킹의 EditorOnly·Build·ImportOwner, Hard가 아닌
// 종류의 expectedType, (assetId, kind) 정렬·중복.
TEST(AssetPackage_DependenciesFields) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Base base(env.allocator);
    REQUIRE(MakeBase(env, base));
    const Reader reader{env};
    Allocator& allocator = env.allocator;
    auto valid = ReadPackage(allocator, base.cooked.View(), PackageReadOptions{});
    REQUIRE(valid);
    REQUIRE(valid.Value().dependencies.Size() == 2);
    const PackageDependencyEntry hard = valid.Value().dependencies[0];
    const PackageDependencyEntry soft = valid.Value().dependencies[1];
    auto with = [&](const Array<uint8>& file, Span<const PackageDependencyEntry> entries,
                    uint32 count, uint32 flags = 0, uint32 kind = 0) noexcept {
        Array<uint8> bytes = DependenciesBytes(allocator, entries, count, flags, kind);
        return WithSection(allocator, file, PackageSectionKind::Dependencies, bytes.View());
    };
    auto corrupt = [&](Span<const PackageDependencyEntry> entries,
                       PackageKind kind = PackageKind::Cooked) noexcept {
        const Array<uint8>& file = kind == PackageKind::Cooked ? base.cooked : base.editor;
        return reader.Corrupt(with(file, entries, static_cast<uint32>(entries.Size())),
                              PackageSectionKind::Dependencies, kind);
    };

    const PackageDependencyEntry both[] = {hard, soft};
    CHECK(reader.Opens(with(base.cooked, both, 2), PackageKind::Cooked, false));
    CHECK(reader.Corrupt(with(base.cooked, Span<const PackageDependencyEntry>(), 0),
                         PackageSectionKind::Dependencies));
    CHECK(reader.Corrupt(with(base.cooked, both, 3), PackageSectionKind::Dependencies));
    CHECK(reader.Corrupt(with(base.cooked, both, 100), PackageSectionKind::Dependencies));
    {
        Array<uint8> bytes = DependenciesBytes(allocator, both, 2);
        REQUIRE(bytes.TryPushBack(0));
        CHECK(reader.Corrupt(
            WithSection(allocator, base.cooked, PackageSectionKind::Dependencies, bytes.View()),
            PackageSectionKind::Dependencies));
    }

    const PackageDependencyEntry single[] = {hard};
    CHECK(reader.Corrupt(with(base.cooked, single, 1, 0, 6), PackageSectionKind::Dependencies));
    CHECK(reader.Corrupt(with(base.cooked, single, 1, 0, 0xFFFFFFFFu),
                         PackageSectionKind::Dependencies));
    // kind 0은 DependenciesBytes의 "덮어쓰지 않음" 값이라 따로 만든다.
    {
        ArchiveWriter out(allocator, EditLimits());
        REQUIRE(out.WriteU32(1));
        REQUIRE(out.WriteGuid(hard.asset.value));
        REQUIRE(out.WriteU32(0));
        REQUIRE(out.WriteGuid(hard.expectedType));
        REQUIRE(out.WriteU32(0));
        CHECK(reader.Corrupt(
            WithSection(allocator, base.cooked, PackageSectionKind::Dependencies, out.View()),
            PackageSectionKind::Dependencies));
    }

    CHECK(reader.Corrupt(with(base.cooked, single, 1, 2), PackageSectionKind::Dependencies));
    const PackageDependencyEntry nullAsset[] = {
        {AssetId{}, PackageDependencyKind::Hard, hard.expectedType, false}};
    const PackageDependencyEntry selfAsset[] = {
        {FixedId(600), PackageDependencyKind::Hard, hard.expectedType, false}};
    const PackageDependencyEntry softType[] = {
        hard, {soft.asset, PackageDependencyKind::Soft, hard.expectedType, false}};
    const PackageDependencyEntry unordered[] = {soft, hard};
    const PackageDependencyEntry duplicate[] = {hard, hard};
    const PackageDependencyEntry sameAssetOrder[] = {
        {hard.asset, PackageDependencyKind::Soft, TypeId{}, false}, hard};
    CHECK(corrupt(nullAsset));
    CHECK(corrupt(selfAsset));
    CHECK(corrupt(softType));
    CHECK(corrupt(unordered));
    CHECK(corrupt(duplicate));
    CHECK(corrupt(sameAssetOrder));
    const PackageDependencyKind editorKinds[] = {PackageDependencyKind::EditorOnly,
                                                 PackageDependencyKind::Build,
                                                 PackageDependencyKind::ImportOwner};
    for (PackageDependencyKind kind : editorKinds) {
        const PackageDependencyEntry extra[] = {hard, soft, {FixedId(650), kind, TypeId{}, false}};
        CHECK(corrupt(extra));
        // 에디터 패키지 표 규칙은 받는다(로드 대조는 PackageLoad 시험).
        PackageReadOptions editor;
        editor.expectedKind = PackageKind::Editor;
        CHECK(ReadPackage(allocator, with(base.editor, extra, 3).View(), editor));
    }
}

// BulkIndex·Bulk(AS 5.2절 BulkIndex 검증): count(0·크기 상한·실제와 다름), 잘림·남는 byte, codec
// 범위, bulkId 연속, offset = 앞 끝 정렬 값, flags(정의되지 않은 비트·쿠킹 EditorOnly), None의
// stored = raw, 섹션 범위, entry 사이 패딩 0, entry CRC, 마지막 끝 = 섹션 크기.
TEST(AssetPackage_BulkIndexFields) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Base base(env.allocator);
    REQUIRE(MakeBase(env, base));
    const Reader reader{env};
    Allocator& allocator = env.allocator;
    auto valid = ReadPackage(allocator, base.cooked.View(), PackageReadOptions{});
    REQUIRE(valid);
    REQUIRE(valid.Value().bulkEntries.Size() == 2);
    const Array<PackageBulkEntry> entries = CopyEntries(allocator, valid.Value().bulkEntries);
    auto with = [&](Span<const PackageBulkEntry> rows, uint32 count, uint32 codec = 0) noexcept {
        Array<uint8> bytes = BulkIndexBytes(allocator, rows, count, codec);
        return WithSection(allocator, base.cooked, PackageSectionKind::BulkIndex, bytes.View());
    };
    auto corrupt = [&](Span<const PackageBulkEntry> rows) noexcept {
        return reader.Corrupt(with(rows, static_cast<uint32>(rows.Size())),
                              PackageSectionKind::BulkIndex);
    };

    CHECK(reader.Opens(with(entries.View(), 2), PackageKind::Cooked, false));
    CHECK(reader.Corrupt(with(Span<const PackageBulkEntry>(), 0), PackageSectionKind::BulkIndex));
    CHECK(reader.Corrupt(with(entries.View(), 3), PackageSectionKind::BulkIndex));
    CHECK(reader.Corrupt(with(entries.View(), 1000), PackageSectionKind::BulkIndex));
    CHECK(reader.Corrupt(with(entries.View(), 2, 2), PackageSectionKind::BulkIndex));
    {
        Array<uint8> bytes = BulkIndexBytes(allocator, entries.View(), 2);
        REQUIRE(bytes.TryPushBack(0));
        CHECK(reader.Corrupt(
            WithSection(allocator, base.cooked, PackageSectionKind::BulkIndex, bytes.View()),
            PackageSectionKind::BulkIndex));
    }

    auto mutate = [&](usize index, auto&& change) noexcept {
        Array<PackageBulkEntry> rows = CopyEntries(allocator, entries);
        change(rows[index]);
        return corrupt(rows.View());
    };

    CHECK(mutate(0, [](PackageBulkEntry& row) noexcept { row.bulkId = 2; }));
    CHECK(mutate(1, [](PackageBulkEntry& row) noexcept { row.bulkId = 3; }));
    CHECK(mutate(0, [](PackageBulkEntry& row) noexcept { row.offset = 8; }));
    CHECK(mutate(1, [](PackageBulkEntry& row) noexcept { row.offset += 8; }));
    CHECK(mutate(1, [](PackageBulkEntry& row) noexcept { row.offset -= 8; }));
    CHECK(mutate(0, [](PackageBulkEntry& row) noexcept { row.flags = 2; }));
    CHECK(mutate(0, [](PackageBulkEntry& row) noexcept { row.flags = 1; }));
    CHECK(mutate(0, [](PackageBulkEntry& row) noexcept { row.codec = BulkCodec::None; }));
    CHECK(mutate(
        1, [](PackageBulkEntry& row) noexcept { row.storedBytes = NumericLimits<uint64>::Max(); }));
    CHECK(mutate(
        1, [](PackageBulkEntry& row) noexcept { row.offset = NumericLimits<uint64>::Max() - 7; }));
    CHECK(mutate(0, [](PackageBulkEntry& row) noexcept { row.crc32 ^= 1; }));
    CHECK(mutate(1, [](PackageBulkEntry& row) noexcept { row.storedBytes -= 1; }));

    // entry 사이 패딩의 0이 아닌 byte(섹션 CRC 칸은 0 — entry CRC 밖이므로 패딩 검사만이 잡는다),
    // Bulk 섹션 끝에 남는 byte.
    {
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(base.cooked.View()));
        EditableSection* bulk = edit.Find(PackageSectionKind::Bulk);
        REQUIRE(bulk != nullptr);
        const usize gap = static_cast<usize>(entries[0].offset + entries[0].storedBytes);
        REQUIRE(gap < entries[1].offset);
        bulk->bytes[gap] = 0x5A;
        CHECK(reader.Corrupt(edit.Build(), PackageSectionKind::BulkIndex));
        bulk->bytes[gap] = 0;
        REQUIRE(bulk->bytes.TryPushBack(0));
        CHECK(reader.Corrupt(edit.Build(), PackageSectionKind::BulkIndex));
    }

    // 에디터 패키지는 EditorOnly 표시를 받는다(쿠킹은 0이어야 한다 — 위).
    {
        auto editor = ReadPackage(allocator, base.editor.View(), [] {
            PackageReadOptions options;
            options.expectedKind = PackageKind::Editor;
            return options;
        }());
        REQUIRE(editor);
    }
}

// bulk 해제(DecodePackageBulk — AS 5.2절, 5.7절 6): raw 합 상한(CapacityExceeded, 진단 None), Zstd
// 해제 실패·크기 불일치·SHA-256 불일치(CorruptPackage, out·slots는 호출 전 길이로), 앞에 내용이
// 있는 out에 이어 붙이기(slot offset은 이번 호출 기준).
TEST(AssetPackage_DecodeBulk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Base base(env.allocator);
    REQUIRE(MakeBase(env, base));
    Allocator& allocator = env.allocator;
    Array<uint8> file = Copy(allocator, base.cooked.View());
    auto read = ReadPackage(allocator, file.View(), PackageReadOptions{});
    REQUIRE(read);
    PackageContents& contents = read.Value();
    REQUIRE(contents.bulkEntries[0].codec == BulkCodec::Zstd);

    Array<uint8> out(allocator);
    Array<PackageBulkSlot> slots(allocator);
    REQUIRE(out.TryPushBack(0xEE));
    REQUIRE(DecodePackageBulk(contents, out, slots, NumericLimits<uint64>::Max()));
    REQUIRE(slots.Size() == 2);
    CHECK(slots[0].offset == 0);
    CHECK(SameBytes(out.View().Slice(1, base.pixels.Size()), base.pixels.View()));
    CHECK(slots[1].offset == base.pixels.Size());

    AssetDiagnostic failure;
    const uint64 total = base.pixels.Size() + base.other.Size();
    CHECK(DecodePackageBulk(contents, out, slots, total - 1, &failure).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(failure.code == AssetDiagnosticCode::None);
    CHECK(DecodePackageBulk(contents, out, slots, total));

    const usize before = out.Size();
    const usize slotsBefore = slots.Size();
    auto expectCorrupt = [&](PackageContents& changed) noexcept {
        AssetDiagnostic diagnostic;
        auto decoded =
            DecodePackageBulk(changed, out, slots, NumericLimits<uint64>::Max(), &diagnostic);
        return !decoded && decoded.Error() == ErrorCode::InvalidArgument &&
               diagnostic.code == AssetDiagnosticCode::CorruptPackage && out.Size() == before &&
               slots.Size() == slotsBefore;
    };

    // raw 크기 불일치(Zstd가 다른 크기로 풀린다), SHA-256 불일치(None entry).
    contents.bulkEntries[0].rawBytes += 1;
    CHECK(expectCorrupt(contents));
    contents.bulkEntries[0].rawBytes -= 2;
    CHECK(expectCorrupt(contents));
    contents.bulkEntries[0].rawBytes += 1;
    contents.bulkEntries[1].sha256.bytes[0] ^= 1;
    CHECK(expectCorrupt(contents));
    contents.bulkEntries[1].sha256.bytes[0] ^= 1;
    // 저장 byte 손상(읽은 뒤 바꿈 — CRC는 이미 지났다): Zstd 해제 실패.
    usize row = 0;
    uint64 offset = 0;
    uint64 size = 0;
    REQUIRE(FindSection(file.View(), PackageSectionKind::Bulk, row, offset, size));
    for (usize index = 0; index < 8; ++index) {
        file[static_cast<usize>(offset) + index] ^= 0xFF;
    }

    CHECK(expectCorrupt(contents));
}

// 잘림(AS 5.1절 섹션별 내용 검증): Summary·Exports·ObjectImports·Dependencies·BulkIndex 섹션을
// 0부터 원래 길이 − 1까지 모든 길이로 잘라 다시 배치하면 언제나 그 섹션의 CorruptPackage다(고정
// 배치의 칸마다 읽기 실패 지점, Record 잘림).
TEST(AssetPackage_TruncatedSections) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Base base(env.allocator);
    REQUIRE(MakeBase(env, base));
    const Reader reader{env};
    Allocator& allocator = env.allocator;
    const PackageSectionKind kinds[] = {
        PackageSectionKind::Summary, PackageSectionKind::Exports, PackageSectionKind::ObjectImports,
        PackageSectionKind::Dependencies, PackageSectionKind::BulkIndex};
    for (PackageSectionKind kind : kinds) {
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(base.cooked.View()));
        EditableSection* section = edit.Find(kind);
        REQUIRE(section != nullptr);
        const Array<uint8> original = Copy(allocator, section->bytes.View());
        uint32 failures = 0;
        for (usize length = 1; length < original.Size(); ++length) {
            section->bytes.Clear();
            REQUIRE(section->bytes.TryAppend(original.View().Slice(0, length)));
            if (reader.Corrupt(edit.Build(), kind)) {
                ++failures;
            }
        }

        CHECK(failures == original.Size() - 1);
    }
}
