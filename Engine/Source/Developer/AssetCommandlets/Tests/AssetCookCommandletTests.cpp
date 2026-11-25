#include "AssetCommandletTestSupport.h"

#include <AssetCommandlets/AssetCookCommandlet.h>
#include <AssetCommandlets/AssetEditCommandlet.h>
#include <AssetCooker/CookArtifacts.h>
#include <AssetCooker/CookConfig.h>
#include <AssetCooker/CookRootLock.h>
#include <Core/FileRead.h>
#include <HostConfig/HostSettings.h>
#include <AssetNugget/NuggetCacheSettings.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <AssetNugget/NuggetRecord.h>
#include <AssetRuntime/CookedManifest.h>
#include <Base/InitializerList.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>

// asset-cook 명령 시험이다(Docs/Design/AssetSystem 13절, 10.2절 "A4e 구현 세부" — A4e1):
// 명령 함수를 이 프로세스에서 부른다 — 문맥이 시험 타입 모듈(AssetTestFixtures)과 시험 임포터를
// 주고, 에셋은 asset-import로 만든다. 엔진 설정은 저장소 `Engine/Config`(FindEngineDirectory)다.
// 전체·재사용·verify·JSON·부분 쿠킹, 진단 줄(경로·AssetId·related·alwaysCook 순번·인자 경로·
// 원인 오류), 설정 거부(프로파일·쿠킹 규칙·너겟 설정 칸마다), 너겟 캐시 끔 경고·required 실패,
// 쿠킹 함수 오류, 출력 실패, 사용법. Windows 프로필은 셰이더가 있어 가짜 셰이더 라이브러리 단계를
// 프로세스 표에 RAII로 등록해 돈다(10.3.1절 A4f3 6번) — 미등록 문구·단계 실패 줄도 본다.
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

    // 다른 명령 함수를 같은 문맥으로 부른다(asset-load-cooked·nugget-gc).
    int32 Call(Result<int32> (*command)(void*, Span<const StringView>, CommandletOutput&),
               Span<const StringView> args) noexcept {
        captured.out.Clear();
        captured.error.Clear();
        CommandletOutput output = captured.Output();
        auto ran = command(&context, args, output);
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

constexpr Guid CommandStageGuid{0xA4F3000000000002ull, 0x434d445354470001ull};
// staging 경로 조립 버퍼의 인라인 크기다.
constexpr usize CommandStagingBytes = 64;

// 명령 시험의 가짜 셰이더 라이브러리 단계다: 형식마다 `Engine` 파일 하나(내용 = 형식 id), cookKey =
// SHA-256(형식 id). 같은 키 후보는 Reuse한다. badFormat이면 소문자 형식을 넘긴다(계약 위반).
class CommandStage final : public CookArtifactStage {
public:
    CookArtifactKind Kind() const noexcept override {
        return CookArtifactKind::ShaderLibrary;
    }

    NuggetProcessorId Identity() const noexcept override {
        return NuggetProcessorId{CommandStageGuid, Guid{1, 1}};
    }

    Result<void> Run(const CookArtifactRequest& request, CookArtifactSink& sink) noexcept override {
        const TargetProfile& profile = *request.profile;
        for (const String& backend : profile.graphicsBackends) {
            const ShaderFormatList* list = profile.shaderFormats.Find(backend);
            if (list == nullptr) {
                continue;
            }

            for (const String& format : list->formats) {
                const StringView id = format.View();
                const Sha256Digest key = Sha256::Hash(
                    Span<const uint8>(reinterpret_cast<const uint8*>(id.Data()), id.ByteCount()));
                bool reused = false;
                for (const CookedManifestArtifact& candidate : request.reusable) {
                    if (!reused && candidate.formatId.View() == id && candidate.cookKey == key) {
                        if (auto taken = sink.Reuse(candidate); !taken) {
                            return taken;
                        }

                        reused = true;
                    }
                }

                if (reused) {
                    continue;
                }

                StringBuilder<CommandStagingBytes> path(*request.allocator);
                if (auto issued = sink.NewStagingPath(path); !issued) {
                    return issued;
                }

                platform::File file;
                if (!file.Open(*request.files, path.View().Data(), path.View().ByteCount(),
                               platform::FileOpenMode::CreateAlways)) {
                    return Result<void>::Failure(ErrorCode::IoError);
                }

                const platform::FileTransfer written =
                    file.WriteAll(reinterpret_cast<const uint8*>(id.Data()), id.ByteCount());
                if (!file.Close() || !written.status) {
                    return Result<void>::Failure(ErrorCode::IoError);
                }

                const StringView sent = badFormat ? StringView("d3d12_sm6") : id;
                if (auto produced = sink.Produce(
                        CookArtifactOutput{CookedArtifactEngineName, sent, key, path.View()});
                    !produced) {
                    return produced;
                }
            }
        }

        return Result<void>::Success();
    }

    bool badFormat = false;
};

// 프로세스 단계 표 등록 구간이다(다른 시험으로 새지 않게 소멸에서 해제한다).
struct StageRegistration {
    explicit StageRegistration(CookArtifactStage& stage) noexcept : registered(stage) {
        ok = static_cast<bool>(CookArtifactStages().Register(stage));
    }

    ~StageRegistration() {
        if (ok) {
            (void)CookArtifactStages().Unregister(registered);
        }
    }

    StageRegistration(const StageRegistration&) = delete;
    StageRegistration& operator=(const StageRegistration&) = delete;

    CookArtifactStage& registered;
    bool ok = false;
};

// 쿠킹할 프로젝트다: Rock(Hard 참조 → Rock_extra)과 Rock_extra, 쿠킹 규칙(전부).
struct CookScene {
    CookScene(Allocator& allocator, const char* tag) noexcept
        : scene(tag), run(allocator, state), project(Span<char>(projectText)),
          source(Span<char>(sourceText)), registration(stage) {
        ready = scene.ready && registration.ok &&
                project.TryAppendFormat("--project={}", scene.root.Root()) &&
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
    CommandStage stage;
    StageRegistration registration;
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

// 비에셋 산출물 단계(10.3.1절 A4f3): 가짜 단계로 게시하면 Shaders 파일과 보고서 artifacts가 있다.
// 단계가 없으면 `UnsupportedCapability: <루트>: error` 줄로 게시하지 않는다. 계약 위반(소문자
// 형식)은 실패 줄에 단계 정체성·호출·형식을 적는다(보고서 없음).
TEST(AssetCookCommand_ArtifactStage) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-stage");
    REQUIRE(cook.ready);
    CHECK(cook.Cook({"--json"}) == 0);
    CHECK(cook.run.Out("\"published\": true") && cook.run.Out("\"ShaderLibrary\"") &&
          cook.run.Out("\"format\": \"D3D12_SM6\"") &&
          cook.scene.DirectoryExists("Saved/Cooked/Windows/Shaders"));

    REQUIRE(CookArtifactStages().Unregister(cook.stage));
    cook.registration.ok = false;
    CHECK(cook.Cook() == 1);
    CHECK(cook.run.Error("asset-cook: UnsupportedCapability: ") &&
          cook.run.Error("Saved/Cooked/Windows: error\n") && cook.run.Out("not published"));

    REQUIRE(CookArtifactStages().Register(cook.stage));
    cook.registration.ok = true;
    cook.stage.badFormat = true;
    REQUIRE(cook.scene.RemoveFile("Saved/Cooked/Windows/CookedManifest.rman"));
    CHECK(cook.Cook() == 1);
    CHECK(
        cook.run.Error("asset-cook: InvalidArgument: ") &&
        cook.run.Error(": stage a4f30000-0000-0002-434d-445354470001 Produce(d3d12_sm6): the cook "
                       "failed\n") &&
        cook.run.captured.out.IsEmpty());
}

// `--wait` 마감 시각 하나(10.3.1절 A4f4 1번): 지난 시간을 빼고 남은 밀리초, 다 쓰면 0, 시계가
// 거꾸로면 지난 시간 0.
TEST(AssetCookCommand_RemainingWait) {
    constexpr uint64 Millisecond = 1000000;
    CHECK(RemainingWaitMilliseconds(1000, 0, 0) == 1000);
    CHECK(RemainingWaitMilliseconds(1000, 5 * Millisecond, 405 * Millisecond) == 600);
    CHECK(RemainingWaitMilliseconds(1000, 0, 1000 * Millisecond) == 0);
    CHECK(RemainingWaitMilliseconds(1000, 0, 5000 * Millisecond) == 0);
    CHECK(RemainingWaitMilliseconds(1000, 10 * Millisecond, 0) == 1000);
    CHECK(RemainingWaitMilliseconds(0, 0, Millisecond) == 0);
}

// 쿠킹 루트 잠금(10.3.1절 A4f4 2번): 다른 쪽이 같은 쿠킹 루트를 쥐고 있으면 프로젝트 쿠킹은
// `Busy: <루트>: the cook root is locked` 줄로 끝난다(--wait=0). 놓으면 다시 된다.
TEST(AssetCookCommand_CookRootLock) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-root-lock");
    REQUIRE(cook.ready);
    StringBuilder<512> saved(env.allocator);
    REQUIRE(saved.TryAppendFormat("{}/Saved", cook.scene.root.Root()));
    CookRootLock held;
    REQUIRE(held.Acquire(env.allocator, saved.View(), "Windows", 0));
    CHECK(cook.Cook() == 1);
    CHECK(cook.run.Error("Busy: ") && cook.run.Error("the cook root is locked by another process"));
    held.Release();
    CHECK(cook.Cook() == 0);
}

// 엔진 전용 쿠킹(10.3.1절 A4f4 4번): 문맥의 엔진 디렉터리 재정의(임시 디렉터리 — 실제 저장소
// Engine/Saved를 건드리지 않는다)에 저장소의 Windows 프로필을 두고 `--engine`으로 쿠킹하면 엔진
// 쿠킹 루트(`<엔진>/Saved/Cooked/Windows`)에 게시한다(항목 0 — 요약 줄 targets 0). 엔진
// CookSettings 파일을 읽는다(문법 오류는 그 파일 줄). 쿠킹 루트 잠금을 쥐고 있으면 Busy다.
TEST(AssetCookCommand_EngineCook) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "cook-command-engine");
    REQUIRE(cook.ready);
    CommandScene engine("cook-command-engine-dir");
    REQUIRE(engine.ready);
    char source[1024] = {};
    auto found = FindEngineDirectory(Span<char>(source));
    REQUIRE(found);
    StringBuilder<1100> profilePath(env.allocator);
    REQUIRE(profilePath.TryAppendFormat("{}/Config/TargetProfiles/Windows.json",
                                        StringView(source, found.Value())));
    Array<uint8> profileBytes(env.allocator);
    bool existed = false;
    REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), profilePath.View(), 1024 * 1024,
                          profileBytes, existed) &&
            existed);
    REQUIRE(engine.MakeDirectory("Config") && engine.MakeDirectory("Config/TargetProfiles") &&
            engine.WriteFile("Config/TargetProfiles/Windows.json",
                             StringView(reinterpret_cast<const char*>(profileBytes.Data()),
                                        profileBytes.Size())));
    cook.run.context.engineDirectory = engine.root.Root();
    const StringView args[] = {"--engine", "--profile=Windows"};
    CHECK(cook.run.Cook(args) == 0);
    CHECK(cook.run.Out("asset-cook: Windows published targets 0 cooked 0 reused 0 bytes ") &&
          engine.Exists("Saved/Cooked/Windows/CookedManifest.rman") &&
          engine.DirectoryExists("Saved/Cooked/Windows/Shaders") &&
          !cook.scene.Exists("Saved/Cooked/Windows/CookedManifest.rman"));

    REQUIRE(engine.WriteFile("Config/CookSettings.json", "{"));
    CHECK(cook.run.Cook(args) == 1);
    CHECK(cook.run.Error("Config/CookSettings.json"));
    REQUIRE(engine.WriteFile("Config/CookSettings.json", "{\"cookDevelopmentShaders\": true}"));

    StringBuilder<512> saved(env.allocator);
    REQUIRE(saved.TryAppendFormat("{}/Saved", engine.root.Root()));
    CookRootLock held;
    REQUIRE(held.Acquire(env.allocator, saved.View(), "Windows", 0));
    CHECK(cook.run.Cook(args) == 1);
    CHECK(cook.run.Error("Busy: ") && cook.run.Error("the cook root is locked"));
    held.Release();
    const StringView json[] = {"--engine", "--profile=Windows", "--json"};
    CHECK(cook.run.Cook(json) == 0);
    CHECK(cook.run.Out("\"ShaderLibrary\"") && cook.run.Out("\"name\": \"Engine\""));
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
    // 엔진 전용은 프로젝트·경로·대조 원천과 함께 쓰지 않는다(10.3.1절 A4f4 4번).
    CHECK(usage({"--engine", "--profile=Windows", "--project=X"}));
    CHECK(usage({"--engine", "--profile=Windows", "/Game/A"}));
    CHECK(usage(
        {"--engine", "--profile=Windows", "--verify-nuggets", "--verify-nuggets-source=local"}));
    CHECK(usage({"--engine", "--engine", "--profile=Windows"}));
    CHECK(usage({"--engine"}));

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

namespace {

// 쿠킹한 Windows 매니페스트를 읽는다.
Result<CookedManifest> ReadCooked(Allocator& allocator, CookScene& cook) noexcept {
    StringBuilder<512> path(allocator);
    Array<uint8> bytes(allocator);
    bool existed = false;
    if (!path.TryAppendFormat("{}/Saved/Cooked/Windows/{}", cook.scene.root.Root(),
                              CookedManifestFileName) ||
        !ReadWholeFile(platform::PhysicalFileSystem(), path.View(), CookedManifestMaxFileBytes,
                       bytes, existed) ||
        !existed) {
        return Result<CookedManifest>::Failure(ErrorCode::NotFound);
    }

    return ReadCookedManifest(allocator, bytes.View());
}

} // namespace

// asset-load-cooked(13.1절 A6): 쿠킹한 프로젝트에서 경로 힌트(대소문자 다르게)·AssetId로 로드하면
// 성공 줄(Rock과 Hard 의존 Rock_extra — packages 2)이다. 없는 경로 NotFound, 패키지 손상
// CorruptPackage, 다른 프로필의 매니페스트 없음, 프로필이 다른 매니페스트 IncompatibleCook, 사용법
// 오류(대상 없음·둘·잘못된 AssetId·프로필 없음).
TEST(AssetLoadCookedCommand_LoadsAndRejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CookScene cook(env.allocator, "load-cooked-command");
    REQUIRE(cook.ready);
    REQUIRE(cook.Cook() == 0);
    StringBuilder<512> project(env.allocator);
    REQUIRE(project.TryAppendFormat("--project={}", cook.scene.root.Root()));
    const auto load = [&](std::initializer_list<StringView> extra) noexcept {
        StringView args[8] = {project.View()};
        usize count = 1;
        for (StringView one : extra) {
            args[count++] = one;
        }

        return cook.run.Call(&RunAssetLoadCookedCommand, Span<const StringView>(args, count));
    };
    CHECK(load({"--profile=Windows", "/game/rock"}) == 0);
    CHECK(cook.run.Out("asset-load-cooked: Windows loaded ") &&
          cook.run.Out(" (/Game/Rock) packages 2\n"));

    auto manifest = ReadCooked(env.allocator, cook);
    REQUIRE(manifest);
    const CookedManifestEntry* rock = nullptr;
    for (const CookedManifestEntry& entry : manifest.Value().entries) {
        if (entry.pathHint.View() == "/Game/Rock") {
            rock = &entry;
        }
    }

    REQUIRE(rock != nullptr);
    StringBuilder<64> id(env.allocator);
    REQUIRE(id.TryAppendFormat("{}", rock->asset));
    CHECK(load({"--profile=Windows", id.View()}) == 0);
    CHECK(cook.run.Out(" (/Game/Rock) packages 2\n"));

    CHECK(load({"--profile=Windows", "/Game/Nope"}) == 1);
    CHECK(cook.run.Error("asset-load-cooked: NotFound: /Game/Nope: error NotFound\n"));
    CHECK(load({"--profile=Android", "/Game/Rock"}) == 1);
    CHECK(cook.run.Error("asset-load-cooked: NotFound: ") &&
          cook.run.Error("cannot read the cooked manifest"));

    // 프로필이 다른 매니페스트(Windows 쿠킹 결과를 Android 자리에 둠)는 IncompatibleCook이다.
    StringBuilder<512> windows(env.allocator);
    Array<uint8> bytes(env.allocator);
    bool existed = false;
    REQUIRE(windows.TryAppendFormat("{}/Saved/Cooked/Windows/{}", cook.scene.root.Root(),
                                    CookedManifestFileName) &&
            ReadWholeFile(platform::PhysicalFileSystem(), windows.View(),
                          CookedManifestMaxFileBytes, bytes, existed) &&
            existed);
    REQUIRE(cook.scene.MakeDirectory("Saved/Cooked/Android") &&
            cook.scene.WriteFile(
                "Saved/Cooked/Android/CookedManifest.rman",
                StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size())));
    CHECK(load({"--profile=Android", "/Game/Rock"}) == 1);
    CHECK(cook.run.Error("asset-load-cooked: IncompatibleCook: "));

    // 패키지 손상: Rock 패키지 파일의 한 byte를 바꾸면 CorruptPackage다.
    StringBuilder<256> package(env.allocator);
    REQUIRE(package.TryAppend("Saved/Cooked/Windows/") &&
            AppendCookedPackagePath(package, rock->asset, rock->packageSha256));
    StringBuilder<512> packagePath(env.allocator);
    REQUIRE(packagePath.TryAppendFormat("{}/{}", cook.scene.root.Root(), package.View()));
    Array<uint8> packageBytes(env.allocator);
    constexpr uint64 PackageReadMaxBytes = 64ull * 1024 * 1024;
    REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), packagePath.View(), PackageReadMaxBytes,
                          packageBytes, existed) &&
            existed && packageBytes.Size() > 100);
    packageBytes[100] ^= 1;
    REQUIRE(cook.scene.WriteFile(
        package.View(),
        StringView(reinterpret_cast<const char*>(packageBytes.Data()), packageBytes.Size())));
    CHECK(load({"--profile=Windows", "/Game/Rock"}) == 1);
    CHECK(cook.run.Error("asset-load-cooked: CorruptPackage: /Game/Rock: error "));

    const auto usage = [&](std::initializer_list<StringView> extra) noexcept {
        return load(extra) == 2 && cook.run.Error("usage: asset-load-cooked");
    };
    CHECK(usage({"--profile=Windows"}));
    CHECK(usage({"--profile=Windows", "/Game/A", "/Game/B"}));
    CHECK(usage({"--profile=Windows", "1234"}));
    // AssetId는 36자 소문자 표기만 받는다(구분자 없는 32자·대문자는 사용법 오류 — 다른 명령과
    // 같다).
    CHECK(usage({"--profile=Windows", "0123456789abcdef0123456789abcdef"}));
    CHECK(usage({"--profile=Windows", "01234567-89AB-CDEF-0123-456789ABCDEF"}));
    // 프로필 id 문법(경로 조각 — Saved/Cooked 밖을 가리키지 못한다).
    CHECK(usage({"--profile=../Windows", "/Game/Rock"}));
    CHECK(usage({"/Game/Rock"}));
    CHECK(usage({"--profile=Windows", "--profile=Windows", "/Game/Rock"}));
}

// nugget-gc(13.1절 A6): 로컬 backend에 너겟 하나를 둔 뒤 큰 상한(바이트 최대·일수 포화)은 남기고,
// --max-bytes=0은 지운다(바이트 상한이 결정적으로 보장 — 시계에 기대지 않는다). off·rockpile 설정
// 줄, 읽기 전용 루트(tmp 자리가 파일) 줄, 사용법 오류(빠짐·문법·u32 넘침·두 번).
TEST(NuggetGcCommand_CollectsAndRejects) {
    AssetEnv env;
    REQUIRE(env.Ready());
    CommandScene scene("nugget-gc-command");
    REQUIRE(scene.ready);
    ImporterState state;
    Run run(env.allocator, state);
    StringBuilder<512> project(env.allocator);
    StringBuilder<512> nuggets(env.allocator);
    REQUIRE(project.TryAppendFormat("--project={}", scene.root.Root()) &&
            nuggets.TryAppendFormat("{}/{}", scene.root.Root(), NuggetDefaultLocalPath));
    {
        NuggetSystemClock clock;
        NuggetFileSystemBackend backend(env.allocator, platform::PhysicalFileSystem(),
                                        nuggets.View(), clock);
        REQUIRE(backend.Open());
        const uint8 value[] = {1, 2, 3, 4};
        const NuggetPutValue values[] = {{0, value}};
        Sha256Digest key;
        for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
            key.bytes[index] = static_cast<uint8>(index + 1);
        }

        REQUIRE(backend.Put(NuggetBackendPut{NuggetBucketTexture, key, values}).status ==
                NuggetStatus::Stored);
    }

    const auto gc = [&](std::initializer_list<StringView> extra) noexcept {
        StringView args[8] = {project.View()};
        usize count = 1;
        for (StringView one : extra) {
            args[count++] = one;
        }

        return run.Call(&RunNuggetGcCommand, Span<const StringView>(args, count));
    };
    CHECK(gc({"--max-bytes=18446744073709551615", "--unused-days=4294967295"}) == 0);
    CHECK(run.Out("nugget-gc: removed 0 (0 bytes) kept 1 ("));
    CHECK(gc({"--max-bytes=0", "--unused-days=4294967295"}) == 0);
    CHECK(run.Out("nugget-gc: removed 1 (") && run.Out(" kept 0 (0 bytes) skipped 0"));

    REQUIRE(scene.MakeDirectory("Config") &&
            scene.WriteFile("Config/AssetNuggets.json", "{\"backend\": \"off\"}"));
    CHECK(gc({"--max-bytes=0", "--unused-days=0"}) == 0);
    CHECK(run.Out("nugget-gc: the nugget cache is off\n"));
    REQUIRE(scene.WriteFile("Config/AssetNuggets.json", "{\"backend\": \"rockpile\"}"));
    CHECK(gc({"--max-bytes=0", "--unused-days=0"}) == 1);
    CHECK(run.Error(
        "nugget-gc: Unsupported: rockpile: the Rockpile nugget backend is not available"));
    REQUIRE(scene.WriteFile("Config/AssetNuggets.json", "{"));
    CHECK(gc({"--max-bytes=0", "--unused-days=0"}) == 1);
    CHECK(run.Error("nugget-gc: InvalidSetting: ") && run.Error("cannot read the nugget settings"));

    // 읽기 전용: 로컬 루트의 tmp 자리를 파일로 두면 tmp를 만들 수 없다.
    REQUIRE(scene.WriteFile("Config/AssetNuggets.json",
                            "{\"backend\": \"filesystem\", \"localPath\": \"Shared\"}") &&
            scene.MakeDirectory("Shared") && scene.WriteFile("Shared/tmp", "file"));
    CHECK(gc({"--max-bytes=0", "--unused-days=0"}) == 1);
    CHECK(run.Error("nugget-gc: Unsupported: ") && run.Error("the nugget cache is read-only"));

    const auto usage = [&](std::initializer_list<StringView> extra) noexcept {
        return gc(extra) == 2 && run.Error("usage: nugget-gc");
    };
    CHECK(usage({"--max-bytes=0"}));
    CHECK(usage({"--unused-days=0"}));
    CHECK(usage({"--max-bytes=1x", "--unused-days=0"}));
    CHECK(usage({"--max-bytes=-1", "--unused-days=0"}));
    CHECK(usage({"--max-bytes=0", "--unused-days=4294967296"}));
    CHECK(usage({"--max-bytes=18446744073709551616", "--unused-days=0"}));
    CHECK(usage({"--max-bytes=0", "--max-bytes=0", "--unused-days=0"}));
    CHECK(usage({"--max-bytes=0", "--unused-days=0", "extra"}));
}

} // namespace asset_cook_commandlet_tests_file
