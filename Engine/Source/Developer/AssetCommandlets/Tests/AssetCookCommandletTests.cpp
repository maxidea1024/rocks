#include "AssetCommandletTestSupport.h"

#include <AssetCommandlets/AssetCookCommandlet.h>
#include <AssetCommandlets/AssetEditCommandlet.h>
#include <AssetCooker/CookConfig.h>
#include <AssetNugget/NuggetCacheSettings.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>

// asset-cook 명령 시험이다(Docs/Design/AssetSystem 13절, 10.2절 "A4e 구현 세부" — A4e1):
// 명령 함수를 이 프로세스에서 부른다 — 문맥이 시험 타입 모듈(AssetTestFixtures)과 시험 임포터를
// 주고, 에셋은 asset-import로 만든다. 엔진 설정은 저장소 `Engine/Config`(FindEngineDirectory)다.
// 전체·재사용·verify·JSON·부분 쿠킹, 진단 줄(경로·AssetId·related·alwaysCook 순번·인자 경로·
// 원인 오류), 설정 거부(프로파일·쿠킹 규칙·너겟 설정 칸마다), 너겟 캐시 끔 경고·required 실패,
// 쿠킹 함수 오류, 출력 실패, 사용법.
using namespace rocks;
namespace asset_cook_commandlet_tests_file {

using namespace rocks::asset_test;
using namespace rocks::commandlet_host;
using namespace rocks::commandlet_test;

namespace {

struct Run {
    Run(Allocator& allocator, ImporterState& state) noexcept : captured(allocator) {
        context.allocator = &allocator;
        context.modules = &test::ProcessModules();
        context.typeModule = "AssetTestFixtures";
        context.importers.registerImporters = &RegisterTestImporters;
        context.importers.state = &state;
    }

    int32 Cook(Span<const StringView> args) noexcept {
        captured.out.Clear();
        captured.error.Clear();
        CommandletOutput output = captured.Output();
        auto ran = RunAssetCookCommand(&context, args, output);
        return ran ? ran.Value() : -1;
    }

    int32 Import(Span<const StringView> args) noexcept {
        CommandletOutput output = captured.Output();
        auto ran = RunAssetImportCommand(&context, args, output);
        return ran ? ran.Value() : -1;
    }

    bool Out(StringView part) const noexcept {
        return StringView(captured.out.View()).Contains(part);
    }

    bool Error(StringView part) const noexcept {
        return StringView(captured.error.View()).Contains(part);
    }

    AssetCommandContext context;
    Captured captured;
};

constexpr StringView AlwaysAll = "{\"alwaysCook\": [\"/Game/\"]}\n";

// 쿠킹할 프로젝트다: Rock(Hard 참조 → Rock_extra)과 Rock_extra, 쿠킹 규칙(전부).
struct CookScene {
    CookScene(Allocator& allocator, const char* tag) noexcept
        : scene(tag), run(allocator, state), project(Span<char>(projectText)),
          source(Span<char>(sourceText)) {
        ready = scene.ready && project.TryAppendFormat("--project={}", scene.root.Root()) &&
                source.TryAppendFormat("{}/Rock.trock", scene.root.Root()) &&
                scene.WriteFile("Rock.trock", "value=2\noutputs=extra\nreference=extra") &&
                scene.MakeDirectory("Config") && Settings(AlwaysAll);
        const StringView args[] = {"/Game/Rock", source.View(), project.View()};
        ready = ready && run.Import(args) == 0;
    }

    bool Settings(StringView text) noexcept {
        return scene.WriteFile("Config/CookSettings.json", text);
    }

    // `--profile=Windows`와 프로젝트 옵션에 extra를 더해 쿠킹한다.
    int32 Cook(std::initializer_list<StringView> extra = {}) noexcept {
        StringView args[8] = {"--profile=Windows", project.View()};
        usize count = 2;
        for (StringView one : extra) {
            args[count++] = one;
        }

        return run.Cook(Span<const StringView>(args, count));
    }

    CommandScene scene;
    ImporterState state;
    Run run;
    char projectText[512] = {};
    char sourceText[512] = {};
    FixedStringBuilder project;
    FixedStringBuilder source;
    bool ready = false;
};

} // namespace

// 성공: 전체 쿠킹(게시·요약 줄·매니페스트·보고서), 두 번째는 전부 재사용, verify는 재사용 없이
// 모두 쿠킹(source local 포함), --json은 보고서 내용, 부분 쿠킹 표지.
TEST(AssetCookCommand_CooksAndReuses) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command");
    REQUIRE(cook.ready);
    CHECK(cook.Cook() == 0);
    CHECK(cook.run.Out("asset-cook: Windows published targets 2 cooked 2 reused 0 bytes ") &&
          cook.run.captured.error.IsEmpty());
    CHECK(!cook.run.Out(" partial") && !cook.run.Out("bytes 0\n"));
    CHECK(cook.scene.Exists("Saved/Cooked/Windows/CookedManifest.rman") &&
          cook.scene.Exists("Saved/Cooked/Windows/CookReport.json"));

    CHECK(cook.Cook() == 0);
    CHECK(cook.run.Out("asset-cook: Windows published targets 2 cooked 0 reused 2 bytes 0\n"));

    CHECK(cook.Cook({"--verify-nuggets"}) == 0);
    CHECK(cook.run.Out("published targets 2 cooked 2 reused 0 bytes "));
    CHECK(cook.Cook({"--verify-nuggets", "--verify-nuggets-source=local"}) == 0);
    CHECK(cook.run.Out("published targets 2 cooked 2 reused 0 bytes "));

    CHECK(cook.Cook({"--json"}) == 0);
    CHECK(cook.run.captured.out.View().StartsWith("{") && cook.run.Out("\"published\": true") &&
          cook.run.Out("\"reused\": 2") && cook.run.Out("\"profile\": \"Windows\"") &&
          !cook.run.Out("asset-cook:"));

    CHECK(cook.Cook({"/Game/Rock"}) == 0);
    CHECK(cook.run.Out("published targets 2 cooked 0 reused 2 bytes 0 partial\n"));
}

// 진단 줄: alwaysCook 순번 경고(게시 0), NeverCook 우선 경고(에셋 경로), 인자 경로 오류(게시하지
// 않음 1·--json도 1), 사라진 Hard 의존(related AssetId — 색인에 없음), 쿠킹 함수 오류.
TEST(AssetCookCommand_Diagnostics) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-diag");
    REQUIRE(cook.ready);
    // 기존 매니페스트가 없으면 인자가 있어도 전체 쿠킹이라 경로 진단 순번은 alwaysCook 순번이다
    // (인자 하나 — 순번 1로 인자를 읽지 않는다, 리뷰 필수).
    REQUIRE(cook.Settings("{\"alwaysCook\": [\"/Game/\", \"/Game/Missing\"]}"));
    CHECK(cook.Cook({"/Game/Rock"}) == 0);
    CHECK(cook.run.Error("asset-cook: InvalidPath: alwaysCook[1]: warning\n") &&
          cook.run.Out("published targets 2") && !cook.run.Out(" partial"));
    CHECK(cook.Cook() == 0);
    CHECK(cook.run.Error("asset-cook: InvalidPath: alwaysCook[1]: warning\n"));

    REQUIRE(cook.Settings("{\"alwaysCook\": [\"/Game/Rock\"], \"neverCook\": [\"/Game/Rock\"]}"));
    CHECK(cook.Cook() == 0);
    CHECK(cook.run.Error("asset-cook: NeverCookReferenced: /Game/Rock: warning\n"));

    // 게시본의 쿠킹 규칙이 다르면 부분 쿠킹은 쿠킹 전에 IncompatibleCook이다(대상 루트 줄).
    REQUIRE(cook.Settings(AlwaysAll));
    CHECK(cook.Cook({"/Game/Rock"}) == 1);
    CHECK(cook.run.Error("asset-cook: IncompatibleCook: ") &&
          cook.run.Error("Saved/Cooked/Windows: error Unsupported\n") &&
          cook.run.Out("not published targets 0 cooked 0 reused 0 bytes 0 partial\n"));
    CHECK(cook.Cook() == 0);
    CHECK(cook.Cook({"/Game/Nope"}) == 1);
    CHECK(cook.run.Error("asset-cook: InvalidPath: /Game/Nope: error\n") &&
          cook.run.Out("asset-cook: Windows not published targets "));
    CHECK(cook.Cook({"/Game/Nope", "--json"}) == 1);
    CHECK(cook.run.Out("\"published\": false") && cook.run.Error("InvalidPath: /Game/Nope"));

    // Rock_extra를 지우면 Rock의 Hard 의존이 색인에 없다(related는 AssetId 표준 문자열).
    REQUIRE(cook.scene.RemoveFile("Content/Rock_extra.rasset"));
    CHECK(cook.Cook() == 1);
    CHECK(cook.run.Error("asset-cook: MissingDependency: /Game/Rock: error (related ") &&
          cook.run.Out("not published"));
}

// 같은 AssetId 파일 둘은 오류 DuplicateAssetId이고 대상은 AssetId 표준 문자열이다(색인에 고유하지
// 않다). 대상 계산이 Rock을 두 번 보고(alwaysCook 뿌리·기존 매니페스트 항목) 줄도 둘이다.
TEST(AssetCookCommand_DuplicateId) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-dup");
    REQUIRE(cook.ready);
    REQUIRE(cook.Cook() == 0);
    REQUIRE(cook.scene.CopyFile("Content/Rock.rasset", "Content/Copy.rasset"));
    CHECK(cook.Cook() == 1);
    CHECK(cook.run.Error("asset-cook: DuplicateAssetId: ") && cook.run.Error(": error\n") &&
          !cook.run.Error("/Game/Rock: error") && cook.run.Out("not published"));
}

// 쿠킹 함수 오류: 쿠킹 루트의 Packages 자리가 파일이면 게시가 실패한다(실패 줄 1 — 보고서 없음).
TEST(AssetCookCommand_CookFailure) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-fail");
    REQUIRE(cook.ready);
    REQUIRE(cook.scene.MakeDirectory("Saved") && cook.scene.MakeDirectory("Saved/Cooked") &&
            cook.scene.MakeDirectory("Saved/Cooked/Windows") &&
            cook.scene.WriteFile("Saved/Cooked/Windows/Packages", "file"));
    CHECK(cook.Cook() == 1);
    CHECK(cook.run.Error(": the cook failed\n") && cook.run.Error("Saved/Cooked/Windows") &&
          cook.run.captured.out.IsEmpty() &&
          !cook.scene.Exists("Saved/Cooked/Windows/CookReport.json"));
}

// 설정 거부: 프로파일(엔진 파일 없음·id 형식·프로젝트 파일 JSON·id 다름·bulkAlignment), 쿠킹
// 규칙(JSON·대입 진단·alwaysCook·neverCook·PrimaryAsset 타입·디렉터리·크기 상한·읽기 오류).
TEST(AssetCookCommand_ConfigRejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-config");
    REQUIRE(cook.ready);
    const auto rejects = [&](StringView profile, StringView expected) noexcept {
        const StringView args[] = {profile, cook.project.View()};
        const int32 code = cook.run.Cook(args);
        return code == 1 && cook.run.Error(expected) && cook.run.captured.out.IsEmpty();
    };

    CHECK(rejects("--profile=Nope",
                  "TargetProfiles/Nope.json: the file does not exist (InvalidArgument)"));
    CHECK(
        rejects("--profile=No-pe",
                "asset-cook: InvalidSetting: TargetProfiles/No-pe: invalid id (InvalidArgument)"));

    REQUIRE(cook.scene.MakeDirectory("Config/TargetProfiles"));
    REQUIRE(cook.scene.WriteFile("Config/TargetProfiles/Windows.json", "{"));
    CHECK(rejects("--profile=Windows", "Config/TargetProfiles/Windows.json: invalid JSON at "));
    REQUIRE(cook.scene.WriteFile("Config/TargetProfiles/Windows.json", "{\"id\": \"Other\"}"));
    CHECK(rejects("--profile=Windows", "Config/TargetProfiles/Windows.json: invalid id"));
    REQUIRE(cook.scene.WriteFile("Config/TargetProfiles/Windows.json", "{\"bulkAlignment\": 3}"));
    CHECK(rejects("--profile=Windows",
                  "InvalidSetting: TargetProfiles/Windows: invalid bulkAlignment"));
    REQUIRE(cook.scene.RemoveFile("Config/TargetProfiles/Windows.json"));

    const auto settings = [&](StringView text, StringView expected) noexcept {
        return cook.Settings(text) && rejects("--profile=Windows", expected);
    };

    CHECK(settings("[", "Config/CookSettings.json: invalid JSON at "));
    CHECK(settings("{\"nope\": 1}", "Config/CookSettings.json: invalid values"));
    CHECK(cook.run.Error("asset-cook: UnknownProperty: "));
    CHECK(settings("{\"alwaysCook\": [\"/Game/\", \"bad\"]}", "invalid alwaysCook[1]"));
    CHECK(settings("{\"neverCook\": [\"bad\"]}", "invalid neverCook[0]"));
    CHECK(settings("{\"primaryAssets\": [{\"type\": \"Nope\", \"directories\": [\"/Game/\"]}]}",
                   "invalid primaryAssets[0].type"));
    CHECK(settings(
        "{\"primaryAssets\": [{\"type\": \"TestAsset\", \"directories\": [\"/Game/\", \"bad\"]}]}",
        "invalid primaryAssets[0].directories[1]"));

    // 크기 상한(CapacityExceeded)과 읽기 오류(디렉터리 — 원인 오류 이름이 코드 자리).
    String large(env.allocator);
    REQUIRE(large.TryAppend("{\"alwaysCook\": [] }"));
    while (large.ByteCount() <= CookConfigMaxFileBytes) {
        REQUIRE(
            large.TryAppend("                                                                "));
    }

    CHECK(settings(large.View(),
                   "Config/CookSettings.json: the file is too large (CapacityExceeded)"));
    REQUIRE(cook.scene.RemoveFile("Config/CookSettings.json") &&
            cook.scene.MakeDirectory("Config/CookSettings.json"));
    CHECK(rejects("--profile=Windows", "Config/CookSettings.json: cannot read ("));
}

// 너겟 설정: 거부 칸마다 실패 줄, 읽기 오류, 열기 실패로 꺼지면 경고 뒤 쿠킹(0), required면 실패.
TEST(AssetCookCommand_NuggetSettings) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-nugget");
    REQUIRE(cook.ready);
    const auto rejects = [&](StringView text, StringView expected) noexcept {
        return cook.scene.WriteFile("Config/AssetNuggets.json", text) && cook.Cook() == 1 &&
               cook.run.Error("Config/AssetNuggets.json: ") && cook.run.Error(expected);
    };

    CHECK(rejects("{", "InvalidSetting: ") && cook.run.Error("invalid JSON at "));
    CHECK(rejects("[]", "the root is not an object"));
    CHECK(rejects("{\"nope\": 1}", "unknown key"));
    CHECK(rejects("{\"backend\": \"x\"}", "invalid backend"));
    CHECK(rejects("{\"required\": \"x\"}", "invalid required"));
    CHECK(rejects("{\"localPath\": 1}", "invalid localPath"));
    CHECK(rejects("{\"generation\": \"x\"}", "invalid generation"));
    String large(env.allocator);
    REQUIRE(large.TryAppend("{}"));
    while (large.ByteCount() <= NuggetSettingsMaxBytes) {
        REQUIRE(
            large.TryAppend("                                                                "));
    }

    CHECK(rejects(large.View(), "the file is too large (CapacityExceeded)"));
    REQUIRE(cook.scene.RemoveFile("Config/AssetNuggets.json") &&
            cook.scene.MakeDirectory("Config/AssetNuggets.json"));
    CHECK(cook.Cook() == 1 && cook.run.Error("Config/AssetNuggets.json: cannot read ("));

    // 열기 실패(로컬 경로가 파일 아래)는 required가 아니면 경고 뒤 쿠킹, required면 실패 줄이다.
    REQUIRE(cook.scene.RemoveDirectory("Config/AssetNuggets.json"));
    REQUIRE(cook.scene.WriteFile("Config/AssetNuggets.json",
                                 "{\"localPath\": \"Rock.trock/nuggets\"}"));
    CHECK(cook.Cook() == 0);
    CHECK(cook.run.Error(": warning NuggetCacheDisabled\n") &&
          cook.run.Error("Rock.trock/nuggets") && cook.run.Out("published targets 2"));
    REQUIRE(cook.scene.WriteFile("Config/AssetNuggets.json",
                                 "{\"localPath\": \"Rock.trock/nuggets\", \"required\": true}"));
    CHECK(cook.Cook() == 1);
    CHECK(cook.run.Error(": cannot open the nugget cache\n") && cook.run.captured.out.IsEmpty());
}

// 사용법: 도움말 0, 형식 위반 2(표준 오류에 사용법), remote 대조 Unsupported 1, 프로젝트 없음 1.
TEST(AssetCookCommand_Usage) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ImporterState state;
    Run run(env.allocator, state);
    const StringView help[] = {"--help"};
    CHECK(run.Cook(help) == 0);
    CHECK(run.captured.out.View().StartsWith("usage: asset-cook --profile=<id>"));

    const auto usage = [&](std::initializer_list<StringView> args) noexcept {
        return run.Cook(Span<const StringView>(args.begin(), args.size())) == 2 &&
               run.captured.error.View().StartsWith("usage: asset-cook") &&
               run.captured.out.IsEmpty();
    };

    CHECK(usage({}));
    CHECK(usage({"/Game/A"}));
    CHECK(usage({"--profile="}));
    CHECK(usage({"--profile=Windows", "--profile=Windows"}));
    CHECK(usage({"--profile=Windows", "--json", "--json"}));
    CHECK(usage({"--profile=Windows", "--verify-nuggets", "--verify-nuggets"}));
    CHECK(usage({"--profile=Windows", "--verify-nuggets-source=local"}));
    CHECK(usage({"--profile=Windows", "--verify-nuggets", "--verify-nuggets-source=other"}));
    CHECK(usage({"--profile=Windows", "--wait=abc"}));
    CHECK(usage({"--profile=Windows", "--force"}));
    CHECK(usage({"--profile=Windows", ""}));
    CHECK(usage({"--help", "--profile=Windows"}));

    const StringView remote[] = {"--profile=Windows", "--verify-nuggets",
                                 "--verify-nuggets-source=remote"};
    CHECK(run.Cook(remote) == 1);
    CHECK(run.captured.error.View() ==
          "asset-cook: Unsupported: remote: the Rockpile nugget backend is not available\n");

    CommandScene empty("cook-command-empty");
    REQUIRE(empty.ready);
    char missing[512] = {};
    FixedStringBuilder missingOption{Span<char>(missing)};
    REQUIRE(missingOption.TryAppendFormat("--project={}/Nope", empty.root.Root()));
    const StringView noProject[] = {"--profile=Windows", missingOption.View()};
    CHECK(run.Cook(noProject) == 1);
    CHECK(run.Error("asset-cook: ") && run.captured.out.IsEmpty());
}

// 출력 실패: 요약·JSON·진단 줄·경고 줄·설정 실패 줄의 쓰기 실패는 명령 오류다.
TEST(AssetCookCommand_OutputFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-output");
    REQUIRE(cook.ready);
    cook.run.captured.failWrites = true;
    cook.run.captured.failStream = CommandletStream::Output;
    CHECK(cook.Cook() == -1);
    CHECK(cook.Cook({"--json"}) == -1);
    cook.run.captured.failStream = CommandletStream::Error;
    REQUIRE(cook.Settings("{\"alwaysCook\": [\"/Game/\", \"/Game/Missing\"]}"));
    CHECK(cook.Cook() == -1);
    REQUIRE(cook.Settings("{\"nope\": 1}"));
    CHECK(cook.Cook() == -1);
    REQUIRE(cook.Settings(AlwaysAll));
    REQUIRE(cook.scene.WriteFile("Config/AssetNuggets.json",
                                 "{\"localPath\": \"Rock.trock/nuggets\"}"));
    CHECK(cook.Cook() == -1);
}

} // namespace asset_cook_commandlet_tests_file
