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
#include <Base/InitializerList.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Time/DateTime.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>

// 새 임포트 시험이다(Docs/Design/AssetSystem 7.1절 18 A2f1b): 원본 하나·여럿(역할)·추가
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

// 두 byte 열이 같은지다.
bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
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

// 임포터 태그(7.1절 19 A2f3a UXR8): 새 임포트의 AddTag가 소유 기록 필드 8과 패키지 Tags(색인 항목
// 태그)에 남는다(같은 쌍은 한 번). 재임포트는 PreviousTags로 옛 태그를 보이고 이번에 더한 것만
// 남긴다(다시 더하지 않으면 사라진다). 복제는 태그를 옮긴다. 규칙 위반 태그는 임포트 실패(아무것도
// 쓰지 않음)다. 엔진 태그 이름공간 이름(`Bundle`)의 임포터는 등록할 수 없고, 역할에 `:`를 쓸 수
// 있다(A2f3b — `blob:<sha256>`).
TEST(AssetImport_ImporterTags) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-tags");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource(RockSource, "value=1\ntags=TestImporter.Path|/Game/A,"
                                        "TestImporter.Path|/Game/A,TestImporter.Former|x"));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    REQUIRE(service.Import(scene.lock, "/Game/Tagged", inputs, {}, {}, diagnostics, result));
    const auto tagCount = [&](AssetId id, StringView key, StringView value) noexcept {
        auto entry = service.Index().FindById(id);
        usize count = 0;
        for (const PackageTag& tag :
             entry ? entry.Value()->tags.View() : Span<const PackageTag>()) {
            count +=
                tag.key.View() == key && (value.IsEmpty() || tag.value.View() == value) ? 1 : 0;
        }

        return count;
    };
    CHECK(tagCount(result.id, "TestImporter.Path", "/Game/A") == 1 &&
          tagCount(result.id, "TestImporter.Former", "x") == 1);
    CHECK(service.Index().FindById(result.id).Value()->tags.Size() == 2);
    auto read = ReadFile(env, scene, "Content/Tagged.rasset");
    REQUIRE(read);
    auto record = ReadImportRecord(env.allocator, result.id, read.Value().importRecord);
    REQUIRE(record);
    CHECK(record.Value().tags.Size() == 2 && scene.test.lastPreviousTags == 0);

    // 재임포트: 옛 태그 둘을 보이고(keeptags) 새 태그를 더한다.
    REQUIRE(scene.PutSource(RockSource, "value=2\nkeeptags=1\ntags=TestImporter.Former|y"));
    AssetImportResult again(env.allocator);
    REQUIRE(service.Reimport(scene.lock, "/Game/Tagged", {}, {}, diagnostics, again));
    CHECK(scene.test.lastPreviousTags == 2 && scene.test.lastReimport);
    CHECK(tagCount(result.id, "TestImporter.Former", "") == 2 &&
          service.Index().FindById(result.id).Value()->tags.Size() == 3);

    // 다시 더하지 않으면 사라진다.
    REQUIRE(scene.PutSource(RockSource, "value=3"));
    REQUIRE(service.Reimport(scene.lock, "/Game/Tagged", {}, {}, diagnostics, again));
    CHECK(scene.test.lastPreviousTags == 3 &&
          service.Index().FindById(result.id).Value()->tags.IsEmpty());

    // 복제는 태그를 옮긴다.
    REQUIRE(scene.PutSource(RockSource, "value=4\ntags=TestImporter.Path|/Game/C"));
    REQUIRE(service.Reimport(scene.lock, "/Game/Tagged", {}, {}, diagnostics, again));
    AssetDuplicateResult copy(env.allocator);
    REQUIRE(service.Duplicate(scene.lock, "/Game/Tagged", "/Game/TaggedCopy", copy));
    CHECK(tagCount(copy.id, "TestImporter.Path", "/Game/C") == 1);

    // 규칙 위반 태그(접두가 다르다)는 임포트 실패이고 아무것도 쓰지 않는다.
    REQUIRE(scene.PutSource("SourceArt/Bad.trock", "value=1\ntags=Other.A|1"));
    const String bad = scene.Absolute(env.allocator, "SourceArt/Bad.trock");
    const AssetImportSourceInput badInputs[] = {{DefaultImportSourceRole, bad.View()}};
    AssetImportResult refused(env.allocator);
    CHECK(service.Import(scene.lock, "/Game/BadTag", badInputs, {}, {}, diagnostics, refused)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(!scene.files.Has("Content/BadTag.rasset"));

    // 등록: `Bundle` 이름(대소문자 무시)은 거부, 역할 `blob:abc`는 받는다.
    AssetImporterInfo bundle = TestImporterInfo();
    bundle.id = Guid{0x494d50540001ull, 9};
    bundle.name = "bundle";
    bundle.extensions = {};
    TestImporter bundleImporter(bundle);
    CHECK(scene.importers.Register(bundleImporter).Error() == ErrorCode::InvalidArgument);
    static constexpr StringView BlobRoles[] = {"blob:abc"};
    AssetImporterInfo blob = TestImporterInfo();
    blob.id = Guid{0x494d50540001ull, 10};
    blob.name = "BlobImporter";
    blob.extensions = {};
    blob.optionsType = TypeId{};
    blob.roles = BlobRoles;
    TestImporter blobImporter(blob);
    auto registered = scene.importers.Register(blobImporter);
    CHECK(registered);
}

// 다른 에셋 참조(7.1절 19 A2f3c UXR10): OpenAsset으로 색인의 에셋(Leaf)을 열어 Hard로 건다 — 저장이
// Hard 의존을 쓰고, 연 패키지는 끝나면 닫히며(원래 열려 있던 것은 열린 채), 읽기 전용 패키지도
// 연다. 거부: 자기 주 출력·재임포트의 기존 출력·null(InvalidArgument — 파일은 그대로), 색인에
// 없음(NotFound — MissingDependency, related = id, 파일을 쓰지 않는다), 같은 id 둘(AlreadyExists —
// DuplicateAssetId). 묶음 구성원 순서는 AssetImport_Group(A2f3d)이 본다.
TEST(AssetImport_OpenAsset) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-open");
    REQUIRE(scene.ready);
    // Leaf.rasset은 장면이 이미 넣었다.
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    const AssetId leaf = FixedId(LeafId);
    const auto putOpen = [&](StringView extra, AssetId id) noexcept {
        StringBuilder<256> text(env.allocator);
        return text.TryAppendFormat("value=1\n{}open={}", extra, id) &&
               scene.PutSource(RockSource, text.View());
    };
    const auto hardOn = [&](StringView file, AssetId target) noexcept {
        auto read = ReadFile(env, scene, file);
        if (!read) {
            return false;
        }

        bool hard = false;
        for (const PackageDependencyEntry& entry : read.Value().dependencies) {
            hard = hard || (entry.kind == PackageDependencyKind::Hard && entry.asset == target);
        }

        return hard;
    };
    const auto refuses = [&](AssetService& service, StringView path, bool reimport, ErrorCode error,
                             AssetDiagnosticCode code, AssetId related) noexcept {
        AssetImportResult refused(env.allocator);
        AssetDiagnostic failure;
        auto done = reimport ? service.Reimport(scene.lock, path, {}, {}, diagnostics, refused,
                                                nullptr, &failure)
                             : service.Import(scene.lock, path, inputs, {}, {}, diagnostics,
                                              refused, nullptr, &failure);
        return !done && done.Error() == error && failure.code == code &&
               (related.IsNull() || failure.related == related) && service.Packages().Count() == 0;
    };

    AssetId user{};
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        REQUIRE(putOpen({}, leaf));
        AssetImportResult result(env.allocator);
        REQUIRE(service.Import(scene.lock, "/Game/User", inputs, {}, {}, diagnostics, result));
        user = result.id;
        CHECK(hardOn("Content/User.rasset", leaf) && service.Packages().Count() == 0);

        // 원래 열려 있던 대상은 열린 채다.
        REQUIRE(service.Packages().Open(leaf));
        AssetImportResult again(env.allocator);
        REQUIRE(service.Reimport(scene.lock, "/Game/User", {}, {}, diagnostics, again));
        CHECK(service.Packages().FindPackage(leaf) && service.Packages().Count() == 1);
        REQUIRE(service.Packages().Close(leaf));

        // 자기 주 출력·null·기존 출력은 Output으로 받는다.
        REQUIRE(putOpen({}, user));
        CHECK(refuses(service, "/Game/User", true, ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::None, user));
        CHECK(hardOn("Content/User.rasset", leaf));
        REQUIRE(putOpen({}, AssetId{}));
        CHECK(refuses(service, "/Game/Null", false, ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::None, AssetId{}));
        CHECK(!scene.files.Has("Content/Null.rasset"));
        REQUIRE(putOpen("outputs=extra\n", leaf));
        AssetImportResult multi(env.allocator);
        REQUIRE(service.Import(scene.lock, "/Game/Multi", inputs, {}, {}, diagnostics, multi));
        REQUIRE(multi.outputs.Size() == 1);
        REQUIRE(putOpen("outputs=extra\n", multi.outputs[0].id));
        CHECK(refuses(service, "/Game/Multi", true, ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::None, multi.outputs[0].id));
        CHECK(hardOn("Content/Multi.rasset", leaf));

        // 색인에 없는 id.
        const AssetId missing = FixedId(990001);
        REQUIRE(putOpen({}, missing));
        CHECK(refuses(service, "/Game/Missing", false, ErrorCode::NotFound,
                      AssetDiagnosticCode::MissingDependency, missing));
        CHECK(!scene.files.Has("Content/Missing.rasset"));
    }

    // 읽기 전용 대상도 연다.
    PackageWriteOptions readOnly = EditorOptions();
    readOnly.customVersions = UnknownCustomVersions;
    const Array<uint8> readOnlyLeaf =
        Write(env.allocator, *env.source, sources.packages[4].Handle(), readOnly);
    REQUIRE(!readOnlyLeaf.IsEmpty());
    scene.files.Remove("Content/Leaf.rasset");
    REQUIRE(scene.files.AddFile("Content/Leaf.rasset", readOnlyLeaf.View()));
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        REQUIRE(putOpen({}, leaf));
        AssetImportResult result(env.allocator);
        REQUIRE(
            service.Import(scene.lock, "/Game/ReadOnlyUser", inputs, {}, {}, diagnostics, result));
        CHECK(hardOn("Content/ReadOnlyUser.rasset", leaf) && service.Packages().Count() == 0);
    }

    // 같은 id 파일 둘.
    REQUIRE(scene.files.AddFile("Content/Leaf2.rasset", scene.files.Bytes("Content/Leaf.rasset")));
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        CHECK(refuses(service, "/Game/Twice", false, ErrorCode::AlreadyExists,
                      AssetDiagnosticCode::DuplicateAssetId, leaf));
        CHECK(!scene.files.Has("Content/Twice.rasset"));
    }
}

// 묶음 임포트(7.1절 19 A2f3d UXR11): 새 구성원 둘이 서로를 Hard로 건다(MemberAsset·OpenAsset —
// 주 출력을 임포터 호출 전에 만든다), 재임포트 묶음(앞 구성원이 의존으로 연 다른 구성원은 Busy가
// 아니다), 순환 구성원 하나만 단일 재임포트(open=), 새 구성원과 재임포트 구성원 섞기. 거부(detail =
// 구성원 번호, 아무것도 쓰지 않는다): 구성원 0개·상한 초과·결과 수 다름·같은 경로 둘·재임포트
// 구성원의 importer·새 구성원의 replaceSources(InvalidArgument), 둘째 구성원 원본 없음
// (MissingSource — asset = 그 주 출력), 첫째 구성원의 추가 출력이 둘째 주 경로를 청함
// (DuplicatePath), 묶음 전에 열려 있던 구성원(Busy — detail·asset), MemberAsset 범위
// 밖(OpenAsset(null) InvalidArgument). 저장 실패: 새 파일 무리에서 실패하면 새 파일을 모두 지우고,
// 기존 파일을 교체한 뒤 실패하면 새 파일이 남고 교체 못 한 기존 파일은 옛 내용이며 다시 실행하면
// 같은 id로 수렴한다 (새 구성원이 섞였으면 주 출력까지 저장한 구성원의 결과가 남고 그 구성원을
// 재임포트로 넘긴다). 색인 FindByOwner는 참조 기록 항목만 준다.
TEST(AssetImport_Group) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-group");
    REQUIRE(scene.ready);
    auto registration =
        scene.hooks.Register(IdOf<TestAsset>(), AssetTypeHooks{&CountingTags, nullptr, nullptr});
    REQUIRE(registration);
    REQUIRE(scene.PutSource("SourceArt/A.trock", "value=1\nopenmember=1") &&
            scene.PutSource("SourceArt/B.trock", "value=2\nopenmember=0") &&
            scene.PutSource("SourceArt/C.trock", "value=3") &&
            scene.PutSource("SourceArt/P.trock", "value=4"));
    const String aFile = scene.Absolute(env.allocator, "SourceArt/A.trock");
    const String bFile = scene.Absolute(env.allocator, "SourceArt/B.trock");
    const String cFile = scene.Absolute(env.allocator, "SourceArt/C.trock");
    const String pFile = scene.Absolute(env.allocator, "SourceArt/P.trock");
    const String noFile = scene.Absolute(env.allocator, "SourceArt/None.trock");
    const AssetImportSourceInput aInputs[] = {{DefaultImportSourceRole, aFile.View()}};
    const AssetImportSourceInput bInputs[] = {{DefaultImportSourceRole, bFile.View()}};
    const AssetImportSourceInput cInputs[] = {{DefaultImportSourceRole, cFile.View()}};
    const AssetImportSourceInput pInputs[] = {{DefaultImportSourceRole, pFile.View()}};
    const AssetImportSourceInput noInputs[] = {{DefaultImportSourceRole, noFile.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const auto hardOn = [&](StringView file, AssetId target) noexcept {
        auto read = ReadFile(env, scene, file);
        if (!read) {
            return false;
        }

        bool hard = false;
        for (const PackageDependencyEntry& entry : read.Value().dependencies) {
            hard = hard || (entry.kind == PackageDependencyKind::Hard && entry.asset == target);
        }

        return hard;
    };
    const auto newMember = [](StringView path, Span<const AssetImportSourceInput> inputs) noexcept {
        AssetImportGroupMember member;
        member.path = path;
        member.sources = inputs;
        return member;
    };
    const auto reimportMember = [](StringView path) noexcept {
        AssetImportGroupMember member;
        member.path = path;
        member.reimport = true;
        return member;
    };
    AssetImportResult results[] = {AssetImportResult(env.allocator),
                                   AssetImportResult(env.allocator)};

    // 새 구성원 둘의 Hard 순환.
    const AssetImportGroupMember cycle[] = {newMember("/Game/A", aInputs),
                                            newMember("/Game/B", bInputs)};
    REQUIRE(service.ImportGroup(scene.lock, cycle, diagnostics, results));
    const AssetId a = results[0].id;
    const AssetId b = results[1].id;
    CHECK(!a.IsNull() && !b.IsNull() && results[0].path.View() == "/Game/A" &&
          results[1].path.View() == "/Game/B");
    CHECK(hardOn("Content/A.rasset", b) && hardOn("Content/B.rasset", a));
    CHECK(scene.test.lastGroupSize == 2 && scene.test.lastGroupIndex == 1 &&
          service.Packages().Count() == 0 && Matches(env, scene, service.Index()));

    // 재임포트 묶음: A를 열면 B가 의존으로 열린다(Busy가 아니다).
    const AssetImportGroupMember again[] = {reimportMember("/Game/A"), reimportMember("/Game/B")};
    REQUIRE(service.ImportGroup(scene.lock, again, diagnostics, results));
    CHECK(results[0].id == a && results[1].id == b && scene.test.lastReimport &&
          hardOn("Content/A.rasset", b) && hardOn("Content/B.rasset", a) &&
          service.Packages().Count() == 0);

    // 순환 구성원 하나만 단일 재임포트(상대는 색인으로 연다 — 이미 의존으로 열렸다).
    {
        StringBuilder<128> text(env.allocator);
        REQUIRE(text.TryAppendFormat("value=5\nopen={}", b));
        REQUIRE(scene.PutSource("SourceArt/A.trock", text.View()));
        AssetImportResult single(env.allocator);
        REQUIRE(service.Reimport(scene.lock, "/Game/A", {}, {}, diagnostics, single));
        CHECK(single.id == a && scene.test.lastGroupSize == 1 && scene.test.lastGroupIndex == 0 &&
              hardOn("Content/A.rasset", b));
    }

    // 새 구성원과 재임포트 구성원 섞기: A(재임포트) ↔ C(새).
    REQUIRE(scene.PutSource("SourceArt/A.trock", "value=6\nopenmember=1") &&
            scene.PutSource("SourceArt/C.trock", "value=7\nopenmember=0"));
    const AssetImportGroupMember mixed[] = {reimportMember("/Game/A"),
                                            newMember("/Game/C", cInputs)};
    REQUIRE(service.ImportGroup(scene.lock, mixed, diagnostics, results));
    const AssetId c = results[1].id;
    CHECK(results[0].id == a && !c.IsNull() && hardOn("Content/A.rasset", c) &&
          hardOn("Content/C.rasset", a) && Matches(env, scene, service.Index()));

    // 거부.
    AssetDiagnostic failure;
    const auto refuses = [&](Span<const AssetImportGroupMember> members, usize resultCount,
                             ErrorCode error, AssetDiagnosticCode code, uint64 detail) noexcept {
        failure = AssetDiagnostic{};
        results[0].id = AssetId{};
        const usize files = PackageFiles(scene);
        auto done =
            service.ImportGroup(scene.lock, members, diagnostics,
                                Span<AssetImportResult>(results, resultCount), nullptr, &failure);
        return !done && done.Error() == error && failure.code == code && failure.detail == detail &&
               PackageFiles(scene) == files && service.Packages().Count() == 0 &&
               results[0].id.IsNull();
    };
    CHECK(refuses({}, 0, ErrorCode::InvalidArgument, AssetDiagnosticCode::None, 0));
    CHECK(refuses(cycle, 1, ErrorCode::InvalidArgument, AssetDiagnosticCode::None, 0));
    {
        AssetImportGroupMember many[MaxImportGroupMembers + 1];
        Array<AssetImportResult> manyResults(env.allocator);
        for (AssetImportGroupMember& member : many) {
            member = newMember("/Game/Many", pInputs);
            REQUIRE(manyResults.TryPushBack(AssetImportResult(env.allocator)));
        }

        CHECK(service.ImportGroup(scene.lock, many, diagnostics, manyResults.View()).Error() ==
              ErrorCode::InvalidArgument);
    }

    REQUIRE(scene.PutSource("SourceArt/C.trock", "value=3"));
    const AssetImportGroupMember samePath[] = {newMember("/Game/N", cInputs),
                                               newMember("/Game/N", pInputs)};
    CHECK(refuses(samePath, 2, ErrorCode::InvalidArgument, AssetDiagnosticCode::None, 1));
    AssetImportGroupMember named = reimportMember("/Game/B");
    named.importer = "TestImporter";
    const AssetImportGroupMember withImporter[] = {named};
    CHECK(refuses(withImporter, 1, ErrorCode::InvalidArgument, AssetDiagnosticCode::None, 0));
    AssetImportGroupMember replacing = newMember("/Game/N", pInputs);
    replacing.replaceSources = true;
    const AssetImportGroupMember freshReplace[] = {replacing};
    CHECK(refuses(freshReplace, 1, ErrorCode::InvalidArgument, AssetDiagnosticCode::None, 0));
    const AssetImportGroupMember missing[] = {newMember("/Game/N1", pInputs),
                                              newMember("/Game/N2", noInputs)};
    CHECK(refuses(missing, 2, ErrorCode::NotFound, AssetDiagnosticCode::MissingSource, 1) &&
          !failure.asset.IsNull());
    REQUIRE(scene.PutSource("SourceArt/C.trock", "value=3\noutputs=x"));
    const AssetImportGroupMember taken[] = {newMember("/Game/D", cInputs),
                                            newMember("/Game/D_x", pInputs)};
    CHECK(refuses(taken, 2, ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicatePath, 0));

    // 저장 실패 1: 새 주 출력 E·F(Tags 1·2) — 둘째에서 실패하면 E 파일도 지운다.
    REQUIRE(scene.PutSource("SourceArt/A.trock", "value=1\nopenmember=1") &&
            scene.PutSource("SourceArt/B.trock", "value=1\nopenmember=0"));
    const AssetImportGroupMember fresh[] = {newMember("/Game/E", aInputs),
                                            newMember("/Game/F", bInputs)};
    tagCalls = 0;
    failTagsOn = 2;
    CHECK(refuses(fresh, 2, ErrorCode::Busy, AssetDiagnosticCode::None, 1));
    failTagsOn = 0;
    CHECK(!scene.files.Has("Content/E.rasset") && !scene.files.Has("Content/F.rasset") &&
          Matches(env, scene, service.Index()));

    // 저장 실패 2: A(재임포트 + 새 출력 fresh) ↔ C(재임포트). 순서 A_fresh(새) → A → C — C에서
    // 실패하면 A_fresh·A는 새 내용, C는 옛 내용이다. 다시 하면 A 기록(저장됨)에 있는 A_fresh를 같은
    // key로 다시 쓴다(기록 출력 재사용 — 고아 입양 경로는 AssetReimport_PartialFailure가 본다).
    REQUIRE(scene.PutSource("SourceArt/A.trock", "value=8\nopenmember=1\noutputs=fresh") &&
            scene.PutSource("SourceArt/C.trock", "value=9\nopenmember=0"));
    const auto copyOf = [&](StringView file) noexcept {
        Array<uint8> copy(env.allocator);
        CHECK(copy.TryAppend(scene.files.Bytes(file)));
        return copy;
    };
    const Array<uint8> aBefore = copyOf("Content/A.rasset");
    const Array<uint8> cBefore = copyOf("Content/C.rasset");
    const AssetImportGroupMember partial[] = {reimportMember("/Game/A"), reimportMember("/Game/C")};
    tagCalls = 0;
    failTagsOn = 3;
    failure = AssetDiagnostic{};
    auto failed = service.ImportGroup(scene.lock, partial, diagnostics, results, nullptr, &failure);
    failTagsOn = 0;
    CHECK(!failed && failed.Error() == ErrorCode::Busy && failure.detail == 1 &&
          service.Packages().Count() == 0 && scene.files.Has("Content/A_fresh.rasset"));
    CHECK(!SameBytes(scene.files.Bytes("Content/A.rasset"), aBefore.View()) &&
          SameBytes(scene.files.Bytes("Content/C.rasset"), cBefore.View()) &&
          Matches(env, scene, service.Index()));
    auto freshPath = ParseLogicalPath(env.allocator, "/Game/A_fresh");
    REQUIRE(freshPath);
    auto left = service.Index().FindByPath(freshPath.Value());
    REQUIRE(left);
    const AssetId leftId = left.Value()->id;
    // 다시 하면 남은 A_fresh를 같은 key로 다시 써서 수렴한다.
    REQUIRE(service.ImportGroup(scene.lock, partial, diagnostics, results));
    CHECK(results[0].outputs.Size() == 1 && results[0].outputs[0].id == leftId &&
          hardOn("Content/A.rasset", c) && hardOn("Content/C.rasset", a) &&
          Matches(env, scene, service.Index()));

    // 색인 owner 표: A의 참조 기록 항목은 A_fresh 하나(A 자신은 소유 기록), 없는 owner는 빈 목록.
    {
        Array<const AssetIndexEntry*> owned(env.allocator);
        REQUIRE(service.Index().FindByOwner(a, owned));
        CHECK(owned.Size() == 1 && owned[0]->id == leftId);
        owned.Clear();
        REQUIRE(service.Index().FindByOwner(FixedId(990002), owned));
        CHECK(owned.IsEmpty());
    }

    // 저장 실패 3(섞인 묶음): A·C(재임포트) + N(새). 순서 N(새 주) → A_fresh(기존 비주) → A → C —
    // C에서 실패하면 N·A의 결과는 남고(주 출력까지 저장) C의 결과는 빈다. N을 재임포트로 넘기면
    // 수렴한다.
    REQUIRE(scene.PutSource("SourceArt/A.trock", "value=10\nopenmember=1\noutputs=fresh") &&
            scene.PutSource("SourceArt/C.trock", "value=11\nopenmember=0") &&
            scene.PutSource("SourceArt/P.trock", "value=12"));
    const Array<uint8> cKept = copyOf("Content/C.rasset");
    AssetImportResult three[] = {AssetImportResult(env.allocator), AssetImportResult(env.allocator),
                                 AssetImportResult(env.allocator)};
    const AssetImportGroupMember withNew[] = {reimportMember("/Game/A"), reimportMember("/Game/C"),
                                              newMember("/Game/N9", pInputs)};
    tagCalls = 0;
    failTagsOn = 4;
    failure = AssetDiagnostic{};
    auto mixedFailed =
        service.ImportGroup(scene.lock, withNew, diagnostics, three, nullptr, &failure);
    failTagsOn = 0;
    CHECK(!mixedFailed && mixedFailed.Error() == ErrorCode::Busy && failure.detail == 1 &&
          service.Packages().Count() == 0 && scene.files.Has("Content/N9.rasset"));
    CHECK(three[0].id == a && three[1].id.IsNull() && three[1].path.IsEmpty() &&
          !three[2].id.IsNull() && three[2].path.View() == "/Game/N9" &&
          SameBytes(scene.files.Bytes("Content/C.rasset"), cKept.View()) &&
          Matches(env, scene, service.Index()));
    const AssetId n9 = three[2].id;
    // 그대로 다시 실행하면 N9가 새 임포트라 DuplicatePath다(호출자가 재임포트로 넘긴다).
    failure = AssetDiagnostic{};
    auto rerun = service.ImportGroup(scene.lock, withNew, diagnostics, three, nullptr, &failure);
    CHECK(!rerun && rerun.Error() == ErrorCode::AlreadyExists &&
          failure.code == AssetDiagnosticCode::DuplicatePath && failure.detail == 2 &&
          three[2].id.IsNull() && service.Packages().Count() == 0);
    const AssetImportGroupMember converged[] = {
        reimportMember("/Game/A"), reimportMember("/Game/C"), reimportMember("/Game/N9")};
    REQUIRE(service.ImportGroup(scene.lock, converged, diagnostics, three));
    CHECK(three[0].id == a && three[1].id == c && three[2].id == n9 &&
          Matches(env, scene, service.Index()));

    // 묶음 전에 열려 있던 구성원은 Busy다(detail = 그 번호, asset = 그 id). C를 열면 Hard 순환
    // 상대 A도 열리므로 첫 구성원 A에서 걸린다. 열린 것은 그대로다.
    REQUIRE(service.Packages().Open(c));
    const usize openBefore = service.Packages().Count();
    failure = AssetDiagnostic{};
    auto busy = service.ImportGroup(scene.lock, partial, diagnostics, results, nullptr, &failure);
    CHECK(!busy && busy.Error() == ErrorCode::Busy && failure.detail == 0 && failure.asset == a &&
          openBefore == 2 && service.Packages().Count() == openBefore);
    REQUIRE(service.Packages().CloseOpened({}));

    // MemberAsset 범위 밖은 null — OpenAsset(null)은 InvalidArgument다(단일 임포트는 구성원 하나).
    REQUIRE(scene.PutSource("SourceArt/P.trock", "value=1\nopenmember=5"));
    AssetImportResult outside(env.allocator);
    CHECK(service.Import(scene.lock, "/Game/Outside", pInputs, {}, {}, diagnostics, outside)
                  .Error() == ErrorCode::InvalidArgument &&
          scene.test.lastOpenError == ErrorCode::InvalidArgument &&
          !scene.files.Has("Content/Outside.rasset"));
}

// 가변 원본(7.1절 19 A2f3b UXR9): 고정 역할 asset과 접두 blob:의 역할 여럿을 받고 문맥 순서는 고정
// → 접두(byte 순)다. 기록 원본도 그 순서다. 재임포트는 기록 원본을 그대로 풀고, replaceSources는 준
// 목록으로 원본 전체를 바꾼다(역할 없는 원본 하나 = 유일한 고정 역할). 거부: 고정 역할 없음·접두
// 뒤가 빈 역할·모르는 역할·같은 역할 둘(InvalidImportOption), 빈 교체 목록. 등록 거부: 접두
// 문자·고정 역할이 접두로 시작·접두가 다른 접두의 앞부분.
TEST(AssetImport_VariableSources) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-variable");
    REQUIRE(scene.ready);
    static constexpr StringView AssetRoles[] = {"asset"};
    static constexpr StringView BlobPrefixes[] = {"blob:"};
    AssetImporterInfo info = TestImporterInfo();
    info.id = Guid{0x494d50540001ull, 11};
    info.name = "VariableImporter";
    info.extensions = {};
    info.optionsType = TypeId{};
    info.roles = AssetRoles;
    info.rolePrefixes = BlobPrefixes;
    TestImporter variable(info);
    auto registered = scene.importers.Register(variable);
    REQUIRE(registered);
    REQUIRE(scene.PutSource("SourceArt/Asset.trock", "value=1") &&
            scene.PutSource("SourceArt/B.trock", "value=2") &&
            scene.PutSource("SourceArt/A.trock", "value=4") &&
            scene.PutSource("SourceArt/C.trock", "value=8"));
    const String assetFile = scene.Absolute(env.allocator, "SourceArt/Asset.trock");
    const String bFile = scene.Absolute(env.allocator, "SourceArt/B.trock");
    const String aFile = scene.Absolute(env.allocator, "SourceArt/A.trock");
    const String cFile = scene.Absolute(env.allocator, "SourceArt/C.trock");
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    const AssetImportSourceInput inputs[] = {
        {"blob:bbb", bFile.View()}, {"asset", assetFile.View()}, {"blob:aaa", aFile.View()}};
    REQUIRE(service.Import(scene.lock, "/Game/Variable", inputs, "VariableImporter", {},
                           diagnostics, result));
    CHECK(variable.LastRoles() == "asset,blob:aaa,blob:bbb,");
    String text(env.allocator);
    REQUIRE(service.Info("/Game/Variable", AssetInfoOptions{}, text));
    TestAsset* imported = Representative(service.Packages(), result.id);
    REQUIRE(imported != nullptr);
    CHECK(imported->value == 7);
    REQUIRE(service.Packages().Close(result.id));
    auto read = ReadFile(env, scene, "Content/Variable.rasset");
    REQUIRE(read);
    auto record = ReadImportRecord(env.allocator, result.id, read.Value().importRecord);
    REQUIRE(record);
    REQUIRE(record.Value().sources.Size() == 3);
    CHECK(record.Value().sources[0].role.View() == "asset" &&
          record.Value().sources[1].role.View() == "blob:aaa" &&
          record.Value().sources[2].role.View() == "blob:bbb");

    // 재임포트(기록 원본 그대로) — 같은 순서.
    AssetImportResult again(env.allocator);
    REQUIRE(service.Reimport(scene.lock, "/Game/Variable", {}, {}, diagnostics, again));
    CHECK(variable.LastRoles() == "asset,blob:aaa,blob:bbb,");

    // 전체 교체: asset + blob:ccc(blob 둘은 사라진다).
    const AssetImportSourceInput replaced[] = {{"blob:ccc", cFile.View()},
                                               {"asset", assetFile.View()}};
    REQUIRE(service.Reimport(scene.lock, "/Game/Variable", replaced, {}, diagnostics, again,
                             nullptr, nullptr, true));
    CHECK(variable.LastRoles() == "asset,blob:ccc,");
    auto reread = ReadFile(env, scene, "Content/Variable.rasset");
    REQUIRE(reread);
    auto rerecord = ReadImportRecord(env.allocator, result.id, reread.Value().importRecord);
    REQUIRE(rerecord);
    CHECK(rerecord.Value().sources.Size() == 2);

    // 역할 없는 원본 하나로 교체하면 유일한 고정 역할(asset)이다.
    const AssetImportSourceInput onlyAsset[] = {{{}, assetFile.View()}};
    REQUIRE(service.Reimport(scene.lock, "/Game/Variable", onlyAsset, {}, diagnostics, again,
                             nullptr, nullptr, true));
    CHECK(variable.LastRoles() == "asset,");

    // 거부.
    const auto refuses = [&](Span<const AssetImportSourceInput> list) noexcept {
        AssetImportResult refused(env.allocator);
        AssetDiagnostic failure;
        auto imported = service.Import(scene.lock, "/Game/Refused", list, "VariableImporter", {},
                                       diagnostics, refused, nullptr, &failure);
        return !imported && imported.Error() == ErrorCode::InvalidArgument &&
               failure.code == AssetDiagnosticCode::InvalidImportOption;
    };
    const AssetImportSourceInput noFixed[] = {{"blob:aaa", aFile.View()}};
    CHECK(refuses(noFixed));
    const AssetImportSourceInput emptyRest[] = {{"asset", assetFile.View()},
                                                {"blob:", aFile.View()}};
    CHECK(refuses(emptyRest));
    const AssetImportSourceInput unknown[] = {{"asset", assetFile.View()}, {"other", aFile.View()}};
    CHECK(refuses(unknown));
    const AssetImportSourceInput twice[] = {
        {"asset", assetFile.View()}, {"blob:aaa", aFile.View()}, {"blob:aaa", bFile.View()}};
    CHECK(refuses(twice));
    const AssetImportSourceInput fixedTwice[] = {{"asset", assetFile.View()},
                                                 {"asset", aFile.View()}};
    CHECK(refuses(fixedTwice));
    AssetDiagnostic emptyFailure;
    CHECK(service
              .Reimport(scene.lock, "/Game/Variable", {}, {}, diagnostics, again, nullptr,
                        &emptyFailure, true)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(emptyFailure.code == AssetDiagnosticCode::InvalidImportOption);

    // 등록 거부.
    const auto rejects = [&](Span<const StringView> roles, Span<const StringView> prefixes,
                             uint64 low) noexcept {
        AssetImporterInfo bad = info;
        bad.id = Guid{0x494d50540001ull, low};
        bad.name = "BadVariable";
        bad.roles = roles;
        bad.rolePrefixes = prefixes;
        TestImporter importer(bad);
        return scene.importers.Register(importer).Error() == ErrorCode::InvalidArgument;
    };
    static constexpr StringView BadChar[] = {"blob/"};
    CHECK(rejects(AssetRoles, BadChar, 21));
    static constexpr StringView FixedBlob[] = {"blob:x"};
    CHECK(rejects(FixedBlob, BlobPrefixes, 22));
    static constexpr StringView Overlap[] = {"b", "blob:"};
    CHECK(rejects(AssetRoles, Overlap, 23));
}

// 가변만 받는 임포터(Docs/Design/AssetSystem 7.1절 18 개정·12.12절 A5h): 고정 역할 없이 `slice:`
// 역할 여럿(문맥은 byte 순), 원본 0개는 InvalidImportOption(detail 0), 기록 그대로 재임포트, 역할
// 없는 원본 하나(`--source=`)는 바꾸기·전체 교체 모두 InvalidImportOption.
TEST(AssetImport_VariableSourcesOnly) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-variable-only");
    REQUIRE(scene.ready);
    static constexpr StringView SlicePrefixes[] = {"slice:"};
    AssetImporterInfo info = TestImporterInfo();
    info.id = Guid{0x494d50540001ull, 31};
    info.name = "SliceImporter";
    info.extensions = {};
    info.optionsType = TypeId{};
    info.rolePrefixes = SlicePrefixes;
    info.variableSourcesOnly = true;
    TestImporter slices(info);
    auto registered = scene.importers.Register(slices);
    REQUIRE(registered);
    REQUIRE(scene.PutSource("SourceArt/S0.trock", "value=4") &&
            scene.PutSource("SourceArt/S1.trock", "value=2"));
    const String first = scene.Absolute(env.allocator, "SourceArt/S0.trock");
    const String second = scene.Absolute(env.allocator, "SourceArt/S1.trock");
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    const AssetImportSourceInput inputs[] = {{"slice:1", second.View()}, {"slice:0", first.View()}};
    REQUIRE(service.Import(scene.lock, "/Game/Slices", inputs, "SliceImporter", {}, diagnostics,
                           result));
    CHECK(slices.LastRoles() == "slice:0,slice:1,");
    // 임포트는 연 패키지를 닫으므로 Info로 다시 연다.
    String text(env.allocator);
    REQUIRE(service.Info("/Game/Slices", AssetInfoOptions{}, text));
    TestAsset* imported = Representative(service.Packages(), result.id);
    REQUIRE(imported != nullptr);
    CHECK(imported->value == 6);
    REQUIRE(service.Packages().Close(result.id));

    AssetImportResult empty(env.allocator);
    AssetDiagnostic emptyFailure;
    CHECK(service
              .Import(scene.lock, "/Game/Empty", {}, "SliceImporter", {}, diagnostics, empty,
                      nullptr, &emptyFailure)
              .Error() == ErrorCode::InvalidArgument);
    // 원본 0개는 역할 규칙 전에 거부된다(AssetService::Import — 원본 없음 InvalidArgument, 진단
    // None).
    CHECK(emptyFailure.code == AssetDiagnosticCode::None &&
          emptyFailure.error == ErrorCode::InvalidArgument);

    AssetImportResult again(env.allocator);
    REQUIRE(service.Reimport(scene.lock, "/Game/Slices", {}, {}, diagnostics, again));
    CHECK(slices.LastRoles() == "slice:0,slice:1,");
    const AssetImportSourceInput single[] = {{{}, first.View()}};
    for (bool replace : {false, true}) {
        AssetDiagnostic failure;
        CHECK(service
                  .Reimport(scene.lock, "/Game/Slices", single, {}, diagnostics, again, nullptr,
                            &failure, replace)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(failure.code == AssetDiagnosticCode::InvalidImportOption);
    }
}

namespace {

// 설정 정규화 hook 시험 상태다(12.12절 — 서비스가 대입 뒤·저장 전에 부른다).
usize normalizeCalls = 0;
bool normalizeFails = false;
char normalizeFields[64] = {};
usize normalizeFieldBytes = 0;

// 대입 필드 이름을 `a,b,` 꼴로 남기고 value를 1000 더한다(저장 전에 적용됐는지 본다).
Result<void> CountingNormalize(const AssetTypeHookContext& context,
                               Span<const StringView> fields) noexcept {
    ++normalizeCalls;
    normalizeFieldBytes = 0;
    for (StringView field : fields) {
        for (usize at = 0;
             at < field.ByteCount() && normalizeFieldBytes + 1 < sizeof(normalizeFields); ++at) {
            normalizeFields[normalizeFieldBytes++] = field.Data()[at];
        }

        if (normalizeFieldBytes + 1 < sizeof(normalizeFields)) {
            normalizeFields[normalizeFieldBytes++] = ',';
        }
    }

    if (normalizeFails) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    auto borrowed = context.registry.ResolveForWrite<TestAsset>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    value.Value()->value += 1000;
    return Result<void>::Success();
}

StringView NormalizedFields() noexcept {
    return StringView(normalizeFields, normalizeFieldBytes);
}

} // namespace

// 설정 정규화 hook(12.12절): 새 임포트는 임포터·설정 대입 뒤 저장 전에 한 번(옵션 대입은 넘기지
// 않는다 — 설정 최상위 필드만), 재임포트도 한 번(대입 없으면 빈 목록), asset-set은
// 대입·되돌린 필드로 한 번. hook 실패는 임포트 실패(저장 없음)·asset-set 실패다.
TEST(AssetImport_NormalizeSettingsHook) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-normalize");
    REQUIRE(scene.ready);
    AssetTypeHooks hooks;
    hooks.normalizeSettings = &CountingNormalize;
    auto hook = scene.hooks.Register(IdOf<TestAsset>(), hooks);
    REQUIRE(hook);
    REQUIRE(scene.PutSource(RockSource, "value=5"));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    Assignments assignments(env.allocator);
    assignments.Add("scale=2").Add("note=First").Add("label=L");
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    normalizeCalls = 0;
    REQUIRE(service.Import(scene.lock, "/Game/Rock", inputs, {}, assignments.list.View(),
                           diagnostics, result));
    CHECK(normalizeCalls == 1 && NormalizedFields() == "note,label,");
    // 임포트는 연 패키지를 닫으므로 Info로 다시 연다.
    String text(env.allocator);
    REQUIRE(service.Info("/Game/Rock", AssetInfoOptions{}, text));
    TestAsset* imported = Representative(service.Packages(), result.id);
    REQUIRE(imported != nullptr);
    CHECK(imported->value == 1010);
    REQUIRE(service.Packages().Close(result.id));
    auto read = ReadFile(env, scene, "Content/Rock.rasset");
    REQUIRE(read);

    AssetImportResult again(env.allocator);
    REQUIRE(service.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, again));
    CHECK(normalizeCalls == 2 && NormalizedFields().IsEmpty());

    Assignments edits(env.allocator);
    edits.Add("label=Edited");
    REQUIRE(service.Set(scene.lock, "/Game/Rock", edits.list.View(), false, diagnostics, nullptr,
                        nullptr));
    CHECK(normalizeCalls == 3 && NormalizedFields() == "label,");

    normalizeFails = true;
    AssetImportResult refused(env.allocator);
    CHECK(
        service.Import(scene.lock, "/Game/Refused", inputs, {}, {}, diagnostics, refused).Error() ==
        ErrorCode::Busy);
    CHECK(!scene.files.Has("Content/Refused.rasset"));
    CHECK(
        service
            .Set(scene.lock, "/Game/Rock", edits.list.View(), false, diagnostics, nullptr, nullptr)
            .Error() == ErrorCode::Busy);
    normalizeFails = false;
}

namespace {

// 원본 재구성 hook 시험 상태다(12.14절 A5j2 — 서비스가 정규화 앞에서 부른다).
struct RebuildRecord {
    usize calls = 0;
    AssetSourceRebuildCause cause = AssetSourceRebuildCause::Edit;
    AssetId changed{};
    bool importRecord = false;
    char fields[64] = {};
    usize fieldBytes = 0;
    bool fails = false;
    // 원본을 다시 썼다고 알릴지다(false면 묶음과 무관한 참조자처럼 아무것도 바꾸지 않는다).
    bool mark = true;

    StringView Fields() const noexcept {
        return StringView(fields, fieldBytes);
    }
};

RebuildRecord rebuild;

// 부른 까닭·바뀐 원소(대표 에셋 id)·필드를 남기고 value를 1 더한다. fails면
// sink.Fail(InvalidSetting).
Result<void> RecordingRebuild(const AssetTypeHookContext& context, Span<const StringView> fields,
                              AssetSourceRebuildSink& sink) noexcept {
    ++rebuild.calls;
    rebuild.cause = sink.Cause();
    rebuild.importRecord = sink.HasImportRecord();
    rebuild.changed = AssetId{};
    if (!sink.ChangedSource().IsNull()) {
        auto borrowed = context.registry.Resolve<Asset>(context.access, sink.ChangedSource());
        if (borrowed) {
            auto value = borrowed.Value().Get();
            rebuild.changed = value ? value.Value()->Id() : AssetId{};
        }
    }

    rebuild.fieldBytes = 0;
    for (StringView field : fields) {
        for (usize at = 0;
             at < field.ByteCount() && rebuild.fieldBytes + 1 < sizeof(rebuild.fields); ++at) {
            rebuild.fields[rebuild.fieldBytes++] = field.Data()[at];
        }

        if (rebuild.fieldBytes + 1 < sizeof(rebuild.fields)) {
            rebuild.fields[rebuild.fieldBytes++] = ',';
        }
    }

    if (rebuild.fails) {
        return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument, 7);
    }

    if (!rebuild.mark) {
        return Result<void>::Success();
    }

    sink.MarkRebuilt();

    auto borrowed = context.registry.ResolveForWrite<TestAsset>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    value.Value()->value += 1;
    return Result<void>::Success();
}

} // namespace

// 원본 재구성 hook(12.14절 A5j2): 임포트는 부르지 않음, asset-new는 New 까닭·대입 필드로 부르고
// 실패하면 패키지를 만들지 않음(같은 경로 New가 다시 됨), asset-set은 Edit 까닭으로 정규화 앞에서
// 부르고 실패 진단(코드·detail·asset)을 그대로 남기며 저장하지 않음, 원소 재임포트는 EditorOnly로
// 참조하는 에셋을 Propagate 까닭·바뀐 원소로 불러 함께 저장하고, 그 hook이 실패하면 재임포트가
// 실패해 원소도 저장하지 않는다. ImportRecord 유무를 sink가 알려 준다.
TEST(AssetImport_RebuildSourceHook) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-rebuild");
    REQUIRE(scene.ready);
    AssetTypeHooks hooks;
    hooks.rebuildSource = &RecordingRebuild;
    auto hook = scene.hooks.Register(IdOf<TestAsset>(), hooks);
    REQUIRE(hook);
    REQUIRE(scene.PutSource(RockSource, "value=5"));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult rock(env.allocator);
    rebuild = RebuildRecord{};
    REQUIRE(service.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, rock));
    // 임포트는 연 패키지를 닫는다.
    CHECK(rebuild.calls == 0 && service.Packages().Find(rock.id) == nullptr);

    // asset-new: New 까닭, 대입 필드, 저장 전 적용(value 0 + 1).
    Assignments bundle(env.allocator);
    bundle.Add("editorRef=/Game/Rock").Add("label=B");
    auto made =
        service.New(scene.lock, "TestAsset", "/Game/Bundle", bundle.list.View(), diagnostics);
    REQUIRE(made);
    CHECK(rebuild.calls == 1 && rebuild.cause == AssetSourceRebuildCause::New &&
          rebuild.Fields() == "editorRef,label," && rebuild.changed.IsNull() &&
          !rebuild.importRecord);
    TestAsset* created = Representative(service.Packages(), made.Value());
    REQUIRE(created != nullptr);
    CHECK(created->value == 1);
    REQUIRE(service.Packages().Close(made.Value()));

    // asset-new 실패: 진단 그대로, 파일 없음, 같은 경로의 다음 New가 된다.
    rebuild.fails = true;
    AssetDiagnostic failure;
    auto refused = service.New(scene.lock, "TestAsset", "/Game/Refused", bundle.list.View(),
                               diagnostics, nullptr, &failure);
    CHECK(!refused && refused.Error() == ErrorCode::InvalidArgument &&
          failure.code == AssetDiagnosticCode::InvalidSetting && failure.detail == 7);
    CHECK(!scene.files.Has("Content/Refused.rasset"));
    rebuild.fails = false;
    auto retried = service.New(scene.lock, "TestAsset", "/Game/Refused", {}, diagnostics);
    CHECK(retried && rebuild.cause == AssetSourceRebuildCause::New && rebuild.Fields().IsEmpty());
    if (retried) {
        REQUIRE(service.Packages().Close(retried.Value()));
    }

    // asset-set: Edit 까닭, 실패는 진단 그대로·저장하지 않음.
    Assignments edits(env.allocator);
    edits.Add("label=C");
    REQUIRE(service.Set(scene.lock, "/Game/Bundle", edits.list.View(), false, diagnostics, nullptr,
                        nullptr));
    CHECK(rebuild.cause == AssetSourceRebuildCause::Edit && rebuild.Fields() == "label,");
    REQUIRE(service.Packages().Close(made.Value()));
    Array<uint8> before(env.allocator);
    REQUIRE(before.TryAppend(scene.files.Bytes("Content/Bundle.rasset")) && !before.IsEmpty());
    rebuild.fails = true;
    AssetDiagnostic setFailure;
    CHECK(service
              .Set(scene.lock, "/Game/Bundle", edits.list.View(), false, diagnostics, nullptr,
                   &setFailure)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(setFailure.code == AssetDiagnosticCode::InvalidSetting && setFailure.detail == 7 &&
          setFailure.asset == made.Value());
    rebuild.fails = false;
    REQUIRE(service.Packages().Close(made.Value()));
    CHECK(SameBytes(scene.files.Bytes("Content/Bundle.rasset"), before.View()));

    // 원소 재임포트 전파: Bundle(editorRef → Rock)을 Propagate·바뀐 원소 Rock으로 부르고 저장한다.
    const usize calls = rebuild.calls;
    AssetImportResult again(env.allocator);
    // Bundle을 열 때 의존으로 함께 연 Rock도 닫는다(재임포트는 열린 출력이 Busy — AssetService).
    REQUIRE(service.Packages().Close(rock.id));
    REQUIRE(service.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, again));
    CHECK(rebuild.calls == calls + 1 && rebuild.cause == AssetSourceRebuildCause::Propagate &&
          rebuild.changed == rock.id && rebuild.Fields().IsEmpty());
    CHECK(!SameBytes(scene.files.Bytes("Content/Bundle.rasset"), before.View()));

    // 원본을 다시 쓰지 않은 참조자(묶음과 무관 — compositeTexture처럼)는 저장하지 않는다.
    {
        Array<uint8> untouched(env.allocator);
        REQUIRE(untouched.TryAppend(scene.files.Bytes("Content/Bundle.rasset")));
        rebuild.mark = false;
        AssetImportResult quiet(env.allocator);
        REQUIRE(service.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, quiet));
        CHECK(rebuild.cause == AssetSourceRebuildCause::Propagate &&
              SameBytes(scene.files.Bytes("Content/Bundle.rasset"), untouched.View()));
        rebuild.mark = true;
    }

    // 일 전부터 열려 있고 저장하지 않은 편집이 있는 참조자를 두면 Busy다(아무것도 저장하지 않는다).
    // 참조자를 열면 Hard 의존(editorRef)인 Rock도 함께 열리므로, 먼저 걸리는 것은 "출력이 열려
    // 있음" Busy(asset = Rock)다.
    {
        Array<uint8> rockBytes(env.allocator);
        REQUIRE(rockBytes.TryAppend(scene.files.Bytes("Content/Rock.rasset")));
        REQUIRE(service.Packages().Open(made.Value()));
        REQUIRE(service.Packages().MarkDirty(made.Value()));
        REQUIRE(scene.PutSource(RockSource, "value=11"));
        AssetImportResult busy(env.allocator);
        AssetDiagnostic busyFailure;
        CHECK(service
                  .Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, busy, nullptr,
                            &busyFailure)
                  .Error() == ErrorCode::Busy);
        CHECK(busyFailure.asset == rock.id &&
              SameBytes(scene.files.Bytes("Content/Rock.rasset"), rockBytes.View()));
        REQUIRE(service.Packages().Close(made.Value()));
        REQUIRE(service.Packages().Close(rock.id));
    }

    // 전파 hook 실패: 재임포트 실패, 원소·묶음 모두 저장하지 않는다.
    Array<uint8> rockBefore(env.allocator);
    REQUIRE(rockBefore.TryAppend(scene.files.Bytes("Content/Rock.rasset")));
    Array<uint8> bundleBefore(env.allocator);
    REQUIRE(bundleBefore.TryAppend(scene.files.Bytes("Content/Bundle.rasset")));
    REQUIRE(scene.PutSource(RockSource, "value=9"));
    rebuild.fails = true;
    AssetDiagnostic reimportFailure;
    CHECK(service
              .Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, again, nullptr,
                        &reimportFailure)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(reimportFailure.code == AssetDiagnosticCode::InvalidSetting);
    rebuild.fails = false;
    CHECK(SameBytes(scene.files.Bytes("Content/Rock.rasset"), rockBefore.View()) &&
          SameBytes(scene.files.Bytes("Content/Bundle.rasset"), bundleBefore.View()));
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
        scene.hooks.Register(IdOf<TestAsset>(), AssetTypeHooks{&CountingTags, nullptr, nullptr});
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

// 묶음 할당 실패(7.1절 19 A2f3d): 새 구성원 둘의 Hard 순환(추가 출력·경고 포함) 묶음의 모든 할당
// 실패가 OutOfMemory이고 파일·색인·열린 패키지가 그대로다. 재임포트 묶음(A ↔ B)의 할당 실패도
// OutOfMemory이고 열린 패키지가 그대로다(기존 파일을 교체한 뒤의 실패는 그 파일이 새 내용일 수
// 있다 — 색인은 디스크와 같다). GC 표 실패·성공 뒤 닫기 실패는 삼킨 성공이다.
TEST(AssetImport_GroupAllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-group-oom");
    REQUIRE(scene.ready);
    StringBuilder<128> text(env.allocator);
    REQUIRE(text.TryAppendFormat("value=2\nopenmember=1\noutputs=extra\nwarn={}",
                                 static_cast<uint32>(AssetDiagnosticCode::UvOutOfRange)));
    REQUIRE(scene.PutSource("SourceArt/A.trock", text.View()) &&
            scene.PutSource("SourceArt/B.trock", "value=3\nopenmember=0"));
    const String aFile = scene.Absolute(env.allocator, "SourceArt/A.trock");
    const String bFile = scene.Absolute(env.allocator, "SourceArt/B.trock");
    const AssetImportSourceInput aInputs[] = {{DefaultImportSourceRole, aFile.View()}};
    const AssetImportSourceInput bInputs[] = {{DefaultImportSourceRole, bFile.View()}};
    AssetImportGroupMember fresh[2];
    fresh[0].path = "/Game/A";
    fresh[0].sources = aInputs;
    fresh[1].path = "/Game/B";
    fresh[1].sources = bInputs;
    TaggedFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetService service(failing, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        // 열린 패키지가 있으면 열린 id 기록이 할당한다.
        String out(env.allocator);
        REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
        const usize open = service.Packages().Count();
        const usize packages = PackageFiles(scene);
        TextDiagnostics diagnostics(env.allocator);
        AssetImportResult results[] = {AssetImportResult(failing), AssetImportResult(failing)};
        Array<EditorWarning> warnings(failing);
        failing.FailOnAttempt(attempt);
        auto imported = service.ImportGroup(scene.lock, fresh, diagnostics, results, &warnings);
        const bool failed = failing.Failed();
        const uint32 tag = failing.FailedTag();
        failing.DisableFailure();
        if (!failed || imported) {
            REQUIRE(imported);
            CHECK(scene.files.Has("Content/A.rasset") && scene.files.Has("Content/B.rasset") &&
                  scene.files.Has("Content/A_extra.rasset") &&
                  Matches(env, scene, service.Index()));
            if (!failed) {
                CHECK(service.Packages().Count() == open);
                break;
            }

            CHECK(tag == GcReferenceTableTag || service.Packages().Count() > open);
            scene.files.Remove("Content/A.rasset");
            scene.files.Remove("Content/B.rasset");
            scene.files.Remove("Content/A_extra.rasset");
            continue;
        }

        CHECK(!imported && imported.Error() == ErrorCode::OutOfMemory);
        CHECK(PackageFiles(scene) == packages && service.Packages().Count() == open &&
              Matches(env, scene, service.Index()));
        ++failures;
    }

    CHECK(failures > 0);

    // 재임포트 묶음(위 마지막 성공이 남긴 A·B·A_extra).
    AssetImportGroupMember again[2];
    again[0].path = "/Game/A";
    again[0].reimport = true;
    again[1].path = "/Game/B";
    again[1].reimport = true;
    uint32 reimportFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetService service(failing, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        const usize open = service.Packages().Count();
        TextDiagnostics diagnostics(env.allocator);
        AssetImportResult results[] = {AssetImportResult(failing), AssetImportResult(failing)};
        failing.FailOnAttempt(attempt);
        auto reimported = service.ImportGroup(scene.lock, again, diagnostics, results);
        const bool failed = failing.Failed();
        const uint32 tag = failing.FailedTag();
        failing.DisableFailure();
        if (!failed || reimported) {
            REQUIRE(reimported);
            CHECK(Matches(env, scene, service.Index()));
            if (!failed) {
                CHECK(service.Packages().Count() == open);
                break;
            }

            CHECK(tag == GcReferenceTableTag || service.Packages().Count() > open);
            continue;
        }

        CHECK(!reimported && reimported.Error() == ErrorCode::OutOfMemory);
        CHECK(service.Packages().Count() == open && Matches(env, scene, service.Index()));
        ++reimportFailures;
    }

    CHECK(reimportFailures > 0);
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
        if (!failed || imported) {
            REQUIRE(imported);
            CHECK(scene.files.Has("Content/Rock.rasset") &&
                  scene.files.Has("Content/Rock_extra.rasset") &&
                  Matches(env, scene, service.Index()));
            if (!failed) {
                CHECK(service.Packages().Count() == open);
                break;
            }

            // 삼킨 실패: GC 표, 또는 성공 뒤 닫기(이번에 연 패키지가 열린 채 남는다).
            CHECK(tag == GcReferenceTableTag || service.Packages().Count() > open);

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

namespace {

// 기록을 직접 짠 재임포트 장면이다(고정 id): 주 출력 Leaf(TestAsset)의 소유 기록(원본 하나 — 루트·
// 상대 경로·역할, 출력 Leaf·Villain(key extra)), Villain의 참조 기록(owner Leaf·ImportOwner).
// Villain은 Hero(Hard)에, Hero는 Base·Derived에 기댄다. tweak가 기록 입력을 바꾼다.
struct CraftedRecord {
    StringView root = "Project";
    StringView relative = RockSource;
    StringView role = DefaultImportSourceRole;
    Span<const uint8> options{};
    uint32 importerVersion = TestImporterVersion;
    uint64 villainOwner = LeafId;
    bool villainReadOnly = false;
    bool leafReadOnly = false;
    Guid importerId = TestImporterId;
    // 참이면 원본 둘(px·nx — CubeImporter 역할).
    bool twoRoles = false;
    // 기록 출력 타입(실제 대표 타입은 TestAsset)·참이면 출력은 주 출력만.
    TypeId leafType = IdOf<TestAsset>();
    TypeId villainType = IdOf<TestAsset>();
    bool mainOnly = false;
    // 비어 있지 않으면 Leaf의 기록 byte를 이것으로 바꾼다(읽지 못하는 기록).
    Span<const uint8> rawLeafRecord{};
};

bool PutCrafted(AssetEnv& env, Sources& sources, ImportScene& scene,
                const CraftedRecord& crafted) noexcept {
    const ImportSourceInput oneSource[] = {
        {crafted.role, crafted.root, crafted.relative, Sha256Digest{}, 0}};
    const ImportSourceInput twoSources[] = {
        {"px", "Project", "SourceArt/px.png", Sha256Digest{}, 0},
        {"nx", "Project", "SourceArt/nx.png", Sha256Digest{}, 0}};
    const Span<const ImportSourceInput> recordSources =
        crafted.twoRoles ? Span<const ImportSourceInput>(twoSources)
                         : Span<const ImportSourceInput>(oneSource);
    const ImportOutputInput outputs[] = {{"", FixedId(LeafId), crafted.leafType},
                                         {"extra", FixedId(VillainId), crafted.villainType}};
    ImportRecordInput owned;
    owned.owner = FixedId(LeafId);
    owned.importerId = crafted.importerId;
    owned.importerName = "TestImporter";
    owned.importerVersion = crafted.importerVersion;
    owned.sources = recordSources;
    owned.options = crafted.options;
    owned.outputs = crafted.mainOnly ? Span<const ImportOutputInput>(outputs).Left(1)
                                     : Span<const ImportOutputInput>(outputs);
    auto ownedBytes = WriteImportRecord(env.allocator, FixedId(LeafId), owned);
    ImportRecordInput reference;
    reference.owner = FixedId(crafted.villainOwner);
    auto referenceBytes = WriteImportRecord(env.allocator, FixedId(VillainId), reference);
    if (!ownedBytes || !referenceBytes) {
        return false;
    }

    PackageWriteOptions leafOptions = EditorOptions();
    leafOptions.importRecord =
        crafted.rawLeafRecord.IsEmpty() ? ownedBytes.Value().View() : crafted.rawLeafRecord;
    if (crafted.leafReadOnly) {
        leafOptions.customVersions = UnknownCustomVersions;
    }
    const PackageDependencyEntry owner[] = {
        {FixedId(crafted.villainOwner), PackageDependencyKind::ImportOwner, TypeId{}, false}};
    PackageWriteOptions villainOptions = EditorOptions();
    villainOptions.importRecord = referenceBytes.Value().View();
    villainOptions.extraDependencies = owner;
    if (crafted.villainReadOnly) {
        villainOptions.customVersions = UnknownCustomVersions;
    }

    const Array<uint8> leaf =
        Write(env.allocator, *env.source, sources.packages[4].Handle(), leafOptions);
    const Array<uint8> villain =
        Write(env.allocator, *env.source, sources.packages[3].Handle(), villainOptions);
    scene.files.Remove("Content/Leaf.rasset");
    scene.files.Remove("Content/Villain.rasset");
    return !leaf.IsEmpty() && !villain.IsEmpty() &&
           scene.files.AddFile("Content/Leaf.rasset", leaf.View()) &&
           scene.files.AddFile("Content/Villain.rasset", villain.View()) &&
           (scene.files.Has("Content/Base.rasset") ||
            (scene.files.AddFile("Content/Base.rasset", sources.base.View()) &&
             scene.files.AddFile("Content/Derived.rasset", sources.derived.View()) &&
             scene.files.AddDirectory("Content/Dir") &&
             scene.files.AddFile("Content/Dir/Hero.rasset", sources.hero.View())));
}

} // namespace

// 재임포트: 같은 id 제자리 갱신(값은 새 원본 × 기록 옵션, 설정 note 유지, bulk 새로), 기존 출력은
// 같은 id(이동해도 — 색인 경로), 새 key는 새 출력, 다른 패키지의 참조가 그대로 풀림, 기록 원본
// SHA-256·byte 수 새로, 임포터 문맥 IsReimport, 원본 바꾸기(`--source=`)·옵션 명시 대입, 청하지
// 않은 기존 출력은 OrphanedOutput(asset·related)·고아, 지워진 기록 출력은 새 id 새 출력, 경고 목록
// 없이도.
TEST(AssetReimport_InPlace) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "reimport-inplace");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource(RockSource, "value=2\nbulk=01\noutputs=extra\nreference=extra"));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult imported(env.allocator);
    Assignments first(env.allocator);
    first.Add("scale=3").Add("note=Kept");
    REQUIRE(service.Import(scene.lock, "/Game/Rock", inputs, {}, first.list.View(), diagnostics,
                           imported));
    REQUIRE(imported.outputs.Size() == 1);
    const AssetId extraId = imported.outputs[0].id;
    // 다른 패키지가 주 출력을 참조한다(재임포트 뒤에도 같은 id로 풀린다).
    Assignments user(env.allocator);
    user.Add("hard=/Game/Rock");
    auto userId = service.New(scene.lock, "TestAsset", "/Game/User", user.list.View(), diagnostics);
    REQUIRE(userId);
    // 추가 출력을 옮긴다(기록은 id로 찾는다).
    AssetMoveResult moved;
    REQUIRE(service.Move(scene.lock, "/Game/Rock_extra", "/Game/Moved/Extra", moved));
    // 새 서비스(열린 패키지 없음)로 재임포트: 원본이 바뀌었다.
    constexpr StringView Changed = "value=5\nbulk=0203\noutputs=extra,fresh\nreference=extra";
    REQUIRE(scene.PutSource(RockSource, Changed));
    REQUIRE(service.FlushCache(scene.lock));
    AssetService again(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(again.Initialize());
    AssetImportResult result(env.allocator);
    Array<EditorWarning> warnings(env.allocator);
    const uint32 calls = scene.test.calls;
    REQUIRE(again.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, result, &warnings));
    CHECK(result.id == imported.id && result.path.View() == "/Game/Rock" && warnings.IsEmpty() &&
          scene.test.calls == calls + 1 && scene.test.lastReimport);
    REQUIRE(result.outputs.Size() == 2);
    CHECK(result.outputs[0].key.View() == "extra" && result.outputs[0].id == extraId &&
          result.outputs[0].path.View() == "/Game/Moved/Extra" &&
          result.outputs[1].key.View() == "fresh" && !(result.outputs[1].id == extraId) &&
          result.outputs[1].path.View() == "/Game/Rock_fresh");
    String out(env.allocator);
    REQUIRE(again.Info("/Game/Rock", AssetInfoOptions{}, out));
    TestAsset* asset = Representative(again.Packages(), imported.id);
    REQUIRE(asset != nullptr);
    CHECK(asset->value == 15 && asset->note.View() == "Kept");
    auto pixels = again.Packages().Find(imported.id)->store->Read(asset->pixels);
    CHECK(pixels && pixels.Value().Size() == 2 && pixels.Value()[1] == 0x03);
    auto read = ReadFile(env, scene, "Content/Rock.rasset");
    REQUIRE(read);
    auto record = ReadImportRecord(env.allocator, imported.id, read.Value().importRecord);
    REQUIRE(record);
    CHECK(record.Value().sources[0].sha256 == Sha256::Hash(Bytes(Changed)) &&
          record.Value().sources[0].bytes == Changed.ByteCount() &&
          record.Value().outputs.Size() == 3);
    // 다른 패키지의 참조는 그대로 같은 에셋이다.
    REQUIRE(again.Info("/Game/User", AssetInfoOptions{}, out));
    TestAsset* userAsset = Representative(again.Packages(), userId.Value());
    REQUIRE(userAsset != nullptr);
    const EditorPackageInfo* rock = again.Packages().Find(imported.id);
    REQUIRE(rock != nullptr);
    CHECK(userAsset->hard.Handle() == again.Packages()
                                          .Registry()
                                          .FindChild(again.Packages().Access(), rock->package, "")
                                          .Value());
    CHECK(Matches(env, scene, again.Index()));

    // 원본 바꾸기와 옵션 명시 대입, 청하지 않은 기존 출력(fresh)은 고아.
    const AssetId freshId = result.outputs[1].id;
    REQUIRE(scene.PutSource("SourceArt/Other.trock", "value=1\noutputs=extra"));
    const String other = scene.Absolute(env.allocator, "SourceArt/Other.trock");
    const AssetImportSourceInput replace[] = {{{}, other.View()}};
    Assignments option(env.allocator);
    option.Add("scale=7");
    AssetService third(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(third.Initialize());
    warnings.Clear();
    REQUIRE(third.Reimport(scene.lock, "/Game/Rock", replace, option.list.View(), diagnostics,
                           result, &warnings));
    REQUIRE(warnings.Size() == 1);
    CHECK(warnings[0].kind == EditorWarningKind::OrphanedOutput &&
          warnings[0].diagnostic.asset == freshId && warnings[0].diagnostic.related == imported.id);
    auto again2 = ReadFile(env, scene, "Content/Rock.rasset");
    REQUIRE(again2);
    auto replaced = ReadImportRecord(env.allocator, imported.id, again2.Value().importRecord);
    REQUIRE(replaced);
    CHECK(replaced.Value().sources[0].relativePath.View() == "SourceArt/Other.trock" &&
          replaced.Value().outputs.Size() == 2);
    REQUIRE(third.Info("/Game/Rock", AssetInfoOptions{}, out));
    CHECK(Representative(third.Packages(), imported.id)->value == 7);
    Array<const AssetIndexEntry*> orphans(env.allocator);
    REQUIRE(third.Index().Orphans(orphans));
    CHECK(orphans.Size() == 1 && orphans[0]->path.View() == "/Game/Rock_fresh");

    // 지워진 기록 출력(extra 파일을 밖에서 지움)은 새 id 새 출력이다. 경고 목록 없이도.
    // (옮길 때 남은 별칭이 옛 경로를 점유하므로 지운다.)
    bool notDurable = false;
    REQUIRE(third.AliasRemove(scene.lock, "/Game/Rock_extra", notDurable));
    REQUIRE(third.FlushCache(scene.lock));
    scene.files.Remove("Content/Moved/Extra.rasset");
    AssetService fourth(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(fourth.Initialize());
    REQUIRE(fourth.Reimport(scene.lock, "/Game/Rock", replace, {}, diagnostics, result));
    REQUIRE(result.outputs.Size() == 1);
    CHECK(!(result.outputs[0].id == extraId) &&
          result.outputs[0].path.View() == "/Game/Rock_extra" &&
          scene.files.Has("Content/Rock_extra.rasset"));
    CHECK(Matches(env, scene, fourth.Index()));

    // 같은 서비스에서 이어서: 성공한 재임포트·새 임포트는 이번에 연 패키지를 닫는다(다음 재임포트가
    // Busy가 아니다).
    CHECK(fourth.Packages().Count() == 0);
    REQUIRE(fourth.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, result));
    CHECK(fourth.Packages().Count() == 0);
    AssetImportResult second(env.allocator);
    REQUIRE(fourth.Import(scene.lock, "/Game/Second", inputs, {}, {}, diagnostics, second));
    CHECK(fourth.Packages().Count() == 0);
    REQUIRE(fourth.Reimport(scene.lock, "/Game/Second", {}, {}, diagnostics, result));
    CHECK(result.id == second.id && fourth.Packages().Count() == 0 &&
          Matches(env, scene, fourth.Index()));
}

// 옵션 Record 하나다(schema·버전·필드 1의 wire·내용).
Array<uint8> OptionsRecord(AssetEnv& env, Guid schema, uint32 version, WireType wire,
                           StringView text, Guid field = Guid{0, 1}) noexcept {
    RecordBuilder record(env.allocator, schema, version);
    CHECK(record.AddField(field, wire, Bytes(text)));
    auto built = record.Build();
    CHECK(built);
    return built ? rocks::Move(built.Value()) : Array<uint8>(env.allocator);
}

// 재임포트 거부(파일·색인·열린 패키지 그대로): Initialize 전·잠금 아님·경로 규칙·없음·같은 경로 키
// 둘, 기록 없음·참조 기록(related), 임포터 없음·기록 임포터 버전이 더 새, 주 출력·출력 열림 Busy,
// 출력의 기록이 다른 owner·출력 읽기 전용·출력 같은 id 둘·주 출력 같은 id 둘·주 출력 읽기 전용,
// 기록 역할이 임포터와 다름·모르는 루트 MissingSource·원본 없음·모르는 바꾸기 역할·원본 둘인 기록에
// `--source=`·원본 수가 임포터 roles와 다름, 옵션 Record(다른 schema·더 새 버전·필드 wire·JSON
// 문법·모르는 필드·필드 1 없음·옵션 없는 임포터에 기록), 실패 진단 인자 없이도, 임포터 표 없는
// 서비스, 색인 뒤 손상된 출력 파일, 주 출력을 청하지 않는 임포터, 주 출력의 미해결 import
// MissingDependency, 기존 출력과 타입이 다른 요청 TypeMismatch.
TEST(AssetReimport_Rejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "reimport-reject");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource(RockSource, "value=1\noutputs=extra") &&
            scene.PutSource("SourceArt/px.png", "value=1") &&
            scene.PutSource("SourceArt/nx.png", "value=2"));
    REQUIRE(PutCrafted(env, sources, scene, CraftedRecord{}));
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    AssetDiagnostic failure;
    auto rejectsWith = [&](AssetServiceConfig config, StringView path,
                           Span<const AssetImportSourceInput> overrides, ErrorCode error,
                           AssetDiagnosticCode code) noexcept {
        AssetService service(env.allocator, scene.layout, config);
        if (!service.Initialize()) {
            return false;
        }

        const usize packages = PackageFiles(scene);
        failure = AssetDiagnostic{};
        auto reimported = service.Reimport(scene.lock, path, overrides, {}, diagnostics, result,
                                           nullptr, &failure);
        return !reimported && reimported.Error() == error && failure.code == code &&
               PackageFiles(scene) == packages && service.Packages().Count() == 0 &&
               result.id.IsNull() && Matches(env, scene, service.Index());
    };
    auto rejects = [&](StringView path, Span<const AssetImportSourceInput> overrides,
                       ErrorCode error, AssetDiagnosticCode code) noexcept {
        return rejectsWith(scene.Config(env), path, overrides, error, code);
    };
    auto crafted = [&](const CraftedRecord& record) noexcept {
        return PutCrafted(env, sources, scene, record);
    };

    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        CHECK(service.Reimport(scene.lock, "/Game/Leaf", {}, {}, diagnostics, result).Error() ==
              ErrorCode::InvalidArgument);
        REQUIRE(service.Initialize());
        AssetDatabaseLock other;
        CHECK(service.Reimport(other, "/Game/Leaf", {}, {}, diagnostics, result).Error() ==
              ErrorCode::InvalidArgument);
    }

    CHECK(rejects("Game/Leaf", {}, ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Nope", {}, ErrorCode::NotFound, AssetDiagnosticCode::None));
    CHECK(rejects("/Game/Dir/Hero", {}, ErrorCode::InvalidArgument, AssetDiagnosticCode::None) &&
          failure.related.IsNull());
    CHECK(rejects("/Game/Villain", {}, ErrorCode::InvalidArgument, AssetDiagnosticCode::None) &&
          failure.related == FixedId(LeafId));
    // 같은 경로 키 둘.
    REQUIRE(scene.files.AddDirectory("Content/Split") && scene.files.AddDirectory("Content/split"));
    REQUIRE(scene.files.AddFile("Content/Split/Dup.rasset", sources.cycleA.View()) &&
            scene.files.AddFile("Content/split/Dup.rasset", sources.cycleB.View()));
    CHECK(rejects("/Game/Split/Dup", {}, ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicatePath));
    scene.files.Remove("Content/Split/Dup.rasset");
    scene.files.Remove("Content/split/Dup.rasset");
    // 임포터: 표에 없음·기록 버전이 더 새.
    {
        AssetImporterTable cubeOnly(env.allocator, env.types);
        TestImporter cube(CubeImporterInfo());
        auto registered = cubeOnly.Register(cube);
        REQUIRE(registered);
        AssetServiceConfig config = scene.Config(env);
        config.importers = &cubeOnly;
        CHECK(rejectsWith(config, "/Game/Leaf", {}, ErrorCode::Unsupported,
                          AssetDiagnosticCode::UnsupportedSourceFormat) &&
              failure.asset == FixedId(LeafId));
    }

    CraftedRecord newer;
    newer.importerVersion = TestImporterVersion + 1;
    REQUIRE(crafted(newer));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::Unsupported, AssetDiagnosticCode::NewerVersion));
    // 열림(주 출력·출력).
    REQUIRE(crafted(CraftedRecord{}));
    {
        const StringView targets[] = {"/Game/Leaf", "/Game/Villain"};
        const uint64 ids[] = {LeafId, VillainId};
        for (usize index = 0; index < 2; ++index) {
            AssetService service(env.allocator, scene.layout, scene.Config(env));
            REQUIRE(service.Initialize());
            String out(env.allocator);
            REQUIRE(service.Info(targets[index], AssetInfoOptions{}, out));
            failure = AssetDiagnostic{};
            auto busy = service.Reimport(scene.lock, "/Game/Leaf", {}, {}, diagnostics, result,
                                         nullptr, &failure);
            CHECK(!busy && busy.Error() == ErrorCode::Busy && failure.asset == FixedId(ids[index]));
        }
    }

    // 출력: 다른 owner·읽기 전용·같은 id 둘. 주 출력: 같은 id 둘·읽기 전용.
    CraftedRecord stranger;
    stranger.villainOwner = BaseId;
    REQUIRE(crafted(stranger));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::InvalidArgument, AssetDiagnosticCode::None) &&
          failure.asset == FixedId(VillainId) && failure.related == FixedId(LeafId));
    CraftedRecord readOnlyOutput;
    readOnlyOutput.villainReadOnly = true;
    REQUIRE(crafted(readOnlyOutput));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::Unsupported, AssetDiagnosticCode::NewerVersion) &&
          failure.asset == FixedId(VillainId));
    REQUIRE(crafted(CraftedRecord{}));
    REQUIRE(scene.files.AddFile("Content/Villain2.rasset",
                                scene.files.Bytes("Content/Villain.rasset")));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::AlreadyExists,
                  AssetDiagnosticCode::DuplicateAssetId) &&
          failure.asset == FixedId(VillainId));
    scene.files.Remove("Content/Villain2.rasset");
    REQUIRE(scene.files.AddFile("Content/Leaf2.rasset", scene.files.Bytes("Content/Leaf.rasset")));
    CHECK(
        rejects("/Game/Leaf", {}, ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicateAssetId));
    scene.files.Remove("Content/Leaf2.rasset");
    CraftedRecord readOnlyMain;
    readOnlyMain.leafReadOnly = true;
    REQUIRE(crafted(readOnlyMain));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::Unsupported, AssetDiagnosticCode::NewerVersion));

    // 원본: 역할이 임포터와 다름·모르는 루트·파일 없음·모르는 바꾸기 역할·원본 둘인 기록에
    // --source=.
    CraftedRecord role;
    role.role = "zz";
    REQUIRE(crafted(role));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidImportOption));
    CraftedRecord root;
    root.root = "Nope";
    REQUIRE(crafted(root));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::NotFound, AssetDiagnosticCode::MissingSource) &&
          failure.detail == 0);
    CraftedRecord missing;
    missing.relative = "SourceArt/Missing.trock";
    REQUIRE(crafted(missing));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::NotFound, AssetDiagnosticCode::MissingSource));
    REQUIRE(crafted(CraftedRecord{}));
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput unknownRole[] = {{"px", source.View()}};
    CHECK(rejects("/Game/Leaf", unknownRole, ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidImportOption) &&
          failure.detail == 0);
    CraftedRecord cube;
    cube.importerId = CubeImporterId;
    cube.importerVersion = 1;
    cube.twoRoles = true;
    REQUIRE(crafted(cube));
    const AssetImportSourceInput single[] = {{{}, source.View()}};
    CHECK(rejects("/Game/Leaf", single, ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidImportOption));
    // 원본 둘인 기록의 역할 하나 바꾸기는 된다(성공 — 상태가 바뀌므로 마지막에 따로 본다).

    // 옵션 Record.
    const TypeId optionsType = GeneratedType<TestImportOptions>::Id();
    auto lease = env.types.AcquireValueType(optionsType, TypeKind::Struct);
    REQUIRE(lease);
    const uint32 version = lease.Value().Schema().version;
    // 오류 코드는 판정 단계의 것이다(schema TypeMismatch·버전 Unsupported·그 밖 InvalidArgument)
    // — 진단은 모두 InvalidImportOption이다.
    struct BadOptions {
        Array<uint8> record;
        ErrorCode error;
    };
    const BadOptions badOptions[] = {
        {OptionsRecord(env, Guid{0x4f, 1}, version, WireType::String, "{}"),
         ErrorCode::TypeMismatch},
        {OptionsRecord(env, optionsType, version + 1, WireType::String, "{}"),
         ErrorCode::Unsupported},
        {OptionsRecord(env, optionsType, version, WireType::U32, "1234"),
         ErrorCode::InvalidArgument},
        {OptionsRecord(env, optionsType, version, WireType::String, "{"),
         ErrorCode::InvalidArgument},
        {OptionsRecord(env, optionsType, version, WireType::String, "{\"unknown\": 1}"),
         ErrorCode::InvalidArgument},
        {OptionsRecord(env, optionsType, version, WireType::String, "{}", Guid{0, 2}),
         ErrorCode::InvalidArgument}};
    for (const BadOptions& bad : badOptions) {
        CraftedRecord withOptions;
        withOptions.options = bad.record.View();
        REQUIRE(crafted(withOptions));
        CHECK(rejects("/Game/Leaf", {}, bad.error, AssetDiagnosticCode::InvalidImportOption));
    }

    {
        const Array<uint8> options =
            OptionsRecord(env, optionsType, version, WireType::String, "{\"scale\": 2}");
        CraftedRecord cubeWithOptions;
        cubeWithOptions.importerId = CubeImporterId;
        cubeWithOptions.importerVersion = 1;
        cubeWithOptions.twoRoles = true;
        cubeWithOptions.options = options.View();
        REQUIRE(crafted(cubeWithOptions));
        CHECK(rejects("/Game/Leaf", {}, ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::InvalidImportOption));
    }

    // 원본 수가 임포터 roles와 다른 기록(Cube 임포터에 원본 하나).
    {
        CraftedRecord oneForCube;
        oneForCube.importerId = CubeImporterId;
        oneForCube.importerVersion = 1;
        REQUIRE(crafted(oneForCube));
        CHECK(rejects("/Game/Leaf", {}, ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::InvalidImportOption));
    }

    // 원본 둘인 기록의 역할 하나 바꾸기(성공).
    {
        REQUIRE(crafted(cube));
        const String nx = scene.Absolute(env.allocator, "SourceArt/px.png");
        const AssetImportSourceInput replaceOne[] = {{"nx", nx.View()}};
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        REQUIRE(service.Reimport(scene.lock, "/Game/Leaf", replaceOne, {}, diagnostics, result));
        String out(env.allocator);
        REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
        CHECK(Representative(service.Packages(), FixedId(LeafId))->value == 2);
    }

    // 실패 진단 인자 없이도(기록 없음·다른 owner의 출력), 임포터 표 없는 서비스.
    {
        REQUIRE(crafted(CraftedRecord{}));
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        CHECK(service.Reimport(scene.lock, "/Game/Dir/Hero", {}, {}, diagnostics, result).Error() ==
              ErrorCode::InvalidArgument);
        CraftedRecord other;
        other.villainOwner = BaseId;
        REQUIRE(crafted(other));
        AssetService stranger(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(stranger.Initialize());
        CHECK(stranger.Reimport(scene.lock, "/Game/Leaf", {}, {}, diagnostics, result).Error() ==
              ErrorCode::InvalidArgument);
        REQUIRE(crafted(CraftedRecord{}));
        AssetServiceConfig config = scene.Config(env);
        config.importers = nullptr;
        CHECK(rejectsWith(config, "/Game/Leaf", {}, ErrorCode::Unsupported,
                          AssetDiagnosticCode::UnsupportedSourceFormat));
    }

    // 색인 뒤 손상된 출력 파일: 출력 열기 실패가 그대로 돌아온다(진단 인자 있음·없음), 남는 열린
    // 패키지 없음.
    {
        REQUIRE(crafted(CraftedRecord{}));
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        Array<uint8> villain(env.allocator);
        REQUIRE(villain.TryAppend(scene.files.Bytes("Content/Villain.rasset")));
        scene.files.Remove("Content/Villain.rasset");
        REQUIRE(
            scene.files.AddFile("Content/Villain.rasset", villain.View().Left(villain.Size() / 2)));
        failure = AssetDiagnostic{};
        CHECK(!service.Reimport(scene.lock, "/Game/Leaf", {}, {}, diagnostics, result, nullptr,
                                &failure) &&
              failure.asset == FixedId(VillainId) && service.Packages().Count() == 0);
        CHECK(!service.Reimport(scene.lock, "/Game/Leaf", {}, {}, diagnostics, result) &&
              service.Packages().Count() == 0);
        REQUIRE(crafted(CraftedRecord{}));
    }

    // 기록 출력 타입이 그 패키지의 대표 타입과 다름(손 편집 — 주 출력·출력) TypeMismatch.
    {
        CraftedRecord villainType;
        villainType.villainType = optionsType;
        REQUIRE(crafted(villainType));
        CHECK(
            rejects("/Game/Leaf", {}, ErrorCode::TypeMismatch, AssetDiagnosticCode::TypeMismatch) &&
            failure.asset == FixedId(VillainId));
        CraftedRecord leafType;
        leafType.leafType = optionsType;
        REQUIRE(crafted(leafType));
        CHECK(
            rejects("/Game/Leaf", {}, ErrorCode::TypeMismatch, AssetDiagnosticCode::TypeMismatch) &&
            failure.asset == FixedId(LeafId));
    }

    // 색인 뒤 밖에서 바뀐 기록(출력 목록·임포터 버전·임포터 — 판정은 색인 요약으로 했다)
    // ExternalModification / StaleHandle.
    {
        CraftedRecord fewer;
        fewer.mainOnly = true;
        CraftedRecord older;
        older.importerVersion = TestImporterVersion - 1;
        CraftedRecord otherImporter;
        otherImporter.importerId = CubeImporterId;
        otherImporter.importerVersion = 1;
        otherImporter.twoRoles = true;
        const CraftedRecord* changes[] = {&fewer, &older, &otherImporter};
        for (const CraftedRecord* changed : changes) {
            REQUIRE(crafted(CraftedRecord{}));
            AssetService service(env.allocator, scene.layout, scene.Config(env));
            REQUIRE(service.Initialize());
            REQUIRE(crafted(*changed));
            failure = AssetDiagnostic{};
            auto stale = service.Reimport(scene.lock, "/Game/Leaf", {}, {}, diagnostics, result,
                                          nullptr, &failure);
            CHECK(!stale && stale.Error() == ErrorCode::StaleHandle &&
                  failure.code == AssetDiagnosticCode::ExternalModification &&
                  service.Packages().Count() == 0);
        }

        REQUIRE(crafted(CraftedRecord{}));
    }

    // 주 출력을 청하지 않는 재임포트는 InvalidArgument다.
    REQUIRE(scene.PutSource(RockSource, "value=1\nnomain=1"));
    CHECK(rejects("/Game/Leaf", {}, ErrorCode::InvalidArgument, AssetDiagnosticCode::None));

    // 주 출력의 미해결 import(참조하던 추가 출력을 밖에서 지움) MissingDependency(related).
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
        REQUIRE(scene.PutSource(RockSource, "value=1\noutputs=extra\nreference=extra"));
        REQUIRE(service.Import(scene.lock, "/Game/Linked", inputs, {}, {}, diagnostics, result));
        REQUIRE(service.FlushCache(scene.lock));
        const AssetId extra = result.outputs[0].id;
        scene.files.Remove("Content/Linked_extra.rasset");
        CHECK(rejects("/Game/Linked", {}, ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::MissingDependency) &&
              failure.related == extra);
        AssetService quiet(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(quiet.Initialize());
        CHECK(quiet.Reimport(scene.lock, "/Game/Linked", {}, {}, diagnostics, result).Error() ==
              ErrorCode::InvalidArgument);
    }

    // 기존 출력과 타입이 다른 요청(TypeMismatch — 실제 임포트 뒤 원본을 바꾼다).
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
        REQUIRE(scene.PutSource(RockSource, "value=1\noutputs=extra"));
        REQUIRE(service.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, result));
        REQUIRE(service.FlushCache(scene.lock));
        const AssetId extra = result.outputs[0].id;
        REQUIRE(scene.PutSource(RockSource, "value=1\noutputs=extra:BaseGameAsset"));
        CHECK(rejects("/Game/Rock", {}, ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::TypeMismatch) &&
              failure.related == extra);
    }
}

// 재임포트 중간 실패(저장 순서 새 출력 → 기존 출력 → 주 출력 — 7.1절 19 A2f3d): 새 출력 저장이
// 실패하면(기존 파일을 아직 교체하지 않았다) 아무 파일도 바뀌지 않는다. 주 출력 저장이 실패하면
// 먼저 교체한 기존 출력은 새 내용으로 남고, 새 출력 파일도 남는다(교체한 기존 파일이 가리킬 수
// 있다), 주 출력 파일은 그대로다(기록은 옛 SHA). 다시 하면 남은 새 출력(이 owner의 참조 기록
// 고아)을 같은 key로 다시 써서 수렴한다(같은 id).
TEST(AssetReimport_PartialFailure) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "reimport-partial");
    REQUIRE(scene.ready);
    auto registration =
        scene.hooks.Register(IdOf<TestAsset>(), AssetTypeHooks{&CountingTags, nullptr, nullptr});
    REQUIRE(registration);
    REQUIRE(scene.PutSource(RockSource, "value=1\noutputs=extra"));
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult result(env.allocator);
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        REQUIRE(service.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, result));
        REQUIRE(service.FlushCache(scene.lock));
    }

    const Array<uint8> mainBefore = [&]() noexcept {
        Array<uint8> copy(env.allocator);
        CHECK(copy.TryAppend(scene.files.Bytes("Content/Rock.rasset")));
        return copy;
    }();
    const Array<uint8> extraBefore = [&]() noexcept {
        Array<uint8> copy(env.allocator);
        CHECK(copy.TryAppend(scene.files.Bytes("Content/Rock_extra.rasset")));
        return copy;
    }();
    REQUIRE(scene.PutSource(RockSource, "value=9\noutputs=extra,fresh"));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    // 첫째 저장(새 출력 fresh)의 Tags가 실패한다 — 아무 파일도 바뀌지 않는다.
    tagCalls = 0;
    failTagsOn = 1;
    Array<EditorWarning> warnings(env.allocator);
    auto early = service.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, result, &warnings);
    failTagsOn = 0;
    CHECK(!early && early.Error() == ErrorCode::Busy && warnings.IsEmpty() &&
          !scene.files.Has("Content/Rock_fresh.rasset") && service.Packages().Count() == 0);
    CHECK(SameBytes(scene.files.Bytes("Content/Rock.rasset"), mainBefore.View()) &&
          SameBytes(scene.files.Bytes("Content/Rock_extra.rasset"), extraBefore.View()));

    // 저장 순서: fresh(새) → extra(기존) → 주 출력 — 셋째 Tags가 실패한다.
    tagCalls = 0;
    failTagsOn = 3;
    auto reimported =
        service.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, result, &warnings);
    failTagsOn = 0;
    CHECK(!reimported && reimported.Error() == ErrorCode::Busy && warnings.IsEmpty() &&
          scene.files.Has("Content/Rock_fresh.rasset") && service.Packages().Count() == 0);
    const Span<const uint8> mainAfter = scene.files.Bytes("Content/Rock.rasset");
    const Span<const uint8> extraAfter = scene.files.Bytes("Content/Rock_extra.rasset");
    CHECK(SameBytes(mainAfter, mainBefore.View()) && !SameBytes(extraAfter, extraBefore.View()));
    CHECK(Matches(env, scene, service.Index()));
    auto fresh = ParseLogicalPath(env.allocator, "/Game/Rock_fresh");
    REQUIRE(fresh);
    auto left = service.Index().FindByPath(fresh.Value());
    REQUIRE(left);
    const AssetId leftId = left.Value()->id;
    // 다시 하면 남은 fresh를 같은 key로 다시 써서 수렴한다.
    REQUIRE(service.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, result));
    CHECK(result.outputs.Size() == 2 && scene.files.Has("Content/Rock_fresh.rasset"));
    bool reused = false;
    for (const AssetImportOutput& output : result.outputs) {
        reused = reused || (output.key.View() == "fresh" && output.id == leftId);
    }

    CHECK(reused && Matches(env, scene, service.Index()));
}

// 재임포트 할당 실패: 기존 출력·새 출력·옵션 기록이 있는 재임포트의 모든 할당 실패가
// OutOfMemory이고 주 출력 파일·열린 패키지·새 출력 파일이 그대로다(기존 출력은 먼저 저장될 수 있다
// — 중간 실패).
TEST(AssetReimport_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "reimport-oom");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource(RockSource, "value=1\noutputs=extra,gone"));
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        AssetImportResult result(env.allocator);
        Assignments option(env.allocator);
        option.Add("scale=2");
        REQUIRE(service.Import(scene.lock, "/Game/Rock", inputs, {}, option.list.View(),
                               diagnostics, result));
        REQUIRE(service.FlushCache(scene.lock));
    }

    REQUIRE(scene.PutSource(RockSource, "value=3\noutputs=extra,fresh"));
    Array<uint8> mainSnapshot(env.allocator);
    Array<uint8> extraSnapshot(env.allocator);
    REQUIRE(mainSnapshot.TryAppend(scene.files.Bytes("Content/Rock.rasset")) &&
            extraSnapshot.TryAppend(scene.files.Bytes("Content/Rock_extra.rasset")));
    Assignments assignments(env.allocator);
    assignments.Add("note=n");
    TaggedFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetService service(failing, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        // 무관한 열린 패키지가 있으면 열린 id 기록이 할당한다.
        String out(env.allocator);
        REQUIRE(service.Info("/Game/Leaf", AssetInfoOptions{}, out));
        const usize open = service.Packages().Count();
        AssetImportResult result(failing);
        Array<EditorWarning> warnings(failing);
        AssetDiagnostic failure;
        failing.FailOnAttempt(attempt);
        auto reimported = service.Reimport(scene.lock, "/Game/Rock", {}, assignments.list.View(),
                                           diagnostics, result, &warnings, &failure);
        const bool failed = failing.Failed();
        const uint32 tag = failing.FailedTag();
        failing.DisableFailure();
        if (!failed || reimported) {
            REQUIRE(reimported);
            CHECK(scene.files.Has("Content/Rock_fresh.rasset") &&
                  !SameBytes(scene.files.Bytes("Content/Rock.rasset"), mainSnapshot.View()) &&
                  Matches(env, scene, service.Index()));
            if (!failed) {
                CHECK(service.Packages().Count() == open);
                break;
            }

            // 삼킨 실패: GC 표, 또는 성공 뒤 닫기(이번에 연 패키지가 열린 채 남는다). 처음 상태로
            // 되돌린다.
            CHECK(tag == GcReferenceTableTag || service.Packages().Count() > open);
            scene.files.Remove("Content/Rock_fresh.rasset");
            scene.files.Remove("Content/Rock.rasset");
            scene.files.Remove("Content/Rock_extra.rasset");
            REQUIRE(scene.files.AddFile("Content/Rock.rasset", mainSnapshot.View()) &&
                    scene.files.AddFile("Content/Rock_extra.rasset", extraSnapshot.View()));
            continue;
        }

        CHECK(!reimported && reimported.Error() == ErrorCode::OutOfMemory &&
              failure.code == AssetDiagnosticCode::None);
        CHECK(SameBytes(scene.files.Bytes("Content/Rock.rasset"), mainSnapshot.View()) &&
              service.Packages().Count() == open && Matches(env, scene, service.Index()));
        // 새 출력 fresh는 기존 출력을 교체하기 전의 실패면 없다(교체한 뒤면 남는다 — A2f3d).
        if (SameBytes(scene.files.Bytes("Content/Rock_extra.rasset"), extraSnapshot.View())) {
            CHECK(!scene.files.Has("Content/Rock_fresh.rasset"));
        }

        // 먼저 저장된 기존 출력·남은 새 출력(중간 실패)을 처음 상태로 되돌려 다음 반복이 같은
        // 상태에서 시작한다.
        scene.files.Remove("Content/Rock_fresh.rasset");
        scene.files.Remove("Content/Rock_extra.rasset");
        REQUIRE(scene.files.AddFile("Content/Rock_extra.rasset", extraSnapshot.View()));
        ++failures;
    }

    CHECK(failures > 0);
}

// AssetId 텍스트다.
struct IdText {
    explicit IdText(AssetId id) noexcept : builder{Span<char>(text)} {
        CHECK(builder.TryAppendFormat("{}", id));
    }

    StringView View() const noexcept {
        return builder.View();
    }

    char text[AssetId::TextBytes + 1] = {};
    FixedStringBuilder builder;
};

// 임포트 상태(7.1절 18 A2f2b — 실제 임포트한 에셋): 소유 기록의 임포터·원본 current(경로·AssetId
// 대상 같은 결과), 다른 출력의 owner·ownerPath, 원본 같은 크기 다른 내용·다른 크기 SourceChanged,
// 원본 자리가 디렉터리·열기 사용 중·크기 조회 실패·읽기 실패 InvalidSource(error), 원본 없음
// MissingSource, 패키지를 열지 않음(같은 서비스의 재임포트가 된다), 임포터 표 없음
// UnsupportedSourceFormat, 별칭으로 푼 대상(경고 AliasResolved), 원본 대조 함수(빈 버퍼·작은
// 버퍼로 여러 번 읽기).
TEST(AssetImportStatus_Imported) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-status");
    REQUIRE(scene.ready);
    constexpr StringView RockText = "value=2\noutputs=extra";
    REQUIRE(scene.PutSource(RockSource, RockText));
    AssetService service(env.allocator, scene.layout, scene.Config(env));
    REQUIRE(service.Initialize());
    const String source = scene.Absolute(env.allocator, RockSource);
    const AssetImportSourceInput inputs[] = {{DefaultImportSourceRole, source.View()}};
    TextDiagnostics diagnostics(env.allocator);
    AssetImportResult imported(env.allocator);
    REQUIRE(service.Import(scene.lock, "/Game/Rock", inputs, {}, {}, diagnostics, imported));
    REQUIRE(imported.outputs.Size() == 1);

    AssetImportStatus status(env.allocator);
    auto sourceState = [&](StringView target) noexcept {
        AssetDiagnostic failure;
        auto shown = service.ImportStatus(target, status, nullptr, &failure);
        return shown && status.sources.Size() == 1 ? status.sources[0].state
                                                   : AssetDiagnosticCode::CorruptPackage;
    };
    // 소유 기록: 임포터·원본 current, 패키지를 열지 않는다.
    REQUIRE(service.ImportStatus("/Game/Rock", status));
    CHECK(status.Owned() && status.id == imported.id && status.importerId == TestImporterId &&
          status.importerName.View() == "TestImporter" &&
          status.importerVersion == TestImporterVersion &&
          status.importer == AssetDiagnosticCode::None && status.ownerPath.IsEmpty());
    REQUIRE(status.sources.Size() == 1);
    CHECK(status.sources[0].role.View() == DefaultImportSourceRole &&
          status.sources[0].root.View() == "Project" &&
          status.sources[0].relative.View() == RockSource &&
          status.sources[0].state == AssetDiagnosticCode::None &&
          status.sources[0].error == ErrorCode::None);
    CHECK(service.Packages().Count() == 0);
    // AssetId 대상은 같은 결과다.
    const IdText rockId(imported.id);
    CHECK(sourceState(rockId.View()) == AssetDiagnosticCode::None && status.id == imported.id);
    // 다른 출력: owner·ownerPath만.
    REQUIRE(service.ImportStatus("/Game/Rock_extra", status));
    CHECK(!status.Owned() && status.id == imported.outputs[0].id && status.owner == imported.id &&
          status.ownerPath.View() == "/Game/Rock" && status.sources.IsEmpty());

    // 원본 상태: 같은 크기 다른 내용·다른 크기 SourceChanged, 없음 MissingSource.
    REQUIRE(scene.PutSource(RockSource, "value=3\noutputs=extra"));
    CHECK(sourceState("/Game/Rock") == AssetDiagnosticCode::SourceChanged);
    REQUIRE(scene.PutSource(RockSource, "value=30\noutputs=extra"));
    CHECK(sourceState("/Game/Rock") == AssetDiagnosticCode::SourceChanged);
    scene.files.Remove(RockSource);
    CHECK(sourceState("/Game/Rock") == AssetDiagnosticCode::MissingSource &&
          status.sources[0].error == ErrorCode::None);
    // 원본 자리가 디렉터리·열기 사용 중·크기 조회 실패·읽기 실패(같은 크기라 읽는다) InvalidSource.
    REQUIRE(scene.files.AddDirectory(RockSource));
    CHECK(sourceState("/Game/Rock") == AssetDiagnosticCode::InvalidSource &&
          status.sources[0].error != ErrorCode::None);
    scene.files.Remove(RockSource);
    const MemoryFault faults[] = {MemoryFault::OpenBusy, MemoryFault::Size, MemoryFault::Read};
    for (MemoryFault fault : faults) {
        scene.files.Remove(RockSource);
        REQUIRE(scene.files.AddFile(RockSource, Bytes(RockText), fault));
        CHECK(sourceState("/Game/Rock") == AssetDiagnosticCode::InvalidSource &&
              status.sources[0].error != ErrorCode::None);
    }

    // 상태 조회는 패키지를 열지 않으므로 같은 서비스의 재임포트가 된다.
    REQUIRE(scene.PutSource(RockSource, RockText));
    CHECK(sourceState("/Game/Rock") == AssetDiagnosticCode::None);
    AssetImportResult reimported(env.allocator);
    REQUIRE(service.Reimport(scene.lock, "/Game/Rock", {}, {}, diagnostics, reimported));

    // 임포터 표 없는 서비스: UnsupportedSourceFormat(원본 상태는 그대로 본다).
    {
        AssetServiceConfig config = scene.Config(env);
        config.importers = nullptr;
        AssetService bare(env.allocator, scene.layout, config);
        REQUIRE(bare.Initialize());
        REQUIRE(bare.ImportStatus("/Game/Rock", status));
        CHECK(status.importer == AssetDiagnosticCode::UnsupportedSourceFormat &&
              status.sources.Size() == 1 && status.sources[0].state == AssetDiagnosticCode::None);
    }

    // 별칭으로 푼 대상(옮긴 뒤 옛 경로): 경고 AliasResolved.
    AssetMoveResult moved;
    REQUIRE(service.Move(scene.lock, "/Game/Rock", "/Game/Moved/Rock", moved));
    Array<EditorWarning> warnings(env.allocator);
    REQUIRE(service.ImportStatus("/Game/Rock", status, &warnings));
    CHECK(status.id == imported.id && warnings.Size() == 1 &&
          warnings[0].kind == EditorWarningKind::AliasResolved);

    // 원본 대조 함수: 빈 버퍼 InvalidSource / InvalidArgument, 작은 버퍼로 여러 번 읽어도 같다.
    auto digest = Sha256::Hash(Bytes(RockText));
    ErrorCode error = ErrorCode::None;
    uint8 small[3] = {};
    CHECK(CheckImportSource(scene.files, source.View(), RockText.ByteCount(), digest, {}, error) ==
              AssetDiagnosticCode::InvalidSource &&
          error == ErrorCode::InvalidArgument);
    CHECK(CheckImportSource(scene.files, source.View(), RockText.ByteCount(), digest,
                            Span<uint8>(small), error) == AssetDiagnosticCode::None &&
          error == ErrorCode::None);
    digest.bytes[0] ^= 1;
    CHECK(CheckImportSource(scene.files, source.View(), RockText.ByteCount(), digest,
                            Span<uint8>(small), error) == AssetDiagnosticCode::SourceChanged);
    CHECK(Matches(env, scene, service.Index()));
}

// 임포트 상태(손으로 만든 기록 — Leaf 소유·Villain 참조): 임포터 버전 낮음 ImporterOutdated·높음
// NewerVersion·모르는 임포터 UnsupportedSourceFormat, 원본 둘(역할 순), 모르는 루트 MissingSource
// (NotFound), 절대 경로 원본(빈 루트 — 내용 비교로 SourceChanged), 기록 byte 수와 다른 원본
// SourceChanged, owner가 색인에 없음·같은 id 둘이면 ownerPath 빔.
TEST(AssetImportStatus_Crafted) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-status-crafted");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource(RockSource, "value=1") &&
            scene.PutSource("SourceArt/px.png", "value=1") &&
            scene.PutSource("SourceArt/nx.png", "value=2"));
    AssetImportStatus status(env.allocator);
    auto statusOf = [&](const CraftedRecord& record, StringView target) noexcept {
        if (!PutCrafted(env, sources, scene, record)) {
            return false;
        }

        AssetService service(env.allocator, scene.layout, scene.Config(env));
        return service.Initialize() && service.ImportStatus(target, status) &&
               service.Packages().Count() == 0;
    };

    // 기록 byte 수 0과 다른 원본은 읽지 않고 SourceChanged다.
    REQUIRE(statusOf(CraftedRecord{}, "/Game/Leaf"));
    REQUIRE(status.sources.Size() == 1);
    CHECK(status.importer == AssetDiagnosticCode::None &&
          status.sources[0].state == AssetDiagnosticCode::SourceChanged);
    // 임포터 버전.
    CraftedRecord older;
    older.importerVersion = TestImporterVersion - 1;
    REQUIRE(statusOf(older, "/Game/Leaf"));
    CHECK(status.importer == AssetDiagnosticCode::ImporterOutdated &&
          status.importerVersion == TestImporterVersion - 1);
    CraftedRecord newer;
    newer.importerVersion = TestImporterVersion + 1;
    REQUIRE(statusOf(newer, "/Game/Leaf"));
    CHECK(status.importer == AssetDiagnosticCode::NewerVersion);
    constexpr Guid UnknownImporterId{0x5a5a, 0x77};
    CraftedRecord unknown;
    unknown.importerId = UnknownImporterId;
    REQUIRE(statusOf(unknown, "/Game/Leaf"));
    CHECK(status.importer == AssetDiagnosticCode::UnsupportedSourceFormat &&
          status.importerId == UnknownImporterId);
    // 원본 둘(역할 순).
    CraftedRecord cube;
    cube.importerId = CubeImporterId;
    cube.importerVersion = 1;
    cube.twoRoles = true;
    REQUIRE(statusOf(cube, "/Game/Leaf"));
    REQUIRE(status.sources.Size() == 2);
    CHECK(status.sources[0].role.View() == "px" && status.sources[1].role.View() == "nx" &&
          status.sources[1].relative.View() == "SourceArt/nx.png" &&
          status.importer == AssetDiagnosticCode::None);
    // 모르는 루트 MissingSource(NotFound).
    CraftedRecord root;
    root.root = "Nope";
    REQUIRE(statusOf(root, "/Game/Leaf"));
    CHECK(status.sources[0].state == AssetDiagnosticCode::MissingSource &&
          status.sources[0].error == ErrorCode::NotFound);
    // 절대 경로 원본(빈 루트): 빈 파일이면 크기는 같고 내용(SHA-256 — 기록은 0)이 다르다.
    REQUIRE(scene.PutSource("SourceArt/Empty.trock", ""));
    const String absolute = scene.Absolute(env.allocator, "SourceArt/Empty.trock");
    CraftedRecord outside;
    outside.root = "";
    outside.relative = absolute.View();
    REQUIRE(statusOf(outside, "/Game/Leaf"));
    CHECK(status.sources[0].root.IsEmpty() &&
          status.sources[0].relative.View() == absolute.View() &&
          status.sources[0].state == AssetDiagnosticCode::SourceChanged);
    // 참조 기록: owner가 색인에 있음(Base)·없음·같은 id 둘.
    CraftedRecord base;
    base.villainOwner = BaseId;
    REQUIRE(statusOf(base, "/Game/Villain"));
    CHECK(!status.Owned() && status.owner == FixedId(BaseId) &&
          status.ownerPath.View() == "/Game/Base");
    CraftedRecord nowhere;
    nowhere.villainOwner = NowhereId;
    REQUIRE(statusOf(nowhere, "/Game/Villain"));
    CHECK(!status.Owned() && status.owner == FixedId(NowhereId) && status.ownerPath.IsEmpty());
    REQUIRE(scene.files.AddFile("Content/Leaf2.rasset", scene.files.Bytes("Content/Leaf.rasset")));
    REQUIRE(statusOf(CraftedRecord{}, "/Game/Villain"));
    CHECK(status.owner == FixedId(LeafId) && status.ownerPath.IsEmpty());
    scene.files.Remove("Content/Leaf2.rasset");
}

// 임포트 상태 거부(진단·오류): Initialize 전, 대상 형식(경로 규칙·AssetId 아님·null id), 없음
// (경로·id), 같은 경로 키 둘 DuplicatePath, 같은 id 둘 DuplicateAssetId, 기록 없음, 읽지 못하는
// 기록 NewerVersion / Unsupported, 형식 위반 CorruptPackage(칸 ImportRecord), 색인 뒤 사라진 파일
// MissingFile, 다른 패키지로 바뀐 파일 ExternalModification, 열기 사용 중, 잘린 파일, 원본 루트 표
// 손상.
TEST(AssetImportStatus_Rejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-status-rejects");
    REQUIRE(scene.ready);
    REQUIRE(PutCrafted(env, sources, scene, CraftedRecord{}));
    AssetImportStatus status(env.allocator);
    AssetDiagnostic failure;
    auto rejects = [&](StringView target, ErrorCode error, AssetDiagnosticCode code) noexcept {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        if (!service.Initialize()) {
            return false;
        }

        failure = AssetDiagnostic{};
        auto shown = service.ImportStatus(target, status, nullptr, &failure);
        return !shown && shown.Error() == error && failure.code == code &&
               service.Packages().Count() == 0;
    };
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        CHECK(service.ImportStatus("/Game/Leaf", status).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(rejects("Game/Leaf", ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("not-an-id", ErrorCode::InvalidArgument, AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("00000000-0000-0000-0000-000000000000", ErrorCode::InvalidArgument,
                  AssetDiagnosticCode::InvalidPath));
    CHECK(rejects("/Game/Nope", ErrorCode::NotFound, AssetDiagnosticCode::None));
    const IdText nowhere(FixedId(NowhereId));
    CHECK(rejects(nowhere.View(), ErrorCode::NotFound, AssetDiagnosticCode::None));
    CHECK(rejects("/Game/Base", ErrorCode::InvalidArgument, AssetDiagnosticCode::None) &&
          failure.asset == FixedId(BaseId));
    // 같은 경로 키 둘(대소문자만 다른 이름)·같은 id 둘.
    REQUIRE(scene.files.AddFile("Content/LEAF.rasset", scene.files.Bytes("Content/Leaf.rasset")));
    CHECK(rejects("/Game/Leaf", ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicatePath));
    const IdText leaf(FixedId(LeafId));
    CHECK(rejects(leaf.View(), ErrorCode::AlreadyExists, AssetDiagnosticCode::DuplicateAssetId) &&
          failure.asset == FixedId(LeafId));
    scene.files.Remove("Content/LEAF.rasset");
    // 읽지 못하는 기록: 더 새 스키마 버전·필드 빠진 형식 위반.
    {
        RecordBuilder newer(env.allocator, ImportRecordSchema, ImportRecordVersion + 1);
        auto newerBytes = newer.Build();
        REQUIRE(newerBytes);
        CraftedRecord unreadable;
        unreadable.rawLeafRecord = newerBytes.Value().View();
        REQUIRE(PutCrafted(env, sources, scene, unreadable));
        CHECK(rejects("/Game/Leaf", ErrorCode::Unsupported, AssetDiagnosticCode::NewerVersion));
        RecordBuilder broken(env.allocator, ImportRecordSchema, ImportRecordVersion);
        auto brokenBytes = broken.Build();
        REQUIRE(brokenBytes);
        CraftedRecord corrupt;
        corrupt.rawLeafRecord = brokenBytes.Value().View();
        REQUIRE(PutCrafted(env, sources, scene, corrupt));
        CHECK(rejects("/Game/Leaf", ErrorCode::InvalidArgument,
                      AssetDiagnosticCode::CorruptPackage) &&
              failure.section == static_cast<uint32>(PackageSectionKind::ImportRecord));
        REQUIRE(PutCrafted(env, sources, scene, CraftedRecord{}));
    }

    // 색인 뒤 파일: 사라짐 MissingFile, 다른 패키지 ExternalModification, 열기 사용 중, 잘림.
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        Array<uint8> leafBytes(env.allocator);
        REQUIRE(leafBytes.TryAppend(scene.files.Bytes("Content/Leaf.rasset")));
        auto again = [&](ErrorCode error, AssetDiagnosticCode code) noexcept {
            failure = AssetDiagnostic{};
            auto shown = service.ImportStatus("/Game/Leaf", status, nullptr, &failure);
            return !shown && shown.Error() == error && failure.code == code &&
                   failure.asset == FixedId(LeafId);
        };
        scene.files.Remove("Content/Leaf.rasset");
        CHECK(again(ErrorCode::NotFound, AssetDiagnosticCode::MissingFile));
        REQUIRE(scene.files.AddFile("Content/Leaf.rasset", sources.base.View()));
        CHECK(again(ErrorCode::StaleHandle, AssetDiagnosticCode::ExternalModification));
        scene.files.Remove("Content/Leaf.rasset");
        REQUIRE(
            scene.files.AddFile("Content/Leaf.rasset", leafBytes.View(), MemoryFault::OpenBusy));
        CHECK(again(ErrorCode::Busy, AssetDiagnosticCode::None));
        scene.files.Remove("Content/Leaf.rasset");
        REQUIRE(scene.files.AddFile("Content/Leaf.rasset",
                                    leafBytes.View().Left(leafBytes.Size() / 2)));
        // 잘린 파일: 섹션 표가 파일 끝 밖을 가리킨다(부분 읽기 CorruptPackage / InvalidArgument).
        CHECK(again(ErrorCode::InvalidArgument, AssetDiagnosticCode::CorruptPackage) &&
              failure.section == 0);
        // 진단 인자 없이도(잘린 파일·사라진 파일).
        CHECK(service.ImportStatus("/Game/Leaf", status).Error() == ErrorCode::InvalidArgument);
        scene.files.Remove("Content/Leaf.rasset");
        CHECK(service.ImportStatus("/Game/Leaf", status).Error() == ErrorCode::NotFound);
        REQUIRE(scene.files.AddFile("Content/Leaf.rasset", leafBytes.View()));
    }

    // 형식 위반 기록·별칭 대상이 색인에 없음, 진단 인자 없이도.
    {
        RecordBuilder broken(env.allocator, ImportRecordSchema, ImportRecordVersion);
        auto brokenBytes = broken.Build();
        REQUIRE(brokenBytes);
        CraftedRecord corrupt;
        corrupt.rawLeafRecord = brokenBytes.Value().View();
        REQUIRE(PutCrafted(env, sources, scene, corrupt));
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        CHECK(service.ImportStatus("/Game/Leaf", status).Error() == ErrorCode::InvalidArgument);
        REQUIRE(PutCrafted(env, sources, scene, CraftedRecord{}));
    }

    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        AssetMoveResult moved;
        REQUIRE(service.Move(scene.lock, "/Game/Villain", "/Game/Gone", moved));
        REQUIRE(service.FlushCache(scene.lock));
        scene.files.Remove("Content/Gone.rasset");
        AssetService after(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(after.Initialize());
        failure = AssetDiagnostic{};
        auto shown = after.ImportStatus("/Game/Villain", status, nullptr, &failure);
        CHECK(!shown && shown.Error() == ErrorCode::NotFound &&
              failure.asset == FixedId(VillainId));
        bool notDurable = false;
        REQUIRE(after.AliasRemove(scene.lock, "/Game/Villain", notDurable));
        REQUIRE(PutCrafted(env, sources, scene, CraftedRecord{}));
    }

    // 원본 루트 표 손상(JSON 문법)은 Load의 오류다.
    REQUIRE(scene.files.AddFile("Config/AssetSources.json", Bytes("{")));
    {
        AssetService service(env.allocator, scene.layout, scene.Config(env));
        REQUIRE(service.Initialize());
        failure = AssetDiagnostic{};
        // JSON 문법 오류의 줄·열(1, 2)이 detail이다((줄 << 32) | 열).
        constexpr uint64 LineShift = 32;
        auto shown = service.ImportStatus("/Game/Leaf", status, nullptr, &failure);
        CHECK(!shown && shown.Error() == ErrorCode::InvalidArgument &&
              failure.code == AssetDiagnosticCode::InvalidSource &&
              failure.detail == ((uint64{1} << LineShift) | 2));
        // 참조 기록은 원본 루트 표를 읽지 않는다.
        CHECK(service.ImportStatus("/Game/Villain", status) && !status.Owned());
    }

    scene.files.Remove("Config/AssetSources.json");
}

// 임포트 상태 할당 실패: 소유 기록(원본 둘)·참조 기록의 모든 할당 실패가 OutOfMemory·진단 None이고
// 성공 결과는 실패 없는 기준과 같다.
TEST(AssetImportStatus_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    Sources sources(env.allocator);
    REQUIRE(MakeSources(env, sources));
    ImportScene scene(env, sources, "import-status-oom");
    REQUIRE(scene.ready);
    REQUIRE(scene.PutSource("SourceArt/px.png", "value=1") &&
            scene.PutSource("SourceArt/nx.png", "value=2"));
    CraftedRecord cube;
    cube.importerId = CubeImporterId;
    cube.importerVersion = 1;
    cube.twoRoles = true;
    REQUIRE(PutCrafted(env, sources, scene, cube));
    const StringView targets[] = {"/Game/Leaf", "/Game/Villain"};
    for (StringView target : targets) {
        AssetImportStatus expected(env.allocator);
        {
            AssetService service(env.allocator, scene.layout, scene.Config(env));
            REQUIRE(service.Initialize());
            REQUIRE(service.ImportStatus(target, expected));
        }

        TaggedFailAllocator failing(env.allocator);
        uint32 failures = 0;
        for (uint64 attempt = 1;; ++attempt) {
            AssetService service(failing, scene.layout, scene.Config(env));
            REQUIRE(service.Initialize());
            AssetImportStatus status(failing);
            AssetDiagnostic failure;
            failing.FailOnAttempt(attempt);
            auto shown = service.ImportStatus(target, status, nullptr, &failure);
            const bool failed = failing.Failed();
            failing.DisableFailure();
            if (!failed) {
                REQUIRE(shown);
                CHECK(status.id == expected.id && status.owner == expected.owner &&
                      status.ownerPath.View() == expected.ownerPath.View() &&
                      status.importerId == expected.importerId &&
                      status.importerVersion == expected.importerVersion &&
                      status.importer == expected.importer &&
                      status.importerName.View() == expected.importerName.View() &&
                      status.sources.Size() == expected.sources.Size());
                for (usize index = 0; index < status.sources.Size(); ++index) {
                    const AssetImportSourceStatus& got = status.sources[index];
                    const AssetImportSourceStatus& want = expected.sources[index];
                    CHECK(got.role.View() == want.role.View() &&
                          got.root.View() == want.root.View() &&
                          got.relative.View() == want.relative.View() && got.state == want.state &&
                          got.error == want.error);
                }

                break;
            }

            CHECK(!shown && shown.Error() == ErrorCode::OutOfMemory &&
                  failure.code == AssetDiagnosticCode::None);
            ++failures;
        }

        CHECK(failures > 0);
    }
}

} // namespace asset_import_tests_file
