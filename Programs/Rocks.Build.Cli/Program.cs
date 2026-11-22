using System.Text.Json;
using Rocks.Build;
using Rocks.Build.Cli;
using Rocks.Build.Descriptors;
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
    // Visual Studio NMake 빌드(Build/IdeBuild.ps1)에서는 오류 목록에 잡히는 정규 형식 한 줄을 더 낸다(Docs/Design/ProjectFiles.ko.md 7.1절).
    if (Environment.GetEnvironmentVariable("ROCKS_IDE_BUILD") == "1")
    {
        Console.WriteLine($"error RB0001: {error.Message}");
    }

    return 2;
}

namespace Rocks.Build.Cli
{
    public static class BuildCommand
    {
        public static Task<int> RunAsync(string[] args, CancellationToken cancellationToken) => RunAsync(args, cancellationToken, null);

        // profile이 있으면 묶음 크기 실측의 실행 하나를 도는 build다(UnityProfileCommand가 부른다 — Docs/Design/UnityBuildPch.ko.md 12절).
        // 조합이 있으면 그 모듈에만 조합을 재정의로 얹고, 빌드 대상을 그 모듈로 줄이고, 그 모듈의 object를 지워 처음부터 컴파일한 뒤 결과
        // 행을 채운다. 조합이 없으면(준비 실행) 그 모듈이 의존하는 모듈만 빌드한다. 출력은 이 명령의 것이어야 한다(평소 빌드 출력 거부).
        // sdkAssembly가 있으면 다중 구성 SDK 내보내기(SdkExportCommand)가 부른 구성 하나의 sdk 빌드다. 빌드가 끝나면 그 구성을 조립에 더한다.
        internal static async Task<int> RunAsync(string[] args, CancellationToken cancellationToken, UnityProfileRun? profile,
            SdkAssembly? sdkAssembly = null)
        {
            if (args.Length == 0)
            {
                throw new ArgumentException("Expected build, test, sdk, game, bootstrap, clean, sdk-register, sdk-unregister, associate, new-project, " +
                    "shell-register, shell-unregister, unity-profile, or projectfiles command.");
            }

            if (args[0] == UnityProfileCommand.Name)
            {
                return await UnityProfileCommand.RunAsync(args.Skip(1).ToArray(), cancellationToken);
            }

            if (args[0] == ProjectFilesCommand.Name)
            {
                return await ProjectFilesCommand.RunAsync(args.Skip(1).ToArray(), cancellationToken);
            }

            if (profile is not null && args[0] != "build")
            {
                throw new ArgumentException("A unity profile run is a build.");
            }

            var options = Parse(args.Skip(1).ToArray());
            var allowed = args[0] switch
            {
                "game" => new HashSet<string>(["sdk", "project", "target", "configuration", "output", "compiler", "jobs", SingleFileCommand.Option, MetadataLayout.VerifySourcesOption, RequireVerifiedToolchainOption, .. BuildModeOptions], StringComparer.Ordinal),
                "sdk" => new HashSet<string>(["root", "target", "configuration", "configurations", "output", "compiler", "rules", "jobs", "destination", "register", MetadataLayout.VerifySourcesOption, RequireVerifiedToolchainOption, .. BuildModeOptions], StringComparer.Ordinal),
                "build" or "test" => new HashSet<string>(["root", "target", "configuration", "output", "compiler", "rules", "jobs", "platform", SingleFileCommand.Option, "android-abi", "android-api", "android-ndk", "adb", MetadataLayout.VerifySourcesOption, PlanOnlyOption, RequireVerifiedToolchainOption, .. BuildModeOptions], StringComparer.Ordinal),
                // 미리 빌드한 바이너리를 사용자 저장소에 받는다(build·test는 받지 않는다, Docs/Design/ShaderSystem.ko.md 8절 2항).
                "bootstrap" => new HashSet<string>(["root", "library", "store", "mirror", "offline", "windows-sdk"], StringComparer.Ordinal),
                // 출력 디렉터리 하나를 비운다(Docs/Design/ProjectFiles.ko.md 7.3절 — Visual Studio Clean·Rebuild가 부른다).
                CleanCommand.Name => new HashSet<string>(["root", "project", "target", "configuration", "platform", "android-abi", "output"], StringComparer.Ordinal),
                // 설치 SDK 등록부와 게임 프로젝트 연결(Docs/Design/ProjectFiles.ko.md 10.1·10.2절).
                SdkCommands.Register => new HashSet<string>(["sdk"], StringComparer.Ordinal),
                SdkCommands.Unregister => new HashSet<string>(["sdk", "id"], StringComparer.Ordinal),
                SdkCommands.Associate => new HashSet<string>(["project", "sdk", "id", "version"], StringComparer.Ordinal),
                // .rocksproject 탐색기 메뉴(Docs/Design/ProjectFiles.ko.md 12절).
                "shell-register" or "shell-unregister" => new HashSet<string>(StringComparer.Ordinal),
                // 템플릿에서 새 게임 프로젝트를 만든다(Docs/Design/ProjectFiles.ko.md 11.5절).
                NewProjectCommand.Name => new HashSet<string>(["template", "name", "destination", "sdk", "projectfiles", "compiler"], StringComparer.Ordinal),
                _ => throw new ArgumentException($"Unknown command: {args[0]}")
            };
            foreach (var name in options.Keys)
            {
                if (!allowed.Contains(name))
                {
                    throw new ArgumentException($"Option --{name} is not supported by '{args[0]}'.");
                }
            }

            // game의 --sdk는 선택이다(없으면 .rocksproject의 SDK 연결로 고른다, Docs/Design/ProjectFiles.ko.md 10.1절).
            foreach (var name in args[0] == "game" ? new[] { "project" } : args[0] == "sdk" ? new[] { "destination" } : Array.Empty<string>())
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

            if (args[0] is SdkCommands.Register or SdkCommands.Unregister or SdkCommands.Associate)
            {
                int registered = SdkCommands.Run(args[0], options, SdkRegistries.Default());
                // 연결을 바꾸면 이미 만든 게임 프로젝트 파일을 다시 만든다(Docs/Design/ProjectFiles.ko.md 10.4절, Windows 호스트).
                return registered != 0 || args[0] != SdkCommands.Associate || !OperatingSystem.IsWindows()
                    ? registered
                    : await GameProjectFiles.RegenerateAsync(options["project"], cancellationToken);
            }

            if (args[0] is "shell-register" or "shell-unregister")
            {
                return OperatingSystem.IsWindows() ? ShellRegistration.Run(args[0], SdkRegistries.Default())
                    : throw new ArgumentException($"'{args[0]}' registers the Windows Explorer menu and runs on Windows hosts only.");
            }

            if (args[0] == NewProjectCommand.Name)
            {
                return await NewProjectCommand.RunAsync(options, cancellationToken);
            }

            if (args[0] == CleanCommand.Name)
            {
                return await CleanCommand.RunAsync(options, cancellationToken);
            }

            if (args[0] == "bootstrap")
            {
                return await ThirdPartyBootstrapCommand.RunAsync(options, cancellationToken);
            }

            if (args[0] is not ("build" or "test" or "sdk"))
            {
                throw new ArgumentException($"Unknown command: {args[0]}");
            }

            // 설치 SDK는 구성마다 이 빌드 흐름을 돌려 한 SDK로 조립한다(Docs/Design/ProjectFiles.ko.md 10.3절).
            if (args[0] == "sdk" && sdkAssembly is null)
            {
                return await SdkExportCommand.RunAsync(args, options, cancellationToken);
            }

            // 받은 경로는 디스크에 있는 철자로 맞춘다(Docs/Design/UnityBuildPch.ko.md 4.4절). 규칙 평가와 메타 생성이 빌드 엔진보다 먼저
            // 이 경로로 파일을 쓰므로 여기서 한다.
            var root = PathSpelling.OnDisk(Path.GetFullPath(options.GetValueOrDefault("root", Directory.GetCurrentDirectory())));
            var configuration = Configuration(options);
            // 값이 틀린 빌드 방식 옵션은 출력을 만들기 전에 거부한다.
            foreach (string mode in BuildModeOptions.Where(mode => mode != TimingOption))
            {
                Switch(options, mode);
            }

            Timing(options);
            // 파일 하나 컴파일(Docs/Design/ProjectFiles.ko.md 7.4절)은 build 전용이고 계획만·묶음 실측과 함께 쓰지 않는다. 출력을 만지기 전에 알린다.
            bool singleFile = options.ContainsKey(SingleFileCommand.Option);
            if (singleFile && (args[0] != "build" || profile is not null || options.GetValueOrDefault(PlanOnlyOption, "off") == "on"))
            {
                throw new ArgumentException($"--{SingleFileCommand.Option} compiles single files with 'build' only, without --{PlanOnlyOption} on.");
            }

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
            // 끝 구분자를 뗀다(out\custom\이면 아래 증거 preset이 빈 조각을 가져 거부됐다).
            var output = PathSpelling.OnDisk(Path.TrimEndingDirectorySeparator(Path.GetFullPath(options.GetValueOrDefault("output",
                EnginePlanning.DefaultOutput(root, platformName, targetName, configuration)))));
            // build·test·sdk의 증거는 <root>/out/<preset>에 쓰므로 출력은 그 아래여야 한다. 출력·잠금을 만들기 전에 알린다.
            if (!DirectoryPath.IsStrictlyUnder(output, Path.Combine(root, "out")))
            {
                throw new ArgumentException($"--output must be under {Path.Combine(root, "out")} (build evidence location): {output}");
            }

            // 실측 실행은 그 모듈의 object를 지우고 보고서·증분 기록을 덮어쓴다. 출력을 만지기 전에 이 명령의 출력인지 다시 확인한다.
            if (profile is not null)
            {
                UnityProfile.RequireClaimedOutput(output);
            }

            // 엔진 버전의 단일 출처(Build/Build.version)를 이 실행에서 한 번만 읽는다(Docs/Design/ReflectionFoundations.ko.md 8.2절).
            // 값은 NativeBuildRequest.EngineVersion(BuildInfo 생성 TU)과 SDK manifest로만 가고 action key·ContextFingerprint에는 넣지
            // 않는다 — 넣으면 버전을 올릴 때마다 모든 compile key가 바뀐다.
            var engineVersion = EngineVersion.Read(EngineVersion.PathIn(root));

            var toolchain = android
                ? await NativeToolchains.DiscoverAndroidAsync(androidAbi, androidApi, options.GetValueOrDefault("android-ndk"), cancellationToken)
                : await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
            if (NativeToolchains.EnvironmentNotice(toolchain) is string notice)
            {
                Console.WriteLine(notice);
            }

            // 옵션 값은 출력을 만들기 전에 확인하고, 미검증 실패는 아래 evidence를 시작한 뒤 FAIL로 남긴다(6.2절).
            bool requireVerified = ToolchainCheck.RequireVerified(options.GetValueOrDefault(ToolchainCheck.RequireVerifiedOption));
            if (!requireVerified && ToolchainCheck.Notice(toolchain) is string unverified)
            {
                Console.WriteLine(unverified);
            }

            var context = new BuildContext(root, output, toolchain.Platform, toolchain.Architecture, configuration);
            var rulesPath = options.GetValueOrDefault("rules", HostTool(root, "Rocks.Engine.Rules"));
            var rules = RuleLoader.Load(rulesPath, targetName, context);
            // 모르는 모듈 이름은 출력과 잠금을 만들기 전에 알린다(메타 생성까지 가지 않는다).
            if (profile is not null && !rules.Modules.Any(module => string.Equals(module.Name, profile.Module, StringComparison.OrdinalIgnoreCase)))
            {
                throw new ArgumentException($"Unknown module {profile.Module}: no module rule has that name.");
            }

            // 설치 SDK의 엔진 라이브러리는 구성 기본 로그 하한으로 빌드되어야 한다. 게임은 같은 기본값으로 컴파일되므로 다르면 ODR 위반이다.
            if (args[0] == "sdk" && rules.Target.LinkType != NativeLinkType.Monolithic)
            {
                throw new ArgumentException($"Target {targetName}: an installed SDK is exported from a Monolithic build only (Modular SDK export is " +
                    "not supported yet — BuildTargets BS8d).");
            }

            if (args[0] == "sdk" && rules.Target.LogCompiledMinimum is not null)
            {
                throw new ArgumentException($"Target {targetName}: LogCompiledMinimum cannot be overridden for an installed SDK export " +
                    "(game builds compile against the SDK with the configuration default).");
            }

            Directory.CreateDirectory(output);
            await using var invocationLock = await OutputInvocationLock.AcquireAsync(output, cancellationToken);
            // 앞 실행의 PASS 보고서가 이번 실행의 실패(메타 단계·빌드 예외 포함)를 성공으로 보이게 하지 않는다.
            // 파일 하나 컴파일은 빌드 보고서(action-report)를 바꾸지 않는다.
            if (!singleFile)
            {
                WritePendingReport(output, "Current build has not completed.");
            }

            Directory.CreateDirectory(Path.Combine(output, "Generated"));
            var evidence = await BuildEvidence.StartAsync(root, Path.GetRelativePath(Path.Combine(root, "out"), output), toolchain.Environment);
            evidence.SetConfiguration(configuration.ToString(), targetName, configuration == NativeConfiguration.Asan ? "address" : "none", toolchain.CompilerPath, toolchain.Identity);
            evidence.SetToolchainVerified(toolchain.ToolchainVerified, toolchain.StlClangCheck);
            if (ToolchainCheck.Enforce(toolchain, requireVerified) is string rejected)
            {
                await Console.Error.WriteLineAsync(rejected);
                await evidence.FinishAsync(rejected);
                return 1;
            }

            try
            {
                // 모듈 소유 메타데이터의 배치(생성 include 디렉터리·명세 경로·검증)를 타깃에서 도달하는 모듈에 대해 정한다.
                // 아래 모든 단계가 이 결과를 쓰고, 검증 실패도 build-evidence에 기록된다.
                // 엔진 플러그인(Docs/Design/BuildTargets.ko.md 3.9절): 타깃이 켠 플러그인의 모듈만 기술자 종류대로 남기고(끈 플러그인 모듈과
                // 그 사용은 오류), 주 실행 파일에 링크하도록 표시한다. 켠 플러그인 모듈은 타깃이 참조하지 않아도 빌드 대상이다.
                // 규칙 해석·플러그인 적용·메타 배치는 projectfiles와 같은 함수다(Docs/Design/ProjectFiles.ko.md 8.1절).
                EngineLayout layout = EnginePlanning.Layout(rules, context, root, toolchain, configuration, createDirectories: true);
                var pluginSet = layout.PluginSet;
                if (args[0] == "sdk" && pluginSet.EnabledPlugins.Count != 0)
                {
                    throw new ArgumentException($"Target {targetName}: an installed SDK does not carry engine plugins; the export target enables " +
                        string.Join(", ", pluginSet.EnabledPlugins.Select(plugin => plugin.Name)) + ".");
                }

                string[] buildRoots = layout.BuildRoots;
                var plan = layout.Metadata;
                var metaStart = System.Diagnostics.Stopwatch.GetTimestamp();
                var metadata = await MetadataRunner.GenerateAsync(context, toolchain, plan, rules.Target,
                    HostTool(root, "Rocks.MetaCompiler"), cancellationToken, buildRoots, jobs: Jobs(options), verifySources: VerifyMetadataSources(options));
                evidence.RecordStep("metadata", ["Rocks.MetaCompiler", targetName], metadata,
                    System.Diagnostics.Stopwatch.GetElapsedTime(metaStart));
                if (metadata != 0)
                {
                    await evidence.FinishAsync("Metadata validation failed.");
                    return 1;
                }

                // 파일 하나 컴파일: 메타 생성까지 평소처럼 한 뒤 고른 소스의 논리 명령만 돈다(증분 기록·보고서·평소 object는 그대로다).
                if (options.TryGetValue(SingleFileCommand.Option, out string? files))
                {
                    int compiled = await SingleFileCommand.RunAsync(EnginePlanning.Request(toolchain, configuration, rules.Target, root, output,
                        plan.Modules, buildRoots, engineVersion, Jobs(options)), files, root, cancellationToken,
                        SingleFileCommand.EngineTargets(root, rulesPath, targetName, toolchain, configuration, engineVersion));
                    await evidence.FinishAsync("Single-file compile: no build was run.");
                    return compiled;
                }

                var modules = plan.Modules.ToList();
                var targets = buildRoots.ToList();
                if (profile is not null)
                {
                    int index = modules.FindIndex(module => string.Equals(module.Name, profile.Module, StringComparison.OrdinalIgnoreCase));
                    if (index < 0)
                    {
                        throw new ArgumentException($"Module {profile.Module} is not built by target {targetName}.");
                    }

                    // 규칙의 정식 이름이다(명령줄 철자의 대소문자와 다를 수 있다). 보고서 이름과 행 계산이 이 이름을 쓴다.
                    string profiled = modules[index].Name;
                    profile.ResolvedModule = profiled;
                    // 평소 빌드의 증거와 구별되게 실측 실행임을 남긴다.
                    evidence.RecordStep(UnityProfileCommand.Name, [UnityProfileCommand.Name, profiled, profile.Case?.Name ?? "prepare"], 0, TimeSpan.Zero);
                    if (profile.Case is null)
                    {
                        // 준비 실행: 그 모듈이 의존하는 모듈만 빌드한다(그 모듈 자신은 조합마다 한 번씩만 컴파일한다).
                        targets = modules[index].AllDependencies.Concat(modules[index].DynamicModules).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
                        if (targets.Count == 0)
                        {
                            var prepared = await evidence.FinishAsync();
                            return prepared.Status == "PASS" ? 0 : 1;
                        }
                    }
                    else
                    {
                        modules[index] = UnityProfile.Apply(modules[index], profile.Case);
                        targets = [profiled];
                        // 조합마다 처음부터 컴파일하게 그 모듈의 object·묶음 파일과 PCH를 지운다(의존 모듈의 object는 cache hit으로 남는다).
                        foreach (string directory in UnityProfile.ModuleOutputDirectories(output, profiled))
                        {
                            if (Directory.Exists(directory))
                            {
                                Directory.Delete(directory, recursive: true);
                            }
                        }
                    }
                }
                else if (rules.Target.CheckPublicHeaders)
                {
                    var sources = new List<string>();
                    // 실행 중 적재 모듈도 빌드하므로 공개 헤더 검사 대상이다(Docs/Design/ModuleLoader.ko.md 3.8절).
                    HashSet<string> reached = LinkClosure.Reachable(targets,
                        modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase),
                        (name, _) => throw new ArgumentException($"Unknown dependency {name}"));

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
                        // 헤더 하나가 스스로 컴파일되는지 보는 검사이므로 유니티와 PCH 없이 돈다(Docs/Design/UnityBuildPch.ko.md 10절).
                        // 공유 PCH를 쓰면 PCH에 든 헤더가 빠진 include를 메워 검사가 무력해진다.
                        Unity = UnityMode.Off,
                        PchUsage = PchUsage.Off,
                        // 공개 헤더의 쓰기 가능한 정적 저장소(공유 라이브러리마다 복제된다)를 Clang 진단으로 거부한다(Docs/Design/BuildTargets.ko.md
                        // 3.8절 BD29·BS8c). Windows 대상만 켠다(다른 대상에서는 숨김 가시성 객체만 알린다 — thread_local은 정책 검사가 따로 본다).
                        AdditionalCompileArguments = ToolchainCheck.SupportsUniqueObjectDuplication(toolchain.Platform, toolchain.CompilerVersion)
                            ? [ToolchainCheck.UniqueObjectDuplicationWarning] : [],
                        // 저장소 루트를 include하지 않는다: 검사 TU는 의존 모듈의 public include만으로 헤더를 연다. 도달한 라이브러리를
                        // 모두 공개 의존으로 두므로 모듈마다 자기 공개 환경만으로 컴파일하는 검사(BD9)가 아니다 — 그 검사는 BS1이다.
                        // BS1 전에는 의존을 비공개로 내리지 않는다(Docs/Design/BuildTargets.ko.md 3.5절, BS0 → BS1 → BS3).
                        // 서드파티 모듈은 뺀다: 공개 include가 없고(서드파티 규칙 계약) 전역 상태 서드파티의 직접 소비 모듈이 하나여야 한다(BD29·BS8c).
                        PublicDependencies = reached.Select(name => modules.Single(module => module.Name == name))
                            .Where(module => module.Kind == NativeModuleKind.StaticLibrary && !module.ThirdParty)
                            .Select(module => module.Name).Order(StringComparer.Ordinal).ToArray()
                    });
                    targets.Add("PublicHeaderChecks");
                }

                // --plan-only on(build 전용): 메타 생성과 계획까지만 하고 compile_commands.json·묶음 파일을 쓴 뒤 끝낸다. 아무것도
                // 컴파일하지 않았으므로 증거는 PASS가 아니다.
                bool planOnly = options.GetValueOrDefault(PlanOnlyOption, "off") switch
                {
                    "on" when args[0] == "build" => true,
                    "off" => false,
                    "on" => throw new ArgumentException($"--{PlanOnlyOption} on is supported by 'build' only."),
                    _ => throw new ArgumentException($"{PlanOnlyOption} must be on or off.")
                };
                var nativeStart = System.Diagnostics.Stopwatch.GetTimestamp();
                var result = await new NativeBuildEngine().BuildAsync(EnginePlanning.Request(toolchain, configuration, rules.Target, root, output,
                    modules, targets, engineVersion, Jobs(options)) with
                {
                    Unity = Switch(options, UnityOption),
                    Pch = Switch(options, PchOption),
                    Adaptive = Switch(options, AdaptiveOption),
                    Timing = Timing(options),
                    TimingLabel = targetName,
                    PlanOnly = planOnly,
                    MeasureProcesses = profile?.Case is not null
                }, cancellationToken);
                // 벽시계는 native 단계가 끝난 바로 이 자리에서 잡는다(뒤의 출력·보고서 쓰기를 넣지 않는다).
                TimeSpan nativeElapsed = System.Diagnostics.Stopwatch.GetElapsedTime(nativeStart);
                if (result.PlanOnly)
                {
                    Console.WriteLine($"Plan only: wrote {result.CompilationDatabasePath} and {result.UnityBatches} unity batch file(s); no action was run.");
                    await evidence.FinishAsync("Plan only: no action was run.");
                    return 0;
                }

                Print(result);
                // Visual Studio 프로젝트 파일 낡음 경고(Docs/Design/ProjectFiles.ko.md 9절). 생성기 모델과 같은 조건(Windows 호스트, PCH 켬,
                // 기본 출력, IDE 구성)일 때만 비교한다 — Android·--pch off 빌드의 논리 명령은 IDE 모델과 다르고, --output·Coverage 빌드는
                // 프로젝트 파일이 가리키는 출력이 아니다.
                if (!android && OperatingSystem.IsWindows() && Switch(options, PchOption) && !options.ContainsKey("output") &&
                    ProjectFilesCommand.IsIdeConfiguration(configuration) &&
                    ProjectFilesStaleness.Warning(root, rules.Target, configuration, toolchain, result) is string stale)
                {
                    Console.WriteLine(stale);
                }

                AtomicFile.WriteIfChanged(Path.Combine(output, "action-report.json"), JsonSerializer.Serialize(result, JsonOptions));
                evidence.RecordStep("native", ["Rocks.Build", targetName, configuration.ToString()], result.Succeeded ? 0 : 1, nativeElapsed);
                if (!result.Succeeded)
                {
                    if (profile is not null)
                    {
                        NativeActionResult? failed = result.Actions.FirstOrDefault(action => action.Executed && !action.Succeeded);
                        profile.Failure = UnityProfile.FirstErrorLine(failed is null ? result.Failure : failed.StdErr + "\n" + failed.StdOut + "\n" + result.Failure);
                    }

                    await evidence.FinishAsync(result.Failure);
                    return 1;
                }

                if (profile?.Case is not null)
                {
                    profile.Row = UnityProfile.Row(profile.Case, result, targets[0], nativeElapsed);
                }

                if (args[0] == "sdk")
                {
                    await sdkAssembly!.AddConfigurationAsync(root, configuration, toolchain, engineVersion, evidence.SourceBefore,
                        InstalledSdk.Reachable(plan.Modules, targets), plan.Generated, result, cancellationToken);
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
                        int deviceExit = await AndroidDevice.RunTestsAsync(root, executable, report, options.GetValueOrDefault("adb"),
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
                    // 제한은 멈춘 시험을 끊는 장치다. 네이티브 시험은 그래프 한도 fixture처럼 큰 입력을 쓰고 Asan은 계측 때문에
                    // 몇 배 느리므로(Windows CI 러너 Asan에서 기본값 5분을 넘겼다) 구성별로 넉넉히 둔다.
                    TimeSpan nativeLimit = configuration is NativeConfiguration.Asan or NativeConfiguration.Coverage ? TimeSpan.FromMinutes(30) : TimeSpan.FromMinutes(15);
                    var native = await ProcessRunner.RunAsync(new(executable, ["--seed", "7351", "--json", report], root, toolchain.Environment,
                        Timeout: nativeLimit), cancellationToken);
                    Console.Write(native.StdOut);
                    Console.Error.Write(native.StdErr);
                    evidence.RecordStep("foundation", [executable, "--seed", "7351", "--json", report], native.ExitCode, System.Diagnostics.Stopwatch.GetElapsedTime(testStart), tests: true);
                    if (native.ExitCode != 0)
                    {
                        await evidence.FinishAsync("Foundation tests failed.");
                        return 1;
                    }

                    // 모듈 API 매크로(Docs/Design/BuildTargets.ko.md 3.8절 BD27)는 빌드가 정의 헤더로 넣는다. 정의 헤더 없이 헤더를 컴파일하는
                    // 검사(native 컴파일 시험·생성 헤더 검증)는 Monolithic 값(빈 값)을 받는다.
                    string[] apiMacros = [.. modules.Where(module => !module.ThirdParty)
                        .SelectMany(module => new[] { "--clang-arg", "-D" + LinkTypes.ApiMacro(module.Name) + "=" })];
                    var checks = new List<string[]> {
                        new[] { "self", "--root", root, "--json", Path.Combine(output, "tool-self-report.json") },
                        // native 검사의 컴파일 시험(AlgorithmsCompilerTests 등)도 Core 헤더를 include하므로 구성·대상 OS 매크로를 받는다.
                        (string[])["native", "--root", root, "--native-executable", executable, "--compiler", toolchain.CompilerPath, "--compile-commands", result.CompilationDatabasePath,
                            "--clang-arg", "-D" + NativeConfigurations.BuildMacro(configuration), "--clang-arg", "-D" + NativeConfigurations.PlatformMacro(toolchain.Platform),
                            "--clang-arg", "-D" + LinkTypes.MonolithicDefine(rules.Target.LinkType), .. apiMacros,
                            "--json", Path.Combine(output, "tool-native-report.json")]
                    };
                    if (configuration == NativeConfiguration.Asan)
                    {
                        checks[1] = [.. checks[1], "--asan"];
                    }

                    if (plan.Generated.Count != 0)
                    {
                        // 생성 헤더 검증 컴파일은 Core 헤더를 include하므로 이 구성·대상 OS의 매크로가 필요하다.
                        checks.Add(["meta", "--root", root, "--compiler", toolchain.CompilerPath, "--metacompiler", HostTool(root, "Rocks.MetaCompiler"),
                            "--clang-arg", "-D" + NativeConfigurations.BuildMacro(configuration),
                            "--clang-arg", "-D" + NativeConfigurations.PlatformMacro(toolchain.Platform),
                            "--clang-arg", "-D" + LinkTypes.MonolithicDefine(rules.Target.LinkType), .. apiMacros,
                            "--json", Path.Combine(output, "tool-meta-report.json")]);
                    }

                    foreach (var check in checks)
                    {
                        var command = new[] { HostTool(root, "Rocks.ToolTests") }.Concat(check).ToArray();
                        var checkStart = System.Diagnostics.Stopwatch.GetTimestamp();
                        // 제한은 멈춘 검사를 끊는 장치다. meta는 시험마다 MetaCompiler·Clang을 띄워 3코어 macOS
                        // 러너에서 병렬로도 9분 넘게 걸리므로(128건) 다른 검사보다 넉넉히 둔다.
                        TimeSpan checkLimit = check[0] == "meta" ? TimeSpan.FromMinutes(30) : TimeSpan.FromMinutes(10);
                        var tested = await ProcessRunner.RunAsync(new("dotnet", command, root, toolchain.Environment, Timeout: checkLimit), cancellationToken);
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

        // 빌드를 시작하기 전에 실패 상태의 action-report.json을 쓴다. 빌드가 끝나야 실제 결과로 바뀐다.
        internal static void WritePendingReport(string output, string failure) =>
            AtomicFile.WriteIfChanged(Path.Combine(output, "action-report.json"), JsonSerializer.Serialize(new NativeBuildResult
            {
                Succeeded = false,
                ExecutedActions = 0,
                SkippedActions = 0,
                Actions = [],
                ModuleOutputs = new Dictionary<string, string>(),
                CompilationDatabasePath = "",
                Failure = failure
            }, JsonOptions));

        internal static string HostTool(string root, string name) => Path.Combine(root, "out/dotnet/bin", name, "Debug/net10.0", name + ".dll");

        internal static NativeConfiguration Configuration(Dictionary<string, string> options) =>
            Enum.TryParse<NativeConfiguration>(options.GetValueOrDefault("configuration", "Debug"), true, out var value) && Enum.IsDefined(value)
                ? value : throw new ArgumentException("Configuration must be Debug, Development, Release, or Asan.");

        internal static int Jobs(Dictionary<string, string> options) =>
            int.TryParse(options.GetValueOrDefault("jobs", Math.Min(8, Environment.ProcessorCount).ToString()), out var value) &&
            value > 0 && value <= ProcessRunner.MaximumConcurrentProcesses
                ? value : throw new ArgumentException($"jobs must be between 1 and {ProcessRunner.MaximumConcurrentProcesses}.");

        // 빌드 방식 옵션이다(Docs/Design/UnityBuildPch.ko.md 11절). 값은 on|off이고 기본은 모두 on이다. --unity off와 --pch off는 유니티
        // 묶기와 PCH를 끈 검증 변형이고(object는 변형마다 다른 디렉터리 obj/<모듈>/<변형>에 둔다), --adaptive off는 작업 파일 분리를
        // 끈다(CI). 도구 빌드 ID와 compile_commands.json에는 반영되지 않는다.
        internal const string PlanOnlyOption = "plan-only";

        // --timing on|off(기본 off): compile·PCH action에 -ftime-trace를 붙이고 <out>/Timing/<타깃>.timing.json으로 집계한다
        // (Docs/Design/UnityBuildPch.ko.md 12절 — 공유 PCH 헤더 구성을 정하는 자료다).
        internal const string TimingOption = "timing";

        internal static bool Timing(Dictionary<string, string> options) =>
            options.GetValueOrDefault(TimingOption, "off") switch
            {
                "on" => true,
                "off" => false,
                _ => throw new ArgumentException($"{TimingOption} must be on or off.")
            };

        internal const string UnityOption = "unity";

        internal const string PchOption = "pch";

        internal const string AdaptiveOption = "adaptive";

        internal static readonly string[] BuildModeOptions = [UnityOption, PchOption, AdaptiveOption, TimingOption];

        internal static bool Switch(Dictionary<string, string> options, string name) =>
            options.GetValueOrDefault(name, "on") switch
            {
                "on" => true,
                "off" => false,
                _ => throw new ArgumentException($"{name} must be on or off.")
            };

        // --verify-metadata-sources on|off(기본 off): on이면 MetaCompiler가 게시 전에 생성 소스도 컴파일한다. 본문 계약 위반을 빌드의
        // compile action 대신 메타 단계가 원본 위치와 함께 알린다(Docs/Design/ReflectionGeneratedSources.ko.md 7절).
        internal static bool VerifyMetadataSources(Dictionary<string, string> options) =>
            options.GetValueOrDefault(MetadataLayout.VerifySourcesOption, "off") switch
            {
                "on" => true,
                "off" => false,
                _ => throw new ArgumentException($"{MetadataLayout.VerifySourcesOption} must be on or off.")
            };

        // --require-verified-toolchain on|off(기본 off): on이면 검증 목록(Build/Toolchain.json) 밖의 컴파일러로 빌드하지 않는다(CI — Docs/Design/
        // ClangVersionSupport.ko.md 6.2절). off면 한 줄 알리고 진행한다(build-evidence에 toolchainVerified: false). 판정은 ToolchainCheck가 한다.
        internal const string RequireVerifiedToolchainOption = ToolchainCheck.RequireVerifiedOption;

        internal static Dictionary<string, string> Parse(string[] args)
        {
            var allowed = new HashSet<string>(["root", "target", "configuration", "output", "compiler", "rules", "jobs", "destination", "sdk", "project", "platform", "android-abi", "android-api", "android-ndk", "adb", "library", "store", "mirror", "offline", MetadataLayout.VerifySourcesOption, PlanOnlyOption, .. BuildModeOptions, "windows-sdk", RequireVerifiedToolchainOption, .. UnityProfileCommand.Options, .. ProjectFilesCommand.Options, .. SdkCommands.Options, .. SdkExportCommand.Options, .. NewProjectCommand.Options, SingleFileCommand.Option], StringComparer.Ordinal);
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

            // 첫 #include 앞의 #define 때문에 묶음에서 뺀 파일을 알린다(Docs/Design/UnityBuildPch.ko.md 6.1절 — 단독 번역 단위로 컴파일한다).
            if (result.UnityDefineExcluded.Count != 0)
            {
                Console.WriteLine($"Unity: {result.UnityDefineExcluded.Count} source file(s) define macros before their first #include and compile as standalone translation units:");
                foreach (string source in result.UnityDefineExcluded)
                {
                    Console.WriteLine("  " + source);
                }
            }

            foreach (string notice in result.Notices)
            {
                Console.WriteLine("note: " + notice);
            }

            Console.WriteLine($"Native build: {(result.Succeeded ? "PASS" : "FAIL")}; executed={result.ExecutedActions}; cached={result.SkippedActions}; " +
                $"unity-batches={result.UnityBatches}; pch={result.PchActions}; adaptive-separated={result.AdaptiveSeparated}");
            if (result.Failure is not null)
            {
                Console.Error.WriteLine(result.Failure);
            }
        }
    }
}
