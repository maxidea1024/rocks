using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

if (args.Contains("--fake-child")) { await Task.Delay(Timeout.InfiniteTimeSpan); return 0; }
if (args.Contains("--fake-compiler"))
{
    string marker = args[Array.IndexOf(args, "--fake-compiler") + 1];
    var start = new ProcessStartInfo("dotnet") { UseShellExecute = false, CreateNoWindow = true };
    start.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
    start.ArgumentList.Add("--fake-child");
    using var child = Process.Start(start)!;
    File.WriteAllText(marker, child.Id.ToString(System.Globalization.CultureInfo.InvariantCulture));
    await Task.Delay(Timeout.InfiniteTimeSpan);
    return 0;
}

string root = Path.Combine(Path.GetTempPath(), "Rocks Native Tests 한글 " + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
int passed = 0;
bool allPassed = false;
try
{
    var toolchain = await NativeToolchains.DiscoverAsync();
    string sdk = Path.Combine(root, "sdk.identity");
    File.WriteAllText(sdk, "fixture SDK 1");
    // 실제 compiler/linker는 사용한다. SDK 변형은 작은 독립 입력으로 주입해 key 계약을 검사한다.
    toolchain = toolchain with { FingerprintPaths = [sdk] };
    string include = Directory.CreateDirectory(Path.Combine(root, "engine", "include")).FullName;
    string engine = Directory.CreateDirectory(Path.Combine(root, "engine", "src")).FullName;
    string game = Directory.CreateDirectory(Path.Combine(root, "game")).FullName;
    string publicHeader = Path.Combine(include, "api.h");
    string privateHeader = Path.Combine(engine, "private.h");
    string librarySource = Path.Combine(engine, "library.cpp");
    string gameSource = Path.Combine(game, "main.cpp");
    File.WriteAllText(publicHeader, "#pragma once\nint Answer() noexcept;\n");
    File.WriteAllText(privateHeader, "#pragma once\n#define ANSWER 7\n");
    const string libraryText = "#include \"api.h\"\n#include \"private.h\"\nint Answer() noexcept { return ANSWER; }\n";
    File.WriteAllText(librarySource, libraryText);
    File.WriteAllText(gameSource, "#include \"api.h\"\nint main() { return Answer() == 7 ? 0 : 9; }\n");
    var modules = new NativeModuleInput[]
    {
        new() { Name = "Headers", PublicIncludeDirectories = [include] },
        new() { Name = "Engine", Sources = [librarySource], Dependencies = ["Headers"] },
        new() { Name = "Game", Kind = NativeModuleKind.Executable, Sources = [gameSource], Dependencies = ["Engine"] }
    };
    var request = new NativeBuildRequest { RootDirectory = root, OutputDirectory = Path.Combine(root, "out"),
        Toolchain = toolchain, Modules = modules, Targets = ["Game"], MaxParallelism = 4 };
    var builder = new NativeBuildEngine();
    async Task<NativeBuildResult> Build(NativeBuildRequest? input = null)
    {
        var value = await builder.BuildAsync(input ?? request);
        if (!value.Succeeded) throw new InvalidOperationException(value.Failure + "\n" + string.Join("\n", value.Actions.Select(action => action.StdOut + action.StdErr)));
        return value;
    }
    void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
    void Pass(string name) { ++passed; Console.WriteLine("PASS " + name); }

    await PlatformSelectionTests.RunAsync(request, root);
    Pass("platform routing rejects unknown and mismatched targets");

    DescriptorTests.Run(root, args.Contains("--root") ? args[Array.IndexOf(args, "--root") + 1] : null);
    Pass("project and plugin descriptors parse strictly and resolve plugins");

    await ExecutorCleanupTests.RunAsync(request, root);
    Pass("cleanup failures preserve diagnostics, cancellation, and cache safety");

    foreach (int invalidJobs in new[] { 0, ProcessRunner.MaximumConcurrentProcesses + 1 })
    {
        string invalidOutput = Path.Combine(root, "invalid-jobs-" + invalidJobs);
        try
        {
            await builder.BuildAsync(request with { MaxParallelism = invalidJobs, OutputDirectory = invalidOutput });
            throw new InvalidOperationException("Invalid parallelism accepted.");
        }
        catch (ArgumentOutOfRangeException error)
        {
            Check(error.ParamName == "MaxParallelism" && !Directory.Exists(invalidOutput),
                "Invalid parallelism must fail before creating build output.");
        }
    }
    Pass("invalid process parallelism rejected before output creation");

    var cold = await Build();
    Check(cold.ExecutedActions == 4 && !cold.ModuleOutputs.ContainsKey("Headers"), "Cold build/header-only count");
    var run = await ProcessRunner.RunAsync(new(cold.ModuleOutputs["Game"], [], root, toolchain.Environment));
    Check(run.ExitCode == 0, "Built executable behavior");
    Pass("actual compiler/archive/link and header-only transitive includes");

    var warm = await Build();
    Check(warm.ExecutedActions == 0 && warm.SkippedActions == 4, "No-op must execute zero actions");
    Pass("no-op output validation");

    using var compileCommands = JsonDocument.Parse(File.ReadAllText(cold.CompilationDatabasePath));
    string gameObject = compileCommands.RootElement.EnumerateArray()
        .First(value => value.GetProperty("file").GetString() == gameSource).GetProperty("output").GetString()!;
    string objectBeforeComment = Hashing.Sha256File(gameObject);
    File.AppendAllText(gameSource, "// game-only comment\n");
    var commentChanged = await Build();
    bool objectChanged = Hashing.Sha256File(gameObject) != objectBeforeComment;
    var commentCompile = commentChanged.Actions.Single(action => action.Module == "Game" && action.Kind == "Compile");
    var commentLink = commentChanged.Actions.Single(action => action.Module == "Game" && action.Kind == "Link");
    Check(commentCompile.Executed && commentLink.Executed == objectChanged &&
        commentChanged.Actions.All(action => action.Module != "Engine" || !action.Executed),
        $"Comment edit must compile Game, skip Engine, and link iff object bytes changed; objectChanged={objectChanged}, " +
        string.Join(", ", commentChanged.Actions.Select(action => $"{action.Id}:executed={action.Executed}")));
    Pass($"comment edit objectChanged={objectChanged}, linkExecuted={commentLink.Executed}");

    string objectBeforeCode = Hashing.Sha256File(gameObject);
    File.AppendAllText(gameSource, "int RocksBuildGameMarker() noexcept { return 123; }\n");
    var gameChanged = await Build();
    Check(Hashing.Sha256File(gameObject) != objectBeforeCode, "Semantic game edit did not change object contents");
    Check(gameChanged.Actions.All(action => action.Module != "Engine" || !action.Executed), "Game change rebuilt Engine");
    Check(gameChanged.ExecutedActions == 2 &&
        gameChanged.Actions.Any(action => action.Module == "Game" && action.Kind == "Compile" && action.Executed) &&
        gameChanged.Actions.Any(action => action.Module == "Game" && action.Kind == "Link" && action.Executed),
        "Semantic game change should compile/link only Game; " +
        string.Join(", ", gameChanged.Actions.Select(action => $"{action.Id}:executed={action.Executed}")));
    Pass("game changes execute zero engine actions");

    File.AppendAllText(privateHeader, "// private header changed\n");
    var headerChanged = await Build();
    Check(headerChanged.Actions.Any(action => action.Module == "Engine" && action.Kind == "Compile" && action.Executed), "Private header change missed");
    Check(headerChanged.Actions.All(action => action.Module != "Game" || action.Kind != "Compile" || !action.Executed), "Private header leaked to Game compilation");
    Pass("private header invalidation with private boundary");

    string optional = Path.Combine(include, "new_optional.h");
    File.WriteAllText(optional, "#pragma once\n");
    var addedHeader = await Build();
    Check(addedHeader.Actions.Count(action => action.Kind == "Compile" && action.Executed) == 2, "Added optional header did not invalidate consumers");
    File.Delete(optional);
    var deletedHeader = await Build();
    Check(deletedHeader.Actions.Count(action => action.Kind == "Compile" && action.Executed) == 2, "Deleted optional header did not invalidate consumers");
    Pass("header directory membership invalidation");

    var database = JsonDocument.Parse(File.ReadAllText(cold.CompilationDatabasePath));
    string objectFile = database.RootElement.EnumerateArray().First(value => value.GetProperty("file").GetString() == librarySource).GetProperty("output").GetString()!;
    File.AppendAllText(objectFile, "corrupt");
    var modifiedOutput = await Build();
    Check(modifiedOutput.Actions.Any(action => action.Module == "Engine" && action.Kind == "Compile" && action.Executed), "Modified object accepted as hit");
    File.Delete(cold.ModuleOutputs["Engine"]);
    var missingOutput = await Build();
    Check(missingOutput.Actions.Any(action => action.Module == "Engine" && action.Kind == "Archive" && action.Executed), "Missing archive accepted as hit");
    Pass("modified and missing output invalidation");

    string oldExe = Hashing.Sha256File(cold.ModuleOutputs["Game"]);
    string oldLibrary = Hashing.Sha256File(cold.ModuleOutputs["Engine"]);
    File.WriteAllText(librarySource, "#error deliberate compile failure\n");
    var failed = await builder.BuildAsync(request);
    Check(!failed.Succeeded && failed.Actions.Any(action => action.Executed && action.ExitCode != 0), "Compiler failure reported success");
    Check(Hashing.Sha256File(cold.ModuleOutputs["Game"]) == oldExe && Hashing.Sha256File(cold.ModuleOutputs["Engine"]) == oldLibrary, "Failure replaced prior output");
    Check(!Directory.EnumerateFileSystemEntries(Path.Combine(request.OutputDirectory, ".native-tmp")).Any(), "Temporary output retained");
    File.WriteAllText(librarySource, libraryText);
    await Build();
    Pass("compiler failure preserves published outputs and retry");

    File.WriteAllText(sdk, "fixture SDK 2");
    var changedSdk = await Build();
    Check(changedSdk.ExecutedActions == 4, "SDK content change missed");
    var changedFlags = await Build(request with { ContextFingerprint = "target/ABI revision 2" });
    Check(changedFlags.ExecutedActions == 4, "Context fingerprint change missed");
    Pass("SDK content and target context invalidate actions");

    var parallelRequest = request with { OutputDirectory = Path.Combine(root, "parallel") };
    var both = await Task.WhenAll(builder.BuildAsync(parallelRequest), builder.BuildAsync(parallelRequest));
    Check(both.All(value => value.Succeeded) && both.Sum(value => value.ExecutedActions) == 4, "Concurrent output writers duplicated work or failed");
    Pass("same-output concurrent lock and atomic cache");

    try
    {
        await builder.BuildAsync(request with { Modules = [new() { Name = "A", Dependencies = ["B"] }, new() { Name = "B", Dependencies = ["A"] }], Targets = ["A"] });
        throw new InvalidOperationException("Cycle accepted");
    }
    catch (ArgumentException) { }
    Pass("module cycle rejection");

    foreach (var badRequest in new[]
    {
        request with { OutputDirectory = root },
        request with { Modules = [modules[0], modules[1] with { AdditionalCompileArguments = ["/WX-"] }, modules[2]] },
        request with { Modules = [modules[0], modules[1] with { Sources = [librarySource, librarySource] }, modules[2]] },
        request with { Modules = [new() { Name = "CON" }], Targets = ["CON"] }
    })
    {
        try { await builder.BuildAsync(badRequest); throw new InvalidOperationException("Invalid input accepted"); }
        catch (ArgumentException) { }
    }
    Pass("invalid output, warning downgrade, duplicate source and reserved module rejected");

    string generated = Directory.CreateDirectory(Path.Combine(root, "out", "Generated")).FullName;
    string generatedHeader = Path.Combine(generated, "generated.h");
    File.WriteAllText(generatedHeader, "#pragma once\n");
    File.AppendAllText(gameSource, "#include \"generated.h\"\n");
    var rootIncludeRequest = request with { Modules = [modules[0], modules[1], modules[2] with { PrivateIncludeDirectories = [root, generated] }] };
    await Build(rootIncludeRequest);
    File.AppendAllText(generatedHeader, "// explicit generated input changed\n");
    var generatedChanged = await Build(rootIncludeRequest);
    Check(generatedChanged.Actions.Any(action => action.Module == "Game" && action.Kind == "Compile" && action.Executed), "Explicit Generated input excluded");
    Check((await Build(rootIncludeRequest)).ExecutedActions == 0, "Repo-root include rescanned transient outputs");
    File.WriteAllText(gameSource, "#include \"api.h\"\nint main() { return Answer() == 7 ? 0 : 9; }\n");
    Pass("repository root include excludes transient outputs but retains declared Generated input");

    var release = await Build(request with { Configuration = NativeConfiguration.Release, OutputDirectory = Path.Combine(root, "release") });
    Check((await ProcessRunner.RunAsync(new(release.ModuleOutputs["Game"], [], root, toolchain.Environment))).ExitCode == 0, "Release executable failed");
    var asan = await Build(request with { Configuration = NativeConfiguration.Asan, OutputDirectory = Path.Combine(root, "asan") });
    var asanRun = await ProcessRunner.RunAsync(new(asan.ModuleOutputs["Game"], [], root, toolchain.Environment));
    Check(asanRun.ExitCode == 0, "ASan executable failed: " + asanRun.StdErr);
    Pass("Release and ASan compiler/link/runtime deployment");

    await BuildConfigurationTests.RunAsync(request, root);
    Pass("build configurations, ThinLTO, and per-module optimization overrides");

    string marker = Path.Combine(root, "child.pid");
    string dotnet = NativeEnvironment.FindExecutable("dotnet", toolchain.Environment);
    var fakeToolchain = toolchain with { CompilerPath = dotnet, CompileArguments = [Assembly.GetExecutingAssembly().Location, "--fake-compiler", marker] };
    var cancellationRequest = request with { Toolchain = fakeToolchain, OutputDirectory = Path.Combine(root, "cancel"), Targets = ["Engine"], MaxParallelism = 1 };
    using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(20));
    Task<NativeBuildResult> cancelling = builder.BuildAsync(cancellationRequest, cancellation.Token);
    while (!File.Exists(marker)) { cancellation.Token.ThrowIfCancellationRequested(); await Task.Delay(10, cancellation.Token); }
    int childId = int.Parse(File.ReadAllText(marker), System.Globalization.CultureInfo.InvariantCulture);
    cancellation.Cancel();
    try { await cancelling; throw new InvalidOperationException("Cancellation ignored"); }
    catch (OperationCanceledException) { }
    bool childAlive;
    try { using var child = Process.GetProcessById(childId); childAlive = !child.HasExited; }
    catch (ArgumentException) { childAlive = false; }
    Check(!childAlive, "Cancelled compiler child survived");
    Check(!Directory.EnumerateFileSystemEntries(Path.Combine(cancellationRequest.OutputDirectory, ".native-tmp")).Any(), "Cancelled temporary output retained");
    var afterCancel = await Build(request with { OutputDirectory = cancellationRequest.OutputDirectory });
    Check(afterCancel.ExecutedActions == 4, "Cancelled build published cache state");
    Pass("cancellation kills process tree and releases output ownership");

    Console.WriteLine($"Native build regression groups: {passed}, failed: 0");
    allPassed = true;
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    Console.Error.WriteLine("Fixture retained: " + root);
    return 1;
}
finally
{
    // 작업 시작 시 직접 만든 고유 임시 경로만 정리한다. 실패 시 원인 자료를 보존한다.
    if (allPassed && Directory.Exists(root)) Directory.Delete(root, true);
}
