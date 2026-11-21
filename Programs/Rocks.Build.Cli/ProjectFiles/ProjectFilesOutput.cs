using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 생성 상태 파일(<c>ProjectFiles.state.json</c>)의 내용이다(Docs/Design/ProjectFiles.ko.md 6.5·9절). Files는 이번 실행이 만든 파일의
/// 저장소 기준 경로(정렬), Fingerprints는 <c>타깃|구성</c> → 낡음 지문(<see cref="IdePlanner.Fingerprint"/>)이다.
/// </summary>
/// Compiler는 생성 때 쓴 clang-cl 경로다(빌드 컴파일러가 다르면 낡음을 비교하지 않는다, 개정 8).
internal sealed record ProjectFilesState(int FileVersion, IReadOnlyList<string> Files, IReadOnlyDictionary<string, string> Fingerprints, string Compiler)
{
    // 지문 정의를 (타깃, 구성) 계획 단위로 바꾸며 2, 생성 컴파일러를 더하며 3으로 올렸다(개정 6·8).
    public const int CurrentFileVersion = 3;
}

/// <summary>쓰기 결과다. Written은 내용이 바뀌어 쓴 파일 수, Removed는 지운 파일 수다.</summary>
internal sealed record ProjectFilesWriteResult(int Written, int Unchanged, int Removed);

/// <summary>
/// 렌더링한 파일을 쓴다(6.5절): 내용이 같으면 쓰지 않고, 직전 상태 파일의 목록에 있었는데 이번에 만들지 않은 파일만 지운다(그 밖의
/// 파일 — Visual Studio의 <c>.vcxproj.user</c>·<c>Intermediate/</c> — 은 건드리지 않는다). 지울 수 있는 곳은 프로젝트 파일 디렉터리
/// 아래와 솔루션 파일 하나뿐이다. 상태 파일은 마지막에 쓴다.
/// </summary>
internal static class ProjectFilesOutput
{
    public const string StateFileName = "ProjectFiles.state.json";

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    // 경로 비교 규칙이다(Windows는 대소문자 무시 — 대소문자만 바뀐 개명이 새 파일을 지우지 않게).
    private static readonly StringComparer PathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    public static async Task<ProjectFilesWriteResult> WriteAsync(string root, string projectFilesDirectory, string solutionPath,
        IReadOnlyDictionary<string, byte[]> files, IReadOnlyDictionary<string, string> fingerprints, string compiler, CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(projectFilesDirectory);
        await using var invocationLock = await OutputInvocationLock.AcquireAsync(projectFilesDirectory, cancellationToken);
        string statePath = Path.Combine(projectFilesDirectory, StateFileName);
        ProjectFilesState? previous = Read(statePath);
        int written = 0;
        int unchanged = 0;
        foreach ((string path, byte[] content) in files)
        {
            if (File.Exists(path) && File.ReadAllBytes(path).AsSpan().SequenceEqual(content))
            {
                ++unchanged;
                continue;
            }

            AtomicFile.WriteIfChanged(path, content);
            ++written;
        }

        var current = new SortedSet<string>(files.Keys.Select(path => Relative(root, path)), StringComparer.Ordinal);
        var currentLookup = new HashSet<string>(current, PathComparer);
        int removed = 0;
        foreach (string stale in previous?.Files.Where(path => !currentLookup.Contains(path)) ?? [])
        {
            string path = Path.GetFullPath(Path.Combine(root, stale));
            // 상태 파일이 손상되거나 조작돼도 생성 영역(프로젝트 파일 디렉터리와 이 솔루션 파일) 밖은 지우지 않는다.
            bool inside = DirectoryPath.IsStrictlyUnder(path, projectFilesDirectory) || PathComparer.Equals(path, Path.GetFullPath(solutionPath));
            if (inside && File.Exists(path))
            {
                File.Delete(path);
                ++removed;
            }
        }

        var state = new ProjectFilesState(ProjectFilesState.CurrentFileVersion, [.. current],
            new SortedDictionary<string, string>(fingerprints.ToDictionary(pair => pair.Key, pair => pair.Value), StringComparer.Ordinal), compiler);
        AtomicFile.WriteIfChanged(statePath, new UTF8Encoding(false).GetBytes(JsonSerializer.Serialize(state, JsonOptions) + "\n"));
        return new(written, unchanged, removed);
    }

    /// <summary>
    /// 상태 파일을 읽는다. 없거나, 판이 다르거나, 형식이 깨졌으면(목록·항목·지문이 null) null이다 — 지울 목록이 없을 뿐 생성은 계속한다.
    /// </summary>
    public static ProjectFilesState? Read(string statePath)
    {
        if (!File.Exists(statePath))
        {
            return null;
        }

        try
        {
            ProjectFilesState? state = JsonSerializer.Deserialize<ProjectFilesState>(File.ReadAllBytes(statePath));
            bool valid = state is not null && state.FileVersion == ProjectFilesState.CurrentFileVersion && state.Files is not null &&
                state.Files.All(path => !string.IsNullOrEmpty(path)) && state.Fingerprints is not null &&
                state.Fingerprints.All(pair => !string.IsNullOrEmpty(pair.Key) && !string.IsNullOrEmpty(pair.Value)) && !string.IsNullOrEmpty(state.Compiler);
            return valid ? state : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    /// <summary>두 도구 모음 props 내용 해시다(낡음 지문의 도구 모음 부분, 9절).</summary>
    public static string ToolchainHash(string projectFilesDirectory, IReadOnlyDictionary<string, byte[]> files) => string.Concat(
        new[] { VisualStudioProjects.ToolchainProps, VisualStudioProjects.ToolchainPathsProps }
            .Select(name => Convert.ToHexStringLower(SHA256.HashData(files[Path.GetFullPath(Path.Combine(projectFilesDirectory, name))]))));

    /// <summary>(타깃, 구성)마다 낡음 지문이다(9절). 빌드가 같은 계획으로 같은 값을 계산한다(<see cref="IdePlanner.Fingerprint"/>).</summary>
    public static SortedDictionary<string, string> Fingerprints(IReadOnlyList<IdeTargetPlan> plans, IReadOnlyDictionary<string, ProjectFilesTarget> targets,
        string toolchainHash)
    {
        var result = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (IdeTargetPlan plan in plans)
        {
            string? executable = targets[plan.Target].Executables.GetValueOrDefault(plan.Configuration);
            result[plan.Target + "|" + plan.Configuration] = IdePlanner.Fingerprint(plan.Plan.Compiles, executable, toolchainHash);
        }

        return result;
    }

    private static string Relative(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
}
