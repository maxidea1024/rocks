using System.Security;
using System.Text.Json;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

// SystemLibraries·Frameworks는 정적 라이브러리가 요구하는 플랫폼 링크 의존이다. 외부 게임의 최종 링크가 같은 선언을
// 받아야 하므로 SDK에 함께 담는다(NativeModuleInput과 같은 의미·검증). Descriptor·IdentityManifest는 리플렉션 헤더가
// 있는 모듈의 descriptor(<M>.rocksmeta.json)와 identity 명세의 SDK 상대 경로다(없으면 둘 다 null). 게임 빌드는 이 모듈을
// 다시 생성하지 않고 descriptor를 import하며 명세를 명세 집합 검증에 넣는다. ProvidesBuildInfo는 NativeModuleInput과 같은 표지다
// (엔진 Core — 게임 실행 파일이 이 모듈에 닿으면 빌드가 BuildInfo 정의 TU를 더한다).
public sealed record SdkModule(string Name, string[] Includes, string[] Defines, string[] Dependencies, string[] Libraries,
    string[] SystemLibraries, string[] Frameworks, string? Descriptor, string? IdentityManifest, bool ProvidesBuildInfo);

public sealed record SdkManifest(int Version, string Configuration, string Platform, string Architecture,
    string CompilerIdentity, string SdkIdentity, SdkModule[] Modules, Dictionary<string, string> Files);

public static class InstalledSdk
{
    // 2: SdkModule에 SystemLibraries·Frameworks 추가. 3: Descriptor·IdentityManifest 추가. 4: ProvidesBuildInfo 추가, AbiIdentity를
    // effective triple로 계산(B7 — B7 이전 SDK는 생성 TU의 Core/Build/BuildInfo.h include가 깨지는 대신 버전 불일치로 거부한다).
    internal const int ManifestVersion = 4;

    /// <summary>
    /// SDK 타깃에서 도달하는 모듈만 고른다. 규칙 로더는 모든 모듈 규칙을 읽으므로 타깃이 쓰지 않는(빌드되지 않은)
    /// 모듈까지 내보내려 하면 "built for this SDK target" 오류가 난다.
    /// </summary>
    public static IReadOnlyList<NativeModuleInput> Reachable(IReadOnlyList<NativeModuleInput> modules, IEnumerable<string> targets)
    {
        var byName = modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        var reached = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var pending = new Stack<string>(targets);
        while (pending.Count != 0)
        {
            string name = pending.Pop();
            if (!reached.Add(name) || !byName.TryGetValue(name, out var module))
            {
                continue;
            }

            foreach (string dependency in module.Dependencies)
            {
                pending.Push(dependency);
            }
        }

        return modules.Where(module => reached.Contains(module.Name)).ToArray();
    }

    public static async Task ExportAsync(string root, string destination, NativeConfiguration configuration,
        NativeToolchain toolchain, IReadOnlyList<NativeModuleInput> modules, IReadOnlyList<MetadataModule> metadata,
        NativeBuildResult build, CancellationToken cancellationToken)
    {
        // 끝 구분자를 떼야 stage가 destination 옆(같은 부모 아래)에 생긴다(C:\sdk\v1\이면 stage가 destination 안에 생겨 Move가
        // 실패했다). TrimEndingDirectorySeparator는 루트를 그대로 두므로 루트는 따로 거부한다.
        destination = Path.TrimEndingDirectorySeparator(Path.GetFullPath(destination));
        if (Path.GetDirectoryName(destination) is null)
        {
            throw new IOException("SDK destination must not be a filesystem root. Use a new versioned directory.");
        }

        if (Directory.Exists(destination) || File.Exists(destination))
        {
            throw new IOException("SDK destination already exists. Use a new versioned directory.");
        }

        var stage = destination + ".staging-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(stage);
        try
        {
            var packaged = new List<SdkModule>();
            foreach (var module in modules.Where(module => module.Kind == NativeModuleKind.StaticLibrary))
            {
                // 원시 compile·link 인자와 외부 prebuilt 라이브러리는 ABI·배포 규칙이 정해지지 않아 거부한다.
                // 플랫폼 시스템 라이브러리·framework는 구조화된 선언이라 SDK manifest로 전달한다.
                if (module.PrebuiltLibraries.Count != 0 || module.AdditionalLinkArguments.Count != 0 || module.AdditionalCompileArguments.Count != 0)
                {
                    throw new NotSupportedException($"SDK module '{module.Name}' declares external libraries/custom compile or link flags. Add an explicit SDK packaging/ABI rule before exporting this module.");
                }

                var includes = new List<string>();
                foreach (var directory in module.PublicIncludeDirectories)
                {
                    var relative = $"Headers/{module.Name}/{includes.Count}";
                    CopyTree(directory, Path.Combine(stage, relative));
                    includes.Add(relative);
                }

                var libraries = new List<string>();
                if (build.ModuleOutputs.TryGetValue(module.Name, out var library))
                {
                    var relative = "Libraries/" + Path.GetFileName(library);
                    Directory.CreateDirectory(Path.Combine(stage, "Libraries"));
                    File.Copy(library, Path.Combine(stage, relative));
                    libraries.Add(relative);
                }
                else if (module.Sources.Count != 0)
                {
                    throw new InvalidOperationException($"Module {module.Name} was not built for this SDK target.");
                }

                // 생성 public 헤더는 모듈의 public include 디렉터리라 위에서 Headers/<M>/로 복사되었다.
                // descriptor와 명세는 Metadata/<M>/로 내보낸다.
                string? descriptor = null;
                string? identityManifest = null;
                if (metadata.FirstOrDefault(item => item.Name == module.Name) is MetadataModule reflected)
                {
                    if (!File.Exists(reflected.Descriptor))
                    {
                        throw new InvalidOperationException($"Module {module.Name} has no generated metadata descriptor: {reflected.Descriptor}");
                    }

                    descriptor = $"Metadata/{module.Name}/{module.Name}.rocksmeta.json";
                    identityManifest = $"Metadata/{module.Name}/{module.Name}.rocksids.json";
                    Directory.CreateDirectory(Path.Combine(stage, "Metadata", module.Name));
                    File.Copy(reflected.Descriptor, Path.Combine(stage, descriptor));
                    File.Copy(reflected.Manifest, Path.Combine(stage, identityManifest));
                }

                // BuildInfo 표지 모듈은 생성 TU가 include하는 선언 헤더를 public include로 담아야 한다.
                if (module.ProvidesBuildInfo &&
                    !includes.Any(relative => File.Exists(Path.Combine(stage, relative, "Core", "Build", "BuildInfo.h"))))
                {
                    throw new InvalidOperationException($"SDK module '{module.Name}' provides BuildInfo but exports no Core/Build/BuildInfo.h.");
                }

                packaged.Add(new(module.Name, includes.ToArray(), module.PublicDefines.ToArray(),
                    module.Dependencies.ToArray(), libraries.ToArray(), module.SystemLibraries.ToArray(),
                    module.Frameworks.ToArray(), descriptor, identityManifest, module.ProvidesBuildInfo));
            }
            // 고정 버전 외부 라이브러리의 라이선스 원문을 모두 함께 배포한다(BSD 계열은 바이너리 배포 시 고지 의무가 있다).
            // code 라이브러리는 모듈을 담을 때, 데이터 전용 라이브러리(예: Unicode UCD)는 파생물이 들어간 모듈(embeddedIn)을
            // 담을 때 복사하고, 시험 전용 데이터(embeddedIn 없음)는 배포하지 않는다. 경로는 Licenses/<Lib>/<원본 디렉터리 기준
            // 경로>라 이름이 같은 원문도 겹치지 않는다(ThirdPartyManifest.CopySdkLicenses).
            ThirdPartyManifest.CopySdkLicenses(root, stage, packaged.Select(module => module.Name).ToArray());
            // SDK 자체의 도구만으로 외부 게임 규칙을 빌드할 수 있어야 한다.
            foreach (var tool in new[] { "Rocks.Build.Cli", "Rocks.MetaCompiler" })
            {
                CopyTree(Path.GetDirectoryName(BuildCommand.HostTool(root, tool))!, Path.Combine(stage, "Tools", tool));
            }

            File.Copy(Path.Combine(root, "global.json"), Path.Combine(stage, "global.json"));
            File.Copy(Path.Combine(root, ".clang-format"), Path.Combine(stage, ".clang-format"));
            var hashes = Directory.GetFiles(stage, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal)
                .ToDictionary(file => Path.GetRelativePath(stage, file).Replace('\\', '/'), Hashing.Sha256File, StringComparer.Ordinal);
            var manifest = new SdkManifest(ManifestVersion, configuration.ToString(), toolchain.Platform.ToString(),
                System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString(), toolchain.AbiIdentity,
                toolchain.AbiIdentity, packaged.ToArray(), hashes);
            AtomicFile.WriteIfChanged(Path.Combine(stage, "rocks-sdk.json"), JsonSerializer.Serialize(manifest, BuildCommand.JsonOptions));
            cancellationToken.ThrowIfCancellationRequested();
            Directory.Move(stage, destination);
            Console.WriteLine($"Installed SDK: {destination}");
            await Task.CompletedTask;
        }
        finally
        {
            // 삭제 대상은 이 호출이 만든 동일 부모의 임의 stage 하나로 제한한다.
            if (Directory.Exists(stage))
            {
                Directory.Delete(stage, recursive: true);
            }
        }
    }

    public static async Task<int> BuildGameAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        var sdk = Path.GetFullPath(options["sdk"]);
        var project = Path.GetFullPath(options["project"]);
        var output = Path.GetFullPath(options.GetValueOrDefault("output", Path.Combine(project, "out", BuildCommand.Configuration(options).ToString())));
        if (Inside(sdk, output) || Inside(sdk, project) || Inside(output, project) || Inside(output, sdk))
        {
            throw new ArgumentException("Game output cannot overlap or contain the project or installed SDK.");
        }

        Directory.CreateDirectory(output);
        await using var invocationLock = await OutputInvocationLock.AcquireAsync(output, cancellationToken);
        var started = BeginGameEvidence(output, sdk, project);
        // 앞 실행의 PASS 보고서가 이번 실패를 성공으로 보이게 하지 않는다. 네이티브 출력 자체는 보존한다.
        BuildCommand.WritePendingReport(output, "Current game build has not completed.");
        var status = "FAIL";
        string? failure = null;
        try
        {
            var result = await BuildGameCoreAsync(options, cancellationToken);
            status = result == 0 ? "PASS" : "FAIL";
            return result;
        }
        catch (Exception error)
        {
            failure = error.Message;
            throw;
        }
        finally
        {
            AtomicFile.WriteIfChanged(Path.Combine(output, "game-build-evidence.json"), JsonSerializer.Serialize(new
            {
                status,
                startedUtc = started,
                finishedUtc = DateTime.UtcNow,
                sdk,
                project,
                error = failure
            }, BuildCommand.JsonOptions));
        }
    }

    public static DateTime BeginGameEvidence(string output, string sdk, string project)
    {
        var started = DateTime.UtcNow;
        AtomicFile.WriteIfChanged(Path.Combine(output, "game-build-evidence.json"), JsonSerializer.Serialize(new
        {
            status = "RUNNING",
            startedUtc = started,
            finishedUtc = (DateTime?)null,
            sdk,
            project,
            error = (string?)null
        }, BuildCommand.JsonOptions));
        return started;
    }

    private static async Task<int> BuildGameCoreAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        var sdk = Path.GetFullPath(options["sdk"]);
        var project = Path.GetFullPath(options["project"]);
        var configuration = BuildCommand.Configuration(options);
        var output = Path.GetFullPath(options.GetValueOrDefault("output", Path.Combine(project, "out", configuration.ToString())));
        if (Inside(sdk, output) || Inside(sdk, project) || Inside(output, project) || Inside(output, sdk))
        {
            throw new ArgumentException("Game output cannot overlap or contain the project or installed SDK.");
        }

        var toolchain = await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
        if (NativeToolchains.EnvironmentNotice(toolchain) is string notice)
        {
            Console.WriteLine(notice);
        }

        var manifest = Verify(sdk, configuration, toolchain);
        Directory.CreateDirectory(output);
        Directory.CreateDirectory(Path.Combine(output, "Generated"));
        var initialProject = GameInputSnapshot.Capture(project, output, sdk, []);
        // 프로젝트 기술자(UE .uproject 대응)가 빌드할 모듈·플러그인·플랫폼을 정한다. 입력 snapshot 뒤에 읽어
        // 빌드 중 기술자 변경을 최종 검사가 잡게 하고, 규칙을 컴파일하기 전에 기술자 오류를 먼저 알린다.
        var projectFiles = Directory.GetFiles(project, "*" + DescriptorReader.ProjectExtension, SearchOption.TopDirectoryOnly);
        if (projectFiles.Length != 1)
        {
            throw new ArgumentException($"The game project root must contain exactly one {DescriptorReader.ProjectExtension} file (found {projectFiles.Length}).");
        }

        var projectDescriptor = DescriptorReader.ReadProject(projectFiles[0]);
        var moduleSet = DescriptorModuleSet.Resolve(projectDescriptor,
            PluginCatalog.Discover([Path.Combine(project, "Plugins")]), toolchain.Platform, configuration);
        var rulesFiles = Directory.GetFiles(project, "*.cs", SearchOption.AllDirectories)
            .Where(file => !Inside(output, file) && (file.EndsWith(".Build.cs", StringComparison.Ordinal) || file.EndsWith(".Target.cs", StringComparison.Ordinal)))
            .Order(StringComparer.Ordinal).ToArray();
        if (rulesFiles.Length == 0)
        {
            throw new ArgumentException("No *.Build.cs or *.Target.cs rules found in the game project.");
        }

        var rulesDirectory = Path.Combine(output, "Rules");
        var projectFile = Path.Combine(rulesDirectory, "Rocks.Game.Rules.csproj");
        string Escape(string value) => SecurityElement.Escape(value)!;
        var references = new[] { "Rocks.Build.Abstractions", "Rocks.Build", "Rocks.Tooling.Common" }.Select(name =>
            $"<Reference Include=\"{name}\"><HintPath>{Escape(Path.Combine(sdk, "Tools/Rocks.Build.Cli", name + ".dll"))}</HintPath></Reference>");
        // 사용자 게임 디렉터리의 Directory.Build.props가 호스트 bootstrap을 바꾸지 못하게 입력을 고정한다.
        var projectXml = "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup>" +
            "<TargetFramework>net10.0</TargetFramework><Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings>" +
            "<TreatWarningsAsErrors>true</TreatWarningsAsErrors><EnableDefaultCompileItems>false</EnableDefaultCompileItems>" +
            "<AssemblyName>Rocks.Game.Rules</AssemblyName><Deterministic>true</Deterministic>" +
            "</PropertyGroup><ItemGroup>" + string.Join("", rulesFiles.Select(file => $"<Compile Include=\"{Escape(file)}\" />")) +
            string.Join("", references) + "</ItemGroup></Project>\n";
        AtomicFile.WriteIfChanged(projectFile, projectXml);
        var compiled = await ProcessRunner.RunAsync(new("dotnet", ["build", projectFile, "--nologo", "-warnaserror",
            "-p:ImportDirectoryBuildProps=false", "-p:ImportDirectoryBuildTargets=false"], sdk, toolchain.Environment), cancellationToken);
        Console.Write(compiled.StdOut);
        Console.Error.Write(compiled.StdErr);
        if (compiled.ExitCode != 0)
        {
            return 1;
        }

        var rulesPath = Path.Combine(rulesDirectory, "bin/Debug/net10.0/Rocks.Game.Rules.dll");
        var context = new BuildContext(project, output, toolchain.Platform, toolchain.Architecture, configuration, sdk);
        var rules = RuleLoader.Load(rulesPath, options.GetValueOrDefault("target", "Game"), context);
        // 규칙 모듈을 기술자와 대조하고, 켜지지 않은 플러그인·Editor·플랫폼 미지원 모듈 등을 뺀다.
        var projectModules = moduleSet.Apply(rules.Modules, rules.Target.Modules);
        // SDK 모듈은 다시 생성하지 않는다. 리플렉션 모듈이면 descriptor·명세를 ImportedMetadata로 넘긴다.
        var modules = manifest.Modules.Select(module => new NativeModuleInput
        {
            Name = module.Name,
            PublicIncludeDirectories = module.Includes.Select(path => SafePath(sdk, path)).ToArray(),
            PublicDefines = module.Defines,
            Dependencies = module.Dependencies,
            PrebuiltLibraries = module.Libraries.Select(path => SafePath(sdk, path)).ToArray(),
            SystemLibraries = module.SystemLibraries,
            Frameworks = module.Frameworks,
            ImportedMetadata = module.Descriptor is null
                ? null
                : new ImportedMetadata(SafePath(sdk, module.Descriptor), SafePath(sdk, module.IdentityManifest!)),
            ProvidesBuildInfo = module.ProvidesBuildInfo
        }).Concat(projectModules).ToArray();
        // 켠 플러그인의 모듈도 빌드한다(UE처럼 타깃이 직접 참조하지 않아도 컴파일된다). 메타데이터 생성도 같은 root 집합이다.
        string[] targets = rules.Target.Modules.Concat(moduleSet.IncludedPluginModules).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        var plan = MetadataLayout.Apply(context, modules, targets);
        var declaredInputs = GameInputSnapshot.DeclaredInputs(context, plan);
        var initialDeclared = GameInputSnapshot.Capture(project, output, sdk, declaredInputs);
        var metadata = await MetadataRunner.GenerateAsync(context, toolchain, plan, rules.Target,
            Path.Combine(sdk, "Tools/Rocks.MetaCompiler/Rocks.MetaCompiler.dll"), cancellationToken, targets);
        if (metadata != 0)
        {
            return 1;
        }

        var result = await new NativeBuildEngine().BuildAsync(new()
        {
            Toolchain = toolchain,
            Configuration = configuration,
            OutputDirectory = output,
            // 정적 링크이므로 실행 파일에는 참조한 모듈만 들어간다.
            RootDirectory = project,
            Modules = plan.Modules,
            Targets = targets,
            ContextFingerprint = "rocks-installed-sdk-v1",
            MaxParallelism = BuildCommand.Jobs(options),
            // 도구 빌드 ID의 sdk: 경로 키 기준이다(SDK 헤더·라이브러리가 설치 위치와 무관한 키를 갖는다).
            SdkDirectory = sdk
        }, cancellationToken);
        BuildCommand.Print(result);
        // 최종 검사 전 성공 보고서를 게시하면 SDK/입력 변경에 따른 실패가 PASS로 보일 수 있다.
        PublishVerifiedGameResult(sdk, configuration, toolchain, project, output, initialProject,
            initialDeclared, declaredInputs, result);
        return result.Succeeded ? 0 : 1;
    }

    public static void PublishVerifiedGameResult(string sdk, NativeConfiguration configuration, NativeToolchain toolchain,
        string project, string output, string initialProject, string initialDeclared,
        IReadOnlyList<string> declaredInputs, NativeBuildResult result)
    {
        Verify(sdk, configuration, toolchain);
        if (GameInputSnapshot.Capture(project, output, sdk, []) != initialProject ||
            GameInputSnapshot.Capture(project, output, sdk, declaredInputs) != initialDeclared)
        {
            throw new InvalidOperationException("Game inputs changed during rule evaluation, metadata generation, or native build. Retry the build.");
        }

        AtomicFile.WriteIfChanged(Path.Combine(output, "action-report.json"), JsonSerializer.Serialize(result, BuildCommand.JsonOptions));
    }

    public static SdkManifest Verify(string sdk, NativeConfiguration configuration, NativeToolchain toolchain)
    {
        var manifest = JsonSerializer.Deserialize<SdkManifest>(File.ReadAllText(Path.Combine(sdk, "rocks-sdk.json")), new JsonSerializerOptions { UnmappedMemberHandling = System.Text.Json.Serialization.JsonUnmappedMemberHandling.Disallow })
            ?? throw new InvalidDataException("Empty SDK manifest.");
        if (manifest.Version != ManifestVersion)
        {
            throw new InvalidDataException($"SDK manifest version mismatch: {manifest.Version} (this build tool requires {ManifestVersion}). " +
                "Re-export the SDK with the current tools.");
        }

        if (manifest.Configuration != configuration.ToString() ||
            manifest.Platform != toolchain.Platform.ToString() || manifest.CompilerIdentity != toolchain.AbiIdentity ||
            manifest.SdkIdentity != toolchain.AbiIdentity ||
            manifest.Architecture != System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString())
        {
            throw new InvalidDataException("SDK configuration, compiler, platform, architecture, or native SDK ABI mismatch.");
        }

        if (manifest.Files is null || manifest.Modules is null || manifest.Files.Count == 0 || manifest.Modules.Length == 0)
        {
            throw new InvalidDataException("SDK manifest is missing its file inventory or module declarations.");
        }

        var actual = new HashSet<string>(StringComparer.Ordinal);
        void Inventory(string directory)
        {
            foreach (var entry in Directory.EnumerateFileSystemEntries(directory))
            {
                if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0)
                {
                    throw new InvalidDataException("SDK inventory must not contain linked files/directories.");
                }

                if (Directory.Exists(entry))
                {
                    Inventory(entry);
                }
                else if (Path.GetRelativePath(sdk, entry) != "rocks-sdk.json")
                {
                    actual.Add(Path.GetRelativePath(sdk, entry).Replace('\\', '/'));
                }
            }
        }

        Inventory(sdk);
        if (!actual.SetEquals(manifest.Files.Keys))
        {
            throw new InvalidDataException("SDK file inventory changed. Re-export instead of adding/removing installed headers or tools.");
        }

        if (manifest.Modules.Select(module => module.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != manifest.Modules.Length)
        {
            throw new InvalidDataException("SDK contains duplicate module names.");
        }

        foreach (var pair in manifest.Files)
        {
            if (!File.Exists(SafePath(sdk, pair.Key)) || Hashing.Sha256File(SafePath(sdk, pair.Key)) != pair.Value)
            {
                throw new InvalidDataException($"SDK file changed or missing: {pair.Key}");
            }
        }

        foreach (var module in manifest.Modules)
        {
            if (module.Includes is null || module.Libraries is null || module.Dependencies is null || module.Defines is null ||
                module.SystemLibraries is null || module.Frameworks is null)
            {
                throw new InvalidDataException($"SDK module '{module.Name}' has incomplete declarations.");
            }

            foreach (var include in module.Includes)
            {
                if (!Directory.Exists(SafePath(sdk, include)))
                {
                    throw new InvalidDataException($"Missing SDK include directory: {include}");
                }
            }

            foreach (var dependency in module.Dependencies)
            {
                if (!manifest.Modules.Any(candidate => candidate.Name == dependency))
                {
                    throw new InvalidDataException($"SDK module '{module.Name}' has unknown dependency '{dependency}'.");
                }
            }

            foreach (var library in module.Libraries)
            {
                if (!manifest.Files.ContainsKey(library))
                {
                    throw new InvalidDataException("Unverified SDK library.");
                }
            }

            // 리플렉션 모듈은 descriptor와 명세를 함께 담는다(둘 다 검증된 SDK 파일).
            if ((module.Descriptor is null) != (module.IdentityManifest is null) ||
                (module.Descriptor is not null && (!manifest.Files.ContainsKey(module.Descriptor) || !manifest.Files.ContainsKey(module.IdentityManifest!))))
            {
                throw new InvalidDataException($"SDK module '{module.Name}' has incomplete or unverified reflection metadata.");
            }
        }

        return manifest;
    }

    private static string SafePath(string root, string relative)
    {
        var full = Path.GetFullPath(relative, root);
        if (Path.IsPathRooted(relative) || !Inside(root, full) || relative.Split('/', '\\').Any(part => part == ".."))
        {
            throw new InvalidDataException("SDK path escapes its root.");
        }

        return full;
    }

    private static bool Inside(string root, string candidate) => DirectoryPath.IsAtOrUnder(candidate, root);

    private static void CopyTree(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var entry in Directory.EnumerateFileSystemEntries(source))
        {
            if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException("SDK packaging does not accept symbolic links/reparse points.");
            }

            var target = Path.Combine(destination, Path.GetFileName(entry));
            if (Directory.Exists(entry))
            {
                CopyTree(entry, target);
            }
            else
            {
                File.Copy(entry, target);
            }
        }
    }
}
