#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/AssetImportTestSupport.h"

#include <AssetCommandlets/AssetCommandletSupport.h>
#include <AssetCommandlets/AssetEditCommandlet.h>
#include <Core/Commandlet/Commandlet.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>

// asset-import 명령 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 18 A2f1b, 13절): 제품 실행 파일은
// 실제 임포터만 등록하므로 명령 함수를 이 프로세스에서 부른다 — 문맥이 시험 타입 모듈
// (AssetTestFixtures)과 시험 임포터를 준다. 성공 줄·출력 줄·경고 줄·캐시 쓰기, 역할 원본, 상대 원본
// 경로(작업 디렉터리 기준), 실패 줄(원본 없음·옵션 오류·임포터 없음·등록 실패), 사용법·도움말.
using namespace rocks;
namespace asset_import_commandlet_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::import_test;
using namespace rocks::commandlet_host;

namespace {

// 표준 출력·오류를 모은다.
struct Captured {
    explicit Captured(Allocator& allocator) noexcept : out(allocator), error(allocator) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        Captured& captured = *static_cast<Captured*>(context);
        if (stream == captured.failStream && captured.failWrites) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return (stream == CommandletStream::Output ? captured.out : captured.error).TryAppend(text);
    }

    CommandletOutput Output() noexcept {
        return CommandletOutput{&Captured::Write, this};
    }

    String out;
    String error;
    // 참이면 failStream 쓰기가 Busy로 실패한다(출력 실패 시험).
    bool failWrites = false;
    CommandletStream failStream = CommandletStream::Output;
};

// 시험 임포터 등록 상태다(failWith가 None이 아니면 등록이 그 오류로 실패한다).
struct ImporterState {
    TestImporter test{TestImporterInfo()};
    TestImporter cube{CubeImporterInfo()};
    ErrorCode failWith = ErrorCode::None;
};

Result<void> RegisterTestImporters(void* state, AssetImporterTable& table,
                                   Array<AssetImporterRegistration>& tokens) noexcept {
    ImporterState& importers = *static_cast<ImporterState*>(state);
    if (importers.failWith != ErrorCode::None) {
        return Result<void>::Failure(importers.failWith);
    }

    TestImporter* const all[] = {&importers.test, &importers.cube};
    for (TestImporter* importer : all) {
        auto registered = table.Register(*importer);
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }

        if (auto pushed = tokens.TryPushBack(rocks::Move(registered.Value())); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 디스크 프로젝트(작업 디렉터리 아래 상대 이름 — `*.rocksproject`)와 원본 파일이다.
struct CommandScene {
    explicit CommandScene(const char* tag) noexcept : root(tag) {
        ready = root.made && WriteFile("Scene.rocksproject", "{}\n");
    }

    bool WriteFile(StringView relative, StringView text) noexcept {
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
    bool ready = false;
};

struct Run {
    Run(Allocator& allocator, ImporterState* state) noexcept : captured(allocator) {
        context.allocator = &allocator;
        context.modules = &test::ProcessModules();
        context.typeModule = "AssetTestFixtures";
        if (state != nullptr) {
            context.importers.registerImporters = &RegisterTestImporters;
            context.importers.state = state;
        }
    }

    int32 Call(Span<const StringView> args) noexcept {
        CommandletOutput output = captured.Output();
        auto ran = RunAssetImportCommand(&context, args, output);
        return ran ? ran.Value() : -1;
    }

    int32 Reimport(Span<const StringView> args) noexcept {
        CommandletOutput output = captured.Output();
        auto ran = RunAssetReimportCommand(&context, args, output);
        return ran ? ran.Value() : -1;
    }

    AssetCommandContext context;
    Captured captured;
};

} // namespace

// 성공: 주 출력 줄·추가 출력 줄, 경고 줄(ImportWarning), 옵션·설정, 패키지 파일과 색인
// 캐시, 역할 원본(--source.<역할>=·--importer=), 상대 원본 경로.
TEST(AssetImportCommand_Imports) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene("import-command");
    REQUIRE(scene.ready);
    String text(env.allocator);
    char warn[32] = {};
    FixedStringBuilder warnText{Span<char>(warn)};
    REQUIRE(warnText.TryAppendFormat("{}", static_cast<uint32>(AssetDiagnosticCode::UvOutOfRange)));
    REQUIRE(text.TryAppend("value=2\noutputs=extra\nwarn=") && text.TryAppend(warnText.View()));
    REQUIRE(scene.WriteFile("Rock.trock", text.View()) && scene.WriteFile("px.png", "value=1") &&
            scene.WriteFile("nx.png", "value=2"));
    ImporterState state;
    Run run(env.allocator, &state);
    char project[512] = {};
    FixedStringBuilder projectOption{Span<char>(project)};
    REQUIRE(projectOption.TryAppendFormat("--project={}", scene.root.Root()));
    char source[512] = {};
    FixedStringBuilder sourcePath{Span<char>(source)};
    REQUIRE(sourcePath.TryAppendFormat("{}/Rock.trock", scene.root.Root()));
    const StringView args[] = {"/Game/Rock", sourcePath.View(), "--scale=3",
                               "--label=Hi", "--note=Remember", projectOption.View()};
    CHECK(run.Call(args) == 0);
    CHECK(run.captured.out.View().StartsWith("asset-import: /Game/Rock ") &&
          StringView(run.captured.out.View())
              .Contains("asset-import: output extra /Game/Rock_extra "));
    CHECK(StringView(run.captured.error.View()).Contains("warning ImportWarning"));
    CHECK(scene.Exists("Content/Rock.rasset") && scene.Exists("Content/Rock_extra.rasset") &&
          scene.Exists("Intermediate/AssetIndex.cache") && state.test.calls == 1);

    // 역할 원본(이름으로 고르는 임포터 — 상대 원본 경로는 작업 디렉터리 기준).
    char px[512] = {};
    char nx[512] = {};
    FixedStringBuilder pxOption{Span<char>(px)};
    FixedStringBuilder nxOption{Span<char>(nx)};
    REQUIRE(pxOption.TryAppendFormat("--source.px={}/px.png", scene.root.Root()) &&
            nxOption.TryAppendFormat("--source.nx={}/nx.png", scene.root.Root()));
    Run cube(env.allocator, &state);
    const StringView cubeArgs[] = {"/Game/Cube", pxOption.View(), nxOption.View(),
                                   "--importer=CubeImporter", projectOption.View()};
    CHECK(cube.Call(cubeArgs) == 0);
    CHECK(cube.captured.out.View().StartsWith("asset-import: /Game/Cube ") &&
          cube.captured.error.IsEmpty() && scene.Exists("Content/Cube.rasset"));
}

// 실패 줄(종료 1): 원본 없음 MissingSource, 옵션 값 오류, 설정 대입 오류(진단 줄), 임포터 없음(등록
// hook 없음 — 제품과 같다), 등록 실패, 잠금 경로는 다른 명령과 같다.
TEST(AssetImportCommand_Failures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene("import-command-fail");
    REQUIRE(scene.ready);
    REQUIRE(scene.WriteFile("Rock.trock", "value=1"));
    char project[512] = {};
    FixedStringBuilder projectOption{Span<char>(project)};
    REQUIRE(projectOption.TryAppendFormat("--project={}", scene.root.Root()));
    char source[512] = {};
    FixedStringBuilder sourcePath{Span<char>(source)};
    REQUIRE(sourcePath.TryAppendFormat("{}/Rock.trock", scene.root.Root()));
    ImporterState state;
    {
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", "missing-file.trock", projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View())
                  .Contains("asset-import: MissingSource: /Game/Rock"));
    }

    {
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", sourcePath.View(), "--scale=abc",
                                   projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View()).Contains("asset-import: "));
    }

    // 설정 대입 오류(Optional이 아닌 속성의 --unset — 대입 진단 줄).
    {
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", sourcePath.View(), "--unset=note",
                                   projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View()).Contains("asset-import: InvalidValue: note"));
    }

    {
        Run run(env.allocator, nullptr);
        const StringView args[] = {"/Game/Rock", sourcePath.View(), projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View())
                  .Contains("asset-import: UnsupportedSourceFormat: /Game/Rock"));
    }

    {
        state.failWith = ErrorCode::Busy;
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", sourcePath.View(), projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View())
                  .Contains("asset-import: Busy: cannot register the importers"));
        state.failWith = ErrorCode::None;
    }

    // 절대 원본 경로(작업 디렉터리를 쓰지 않는다)·루트 위로 벗어나는 상대 경로·--erase.
    {
        Run run(env.allocator, &state);
        const StringView absolute = IsWindows ? StringView("C:/rocks-missing/Rock.trock")
                                              : StringView("/rocks-missing/Rock.trock");
        const StringView args[] = {"/Game/Rock", absolute, projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View())
                  .Contains("asset-import: MissingSource: /Game/Rock"));
    }

    {
        Run run(env.allocator, &state);
        StringBuilder<512> escape(env.allocator);
        for (usize index = 0; index < 64; ++index) {
            REQUIRE(escape.TryAppend("../"));
        }

        REQUIRE(escape.TryAppend("Rock.trock"));
        const StringView args[] = {"/Game/Rock", escape.View(), projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View()).Contains("asset-import: InvalidSource: "));
    }

    {
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", sourcePath.View(), "--erase=missing",
                                   projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View())
                  .Contains("asset-import: UnknownProperty: /Game/Rock"));
    }

    // 타입 모듈을 시작하지 못하면 그 모듈 이름을 쓴다.
    {
        Run run(env.allocator, &state);
        run.context.typeModule = "NoSuchAssetTypeModule";
        const StringView args[] = {"/Game/Rock", sourcePath.View(), projectOption.View()};
        CHECK(run.Call(args) == 1);
        CHECK(StringView(run.captured.error.View())
                  .Contains("NoSuchAssetTypeModule: cannot register the asset types"));
    }

    // 등록 hook의 할당 실패는 실행 오류다.
    {
        state.failWith = ErrorCode::OutOfMemory;
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", sourcePath.View(), projectOption.View()};
        CHECK(run.Call(args) == -1);
        state.failWith = ErrorCode::None;
    }

    CHECK(!scene.Exists("Content/Rock.rasset"));

    // 출력 쓰기 실패: 결과 줄(표준 출력)·경고 줄(표준 오류)이 쓰이지 않으면 실행 오류다.
    REQUIRE(scene.WriteFile("Warned.trock", "value=1\nwarn="));
    {
        Run run(env.allocator, &state);
        run.captured.failWrites = true;
        run.captured.failStream = CommandletStream::Output;
        const StringView args[] = {"/Game/Rock", sourcePath.View(), projectOption.View()};
        CHECK(run.Call(args) == -1);
    }

    {
        char text[32] = {};
        FixedStringBuilder warn{Span<char>(text)};
        REQUIRE(warn.TryAppendFormat("value=1\nwarn={}",
                                     static_cast<uint32>(AssetDiagnosticCode::UvOutOfRange)));
        REQUIRE(scene.WriteFile("Warned.trock", warn.View()));
        char warnedPath[512] = {};
        FixedStringBuilder warned{Span<char>(warnedPath)};
        REQUIRE(warned.TryAppendFormat("{}/Warned.trock", scene.root.Root()));
        Run run(env.allocator, &state);
        run.captured.failWrites = true;
        run.captured.failStream = CommandletStream::Error;
        const StringView args[] = {"/Game/Warned", warned.View(), projectOption.View()};
        CHECK(run.Call(args) == -1);
    }
}

// 사용법(종료 2)과 도움말(종료 0).
TEST(AssetImportCommand_Usage) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ImporterState state;
    const StringView noArgs[] = {"--wait=1"};
    const StringView onlyPath[] = {"/Game/A"};
    const StringView both[] = {"/Game/A", "a.trock", "--source.px=b"};
    const StringView threePositional[] = {"/Game/A", "a", "b"};
    const StringView emptyPositional[] = {""};
    const StringView sourceNoValue[] = {"/Game/A", "--source.px"};
    const StringView sourceEmptyRole[] = {"/Game/A", "--source.=x"};
    const StringView sourceEmptyFile[] = {"/Game/A", "--source.px="};
    const StringView sourceTwice[] = {"/Game/A", "--source.px=a", "--source.px=b"};
    const StringView importerEmpty[] = {"/Game/A", "a.trock", "--importer="};
    const StringView importerTwice[] = {"/Game/A", "a.trock", "--importer=x", "--importer=y"};
    const StringView projectTwice[] = {"/Game/A", "a.trock", "--project=a", "--project=b"};
    const StringView reserved[] = {"/Game/A", "a.trock", "--cube=x"};
    const StringView reservedCase[] = {"/Game/A", "a.trock", "--Force=1"};
    const StringView noValue[] = {"/Game/A", "a.trock", "--force"};
    const StringView badUnset[] = {"/Game/A", "a.trock", "--unset="};
    const StringView emptySetting[] = {"/Game/A", "a.trock", "--=1"};
    const Span<const StringView> cases[] = {noArgs,          onlyPath,        both,
                                            threePositional, emptyPositional, sourceNoValue,
                                            sourceEmptyRole, sourceEmptyFile, sourceTwice,
                                            importerEmpty,   importerTwice,   projectTwice,
                                            reserved,        reservedCase,    noValue,
                                            badUnset,        emptySetting};
    for (Span<const StringView> args : cases) {
        Run run(env.allocator, &state);
        CHECK(run.Call(args) == 2);
        CHECK(run.captured.error.View().StartsWith("usage: asset-import"));
    }

    Run help(env.allocator, &state);
    const StringView helpArgs[] = {"--help"};
    CHECK(help.Call(helpArgs) == 0 && help.captured.out.View().StartsWith("usage: asset-import"));
    CHECK(state.test.calls == 0);
}

// asset-reimport(7.1절 18 A2f2a): 임포트한 에셋을 기록대로 다시(결과 줄·출력 줄), `--source=` 원본
// 바꾸기와 설정, 실패 줄(기록 없음 InvalidArgument·원본 없음 MissingSource), 사용법·도움말.
TEST(AssetReimportCommand_Reimports) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene("reimport-command");
    REQUIRE(scene.ready);
    REQUIRE(scene.WriteFile("Rock.trock", "value=2\noutputs=extra") &&
            scene.WriteFile("Other.trock", "value=5\noutputs=extra"));
    ImporterState state;
    char project[512] = {};
    FixedStringBuilder projectOption{Span<char>(project)};
    REQUIRE(projectOption.TryAppendFormat("--project={}", scene.root.Root()));
    char source[512] = {};
    FixedStringBuilder sourcePath{Span<char>(source)};
    REQUIRE(sourcePath.TryAppendFormat("{}/Rock.trock", scene.root.Root()));
    {
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", sourcePath.View(), projectOption.View()};
        REQUIRE(run.Call(args) == 0);
    }

    {
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", projectOption.View()};
        CHECK(run.Reimport(args) == 0);
        CHECK(run.captured.out.View().StartsWith("asset-reimport: /Game/Rock ") &&
              StringView(run.captured.out.View())
                  .Contains("asset-reimport: output extra /Game/Rock_extra ") &&
              run.captured.error.IsEmpty() && state.test.lastReimport);
    }

    {
        char other[512] = {};
        FixedStringBuilder otherOption{Span<char>(other)};
        REQUIRE(otherOption.TryAppendFormat("--source={}/Other.trock", scene.root.Root()));
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", otherOption.View(), "--scale=2",
                                   projectOption.View()};
        CHECK(run.Reimport(args) == 0);
    }

    // 실패 줄: 기록 없는 에셋(시험 임포터가 만든 추가 출력 — 참조 기록), 원본 없음.
    {
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock_extra", projectOption.View()};
        CHECK(run.Reimport(args) == 1);
        CHECK(StringView(run.captured.error.View())
                  .Contains("asset-reimport: InvalidArgument: /Game/Rock_extra"));
    }

    {
        Run run(env.allocator, &state);
        const StringView args[] = {"/Game/Rock", "--source=missing-file.trock",
                                   projectOption.View()};
        CHECK(run.Reimport(args) == 1);
        CHECK(StringView(run.captured.error.View())
                  .Contains("asset-reimport: MissingSource: /Game/Rock"));
    }

    // 사용법(종료 2)·도움말.
    const StringView noPath[] = {"--wait=1"};
    const StringView positionalSource[] = {"/Game/A", "a.trock"};
    const StringView importer[] = {"/Game/A", "--importer=x"};
    const StringView sourceTwice[] = {"/Game/A", "--source=a", "--source=b"};
    const StringView mixed[] = {"/Game/A", "--source=a", "--source.px=b"};
    const StringView mixedReverse[] = {"/Game/A", "--source.px=b", "--source=a"};
    const StringView emptySource[] = {"/Game/A", "--source="};
    const Span<const StringView> cases[] = {noPath, positionalSource, importer,   sourceTwice,
                                            mixed,  mixedReverse,     emptySource};
    for (Span<const StringView> args : cases) {
        Run run(env.allocator, &state);
        CHECK(run.Reimport(args) == 2);
        CHECK(run.captured.error.View().StartsWith("usage: asset-reimport"));
    }

    Run help(env.allocator, &state);
    const StringView helpArgs[] = {"--help"};
    CHECK(help.Reimport(helpArgs) == 0 &&
          help.captured.out.View().StartsWith("usage: asset-reimport"));
}

} // namespace asset_import_commandlet_tests_file
