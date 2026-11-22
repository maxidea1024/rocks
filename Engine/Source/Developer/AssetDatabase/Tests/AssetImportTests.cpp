#include "AssetDatabaseTestSupport.h"
#include "AssetImportTestSupport.h"
#include "AssetIndexCompare.h"

#include <AssetDatabase/AssetAliasTable.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/AssetSourceRoots.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/AssetTypeHooks.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Time/DateTime.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>

// 새 임포트 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 18 A2f1b): 원본 하나·여럿(역할)·추가
// 출력의 저장과 ImportRecord(소유·참조 기록, 원본 위치·SHA-256·옵션 Record·출력 목록), 옵션과 설정
// 나누기, 임포터 고르기(이름·확장자), 모든 거부, 임포터 실패·출력 규칙·경고, 저장 실패의
// 되돌리기(출력 파일 지우기·못 지우면 경고), 할당 실패, 실제 디스크.
using namespace rocks;
namespace asset_import_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;
using namespace rocks::import_test;
using reflection_text::TextAssignment;
using reflection_text::TextDiagnostics;

namespace {

constexpr StringView RockSource = "SourceArt/Rock.trock";

PackageReadOptions ReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.engineVersion = TestEngineVersion;
    return options;
}

int64 StartTicks() noexcept {
    auto parsed = TryParseIso8601("2026-11-22T00:00:00Z");
    CHECK(parsed);
    return parsed ? parsed.Value().Ticks() : 0;
}

Span<const uint8> Bytes(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

// 임포트 장면이다: 디스크 임시 루트(잠금)에 놓인 메모리 파일 시스템(Content·Config·SourceArt),
// Leaf 패키지, 임포터 표(TestImporter·CubeImporter).
struct ImportScene {
    ImportScene(AssetEnv& env, Sources& sources, const char* tag) noexcept
        : root(tag), layout(env.allocator), files(env.allocator, Initialized(layout, root)),
          hooks(env.allocator, env.types), clock(StartTicks()), importers(env.allocator, env.types),
          test(TestImporterInfo()), cube(CubeImporterInfo()) {
        ready = root.made && !layout.Root().IsEmpty() && files.Ready() &&
                files.AddDirectory("Content") && files.AddDirectory("Config") &&
                files.AddDirectory("SourceArt") &&
                files.AddFile("Content/Leaf.rasset", sources.leaf.View()) &&
                static_cast<bool>(lock.Acquire(layout, 0));
        auto first = importers.Register(test);
        auto second = importers.Register(cube);
        ready = ready && first && second;
        if (first && second) {
            testToken = rocks::Move(first.Value());
            cubeToken = rocks::Move(second.Value());
        }
    }

    static StringView Initialized(ProjectLayout& layout, const TempProject& project) noexcept {
        return layout.Initialize(project.Root()) ? layout.Root() : project.Root();
    }

    AssetServiceConfig Config(AssetEnv& env) noexcept {
        AssetServiceConfig config;
        config.packages.types = &env.types;
        config.packages.files = &files;
        config.packages.hooks = &hooks;
        config.packages.read.engineVersion = TestEngineVersion;
        config.packages.registryContext = ContextId{0x494d5054ull, 1};
        config.packages.registryOptions.names = &env.names;
        config.clock = &clock;
        config.importers = &importers;
        return config;
    }

    // 원본 파일을 둔다(SourceArt 아래).
    bool PutSource(StringView relative, StringView text) noexcept {
        files.Remove(relative);
        return files.AddFile(relative, Bytes(text));
    }

    // relative(루트 아래)의 절대 경로다.
    String Absolute(Allocator& allocator, StringView relative) noexcept {
        String out(allocator);
        CHECK(out.TryAppend(layout.Root()) && out.TryAppend("/") && out.TryAppend(relative));
        return out;
    }

    TempProject root;
    ProjectLayout layout;
    MemoryFileSystem files;
    AssetTypeHookTable hooks;
    ManualAssetClock clock;
    AssetDatabaseLock lock;
    AssetImporterTable importers;
    TestImporter test;
    TestImporter cube;
    AssetImporterRegistration testToken;
    AssetImporterRegistration cubeToken;
    bool ready = false;
};

bool Matches(AssetEnv& env, ImportScene& scene, const AssetIndex& index) noexcept {
    AssetIndex fresh(env.allocator, scene.layout);
    return fresh.Rebuild(scene.files, ReadOptions()) && SameIndex(fresh, index);
}

// "<경로>=<값>" 대입 목록이다(번호는 순서).
struct Assignments {
    explicit Assignments(Allocator& allocator) noexcept : list(allocator) {
    }

    Assignments& Add(StringView text) noexcept {
        TextAssignment assignment;
        CHECK(reflection_text::MakeTextAssignment(text, static_cast<uint32>(list.Size()),
                                                  assignment) &&
              list.TryPushBack(assignment));
        return *this;
    }

    Array<TextAssignment> list;
};

Result<PackageContents> ReadFile(AssetEnv& env, ImportScene& scene, StringView relative) noexcept {
    return ReadPackage(env.allocator, scene.files.Bytes(relative), ReadOptions());
}

TestAsset* Representative(EditorPackageSet& set, AssetId id) noexcept {
    const EditorPackageInfo* info = set.Find(id);
    return info == nullptr ? nullptr : AssetOf(set, info->package);
}

bool HasText(Span<const uint8> bytes, StringView text) noexcept {
    for (usize at = 0; at + text.ByteCount() <= bytes.Size(); ++at) {
        if (StringView(reinterpret_cast<const char*>(bytes.Data()) + at, text.ByteCount()) ==
            text) {
            return true;
        }
    }

    return false;
}

// 메모리 파일 시스템의 Content 아래 .rasset 수다.
usize PackageFiles(ImportScene& scene) noexcept {
    usize count = 0;
    for (usize at = 0; at < scene.files.NodeCount(); ++at) {
        count += scene.files.NodeName(at).EndsWith(".rasset") ? 1 : 0;
    }

    return count;
}

// Tags hook: failTagsOn번째 호출이 Busy다(0이면 끔 — 저장 실패 시험).
uint32 tagCalls = 0;
uint32 failTagsOn = 0;

Result<void> CountingTags(const AssetTypeHookContext&, AssetTagSink& sink) noexcept {
    ++tagCalls;
    if (failTagsOn != 0 && tagCalls == failTagsOn) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    return sink.Add("Kind", "Imported");
}

} // namespace

// 원본 하나: 주 출력 파일·헤더 id, 소유 기록(임포터 id·이름·버전, 원본 역할·루트 Project·상대
// 경로·SHA-256·byte 수, 옵션 Record — schema = 옵션 struct·버전, JSON에 scale, 출력 목록 하나),
// 옵션(scale)과 설정(note — 저장 속성)의 나누기, 값·라벨·bulk, 색인 요약(소유 기록·출력 목록),
// 임포터 한 번, 경고 없음, 패키지는 열린 채.
TEST(AssetImport_NewAsset) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-new");
    REQUIRE(scene.ready);
    constexpr StringView Text = "value=5\nlabel=Hi\nbulk=0a0b0c";
    REQUIRE(scene.PutSource(RockSource, Text));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    Assignments assignments(env.allocator);
    assignments.Add("scale=3").Add("note=Remember");
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(service.Import(scene.lock, "/Game/Rock", inputs, {}, assignments.list.View(),
                           diagnostics, result, &warnings));
    CHECK(!result.id.IsNull() && result.path.View() == "/Game/Rock" && result.outputs.IsEmpty() &&
          warnings.IsEmpty() && scene.test.calls == 1);
    auto read = ReadFile(env, scene, "Content/Rock.rasset");
    REQUIRE(read);
    CHECK(read.Value().header.assetId == result.id);
    auto record = ReadImportRecord(env.allocator, result.id, read.Value().importRecord);
    REQUIRE(record);
    const ImportRecord& owned = record.Value();
    CHECK(owned.OwnedBy(result.id) && owned.importerId == TestImporterId &&
          owned.importerName.View() == "TestImporter" &&
          owned.importerVersion == TestImporterVersion);
    REQUIRE(owned.sources.Size() == 1);
    CHECK(owned.sources[0].role.View() == DefaultImportSourceRole &&
          owned.sources[0].root.View() == ProjectSourceRootName &&
          owned.sources[0].relativePath.View() == RockSource &&
          owned.sources[0].sha256 == Sha256::Hash(Bytes(Text)) &&
          owned.sources[0].bytes == Text.ByteCount());
    REQUIRE(owned.outputs.Size() == 1);
    CHECK(owned.outputs[0].outputKey.IsEmpty() && owned.outputs[0].asset == result.id &&
          owned.outputs[0].type == IdOf<TestAsset>());
    // 옵션 Record: schema = TestImportOptions, 버전 그대로, 필드 1 = JSON(scale 3).
    const TypeId optionsType = GeneratedType<TestImportOptions>::Id();
    auto lease = env.types.AcquireValueType(optionsType, TypeKind::Struct);
    REQUIRE(lease);
    const uint32 version = lease.Value().Schema().version;
    auto options =
        RecordReader::Open(env.allocator, owned.options.View(), optionsType, version, version);
    REQUIRE(options);
    auto json = options.Value().Find(Guid{0, 1});
    REQUIRE(json);
    CHECK(HasText(json.Value().payload, "\"scale\": 3") &&
          HasText(json.Value().payload, "\"failAt\": 0"));
    // 값(5 × 3)·라벨·설정 note·bulk.
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Rock", AssetInfoOptions{}, out));
    TestAsset* asset = Representative(service.Packages(), result.id);
    REQUIRE(asset != nullptr);
    CHECK(asset->value == 15 && asset->label.View() == "Hi" && asset->note.View() == "Remember");
    auto pixels = service.Packages().Find(result.id)->store->Read(asset->pixels);
    REQUIRE(pixels);
    REQUIRE(pixels.Value().Size() == 3);
    CHECK(pixels.Value()[0] == 0x0a && pixels.Value()[2] == 0x0c);
    // 색인: 소유 기록 요약.
    auto entry = service.Index().FindById(result.id);
    REQUIRE(entry);
    CHECK(entry.Value()->importRecord.present && entry.Value()->importRecord.owner == result.id &&
          entry.Value()->importRecord.outputs.Size() == 1);
    CHECK(Matches(env, scene, service.Index()));
    // 경고 목록 없이·옵션 없이(기본 scale 1).
    REQUIRE(scene.PutSource("SourceArt/Pebble.trock", "value=2"));
    const String pebble = scene.Absolute(env.allocator, "SourceArt/Pebble.trock");
    const AssetImportSourceInput pebbleInputs[] = {{DefaultImportSourceRole, pebble.View()}};
    AssetImportResult quiet(env.allocator);
    REQUIRE(service.Import(scene.lock, "/Game/Pebble", pebbleInputs, "testimporter", {},
                           diagnostics, quiet));
    REQUIRE(service.Info("/Game/Pebble", AssetInfoOptions{}, out));
    CHECK(Representative(service.Packages(), quiet.id)->value == 2);
}

// 추가 출력: Output 순서대로 `<path>_<key>`에 저장, 참조 기록(owner = 주 출력)·ImportOwner 의존,
// 주 출력의 소유 기록 출력 목록(key 순 — 빈 key 먼저), 출력 사이 Hard 참조, 고아 없음, 경고(Warn)
// ImportWarning(코드·detail·asset = 주 출력), AbsoluteSourcePath(Project를 다른 곳으로 — 원본은
// 절대 경로로 기록).
TEST(AssetImport_MultipleOutputs) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-multi");
    REQUIRE(scene.ready);
    StringBuilder<128> text(env.allocator);
    REQUIRE(text.TryAppendFormat("value=1\noutputs=extra:TestAsset,base:BaseGameAsset\n"
                                 "reference=extra\nwarn={}",
                                 static_cast<uint32>(AssetDiagnosticCode::UvOutOfRange)));
    REQUIRE(scene.PutSource(RockSource, text.View()));
    const char roots[] = "{\"roots\": {\"Project\": \"Content/Elsewhere\"}}";
    REQUIRE(scene.files.AddFile("Config/AssetSources.json", Bytes(roots)));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(
        service.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, result, &warnings));
    REQUIRE(result.outputs.Size() == 2);
    CHECK(result.outputs[0].key.View() == "extra" &&
          result.outputs[0].path.View() == "/Game/Rock_extra" &&
          result.outputs[1].key.View() == "base" &&
          result.outputs[1].path.View() == "/Game/Rock_base");
    REQUIRE(warnings.Size() == 2);
    CHECK(warnings[0].kind == EditorWarningKind::ImportWarning &&
          warnings[0].diagnostic.code == AssetDiagnosticCode::AbsoluteSourcePath &&
          warnings[0].diagnostic.detail == 0 && warnings[0].diagnostic.asset == result.id);
    CHECK(warnings[1].kind == EditorWarningKind::ImportWarning &&
          warnings[1].diagnostic.code == AssetDiagnosticCode::UvOutOfRange &&
          warnings[1].diagnostic.detail == 7 && warnings[1].diagnostic.asset == result.id);
    // 주 출력: 소유 기록 출력 셋(key 순), 원본은 절대 경로, Hard 의존 extra.
    auto main = ReadFile(env, scene, "Content/Rock.rasset");
    REQUIRE(main);
    auto owned = ReadImportRecord(env.allocator, result.id, main.Value().importRecord);
    REQUIRE(owned);
    REQUIRE(owned.Value().outputs.Size() == 3);
    CHECK(owned.Value().outputs[0].outputKey.IsEmpty() &&
          owned.Value().outputs[1].outputKey.View() == "base" &&
          owned.Value().outputs[1].type == IdOf<BaseGameAsset>() &&
          owned.Value().outputs[2].outputKey.View() == "extra");
    CHECK(owned.Value().sources[0].root.IsEmpty() &&
          owned.Value().sources[0].relativePath.View() == source.View());
    bool hard = false;
    for (const PackageDependencyEntry& entry : main.Value().dependencies) {
        hard = hard ||
               (entry.kind == PackageDependencyKind::Hard && entry.asset == result.outputs[0].id);
    }

    CHECK(hard);
    // 추가 출력: 참조 기록(owner = 주 출력)·ImportOwner 의존.
    for (const AssetImportOutput& output : result.outputs) {
        String file(env.allocator);
        REQUIRE(file.TryAppend("Content/Rock_") && file.TryAppend(output.key.View()) &&
                file.TryAppend(".rasset"));
        auto extra = ReadFile(env, scene, file.View());
        REQUIRE(extra);
        auto reference = ReadImportRecord(env.allocator, output.id, extra.Value().importRecord);
        REQUIRE(reference);
        CHECK(reference.Value().owner == result.id);
        bool owner = false;
        for (const PackageDependencyEntry& entry : extra.Value().dependencies) {
            owner = owner ||
                    (entry.kind == PackageDependencyKind::ImportOwner && entry.asset == result.id);
        }

        CHECK(owner);
    }

    Array<const AssetIndexEntry*> orphans(env.allocator);
    REQUIRE(service.Index().Orphans(orphans));
    CHECK(orphans.IsEmpty() && Matches(env, scene, service.Index()));
    // 경고 목록 없이도(절대 경로·임포터 경고).
    AssetImportResult quiet(env.allocator);
    REQUIRE(service.Import(scene.lock, "/Game/Quiet", inputs, {}, {}, diagnostics, quiet));
    CHECK(!quiet.id.IsNull() && quiet.outputs.Size() == 2);
}

// 역할 둘(CubeImporter — 이름으로만): 원본 역할 순(px·nx)으로 기록, 값의 합. 역할 위반(빠짐·
// 넘침·같은 역할 둘·모르는 역할) InvalidImportOption, 옵션 없는 임포터의 옵션 이름은
// UnknownProperty.
TEST(AssetImport_Roles) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-roles");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource("SourceArt/px.png", "value=2") &&
            scene.PutSource("SourceArt/nx.png", "value=3"));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String px = scene.Absolute(env.allocator, "SourceArt/px.png");
    const String nx = scene.Absolute(env.allocator, "SourceArt/nx.png");
    const AssetImportSourceInput inputs[] = {{"nx", nx.View()}, {"px", px.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    REQUIRE(
        service.Import(scene.lock, "/Game/Cube", inputs, "CubeImporter", {}, diagnostics, result));
    auto read = ReadFile(env, scene, "Content/Cube.rasset");
    REQUIRE(read);
    auto record = ReadImportRecord(env.allocator, result.id, read.Value().importRecord);
    REQUIRE(record && record.Value().sources.Size() == 2);
    CHECK(record.Value().sources[0].role.View() == "px" &&
          record.Value().sources[0].relativePath.View() == "SourceArt/px.png" &&
          record.Value().sources[1].role.View() == "nx" && record.Value().options.IsEmpty());
    String out(env.allocator);
    REQUIRE(service.Info("/Game/Cube", AssetInfoOptions{}, out));
    CHECK(Representative(service.Packages(), result.id)->value == 5);
    auto rejects = [&](Span<const AssetImportSourceInput> given, StringView path) noexcept {
        AssetDiagnostic failure;
        auto imported = service.Import(scene.lock, path, given, "CubeImporter", {}, diagnostics,
                                       result, nullptr, &failure);
        return !imported && imported.Error() == ErrorCode::InvalidArgument &&
               failure.code == AssetDiagnosticCode::InvalidImportOption;
    };
    const AssetImportSourceInput missing[] = {{"px", px.View()}};
    const AssetImportSourceInput extra[] = {
        {"px", px.View()}, {"nx", nx.View()}, {"py", px.View()}};
    const AssetImportSourceInput twice[] = {{"px", px.View()}, {"px", nx.View()}};
    const AssetImportSourceInput unknown[] = {{"px", px.View()}, {"zz", nx.View()}};
    CHECK(rejects(missing, "/Game/C1"));
    CHECK(rejects(extra, "/Game/C2"));
    CHECK(rejects(twice, "/Game/C3"));
    CHECK(rejects(unknown, "/Game/C4"));
    Assignments assignments(env.allocator);
    assignments.Add("value=1").Add("scale=2");
    AssetDiagnostic failure;
    auto unknownName =
        service.Import(scene.lock, "/Game/C5", inputs, "CubeImporter", assignments.list.View(),
                       diagnostics, result, nullptr, &failure);
    CHECK(!unknownName && failure.code == AssetDiagnosticCode::UnknownProperty &&
          failure.detail == 1);
    CHECK(Matches(env, scene, service.Index()));
}

// 거부(아무것도 쓰지 않고 이번에 연 패키지는 닫는다): Initialize 전·잠금 아님·원본 없음, 경로
// 규칙·별칭·점유, 임포터 표 없음·이름 없음·확장자 없음·확장자 섞임, 모르는 이름, 옵션 값 오류, 설정
// 값 오류, 원본 루트 표 손상·상대 원본·없는 원본, 임포터 실패(Fail 진단·진단 없는 실패·출력 뒤
// 실패), 주 출력 없음, 출력 규칙(주 출력 타입·key·같은 key·추상·에셋 아님·점유·별칭·개수), 남의
// 객체 AddBulk, 경고 아닌 Warn.
TEST(AssetImport_Rejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-reject");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource(RockSource, "value=1"));
    REQUIRE(scene.files.AddFile("Content/Rock_taken.rasset", sources.villain.View()));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    AssetDiagnostic failure;
    CHECK(service
              .Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, result, nullptr,
                      &failure)
              .Error() == ErrorCode::InvalidArgument);
    REQUIRE(service.Initialize());
    {
        auto aliases = service.Aliases();
        auto created = DateTime::TryFromTicks(scene.clock.now);
        REQUIRE(aliases && created);
        const AssetAliasInput add[] = {{"/Game/Aliased", FixedId(LeafId), created.Value(), 9},
                                       {"/Game/Rock_aliased", FixedId(LeafId), created.Value(), 9}};
        bool notDurable = false;
        REQUIRE(aliases.Value()->Write(scene.files, scene.lock, {}, add, notDurable));
    }

    const usize packages = PackageFiles(scene);
    const usize open = service.Packages().Count();
    auto unchanged = [&]() noexcept {
        return PackageFiles(scene) == packages && service.Packages().Count() == open &&
               Matches(env, scene, service.Index());
    };
    auto rejects = [&](StringView path, Span<const AssetImportSourceInput> given,
                       StringView importer, Span<const TextAssignment> assignments, ErrorCode error,
                       AssetDiagnosticCode code) noexcept {
        failure = AssetDiagnostic{};
        diagnostics.Clear();
        auto imported = service.Import(scene.lock, path, given, importer, assignments, diagnostics,
                                       result, nullptr, &failure);
        return !imported && imported.Error() == error && failure.code == code &&
               result.id.IsNull() && unchanged();
    };
    AssetDatabaseLock other;
    CHECK(service.Import(other, "/Game/Rock", inputs, {}, {}, diagnostics, result).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(rejects("/Game/Rock", {}, {}, {}, ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    CHECK(rejects("Game/Rock", inputs, {}, {}, ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Aliased", inputs, {}, {}, ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath) &&
          failure.related == FixedId(LeafId));
    CHECK(rejects("/Game/Leaf", inputs, {}, {}, ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    CHECK(
        service.Import(scene.lock, "/Game/Aliased", inputs, {}, {}, diagnostics, result).Error() ==
        ErrorCode::AlreadyExists);
    CHECK(rejects("/Game/Rock", inputs, "Nope", {}, ErrorCode::NotFound,
                  AssetDiagnosticCode::UnsupportedSourceFormat));
    // 확장자: 없음·모름·섞임(detail = 원본 번호).
    REQUIRE(scene.PutSource("SourceArt/Rock.xyz", "value=1") &&
            scene.PutSource("SourceArt/Rock", "value=1"));
    const String unknownExtension = scene.Absolute(env.allocator, "SourceArt/Rock.xyz");
    const String noExtension = scene.Absolute(env.allocator, "SourceArt/Rock");
    const AssetImportSourceInput unknown[] = {{DefaultImportSourceRole, unknownExtension.View()}};
    const AssetImportSourceInput bare[] = {{DefaultImportSourceRole, noExtension.View()}};
    const AssetImportSourceInput mixed[] = {{DefaultImportSourceRole, source.View()},
                                            {"other", unknownExtension.View()}};
    REQUIRE(scene.PutSource("SourceArt/Rock.tcube", "value=1"));
    const String cubeExtension = scene.Absolute(env.allocator, "SourceArt/Rock.tcube");
    const AssetImportSourceInput twoImporters[] = {{DefaultImportSourceRole, source.View()},
                                                   {"other", cubeExtension.View()}};
    const AssetImportSourceInput unparsable[] = {
        {DefaultImportSourceRole, StringView("C:/a\0b.trock", 13)}};
    CHECK(rejects("/Game/Rock", unknown, {}, {}, ErrorCode::Unsupported,
                  AssetDiagnosticCode::UnsupportedSourceFormat));
    CHECK(rejects("/Game/Rock", bare, {}, {}, ErrorCode::Unsupported,
                  AssetDiagnosticCode::UnsupportedSourceFormat));
    CHECK(rejects("/Game/Rock", mixed, {}, {}, ErrorCode::Unsupported,
                  AssetDiagnosticCode::UnsupportedSourceFormat) &&
          failure.detail == 1);
    // 같은 임포터의 원본 둘은 확장자 검사를 지나 역할에서 거부된다(원본 하나 임포터).
    const AssetImportSourceInput sameImporter[] = {{DefaultImportSourceRole, source.View()},
                                                   {"other", source.View()}};
    CHECK(rejects("/Game/Rock", sameImporter, {}, {}, ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidImportOption));
    CHECK(rejects("/Game/Rock", twoImporters, {}, {}, ErrorCode::Unsupported,
                  AssetDiagnosticCode::UnsupportedSourceFormat) &&
          failure.detail == 1);
    CHECK(rejects("/Game/Rock", unparsable, {}, {}, ErrorCode::Unsupported,
                  AssetDiagnosticCode::UnsupportedSourceFormat) &&
          failure.detail == 0);
    // 대입: 모르는 이름(detail = 대입 번호 — 첫 이름은 `.`·`[` 앞)·옵션 값·설정 값.
    {
        Assignments unknownName(env.allocator);
        unknownName.Add("label=x").Add("missing=1");
        CHECK(rejects("/Game/Rock", inputs, {}, unknownName.list.View(), ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::UnknownProperty) &&
              failure.detail == 1);
        Assignments dotted(env.allocator);
        dotted.Add("missing.sub=1");
        CHECK(rejects("/Game/Rock", inputs, {}, dotted.list.View(), ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::UnknownProperty));
        Assignments indexed(env.allocator);
        indexed.Add("missing[0]=1");
        CHECK(rejects("/Game/Rock", inputs, {}, indexed.list.View(), ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::UnknownProperty));
        Assignments badOption(env.allocator);
        badOption.Add("scale=abc");
        CHECK(rejects("/Game/Rock", inputs, {}, badOption.list.View(), ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::InvalidImportOption) &&
              diagnostics.Count() != 0);
        Assignments badSetting(env.allocator);
        badSetting.Add("value=abc");
        CHECK(rejects("/Game/Rock", inputs, {}, badSetting.list.View(), ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::None) &&
              diagnostics.Count() != 0 && scene.test.calls > 0);
    }

    // 원본: 상대 경로·없음(detail = 원본 번호)·루트 표 손상.
    const AssetImportSourceInput relative[] = {{DefaultImportSourceRole, RockSource}};
    CHECK(rejects("/Game/Rock", relative, {}, {}, ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidSource));
    const String missingPath = scene.Absolute(env.allocator, "SourceArt/Missing.trock");
    const AssetImportSourceInput missing[] = {{DefaultImportSourceRole, missingPath.View()}};
    CHECK(rejects("/Game/Rock", missing, {}, {}, ErrorCode::NotFound,
                  AssetDiagnosticCode::MissingSource) &&
          failure.detail == 0);
    REQUIRE(scene.files.AddFile("Config/AssetSources.json", Bytes("{")));
    CHECK(rejects("/Game/Rock", inputs, {}, {}, ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidSource));
    scene.files.Remove("Config/AssetSources.json");

    // 임포터 실패: Fail 진단(원본 뒤·출력 뒤)·진단 없는 실패(주 출력 뒤).
    auto failsAt = [&](int32 stage, ErrorCode error, AssetDiagnosticCode code,
                       uint64 detail) noexcept {
        Assignments option(env.allocator);
        char text[32] = {};
        FixedStringBuilder builder{Span<char>(text)};
        CHECK(builder.TryAppendFormat("failAt={}", stage));
        option.Add(builder.View());
        CHECK(scene.PutSource(RockSource, "value=1\noutputs=extra"));
        const bool rejected = rejects("/Game/Rock", inputs, {}, option.list.View(), error, code);
        return rejected && failure.detail == detail;
    };
    CHECK(failsAt(1, ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidSource, 5));
    // 실패한 임포트는 이번 임포트의 경고(임포터 경고)를 남기지 않는다.
    {
        StringBuilder<64> warned(env.allocator);
        REQUIRE(warned.TryAppendFormat("value=1\nwarn={}",
                                       static_cast<uint32>(AssetDiagnosticCode::UvOutOfRange)));
        REQUIRE(scene.PutSource(RockSource, warned.View()));
        Assignments option(env.allocator);
        option.Add("failAt=3");
        Array<EditorWarning> warnings(env.allocator);
        auto imported = service.Import(scene.lock, "/Game/Rock", inputs, {}, option.list.View(),
                                       diagnostics, result, &warnings);
        CHECK(!imported && warnings.IsEmpty() && unchanged());
    }

    CHECK(failsAt(2, ErrorCode::Busy, AssetDiagnosticCode::None, 0));
    CHECK(failsAt(3, ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidSource, 6));

    // 출력 규칙(임포터가 Output 실패를 그대로 돌려준다).
    auto outputRejects = [&](StringView text, ErrorCode error, AssetDiagnosticCode code) noexcept {
        CHECK(scene.PutSource(RockSource, text));
        return rejects("/Game/Rock", inputs, {}, {}, error, code);
    };
    CHECK(
        outputRejects("value=1\nnomain=1", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    CHECK(outputRejects("mainType=BaseGameAsset", ErrorCode::InvalidArgument,
                        AssetDiagnosticCode::None));
    CHECK(outputRejects("outputs=Ok_9,a-b", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    CHECK(outputRejects("outputs=a{", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    CHECK(outputRejects("outputs=a/b", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    CHECK(outputRejects("outputs=a[", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    CHECK(outputRejects("outputs=a`", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    CHECK(outputRejects("outputs=a@", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    {
        StringBuilder<128> longKey(env.allocator);
        REQUIRE(longKey.TryAppend("outputs="));
        for (usize index = 0; index <= MaxImportOutputKeyBytes; ++index) {
            REQUIRE(longKey.TryAppend("k"));
        }

        CHECK(outputRejects(longKey.View(), ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    }

    CHECK(outputRejects("outputs=a,a", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    CHECK(outputRejects("outputs=x:Asset", ErrorCode::InvalidArgument,
                        AssetDiagnosticCode::TypeMismatch));
    CHECK(outputRejects("outputs=x:TestPart", ErrorCode::InvalidArgument,
                        AssetDiagnosticCode::TypeMismatch));
    CHECK(
        outputRejects("outputs=x:Unknown", ErrorCode::NotFound, AssetDiagnosticCode::TypeMismatch));
    CHECK(outputRejects("outputs=x:Options", ErrorCode::InvalidArgument,
                        AssetDiagnosticCode::TypeMismatch));
    CHECK(outputRejects("outputs=taken", ErrorCode::AlreadyExists,
                        AssetDiagnosticCode::DuplicatePath));
    CHECK(outputRejects("outputs=aliased", ErrorCode::AlreadyExists,
                        AssetDiagnosticCode::DuplicatePath) &&
          failure.related == FixedId(LeafId));
    CHECK(outputRejects("bulkForeign=1", ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    {
        char text[32] = {};
        FixedStringBuilder builder{Span<char>(text)};
        CHECK(builder.TryAppendFormat("warn={}",
                                      static_cast<uint32>(AssetDiagnosticCode::CorruptPackage)));
        CHECK(outputRejects(builder.View(), ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    }

    // 출력 수 상한(1024 받음 — 주 출력 포함, 1025번째 거부).
    {
        StringBuilder<1024> text(env.allocator);
        CHECK(text.TryAppend("outputs="));
        for (usize index = 0; index < MaxImportOutputs; ++index) {
            CHECK(text.TryAppendFormat(index == 0 ? "k{}" : ",k{}", index));
        }

        CHECK(outputRejects(text.View(), ErrorCode::InvalidArgument, AssetDiagnosticCode::None));
    }

    // 주 경로 점유: 저장하지 않은 새 패키지(원본을 읽기 전에), 디스크에만 있는 파일(Create가).
    REQUIRE(scene.PutSource(RockSource, "value=1"));
    {
        auto fresh = ParseLogicalPath(env.allocator, "/Game/Fresh");
        REQUIRE(fresh);
        REQUIRE(service.Packages().Create(fresh.Value(), IdOf<TestAsset>(), FixedId(0xf77)));
        failure = AssetDiagnostic{};
        auto occupied = service.Import(scene.lock, "/Game/FRESH", inputs, {}, {}, diagnostics,
                                       result, nullptr, &failure);
        CHECK(!occupied && occupied.Error() == ErrorCode::AlreadyExists &&
              failure.code == AssetDiagnosticCode::DuplicatePath);
        REQUIRE(scene.files.AddFile("Content/DiskOnly.rasset", sources.villain.View()));
        failure = AssetDiagnostic{};
        auto diskOnly = service.Import(scene.lock, "/Game/DiskOnly", inputs, {}, {}, diagnostics,
                                       result, nullptr, &failure);
        CHECK(!diskOnly && diskOnly.Error() == ErrorCode::AlreadyExists &&
              failure.code == AssetDiagnosticCode::DuplicatePath);
        scene.files.Remove("Content/DiskOnly.rasset");
        REQUIRE(service.Packages().Close(FixedId(0xf77)));
    }

    // 집합 API: 열리지 않은 id NotFound, Initialize 전 InvalidArgument.
    CHECK(service.Packages().SetImportRecord(FixedId(0xabc), {}, AssetId{}).Error() ==
          ErrorCode::NotFound);
    {
        Project bare(env.allocator);
        REQUIRE(bare.ready && bare.Rebuild());
        EditorPackageSet uninitialized(env.allocator, rocks::editor_set_test::Config(env, bare));
        CHECK(uninitialized.SetImportRecord(FixedId(LeafId), {}, AssetId{}).Error() ==
              ErrorCode::InvalidArgument);
    }

    // 표 없는 서비스.
    {
        AssetServiceConfig config = scene.Config(env);
        config.importers = nullptr;
        AssetService bare(env.allocator, scene.layout, config);
        REQUIRE(bare.Initialize());
        failure = AssetDiagnostic{};
        auto imported = bare.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, result,
                                    nullptr, &failure);
        CHECK(!imported && imported.Error() == ErrorCode::Unsupported &&
              failure.code == AssetDiagnosticCode::UnsupportedSourceFormat);
    }

    CHECK(unchanged());
}

// 저장 실패: 주 출력 저장이 실패하면 먼저 저장한 추가 출력 파일을 지우고 색인에서 뺀다(파일·색인
// 그대로, 패키지 닫힘). 지우기도 실패하면 경고 OrphanNotDeleted(asset = 그 출력)이고 파일이 고아
// (OwnerMissing)로 남는다. 첫 출력 저장 실패는 아무것도 쓰지 않는다.
TEST(AssetImport_SaveFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-save");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource(RockSource, "value=1\noutputs=extra"));
    auto registration =
        scene.hooks.Register(IdOf<TestAsset>(), AssetTypeHooks{&CountingTags, nullptr});
    REQUIRE(registration);
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    const usize packages = PackageFiles(scene);
    const usize open = service.Packages().Count();
    for (uint32 stage = 1; stage <= 2; ++stage) {
        tagCalls = 0;
        failTagsOn = stage;
        Array<EditorWarning> warnings(env.allocator);
        AssetDiagnostic failure;
        auto imported = service.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics,
                                       result, &warnings, &failure);
        failTagsOn = 0;
        CHECK(!imported && imported.Error() == ErrorCode::Busy && warnings.IsEmpty() &&
              PackageFiles(scene) == packages && service.Packages().Count() == open &&
              !scene.files.Has("Content/Rock_extra.rasset") && result.outputs.IsEmpty() &&
              Matches(env, scene, service.Index()));
    }

    // 지우기 실패: 경고, 파일은 고아로 남는다.
    tagCalls = 0;
    failTagsOn = 2;
    scene.files.faults.remove = MemoryBusy;
    Array<EditorWarning> warnings(env.allocator);
    auto imported =
        service.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, result, &warnings);
    scene.files.faults.remove = {};
    failTagsOn = 0;
    CHECK(!imported && scene.files.Has("Content/Rock_extra.rasset"));
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorWarningKind::OrphanNotDeleted &&
          warnings[0].diagnostic.error == ErrorCode::Busy &&
          !warnings[0].diagnostic.asset.IsNull());
    Array<const AssetIndexEntry*> orphans(env.allocator);
    REQUIRE(service.Index().Orphans(orphans));
    CHECK(orphans.Size() == 1 && Matches(env, scene, service.Index()));
    // 경고 목록 없이도.
    tagCalls = 0;
    failTagsOn = 2;
    scene.files.faults.remove = MemoryBusy;
    REQUIRE(scene.PutSource(RockSource, "value=1\noutputs=second"));
    CHECK(!service.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, result));
    scene.files.faults.remove = {};
    failTagsOn = 0;
}

// 할당 실패: 추가 출력·옵션·설정·경고가 있는 임포트의 모든 할당 실패가 OutOfMemory이고 파일·
// 색인·열린 패키지가 그대로다(GC 표 실패는 Registry가 삼킨 성공).
TEST(AssetImport_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-oom");
    REQUIRE(scene.ready);
    StringBuilder<128> text(env.allocator);
    REQUIRE(text.TryAppendFormat("value=2\nbulk=0102\noutputs=extra\nreference=extra\nwarn={}",
                                 static_cast<uint32>(AssetDiagnosticCode::UvOutOfRange)));
    REQUIRE(scene.PutSource(RockSource, text.View()));
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    Assignments assignments(env.allocator);
    assignments.Add("scale=2").Add("note=n");
    // 원본을 절대 경로로 기록하게(경고 자리) Project를 다른 곳으로 둔다.
    const char roots[] = "{\"roots\": {\"Project\": \"Content/Elsewhere\"}}";
    REQUIRE(scene.files.AddFile("Config/AssetSources.json", Bytes(roots)));
    TaggedFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetService service(failing, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        // 열린 패키지가 있으면 열린 id 기록이 할당한다.
        String out(env.allocator);
        REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
        // 저장하지 않은 새 패키지가 있으면 주 경로 점유 판정이 그 경로를 읽는다.
        auto pendingPath = ParseLogicalPath(env.allocator, "/Game/Pending");
        REQUIRE(pendingPath);
        REQUIRE(service.Packages().Create(pendingPath.Value(), IdOf<TestAsset>(), FixedId(0xf78)));
        const usize open = service.Packages().Count();
        const usize packages = PackageFiles(scene);
        TextDiagnostics diagnostics(env.allocator);
        AssetImportResult result(failing);
        Array<EditorWarning> warnings(failing);
        failing.FailOnAttempt(attempt);
        auto imported = service.Import(scene.lock, "/Game/Rock", inputs, {},
                                       assignments.list.View(), diagnostics, result, &warnings);
        const bool failed = failing.Failed();
        const uint32 tag = failing.FailedTag();
        failing.DisableFailure();
        if (!failed || (imported && tag == GcReferenceTableTag)) {
            REQUIRE(imported);
            CHECK(scene.files.Has("Content/Rock.rasset") &&
                  scene.files.Has("Content/Rock_extra.rasset") &&
                  Matches(env, scene, service.Index()));
            if (!failed) {
                break;
            }

            scene.files.Remove("Content/Rock.rasset");
            scene.files.Remove("Content/Rock_extra.rasset");
            continue;
        }

        CHECK(!imported && imported.Error() == ErrorCode::OutOfMemory);
        CHECK(PackageFiles(scene) == packages && service.Packages().Count() == open &&
              Matches(env, scene, service.Index()));
        ++failures;
    }

    CHECK(failures > 0);
}

// 실제 디스크: 디스크의 원본을 임포트하고 다시 열어 bulk를 읽는다.
TEST(AssetImport_RealDisk) {
    AssetEnv env;
    REQUIRE(env.Ready());
    TempProject project("import-disk");
    REQUIRE(project.made);
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    String sourcePath(env.allocator);
    REQUIRE(sourcePath.TryAppend(layout.Root()) && sourcePath.TryAppend("/Rock.trock"));
    {
        platform::File file;
        REQUIRE(file.Open(platform::PhysicalFileSystem(), sourcePath.View().Data(),
                          sourcePath.View().ByteCount(), platform::FileOpenMode::CreateAlways));
        constexpr StringView Text = "value=4\nbulk=ff";
        REQUIRE(file.WriteAll(Text.Data(), Text.ByteCount()).status);
        REQUIRE(file.Close());
    }

    AssetDatabaseLock lock;
    REQUIRE(lock.Acquire(layout, 0));
    AssetTypeHookTable hooks(env.allocator, env.types);
    AssetImporterTable importers(env.allocator, env.types);
    TestImporter importer(TestImporterInfo());
    auto registered = importers.Register(importer);
    REQUIRE(registered);
    AssetServiceConfig config;
    config.packages.types = &env.types;
    config.packages.files = &platform::PhysicalFileSystem();
    config.packages.hooks = &hooks;
    config.packages.read.engineVersion = TestEngineVersion;
    config.packages.registryContext = ContextId{0x494d5044ull, 1};
    config.packages.registryOptions.names = &env.names;
    config.importers = &importers;
    AssetService service(env.allocator, layout, config);
    REQUIRE(service.Initialize());
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, sourcePath.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    REQUIRE(service.Import(lock, "/Game/Rock", inputs, {}, {}, diagnostics, result));
    AssetService reopened(env.allocator, layout, config);
    REQUIRE(reopened.Initialize());
    String out(env.allocator);
    REQUIRE(reopened.Info("/Game/Rock", AssetInfoOptions{}, out));
    TestAsset* asset = Representative(reopened.Packages(), result.id);
    REQUIRE(asset != nullptr);
    auto pixels = reopened.Packages().Find(result.id)->store->Read(asset->pixels);
    CHECK(asset->value == 4 && pixels && pixels.Value().Size() == 1 && pixels.Value()[0] == 0xff);
}

} // namespace asset_import_tests_file
