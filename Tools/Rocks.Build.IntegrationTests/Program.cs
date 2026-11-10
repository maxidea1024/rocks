using System.Diagnostics;
using System.Text.Json;
using Rocks.Build;
using Rocks.Build.Cli;
using Rocks.Tooling.Common;

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
    var exported = await Cli("sdk", "--root", root, "--target", "Engine", "--configuration", "Debug", "--destination", sdk);
    Require(exported.ExitCode == 0, "SDK export failed: " + exported.StdErr);
    Require(File.Exists(Path.Combine(sdk, "rocks-sdk.json")), "SDK manifest missing");
    Require(!Directory.GetFiles(sdk, "*.cpp", SearchOption.AllDirectories).Any(), "Installed SDK contains engine sources");
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
    File.AppendAllText(Path.Combine(game, "Source/Main.cpp"), "\n// 독립 게임 소스만 수정한다.\n");
    Require((await Game()).ExitCode == 0, "Game source edit build failed");
    var edited = ReadActionReport();
    Require(edited.ExecutedActions == 2 && edited.Actions.Where(action => action.Executed).All(action => action.Module == "SampleGame"),
        "Game edit must compile one game source and relink without engine actions");
    Require(originalFiles.All(pair => Hashing.Sha256File(pair.Key) == pair.Value), "Game build modified installed SDK");
    Pass("game-edit-engine-reuse", timer.Elapsed);

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
