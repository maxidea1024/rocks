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
// (엔진 Core — 게임 실행 파일이 이 모듈에 닿으면 빌드가 BuildInfo 정의 TU를 더한다). ProvidesModuleTable·ThirdParty도 NativeModuleInput과 같은
// 표지다(Docs/Design/ModuleLoader.ko.md 3.9절 — 게임 실행 파일의 모듈 표가 SDK 모듈의 진입점을 참조하고 서드파티 모듈은 뺀다).
// 손으로 쓴 소스가 없던 메타데이터 모듈(CoreTypeBindings 등)의 라이브러리도 진입점 TU를 담으므로 다른 모듈과 같은 판정을 받는다
// (3.10절). SDK는 생성 소스 자체는 싣지 않는다(생성 헤더와 라이브러리만). DynamicModules는 NativeModuleInput과 같은 실행 중 적재 모듈
// 목록이다(3.8절 — 게임 실행 파일이 SDK 모듈을 링크하면 그 동적 모듈도 링크하고 모듈 표에 싣는다). LoadingPhase(단계 이름 —
// Descriptors.LoadingPhase)·ProvidesLaunch는 NativeModuleInput과 같은 칸이다(3.11절 — 게임 실행 파일의 모듈 표가 SDK 모듈의 단계로 단계
// 순서·부트 모듈 검사를 소스 빌드와 같게 한다). PublicDependencies·PrivateDependencies는 NativeModuleInput과 같은 두 의존 목록이다
// (Docs/Design/BuildTargets.ko.md 3.5절 — 게임 빌드가 같은 공개 환경 규칙으로 컴파일 환경을 만든다). HasGlobalState는 NativeModuleInput과
// 같은 서드파티 표지다(3.8절 BD29 — 게임 모듈이 SDK의 전역 상태 서드파티를 직접 의존하면 소비 모듈이 둘이 되어 거부된다).
public sealed record SdkModule(string Name, string[] Includes, string[] Defines, string[] PublicDependencies, string[] PrivateDependencies,
    string[] Libraries,
    string[] SystemLibraries, string[] Frameworks, string? Descriptor, string? IdentityManifest, bool ProvidesBuildInfo,
    bool ProvidesModuleTable, bool ThirdParty, string? SharedPchHeader, string[] DynamicModules, string? LoadingPhase,
    bool ProvidesLaunch, bool HasGlobalState);

// SDK를 만든 엔진 버전이다(Build/Build.version 값 — 그 파일은 SDK에 복사하지 않는다, Docs/Design/ReflectionFoundations.ko.md 8.2절).
// 게임 빌드는 저장소 파일을 읽지 않고 이 값만 쓴다(BuildInfo 생성 TU·.rocksproject engineVersion 대조). 위치 record는 JSON에서 빠진
// 인자를 null로 채우므로 칸을 int?로 두고 Verify가 null·범위·0.0을 직접 검사한다.
public sealed record SdkEngineVersion(int? Major, int? Minor, int? Patch);

// 설치 SDK의 구성 하나다(Docs/Design/ProjectFiles.ko.md 10.3절). Modules는 그 구성의 모듈 선언(라이브러리·descriptor는 그 구성 디렉터리 아래,
// include는 한 벌 Headers/), Files는 그 구성에만 있는 파일(Configurations/<구성>/ 아래)의 SDK 상대 경로다.
public sealed record SdkConfiguration(SdkModule[] Modules, string[] Files);

// Configurations는 구성 이름 → 구성이다. Files는 manifest를 뺀 SDK 전체 파일의 내용 해시다.
public sealed record SdkManifest(int Version, SdkEngineVersion? EngineVersion, Dictionary<string, SdkConfiguration> Configurations, string Platform,
    string Architecture, string CompilerIdentity, string SdkIdentity, Dictionary<string, string> Files);

public static class InstalledSdk
{
    // 2: SdkModule에 SystemLibraries·Frameworks 추가. 3: Descriptor·IdentityManifest 추가. 4: ProvidesBuildInfo 추가, AbiIdentity를
    // effective triple로 계산(B7 — B7 이전 SDK는 생성 TU의 Core/Build/BuildInfo.h include가 깨지는 대신 버전 불일치로 거부한다).
    // 5: EngineVersion {Major, Minor, Patch} 추가(엔진 버전 단일 출처 — 생성 TU의 CurrentEngineVersion 정의에 필요하다).
    // 6: SdkModule에 ProvidesModuleTable·ThirdParty 추가(게임 실행 파일의 모듈 표 — Docs/Design/ModuleLoader.ko.md 3.2절).
    // 7: SdkModule에 ReflectionOnlyLibrary 추가. 손으로 쓴 소스가 없던 리플렉션 모듈이 모듈 생성 소스 때문에 라이브러리를 갖는다
    // (Docs/Design/ReflectionGeneratedSources.ko.md 6.3절 — 이전 SDK의 모듈 생성 헤더는 등록 함수를 inline으로 정의해 새 도구의 선언과
    // 맞지 않으므로 버전 불일치로 거부한다).
    // 8: ReflectionOnlyLibrary 제거. 메타데이터 모듈이 모두 진입점 모듈이 되고 진입 기술이 자기 타입 등록 함수를 싣는다
    // (Docs/Design/ModuleLoader.ko.md 3.10절 — 이전 SDK의 그런 모듈 라이브러리에는 진입점이 없어 게임 모듈 표가 링크되지 않으므로
    // 버전 불일치로 거부한다).
    // 9: 타입별 생성 헤더가 본문을 정의하지 않고(선언·형질·inline Trace만 — 본문은 SDK 라이브러리에 든 타입별 생성 소스의 object다,
    // Docs/Design/ReflectionGeneratedSources.ko.md 5절) SdkModule에 SharedPchHeader가 더해졌다(공유 PCH 제공 모듈의 헤더 철자 —
    // Docs/Design/UnityBuildPch.ko.md 8.4절. 게임 빌드가 SDK 헤더로 공유 PCH를 자기 출력에 직접 만들고, 공유 정의는 Defines에서 다시
    // 계산한다). 이전 SDK는 생성 헤더가 본문을 inline으로 정의하고 동봉한 MetaCompiler가 타입별 생성 소스·정의 헤더 옵션
    // (--jobs·--definitions-header)을 모르므로 버전 불일치로 거부한다.
    // 10: SdkModule에 DynamicModules 추가(실행 중 적재 모듈 — Docs/Design/ModuleLoader.ko.md 3.8절. 게임 실행 파일이 SDK 모듈을 링크하면
    // 그 동적 모듈도 함께 링크하고 모듈 표에 싣는다).
    // 11: SdkModule에 LoadingPhase·ProvidesLaunch 추가(로딩 단계 — Docs/Design/ModuleLoader.ko.md 3.11절. 이전 SDK의 Core·ModuleLoader·
    // Launch는 단계가 없어 게임 실행 파일의 부트 모듈이 EarliestPossible에 시작하지 않으므로 버전 불일치로 거부한다).
    // 12: SdkModule의 Dependencies를 PublicDependencies·PrivateDependencies로 나눴다(Docs/Design/BuildTargets.ko.md 3.5절 BS0 — 이전 SDK는
    // 두 목록이 없어 게임 빌드가 공개 환경을 계산할 수 없으므로 버전 불일치로 거부한다).
    // 13: SdkModule에 HasGlobalState 추가(Docs/Design/BuildTargets.ko.md 3.8절 BS8c — 이전 SDK는 전역 상태 서드파티를 표시하지 않아 게임
    // 빌드가 단일 소비 검사를 할 수 없으므로 버전 불일치로 거부한다).
    // 14: 다중 구성 SDK. Configuration·Modules를 Configurations(구성 이름 → 모듈 선언·구성 파일 목록)로 바꾸고 구성마다 다른 파일을
    // Configurations/<구성>/ 아래에 둔다(Docs/Design/ProjectFiles.ko.md 10.3절 — 이전 SDK는 구성 목록이 없어 게임 솔루션이 구성을 알 수 없으므로
    // 버전 불일치로 거부한다).
    internal const int ManifestVersion = 14;

    /// <summary>
    /// SDK 타깃에서 도달하는 모듈만 고른다. 규칙 로더는 모든 모듈 규칙을 읽으므로 타깃이 쓰지 않는(빌드되지 않은)
    /// 모듈까지 내보내려 하면 "built for this SDK target" 오류가 난다.
    /// </summary>
    public static IReadOnlyList<NativeModuleInput> Reachable(IReadOnlyList<NativeModuleInput> modules, IEnumerable<string> targets)
    {
        var byName = modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        // 실행 중 적재 모듈도 SDK에 싣는다(게임 실행 파일이 함께 링크한다 — Docs/Design/ModuleLoader.ko.md 3.8절).
        HashSet<string> reached = LinkClosure.Reachable(targets, byName);
        return modules.Where(module => reached.Contains(module.Name)).ToArray();
    }

    /// <summary>
    /// 빌드한 구성 하나의 SDK 배치를 stage 디렉터리에 만든다(헤더·라이브러리·메타데이터·라이선스·도구·스크립트). 구성 사이 합치기와 manifest는
    /// <see cref="SdkAssembly"/>가 한다. 돌려주는 모듈 선언의 경로는 stage 기준이다.
    /// </summary>
    internal static SdkModule[] StageConfiguration(string root, string stage, IReadOnlyList<NativeModuleInput> modules,
        IReadOnlyList<MetadataModule> metadata, NativeBuildResult build)
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
            else if (module.Sources.Count != 0 || module.ReflectionSources.Count != 0)
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
                module.PublicDependencies.ToArray(), module.PrivateDependencies.ToArray(), libraries.ToArray(),
                module.SystemLibraries.ToArray(),
                module.Frameworks.ToArray(), descriptor, identityManifest, module.ProvidesBuildInfo, module.ProvidesModuleTable,
                module.ThirdParty, module.SharedPchHeader, module.DynamicModules.ToArray(), module.LoadingPhase.ToString(),
                module.ProvidesLaunch, module.HasGlobalState));
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
        // Visual Studio 진입 스크립트를 싣는다(Docs/Design/ProjectFiles.ko.md 12절). SDK 안에서는 rocks-sdk.json이 있어 도구를 빌드하지
        // 않는다(Build/Tools.ps1 SDK 모드).
        Directory.CreateDirectory(Path.Combine(stage, "Build"));
        foreach (string script in SdkScripts)
        {
            File.Copy(Path.Combine(root, "Build", script), Path.Combine(stage, "Build", script));
        }

        File.Copy(Path.Combine(root, ".clang-format"), Path.Combine(stage, ".clang-format"));
        // 프로젝트 템플릿을 싣는다(new-project가 SDK의 Templates/Game/<이름>에서 찾는다, Docs/Design/ProjectFiles.ko.md 11.5절).
        CopyTree(Path.Combine(root, "Templates"), Path.Combine(stage, "Templates"));
        return [.. packaged];
    }

    // SDK가 싣는 Visual Studio 진입 스크립트다(Build/ 아래 같은 이름).
    private static readonly string[] SdkScripts = ["IdeBuild.bat", "IdeBuild.ps1", "Tools.ps1", "GenerateProjectFiles.ps1"];

    /// <summary>
    /// <c>--sdk</c>가 없으면 게임 프로젝트의 <c>.rocksproject</c> 연결로 등록 SDK를 골라 넣는다(Docs/Design/ProjectFiles.ko.md 10.1절).
    /// 호환 후보는 manifest 판·플랫폼·아키텍처·컴파일러 ABI가 이 컴퓨터의 도구 모음과 맞는 SDK다(구성은 빌드가 따로 본다).
    /// </summary>
    internal static async Task ResolveSdkOptionAsync(Dictionary<string, string> options, CancellationToken cancellationToken,
        bool anyConfiguration = false)
    {
        if (options.ContainsKey("sdk"))
        {
            return;
        }

        string project = Path.GetFullPath(options["project"]);
        string[] descriptors = Directory.GetFiles(project, "*" + DescriptorReader.ProjectExtension, SearchOption.TopDirectoryOnly);
        if (descriptors.Length != 1)
        {
            throw new ArgumentException($"The game project root must contain exactly one {DescriptorReader.ProjectExtension} file (found {descriptors.Length}).");
        }

        ProjectDescriptor descriptor = DescriptorReader.ReadProject(descriptors[0]);
        NativeToolchain toolchain = await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
        // 프로젝트 파일 생성은 구성 하나를 빌드하지 않으므로 구성 조건 없이 고른다(Docs/Design/ProjectFiles.ko.md 10.4절).
        NativeConfiguration? configuration = anyConfiguration ? null : BuildCommand.Configuration(options);
        string sdk = SdkAssociation.Resolve(descriptor, SdkRegistries.Default(), path => Incompatibility(path, toolchain, configuration));
        Console.WriteLine($"Installed SDK: {sdk} ({(descriptor.EngineAssociation is null ? "engineVersion " + descriptor.EngineVersion : "engineAssociation")}).");
        options["sdk"] = sdk;
    }

    /// <summary>
    /// Visual Studio 빌드(<c>ROCKS_IDE_BUILD=1</c>)에서, 프로젝트에 <c>engineAssociation</c>이 있으면 그것이 해석하는 SDK가 프로젝트 파일의
    /// <c>--sdk</c>와 같은지 본다(Docs/Design/ProjectFiles.ko.md 10.4절). 다르면 연결을 바꾼 뒤 다시 만들지 못한 낡은 프로젝트 파일이다 — 옛 SDK로
    /// 조용히 빌드하지 않고 다시 만들라고 알린다. 연결이 없으면(등록부 선택) 보지 않는다.
    /// </summary>
    private static void RequireCurrentProjectFiles(Dictionary<string, string> options)
    {
        if (Environment.GetEnvironmentVariable("ROCKS_IDE_BUILD") != "1")
        {
            return;
        }

        string project = Path.GetFullPath(options["project"]);
        string[] descriptors = Directory.GetFiles(project, "*" + DescriptorReader.ProjectExtension, SearchOption.TopDirectoryOnly);
        if (descriptors.Length != 1 || DescriptorReader.ReadProject(descriptors[0]) is not { EngineAssociation: not null } descriptor)
        {
            return;
        }

        string associated;
        try
        {
            associated = SdkAssociation.Resolve(descriptor, SdkRegistries.Default(), _ => null);
        }
        catch (ArgumentException error)
        {
            throw new ArgumentException($"The Visual Studio project files are stale: {error.Message} Next: run Build/GenerateProjectFiles.ps1 -Project " +
                $"\"{project}\".", error);
        }

        if (!SdkRegistries.SamePath(associated, options["sdk"]))
        {
            throw new ArgumentException($"The Visual Studio project files are stale: {descriptor.FilePath} associates {associated}, but the project " +
                $"files build with {options["sdk"]}. Next: run Build/GenerateProjectFiles.ps1 -Project \"{project}\".");
        }
    }

    /// <summary>
    /// 이 SDK를 이 컴퓨터의 도구 모음으로 요청 구성을 빌드하는 데 쓸 수 없는 이유다(쓸 수 있으면 null). Verify의 호환 조건(manifest 판·구성·
    /// 플랫폼·아키텍처·ABI)이고, 파일 목록·내용 검사는 고른 SDK에만 Verify가 한다.
    /// </summary>
    internal static string? Incompatibility(string sdk, NativeToolchain toolchain, NativeConfiguration? configuration)
    {
        string manifestText = File.ReadAllText(Path.Combine(sdk, "rocks-sdk.json"));
        int? version = ManifestVersionOf(manifestText);
        if (version != ManifestVersion)
        {
            return $"SDK manifest version {(version?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "missing")}, these tools need {ManifestVersion}";
        }

        SdkManifest? manifest;
        try
        {
            manifest = JsonSerializer.Deserialize<SdkManifest>(manifestText);
        }
        catch (JsonException error)
        {
            return "unreadable manifest: " + error.Message;
        }

        if (manifest is null)
        {
            return "empty manifest";
        }

        string architecture = System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString();
        return manifest.Configurations is null || manifest.Configurations.Count == 0 ||
            (configuration is not null && !manifest.Configurations.ContainsKey(configuration.Value.ToString()))
            ? (configuration is null ? "no configurations" : $"no {configuration} configuration (has {Names(manifest)})")
            : manifest.Platform != toolchain.Platform.ToString() ? $"platform {manifest.Platform}, this computer builds {toolchain.Platform}"
            : manifest.Architecture != architecture ? $"architecture {manifest.Architecture}, this computer is {architecture}"
            : manifest.CompilerIdentity != toolchain.AbiIdentity || manifest.SdkIdentity != toolchain.AbiIdentity
                ? "exported with another compiler or native SDK ABI than this computer's toolchain"
                : null;
    }

    public static async Task<int> BuildGameAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
    {
        // 값이 틀린 빌드 방식 옵션은 출력을 만들기 전에 거부한다.
        foreach (string mode in BuildCommand.BuildModeOptions.Where(mode => mode != BuildCommand.TimingOption))
        {
            BuildCommand.Switch(options, mode);
        }

        BuildCommand.Timing(options);
        bool requireVerified = ToolchainCheck.RequireVerified(options.GetValueOrDefault(ToolchainCheck.RequireVerifiedOption));

        // --sdk가 없으면 .rocksproject의 SDK 연결로 등록 SDK를 고른다(Docs/Design/ProjectFiles.ko.md 10.1절).
        await ResolveSdkOptionAsync(options, cancellationToken);
        RequireCurrentProjectFiles(options);
        var sdk = PathSpelling.OnDisk(Path.GetFullPath(options["sdk"]));
        var project = PathSpelling.OnDisk(Path.GetFullPath(options["project"]));
        // 기본 출력은 --output이 없을 때만 계산한다(기본 타깃은 .rocksproject를 읽는다).
        var output = PathSpelling.OnDisk(Path.GetFullPath(options.TryGetValue("output", out string? given) ? given
            : GameProjectLayout.DefaultOutput(project, options.GetValueOrDefault("target") ?? GameProjectLayout.DefaultTarget(project), BuildCommand.Configuration(options))));
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
        var toolchainRecord = new GameToolchainRecord();
        try
        {
            var result = await BuildGameCoreAsync(options, requireVerified, toolchainRecord, cancellationToken);
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
                toolchainVerified = toolchainRecord.Verified,
                stlClangCheck = toolchainRecord.StlClangCheck,
                error = failure
            }, BuildCommand.JsonOptions));
        }
    }

    // 게임 evidence에 남길 도구 모음 확인 결과다(build-evidence와 같은 toolchainVerified·stlClangCheck — Docs/Design/ClangVersionSupport.ko.md
    // 6.2절). discovery 전에 실패하면 비어 있다.
    private sealed class GameToolchainRecord
    {
        public bool? Verified { get; set; }

        public string? StlClangCheck { get; set; }
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

    private static async Task<int> BuildGameCoreAsync(Dictionary<string, string> options, bool requireVerified, GameToolchainRecord toolchainRecord,
        CancellationToken cancellationToken)
    {
        // 받은 경로는 디스크에 있는 철자로 맞춘다(Docs/Design/UnityBuildPch.ko.md 4.4절).
        var sdk = PathSpelling.OnDisk(Path.GetFullPath(options["sdk"]));
        var project = PathSpelling.OnDisk(Path.GetFullPath(options["project"]));
        var configuration = BuildCommand.Configuration(options);
        var output = PathSpelling.OnDisk(Path.GetFullPath(options.TryGetValue("output", out string? given) ? given
            : GameProjectLayout.DefaultOutput(project, options.GetValueOrDefault("target") ?? GameProjectLayout.DefaultTarget(project), configuration)));
        if (Inside(sdk, output) || Inside(sdk, project) || Inside(output, project) || Inside(output, sdk))
        {
            throw new ArgumentException("Game output cannot overlap or contain the project or installed SDK.");
        }

        var toolchain = await NativeToolchains.DiscoverAsync(options.GetValueOrDefault("compiler"), cancellationToken);
        if (NativeToolchains.EnvironmentNotice(toolchain) is string notice)
        {
            Console.WriteLine(notice);
        }

        // 확인 결과를 evidence 기록에 먼저 남긴 뒤 검증 요구를 적용한다(실패도 game-build-evidence.json에 FAIL로 남는다).
        toolchainRecord.Verified = toolchain.ToolchainVerified;
        toolchainRecord.StlClangCheck = toolchain.StlClangCheck;
        if (ToolchainCheck.Enforce(toolchain, requireVerified) is string rejected)
        {
            throw new ArgumentException(rejected);
        }

        if (!requireVerified && ToolchainCheck.Notice(toolchain) is string unverified)
        {
            Console.WriteLine(unverified);
        }

        // SDK 내용은 빌드마다 한 번만 해시한다. 끝 검사는 그때 기록한 (크기, 수정 시각)과 파일 목록을 다시 본다(Docs/Design/ProjectFiles.ko.md 10.3절).
        var verified = VerifyStamped(sdk, configuration, toolchain);
        var manifest = verified.Manifest;
        SdkModule[] sdkModules = manifest.Configurations[configuration.ToString()].Modules;
        Directory.CreateDirectory(output);
        Directory.CreateDirectory(Path.Combine(output, "Generated"));
        var initialProject = GameInputSnapshot.Capture(project, output, sdk, []);
        // 프로젝트 기술자(UE .uproject 대응)가 빌드할 모듈·플러그인·플랫폼을 정한다. 입력 snapshot 뒤에 읽어
        // 빌드 중 기술자 변경을 최종 검사가 잡게 하고, 규칙을 컴파일하기 전에 기술자 오류를 먼저 알린다. 기술자·규칙·SDK 모듈 해석은 게임
        // 프로젝트 파일 생성과 같은 함수다(GamePlanning, Docs/Design/ProjectFiles.ko.md 10.4절).
        var engineVersion = EngineVersionOf(manifest);
        var projectDescriptor = GamePlanning.Descriptor(project, engineVersion);
        // 플러그인 기술자 검증은 규칙을 읽기 전에 한다. 켤 플러그인과 모듈 해석은 타깃 종류·타깃 플러그인 참조가 필요하므로 규칙 로드 뒤다
        // (Docs/Design/BuildTargets.ko.md BD24, 3.9절).
        var availablePlugins = PluginCatalog.Discover([Path.Combine(project, "Plugins")]);
        // 입력 snapshot과 같은 제외 규칙으로 찾는다(점 디렉터리·out/·Saved/ 아래 사본을 규칙으로 컴파일하지 않는다, 10.6절).
        string? rulesPath = await GamePlanning.CompileRulesAsync(Path.Combine(output, "Rules"), GameProjectLayout.RuleFiles(project, output, sdk), sdk,
            toolchain, cancellationToken);
        if (rulesPath is null)
        {
            return 1;
        }

        var context = new BuildContext(project, output, toolchain.Platform, toolchain.Architecture, configuration, sdk);
        var rules = RuleLoader.Load(rulesPath, (options.GetValueOrDefault("target") ?? GameProjectLayout.DefaultTarget(project)), context);
        GameLayout layout = GamePlanning.Layout(rules, projectDescriptor, availablePlugins, context, toolchain, configuration, sdk, sdkModules,
            createDirectories: true);
        string[] targets = layout.Targets;
        var plan = layout.Metadata;
        var declaredInputs = GameInputSnapshot.DeclaredInputs(context, plan);
        var initialDeclared = GameInputSnapshot.Capture(project, output, sdk, declaredInputs);
        var metadata = await MetadataRunner.GenerateAsync(context, toolchain, plan, rules.Target,
            Path.Combine(sdk, "Tools/Rocks.MetaCompiler/Rocks.MetaCompiler.dll"), cancellationToken, targets,
            jobs: BuildCommand.Jobs(options), verifySources: BuildCommand.VerifyMetadataSources(options));
        if (metadata != 0)
        {
            return 1;
        }

        var result = await new NativeBuildEngine().BuildAsync(GamePlanning.Request(toolchain, configuration, project, output, plan.Modules, targets, sdk,
            engineVersion, BuildCommand.Jobs(options)) with
        {
            Unity = BuildCommand.Switch(options, BuildCommand.UnityOption),
            Pch = BuildCommand.Switch(options, BuildCommand.PchOption),
            Adaptive = BuildCommand.Switch(options, BuildCommand.AdaptiveOption),
            Timing = BuildCommand.Timing(options),
            TimingLabel = rules.Target.Name
        }, cancellationToken);
        BuildCommand.Print(result);
        // 최종 검사 전 성공 보고서를 게시하면 SDK/입력 변경에 따른 실패가 PASS로 보일 수 있다.
        PublishVerifiedGameResult(sdk, configuration, toolchain, project, output, initialProject,
            initialDeclared, declaredInputs, result, verified);
        return result.Succeeded ? 0 : 1;
    }

    public static void PublishVerifiedGameResult(string sdk, NativeConfiguration configuration, NativeToolchain toolchain,
        string project, string output, string initialProject, string initialDeclared,
        IReadOnlyList<string> declaredInputs, NativeBuildResult result, VerifiedSdk? verified = null)
    {
        // 빌드 시작에 검증한 SDK면 그 뒤 바뀌지 않았는지만 본다(파일 목록 + 크기·수정 시각). 없으면 전체를 검증한다.
        if (verified is null)
        {
            Verify(sdk, configuration, toolchain);
        }
        else
        {
            RequireUnchanged(sdk, verified);
        }

        if (GameInputSnapshot.Capture(project, output, sdk, []) != initialProject ||
            GameInputSnapshot.Capture(project, output, sdk, declaredInputs) != initialDeclared)
        {
            throw new InvalidOperationException("Game inputs changed during rule evaluation, metadata generation, or native build. Retry the build.");
        }

        AtomicFile.WriteIfChanged(Path.Combine(output, "action-report.json"), JsonSerializer.Serialize(result, BuildCommand.JsonOptions));
    }

    // manifest 최상위 Version 칸의 값이다. 객체가 아니거나 칸이 없거나 정수가 아니면 null이다(버전 불일치로 알린다).
    private static int? ManifestVersionOf(string manifestText)
    {
        using JsonDocument document = JsonDocument.Parse(manifestText);
        return document.RootElement.ValueKind == JsonValueKind.Object &&
            document.RootElement.TryGetProperty(nameof(SdkManifest.Version), out JsonElement version) &&
            version.ValueKind == JsonValueKind.Number && version.TryGetInt32(out int value)
            ? value
            : null;
    }

    /// <summary>검증한 SDK와 그때 본 파일의 (크기, 수정 시각)이다. 빌드 끝 검사가 다시 해시하지 않고 변경을 잡는 데 쓴다.</summary>
    public sealed record VerifiedSdk(SdkManifest Manifest, IReadOnlyDictionary<string, (long Length, DateTime Written)> Stamp);

    public static SdkManifest Verify(string sdk, NativeConfiguration configuration, NativeToolchain toolchain) =>
        VerifyStamped(sdk, configuration, toolchain).Manifest;

    // 빌드 시작 뒤 SDK가 바뀌었으면 거부한다. 파일 목록은 전체를, 크기·수정 시각은 시작 때 해시한 파일(한 벌 + 선택 구성)을 본다.
    private static void RequireUnchanged(string sdk, VerifiedSdk verified)
    {
        if (!Inventory(sdk).SetEquals(verified.Manifest.Files.Keys))
        {
            throw new InvalidDataException("SDK file inventory changed. Re-export instead of adding/removing installed headers or tools.");
        }

        foreach ((string relative, (long length, DateTime written)) in verified.Stamp)
        {
            var file = new FileInfo(SafePath(sdk, relative));
            if (!file.Exists || file.Length != length || file.LastWriteTimeUtc != written)
            {
                throw new InvalidDataException($"SDK file changed or missing: {relative}");
            }
        }
    }

    // hashContents가 false면 구조(판·구성·파일 목록·모듈 선언)만 보고 내용 해시를 건너뛴다(게임 프로젝트 파일 생성이 둘째 구성부터 쓴다).
    public static VerifiedSdk VerifyStamped(string sdk, NativeConfiguration configuration, NativeToolchain toolchain, bool hashContents = true)
    {
        string manifestText = File.ReadAllText(Path.Combine(sdk, "rocks-sdk.json"));
        // 버전을 먼저 본다. 다른 버전의 manifest는 칸이 달라(예: 7의 ReflectionOnlyLibrary) 엄격한 역직렬화가 버전 불일치 안내
        // 대신 알 수 없는 칸 오류를 내므로, 버전이 같을 때만 엄격하게 읽는다.
        int? version = ManifestVersionOf(manifestText);
        if (version != ManifestVersion)
        {
            throw new InvalidDataException($"SDK manifest version mismatch: {(version is int value ? value.ToString(System.Globalization.CultureInfo.InvariantCulture) : "missing")} " +
                $"(this build tool requires {ManifestVersion}). Re-export the SDK with the current tools.");
        }

        var manifest = JsonSerializer.Deserialize<SdkManifest>(manifestText, new JsonSerializerOptions { UnmappedMemberHandling = System.Text.Json.Serialization.JsonUnmappedMemberHandling.Disallow })
            ?? throw new InvalidDataException("Empty SDK manifest.");

        // 엔진 버전 칸을 직접 검사한다(빠진 칸이 null·0으로 채워져 조용히 통과하지 않게).
        _ = EngineVersionOf(manifest);

        if (manifest.Platform != toolchain.Platform.ToString() || manifest.CompilerIdentity != toolchain.AbiIdentity ||
            manifest.SdkIdentity != toolchain.AbiIdentity ||
            manifest.Architecture != System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString())
        {
            throw new InvalidDataException("SDK compiler, platform, architecture, or native SDK ABI mismatch.");
        }

        if (manifest.Files is null || manifest.Configurations is null || manifest.Files.Count == 0 || manifest.Configurations.Count == 0)
        {
            throw new InvalidDataException("SDK manifest is missing its file inventory or configurations.");
        }

        if (!manifest.Configurations.TryGetValue(configuration.ToString(), out SdkConfiguration? selected))
        {
            throw new InvalidDataException($"The SDK has no {configuration} configuration (it has {Names(manifest)}). Export it with " +
                $"sdk --configurations including {configuration}.");
        }

        // 구성마다 파일 목록은 Configurations/<구성>/ 아래이고 그 아래 실제 파일과 같다(모르는 구성 디렉터리도 거부한다).
        var claimed = new HashSet<string>(StringComparer.Ordinal);
        foreach ((string name, SdkConfiguration entry) in manifest.Configurations)
        {
            if (!Enum.GetNames<NativeConfiguration>().Contains(name, StringComparer.Ordinal) || entry?.Modules is null || entry.Files is null ||
                entry.Modules.Length == 0)
            {
                throw new InvalidDataException($"SDK configuration '{name}' is unknown or has no module declarations.");
            }

            string prefix = $"{SdkAssembly.ConfigurationsDirectory}/{name}/";
            if (entry.Files.Any(file => !file.StartsWith(prefix, StringComparison.Ordinal) || !claimed.Add(file)) ||
                !manifest.Files.Keys.Where(file => file.StartsWith(prefix, StringComparison.Ordinal)).ToHashSet(StringComparer.Ordinal).SetEquals(entry.Files))
            {
                throw new InvalidDataException($"SDK configuration '{name}' file list does not match {prefix}.");
            }
        }

        if (manifest.Files.Keys.Any(file => file.StartsWith(SdkAssembly.ConfigurationsDirectory + "/", StringComparison.Ordinal) && !claimed.Contains(file)))
        {
            throw new InvalidDataException($"SDK has files under {SdkAssembly.ConfigurationsDirectory}/ that belong to no configuration.");
        }

        var configurationFiles = selected.Files.ToHashSet(StringComparer.Ordinal);
        SdkModule[] modules = selected.Modules;

        if (!Inventory(sdk).SetEquals(manifest.Files.Keys))
        {
            throw new InvalidDataException("SDK file inventory changed. Re-export instead of adding/removing installed headers or tools.");
        }

        if (modules.Select(module => module.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != modules.Length)
        {
            throw new InvalidDataException("SDK contains duplicate module names.");
        }

        // 내용 해시는 한 벌 파일과 요청 구성의 파일만 본다(다른 구성의 라이브러리는 이 빌드가 읽지 않는다). 파일 목록은 위에서 전체를 봤다.
        // 크기·수정 시각을 해시 전에 기록해, 해시 뒤의 변경도 빌드 끝 검사가 잡는다.
        var stamp = new Dictionary<string, (long Length, DateTime Written)>(StringComparer.Ordinal);
        foreach (var pair in manifest.Files.Where(pair => !pair.Key.StartsWith(SdkAssembly.ConfigurationsDirectory + "/", StringComparison.Ordinal) ||
            configurationFiles.Contains(pair.Key)))
        {
            var file = new FileInfo(SafePath(sdk, pair.Key));
            if (!file.Exists)
            {
                throw new InvalidDataException($"SDK file changed or missing: {pair.Key}");
            }

            stamp[pair.Key] = (file.Length, file.LastWriteTimeUtc);
            if (hashContents && Hashing.Sha256File(file.FullName) != pair.Value)
            {
                throw new InvalidDataException($"SDK file changed or missing: {pair.Key}");
            }
        }

        foreach (var module in modules)
        {
            if (module.Includes is null || module.Libraries is null || module.PublicDependencies is null ||
                module.PrivateDependencies is null || module.Defines is null ||
                module.SystemLibraries is null || module.Frameworks is null || module.DynamicModules is null || module.LoadingPhase is null)
            {
                throw new InvalidDataException($"SDK module '{module.Name}' has incomplete declarations.");
            }

            // 이름만 받는다(Enum.Parse는 숫자 철자 "3"도 받으므로 IsDefined로 이름을 확인한다).
            if (!Enum.IsDefined(typeof(Descriptors.LoadingPhase), module.LoadingPhase))
            {
                throw new InvalidDataException($"SDK module '{module.Name}' has an unknown loading phase '{module.LoadingPhase}'.");
            }

            foreach (var include in module.Includes)
            {
                if (!Directory.Exists(SafePath(sdk, include)))
                {
                    throw new InvalidDataException($"Missing SDK include directory: {include}");
                }
            }

            foreach (var dependency in module.PublicDependencies.Concat(module.PrivateDependencies).Concat(module.DynamicModules))
            {
                if (!modules.Any(candidate => candidate.Name == dependency))
                {
                    throw new InvalidDataException($"SDK module '{module.Name}' has unknown dependency '{dependency}'.");
                }
            }

            foreach (var library in module.Libraries)
            {
                if (!configurationFiles.Contains(library))
                {
                    throw new InvalidDataException("Unverified SDK library.");
                }
            }

            // 리플렉션 모듈은 descriptor와 명세를 함께 담는다(둘 다 검증된 SDK 파일).
            if ((module.Descriptor is null) != (module.IdentityManifest is null) ||
                (module.Descriptor is not null && (!configurationFiles.Contains(module.Descriptor) || !configurationFiles.Contains(module.IdentityManifest!))))
            {
                throw new InvalidDataException($"SDK module '{module.Name}' has incomplete or unverified reflection metadata.");
            }
        }

        return new VerifiedSdk(manifest, stamp);
    }

    // manifest를 뺀 SDK 파일의 상대 경로 전체다. 링크(symlink·junction)는 거부한다. OneDrive 자리 표시자·중복 제거 파일처럼 링크가 아닌 reparse
    // point는 보통 파일이다(Docs/Design/ProjectFiles.ko.md 10.6절).
    private static HashSet<string> Inventory(string sdk)
    {
        var actual = new HashSet<string>(StringComparer.Ordinal);
        void Visit(string directory)
        {
            foreach (var entry in Directory.EnumerateFileSystemEntries(directory))
            {
                if (FileLinks.IsLink(entry))
                {
                    throw new InvalidDataException("SDK inventory must not contain linked files/directories.");
                }

                if (Directory.Exists(entry))
                {
                    Visit(entry);
                }
                else if (Path.GetRelativePath(sdk, entry) != "rocks-sdk.json")
                {
                    actual.Add(Path.GetRelativePath(sdk, entry).Replace('\\', '/'));
                }
            }
        }

        Visit(sdk);
        return actual;
    }

    /// <summary>
    /// SDK manifest가 가진 구성 이름이다(검증 없이 읽는다 — 게임 프로젝트 파일 생성이 구성을 고른 뒤 <see cref="Verify"/>한다). manifest 판이
    /// 다르거나 읽을 수 없으면 Verify와 같은 판 불일치 오류다.
    /// </summary>
    internal static HashSet<string> ManifestConfigurations(string sdk)
    {
        string manifestText = File.ReadAllText(Path.Combine(sdk, "rocks-sdk.json"));
        int? version = ManifestVersionOf(manifestText);
        if (version != ManifestVersion)
        {
            throw new InvalidDataException($"SDK manifest version mismatch: {(version is int value ? value.ToString(System.Globalization.CultureInfo.InvariantCulture) : "missing")} " +
                $"(this build tool requires {ManifestVersion}). Re-export the SDK with the current tools.");
        }

        using JsonDocument document = JsonDocument.Parse(manifestText);
        return document.RootElement.TryGetProperty(nameof(SdkManifest.Configurations), out JsonElement configurations) &&
            configurations.ValueKind == JsonValueKind.Object
            ? configurations.EnumerateObject().Select(entry => entry.Name).ToHashSet(StringComparer.Ordinal)
            : [];
    }

    /// <summary>SDK manifest의 모든 구성에 든 모듈 이름이다(new-project 이름 검사, 검증 없이 읽는다).</summary>
    internal static HashSet<string> ManifestModuleNames(string sdk)
    {
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(Path.Combine(sdk, "rocks-sdk.json")));
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (document.RootElement.TryGetProperty(nameof(SdkManifest.Configurations), out JsonElement configurations) &&
            configurations.ValueKind == JsonValueKind.Object)
        {
            foreach (JsonProperty configuration in configurations.EnumerateObject())
            {
                if (configuration.Value.TryGetProperty(nameof(SdkConfiguration.Modules), out JsonElement modules) && modules.ValueKind == JsonValueKind.Array)
                {
                    names.UnionWith(modules.EnumerateArray().Where(module => module.TryGetProperty(nameof(SdkModule.Name), out _))
                        .Select(module => module.GetProperty(nameof(SdkModule.Name)).GetString() ?? ""));
                }
            }
        }

        return names;
    }

    // manifest가 가진 구성 이름 목록이다(오류 안내용).
    private static string Names(SdkManifest manifest) =>
        manifest.Configurations is null || manifest.Configurations.Count == 0 ? "none" : string.Join(", ", manifest.Configurations.Keys);

    /// <summary>
    /// manifest의 엔진 버전이다. EngineVersion이나 그 칸이 없거나, 0–65535 밖이거나, 주·부가 모두 0이면 InvalidDataException이다 —
    /// 다른 값(저장소 Build/Build.version 등)으로 대신하지 않고 SDK를 다시 내보내게 한다.
    /// </summary>
    public static EngineVersion EngineVersionOf(SdkManifest manifest)
    {
        static bool InRange(int value) => value is >= 0 and <= ushort.MaxValue;

        if (manifest.EngineVersion is not { Major: int major, Minor: int minor, Patch: int patch } ||
            !InRange(major) || !InRange(minor) || !InRange(patch) || (major == 0 && minor == 0))
        {
            throw new InvalidDataException("SDK manifest has no valid EngineVersion { Major, Minor, Patch } " +
                $"(each 0-{ushort.MaxValue}, not 0.0). Re-export the SDK with the current tools.");
        }

        return new EngineVersion(major, minor, patch);
    }

    internal static string SafePath(string root, string relative)
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
            if (FileLinks.IsLink(entry))
            {
                throw new IOException("SDK packaging does not accept symbolic links or junctions.");
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
