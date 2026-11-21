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

public sealed record SdkManifest(int Version, SdkEngineVersion? EngineVersion, string Configuration, string Platform, string Architecture,
    string CompilerIdentity, string SdkIdentity, SdkModule[] Modules, Dictionary<string, string> Files);

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
    internal const int ManifestVersion = 13;

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
    /// 빌드한 모듈을 새 SDK 디렉터리로 내보낸다. engineVersion은 CLI가 Build/Build.version에서 한 번 읽은 값이며 manifest의
    /// EngineVersion으로 기록한다(파일은 복사하지 않는다).
    /// </summary>
    public static async Task ExportAsync(string root, string destination, NativeConfiguration configuration,
        NativeToolchain toolchain, EngineVersion engineVersion, IReadOnlyList<NativeModuleInput> modules,
        IReadOnlyList<MetadataModule> metadata, NativeBuildResult build, CancellationToken cancellationToken)
    {
        if (engineVersion.IsEmpty)
        {
            throw new ArgumentException("SDK export requires the engine version (0.0.x is the empty value).", nameof(engineVersion));
        }

        // 끝 구분자를 떼야 stage가 destination 옆(같은 부모 아래)에 생긴다(C:\sdk\v1\이면 stage가 destination 안에 생겨 Move가
        // 실패했다). TrimEndingDirectorySeparator는 루트를 그대로 두므로 루트는 따로 거부한다.
        destination = PathSpelling.OnDisk(Path.TrimEndingDirectorySeparator(Path.GetFullPath(destination)));
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
            var hashes = Directory.GetFiles(stage, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal)
                .ToDictionary(file => Path.GetRelativePath(stage, file).Replace('\\', '/'), Hashing.Sha256File, StringComparer.Ordinal);
            var manifest = new SdkManifest(ManifestVersion,
                new SdkEngineVersion(engineVersion.Major, engineVersion.Minor, engineVersion.Patch),
                configuration.ToString(), toolchain.Platform.ToString(),
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

    /// <summary>게임 타깃 기본 이름이다(<c>--target</c>가 없을 때).</summary>
    internal const string DefaultGameTarget = "Game";

    // SDK가 싣는 Visual Studio 진입 스크립트다(Build/ 아래 같은 이름).
    private static readonly string[] SdkScripts = ["IdeBuild.bat", "IdeBuild.ps1", "Tools.ps1", "GenerateProjectFiles.ps1"];

    /// <summary>
    /// <c>--sdk</c>가 없으면 게임 프로젝트의 <c>.rocksproject</c> 연결로 등록 SDK를 골라 넣는다(Docs/Design/ProjectFiles.ko.md 10.1절).
    /// 호환 후보는 manifest 판·플랫폼·아키텍처·컴파일러 ABI가 이 컴퓨터의 도구 모음과 맞는 SDK다(구성은 빌드가 따로 본다).
    /// </summary>
    internal static async Task ResolveSdkOptionAsync(Dictionary<string, string> options, CancellationToken cancellationToken)
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
        NativeConfiguration configuration = BuildCommand.Configuration(options);
        string sdk = SdkAssociation.Resolve(descriptor, SdkRegistries.Default(), path => Incompatibility(path, toolchain, configuration));
        Console.WriteLine($"Installed SDK: {sdk} ({(descriptor.EngineAssociation is null ? "engineVersion " + descriptor.EngineVersion : "engineAssociation")}).");
        options["sdk"] = sdk;
    }

    /// <summary>
    /// 이 SDK를 이 컴퓨터의 도구 모음으로 요청 구성을 빌드하는 데 쓸 수 없는 이유다(쓸 수 있으면 null). Verify의 호환 조건(manifest 판·구성·
    /// 플랫폼·아키텍처·ABI)이고, 파일 목록·내용 검사는 고른 SDK에만 Verify가 한다.
    /// </summary>
    internal static string? Incompatibility(string sdk, NativeToolchain toolchain, NativeConfiguration configuration)
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
        return manifest.Configuration != configuration.ToString() ? $"configuration {manifest.Configuration}, the build needs {configuration}"
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
        var sdk = PathSpelling.OnDisk(Path.GetFullPath(options["sdk"]));
        var project = PathSpelling.OnDisk(Path.GetFullPath(options["project"]));
        var output = PathSpelling.OnDisk(Path.GetFullPath(options.GetValueOrDefault("output",
            GameProjectLayout.DefaultOutput(project, options.GetValueOrDefault("target", DefaultGameTarget), BuildCommand.Configuration(options)))));
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
        var output = PathSpelling.OnDisk(Path.GetFullPath(options.GetValueOrDefault("output",
            GameProjectLayout.DefaultOutput(project, options.GetValueOrDefault("target", DefaultGameTarget), configuration))));
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
        // 게임은 SDK manifest의 엔진 버전만 쓴다(저장소 Build/Build.version을 읽지 않는다). 프로젝트 연결은 주·부가 같아야 한다.
        var engineVersion = EngineVersionOf(manifest);
        ProjectEngineVersion projectEngine = projectDescriptor.EngineVersion;
        if (!engineVersion.CanLinkProject(projectEngine.Major, projectEngine.Minor))
        {
            throw new DescriptorException(projectDescriptor.FilePath,
                $"engineVersion {projectEngine} does not match the installed SDK engine {engineVersion}; " +
                $"set engineVersion to {engineVersion.Major}.{engineVersion.Minor} to upgrade the project.");
        }

        // 플러그인 기술자 검증은 규칙을 읽기 전에 한다. 켤 플러그인과 모듈 해석은 타깃 종류·타깃 플러그인 참조가 필요하므로 규칙 로드 뒤다
        // (Docs/Design/BuildTargets.ko.md BD24, 3.9절).
        var availablePlugins = PluginCatalog.Discover([Path.Combine(project, "Plugins")]);
        // 입력 snapshot과 같은 제외 규칙으로 찾는다(점 디렉터리·out/·Saved/ 아래 사본을 규칙으로 컴파일하지 않는다, 10.6절).
        var rulesFiles = GameProjectLayout.RuleFiles(project, output, sdk);
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
        var rules = RuleLoader.Load(rulesPath, options.GetValueOrDefault("target", DefaultGameTarget), context);
        // SDK 엔진 라이브러리는 구성 기본 로그 하한으로 미리 빌드됐다. 게임 타깃만 다른 값을 쓰면 Log.h inline 본문이 갈려 ODR 위반이다.
        if (rules.Target.LogCompiledMinimum is not null)
        {
            throw new ArgumentException($"Target {rules.Target.Name}: LogCompiledMinimum cannot be overridden in an installed SDK game build " +
                "(the prebuilt engine libraries use the configuration default).");
        }
        // 규칙 모듈을 기술자와 대조하고, 켜지지 않은 플러그인·Editor·플랫폼 미지원 모듈 등을 뺀다.
        var moduleSet = DescriptorModuleSet.Resolve(DescriptorOwner.ForProject(projectDescriptor, rules.Target.Name, rules.Target.Plugins),
            availablePlugins, toolchain.Platform, configuration, rules.Target.Type);
        var projectModules = moduleSet.Apply(rules.Modules, rules.Target.Modules);
        // SDK 모듈은 다시 생성하지 않는다. 리플렉션 모듈이면 descriptor·명세를 ImportedMetadata로 넘긴다. 타입별 본문·등록 함수 등 생성
        // 소스의 object는 SDK 라이브러리(PrebuiltLibraries)에 들어 있으므로 ReflectionSources는 비어 있다 — 게임 소유 모듈만 자기 생성
        // 소스를 낸다.
        var modules = manifest.Modules.Select(module => new NativeModuleInput
        {
            Name = module.Name,
            PublicIncludeDirectories = module.Includes.Select(path => SafePath(sdk, path)).ToArray(),
            PublicDefines = module.Defines,
            PublicDependencies = module.PublicDependencies,
            PrivateDependencies = module.PrivateDependencies,
            PrebuiltLibraries = module.Libraries.Select(path => SafePath(sdk, path)).ToArray(),
            SystemLibraries = module.SystemLibraries,
            Frameworks = module.Frameworks,
            ImportedMetadata = module.Descriptor is null
                ? null
                : new ImportedMetadata(SafePath(sdk, module.Descriptor), SafePath(sdk, module.IdentityManifest!)),
            ProvidesBuildInfo = module.ProvidesBuildInfo,
            ProvidesModuleTable = module.ProvidesModuleTable,
            ThirdParty = module.ThirdParty,
            SharedPchHeader = module.SharedPchHeader,
            DynamicModules = module.DynamicModules,
            // Verify가 이름을 확인했다.
            LoadingPhase = Enum.Parse<Descriptors.LoadingPhase>(module.LoadingPhase!),
            ProvidesLaunch = module.ProvidesLaunch,
            HasGlobalState = module.HasGlobalState
        }).Concat(projectModules).ToArray();
        // 켠 플러그인의 모듈은 타깃이 직접 참조하지 않아도 빌드하고 주 실행 파일에 링크한다(Docs/Design/BuildTargets.ko.md 3.9절 BD36, UE와
        // 같다). 메타데이터 생성도 같은 root 집합이다.
        modules = [.. moduleSet.LinkIntoPrimary(modules, rules.Target.Modules)];
        string[] targets = rules.Target.Modules.Concat(moduleSet.IncludedPluginModules).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        var plan = MetadataLayout.Apply(context, modules, targets);
        var declaredInputs = GameInputSnapshot.DeclaredInputs(context, plan);
        var initialDeclared = GameInputSnapshot.Capture(project, output, sdk, declaredInputs);
        var metadata = await MetadataRunner.GenerateAsync(context, toolchain, plan, rules.Target,
            Path.Combine(sdk, "Tools/Rocks.MetaCompiler/Rocks.MetaCompiler.dll"), cancellationToken, targets,
            jobs: BuildCommand.Jobs(options), verifySources: BuildCommand.VerifyMetadataSources(options));
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
            Unity = BuildCommand.Switch(options, BuildCommand.UnityOption),
            Pch = BuildCommand.Switch(options, BuildCommand.PchOption),
            Adaptive = BuildCommand.Switch(options, BuildCommand.AdaptiveOption),
            Timing = BuildCommand.Timing(options),
            TimingLabel = rules.Target.Name,
            // 도구 빌드 ID의 sdk: 경로 키 기준이다(SDK 헤더·라이브러리가 설치 위치와 무관한 키를 갖는다).
            SdkDirectory = sdk,
            EngineVersion = engineVersion
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

    public static SdkManifest Verify(string sdk, NativeConfiguration configuration, NativeToolchain toolchain)
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
