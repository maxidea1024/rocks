using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// 생성 상태 파일(<c>ProjectFiles.state.json</c>)의 내용이다(Docs/Design/ProjectFiles.ko.md 6.5·9절). Files는 이번 실행이 만든 파일의
/// 저장소 기준 경로(정렬), Fingerprints는 <c>타깃|구성</c> → 지문(9절 낡음 경고가 비교한다)이다.
/// </summary>
internal sealed record ProjectFilesState(int FileVersion, IReadOnlyList<string> Files, IReadOnlyDictionary<string, string> Fingerprints)
{
    public const int CurrentFileVersion = 1;
}

/// <summary>쓰기 결과다. Written은 내용이 바뀌어 쓴 파일 수, Removed는 지운 파일 수다.</summary>
internal sealed record ProjectFilesWriteResult(int Written, int Unchanged, int Removed);

/// <summary>
/// 렌더링한 파일을 쓴다(6.5절): 내용이 같으면 쓰지 않고, 직전 상태 파일의 목록에 있었는데 이번에 만들지 않은 파일만 지운다(그 밖의
/// 파일 — Visual Studio의 <c>.vcxproj.user</c>·<c>Intermediate/</c> — 은 건드리지 않는다). 상태 파일은 마지막에 쓴다.
/// </summary>
internal static class ProjectFilesOutput
{
    public const string StateFileName = "ProjectFiles.state.json";

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    public static async Task<ProjectFilesWriteResult> WriteAsync(string root, string projectFilesDirectory, IReadOnlyDictionary<string, byte[]> files,
        IReadOnlyDictionary<string, string> fingerprints, CancellationToken cancellationToken)
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
        int removed = 0;
        foreach (string stale in previous?.Files.Where(path => !current.Contains(path)) ?? [])
        {
            string path = Path.GetFullPath(Path.Combine(root, stale));
            // 상태 파일이 손상돼도 생성 영역 밖은 지우지 않는다.
            if ((DirectoryPath.IsStrictlyUnder(path, projectFilesDirectory) || IsRootSolution(root, path)) && File.Exists(path))
            {
                File.Delete(path);
                ++removed;
            }
        }

        var state = new ProjectFilesState(ProjectFilesState.CurrentFileVersion, [.. current],
            new SortedDictionary<string, string>(fingerprints.ToDictionary(pair => pair.Key, pair => pair.Value), StringComparer.Ordinal));
        AtomicFile.WriteIfChanged(statePath, new UTF8Encoding(false).GetBytes(JsonSerializer.Serialize(state, JsonOptions) + "\n"));
        return new(written, unchanged, removed);
    }

    /// <summary>상태 파일을 읽는다. 없거나 형식이 다르면 null이다(정리할 목록이 없다).</summary>
    public static ProjectFilesState? Read(string statePath)
    {
        if (!File.Exists(statePath))
        {
            return null;
        }

        try
        {
            ProjectFilesState? state = JsonSerializer.Deserialize<ProjectFilesState>(File.ReadAllBytes(statePath));
            return state?.FileVersion == ProjectFilesState.CurrentFileVersion ? state : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    /// <summary>
    /// (타깃, 구성)마다 낡음 지문이다(9절): 그 프로젝트 구성의 파일 목록과 묶음 값 정규 텍스트, 실행 파일 경로, 도구 모음 props 내용의
    /// SHA-256이다.
    /// </summary>
    public static SortedDictionary<string, string> Fingerprints(ProjectFilesInput input, IReadOnlyDictionary<string, byte[]> files)
    {
        string toolchain = string.Concat(new[] { VisualStudioProjects.ToolchainProps, VisualStudioProjects.ToolchainPathsProps }
            .Select(name => Convert.ToHexStringLower(SHA256.HashData(files[Path.GetFullPath(Path.Combine(input.ProjectFilesDirectory, name))]))));
        var result = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (IdeProject project in input.Model.Projects)
        {
            foreach (IdeProjectConfiguration configuration in project.Configurations)
            {
                var text = new StringBuilder(toolchain).Append('\n');
                text.Append(input.Targets[configuration.Target].Executables.GetValueOrDefault(configuration.Configuration)).Append('\n');
                foreach (IdeSourceFile file in project.Sources)
                {
                    IntelliSenseValues? values = file.Bundle is int bundle ? project.Bundles[bundle].Values[configuration.Key] : null;
                    text.Append(file.Path).Append('\t').Append(values?.Canonical() ?? "-").Append('\n');
                }

                result[configuration.Key] = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text.ToString())));
            }
        }

        return result;
    }

    private static bool IsRootSolution(string root, string path) =>
        string.Equals(Path.GetDirectoryName(path), Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)), StringComparison.OrdinalIgnoreCase) &&
        path.EndsWith(".slnx", StringComparison.OrdinalIgnoreCase);

    private static string Relative(string root, string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
}
