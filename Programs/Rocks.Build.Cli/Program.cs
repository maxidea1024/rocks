using System.Text.Json;
using Rocks.Build;
using Rocks.Build.Cli;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

Console.OutputEncoding = new System.Text.UTF8Encoding(false);
Console.InputEncoding = new System.Text.UTF8Encoding(false);
using var cancellation = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    cancellation.Cancel();
};

try
{
    return await BuildCommand.RunAsync(args, cancellation.Token);
}
catch (OperationCanceledException)
{
    Console.Error.WriteLine("ROCKS-BUILD-CANCELLED: Build cancelled before successful completion.");
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
            {
                throw new ArgumentException("Expected build, test, sdk, or game command.");
            }

            var options = Parse(args.Skip(1).ToArray());
            var allowed = args[0] switch
            {
                "game" => new HashSet<string>(["sdk", "project", "target", "configuration", "output", "compiler", "jobs"], StringComparer.Ordinal),
                "sdk" => new HashSet<string>(["root", "target", "configuration", "output", "compiler", "rules", "jobs", "destination"], StringComparer.Ordinal),
                "build" or "test" => new HashSet<string>(["root", "target", "configuration", "output", "compiler", "rules", "jobs", "platform", "android-abi", "android-api", "android-ndk", "adb"], StringComparer.Ordinal),
                _ => throw new ArgumentException($"Unknown command: {args[0]}")
            };
            foreach (var name in options.Keys)
            {
                if (!allowed.Contains(name))
                {
                    throw new ArgumentException($"Option --{name} is not supported by '{args[0]}'.");
                }
            }

            foreach (var name in args[0] == "game" ? new[] { "sdk", "project" } : args[0] == "sdk" ? new[] { "destination" } : Array.Empty<string>())
            {
                if (!options.TryGetValue(name, out var value) || string.IsNullOrWhiteSpace(value))
                {
                    throw new ArgumentException($"Command '{args[0]}' requires --{name} <path>.");
                }
            }

            if (args[0] == "game")
            {
                return await InstalledSdk.BuildGameAsync(options, cancellationToken);
            }

            if (args[0] is not ("build" or "test" or "sdk"))
            {
                throw new ArgumentException($"Unknown command: {args[0]}");
            }

            var root = Path.GetFullPath(options.GetValueOrDefault("root", Directory.GetCurrentDirectory()));
            var configuration = Configuration(options);
            var targetName = options.GetValueOrDefault("target", args[0] == "sdk" ? "Engine" : "Foundation");
            // --platform android면 NDK로 교차 빌드한다(--android-abi arm64|x86_64, --android-api, --android-ndk).
            var platform = options.GetValueOrDefault("platform", "host");
            if (platform is not ("host" or "android"))
            {
                throw new ArgumentException("platform must be host or android.");
            }

            bool android = platform == "android";
            if (!android && options.Keys.Any(key => key.StartsWith("android-", StringComparison.Ordinal) || key == "adb"))
            {
                throw new ArgumentException("--android-* and --adb options require --platform android.");
            }

            if (android && options.ContainsKey("compiler"))
            {
                throw new ArgumentException("--compiler is not used for --platform android; pass --android-ndk instead.");
            }

            if (android && args[0] == "sdk")
            {
                throw new ArgumentException("Installed SDK export does not support --platform android yet.");
            }

            string androidAbi = options.GetValueOrDefault("android-abi", "arm64");
            int androidApi = int.TryParse(options.GetValueOrDefault("android-api", "26"), out var api) ? api
                : throw new ArgumentException("android-api must be an integer.");
            string platformName = android ? "android-" + androidAbi : OperatingSystem.IsWindows() ? "windows" : "macos";
            var output = Path.GetFullPath(options.GetValueOrDefault("output",
                Path.Combine(root, "out", $"dotnet-{platformName}-{targetName.ToLowerInvariant()}-{configuration.ToString().ToLowerInvariant()}")));
            var toolchain = android
                ? await NativeToolchains.DiscoverAndroidAsync(androidAbi, androidApi, options.GetValueOrDefault("android-ndk"), cancellationToken)
                : await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
            var context = new BuildContext(root, output, toolchain.Platform, configuration);
            var rulesPath = options.GetValueOrDefault("rules", HostTool(root, "Rocks.Engine.Rules"));
            var rules = RuleLoader.Load(rulesPath, targetName, context);
            Directory.CreateDirectory(output);
            await using var invocationLock = await OutputInvocationLock.AcquireAsync(output, cancellationToken);
            Directory.CreateDirectory(Path.Combine(output, "Generated"));
            var evidence = await BuildEvidence.StartAsync(root, Path.GetRelativePath(Path.Combine(root, "out"), output), toolchain.Environment);
            evidence.SetConfiguration(configuration.ToString(), targetName, configuration == NativeConfiguration.Asan ? "address" : "none", toolchain.CompilerPath, toolchain.Identity);
            try
            {
                // 모듈 소유 메타데이터의 배치(생성 include 디렉터리·명세 경로·검증)를 타깃에서 도달하는 모듈에 대해 정한다.
                // 아래 모든 단계가 이 결과를 쓰고, 검증 실패도 build-evidence에 기록된다.
                var plan = MetadataLayout.Apply(context, rules.Modules, rules.Target.Modules);
                var metaStart = System.Diagnostics.Stopwatch.GetTimestamp();
                var metadata = await MetadataRunner.GenerateAsync(context, toolchain, plan, rules.Target,
                    HostTool(root, "Rocks.MetaCompiler"), cancellationToken);
                evidence.RecordStep("metadata", ["Rocks.MetaCompiler", targetName], metadata,
                    System.Diagnostics.Stopwatch.GetElapsedTime(metaStart));
                if (metadata != 0)
                {
                    await evidence.FinishAsync("Metadata validation failed.");
                    return 1;
                }

                var modules = plan.Modules.ToList();
                var targets = rules.Target.Modules.ToList();
                if (rules.Target.CheckPublicHeaders)
                {
                    var sources = new List<string>();
                    var reached = new HashSet<string>(StringComparer.Ordinal);
                    void Reach(string name)
                    {
                        if (!reached.Add(name))
                        {
                            return;
                        }

                        var module = modules.SingleOrDefault(item => item.Name == name)
                            ?? throw new ArgumentException($"Unknown dependency {name}");
                        foreach (var dependency in module.Dependencies)
                        {
                            Reach(dependency);
                        }
                    }

                    foreach (var name in targets)
                    {
                        Reach(name);
                    }

                    foreach (var includeDirectory in modules.Where(module => reached.Contains(module.Name)).SelectMany(module => module.PublicIncludeDirectories).Distinct())
                    {
                        foreach (var header in Directory.GetFiles(includeDirectory, "*.h", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
                        {
                            var include = Path.GetRelativePath(includeDirectory, header).Replace('\\', '/');
                            var path = Path.Combine(output, "HeaderChecks", Hashing.Sha256Text(include) + ".cpp");
                            AtomicFile.WriteIfChanged(path, $"#include <{include}>\nint RocksHeaderCheck_{Hashing.Sha256Text(include)}() noexcept {{ return 0; }}\n");
                            sources.Add(path);
                        }
                    }

                    modules.Add(new()
                    {
                        Name = "PublicHeaderChecks",
                        Sources = sources,
                        Dependencies = reached.Where(name => modules.Single(module => module.Name == name).Kind == NativeModuleKind.StaticLibrary).Order(StringComparer.Ordinal).ToArray(),
                        PrivateIncludeDirectories = [root]
                    });
                    targets.Add("PublicHeaderChecks");
                }

                var nativeStart = System.Diagnostics.Stopwatch.GetTimestamp();
                var result = await new NativeBuildEngine().BuildAsync(new()
                {
                    Toolchain = toolchain,
                    Configuration = configuration,
                    RootDirectory = root,
                    OutputDirectory = output,
                    Modules = modules,
                    Targets = targets,
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
                    await InstalledSdk.ExportAsync(root, options["destination"], configuration, toolchain, InstalledSdk.Reachable(plan.Modules, targets),
                        plan.Generated, result, cancellationToken);
                }

                if (args[0] == "test")
                {
                    if (rules.Target.TestExecutable is null)
                    {
                        throw new ArgumentException("This target has no test executable.");
                    }

                    var report = Path.Combine(output, "foundation-report.json");
                    evidence.BeginTests(report);
                    var executable = result.ModuleOutputs[rules.Target.TestExecutable];
                    if (android)
                    {
                        // 기기·에뮬레이터에서 native 테스트를 실행하고 보고서를 회수한다. host에서 자식
                        // 프로세스를 띄우는 harness(Fatal 음성 대조 등)와 self·meta 검사는 Android에서 아직
                        // 실행하지 않는다.
                        var deviceStart = System.Diagnostics.Stopwatch.GetTimestamp();
                        int deviceExit = await AndroidDevice.RunTestsAsync(executable, report, options.GetValueOrDefault("adb"),
                            toolchain.Environment, cancellationToken);
                        evidence.RecordStep("foundation-android", [executable, "--seed", "7351", "--json", report], deviceExit,
                            System.Diagnostics.Stopwatch.GetElapsedTime(deviceStart), tests: true);
                        Console.WriteLine("Android: device tests and host object checks only; host harness, self, and meta checks were not run.");
                        if (deviceExit != 0)
                        {
                            await evidence.FinishAsync("Android device tests failed.");
                            return 1;
                        }

                        // 오브젝트 검사(결정적 수학의 외부 심볼·금지 명령)는 host에서 오브젝트만 읽으므로 Android에서도 돈다.
                        var objectsCommand = new[] { HostTool(root, "Rocks.ToolTests"), "objects", "--root", root,
                            "--compile-commands", result.CompilationDatabasePath, "--json", Path.Combine(output, "tool-objects-report.json") };
                        var objectsStart = System.Diagnostics.Stopwatch.GetTimestamp();
                        var objects = await ProcessRunner.RunAsync(new("dotnet", objectsCommand, root, toolchain.Environment,
                            Timeout: TimeSpan.FromMinutes(10)), cancellationToken);
                        Console.Write(objects.StdOut);
                        Console.Error.Write(objects.StdErr);
                        evidence.RecordStep("objects", objectsCommand, objects.ExitCode, System.Diagnostics.Stopwatch.GetElapsedTime(objectsStart),
                            tests: true);
                        if (objects.ExitCode != 0)
                        {
                            await evidence.FinishAsync("objects checks failed.");
                            return 1;
                        }

                        var androidCompleted = await evidence.FinishAsync();
                        return androidCompleted.Status == "PASS" ? 0 : 1;
                    }

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
                    {
                        checks[1] = [.. checks[1], "--asan"];
                    }

                    if (plan.Generated.Count != 0)
                    {
                        // 생성 헤더 검증 컴파일은 Core 헤더를 include하므로 이 구성의 매크로가 필요하다.
                        checks.Add(["meta", "--root", root, "--compiler", toolchain.CompilerPath, "--metacompiler", HostTool(root, "Rocks.MetaCompiler"),
                            "--clang-arg", "-D" + NativeConfigurations.BuildMacro(configuration), "--json", Path.Combine(output, "tool-meta-report.json")]);
                    }

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
                ? value : throw new ArgumentException("Configuration must be Debug, Development, Release, or Asan.");

        internal static int Jobs(Dictionary<string, string> options) =>
            int.TryParse(options.GetValueOrDefault("jobs", Math.Min(8, Environment.ProcessorCount).ToString()), out var value) &&
            value > 0 && value <= ProcessRunner.MaximumConcurrentProcesses
                ? value : throw new ArgumentException($"jobs must be between 1 and {ProcessRunner.MaximumConcurrentProcesses}.");

        internal static Dictionary<string, string> Parse(string[] args)
        {
            var allowed = new HashSet<string>(["root", "target", "configuration", "output", "compiler", "rules", "jobs", "destination", "sdk", "project", "platform", "android-abi", "android-api", "android-ndk", "adb"], StringComparer.Ordinal);
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            for (var index = 0; index < args.Length; index += 2)
            {
                if (!args[index].StartsWith("--", StringComparison.Ordinal) || index + 1 == args.Length ||
                    !allowed.Contains(args[index][2..]) || !values.TryAdd(args[index][2..], args[index + 1]))
                {
                    throw new ArgumentException($"Unknown, duplicate, or incomplete option: {args[index]}");
                }
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
            if (result.Failure is not null)
            {
                Console.Error.WriteLine(result.Failure);
            }
        }
    }
}
