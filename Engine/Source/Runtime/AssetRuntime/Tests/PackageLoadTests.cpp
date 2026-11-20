#include "PackageEditing.h"

// 로드 단계 시험이다(Docs/Design/AssetSystem.ko.md 5.7절 8–10, 11.2절): Prepare의 대조(대표 에셋
// 타입· 저장 버전·BulkDataRef ↔ BulkIndex·Dependencies ↔ 디코딩 결과·읽기 전용 부분집합·import
// 의존), InstantiatePackages의 인자·resolver 오류, 로드 hook의 진단 분류.
using namespace rocks;
namespace package_load_tests_file {

using namespace rocks::asset_test;

namespace {

struct Loaded {
    explicit Loaded(Allocator& allocator) noexcept
        : cooked(allocator), editor(allocator), pixels(allocator), other(allocator) {
    }

    Array<uint8> cooked;
    Array<uint8> editor;
    Array<uint8> pixels;
    Array<uint8> other;
    TestBulkSource bulk;
};

// 대표 에셋 + Part, 다른 패키지(701) Hard, Soft(702), bulk 둘.
bool MakeLoaded(AssetEnv& env, Loaded& out) noexcept {
    ObjectRegistry& registry = *env.source;
    const ObjectAccessContext access = registry.OwnerAccess();
    auto dependency = NewPackage(registry, access, FixedId(701), PackageKind::Editor);
    auto package = NewPackage(registry, access, FixedId(700), PackageKind::Editor);
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

    FillCompressible(out.pixels, 4000, 6);
    FillIncompressible(out.other, 200, 6);
    pointer->pixels = out.bulk.Add(1, out.pixels.View());
    partPointer->data = out.bulk.Add(2, out.other.View());
    if (!pointer->soft.TryAssign(FixedId(702), StringView()) ||
        !registry.SetReference(access, asset, pointer->hard, target)) {
        return false;
    }

    PackageWriteOptions cooked = CookedOptions();
    cooked.bulkSource = &out.bulk;
    PackageWriteOptions editor = EditorOptions();
    editor.bulkSource = &out.bulk;
    out.cooked = Write(env.allocator, registry, package.Value().Handle(), cooked);
    out.editor = Write(env.allocator, registry, package.Value().Handle(), editor);
    return !out.cooked.IsEmpty() && !out.editor.IsEmpty();
}

// Prepare 결과 진단이다(성공이면 code None).
AssetDiagnostic PrepareOf(AssetEnv& env, Array<uint8>&& bytes, PackageKind kind, bool& prepared,
                          Span<const PackageCustomVersion> customs = {}) noexcept {
    PackageLoadOptions options = LoadOptions(kind);
    options.read.customVersions = customs;
    PackageLoadState state(env.allocator, *env.target, env.types, env.target->OwnerAccess(),
                           options);
    AssetDiagnostic failure;
    prepared = state.Prepare(Move(bytes), &failure).IsOk();
    return failure;
}

bool CorruptAt(AssetEnv& env, Array<uint8>&& bytes, PackageKind kind, PackageSectionKind section,
               Span<const PackageCustomVersion> customs = {}) noexcept {
    bool prepared = false;
    const AssetDiagnostic failure = PrepareOf(env, Move(bytes), kind, prepared, customs);
    const bool ok = !prepared && failure.code == AssetDiagnosticCode::CorruptPackage &&
                    failure.error == ErrorCode::InvalidArgument &&
                    failure.section == static_cast<uint32>(section);
    if (!ok) {
        // 실제로 받은 진단 코드를 실패 문구로 남긴다.
        test::RecordFailure(AssetDiagnosticName(failure.code), __FILE__, __LINE__);
    }

    return ok;
}

} // namespace

// Prepare 대조(AS 5.7절 5·6·10): 대표 에셋이 Asset 파생이 아님, 노드 저장 버전 ≠ Summary 버전,
// BulkDataRef의 bulkId가 BulkIndex 밖·rawBytes·SHA-256 불일치, Dependencies 누락·남음·종류·required
// 불일치, 읽기 전용 열기의 부분집합 규칙, 쓰이지 않은 import의 에셋이 Hard·EditorOnly 의존이 아님.
TEST(AssetPackage_PrepareCrossChecks) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Loaded loaded(env.allocator);
    REQUIRE(MakeLoaded(env, loaded));
    Allocator& allocator = env.allocator;
    bool prepared = false;
    (void)PrepareOf(env, Copy(allocator, loaded.cooked.View()), PackageKind::Cooked, prepared);
    REQUIRE(prepared);
    (void)PrepareOf(env, Copy(allocator, loaded.editor.View()), PackageKind::Editor, prepared);
    REQUIRE(prepared);
    auto contents = ReadPackage(allocator, loaded.editor.View(), [] {
        PackageReadOptions options;
        options.expectedKind = PackageKind::Editor;
        return options;
    }());
    REQUIRE(contents);
    const PackageContents& base = contents.Value();

    // 대표 에셋 타입이 Asset 파생이 아니다: 대표 에셋 하나짜리 패키지의 export 1 타입을 TestPart로
    // 바꾸면(Summary 타입 표도 맞춤) 그래프 1단계가 노드 Record와 타입이 다르다고 먼저 거부한다
    // (Objects). Prepare의 대표 타입 대조는 그래프가 이것을 받아 줄 때의 방어선이다(14.1.2절).
    {
        ObjectRegistry& source = *env.source;
        auto plain = NewPackage(source, source.OwnerAccess(), FixedId(705), PackageKind::Editor);
        REQUIRE(plain);
        REQUIRE(!Make<TestAsset>(source, TestAssetType(), plain.Value().Handle(), "").IsNull());
        const Array<uint8> file = Write(allocator, source, plain.Value().Handle(), EditorOptions());
        REQUIRE(!file.IsEmpty());
        auto parsed = ReadPackage(allocator, file.View(), [] {
            PackageReadOptions options;
            options.expectedKind = PackageKind::Editor;
            return options;
        }());
        REQUIRE(parsed);
        Array<PackageExportEntry> exports = CopyEntries(allocator, parsed.Value().exports);
        exports[0].type = TestPartType();
        const PackageTypeVersion types[] = {
            {TestPartType(), parsed.Value().summary.types[0].version}};
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(file.View()));
        Array<uint8> exportBytes =
            ExportsBytes(allocator, exports.View(), static_cast<uint32>(exports.Size()));
        REQUIRE(edit.Replace(PackageSectionKind::Exports, exportBytes.View()));
        Array<uint8> summary =
            SummaryBytes(allocator, SummaryInput{types, {}, true}, SummaryFault::None);
        REQUIRE(edit.Replace(PackageSectionKind::Summary, summary.View()));
        CHECK(CorruptAt(env, edit.Build(), PackageKind::Editor, PackageSectionKind::Objects));
    }

    // 노드 저장 버전 ≠ Summary 버전(에디터 — 더 낮은 Summary 버전은 reader가 받아 그래프 대조가
    // 잡는다).
    {
        Array<PackageTypeVersion> types = CopyEntries(allocator, base.summary.types);
        REQUIRE(types[0].version > 0);
        types[0].version -= 1;
        Array<uint8> summary =
            SummaryBytes(allocator, SummaryInput{types.View(), {}, true}, SummaryFault::None);
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(loaded.editor.View()));
        REQUIRE(edit.Replace(PackageSectionKind::Summary, summary.View()));
        CHECK(CorruptAt(env, edit.Build(), PackageKind::Editor, PackageSectionKind::Summary));
    }

    // BulkDataRef의 bulkId가 BulkIndex 밖: entry 2를 빼고 Bulk를 entry 1 끝까지 자른다.
    {
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(loaded.cooked.View()));
        auto cooked = ReadPackage(allocator, loaded.cooked.View(), PackageReadOptions{});
        REQUIRE(cooked);
        const PackageBulkEntry first = cooked.Value().bulkEntries[0];
        Array<uint8> index = BulkIndexBytes(allocator, Span<const PackageBulkEntry>(&first, 1), 1);
        REQUIRE(edit.Replace(PackageSectionKind::BulkIndex, index.View()));
        EditableSection* bulk = edit.Find(PackageSectionKind::Bulk);
        REQUIRE(bulk != nullptr);
        REQUIRE(bulk->bytes.TryResize(static_cast<usize>(first.storedBytes)));
        CHECK(CorruptAt(env, edit.Build(), PackageKind::Cooked, PackageSectionKind::BulkIndex));
    }

    // BulkIndex entry 2가 다른(그 자체로는 올바른) byte를 가리킨다: ref의 rawBytes·SHA-256과
    // 다르다.
    {
        auto cooked = ReadPackage(allocator, loaded.cooked.View(), PackageReadOptions{});
        REQUIRE(cooked);
        Array<PackageBulkEntry> entries = CopyEntries(allocator, cooked.Value().bulkEntries);
        Array<uint8> replacement(allocator);
        FillIncompressible(replacement, entries[1].rawBytes + 16, 99);
        entries[1].rawBytes = replacement.Size();
        entries[1].storedBytes = replacement.Size();
        entries[1].codec = BulkCodec::None;
        entries[1].crc32 = Crc32(replacement.View());
        entries[1].sha256 = Sha256::Hash(replacement.View());
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(loaded.cooked.View()));
        EditableSection* bulk = edit.Find(PackageSectionKind::Bulk);
        REQUIRE(bulk != nullptr);
        REQUIRE(bulk->bytes.TryResize(static_cast<usize>(entries[1].offset)));
        REQUIRE(bulk->bytes.TryAppend(replacement.View()));
        Array<uint8> index = BulkIndexBytes(allocator, entries.View(), 2);
        REQUIRE(edit.Replace(PackageSectionKind::BulkIndex, index.View()));
        CHECK(CorruptAt(env, edit.Build(), PackageKind::Cooked, PackageSectionKind::BulkIndex));
        // 크기는 같고 SHA-256만 다른 경우.
        entries[1].rawBytes = cooked.Value().bulkEntries[1].rawBytes;
        entries[1].storedBytes = entries[1].rawBytes;
        REQUIRE(bulk->bytes.TryResize(static_cast<usize>(entries[1].offset + entries[1].rawBytes)));
        entries[1].crc32 = Crc32(bulk->bytes.View().Slice(static_cast<usize>(entries[1].offset),
                                                          static_cast<usize>(entries[1].rawBytes)));
        entries[1].sha256 = Sha256::Hash(bulk->bytes.View().Slice(
            static_cast<usize>(entries[1].offset), static_cast<usize>(entries[1].rawBytes)));
        Array<uint8> sameSize = BulkIndexBytes(allocator, entries.View(), 2);
        REQUIRE(edit.Replace(PackageSectionKind::BulkIndex, sameSize.View()));
        CHECK(CorruptAt(env, edit.Build(), PackageKind::Cooked, PackageSectionKind::BulkIndex));
    }

    // Dependencies 누락·남음·종류 다름(쿠킹).
    {
        auto cooked = ReadPackage(allocator, loaded.cooked.View(), PackageReadOptions{});
        REQUIRE(cooked);
        Array<PackageDependencyEntry> deps = CopyEntries(allocator, cooked.Value().dependencies);
        REQUIRE(deps.Size() == 2);
        auto withDeps = [&](const Array<uint8>& file, Span<const PackageDependencyEntry> rows) {
            Array<uint8> bytes =
                DependenciesBytes(allocator, rows, static_cast<uint32>(rows.Size()));
            EditablePackage edit(allocator);
            (void)edit.Parse(file.View());
            (void)edit.Replace(PackageSectionKind::Dependencies, bytes.View());
            return edit.Build();
        };

        CHECK(CorruptAt(env, withDeps(loaded.cooked, deps.View().Slice(0, 1)), PackageKind::Cooked,
                        PackageSectionKind::Dependencies));
        PackageDependencyEntry extra[3] = {
            deps[0], deps[1], {FixedId(799), PackageDependencyKind::Soft, TypeId{}, false}};
        CHECK(CorruptAt(env, withDeps(loaded.cooked, extra), PackageKind::Cooked,
                        PackageSectionKind::Dependencies));
        // Soft 쓰임을 Hard로 적었다(같은 에셋, 다른 종류 — 정렬 유지).
        PackageDependencyEntry kind[2] = {
            deps[0], {deps[1].asset, PackageDependencyKind::Hard, deps[0].expectedType, false}};
        CHECK(CorruptAt(env, withDeps(loaded.cooked, kind), PackageKind::Cooked,
                        PackageSectionKind::Dependencies));

        // 에디터 읽기 전용(더 새 커스텀 버전)에서는 디코딩 결과 ⊆ Dependencies만 본다: 남는 항목은
        // 받고, 빠진 항목은 손상이다.
        auto editor = ReadPackage(allocator, loaded.editor.View(), [] {
            PackageReadOptions options;
            options.expectedKind = PackageKind::Editor;
            return options;
        }());
        REQUIRE(editor);
        Array<PackageDependencyEntry> editorDeps =
            CopyEntries(allocator, editor.Value().dependencies);
        const PackageCustomVersion newer[] = {{Guid{1, 1}, 0}};
        // 저장된 커스텀 버전이 없으므로 "모르는 id" 축이 아니라 더 새 타입 축으로 읽기 전용을
        // 만든다: Summary를 고치는 대신 Tags에 schemaVersion 2를 넣는다(Required가 아닌 더 새 섹션
        // = 읽기 전용).
        (void)newer;
        auto readOnly = [&](Span<const PackageDependencyEntry> rows) {
            Array<uint8> file = withDeps(loaded.editor, rows);
            EditablePackage edit(allocator);
            (void)edit.Parse(file.View());
            RecordBuilder record(allocator, Guid{1, 1}, 1);
            auto built = record.Build();
            EditableSection* tags = edit.Insert(static_cast<uint32>(PackageSectionKind::Tags), 0,
                                                built ? built.Value().View() : Span<const uint8>());
            if (tags != nullptr) {
                tags->schemaVersion = 2;
            }

            return edit.Build();
        };

        PackageDependencyEntry superset[3] = {
            editorDeps[0],
            editorDeps[1],
            {FixedId(799), PackageDependencyKind::Soft, TypeId{}, false}};
        bool ok = false;
        (void)PrepareOf(env, readOnly(superset), PackageKind::Editor, ok);
        CHECK(ok);
        CHECK(CorruptAt(env, readOnly(editorDeps.View().Slice(0, 1)), PackageKind::Editor,
                        PackageSectionKind::Dependencies));
    }

    // import 하나를 더하고 Objects header의 외부 번호 수(externalCount)는 그대로 두면 그래프
    // 1단계가 import 수 불일치로 거부한다(Objects 손상). 외부 번호 수를 맞춘 경우는
    // AssetPackage_OuterOnlyImportCrossChecks·AssetPackage_UnusedImportIsCorrupt가 본다.
    {
        auto cooked = ReadPackage(allocator, loaded.cooked.View(), PackageReadOptions{});
        REQUIRE(cooked);
        Array<PackageImportEntry> imports = CopyEntries(allocator, cooked.Value().imports);
        REQUIRE(imports.TryPushBack(
            PackageImportEntry{FixedId(798), 0, StringView(), imports[0].type}));
        Array<uint8> importBytes =
            ImportsBytes(allocator, imports.View(), static_cast<uint32>(imports.Size()));
        EditablePackage edit(allocator);
        REQUIRE(edit.Parse(loaded.cooked.View()));
        REQUIRE(edit.Replace(PackageSectionKind::ObjectImports, importBytes.View()));
        CHECK(CorruptAt(env, edit.Build(), PackageKind::Cooked, PackageSectionKind::Objects));
    }
}

// InstantiatePackages 인자(AS 5.7절 10): 빈 묶음, 출력 크기 불일치, null·Prepare 안 한 상태, 다른
// Registry의 상태, 같은 AssetId 둘은 InvalidArgument이고 아무것도 만들지 않는다. resolver의
// NotFound는 MissingDependency, 그 밖 오류는 진단 None + 원래 오류, 찾은 패키지에 import 경로가
// 없으면 MissingDependency다.
TEST(AssetPackage_InstantiateArgumentsAndResolver) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Loaded loaded(env.allocator);
    REQUIRE(MakeLoaded(env, loaded));
    Allocator& allocator = env.allocator;
    ObjectRegistry& target = *env.target;
    const PackageLoadOptions options = LoadOptions(PackageKind::Cooked);
    PackageLoadState ready(allocator, target, env.types, target.OwnerAccess(), options);
    REQUIRE(ready.Prepare(Copy(allocator, loaded.cooked.View())));
    PackageLoadState twin(allocator, target, env.types, target.OwnerAccess(), options);
    REQUIRE(twin.Prepare(Copy(allocator, loaded.cooked.View())));
    PackageLoadState fresh(allocator, target, env.types, target.OwnerAccess(), options);
    PackageLoadState foreign(allocator, *env.source, env.types, env.source->OwnerAccess(), options);
    REQUIRE(foreign.Prepare(Copy(allocator, loaded.cooked.View())));
    ObjectHandle one[1];
    ObjectHandle two[2];
    {
        ConstructionContext transaction(target, target.OwnerAccess());
        PackageLoadState* none[1] = {nullptr};
        PackageLoadState* readyOnly[1] = {&ready};
        PackageLoadState* unprepared[1] = {&fresh};
        PackageLoadState* other[1] = {&foreign};
        PackageLoadState* duplicate[2] = {&ready, &twin};
        CHECK(InstantiatePackages(transaction, Span<PackageLoadState* const>(), nullptr,
                                  Span<ObjectHandle>())
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(InstantiatePackages(transaction, readyOnly, nullptr, two).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(InstantiatePackages(transaction, none, nullptr, one).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(InstantiatePackages(transaction, unprepared, nullptr, one).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(InstantiatePackages(transaction, other, nullptr, one).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(InstantiatePackages(transaction, duplicate, nullptr, two).Error() ==
              ErrorCode::InvalidArgument);
        transaction.Rollback();
    }

    CHECK(target.Stats().liveObjects == 0);

    struct FailingResolver final : PackageImportResolver {
        ErrorCode error = ErrorCode::Busy;
        ObjectHandle handle{};

        Result<ObjectHandle> FindPackage(AssetId) noexcept override {
            return error == ErrorCode::None ? Result<ObjectHandle>::Success(handle)
                                            : Result<ObjectHandle>::Failure(error);
        }
    };

    FailingResolver resolver;
    {
        ConstructionContext transaction(target, target.OwnerAccess());
        PackageLoadState* states[1] = {&ready};
        AssetDiagnostic failure;
        CHECK(InstantiatePackages(transaction, states, &resolver, one, &failure).Error() ==
              ErrorCode::Busy);
        CHECK(failure.code == AssetDiagnosticCode::None);
        CHECK(failure.related == FixedId(701));
        transaction.Rollback();
    }

    // resolver가 준 패키지에 대표 에셋이 없다(빈 패키지): import 경로 NotFound → MissingDependency.
    auto empty = NewPackage(target, target.OwnerAccess(), FixedId(701), PackageKind::Cooked);
    REQUIRE(empty);
    resolver.error = ErrorCode::None;
    resolver.handle = empty.Value().Handle();
    {
        PackageLoadState state(allocator, target, env.types, target.OwnerAccess(), options);
        REQUIRE(state.Prepare(Copy(allocator, loaded.cooked.View())));
        ConstructionContext transaction(target, target.OwnerAccess());
        PackageLoadState* states[1] = {&state};
        AssetDiagnostic failure;
        CHECK(!InstantiatePackages(transaction, states, &resolver, one, &failure));
        CHECK(failure.code == AssetDiagnosticCode::MissingDependency);
        CHECK(failure.error == ErrorCode::NotFound);
        CHECK(failure.section == static_cast<uint32>(PackageSectionKind::ObjectImports));
        transaction.Rollback();
    }
}

// 로드 hook의 진단 분류(AS 5.7절 8·9): 쿠킹의 Required 실효 누락은 실패(손상 — writer가
// 거부하므로), 에디터는 세고 계속한다. 모르는 필드·더 새 버전은 읽기 전용 표시, 그 밖 진단은
// 무시한다. Soft 수집·BulkDataRef 관찰은 빈 ref를 건너뛴다.
TEST(AssetPackage_LoadHooksClassifyDiagnostics) {
    SystemAllocator allocator;
    const bool cookedValues[] = {true, false};
    for (bool cooked : cookedValues) {
        detail::PackageLoadHooks hooks(allocator, cooked);
        FieldDiagnostic diagnostic{};
        diagnostic.kind = FieldDiagnosticKind::RequiredMissing;
        CHECK(hooks.Report(diagnostic).IsOk() == !cooked);
        CHECK(hooks.missingRequired == 1);
        diagnostic.kind = FieldDiagnosticKind::UnknownField;
        CHECK(hooks.Report(diagnostic));
        CHECK(hooks.sawNewer);
        hooks.sawNewer = false;
        diagnostic.kind = FieldDiagnosticKind::NewerVersion;
        CHECK(hooks.Report(diagnostic));
        CHECK(hooks.sawNewer);
        // 그 밖의 진단(선언되지 않은 enum 값 등)은 무시한다(그래프가 정책대로 처리한다).
        hooks.sawNewer = false;
        diagnostic.kind = FieldDiagnosticKind::UndeclaredEnumValue;
        CHECK(hooks.Report(diagnostic));
        CHECK(!hooks.sawNewer);
        REQUIRE(hooks.ObservedStructs().Size() == 1);
        CHECK(hooks.ObservedStructs()[0] == GeneratedType<BulkDataRef>::Id());
        BulkDataRef empty{};
        FieldCodecFrame frame{};
        CHECK(hooks.ObserveStruct(frame, GeneratedType<BulkDataRef>::Id(), nullptr, &empty));
        CHECK(hooks.bulkUses.IsEmpty());
        BulkDataRef full{3, 10, {}};
        CHECK(hooks.ObserveStruct(frame, GeneratedType<BulkDataRef>::Id(), nullptr, &full));
        CHECK(hooks.ObserveStruct(frame, TypeId{1, 2}, nullptr, &full));
        REQUIRE(hooks.bulkUses.Size() == 1);
        CHECK(hooks.bulkUses[0].bulkId == 3);
    }
}

// 진단 이름표(AS 4.6절): 모든 코드가 고유한 이름을 갖고 열거 밖 값은 "Unknown"이다.
TEST(AssetPackage_DiagnosticNames) {
    constexpr uint32 last = static_cast<uint32>(AssetDiagnosticCode::EventPropertiesDropped);
    for (uint32 index = 0; index <= last; ++index) {
        const char* name = AssetDiagnosticName(static_cast<AssetDiagnosticCode>(index));
        CHECK(StringView(name) != StringView("Unknown"));
        for (uint32 other = 0; other < index; ++other) {
            CHECK(StringView(name) !=
                  StringView(AssetDiagnosticName(static_cast<AssetDiagnosticCode>(other))));
        }
    }

    CHECK(StringView(AssetDiagnosticName(static_cast<AssetDiagnosticCode>(last + 1))) ==
          StringView("Unknown"));
    CHECK(StringView(AssetDiagnosticName(AssetDiagnosticCode::CorruptPackage)) ==
          StringView("CorruptPackage"));
}
} // namespace package_load_tests_file
