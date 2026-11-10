using System.Text.Json;
using Rocks.Build;
using Rocks.Build.Cli;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

Console.OutputEncoding = new System.Text.UTF8Encoding(false);
Console.InputEncoding = new System.Text.UTF8Encoding(false);
using var cancellation = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancellation.Cancel(); };
try
{
    return await BuildCommand.RunAsync(args, cancellation.Token);
}
catch (OperationCanceledException)
{
    Console.Error.WriteLine("ROCKS-BUILD-CANCELLED: Build cancelled; no successful state was published.");
    return 130;
}
catch (Exception error)
{
    Console.Error.WriteLine($"ROCKS-BUILD-ERROR: {error.Message}");
    return 2;
}

namespace Rocks.Build.Cli
{
    public static class BuildCommand
    {
        public static async Task<int> RunAsync(string[] args, CancellationToken cancellationToken)
        {
            if (args.Length == 0)
                throw new ArgumentException("Expected build, test, sdk, or game command.");
            var options = Parse(args.Skip(1).ToArray());
            var allowed = args[0] switch
            {
                "game" => new HashSet<string>(["sdk", "project", "target", "configuration", "output", "compiler", "jobs"], StringComparer.Ordinal),
                "sdk" => new HashSet<string>(["root", "target", "configuration", "output", "compiler", "rules", "jobs", "destination"], StringComparer.Ordinal),
                "build" or "test" => new HashSet<string>(["root", "target", "configuration", "output", "compiler", "rules", "jobs"], StringComparer.Ordinal),
                _ => throw new ArgumentException($"Unknown command: {args[0]}")
            };
            foreach (var name in options.Keys)
                if (!allowed.Contains(name)) throw new ArgumentException($"Option --{name} is not supported by '{args[0]}'.");
            foreach (var name in args[0] == "game" ? new[] { "sdk", "project" } : args[0] == "sdk" ? new[] { "destination" } : Array.Empty<string>())
                if (!options.TryGetValue(name, out var value) || string.IsNullOrWhiteSpace(value))
                    throw new ArgumentException($"Command '{args[0]}' requires --{name} <path>.");
            if (args[0] == "game")
                return await InstalledSdk.BuildGameAsync(options, cancellationToken);
            if (args[0] is not ("build" or "test" or "sdk"))
                throw new ArgumentException($"Unknown command: {args[0]}");
            var root = Path.GetFullPath(options.GetValueOrDefault("root", Directory.GetCurrentDirectory()));
            var configuration = Configuration(options);
            var targetName = options.GetValueOrDefault("target", args[0] == "sdk" ? "Engine" : "Foundation");
            var output = Path.GetFullPath(options.GetValueOrDefault("output",
                Path.Combine(root, "out", $"dotnet-{(OperatingSystem.IsWindows() ? "windows" : "macos")}-{targetName.ToLowerInvariant()}-{configuration.ToString().ToLowerInvariant()}")));
            var toolchain = await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
            var context = new BuildContext(root, output, toolchain.Platform, configuration);
            var rulesPath = options.GetValueOrDefault("rules", HostTool(root, "Rocks.Engine.Rules"));
            var rules = RuleLoader.Load(rulesPath, targetName, context);
            Directory.CreateDirectory(output);
            Directory.CreateDirectory(Path.Combine(output, "Generated"));

            var evidence = await BuildEvidence.StartAsync(root, Path.GetRelativePath(Path.Combine(root, "out"), output), toolchain.Environment);
            evidence.SetConfiguration(configuration.ToString(), targetName, configuration == NativeConfiguration.Asan ? "address" : "none", toolchain.CompilerPath, toolchain.Identity);
            try
            {
                var metaStart = System.Diagnostics.Stopwatch.GetTimestamp();
                var metadata = await MetadataRunner.GenerateAsync(context, toolchain, rules.Modules, rules.Target,
                    HostTool(root, "Rocks.MetaCompiler"), cancellationToken);
                evidence.RecordStep("metadata", ["Rocks.MetaCompiler", targetName], metadata,
                    System.Diagnostics.Stopwatch.GetElapsedTime(metaStart));
                if (metadata != 0)
                {
                    await evidence.FinishAsync("Metadata validation failed.");
                    return 1;
                }

                var modules = rules.Modules.ToList();
                var targets = rules.Target.Modules.ToList();
                if (rules.Target.CheckPublicHeaders)
                {
                    var sources = new List<string>();
                    var reached = new HashSet<string>(StringComparer.Ordinal);
                    void Reach(string name)
                    {
                        if (!reached.Add(name)) return;
                        var module = modules.SingleOrDefault(item => item.Name == name)
                            ?? throw new ArgumentException($"Unknown dependency {name}");
                        foreach (var dependency in module.Dependencies) Reach(dependency);
                    }
                    foreach (var name in targets) Reach(name);
                    foreach (var includeDirectory in modules.Where(module => reached.Contains(module.Name)).SelectMany(module => module.PublicIncludeDirectories).Distinct())
                        foreach (var header in Directory.GetFiles(includeDirectory, "*.h", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
                        {
                            var include = Path.GetRelativePath(includeDirectory, header).Replace('\\', '/');
                            var path = Path.Combine(output, "HeaderChecks", Hashing.Sha256Text(include) + ".cpp");
                            AtomicFile.WriteIfChanged(path, $"#include <{include}>\nint RocksHeaderCheck_{Hashing.Sha256Text(include)}() noexcept {{ return 0; }}\n");
                            sources.Add(path);
                        }
                    foreach (var unit in rules.Target.Metadata)
                    {
                        var path = Path.Combine(output, "HeaderChecks/" + Hashing.Sha256Text(unit.Output) + ".cpp");
                        AtomicFile.WriteIfChanged(path, $"#include <{unit.Output}>\nint RocksGeneratedHeaderCheck_{Hashing.Sha256Text(unit.Output)}() noexcept {{ return 0; }}\n");
                        sources.Add(path);
                    }
                    modules.Add(new()
                    {
                        Name = "PublicHeaderChecks", Sources = sources,
                        Dependencies = reached.Where(name => modules.Single(module => module.Name == name).Kind == NativeModuleKind.StaticLibrary).Order(StringComparer.Ordinal).ToArray(),
                        PrivateIncludeDirectories = [root, Path.Combine(output, "Generated")]
                    });
                    targets.Add("PublicHeaderChecks");
                }
                var nativeStart = System.Diagnostics.Stopwatch.GetTimestamp();
                var result = await new NativeBuildEngine().BuildAsync(new()
                {
                    Toolchain = toolchain, Configuration = configuration, RootDirectory = root,
                    OutputDirectory = output, Modules = modules, Targets = targets,
                    MaxParallelism = Jobs(options),
                    ContextFingerprint = "rocks-build-rules-v1"
                }, cancellationToken);
                Print(result);
                AtomicFile.WriteIfChanged(Path.Combine(output, "action-report.json"), JsonSerializer.Serialize(result, JsonOptions));
                evidence.RecordStep("native", ["Rocks.Build", targetName, configuration.ToString()], result.Succeeded ? 0 : 1, System.Diagnostics.Stopwatch.GetElapsedTime(nativeStart));
                if (!result.Succeeded)
                {
                    await evidence.FinishAsync(result.Failure);
                    return 1;
                }
                if (args[0] == "sdk")
                {
                    await InstalledSdk.ExportAsync(root, options["destination"], configuration, toolchain, rules.Modules, result, cancellationToken);
                }
                if (args[0] == "test")
                {
                    if (rules.Target.TestExecutable is null)
                        throw new ArgumentException("This target has no test executable.");
                    var report = Path.Combine(output, "foundation-report.json");
                    evidence.BeginTests(report);
                    var executable = result.ModuleOutputs[rules.Target.TestExecutable];
                    var testStart = System.Diagnostics.Stopwatch.GetTimestamp();
                    var native = await ProcessRunner.RunAsync(new(executable, ["--seed", "7351", "--json", report], root, toolchain.Environment), cancellationToken);
                    Console.Write(native.StdOut);
                    Console.Error.Write(native.StdErr);
                    evidence.RecordStep("foundation", [executable, "--seed", "7351", "--json", report], native.ExitCode, System.Diagnostics.Stopwatch.GetElapsedTime(testStart), tests: true);
                    if (native.ExitCode != 0)
                    {
                        await evidence.FinishAsync("Foundation tests failed.");
                        return 1;
                    }
                    var checks = new List<string[]> {
                        new[] { "self", "--root", root, "--json", Path.Combine(output, "tool-self-report.json") },
                        new[] { "native", "--root", root, "--native-executable", executable, "--compiler", toolchain.CompilerPath, "--compile-commands", result.CompilationDatabasePath, "--json", Path.Combine(output, "tool-native-report.json") }
                    };
                    if (configuration == NativeConfiguration.Asan)
                        checks[1] = [.. checks[1], "--asan"];
                    if (rules.Target.Metadata.Count != 0)
                        checks.Add(["meta", "--root", root, "--compiler", toolchain.CompilerPath, "--metacompiler", HostTool(root, "Rocks.MetaCompiler"), "--json", Path.Combine(output, "tool-meta-report.json")]);
                    foreach (var check in checks)
                    {
                        var command = new[] { HostTool(root, "Rocks.ToolTests") }.Concat(check).ToArray();
                        var checkStart = System.Diagnostics.Stopwatch.GetTimestamp();
                        var tested = await ProcessRunner.RunAsync(new("dotnet", command, root, toolchain.Environment, Timeout: TimeSpan.FromMinutes(10)), cancellationToken);
                        Console.Write(tested.StdOut);
                        Console.Error.Write(tested.StdErr);
                        evidence.RecordStep(check[0], command, tested.ExitCode, System.Diagnostics.Stopwatch.GetElapsedTime(checkStart), tests: true);
                        if (tested.ExitCode != 0)
                        {
                            await evidence.FinishAsync($"{check[0]} checks failed.");
                            return 1;
                        }
                    }
                }
                var completed = await evidence.FinishAsync();
                return completed.Status == "PASS" ? 0 : 1;
            }
            catch (Exception error)
            {
                await evidence.FinishAsync(error.Message);
                throw;
            }
        }

        internal static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
        internal static string HostTool(string root, string name) => Path.Combine(root, "out/dotnet/bin", name, "Debug/net10.0", name + ".dll");
        internal static NativeConfiguration Configuration(Dictionary<string, string> options) =>
            Enum.TryParse<NativeConfiguration>(options.GetValueOrDefault("configuration", "Debug"), true, out var value) && Enum.IsDefined(value)
                ? value : throw new ArgumentException("Configuration must be Debug, Release, or Asan.");
        internal static int Jobs(Dictionary<string, string> options) =>
            int.TryParse(options.GetValueOrDefault("jobs", Math.Min(8, Environment.ProcessorCount).ToString()), out var value) &&
            value > 0 && value <= ProcessRunner.MaximumConcurrentProcesses
                ? value : throw new ArgumentException($"jobs must be between 1 and {ProcessRunner.MaximumConcurrentProcesses}.");

        internal static Dictionary<string, string> Parse(string[] args)
        {
            var allowed = new HashSet<string>(["root", "target", "configuration", "output", "compiler", "rules", "jobs", "destination", "sdk", "project"], StringComparer.Ordinal);
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            for (var index = 0; index < args.Length; index += 2)
            {
                if (!args[index].StartsWith("--", StringComparison.Ordinal) || index + 1 == args.Length ||
                    !allowed.Contains(args[index][2..]) || !values.TryAdd(args[index][2..], args[index + 1]))
                    throw new ArgumentException($"Unknown, duplicate, or incomplete option: {args[index]}");
            }
            return values;
        }

        internal static void Print(NativeBuildResult result)
        {
            foreach (var action in result.Actions.Where(action => action.Executed))
            {
                Console.WriteLine($"[{action.Kind}] {action.Module}: {action.Id}");
                Console.Write(action.StdOut);
                Console.Error.Write(action.StdErr);
            }
            Console.WriteLine($"Native build: {(result.Succeeded ? "PASS" : "FAIL")}; executed={result.ExecutedActions}; cached={result.SkippedActions}");
            if (result.Failure is not null) Console.Error.WriteLine(result.Failure);
        }
    }
}
