using System.Security;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>게임 타깃 하나의 계획 입력이다. Metadata는 메타 배치를 거친 모듈, Targets는 빌드 근이다.</summary>
internal sealed record GameLayout(DescriptorModuleSet ModuleSet, MetadataPlan Metadata, string[] Targets);

/// <summary>
/// 설치 SDK 게임의 부작용 없는 계획 함수다(Docs/Design/ProjectFiles.ko.md 8.1·10.4절). 게임 빌드(<c>game</c>)와 게임 프로젝트 파일 생성
/// (<c>projectfiles --project</c>)가 같은 함수로 기술자·규칙·SDK 모듈을 해석하고 요청을 만든다 — 둘이 갈라지면 IDE가 빌드와 다른 명령을
/// 본다. <see cref="CompileRulesAsync"/>는 받은 규칙 디렉터리에만 쓰고, <see cref="Layout"/>은 createDirectories가 false면 아무것도 쓰지 않는다.
/// </summary>
internal static class GamePlanning
{
    /// <summary>
    /// 게임 루트의 <c>.rocksproject</c>(정확히 하나)를 읽고 engineVersion이 SDK 엔진의 주·부와 같은지 본다.
    /// </summary>
    public static ProjectDescriptor Descriptor(string project, EngineVersion engine)
    {
        string[] files = Directory.GetFiles(project, "*" + DescriptorReader.ProjectExtension, SearchOption.TopDirectoryOnly);
        if (files.Length != 1)
        {
            throw new ArgumentException($"The game project root must contain exactly one {DescriptorReader.ProjectExtension} file (found {files.Length}).");
        }

        ProjectDescriptor descriptor = DescriptorReader.ReadProject(files[0]);
        // 게임은 SDK manifest의 엔진 버전만 쓴다(저장소 Build/Build.version을 읽지 않는다). 프로젝트 연결은 주·부가 같아야 한다.
        ProjectEngineVersion projectEngine = descriptor.EngineVersion;
        if (!engine.CanLinkProject(projectEngine.Major, projectEngine.Minor))
        {
            throw new DescriptorException(descriptor.FilePath,
                $"engineVersion {projectEngine} does not match the installed SDK engine {engine}; " +
                $"set engineVersion to {engine.Major}.{engine.Minor} to upgrade the project.");
        }

        return descriptor;
    }

    /// <summary>
    /// 게임 규칙 파일을 규칙 어셈블리로 컴파일한다(SDK의 빌드 어셈블리를 참조). 성공하면 어셈블리 경로, 실패하면 null이다(컴파일러 출력은
    /// 화면으로 낸다).
    /// </summary>
    public static async Task<string?> CompileRulesAsync(string rulesDirectory, IReadOnlyList<string> ruleFiles, string sdk, NativeToolchain toolchain,
        CancellationToken cancellationToken)
    {
        if (ruleFiles.Count == 0)
        {
            throw new ArgumentException("No *.Build.cs or *.Target.cs rules found in the game project.");
        }

        string projectFile = Path.Combine(rulesDirectory, "Rocks.Game.Rules.csproj");
        static string Escape(string value) => SecurityElement.Escape(value)!;
        var references = new[] { "Rocks.Build.Abstractions", "Rocks.Build", "Rocks.Tooling.Common" }.Select(name =>
            $"<Reference Include=\"{name}\"><HintPath>{Escape(Path.Combine(sdk, "Tools/Rocks.Build.Cli", name + ".dll"))}</HintPath></Reference>");
        // 사용자 게임 디렉터리의 Directory.Build.props가 호스트 bootstrap을 바꾸지 못하게 입력을 고정한다.
        string projectXml = "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup>" +
            "<TargetFramework>net10.0</TargetFramework><Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings>" +
            "<TreatWarningsAsErrors>true</TreatWarningsAsErrors><EnableDefaultCompileItems>false</EnableDefaultCompileItems>" +
            "<AssemblyName>Rocks.Game.Rules</AssemblyName><Deterministic>true</Deterministic>" +
            "</PropertyGroup><ItemGroup>" + string.Join("", ruleFiles.Select(file => $"<Compile Include=\"{Escape(file)}\" />")) +
            string.Join("", references) + "</ItemGroup></Project>\n";
        AtomicFile.WriteIfChanged(projectFile, projectXml);
        var compiled = await ProcessRunner.RunAsync(new("dotnet", ["build", projectFile, "--nologo", "-warnaserror",
            "-p:ImportDirectoryBuildProps=false", "-p:ImportDirectoryBuildTargets=false"], sdk, toolchain.Environment), cancellationToken);
        Console.Write(compiled.StdOut);
        Console.Error.Write(compiled.StdErr);
        return compiled.ExitCode == 0 ? Path.Combine(rulesDirectory, "bin/Debug/net10.0/Rocks.Game.Rules.dll") : null;
    }

    /// <summary>
    /// 규칙 모듈을 기술자와 대조하고(켜지지 않은 플러그인·Editor·플랫폼 미지원 모듈 제외) SDK 모듈과 합친 뒤 메타데이터를 배치한다.
    /// createDirectories가 false면 생성 include 디렉터리를 만들지 않는다(IDE 계획).
    /// </summary>
    public static GameLayout Layout(LoadedRules rules, ProjectDescriptor descriptor, IReadOnlyList<PluginDescriptor> availablePlugins, BuildContext context,
        NativeToolchain toolchain, NativeConfiguration configuration, string sdk, IReadOnlyList<SdkModule> sdkModules, bool createDirectories)
    {
        // SDK 엔진 라이브러리는 구성 기본 로그 하한으로 미리 빌드됐다. 게임 타깃만 다른 값을 쓰면 Log.h inline 본문이 갈려 ODR 위반이다.
        if (rules.Target.LogCompiledMinimum is not null)
        {
            throw new ArgumentException($"Target {rules.Target.Name}: LogCompiledMinimum cannot be overridden in an installed SDK game build " +
                "(the prebuilt engine libraries use the configuration default).");
        }

        var moduleSet = DescriptorModuleSet.Resolve(DescriptorOwner.ForProject(descriptor, rules.Target.Name, rules.Target.Plugins),
            availablePlugins, toolchain.Platform, configuration, rules.Target.Type);
        var projectModules = moduleSet.Apply(rules.Modules, rules.Target.Modules);
        // SDK 모듈은 다시 생성하지 않는다. 리플렉션 모듈이면 descriptor·명세를 ImportedMetadata로 넘긴다. 타입별 본문·등록 함수 등 생성
        // 소스의 object는 SDK 라이브러리(PrebuiltLibraries)에 들어 있으므로 ReflectionSources는 비어 있다 — 게임 소유 모듈만 자기 생성
        // 소스를 낸다.
        NativeModuleInput[] modules = [.. sdkModules.Select(module => new NativeModuleInput
        {
            Name = module.Name,
            PublicIncludeDirectories = [.. module.Includes.Select(path => InstalledSdk.SafePath(sdk, path))],
            PublicDefines = module.Defines,
            PublicDependencies = module.PublicDependencies,
            PrivateDependencies = module.PrivateDependencies,
            PrebuiltLibraries = [.. module.Libraries.Select(path => InstalledSdk.SafePath(sdk, path))],
            SystemLibraries = module.SystemLibraries,
            Frameworks = module.Frameworks,
            ImportedMetadata = module.Descriptor is null
                ? null
                : new ImportedMetadata(InstalledSdk.SafePath(sdk, module.Descriptor), InstalledSdk.SafePath(sdk, module.IdentityManifest!)),
            ProvidesBuildInfo = module.ProvidesBuildInfo,
            ProvidesModuleTable = module.ProvidesModuleTable,
            ThirdParty = module.ThirdParty,
            SharedPchHeader = module.SharedPchHeader,
            DynamicModules = module.DynamicModules,
            // Verify가 이름을 확인했다.
            LoadingPhase = Enum.Parse<LoadingPhase>(module.LoadingPhase!),
            ProvidesLaunch = module.ProvidesLaunch,
            HasGlobalState = module.HasGlobalState
        }), .. projectModules];
        // 켠 플러그인의 모듈은 타깃이 직접 참조하지 않아도 빌드하고 주 실행 파일에 링크한다(Docs/Design/BuildTargets.ko.md 3.9절 BD36, UE와
        // 같다). 메타데이터 생성도 같은 root 집합이다.
        modules = [.. moduleSet.LinkIntoPrimary(modules, rules.Target.Modules)];
        string[] targets = [.. rules.Target.Modules.Concat(moduleSet.IncludedPluginModules).Distinct(StringComparer.OrdinalIgnoreCase)];
        return new(moduleSet, MetadataLayout.Apply(context, modules, targets, createDirectories), targets);
    }

    /// <summary>게임 타깃의 빌드 요청이다. 빌드 방식(유니티·PCH·작업 파일 분리·시간 측정·계획 모드)은 호출자가 덧붙인다.</summary>
    public static NativeBuildRequest Request(NativeToolchain toolchain, NativeConfiguration configuration, string project, string output,
        IReadOnlyList<NativeModuleInput> modules, IReadOnlyList<string> targets, string sdk, EngineVersion engineVersion, int jobs) => new()
        {
            Toolchain = toolchain,
            Configuration = configuration,
            OutputDirectory = output,
            // 정적 링크이므로 실행 파일에는 참조한 모듈만 들어간다.
            RootDirectory = project,
            Modules = modules,
            Targets = targets,
            ContextFingerprint = "rocks-installed-sdk-v1",
            MaxParallelism = jobs,
            // 도구 빌드 ID의 sdk: 경로 키 기준이다(SDK 헤더·라이브러리가 설치 위치와 무관한 키를 갖는다).
            SdkDirectory = sdk,
            EngineVersion = engineVersion
        };
}
