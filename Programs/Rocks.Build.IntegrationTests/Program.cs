using System.Diagnostics;
using System.Text.Json;
using Rocks.Build;
using Rocks.Build.Cli;
using Rocks.Build.IntegrationTests;
using Rocks.Tooling.Common;

if (args.Length != 0 && args[0] == "--output-lock-child")
{
    return await OutputInvocationLockTests.ChildAsync(args);
}

if (args.Length == 1 && args[0] == "--platform-path-header")
{
    var native = await NativeToolchains.DiscoverAsync();
    var temporary = Path.Combine(Path.GetTempPath(), "Rocks platform header " + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(temporary);
    try
    {
        await PlatformPrivateHeaderTests.RunAsync(native, temporary);
        Console.WriteLine("[PASS] platform-private-header-incremental");
        return 0;
    }
    finally
    {
        Directory.Delete(temporary, recursive: true);
    }
}

var options = new Dictionary<string, string>(StringComparer.Ordinal);

for (var index = 0; index < args.Length; index += 2)
{
    if (index + 1 >= args.Length || args[index] is not ("--root" or "--compiler") || !options.TryAdd(args[index], args[index + 1]))
    {
        throw new ArgumentException("Expected unique --root/--compiler options with values.");
    }
}

var root = Path.GetFullPath(options.GetValueOrDefault("--root", "."));
var toolchain = await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("--compiler"));
var cli = Path.Combine(root, "out/dotnet/bin/Rocks.Build.Cli/Debug/net10.0/Rocks.Build.Cli.dll");
var fixture = Path.Combine(Path.GetTempPath(), "Rocks SDK 게임 " + Guid.NewGuid().ToString("N"));
var sdk = Path.Combine(fixture, "installed");
var game = Path.Combine(fixture, "game");
var output = Path.Combine(game, "out/Debug");
var reports = Path.Combine(root, "out/sdk-integration");
Directory.CreateDirectory(reports);
var cases = new List<object>();
bool success = false;

try
{
    var timer = Stopwatch.StartNew();
    await OutputInvocationLockTests.RunAsync();
    MetadataOutputTransactionTests.Run();
    MetadataLayoutTests.Run();
    MetadataTemporaryCleanupTests.Run();
    await PlatformPrivateHeaderTests.RunAsync(toolchain, Path.Combine(fixture, "platform-header"));
    Pass("output-invocation-lock-and-platform-header-incremental", timer.Elapsed);
    timer.Restart();
    var exported = await Cli("sdk", "--root", root, "--target", "Engine", "--configuration", "Debug", "--destination", sdk);
    Require(exported.ExitCode == 0, "SDK export failed: " + exported.StdErr);
    Require(File.Exists(Path.Combine(sdk, "rocks-sdk.json")), "SDK manifest missing");
    // Engine SDK는 Core 값 타입 결합 모듈(CoreTypeBindings — Metadata 모듈)을 descriptor·identity 명세·생성 헤더와 함께 담는다.
    using (var engineManifest = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(sdk, "rocks-sdk.json"))))
    {
        var bindingsEntry = engineManifest.RootElement.GetProperty("Modules").EnumerateArray()
            .Single(module => module.GetProperty("Name").GetString() == "CoreTypeBindings");
        Require(bindingsEntry.GetProperty("Descriptor").GetString() == "Metadata/CoreTypeBindings/CoreTypeBindings.rocksmeta.json" &&
            bindingsEntry.GetProperty("IdentityManifest").GetString() == "Metadata/CoreTypeBindings/CoreTypeBindings.rocksids.json",
            "Engine SDK manifest does not name the CoreTypeBindings descriptor and identity manifest");
    }

    Require(File.Exists(Path.Combine(sdk, "Metadata/CoreTypeBindings/CoreTypeBindings.rocksmeta.json")) &&
        File.Exists(Path.Combine(sdk, "Metadata/CoreTypeBindings/CoreTypeBindings.rocksids.json")),
        "Engine SDK is missing the CoreTypeBindings descriptor or identity manifest");
    Require(Directory.Exists(Path.Combine(sdk, "Headers/CoreTypeBindings")) &&
        Directory.GetFiles(Path.Combine(sdk, "Headers/CoreTypeBindings"), "CoreValueTypes.generated.h", SearchOption.AllDirectories).Length == 1,
        "Engine SDK is missing the CoreTypeBindings generated header");
    Require(!Directory.GetFiles(sdk, "*.cpp", SearchOption.AllDirectories).Concat(Directory.GetFiles(sdk, "*.c", SearchOption.AllDirectories)).Any(), "Installed SDK contains engine or third-party sources");
    Pass("engine-export", timer.Elapsed);

    // MSBuild 경유 빌드를 연속 두 번 하면 두 번째는 0 액션이다. dotnet msbuild는 실행마다 다른 세션 변수
    // (DOTNET_CLI_TELEMETRY_SESSIONID)를 넘기고, 여기서도 실행마다 다른 변수를 더한다 — 허용 목록 밖 변수는 증분 key가 아니다.
    timer.Restart();
    var engineReport = Path.Combine(root, "out", $"dotnet-{(OperatingSystem.IsWindows() ? "windows" : "macos")}-engine-debug", "action-report.json");
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        var inherited = Environment.GetEnvironmentVariables().Cast<System.Collections.DictionaryEntry>()
            .ToDictionary(entry => (string)entry.Key, entry => (string?)entry.Value,
                OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        inherited["ROCKS_INTEGRATION_SESSION"] = Guid.NewGuid().ToString("N");
        var msbuild = await ProcessRunner.RunAsync(new("dotnet", ["msbuild", Path.Combine(root, "Rocks.proj"), "-t:Build", "-p:RocksTarget=Engine",
            "-p:Configuration=Debug", "-p:Compiler=" + toolchain.CompilerPath, "-nologo"], root, inherited, Timeout: TimeSpan.FromMinutes(20)));
        File.AppendAllText(Path.Combine(reports, "sdk-integration.log"), "msbuild Engine run " + attempt + "\n" + msbuild.StdOut + msbuild.StdErr + "\n");
        Require(msbuild.ExitCode == 0, "MSBuild Engine build failed: " + msbuild.StdOut + msbuild.StdErr);
    }

    var engineSecond = JsonSerializer.Deserialize<NativeBuildResult>(File.ReadAllText(engineReport))!;
    Require(engineSecond.Succeeded && engineSecond.ExecutedActions == 0,
        $"The second consecutive MSBuild Engine build executed {engineSecond.ExecutedActions} actions");
    Pass("msbuild-consecutive-build-no-op", timer.Elapsed);

    CopyTree(Path.Combine(root, "Templates/Game/Foundation"), game);
    var originalFiles = Directory.GetFiles(sdk, "*", SearchOption.AllDirectories)
        .ToDictionary(path => path, Hashing.Sha256File, StringComparer.Ordinal);
    foreach (var file in originalFiles.Keys)
    {
        SetReadOnly(file, true);
    }

    timer.Restart();
    var built = await Game();
    Require(built.ExitCode == 0, "External game build failed: " + built.StdOut + built.StdErr);
    var initial = ReadActionReport();
    Require(initial.Succeeded && initial.ExecutedActions > 0 && initial.Actions.All(action => action.Module == "SampleGame"),
        "Game build compiled an engine module or failed");
    var run = await ProcessRunner.RunAsync(new(initial.ModuleOutputs["SampleGame"], [], game, toolchain.Environment));
    Require(run.ExitCode == 0, "External game's generated CLASS/PROPERTY runtime failed");
    Pass("read-only-sdk-external-game-reflection", timer.Elapsed);

    // 빌드 주입 상수(B7): SDK의 Core(ProvidesBuildInfo)에 닿는 게임 실행 파일에 BuildInfo TU가 들어가고, manifest는 SDK 경로를
    // sdk: 키로 쓰며 SDK·프로젝트 절대 경로를 담지 않는다.
    timer.Restart();
    var gameBuildInfo = Path.Combine(output, "BuildInfo/SampleGame.BuildInfo.txt");
    Require(File.Exists(gameBuildInfo) && File.Exists(Path.Combine(output, "BuildInfo/SampleGame.BuildInfo.cpp")),
        "Game build did not generate the SampleGame BuildInfo files");
    var gameManifest = File.ReadAllText(gameBuildInfo);
    Require(gameManifest.Contains("\ninput: sdk:Headers/", StringComparison.Ordinal) && gameManifest.Contains("\narg: sdk:Libraries/", StringComparison.Ordinal) &&
        gameManifest.Contains("\nhost-dependent: no\n", StringComparison.Ordinal), "Game BuildInfo manifest lacks sdk: keys:\n" + gameManifest);
    foreach (var absolute in new[] { sdk, game, fixture })
    {
        Require(!gameManifest.Contains(absolute, StringComparison.OrdinalIgnoreCase) &&
            !gameManifest.Contains(absolute.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase),
            "Game BuildInfo manifest contains an absolute host path: " + absolute);
    }

    Require(initial.ToolchainFingerprint is not null && initial.ToolBuildIds.TryGetValue("SampleGame", out var sampleId) &&
        sampleId == Hashing.Sha256File(gameBuildInfo), "Game action report does not carry the fingerprint and sha256(SampleGame.BuildInfo.txt)");
    Pass("game-buildinfo-sdk-keys", timer.Elapsed);

    // 잘못된 새 ID 발급은 이름 변경을 대체하지 못한다. 실패 시 기존 생성물을 보존한다.
    timer.Restart();
    string gameHeader = Path.Combine(game, "Source/GameObject.h");
    string gameMain = Path.Combine(game, "Source/Main.cpp");
    string gameIdentity = Path.Combine(game, "SampleGame.rocksids.json");
    string generatedIdentityHeader = Path.Combine(output, "Generated/SampleGame/Public/GameObject.generated.h");
    string identityOriginalHeader = File.ReadAllText(gameHeader);
    string originalMain = File.ReadAllText(gameMain);
    byte[] originalIdentity = File.ReadAllBytes(gameIdentity);
    string generatedBefore = Hashing.Sha256File(generatedIdentityHeader);
    string metadataTool = Path.Combine(root, "out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll");
    async Task<ProcessResult> Ids(params string[] arguments) => await ProcessRunner.RunAsync(new("dotnet",
        new[] { metadataTool, "ids" }.Concat(arguments).ToArray(), root, toolchain.Environment));
    try
    {
        File.WriteAllText(gameHeader, identityOriginalHeader.Replace("health", "armor", StringComparison.Ordinal));
        var added = await Ids("add", "--manifest", gameIdentity, "--field", "sample::GameObject::armor");
        Require(added.ExitCode == 0, "Could not prepare accidental replacement fixture: " + added.StdErr);
        var rejected = await Game();
        Require(rejected.ExitCode != 0 && rejected.StdErr.Contains("missing from target sources", StringComparison.Ordinal),
            "An unretired field disappeared after an unrelated ID was issued: " + rejected.StdErr);
        Require(Hashing.Sha256File(generatedIdentityHeader) == generatedBefore,
            "Failed identity validation published generated metadata.");
        File.WriteAllBytes(gameIdentity, originalIdentity);
        File.WriteAllText(gameHeader, identityOriginalHeader);
        Require((await Game()).ExitCode == 0, "Game did not recover after rejected identity change");

        var renamed = await Ids("rename", "--manifest", gameIdentity, "--kind", "field", "--old",
            "sample::GameObject::health", "--new", "sample::GameObject::hitPoints");
        Require(renamed.ExitCode == 0, "Could not rename identity: " + renamed.StdErr);
        File.WriteAllText(gameHeader, identityOriginalHeader.Replace("health", "hitPoints", StringComparison.Ordinal));
        File.WriteAllText(gameMain, originalMain.Replace("health", "hitPoints", StringComparison.Ordinal));
        var renamedBuild = await Game();
        Require(renamedBuild.ExitCode == 0, "Game failed after explicit identity rename: " + renamedBuild.StdErr);
        var renamedResult = ReadActionReport();
        var renamedRun = await ProcessRunner.RunAsync(new(renamedResult.ModuleOutputs["SampleGame"], [], game, toolchain.Environment));
        Require(renamedRun.ExitCode == 0, "Renamed game failed at runtime");
        using var identityJson = JsonDocument.Parse(File.ReadAllBytes(gameIdentity));
        var entry = identityJson.RootElement.GetProperty("entries").EnumerateArray()
            .Single(value => value.GetProperty("name").GetString() == "sample::GameObject::hitPoints");
        Require(entry.GetProperty("id").GetString() == "da100000000000000000000000000002",
            "Explicit field rename changed its stable ID");
    }
    finally
    {
        File.WriteAllText(gameHeader, identityOriginalHeader);
        File.WriteAllText(gameMain, originalMain);
        File.WriteAllBytes(gameIdentity, originalIdentity);
    }

    Require((await Game()).ExitCode == 0, "Game did not recover after identity integration test");
    Pass("identity-rename-and-orphan-rejection", timer.Elapsed);

    // SDK descriptor import: 리플렉션 fixture 모듈을 담은 SDK를 내보내고(엔진 SDK의 출력 디렉터리를 같이 써 다시
    // 컴파일하지 않는다), 게임 모듈이 그 SDK 모듈의 타입을 상속·참조한다. SDK 모듈은 다시 생성하지 않는다.
    timer.Restart();
    var reflectionSdk = Path.Combine(fixture, "installed-reflection");
    var engineOutput = Path.Combine(root, "out", $"dotnet-{(OperatingSystem.IsWindows() ? "windows" : "macos")}-engine-debug");
    var reflectionExport = await Cli("sdk", "--root", root, "--target", "ReflectionFixtures", "--configuration", "Debug",
        "--destination", reflectionSdk, "--output", engineOutput);
    Require(reflectionExport.ExitCode == 0, "Reflection fixture SDK export failed: " + reflectionExport.StdOut + reflectionExport.StdErr);
    using (var sdkManifest = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(reflectionSdk, "rocks-sdk.json"))))
    {
        var sdkModules = sdkManifest.RootElement.GetProperty("Modules").EnumerateArray().ToArray();
        var fixtureModule = sdkModules.Single(module => module.GetProperty("Name").GetString() == "ReflectionTestFixtures");
        Require(fixtureModule.GetProperty("Descriptor").GetString() == "Metadata/ReflectionTestFixtures/ReflectionTestFixtures.rocksmeta.json" &&
            fixtureModule.GetProperty("IdentityManifest").GetString() == "Metadata/ReflectionTestFixtures/ReflectionTestFixtures.rocksids.json",
            "SDK manifest does not name the fixture module's descriptor and identity manifest");
        // fixture 모듈은 Core 값 타입 결합 모듈(CoreTypeBindings — 역시 Metadata 모듈)에 의존한다.
        var bindingsModule = sdkModules.Single(module => module.GetProperty("Name").GetString() == "CoreTypeBindings");
        Require(bindingsModule.GetProperty("Descriptor").GetString() == "Metadata/CoreTypeBindings/CoreTypeBindings.rocksmeta.json",
            "SDK manifest does not name the CoreTypeBindings descriptor");
        Require(sdkModules.Where(module => module.GetProperty("Name").GetString() is not ("ReflectionTestFixtures" or "CoreTypeBindings"))
            .All(module => module.GetProperty("Descriptor").ValueKind == JsonValueKind.Null && module.GetProperty("IdentityManifest").ValueKind == JsonValueKind.Null),
            "SDK module without reflection headers exported metadata");
        Require(File.Exists(Path.Combine(reflectionSdk, "Headers/ReflectionTestFixtures/1/ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h")),
            "SDK is missing the fixture module's generated public headers");
    }

    // 게임에는 메타데이터 모듈이 둘이다: 라이브러리 GameCore(CoreThing : SDK Base)와 실행 파일 SampleGame
    // (FixtureChild : GameCore의 CoreThing, SDK Node 참조). 한 빌드 안에서 stage descriptor import와 stage 생성 루트
    // 치환, 상위 모듈 실패 시 미게시, Metadata에서 뺀 헤더의 생성물 삭제를 확인한다.
    var reflectionGame = Path.Combine(fixture, "game-reflection");
    var reflectionOutput = Path.Combine(reflectionGame, "out/Debug");
    CopyTree(Path.Combine(root, "Templates/Game/Foundation"), reflectionGame);
    var reflectionRules = Path.Combine(reflectionGame, "Game.Build.cs");
    var reflectionRulesText = File.ReadAllText(reflectionRules);
    Require(reflectionRulesText.Contains("Dependencies = [\"ObjectRuntime\"]", StringComparison.Ordinal) &&
        reflectionRulesText.Contains("Metadata = [\"GameObject.h\"]", StringComparison.Ordinal), "Game template rules changed; update the SDK descriptor fixture");
    // ObjectSerialization: 게임이 GameCore의 배열 필드를 그래프로 저장·복원한다(SDK 모듈로 링크만 한다).
    File.WriteAllText(reflectionRules, reflectionRulesText
        .Replace("Dependencies = [\"ObjectRuntime\"]", "Dependencies = [\"ObjectRuntime\", \"GameCore\", \"ObjectSerialization\"]", StringComparison.Ordinal)
        .Replace("Metadata = [\"GameObject.h\"]", "Metadata = [\"GameObject.h\", \"FixtureChild.h\"]", StringComparison.Ordinal));
    var reflectionProject = Path.Combine(reflectionGame, "Game.rocksproject");
    var reflectionProjectText = File.ReadAllText(reflectionProject);
    const string sampleModuleEntry = "{ \"name\": \"SampleGame\", \"type\": \"Runtime\", \"loadingPhase\": \"Default\" }";
    Require(reflectionProjectText.Contains(sampleModuleEntry, StringComparison.Ordinal), "Game template descriptor changed; update the SDK descriptor fixture");
    File.WriteAllText(reflectionProject, reflectionProjectText.Replace(sampleModuleEntry,
        sampleModuleEntry + ",\n    { \"name\": \"GameCore\", \"type\": \"Runtime\", \"loadingPhase\": \"Default\" }", StringComparison.Ordinal));
    var coreRules = Path.Combine(reflectionGame, "GameCore.Build.cs");
    const string coreRulesText = """
        using Rocks.Build;
        using Rocks.Build.Rules;

        public sealed class GameCoreModule : ModuleRules
        {
            public override NativeModuleInput Configure(BuildContext context) => new()
            {
                Name = "GameCore",
                PublicIncludeDirectories = [context.PathInRoot("Core/Public")],
                Dependencies = ["ReflectionTestFixtures", "CoreTypeBindings"],
                Metadata = ["GameCore/CoreThing.h", "GameCore/Extra.h"],
                ModuleDirectory = "Core"
            };
        }

        """;
    File.WriteAllText(coreRules, coreRulesText);
    Directory.CreateDirectory(Path.Combine(reflectionGame, "Core/Public/GameCore"));
    var coreHeader = Path.Combine(reflectionGame, "Core/Public/GameCore/CoreThing.h");
    // B5c: 배열 필드(Array<int32>·ObjectRefArray·Array<rocks::Vec3>·참조 struct의 StructRefArray)를 SDK descriptor import와 함께
    // 쓰고, 상속한 FixtureChild가 그래프로 저장·복원한다(Docs/Design/ReflectionFoundations.ko.md 5.6절 SDK 시험).
    const string coreHeaderText = """
        #pragma once

        #include <Core/Containers/Array.h>
        #include <Core/Math/Vector3.h>
        #include <Meta/Annotations.h>
        #include <Object/ObjectRefArray.h>
        #include <Object/StructRefArray.h>
        #include <ReflectionTestFixtures/ArrayFixture.h>
        #include <ReflectionTestFixtures/ReflectionFixture.h>

        namespace sample {

        ROCKS_STRUCT()
        struct Socket {
            explicit Socket(rocks::Allocator& allocator) noexcept : name(allocator) {
            }

            ROCKS_PROPERTY() rocks::ObjectRef<rocks::reflection_fixture::Node> target;
            ROCKS_PROPERTY() rocks::String name;
        };

        ROCKS_CLASS()
        struct CoreThing : rocks::reflection_fixture::Base {
            explicit CoreThing(rocks::Allocator& allocator) noexcept
                : levels(allocator), links(allocator), path(allocator), sockets(allocator), fixtureSlots(allocator) {
            }

            ROCKS_PROPERTY() rocks::int32 level = 1;
            ROCKS_PROPERTY() rocks::Vec3 offset;
            ROCKS_PROPERTY() rocks::Array<rocks::int32> levels;
            ROCKS_PROPERTY() rocks::ObjectRefArray<rocks::reflection_fixture::Node> links;
            ROCKS_PROPERTY() rocks::Array<rocks::Vec3> path;
            ROCKS_PROPERTY() rocks::StructRefArray<Socket> sockets;
            // 설치 SDK의 참조 struct 원소다(SDK 생성 헤더의 GeneratedType<ArraySlot>::Trace가 이 게임에서 인스턴스화된다).
            ROCKS_PROPERTY() rocks::StructRefArray<rocks::reflection_fixture::ArraySlot> fixtureSlots;
        };

        } // namespace sample

        """;
    File.WriteAllText(coreHeader, coreHeaderText);
    File.WriteAllText(Path.Combine(reflectionGame, "Core/Public/GameCore/Extra.h"), """
        #pragma once

        #include <Meta/Annotations.h>
        #include <Object/Object.h>

        namespace sample {

        ROCKS_CLASS()
        struct Extra final : rocks::Object {};

        } // namespace sample

        """);
    var childHeader = Path.Combine(reflectionGame, "Source/FixtureChild.h");
    const string childHeaderText = """
        #pragma once

        #include <GameCore/CoreThing.h>
        #include <Meta/Annotations.h>

        namespace sample {

        ROCKS_CLASS()
        struct FixtureChild final : CoreThing {
            explicit FixtureChild(rocks::Allocator& allocator) noexcept : CoreThing(allocator) {
            }

            ROCKS_PROPERTY() rocks::ObjectRef<rocks::reflection_fixture::Node> link;
        };

        } // namespace sample

        """;
    File.WriteAllText(childHeader, childHeaderText);
    File.WriteAllText(Path.Combine(reflectionGame, "Source/Main.cpp"), """
        #include <SampleGame/SampleGame.module.generated.h>
        #include <Core/Memory/SystemAllocator.h>
        #include <Object/ConstructionContext.h>
        #include <Object/PropertyPath.h>
        #include <ObjectSerialization/ObjectGraph.h>

        // 경로 편집 종류다.
        enum class PathEdit { Write, Insert, Remove };

        // 문자열 경로 text를 type 스키마로 해석해 편집한다: Write는 value를 쓰고, Insert는 value를 넣으며(nullptr이면 기본 원소),
        // Remove는 원소를 지우고 단일 값 원소면 value(FieldValue*로 받는다)에 지운 값을 담는다. 성공하면 true다.
        bool EditPath(rocks::ObjectRegistry& objects, const rocks::TypeLease& type, rocks::ObjectHandle handle, const char* text,
            rocks::FieldValue* value, PathEdit edit) {
            rocks::PropertyPathBuffer path;
            if (!rocks::ParsePropertyPath(type, rocks::StringView(text), path)) {
                return false;
            }

            switch (edit) {
            case PathEdit::Write:
                return static_cast<bool>(rocks::PathWrite(objects, objects.OwnerAccess(), handle, path.View(), *value));
            case PathEdit::Insert:
                return static_cast<bool>(rocks::PathInsertElement(objects, objects.OwnerAccess(), handle, path.View(), value));
            default:
                return static_cast<bool>(rocks::PathRemoveElement(objects, objects.OwnerAccess(), handle, path.View(), value));
            }
        }

        bool SameBytes(rocks::Span<const rocks::uint8> left, rocks::Span<const rocks::uint8> right) {
            if (left.Size() != right.Size()) {
                return false;
            }

            for (rocks::usize index = 0; index < left.Size(); ++index) {
                if (left[index] != right[index]) {
                    return false;
                }
            }

            return true;
        }

        // B6b: 다른 모듈(GameCore)에서 상속한 필드를 이 게임 모듈의 생성 path thunk로 쓴다 — 상속 배열 원소 교체, StructRefArray
        // 원소의 멤버, SDK Binds 미러(Vec3) 멤버, 설치 SDK struct(ArraySlot) 원소의 멤버, ObjectRefArray 원소, Array<Vec3> 기본 원소
        // 삽입과 그 멤버, 상속 Array<int32> 원소 삭제(removed). 성공하면 true다.
        bool EditInheritedFields(rocks::Allocator& allocator, rocks::TypeRegistry& types, rocks::ObjectRegistry& objects,
            rocks::ObjectHandle handle, rocks::ObjectHandle node) {
            auto type = types.Acquire(rocks::TypeIdOf<sample::FixtureChild>());
            if (!type) {
                return false;
            }

            rocks::FieldValue level(allocator);
            level.kind = rocks::ValueKind::Int32;
            level.signedInteger = -9;
            rocks::FieldValue hand(allocator);
            hand.kind = rocks::ValueKind::Utf8String;
            rocks::FieldValue slot(allocator);
            slot.kind = rocks::ValueKind::Utf8String;
            rocks::FieldValue y(allocator);
            y.kind = rocks::ValueKind::Float32;
            y.float32 = 2.5f;
            rocks::FieldValue x(allocator);
            x.kind = rocks::ValueKind::Float32;
            x.float32 = 7.0f;
            rocks::FieldValue link(allocator);
            link.kind = rocks::ValueKind::ObjectReference;
            link.reference = node;
            if (!hand.string.TryAssign("path hand") || !slot.string.TryAssign("path fixture")) {
                return false;
            }

            rocks::FieldValue removed(allocator);
            const rocks::TypeLease& lease = type.Value();
            return EditPath(objects, lease, handle, "levels[1]", &level, PathEdit::Write) &&
                EditPath(objects, lease, handle, "sockets[0].name", &hand, PathEdit::Write) &&
                EditPath(objects, lease, handle, "offset.y", &y, PathEdit::Write) &&
                EditPath(objects, lease, handle, "fixtureSlots[0].name", &slot, PathEdit::Write) &&
                EditPath(objects, lease, handle, "links[1]", &link, PathEdit::Write) &&
                EditPath(objects, lease, handle, "path[1]", nullptr, PathEdit::Insert) &&
                EditPath(objects, lease, handle, "path[1].x", &x, PathEdit::Write) &&
                EditPath(objects, lease, handle, "levels[0]", &removed, PathEdit::Remove) &&
                removed.kind == rocks::ValueKind::Int32 && removed.signedInteger == 3;
        }

        // GameCore에서 상속한 배열 필드(Array<int32>·ObjectRefArray·Array<rocks::Vec3>·StructRefArray, 설치 SDK의 참조 struct
        // 원소 StructRefArray<ArraySlot>)를 source에서 저장해 target으로 복원한다. 배열 원소(ObjectRefArray 원소·StructRefArray
        // 원소 안 참조)로만 닿는 SDK 객체(Node 둘·ArrayOwner 하나)가 수집에서 살아야 한다. 저장 전에 일부 필드를 경로로 바꾼다
        // (EditInheritedFields). 성공하면 0, 실패하면 단계 번호다(root는 돌아갈 때 놓는다).
        int RoundTripArrays(rocks::Allocator& allocator, rocks::TypeRegistry& types, rocks::ObjectRegistry& source, rocks::ObjectRegistry& target) {
            using rocks::reflection_fixture::ArrayOwner;
            using rocks::reflection_fixture::Node;
            rocks::ConstructionContext transaction(source, source.OwnerAccess());
            auto thing = transaction.Create<sample::FixtureChild>(rocks::TypeIdOf<sample::FixtureChild>());
            auto first = transaction.Create<Node>(rocks::TypeIdOf<Node>());
            auto second = transaction.Create<Node>(rocks::TypeIdOf<Node>());
            auto third = transaction.Create<ArrayOwner>(rocks::TypeIdOf<ArrayOwner>());
            if (!thing || !first || !second || !third) {
                return 8;
            }

            auto value = transaction.Get<sample::FixtureChild>(thing.Value());
            if (!value) {
                return 8;
            }

            sample::FixtureChild& child = *value.Value();
            const rocks::ObjectHandle links[] = {first.Value(), rocks::ObjectHandle{}};
            if (!child.levels.TryPushBack(3) || !child.levels.TryPushBack(-4) || !child.path.TryPushBack(rocks::Vec3(1.0f, 2.0f, 3.0f)) ||
                !transaction.AssignReferences(thing.Value(), child.links, links) || !transaction.AssignElements(thing.Value(), child.sockets, 1) ||
                !child.sockets[0].name.TryAssign("hand") ||
                !transaction.SetElementReference(thing.Value(), child.sockets, 0, child.sockets[0].target, second.Value()) ||
                !transaction.AssignElements(thing.Value(), child.fixtureSlots, 1) || !child.fixtureSlots[0].name.TryAssign("fixture") ||
                !transaction.SetElementReference(thing.Value(), child.fixtureSlots, 0, child.fixtureSlots[0].material, third.Value())) {
                return 9;
            }

            auto root = transaction.Commit();
            if (!root) {
                return 10;
            }

            if (!EditInheritedFields(allocator, types, source, root.Value().Handle(), first.Value())) {
                return 17;
            }

            auto bytes = rocks::SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            if (!bytes) {
                return 11;
            }

            auto loaded = rocks::LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bytes.Value().View());
            if (!loaded) {
                return 12;
            }

            {
                auto borrowed = target.Resolve<sample::FixtureChild>(target.OwnerAccess(), loaded.Value().Handle());
                if (!borrowed) {
                    return 13;
                }

                auto restored = borrowed.Value().Get();
                if (!restored) {
                    return 13;
                }

                // levels는 경로로 [3, -9]를 만든 뒤 앞 원소를 지웠고, links[1]은 links[0]과 같은 Node를 가리키게 바꿨다.
                const sample::FixtureChild& copy = *restored.Value();
                if (copy.levels.Size() != 1 || copy.levels[0] != -9 || copy.path.Size() != 2 || copy.path[0].z != 3.0f ||
                    copy.path[1].x != 7.0f || copy.path[1].y != 0.0f || copy.offset.y != 2.5f || copy.links.Size() != 2 ||
                    copy.links[0] == rocks::ObjectHandle{} || copy.links[1] != copy.links[0] || copy.sockets.Size() != 1 ||
                    copy.sockets[0].name.View() != rocks::StringView("path hand") || copy.sockets[0].target.Handle() == rocks::ObjectHandle{} ||
                    copy.fixtureSlots.Size() != 1 || copy.fixtureSlots[0].name.View() != rocks::StringView("path fixture") ||
                    copy.fixtureSlots[0].material.Handle() == rocks::ObjectHandle{}) {
                    return 14;
                }
            }

            // 복원한 그래프를 다시 저장한 byte가 경로로 편집해 저장한 byte와 같다.
            auto resaved = rocks::SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            if (!resaved || !SameBytes(bytes.Value().View(), resaved.Value().View())) {
                return 18;
            }

            if (!target.Collect(target.OwnerAccess()) || target.Stats().liveObjects != 4) {
                return 15;
            }

            return 0;
        }

        int SaveAndLoadArrays(rocks::Allocator& allocator, rocks::TypeRegistry& types) {
            rocks::ObjectRegistry source(allocator, types, {0x5347, 1});
            rocks::ObjectRegistry target(allocator, types, {0x5347, 2});
            const int status = RoundTripArrays(allocator, types, source, target);
            const bool closed = source.Shutdown(source.OwnerAccess()) && target.Shutdown(target.OwnerAccess());
            return status != 0 ? status : closed ? 0 : 16;
        }

        // 게임 집계 헤더가 GameCore와 SDK 모듈(ReflectionTestFixtures)의 RegisterModuleTypes를 먼저 부르고, 게임 타입의
        // schema가 두 모듈 타입의 상속 필드와 SDK 타입을 가리키는 참조 대상 ID를 담는다.
        int main() {
            rocks::SystemAllocator allocator;
            rocks::TypeRegistry types(allocator);
            rocks::ModuleRegistration registration(types, allocator);
            if (!rocks::generated::SampleGame::RegisterModuleTypes(registration)) {
                return 1;
            }
            const rocks::TypeId base = rocks::TypeIdOf<rocks::reflection_fixture::Base>();
            const rocks::TypeId core = rocks::TypeIdOf<sample::CoreThing>();
            auto child = types.Acquire(rocks::TypeIdOf<sample::FixtureChild>());
            if (!types.Acquire(base) || !child || !child.Value().IsA(core) || !child.Value().IsA(base)) {
                return 2;
            }
            // 필드는 상속 포함 안정 ID 순이다. 상속 필드는 CoreThing schema와 같은 ID로, 자기 필드는 SDK 타입을 가리킨다.
            const auto& schema = child.Value().Schema();
            const auto& inherited = rocks::TypeOf<sample::CoreThing>().fields;
            if (schema.baseType != core || schema.fields.Size() != inherited.Size() + 1) {
                return 3;
            }
            auto find = [&](const rocks::Guid& id) noexcept -> const rocks::FieldSchema* {
                for (const auto& field : schema.fields) {
                    if (field.id == id) {
                        return &field;
                    }
                }
                return nullptr;
            };
            for (const auto& field : inherited) {
                if (!find(field.id)) {
                    return 4;
                }
            }
            // SDK의 CoreTypeBindings가 Core 값 타입을 먼저 등록하고, GameCore의 rocks::Vec3 필드가 그 struct를 가리킨다.
            const rocks::TypeId vec3 = rocks::TypeIdOf<rocks::Vec3>();
            if (!types.AcquireValueType(vec3, rocks::TypeKind::Struct)) {
                return 6;
            }
            bool vectorField = false;
            for (const auto& field : inherited) {
                vectorField = vectorField || (field.value.kind == rocks::ValueKind::Struct && field.value.type == vec3);
            }
            if (!vectorField) {
                return 7;
            }

            bool nodeReference = false;
            for (const auto& field : schema.fields) {
                nodeReference = nodeReference || field.value.type == rocks::TypeIdOf<rocks::reflection_fixture::Node>();
            }

            if (!nodeReference) {
                return 5;
            }

            return SaveAndLoadArrays(allocator, types);
        }

        """);
    var coreManifest = Path.Combine(reflectionGame, "Core/GameCore.rocksids.json");
    var coreIds = await Ids("add", "--manifest", coreManifest, "--type", "sample::CoreThing", "--field", "sample::CoreThing::level",
        "--field", "sample::CoreThing::offset", "--field", "sample::CoreThing::levels", "--field", "sample::CoreThing::links",
        "--field", "sample::CoreThing::path", "--field", "sample::CoreThing::sockets", "--field", "sample::CoreThing::fixtureSlots", "--type", "sample::Socket",
        "--field", "sample::Socket::target", "--field", "sample::Socket::name", "--type", "sample::Extra");
    Require(coreIds.ExitCode == 0, "Could not register the GameCore fixture identities: " + coreIds.StdErr);
    var fixtureIds = await Ids("add", "--manifest", Path.Combine(reflectionGame, "SampleGame.rocksids.json"),
        "--type", "sample::FixtureChild", "--field", "sample::FixtureChild::link");
    Require(fixtureIds.ExitCode == 0, "Could not register the SDK descriptor fixture identities: " + fixtureIds.StdErr);
    Task<ProcessResult> ReflectionGame() => Cli("game", "--sdk", reflectionSdk, "--project", reflectionGame, "--configuration", "Debug");
    var reflectionBuilt = await ReflectionGame();
    Require(reflectionBuilt.ExitCode == 0, "Game importing an SDK descriptor failed: " + reflectionBuilt.StdOut + reflectionBuilt.StdErr);
    var reflectionResult = JsonSerializer.Deserialize<NativeBuildResult>(File.ReadAllText(Path.Combine(reflectionOutput, "action-report.json")))!;
    Require(reflectionResult.Succeeded && reflectionResult.Actions.All(action => action.Module is "SampleGame" or "GameCore"),
        "Game importing an SDK descriptor compiled an SDK module");
    Require(!Directory.Exists(Path.Combine(reflectionOutput, "Generated/ReflectionTestFixtures")) &&
        !Directory.Exists(Path.Combine(reflectionOutput, "Generated/CoreTypeBindings")),
        "Game build regenerated the SDK module's metadata");
    var reflectionRun = await ProcessRunner.RunAsync(new(reflectionResult.ModuleOutputs["SampleGame"], [], reflectionGame, toolchain.Environment));
    Require(reflectionRun.ExitCode == 0, $"SDK descriptor import runtime check failed with exit code {reflectionRun.ExitCode}");

    // 상위 모듈(SampleGame)이 실패하면 하위 모듈(GameCore)의 새 생성물도 게시하지 않는다.
    var coreGenerated = Path.Combine(reflectionOutput, "Generated/GameCore/Public/GameCore/CoreThing.generated.h");
    var coreDescriptor = Path.Combine(reflectionOutput, "Metadata/GameCore.rocksmeta.json");
    string coreGeneratedHash = Hashing.Sha256File(coreGenerated);
    string coreDescriptorHash = Hashing.Sha256File(coreDescriptor);
    File.WriteAllText(coreHeader, coreHeaderText.Replace("rocks::int32 level = 1;", "rocks::int32 level = 1;\n    ROCKS_PROPERTY() rocks::int32 rank = 0;", StringComparison.Ordinal));
    var rankIds = await Ids("add", "--manifest", coreManifest, "--field", "sample::CoreThing::rank");
    Require(rankIds.ExitCode == 0, "Could not register the GameCore rank identity: " + rankIds.StdErr);
    File.WriteAllText(childHeader, childHeaderText.Replace("ROCKS_PROPERTY()", "ROCKS_PROPERTY(NoSuchSpecifier)", StringComparison.Ordinal));
    var upperFailed = await ReflectionGame();
    Require(upperFailed.ExitCode != 0 && upperFailed.StdErr.Contains("RMC2000", StringComparison.Ordinal),
        "Invalid dependent module metadata was accepted: " + upperFailed.StdErr);
    Require(Hashing.Sha256File(coreGenerated) == coreGeneratedHash && Hashing.Sha256File(coreDescriptor) == coreDescriptorHash,
        "A failed dependent module published its dependency's staged metadata");
    File.WriteAllText(childHeader, childHeaderText);
    Require((await ReflectionGame()).ExitCode == 0, "Game did not recover after the dependent module metadata failure");
    Require(Hashing.Sha256File(coreGenerated) != coreGeneratedHash, "GameCore metadata change was not published after recovery");

    // Metadata에서 뺀 헤더의 생성물은 게시 트랜잭션이 지운다(ID는 은퇴시켜 예약한다).
    var extraGenerated = Path.Combine(reflectionOutput, "Generated/GameCore/Public/GameCore/Extra.generated.h");
    Require(File.Exists(extraGenerated), "GameCore Extra metadata was not generated");
    File.WriteAllText(coreRules, coreRulesText.Replace(", \"GameCore/Extra.h\"", "", StringComparison.Ordinal));
    var extraRetired = await Ids("retire", "--manifest", coreManifest, "--kind", "type", "--name", "sample::Extra");
    Require(extraRetired.ExitCode == 0, "Could not retire the GameCore Extra identity: " + extraRetired.StdErr);
    Require((await ReflectionGame()).ExitCode == 0, "Game failed after removing a header from Metadata");
    Require(!File.Exists(extraGenerated), "Generated header of a header removed from Metadata was not deleted");
    Require(!Directory.Exists(Path.Combine(reflectionOutput, ".metadata-backup")), "Metadata publication left backup files");
    Pass("sdk-descriptor-import-and-module-chain", timer.Elapsed);

    timer.Restart();
    var evidencePath = Path.Combine(output, "game-build-evidence.json");
    InstalledSdk.BeginGameEvidence(output, sdk, game);
    using (var running = JsonDocument.Parse(File.ReadAllText(evidencePath)))
    {
        Require(running.RootElement.GetProperty("status").GetString() == "RUNNING", "Old game PASS remained when new execution began");
    }

    Require((await Game()).ExitCode == 0, "Game did not finish after interrupted marker");
    using (var completed = JsonDocument.Parse(File.ReadAllText(evidencePath)))
    {
        Require(completed.RootElement.GetProperty("status").GetString() == "PASS", "Game completion did not replace RUNNING marker");
    }

    Pass("game-running-evidence-and-completion", timer.Elapsed);

    timer.Restart();
    var invalidOutput = await Cli("game", "--sdk", sdk, "--project", game, "--configuration", "Debug", "--output", game);
    Require(invalidOutput.ExitCode != 0 && !File.Exists(Path.Combine(game, "action-report.json")),
        "Output overlapping project was accepted or wrote into the source root");
    Pass("game-source-output-overlap", timer.Elapsed);

    timer.Restart();
    var gameSnapshot = GameInputSnapshot.Capture(game, output, sdk, []);
    var reportPath = Path.Combine(output, "action-report.json");
    AtomicFile.WriteIfChanged(reportPath, JsonSerializer.Serialize(initial with { Succeeded = false, Failure = "current run pending" }));
    var changedSource = Path.Combine(game, "Source/GameObject.h");
    var oldSource = File.ReadAllText(changedSource);
    try
    {
        File.WriteAllText(changedSource, oldSource.Replace("체력", "변경된 이름", StringComparison.Ordinal));
        try
        {
            InstalledSdk.PublishVerifiedGameResult(sdk, NativeConfiguration.Debug, toolchain, game, output,
                gameSnapshot, gameSnapshot, [], initial);
            throw new InvalidOperationException("Changed metadata input was accepted");
        }
        catch (InvalidOperationException error) when (error.Message.Contains("Game inputs changed", StringComparison.Ordinal)) { }

        Require(!ReadActionReport().Succeeded, "Input mutation published a successful action report");
    }
    finally
    {
        File.WriteAllText(changedSource, oldSource);
    }

    Pass("game-input-mutation-does-not-publish-success", timer.Elapsed);
    timer.Restart();
    Require((await Game()).ExitCode == 0, "No-op game build failed");
    Require(ReadActionReport().ExecutedActions == 0, "No-op game recompiled/relinked native actions");
    Pass("game-no-op", timer.Elapsed);

    timer.Restart();
    File.AppendAllText(Path.Combine(game, "Source/Main.cpp"), "\nint RocksBuildGameMarker() noexcept { return 123; }\n");
    Require((await Game()).ExitCode == 0, "Game source edit build failed");
    var edited = ReadActionReport();
    // Main.cpp, BuildInfo 생성 TU(도구 빌드 ID가 바뀜), link.
    Require(edited.ExecutedActions == 3 && edited.Actions.Where(action => action.Executed).All(action => action.Module == "SampleGame"),
        "Game edit must compile one game source and the BuildInfo TU and relink without engine actions");
    Require(originalFiles.All(pair => Hashing.Sha256File(pair.Key) == pair.Value), "Game build modified installed SDK");
    Pass("game-edit-engine-reuse", timer.Elapsed);

    timer.Restart();
    // 모듈 소유 메타데이터: 잘못된 선언은 메타 생성 전에 명확한 진단으로 거부하고 기존 생성물을 바꾸지 않는다.
    var moduleRulesPath = Path.Combine(game, "Game.Build.cs");
    var validModuleRules = File.ReadAllText(moduleRulesPath);
    var generatedHeader = Path.Combine(output, "Generated/SampleGame/Public/GameObject.generated.h");
    var validGeneratedHash = Hashing.Sha256File(generatedHeader);
    var manifestPath = Path.Combine(game, "SampleGame.rocksids.json");
    var movedManifest = manifestPath + ".moved";
    try
    {
        // Metadata를 지우면 활성 항목이 남은 명세가 고아가 된다(RMC2102).
        var orphan = validModuleRules.Replace("        Metadata = [\"GameObject.h\"],\n", "", StringComparison.Ordinal);
        Require(orphan != validModuleRules, "Module rule fixture no longer matches (Metadata)");
        File.WriteAllText(moduleRulesPath, orphan);
        var rejected = await Game();
        Require(rejected.ExitCode != 0 && (rejected.StdOut + rejected.StdErr).Contains("RMC2102", StringComparison.Ordinal),
            "Orphan identity manifest with active entries was not rejected: " + rejected.StdErr);
        Require(Hashing.Sha256File(generatedHeader) == validGeneratedHash, "Orphan manifest rejection modified generated output");

        // 같은 헤더를 두 번 소유할 수 없다.
        var duplicate = validModuleRules.Replace("Metadata = [\"GameObject.h\"]", "Metadata = [\"GameObject.h\", \"GameObject.h\"]", StringComparison.Ordinal);
        File.WriteAllText(moduleRulesPath, duplicate);
        rejected = await Game();
        Require(rejected.ExitCode != 0 && (rejected.StdOut + rejected.StdErr).Contains("duplicate input ownership", StringComparison.Ordinal),
            "Duplicate metadata header ownership was not rejected: " + rejected.StdErr);
        Require(Hashing.Sha256File(generatedHeader) == validGeneratedHash, "Duplicate ownership published metadata before rejection");

        // Metadata가 있는데 명세가 없으면 RMC2100이다.
        File.WriteAllText(moduleRulesPath, validModuleRules);
        File.Move(manifestPath, movedManifest);
        rejected = await Game();
        Require(rejected.ExitCode != 0 && (rejected.StdOut + rejected.StdErr).Contains("RMC2100", StringComparison.Ordinal),
            "Missing module identity manifest was not rejected: " + rejected.StdErr);
        Require(Hashing.Sha256File(generatedHeader) == validGeneratedHash, "Missing manifest modified generated output");
    }
    finally
    {
        File.WriteAllText(moduleRulesPath, validModuleRules);
        if (File.Exists(movedManifest))
        {
            File.Move(movedManifest, manifestPath, true);
        }
    }

    Require((await Game()).ExitCode == 0, "Game did not recover after correcting module metadata ownership");
    Pass("metadata-module-ownership", timer.Elapsed);
    timer.Restart();
    var incompatible = await Cli("game", "--sdk", sdk, "--project", game, "--configuration", "Release");
    Require(incompatible.ExitCode != 0 && incompatible.StdErr.Contains("mismatch", StringComparison.OrdinalIgnoreCase),
        "Incompatible SDK configuration was silently accepted");
    Pass("sdk-configuration-mismatch", timer.Elapsed);

    // B7 이전 SDK(manifest 버전 3)는 생성 TU의 Core/Build/BuildInfo.h include가 깨지기 전에 버전 불일치로 거부한다.
    timer.Restart();
    var sdkManifestPath = Path.Combine(sdk, "rocks-sdk.json");
    var sdkManifestBytes = File.ReadAllBytes(sdkManifestPath);
    var sdkManifestText = File.ReadAllText(sdkManifestPath);
    Require(sdkManifestText.Contains("\"Version\": 4,", StringComparison.Ordinal), "SDK manifest is not version 4:\n" + sdkManifestText);
    SetReadOnly(sdkManifestPath, false);
    try
    {
        File.WriteAllText(sdkManifestPath, sdkManifestText.Replace("\"Version\": 4,", "\"Version\": 3,", StringComparison.Ordinal));
        var preB7 = await Game();
        Require(preB7.ExitCode != 0 && preB7.StdErr.Contains("SDK manifest version mismatch: 3", StringComparison.Ordinal),
            "A pre-B7 SDK manifest was not rejected: " + preB7.StdErr);
    }
    finally
    {
        File.WriteAllBytes(sdkManifestPath, sdkManifestBytes);
        SetReadOnly(sdkManifestPath, true);
    }

    Pass("sdk-pre-b7-manifest-rejected", timer.Elapsed);

    timer.Restart();
    var library = Directory.GetFiles(Path.Combine(sdk, "Libraries"))[0];
    SetReadOnly(library, false);
    var original = File.ReadAllBytes(library);
    try
    {
        var mutated = (byte[])original.Clone();
        mutated[0] ^= 0xff;
        File.WriteAllBytes(library, mutated);
        var corruptSnapshot = GameInputSnapshot.Capture(game, output, sdk, []);
        AtomicFile.WriteIfChanged(reportPath, JsonSerializer.Serialize(initial with { Succeeded = false, Failure = "current run pending" }));
        try
        {
            InstalledSdk.PublishVerifiedGameResult(sdk, NativeConfiguration.Debug, toolchain, game, output,
                corruptSnapshot, corruptSnapshot, [], initial);
            throw new InvalidOperationException("Changed SDK was accepted by publication");
        }
        catch (InvalidDataException error) when (error.Message.Contains("changed or missing", StringComparison.Ordinal)) { }

        Require(!ReadActionReport().Succeeded, "SDK mutation published a successful action report");
        var corrupt = await Game();
        Require(corrupt.ExitCode != 0 && corrupt.StdErr.Contains("changed or missing", StringComparison.Ordinal), "SDK library corruption not rejected");
    }
    finally
    {
        File.WriteAllBytes(library, original);
        SetReadOnly(library, true);
    }

    var additional = Path.Combine(sdk, "Headers", "Unexpected.h");
    try
    {
        File.WriteAllText(additional, "#pragma once\n");
        var injected = await Game();
        Require(injected.ExitCode != 0 && injected.StdErr.Contains("inventory changed", StringComparison.Ordinal), "Untracked SDK header not rejected");
    }
    finally
    {
        File.Delete(additional);
    }

    Pass("sdk-content-and-inventory-integrity", timer.Elapsed);

    timer.Restart();
    var header = Path.Combine(game, "Source/GameObject.h");
    var originalHeader = File.ReadAllText(header);
    var generated = Path.Combine(output, "Generated/SampleGame/Public/GameObject.generated.h");
    var generatedHash = Hashing.Sha256File(generated);
    try
    {
        File.WriteAllText(header, originalHeader.Replace("ReadOnly", "ReadOnyl", StringComparison.Ordinal));
        var rejected = await Game();
        Require(rejected.ExitCode != 0 && rejected.StdErr.Contains("ReadOnyl", StringComparison.Ordinal) && rejected.StdErr.Contains("hint:", StringComparison.Ordinal),
            "Author typo lacks an actionable rejection");
        Require(Hashing.Sha256File(generated) == generatedHash, "Failed author validation published partial metadata");
    }
    finally
    {
        File.WriteAllText(header, originalHeader);
    }

    Require((await Game()).ExitCode == 0, "Build failed after correcting the author typo");
    Pass("author-error-and-recovery", timer.Elapsed);

    // 프로젝트 기술자(.rocksproject)가 없거나 규칙 모듈을 선언하지 않으면 빌드를 시작하지 않는다.
    timer.Restart();
    var descriptor = Path.Combine(game, "Game.rocksproject");
    var descriptorText = File.ReadAllText(descriptor);
    try
    {
        File.Delete(descriptor);
        var missing = await Game();
        Require(missing.ExitCode != 0 && (missing.StdOut + missing.StdErr).Contains("exactly one .rocksproject", StringComparison.Ordinal),
            "Game without a project descriptor was not rejected: " + missing.StdErr);
        File.WriteAllText(descriptor, descriptorText.Replace("\"SampleGame\"", "\"OtherGame\"", StringComparison.Ordinal));
        var undeclared = await Game();
        Require(undeclared.ExitCode != 0 && (undeclared.StdOut + undeclared.StdErr).Contains("has rules but no descriptor declares it", StringComparison.Ordinal),
            "Undeclared rules module was not rejected: " + undeclared.StdErr);
        File.WriteAllText(descriptor, descriptorText.Replace("\"MacOS\"", "\"Android\"", StringComparison.Ordinal)
            .Replace("\"Windows\"", "\"Android\"", StringComparison.Ordinal).Replace("[\"Android\", \"Android\"]", "[\"Android\"]", StringComparison.Ordinal));
        var untargeted = await Game();
        Require(untargeted.ExitCode != 0 && (untargeted.StdOut + untargeted.StdErr).Contains("does not target", StringComparison.Ordinal),
            "Game built for a platform outside targetPlatforms: " + untargeted.StdErr);
    }
    finally
    {
        File.WriteAllText(descriptor, descriptorText);
    }

    Require((await Game()).ExitCode == 0, "Build failed after restoring the project descriptor");
    Pass("project-descriptor-validates-modules-and-platforms", timer.Elapsed);

    // 프로젝트 플러그인: 꺼진 플러그인과 Editor 모듈은 컴파일하지 않고, 켜면 타깃이 참조하지 않아도 빌드한다.
    timer.Restart();
    var pluginRoot = Path.Combine(game, "Plugins", "Extras");
    try
    {
        Directory.CreateDirectory(Path.Combine(pluginRoot, "Source", "Extras", "Private"));
        Directory.CreateDirectory(Path.Combine(pluginRoot, "Source", "ExtrasEditor", "Private"));
        File.WriteAllText(Path.Combine(pluginRoot, "Extras.rocksplugin"), """
            { "fileVersion": 1, "version": 1, "versionName": "1.0", "friendlyName": "Extras",
              "modules": [ { "name": "Extras", "type": "Runtime" }, { "name": "ExtrasEditor", "type": "Editor" } ] }
            """);
        File.WriteAllText(Path.Combine(pluginRoot, "Source", "Extras", "Extras.Build.cs"), """
            using Rocks.Build;
            using Rocks.Build.Rules;

            public sealed class ExtrasModule : ModuleRules
            {
                public override NativeModuleInput Configure(BuildContext context) => new()
                {
                    Name = "Extras", Sources = context.Sources("Plugins/Extras/Source/Extras/Private")
                };
            }

            public sealed class ExtrasEditorModule : ModuleRules
            {
                public override NativeModuleInput Configure(BuildContext context) => new()
                {
                    Name = "ExtrasEditor", Sources = context.Sources("Plugins/Extras/Source/ExtrasEditor/Private")
                };
            }
            """);
        File.WriteAllText(Path.Combine(pluginRoot, "Source", "Extras", "Private", "Extras.cpp"),
            "int ExtrasValue() noexcept;\nint ExtrasValue() noexcept { return 3; }\n");
        File.WriteAllText(Path.Combine(pluginRoot, "Source", "ExtrasEditor", "Private", "ExtrasEditor.cpp"),
            "#error \"an Editor module must not be compiled for a game target\"\n");

        var disabled = await Game();
        Require(disabled.ExitCode == 0, "Game with a disabled plugin failed: " + disabled.StdErr);
        Require(!ReadActionReport().Actions.Any(action => action.Module is "Extras" or "ExtrasEditor"),
            "A disabled plugin module was built");

        File.WriteAllText(descriptor, descriptorText.Replace("\"targetPlatforms\"",
            "\"plugins\": [ { \"name\": \"Extras\", \"enabled\": true } ],\n  \"targetPlatforms\"", StringComparison.Ordinal));
        var enabled = await Game();
        Require(enabled.ExitCode == 0, "Game with an enabled plugin failed: " + enabled.StdOut + enabled.StdErr);
        var enabledReport = ReadActionReport();
        Require(enabledReport.Actions.Any(action => action.Module == "Extras" && action.Kind == "Archive" && action.Executed),
            "An enabled plugin module was not built");
        Require(!enabledReport.Actions.Any(action => action.Module == "ExtrasEditor"), "An Editor plugin module was built");
    }
    finally
    {
        File.WriteAllText(descriptor, descriptorText);
        Directory.Delete(Path.Combine(game, "Plugins"), true);
    }

    Require((await Game()).ExitCode == 0, "Build failed after removing the project plugin");
    Pass("project-plugins-enable-and-filter-modules", timer.Elapsed);

    // Release SDK: 엔진 라이브러리는 ThinLTO bitcode이고, 게임 Release가 같은 LLVM으로 link해 실행된다.
    timer.Restart();
    var releaseSdk = Path.Combine(fixture, "installed-release");
    var releaseExport = await Cli("sdk", "--root", root, "--target", "Engine", "--configuration", "Release", "--destination", releaseSdk);
    Require(releaseExport.ExitCode == 0, "Release SDK export failed: " + releaseExport.StdErr);
    static bool ContainsBitcode(string path)
    {
        byte[] bytes = File.ReadAllBytes(path);
        for (int index = 0; index + 4 <= bytes.Length; ++index)
        {
            if (bytes[index] == 0x42 && bytes[index + 1] == 0x43 && bytes[index + 2] == 0xc0 && bytes[index + 3] == 0xde)
            {
                return true;
            }
        }

        return false;
    }

    Require(Directory.GetFiles(Path.Combine(releaseSdk, "Libraries")).All(ContainsBitcode),
        "Release SDK libraries are not ThinLTO bitcode archives");
    var releaseGame = await Cli("game", "--sdk", releaseSdk, "--project", game, "--configuration", "Release");
    Require(releaseGame.ExitCode == 0, "Release game build against the Release SDK failed: " + releaseGame.StdOut + releaseGame.StdErr);
    var releaseReport = JsonSerializer.Deserialize<NativeBuildResult>(File.ReadAllText(Path.Combine(game, "out/Release/action-report.json")))!;
    var releaseRun = await ProcessRunner.RunAsync(new(releaseReport.ModuleOutputs["SampleGame"], [], game, toolchain.Environment));
    Require(releaseRun.ExitCode == 0, "Release ThinLTO game failed at runtime");
    Pass("release-thinlto-sdk-and-game", timer.Elapsed);
    success = true;
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    Console.Error.WriteLine("Fixture retained: " + fixture);
    cases.Add(new { name = "failure", status = "FAIL", detail = error.Message });
    return 1;
}
finally
{
    AtomicFile.WriteIfChanged(Path.Combine(reports, "sdk-integration-report.json"), JsonSerializer.Serialize(new
    { status = success ? "PASS" : "FAIL", tests = cases, fixture }, new JsonSerializerOptions { WriteIndented = true }));
    if (success && Directory.Exists(fixture))
    {
        foreach (var file in Directory.GetFiles(fixture, "*", SearchOption.AllDirectories))
        {
            SetReadOnly(file, false);
        }

        Directory.Delete(fixture, recursive: true);
    }
}

async Task<ProcessResult> Cli(params string[] arguments)
{
    var command = new[] { cli }.Concat(arguments).Concat(["--compiler", toolchain.CompilerPath]).ToArray();
    var result = await ProcessRunner.RunAsync(new("dotnet", command, root, toolchain.Environment, Timeout: TimeSpan.FromMinutes(10)));
    File.AppendAllText(Path.Combine(reports, "sdk-integration.log"), string.Join(" ", arguments) + "\n" + result.StdOut + result.StdErr + "\n");
    return result;
}

Task<ProcessResult> Game() => Cli("game", "--sdk", sdk, "--project", game, "--configuration", "Debug");

NativeBuildResult ReadActionReport() => JsonSerializer.Deserialize<NativeBuildResult>(File.ReadAllText(Path.Combine(output, "action-report.json")))!;

void Pass(string name, TimeSpan elapsed)
{
    Console.WriteLine("[PASS] " + name);
    cases.Add(new { name, status = "PASS", durationMs = elapsed.TotalMilliseconds });
}

static void Require(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

static void SetReadOnly(string path, bool value)
{
    if (OperatingSystem.IsWindows())
    {
        File.SetAttributes(path, value ? File.GetAttributes(path) | FileAttributes.ReadOnly : File.GetAttributes(path) & ~FileAttributes.ReadOnly);
    }
    else
    {
        var mode = File.GetUnixFileMode(path);
        File.SetUnixFileMode(path, value ? mode & ~(UnixFileMode.UserWrite | UnixFileMode.GroupWrite | UnixFileMode.OtherWrite) : mode | UnixFileMode.UserWrite);
    }
}

static void CopyTree(string source, string destination)
{
    Directory.CreateDirectory(destination);
    foreach (var file in Directory.GetFiles(source))
    {
        File.Copy(file, Path.Combine(destination, Path.GetFileName(file)));
    }

    foreach (var directory in Directory.GetDirectories(source))
    {
        CopyTree(directory, Path.Combine(destination, Path.GetFileName(directory)));
    }
}
