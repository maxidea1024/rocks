using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 다중 구성 설치 SDK를 조립한다(Docs/Design/ProjectFiles.ko.md 10.3절). 구성마다 엔진을 빌드한 뒤 <see cref="AddConfigurationAsync"/>로 그 구성의
/// 파일을 stage에 더하고, <see cref="Publish"/>가 manifest를 쓰고 대상 디렉터리로 옮긴다.
/// <list type="bullet">
/// <item>구성과 무관한 부분(공개 헤더, <c>Tools/</c>, <c>Licenses/</c>, 스크립트 등)은 한 벌이다. 구성마다 내용이 다르거나 한 구성에만 있으면
/// 내보내기 오류다(내용 해시로 비교).</item>
/// <item>구성마다 다른 부분(<c>Libraries/</c>, <c>Metadata/</c>)은 <c>Configurations/&lt;구성&gt;/</c> 아래에 둔다. 모듈 선언도 구성마다 따로
/// 싣는다(정의·의존이 구성에 따라 다를 수 있다).</item>
/// </list>
/// 실패하면 stage를 지우고 대상은 만들지 않는다(대상은 처음부터 없어야 한다).
/// </summary>
internal sealed class SdkAssembly : IDisposable
{
    /// <summary>구성마다 다른 파일을 담는 최상위 디렉터리다.</summary>
    internal const string ConfigurationsDirectory = "Configurations";

    // 구성마다 다른 부분의 최상위 디렉터리다(라이브러리, 리플렉션 descriptor·identity 명세 사본).
    private static readonly string[] PerConfigurationRoots = ["Libraries", "Metadata"];

    private readonly string stage;
    // 한 벌 파일 → (내용 해시, 처음 더한 구성)이다.
    private readonly Dictionary<string, (string Hash, NativeConfiguration Configuration)> shared = new(StringComparer.Ordinal);
    private readonly SortedDictionary<NativeConfiguration, HashSet<string>> sharedByConfiguration = [];
    private readonly SortedDictionary<NativeConfiguration, SdkConfiguration> configurations = [];
    private NativeToolchain? toolchain;
    private EngineVersion? engineVersion;

    /// <summary>
    /// 대상 디렉터리 옆에 stage를 만든다. 대상은 새 경로여야 하고 파일시스템 루트가 아니어야 한다.
    /// </summary>
    public SdkAssembly(string destination)
    {
        // 끝 구분자를 떼야 stage가 destination 옆(같은 부모 아래)에 생긴다. TrimEndingDirectorySeparator는 루트를 그대로 두므로 루트는 따로 거부한다.
        Destination = PathSpelling.OnDisk(Path.TrimEndingDirectorySeparator(Path.GetFullPath(destination)));
        if (Path.GetDirectoryName(Destination) is null)
        {
            throw new IOException("SDK destination must not be a filesystem root. Use a new versioned directory.");
        }

        if (Directory.Exists(Destination) || File.Exists(Destination))
        {
            throw new IOException("SDK destination already exists. Use a new versioned directory.");
        }

        stage = Destination + ".staging-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(stage);
    }

    /// <summary>SDK를 게시할 대상 디렉터리다(디스크 철자, 끝 구분자 없음).</summary>
    public string Destination { get; }

    /// <summary>
    /// 빌드한 구성 하나를 더한다. 모든 구성은 같은 도구 모음(플랫폼·ABI)과 엔진 버전이어야 한다.
    /// </summary>
    public async Task AddConfigurationAsync(string root, NativeConfiguration configuration, NativeToolchain configurationToolchain,
        EngineVersion configurationEngine, IReadOnlyList<NativeModuleInput> modules, IReadOnlyList<MetadataModule> metadata, NativeBuildResult build,
        CancellationToken cancellationToken)
    {
        if (configurationEngine.IsEmpty)
        {
            throw new ArgumentException("SDK export requires the engine version (0.0.x is the empty value).", nameof(configurationEngine));
        }

        if (configurations.ContainsKey(configuration))
        {
            throw new InvalidOperationException($"The SDK already has the {configuration} configuration.");
        }

        if (toolchain is not null && (toolchain.Platform != configurationToolchain.Platform || toolchain.AbiIdentity != configurationToolchain.AbiIdentity))
        {
            throw new InvalidOperationException($"The {configuration} configuration was built with another toolchain than the other SDK configurations.");
        }

        if (engineVersion is not null && engineVersion != configurationEngine)
        {
            throw new InvalidOperationException($"The {configuration} configuration has engine {configurationEngine}, the other SDK configurations {engineVersion}.");
        }

        string configurationStage = stage + "-" + configuration.ToString().ToLowerInvariant();
        Directory.CreateDirectory(configurationStage);
        try
        {
            SdkModule[] staged = InstalledSdk.StageConfiguration(root, configurationStage, modules, metadata, build);
            string[] files = Merge(configuration, configurationStage);
            configurations[configuration] = new SdkConfiguration([.. staged.Select(module => module with
            {
                Libraries = [.. module.Libraries.Select(path => Target(configuration, path))],
                Descriptor = module.Descriptor is null ? null : Target(configuration, module.Descriptor),
                IdentityManifest = module.IdentityManifest is null ? null : Target(configuration, module.IdentityManifest)
            })], files);
        }
        finally
        {
            if (Directory.Exists(configurationStage))
            {
                Directory.Delete(configurationStage, recursive: true);
            }
        }

        toolchain ??= configurationToolchain;
        engineVersion ??= configurationEngine;
        cancellationToken.ThrowIfCancellationRequested();
        await Task.CompletedTask;
    }

    /// <summary>
    /// 구성 stage의 파일을 SDK stage로 옮긴다. 구성마다 다른 파일은 <c>Configurations/&lt;구성&gt;/</c> 아래로 가고(돌려주는 목록), 한 벌 파일은 처음 본
    /// 구성의 것을 두고 다음 구성은 내용 해시가 같아야 한다.
    /// </summary>
    internal string[] Merge(NativeConfiguration configuration, string configurationStage)
    {
        foreach (string directory in Directory.GetDirectories(configurationStage, "*", SearchOption.AllDirectories))
        {
            Directory.CreateDirectory(Path.Combine(stage, Target(configuration, Relative(configurationStage, directory))));
        }

        var own = new List<string>();
        var sharedFiles = new HashSet<string>(StringComparer.Ordinal);
        foreach (string file in Directory.GetFiles(configurationStage, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
        {
            string relative = Relative(configurationStage, file);
            string target = Target(configuration, relative);
            if (target != relative)
            {
                File.Move(file, Path.Combine(stage, target));
                own.Add(target);
                continue;
            }

            sharedFiles.Add(relative);
            string hash = Hashing.Sha256File(file);
            if (shared.TryGetValue(relative, out var first))
            {
                if (first.Hash != hash)
                {
                    throw new InvalidOperationException($"SDK file {relative} differs between the {first.Configuration} and {configuration} configurations; " +
                        "configuration-independent files (headers, tools, licenses, scripts) must be identical in every configuration.");
                }

                continue;
            }

            shared[relative] = (hash, configuration);
            File.Move(file, Path.Combine(stage, relative));
        }

        sharedByConfiguration[configuration] = sharedFiles;
        return [.. own];
    }

    /// <summary>모든 구성이 같은 한 벌 파일 집합을 냈는지 본다(한 구성에만 있는 한 벌 파일은 오류다).</summary>
    internal void RequireSameSharedFiles()
    {
        foreach ((NativeConfiguration configuration, HashSet<string> files) in sharedByConfiguration)
        {
            foreach ((NativeConfiguration other, HashSet<string> otherFiles) in sharedByConfiguration)
            {
                if (files.FirstOrDefault(file => !otherFiles.Contains(file)) is string only)
                {
                    throw new InvalidOperationException($"SDK file {only} is in the {configuration} configuration but not in {other}; " +
                        "configuration-independent files must be identical in every configuration.");
                }
            }
        }
    }

    /// <summary>manifest를 쓰고 stage를 대상으로 옮긴다.</summary>
    public void Publish()
    {
        if (configurations.Count == 0 || toolchain is null || engineVersion is not EngineVersion version)
        {
            throw new InvalidOperationException("The SDK has no configuration to publish.");
        }

        RequireSameSharedFiles();
        var hashes = Directory.GetFiles(stage, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal)
            .ToDictionary(file => Relative(stage, file), Hashing.Sha256File, StringComparer.Ordinal);
        var manifest = new SdkManifest(InstalledSdk.ManifestVersion, new SdkEngineVersion(version.Major, version.Minor, version.Patch),
            configurations.ToDictionary(pair => pair.Key.ToString(), pair => pair.Value, StringComparer.Ordinal), toolchain.Platform.ToString(),
            System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString(), toolchain.AbiIdentity, toolchain.AbiIdentity, hashes);
        AtomicFile.WriteIfChanged(Path.Combine(stage, SdkAssociation.ManifestName), JsonSerializer.Serialize(manifest, BuildCommand.JsonOptions));
        Directory.Move(stage, Destination);
    }

    public void Dispose()
    {
        // 삭제 대상은 이 조립이 만든 동일 부모의 임의 stage 하나로 제한한다(게시했으면 이미 없다).
        if (Directory.Exists(stage))
        {
            Directory.Delete(stage, recursive: true);
        }
    }

    // 구성마다 다른 파일이면 Configurations/<구성>/ 아래 경로, 한 벌 파일이면 그대로다.
    private static string Target(NativeConfiguration configuration, string relative) =>
        PerConfigurationRoots.Any(root => relative == root || relative.StartsWith(root + "/", StringComparison.Ordinal))
            ? $"{ConfigurationsDirectory}/{configuration}/{relative}"
            : relative;

    private static string Relative(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
}
