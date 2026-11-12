using System.Diagnostics;
using System.Text.Json;
using Rocks.Build;
using Rocks.Build.Cli;
using Rocks.Build.IntegrationTests;
using Rocks.Tooling.Common;

if (args.Length != 0 && args[0] == "--output-lock-child") return await OutputInvocationLockTests.ChildAsync(args);
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
    finally { Directory.Delete(temporary, recursive: true); }
}

var options = new Dictionary<string, string>(StringComparer.Ordinal);
for (var index = 0; index < args.Length; index += 2)
{
    if (index + 1 >= args.Length || args[index] is not ("--root" or "--compiler") || !options.TryAdd(args[index], args[index + 1]))
        throw new ArgumentException("Expected unique --root/--compiler options with values.");
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
    MetadataTemporaryCleanupTests.Run();
    await PlatformPrivateHeaderTests.RunAsync(toolchain, Path.Combine(fixture, "platform-header"));
    Pass("output-invocation-lock-and-platform-header-incremental", timer.Elapsed);
    timer.Restart();
    var exported = await Cli("sdk", "--root", root, "--target", "Engine", "--configuration", "Debug", "--destination", sdk);
    Require(exported.ExitCode == 0, "SDK export failed: " + exported.StdErr);
    Require(File.Exists(Path.Combine(sdk, "rocks-sdk.json")), "SDK manifest missing");
    Require(!Directory.GetFiles(sdk, "*.cpp", SearchOption.AllDirectories).Concat(Directory.GetFiles(sdk, "*.c", SearchOption.AllDirectories)).Any(), "Installed SDK contains engine or third-party sources");
    Pass("engine-export", timer.Elapsed);

    CopyTree(Path.Combine(root, "Templates/Game/Foundation"), game);
    var originalFiles = Directory.GetFiles(sdk, "*", SearchOption.AllDirectories)
        .ToDictionary(path => path, Hashing.Sha256File, StringComparer.Ordinal);
    foreach (var file in originalFiles.Keys) SetReadOnly(file, true);
    timer.Restart();
    var built = await Game();
    Require(built.ExitCode == 0, "External game build failed: " + built.StdOut + built.StdErr);
    var initial = ReadActionReport();
    Require(initial.Succeeded && initial.ExecutedActions > 0 && initial.Actions.All(action => action.Module == "SampleGame"),
        "Game build compiled an engine module or failed");
    var run = await ProcessRunner.RunAsync(new(initial.ModuleOutputs["SampleGame"], [], game, toolchain.Environment));
    Require(run.ExitCode == 0, "External game's generated CLASS/PROPERTY runtime failed");
    Pass("read-only-sdk-external-game-reflection", timer.Elapsed);

    // 잘못된 새 ID 발급은 이름 변경을 대체하지 못한다. 실패 시 기존 생성물을 보존한다.
    timer.Restart();
    string gameHeader = Path.Combine(game, "Source/GameObject.h");
    string gameMain = Path.Combine(game, "Source/Main.cpp");
    string gameIdentity = Path.Combine(game, "Game.rocksids.json");
    string generatedIdentityHeader = Path.Combine(output, "Generated/GameObject.generated.h");
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

    timer.Restart();
    var evidencePath = Path.Combine(output, "game-build-evidence.json");
    InstalledSdk.BeginGameEvidence(output, sdk, game);
    using (var running = JsonDocument.Parse(File.ReadAllText(evidencePath)))
        Require(running.RootElement.GetProperty("status").GetString() == "RUNNING", "Old game PASS remained when new execution began");
    Require((await Game()).ExitCode == 0, "Game did not finish after interrupted marker");
    using (var completed = JsonDocument.Parse(File.ReadAllText(evidencePath)))
        Require(completed.RootElement.GetProperty("status").GetString() == "PASS", "Game completion did not replace RUNNING marker");
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
    finally { File.WriteAllText(changedSource, oldSource); }
    Pass("game-input-mutation-does-not-publish-success", timer.Elapsed);
    timer.Restart();
    Require((await Game()).ExitCode == 0, "No-op game build failed");
    Require(ReadActionReport().ExecutedActions == 0, "No-op game recompiled/relinked native actions");
    Pass("game-no-op", timer.Elapsed);

    timer.Restart();
    File.AppendAllText(Path.Combine(game, "Source/Main.cpp"), "\nint RocksBuildGameMarker() noexcept { return 123; }\n");
    Require((await Game()).ExitCode == 0, "Game source edit build failed");
    var edited = ReadActionReport();
    Require(edited.ExecutedActions == 2 && edited.Actions.Where(action => action.Executed).All(action => action.Module == "SampleGame"),
        "Game edit must compile one game source and relink without engine actions");
    Require(originalFiles.All(pair => Hashing.Sha256File(pair.Key) == pair.Value), "Game build modified installed SDK");
    Pass("game-edit-engine-reuse", timer.Elapsed);

    timer.Restart();
    var targetRulesPath = Path.Combine(game, "Game.Target.cs");
    var validTargetRules = File.ReadAllText(targetRulesPath);
    var generatedHeader = Path.Combine(output, "Generated/GameObject.generated.h");
    var validGeneratedHash = Hashing.Sha256File(generatedHeader);
    try
    {
        var wrongOwner = validTargetRules.Replace("[\"SampleGame\"]", "[\"Core\"]", StringComparison.Ordinal);
        Require(wrongOwner != validTargetRules, "Target rule fixture no longer matches");
        File.WriteAllText(targetRulesPath, wrongOwner);
        var rejected = await Game();
        Require(rejected.ExitCode != 0 && rejected.StdErr.Contains("not reachable from target", StringComparison.Ordinal),
            "Metadata unit with unreachable owner was not rejected with a clear diagnostic");
        Require(Hashing.Sha256File(generatedHeader) == validGeneratedHash, "Unreachable owner modified previously generated output");

        var duplicateInput = validTargetRules.Replace(
            "new(\"SampleGame\", \"Source/GameObject.h\", \"Source/GameObject.h\", \"GameObject.generated.h\")",
            "new(\"SampleGame\", \"Source/GameObject.h\", \"Source/GameObject.h\", \"GameObject.generated.h\"), " +
            "new(\"SampleGame\", \"Source/./GameObject.h\", \"Source/GameObject.h\", \"Other.generated.h\")", StringComparison.Ordinal);
        Require(duplicateInput != validTargetRules, "Metadata unit fixture no longer matches");
        File.WriteAllText(targetRulesPath, duplicateInput);
        rejected = await Game();
        Require(rejected.ExitCode != 0 && rejected.StdErr.Contains("duplicate input ownership", StringComparison.Ordinal),
            "Duplicate metadata input with different output was not rejected");
        Require(Hashing.Sha256File(generatedHeader) == validGeneratedHash &&
            !File.Exists(Path.Combine(output, "Generated/Other.generated.h")), "Invalid units published metadata before rejection");
    }
    finally { File.WriteAllText(targetRulesPath, validTargetRules); }
    Require((await Game()).ExitCode == 0, "Game did not recover after correcting metadata unit ownership");
    Pass("metadata-unit-target-and-input-ownership", timer.Elapsed);

    timer.Restart();
    var incompatible = await Cli("game", "--sdk", sdk, "--project", game, "--configuration", "Release");
    Require(incompatible.ExitCode != 0 && incompatible.StdErr.Contains("mismatch", StringComparison.OrdinalIgnoreCase),
        "Incompatible SDK configuration was silently accepted");
    Pass("sdk-configuration-mismatch", timer.Elapsed);

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
    finally { File.WriteAllBytes(library, original); SetReadOnly(library, true); }
    var additional = Path.Combine(sdk, "Headers", "Unexpected.h");
    try
    {
        File.WriteAllText(additional, "#pragma once\n");
        var injected = await Game();
        Require(injected.ExitCode != 0 && injected.StdErr.Contains("inventory changed", StringComparison.Ordinal), "Untracked SDK header not rejected");
    }
    finally { File.Delete(additional); }
    Pass("sdk-content-and-inventory-integrity", timer.Elapsed);

    timer.Restart();
    var header = Path.Combine(game, "Source/GameObject.h");
    var originalHeader = File.ReadAllText(header);
    var generated = Path.Combine(output, "Generated/GameObject.generated.h");
    var generatedHash = Hashing.Sha256File(generated);
    try
    {
        File.WriteAllText(header, originalHeader.Replace("ReadOnly", "ReadOnyl", StringComparison.Ordinal));
        var rejected = await Game();
        Require(rejected.ExitCode != 0 && rejected.StdErr.Contains("ReadOnyl", StringComparison.Ordinal) && rejected.StdErr.Contains("hint:", StringComparison.Ordinal),
            "Author typo lacks an actionable rejection");
        Require(Hashing.Sha256File(generated) == generatedHash, "Failed author validation published partial metadata");
    }
    finally { File.WriteAllText(header, originalHeader); }
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
    finally { File.WriteAllText(descriptor, descriptorText); }
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
            if (bytes[index] == 0x42 && bytes[index + 1] == 0x43 && bytes[index + 2] == 0xc0 && bytes[index + 3] == 0xde)
                return true;
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
        foreach (var file in Directory.GetFiles(fixture, "*", SearchOption.AllDirectories)) SetReadOnly(file, false);
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
static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
static void SetReadOnly(string path, bool value)
{
    if (OperatingSystem.IsWindows())
        File.SetAttributes(path, value ? File.GetAttributes(path) | FileAttributes.ReadOnly : File.GetAttributes(path) & ~FileAttributes.ReadOnly);
    else
    {
        var mode = File.GetUnixFileMode(path);
        File.SetUnixFileMode(path, value ? mode & ~(UnixFileMode.UserWrite | UnixFileMode.GroupWrite | UnixFileMode.OtherWrite) : mode | UnixFileMode.UserWrite);
    }
}
static void CopyTree(string source, string destination)
{
    Directory.CreateDirectory(destination);
    foreach (var file in Directory.GetFiles(source)) File.Copy(file, Path.Combine(destination, Path.GetFileName(file)));
    foreach (var directory in Directory.GetDirectories(source)) CopyTree(directory, Path.Combine(destination, Path.GetFileName(directory)));
}
