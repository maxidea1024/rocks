using System.Text.Json;
using System.Text.Json.Nodes;
using Rocks.Build.Descriptors;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 게임 프로젝트의 SDK 연결 해석이다(Docs/Design/ProjectFiles.ko.md 10.1절, PJD20).
/// <list type="number">
/// <item><c>engineAssociation</c>이 경로(<c>/</c>·<c>\</c>를 담음)면 <c>.rocksproject</c> 디렉터리 기준 그 SDK다.</item>
/// <item><c>{GUID}</c>면 등록부의 그 ID다.</item>
/// <item>없으면 등록 SDK 중 이 컴퓨터와 호환되는 후보(<paramref name="incompatibility"/>가 null)이면서 엔진 주·부가 <c>engineVersion</c>과 같은
/// 것 중 패치가 가장 높은 하나다. 최고 패치가 둘 이상이면 모호하다는 오류다.</item>
/// </list>
/// 명령에 <c>--sdk</c>를 주면 이 해석을 건너뛴다(호출자).
/// </summary>
internal static class SdkAssociation
{
    /// <summary>SDK manifest 파일 이름이다.</summary>
    public const string ManifestName = "rocks-sdk.json";

    /// <summary>
    /// SDK 디렉터리를 고른다. incompatibility는 SDK 경로 → 이 컴퓨터에서 쓸 수 없는 이유(쓸 수 있으면 null)다. 해석할 수 없으면 후보와
    /// 이유를 적은 <see cref="ArgumentException"/>이다.
    /// </summary>
    public static string Resolve(ProjectDescriptor project, ISdkRegistry registry, Func<string, string?> incompatibility)
    {
        string? association = project.EngineAssociation;
        if (!string.IsNullOrEmpty(association))
        {
            if (association.Contains('/') || association.Contains('\\'))
            {
                string path = Path.GetFullPath(association, project.RootDirectory);
                return File.Exists(Path.Combine(path, ManifestName)) ? path
                    : throw new ArgumentException($"{project.FilePath}: engineAssociation {association} is not an installed SDK ({ManifestName} not found in {path}).");
            }

            SdkRegistration? entry = registry.List().FirstOrDefault(value => SdkRegistries.SameId(value.Id, association));
            return entry is not null && File.Exists(Path.Combine(entry.Path, ManifestName)) ? entry.Path
                : throw new ArgumentException($"{project.FilePath}: engineAssociation {association} is not a registered SDK on this computer. " +
                    Registered(registry));
        }

        var reasons = new List<string>();
        var candidates = new List<(SdkRegistration Entry, EngineVersion Version)>();
        foreach (SdkRegistration entry in registry.List())
        {
            if (!File.Exists(Path.Combine(entry.Path, ManifestName)))
            {
                reasons.Add($"{entry.Path}: missing (unregister it with sdk-unregister)");
                continue;
            }

            EngineVersion? version = VersionOf(entry.Path);
            if (version is null)
            {
                reasons.Add($"{entry.Path}: unreadable {ManifestName}");
                continue;
            }

            if (!version.Value.CanLinkProject(project.EngineVersion.Major, project.EngineVersion.Minor))
            {
                reasons.Add($"{entry.Path}: engine {version.Value}, the project needs {project.EngineVersion}");
                continue;
            }

            if (incompatibility(entry.Path) is string reason)
            {
                reasons.Add($"{entry.Path}: {reason}");
                continue;
            }

            candidates.Add((entry, version.Value));
        }

        if (candidates.Count == 0)
        {
            throw new ArgumentException($"{project.FilePath}: no registered SDK fits engineVersion {project.EngineVersion} on this computer" +
                (reasons.Count == 0 ? " (no SDK is registered; use sdk-register)." : ": " + string.Join("; ", reasons)) +
                " Set engineAssociation with associate or pass --sdk.");
        }

        int highest = candidates.Max(candidate => candidate.Version.Patch);
        var best = candidates.Where(candidate => candidate.Version.Patch == highest).ToArray();
        return best.Length == 1 ? best[0].Entry.Path
            : throw new ArgumentException($"{project.FilePath}: SDKs {string.Join(", ", best.Select(candidate => candidate.Entry.Path))} all fit engineVersion " +
                $"{project.EngineVersion} at patch {highest}; choose one with associate --sdk <path> or --id <id>.");
    }

    // SDK manifest의 엔진 버전이다(형식이 틀리면 null).
    private static EngineVersion? VersionOf(string sdk)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(sdk, ManifestName)));
            JsonElement version = document.RootElement.GetProperty("EngineVersion");
            return new EngineVersion(version.GetProperty("Major").GetInt32(), version.GetProperty("Minor").GetInt32(), version.GetProperty("Patch").GetInt32());
        }
        catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or ArgumentException)
        {
            return null;
        }
    }

    private static string Registered(ISdkRegistry registry)
    {
        IReadOnlyList<SdkRegistration> entries = registry.List();
        return entries.Count == 0 ? "No SDK is registered." : "Registered: " + string.Join(", ", entries.Select(entry => $"{entry.Id} {entry.Path}"));
    }

    /// <summary>
    /// <c>.rocksproject</c>의 <c>engineAssociation</c>만 바꾸거나 지운다(null). 다른 필드와 순서는 그대로 둔다.
    /// </summary>
    public static void Write(string projectFile, string? association)
    {
        JsonObject root = JsonNode.Parse(File.ReadAllText(projectFile))?.AsObject()
            ?? throw new ArgumentException(projectFile + ": not a JSON object.");
        if (association is null)
        {
            root.Remove("engineAssociation");
        }
        else
        {
            root["engineAssociation"] = association;
        }

        AtomicFile.WriteIfChanged(projectFile, root.ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = true,
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
        }) + "\n");
        DescriptorReader.ReadProject(projectFile);
    }
}
