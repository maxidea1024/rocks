#include "../../../../../../Source/Developer/AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../../../../../Source/Runtime/AssetRuntime/Tests/AssetTestSupport.h"
#include "ExchangeTestSupport.h"

#include <AssetRuntime/PackageTags.h>
#include <Core/Build/BuildInfo.h>
#include <Core/Commandlet/Commandlet.h>
#include <TestSupport/Test.h>
#include <UnrealExchange/ExchangeCommand.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// asset-import-unreal 명령 시험이다(Docs/Design/UnrealExchange 10.2절, 10.5절 9번): 제품
// Commandlet은 종류별 임포터가 없으므로 명령 함수를 이 프로세스에서 시험 임포터와 함께 부른다.
// 도움말·사용법 오류, dry run(전체·--only·redirect 옛 경로·기존 에셋 재임포트 계획), 임포터 없는
// 실행(건너뜀·경고·색인 캐시), 시험 임포터 실행, 실패 줄(없는 --only 값·묶음 읽기·root·임포터
// 등록·프로젝트), 출력 쓰기 실패.
using namespace rocks;
using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
namespace exchange_command_tests_file {

namespace {

// 표준 출력·오류를 모은다.
struct Captured {
    explicit Captured(Allocator& allocator) noexcept : out(allocator), error(allocator) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        Captured& captured = *static_cast<Captured*>(context);
        captured.writes += 1;
        if ((captured.failWrites && stream == captured.failStream) ||
            captured.writes == captured.failAt) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return (stream == CommandletStream::Output ? captured.out : captured.error).TryAppend(text);
    }

    String out;
    String error;
    bool failWrites = false;
    CommandletStream failStream = CommandletStream::Output;
    // 몇 번째 쓰기(1부터)를 실패시킬지다(0 = 없음).
    uint32 writes = 0;
    uint32 failAt = 0;
};

// 서비스에 쓰지 않는 텍스처 임포터다: 새 에셋은 고정 id, 재임포트는 기존 id를 돌려준다. 받은
// 옵션을 기록하고, 서비스 경고를 하나 남기거나(warn) 진단 없이 실패할 수 있다(failWith).
class FixedKind final : public ExchangeKindImporter {
public:
    Result<AssetId> Import(const ExchangeKindRequest& request) noexcept override {
        calls += 1;
        sawService = request.service != nullptr && request.lock != nullptr;
        normals = request.options->meshNormals;
        if (warn) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::ImportWarning;
            warning.diagnostic.code = AssetDiagnosticCode::UvOutOfRange;
            warning.text[0] = 'W';
            if (auto pushed = request.warnings->TryPushBack(warning); !pushed) {
                return Result<AssetId>::Failure(pushed.Error());
            }
        }

        if (failWith != ErrorCode::None) {
            return Result<AssetId>::Failure(failWith);
        }

        return Result<AssetId>::Success(request.existing.IsNull() ? AssetId{Guid{0xF00D, 1}}
                                                                  : request.existing);
    }

    uint32 calls = 0;
    bool sawService = false;
    ExchangeMeshNormals normals = ExchangeMeshNormals::Unreal;
    bool warn = false;
    ErrorCode failWith = ErrorCode::None;
};

struct KindState {
    FixedKind texture;
    ErrorCode failWith = ErrorCode::None;
};

Result<void> RegisterKinds(void* state, ExchangeKindTable& table) noexcept {
    KindState& kinds = *static_cast<KindState*>(state);
    if (kinds.failWith != ErrorCode::None) {
        return Result<void>::Failure(kinds.failWith);
    }

    return table.Register(ExchangeClass::Texture2D, kinds.texture);
}

// 명령 실행 하나다.
struct Run {
    Run(Allocator& allocator, KindState* kinds = nullptr) noexcept : captured(allocator) {
        assets.allocator = &allocator;
        assets.modules = &test::ProcessModules();
        assets.typeModule = "AssetTestFixtures";
        context.assets = &assets;
        if (kinds != nullptr) {
            context.kinds.registerKinds = &RegisterKinds;
            context.kinds.state = kinds;
        }
    }

    int32 Call(Span<const StringView> args) noexcept {
        CommandletOutput output{&Captured::Write, &captured};
        auto ran = RunAssetImportUnrealCommand(&context, args, output);
        return ran ? ran.Value() : -1;
    }

    bool Out(StringView text) const noexcept {
        return StringView(captured.out.View()).Contains(text, StringComparison::CaseSensitive);
    }

    bool Error(StringView text) const noexcept {
        return StringView(captured.error.View()).Contains(text, StringComparison::CaseSensitive);
    }

    commandlet_host::AssetCommandContext assets;
    UnrealExchangeCommandContext context;
    Captured captured;
};

// 디스크 프로젝트와 fixture 묶음 경로·--project 인자다.
struct CommandScene {
    explicit CommandScene(Allocator& allocator, const char* tag) noexcept
        : root(tag), bundle(allocator), project(allocator) {
        ready = root.made && exchange_test::FixtureSourcePath("Basic.rxbundle", bundle) &&
                project.TryAppendFormat("--project={}", root.Root()) &&
                WriteText("Scene.rocksproject", "{}\n");
    }

    bool WriteText(StringView relative, StringView text) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        if (!builder.TryAppendFormat("{}/{}", root.Root(), relative)) {
            return false;
        }

        platform::File file;
        return static_cast<bool>(file.Open(platform::PhysicalFileSystem(), builder.View().Data(),
                                           builder.View().ByteCount(),
                                           platform::FileOpenMode::CreateAlways)) &&
               static_cast<bool>(file.WriteAll(text.Data(), text.ByteCount()).status) &&
               static_cast<bool>(file.Close());
    }

    bool Exists(StringView relative) noexcept {
        char path[1024] = {};
        FixedStringBuilder builder{Span<char>(path)};
        platform::File file;
        return builder.TryAppendFormat("{}/{}", root.Root(), relative) &&
               static_cast<bool>(file.Open(platform::PhysicalFileSystem(), builder.View().Data(),
                                           builder.View().ByteCount(),
                                           platform::FileOpenMode::ReadExisting));
    }

    TempProject root;
    StringBuilder<512> bundle;
    StringBuilder<512> project;
    bool ready = false;
};

// T_Rock에 UnrealExchange.ObjectPath 태그를 단 기존 에셋을 Content/Kept.rasset에 쓴다.
bool WriteKeptTexture(AssetEnv& env, const TempProject& root) noexcept {
    ObjectRegistry& registry = *env.source;
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(42), PackageKind::Editor);
    if (!made) {
        return false;
    }

    RootRef<Package> package = Move(made.Value());
    if (Make<TestAsset>(registry, TestAssetType(), package.Handle(), "").IsNull()) {
        return false;
    }

    const PackageTagInput tags[] = {{UnrealExchangeObjectPathTag, "/Game/Textures/T_Rock.T_Rock"}};
    auto written = WritePackageTags(env.allocator, Span<const PackageTagInput>(tags));
    if (!written) {
        return false;
    }

    PackageWriteOptions options = EditorOptions();
    options.engineVersion = build::CurrentEngineVersion();
    options.tags = written.Value().View();
    const Array<uint8> bytes = Write(env.allocator, registry, package.Handle(), options);
    return !bytes.IsEmpty() && WriteContentFile(env.allocator, root, "Kept.rasset", bytes.View());
}

// 프로젝트 아래 relative 디렉터리를 만든다.
bool MakeProjectDirectory(const TempProject& root, StringView relative) noexcept {
    char path[1024] = {};
    FixedStringBuilder builder{Span<char>(path)};
    return builder.TryAppendFormat("{}/{}", root.Root(), relative) &&
           platform::MakeDirectoryTree(platform::PhysicalFileSystem(), builder.View().Data(),
                                       builder.View().ByteCount());
}

constexpr StringView TextureCreate =
    "asset-import-unreal: plan-create /Game/Textures/T_Rock.T_Rock "
    "/Game/Unreal/Game/Textures/T_Rock\n";

} // namespace

// 도움말과 사용법 오류(프로젝트를 열기 전에 끝난다).
TEST(UnrealExchangeCommand_Usage) {
    SystemAllocator allocator;
    {
        Run help(allocator);
        const StringView helpArgs[] = {"--help"};
        CHECK(help.Call(helpArgs) == 0 &&
              help.captured.out.View().StartsWith("usage: asset-import-unreal"));

        const StringView none[] = {"--dry-run"};
        const StringView two[] = {"A", "B"};
        const StringView unknown[] = {"A", "--bogus"};
        const StringView unknownValue[] = {"A", "--rootx=/Game"};
        const StringView twoRoots[] = {"A", "--root=/Game/A", "--root=/Game/B"};
        const StringView normals[] = {"A", "--mesh-normals=flat"};
        const StringView emptyOnly[] = {"A", "--only="};
        const StringView emptyProject[] = {"A", "--project="};
        const StringView twoDry[] = {"A", "--dry-run", "--dry-run"};
        const StringView lossy[] = {"A", "--allow-lossy-origin"};
        const StringView emptyBundle[] = {""};
        const StringView upper[] = {"A", "--ROOT=/Game"};
        const StringView bareRoot[] = {"A", "--root"};
        const StringView bareOnly[] = {"A", "--only"};
        const Span<const StringView> cases[] = {
            Span<const StringView>(none),        Span<const StringView>(two),
            Span<const StringView>(unknown),     Span<const StringView>(unknownValue),
            Span<const StringView>(twoRoots),    Span<const StringView>(normals),
            Span<const StringView>(emptyOnly),   Span<const StringView>(emptyProject),
            Span<const StringView>(twoDry),      Span<const StringView>(lossy),
            Span<const StringView>(emptyBundle), Span<const StringView>(upper),
            Span<const StringView>(bareRoot),    Span<const StringView>(bareOnly)};
        for (const Span<const StringView> args : cases) {
            Run run(allocator);
            CHECK(run.Call(args) == 2 && run.captured.error.View().StartsWith("usage:") &&
                  run.captured.out.IsEmpty());
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// dry run: 전체 묶음(순환 메시 둘은 실패, 레벨은 범위 밖), --only(텍스처 하나, redirect 옛 경로의
// 닫힘과 누락 텍스처 경고), 쓰기 없음.
TEST(UnrealExchangeCommand_DryRun) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene(env.allocator, "exchange-dry-run");
    REQUIRE(scene.ready);
    {
        Run run(env.allocator);
        const StringView args[] = {scene.bundle.View(), "--dry-run", scene.project.View(),
                                   "--mesh-normals=rocks"};
        CHECK(run.Call(args) == 1);
        CHECK(run.Out(TextureCreate));
        CHECK(run.Out("asset-import-unreal: plan-create /Game/My Props/_Shared._Shared "
                      "/Game/Unreal/Game/My_Props-"));
        CHECK(
            run.Out("asset-import-unreal: failed /Game/Meshes/SM_Rock.SM_Rock UnsupportedCycle\n"));
        CHECK(run.Out("asset-import-unreal: skipped /Game/Levels/L_Main.L_Main unsupported\n"));
        CHECK(run.Out("asset-import-unreal: 7 to create, 0 to reimport, 2 failed, 1 skipped\n"));
        CHECK(run.Error("asset-import-unreal: UnsupportedCycle: /Game/Meshes/SM_Rock.SM_Rock: "
                        "error HardCycle /Game/Meshes/SM_Rock.SM_Rock\n"));
        CHECK(run.Error("asset-import-unreal: UnrealPathRenamed: /Game/My Props/_Shared._Shared: "
                        "info Renamed /Game/Unreal/Game/My_Props-"));
        CHECK(run.Error("asset-import-unreal: UnsupportedAsset: /Game/Levels/L_Main.L_Main: "
                        "warning UnsupportedClass World\n"));
        CHECK(!scene.Exists("Intermediate/AssetIndex.cache"));
    }

    {
        Run run(env.allocator);
        const StringView args[] = {scene.bundle.View(), "--only=/Game/Textures/T_Rock.T_Rock",
                                   "--dry-run", scene.project.View()};
        CHECK(run.Call(args) == 0);
        CHECK(run.captured.out.View() ==
              StringView("asset-import-unreal: plan-create /Game/Textures/T_Rock.T_Rock "
                         "/Game/Unreal/Game/Textures/T_Rock\n"
                         "asset-import-unreal: 1 to create, 0 to reimport, 0 failed, 0 skipped\n"));
    }

    {
        Run run(env.allocator);
        const StringView args[] = {scene.bundle.View(), "--only=/Game/Old/MI_Old.MI_Old",
                                   "--dry-run", scene.project.View(), "--root=/Game/UE"};
        CHECK(run.Call(args) == 0);
        CHECK(run.Out("asset-import-unreal: plan-create /Game/Materials/MI_Rock.MI_Rock "
                      "/Game/UE/Game/Materials/MI_Rock\n"));
        CHECK(run.Out("asset-import-unreal: 4 to create, 0 to reimport, 0 failed, 0 skipped\n"));
        CHECK(run.Error("asset-import-unreal: MissingUnrealDependency: "
                        "/Game/Materials/M_Base.M_Base: warning MissingTarget "
                        "/Game/Textures/T_Missing.T_Missing\n"));
    }
}

// 기존 에셋: UnrealExchange.ObjectPath 태그를 단 패키지가 있으면 그 경로로 다시 임포트할 계획이다.
TEST(UnrealExchangeCommand_PlansReimport) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene(env.allocator, "exchange-reimport");
    REQUIRE(scene.ready);
    REQUIRE(WriteKeptTexture(env, scene.root));
    Run run(env.allocator);
    const StringView args[] = {scene.bundle.View(), "--only=/Game/Textures/T_Rock.T_Rock",
                               "--dry-run", scene.project.View()};
    CHECK(run.Call(args) == 0);
    CHECK(run.Out("asset-import-unreal: plan-reimport /Game/Textures/T_Rock.T_Rock /Game/Kept "));
    CHECK(run.Out("asset-import-unreal: 0 to create, 1 to reimport, 0 failed, 0 skipped\n"));

    // 실행: 시험 임포터가 기존 id로 재임포트한다. 받은 옵션과 서비스 경고 줄.
    KindState kinds;
    kinds.texture.warn = true;
    Run reimport(env.allocator, &kinds);
    const StringView runArgs[] = {scene.bundle.View(), "--only=/Game/Textures/T_Rock.T_Rock",
                                  scene.project.View(), "--mesh-normals=rocks", "--wait=0"};
    CHECK(reimport.Call(runArgs) == 0);
    CHECK(reimport.Out("asset-import-unreal: reimported /Game/Textures/T_Rock.T_Rock /Game/Kept "));
    CHECK(reimport.Out("asset-import-unreal: 0 created, 1 reimported, 0 failed, 0 skipped\n"));
    CHECK(reimport.Error("asset-import-unreal: UvOutOfRange: W: warning\n"));
    CHECK(kinds.texture.normals == ExchangeMeshNormals::Rocks);
}

// 실행: 임포터가 없으면 건너뛰고(경고), 시험 임포터가 있으면 만든다. 색인 캐시를 쓴다.
TEST(UnrealExchangeCommand_Runs) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene(env.allocator, "exchange-run");
    REQUIRE(scene.ready);
    {
        Run run(env.allocator);
        const StringView args[] = {scene.bundle.View(), "--only=/Game/Textures/T_Rock.T_Rock",
                                   scene.project.View()};
        CHECK(run.Call(args) == 0);
        CHECK(run.captured.out.View() ==
              StringView("asset-import-unreal: skipped /Game/Textures/T_Rock.T_Rock no-importer\n"
                         "asset-import-unreal: 0 created, 0 reimported, 0 failed, 1 skipped\n"));
        CHECK(run.Error("asset-import-unreal: UnsupportedAsset: /Game/Textures/T_Rock.T_Rock: "
                        "warning NoKindImporter Texture2D\n"));
        CHECK(scene.Exists("Intermediate/AssetIndex.cache"));
    }

    {
        KindState kinds;
        Run run(env.allocator, &kinds);
        const StringView args[] = {scene.bundle.View(), "--only=/Game/Textures/T_Rock.T_Rock",
                                   scene.project.View()};
        CHECK(run.Call(args) == 0);
        CHECK(run.Out("asset-import-unreal: created /Game/Textures/T_Rock.T_Rock "
                      "/Game/Unreal/Game/Textures/T_Rock "));
        CHECK(run.Out("asset-import-unreal: 1 created, 0 reimported, 0 failed, 0 skipped\n"));
        CHECK(kinds.texture.calls == 1 && kinds.texture.sawService);
    }

    {
        // 실패한 에셋이 있으면 1(순환 메시). 텍스처만 임포터가 있다(함수·머티리얼·인스턴스는
        // 건너뜀).
        KindState kinds;
        Run run(env.allocator, &kinds);
        const StringView args[] = {scene.bundle.View(), "--only=/Game/Meshes/SM_Rock.SM_Rock",
                                   scene.project.View()};
        CHECK(run.Call(args) == 1);
        CHECK(run.Out("asset-import-unreal: 1 created, 0 reimported, 2 failed, 3 skipped\n"));
    }
}

// 실패 줄: 없는 --only 값, 읽지 못하는 묶음, 잘못된 root, 임포터 등록 실패, 프로젝트 없음.
TEST(UnrealExchangeCommand_Failures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene(env.allocator, "exchange-failures");
    REQUIRE(scene.ready);
    {
        Run run(env.allocator);
        const StringView args[] = {scene.bundle.View(), "--only=/Game/Nope.Nope", "--dry-run",
                                   scene.project.View()};
        CHECK(run.Call(args) == 1);
        CHECK(run.Error("asset-import-unreal: NotFound: /Game/Nope.Nope: not in the bundle\n"));
    }

    {
        StringBuilder<512> missing(env.allocator);
        REQUIRE(missing.TryAppendFormat("{}/NoBundle.rxbundle", scene.root.Root()));
        Run run(env.allocator);
        const StringView args[] = {missing.View(), "--dry-run", scene.project.View()};
        CHECK(run.Call(args) == 1);
        CHECK(run.Error("asset-import-unreal: CorruptBundle: bundle: error ReadFailed") &&
              run.Error(": failed to read the bundle\n"));
    }

    {
        Run run(env.allocator);
        const StringView args[] = {scene.bundle.View(), "--root=Bad", "--dry-run",
                                   scene.project.View()};
        CHECK(run.Call(args) == 1);
        CHECK(run.Error("asset-import-unreal: InvalidPath: bundle: error InvalidRoot Bad\n") &&
              run.Error("asset-import-unreal: InvalidArgument: Bad: planning failed\n"));
    }

    {
        KindState kinds;
        kinds.failWith = ErrorCode::Busy;
        Run run(env.allocator, &kinds);
        const StringView args[] = {scene.bundle.View(), "--dry-run", scene.project.View()};
        CHECK(run.Call(args) == 1);
        CHECK(run.Error("asset-import-unreal: Busy: kind importer registration failed\n"));
    }

    {
        Run run(env.allocator);
        const StringView args[] = {scene.bundle.View(), "--dry-run",
                                   "--project=rocks-exchange-no-such-project"};
        CHECK(run.Call(args) == 1 && !run.captured.error.IsEmpty() && run.captured.out.IsEmpty());
    }

    {
        // 출력 쓰기 실패는 명령 오류다(표준 출력·표준 오류).
        Run out(env.allocator);
        out.captured.failWrites = true;
        const StringView args[] = {scene.bundle.View(), "--dry-run", scene.project.View()};
        CHECK(out.Call(args) == -1);
        Run error(env.allocator);
        error.captured.failWrites = true;
        error.captured.failStream = CommandletStream::Error;
        CHECK(error.Call(args) == -1);
    }
}

// --wait 값 검사(사용법 오류)와 받는 값, --mesh-normals=unreal.
TEST(UnrealExchangeCommand_OptionValues) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene(env.allocator, "exchange-options");
    REQUIRE(scene.ready);
    const StringView badWaits[] = {"--wait=abc", "--wait=86401", "--wait=-1"};
    for (const StringView wait : badWaits) {
        Run run(env.allocator);
        const StringView args[] = {scene.bundle.View(), wait, "--dry-run", scene.project.View()};
        CHECK(run.Call(args) == 2 && run.captured.error.View().StartsWith("usage:"));
    }

    Run run(env.allocator);
    const StringView args[] = {scene.bundle.View(),
                               "--wait=5",
                               "--mesh-normals=unreal",
                               "--only=/Game/Textures/T_Rock.T_Rock",
                               "--dry-run",
                               scene.project.View()};
    CHECK(run.Call(args) == 0 && run.Out(TextureCreate));
}

// 실행 결과 줄: 진단 없는 임포터 실패(오류 이름), 다른 쪽이 잠금을 쥐면 dry run은 되고 실행은 실패,
// 색인 캐시를 쓰지 못하면 경고.
TEST(UnrealExchangeCommand_RunOutcomes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene(env.allocator, "exchange-outcomes");
    REQUIRE(scene.ready);
    const StringView args[] = {scene.bundle.View(), "--only=/Game/Textures/T_Rock.T_Rock",
                               scene.project.View()};
    {
        KindState kinds;
        kinds.texture.failWith = ErrorCode::Busy;
        Run run(env.allocator, &kinds);
        CHECK(run.Call(args) == 1);
        CHECK(run.Out("asset-import-unreal: failed /Game/Textures/T_Rock.T_Rock Busy\n"));
        CHECK(run.Out("asset-import-unreal: 0 created, 0 reimported, 1 failed, 0 skipped\n"));
    }

    {
        ProjectLayout layout(env.allocator);
        REQUIRE(layout.Initialize(scene.root.Root()));
        AssetDatabaseLock lock;
        REQUIRE(lock.Acquire(layout, 0));
        Run dry(env.allocator);
        const StringView dryArgs[] = {scene.bundle.View(), "--only=/Game/Textures/T_Rock.T_Rock",
                                      "--dry-run", scene.project.View()};
        CHECK(dry.Call(dryArgs) == 0 && dry.Out(TextureCreate));
        Run locked(env.allocator);
        CHECK(locked.Call(args) == 1 && locked.Error("Busy") && locked.captured.out.IsEmpty());
    }

    {
        // 색인 캐시 파일 자리에 디렉터리가 있으면 쓰지 못한다(경고, 종료 코드는 그대로). 앞 실행이
        // 캐시를 쓰지 않은 새 프로젝트다.
        CommandScene cache(env.allocator, "exchange-cache");
        REQUIRE(cache.ready);
        REQUIRE(MakeProjectDirectory(cache.root, "Intermediate/AssetIndex.cache"));
        Run run(env.allocator);
        const StringView cacheArgs[] = {cache.bundle.View(), "--only=/Game/Textures/T_Rock.T_Rock",
                                        cache.project.View()};
        CHECK(run.Call(cacheArgs) == 0);
        CHECK(run.Error(": warning CacheNotWritten\n"));
    }
}

// 진단이 상한(4096)을 넘으면 버린 수를 한 줄로 쓴다.
TEST(UnrealExchangeCommand_DroppedDiagnostics) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene(env.allocator, "exchange-dropped");
    REQUIRE(scene.ready);
    // 범위 밖 클래스 에셋 4097개(각자 UnsupportedAsset 경고)인 묶음이다.
    constexpr uint32 Count = UnrealExchangeMaxDiagnostics + 1;
    String manifest(env.allocator);
    REQUIRE(manifest.TryAppend(
        "{\"formatVersion\": 1, \"exporter\": {\"plugin\": \"RocksExport\", \"pluginVersion\": "
        "\"1.0.0\", \"engineVersion\": \"5.8.4\", \"engineCommit\": \"0\"}, \"projectSettings\": "
        "{\"substrate\": false, \"retainJpegFormat\": false}, \"assets\": ["));
    for (uint32 index = 0; index < Count; ++index) {
        // 같은 자릿수(10000부터)라 byte 순이 번호 순이다.
        char number[16] = {};
        FixedStringBuilder name{Span<char>(number)};
        REQUIRE(name.TryAppendFormat("L{}", 10000 + index));
        REQUIRE((index == 0 || manifest.TryAppend(", ")) &&
                manifest.TryAppend("{\"objectPath\": \"/Game/W/") &&
                manifest.TryAppend(name.View()) && manifest.TryAppend(".") &&
                manifest.TryAppend(name.View()) &&
                manifest.TryAppend("\", \"class\": \"World\", \"file\": \"assets/Game/W/") &&
                manifest.TryAppend(name.View()) && manifest.TryAppend(".json\"}"));
    }

    REQUIRE(manifest.TryAppend("], \"redirects\": []}\n"));
    REQUIRE(MakeProjectDirectory(scene.root, "Many.rxbundle"));
    REQUIRE(scene.WriteText("Many.rxbundle/bundle.json", manifest.View()));
    StringBuilder<512> bundle(env.allocator);
    REQUIRE(bundle.TryAppendFormat("{}/Many.rxbundle", scene.root.Root()));
    Run run(env.allocator);
    const StringView args[] = {bundle.View(), "--dry-run", scene.project.View()};
    CHECK(run.Call(args) == 0);
    CHECK(run.Error("asset-import-unreal: dropped 1 diagnostics\n"));
}

// 출력 쓰기 실패: 쓰기 하나하나를 실패시키면 명령 오류(-1)이고, 쓰기 수를 넘으면 정상 종료다.
TEST(UnrealExchangeCommand_EachWriteFailure) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene(env.allocator, "exchange-writes");
    REQUIRE(scene.ready);
    REQUIRE(WriteKeptTexture(env, scene.root));
    REQUIRE(MakeProjectDirectory(scene.root, "Intermediate/AssetIndex.cache"));
    const StringView args[] = {scene.bundle.View(), "--only=/Game/Old/MI_Old.MI_Old",
                               "--only=/Game/Textures/T_Rock.T_Rock", scene.project.View()};
    uint32 failures = 0;
    for (uint32 failAt = 1;; ++failAt) {
        KindState kinds;
        kinds.texture.warn = true;
        Run run(env.allocator, &kinds);
        run.captured.failAt = failAt;
        const int32 code = run.Call(args);
        if (code == -1) {
            ++failures;
            continue;
        }

        CHECK(code == 0 && run.captured.writes < failAt);
        break;
    }

    CHECK(failures > 5);

    // 단계 실패(잘못된 root)의 진단·실패 줄 쓰기 실패.
    const StringView badRoot[] = {scene.bundle.View(), "--root=Bad", "--dry-run",
                                  scene.project.View()};
    uint32 stageFailures = 0;
    for (uint32 failAt = 1;; ++failAt) {
        Run run(env.allocator);
        run.captured.failAt = failAt;
        const int32 code = run.Call(badRoot);
        if (code == -1) {
            ++stageFailures;
            continue;
        }

        CHECK(code == 1);
        break;
    }

    CHECK(stageFailures >= 2);
}

} // namespace exchange_command_tests_file
