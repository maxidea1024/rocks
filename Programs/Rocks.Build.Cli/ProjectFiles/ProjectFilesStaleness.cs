using System.Security.Cryptography;
using Rocks.Build.Rules;

namespace Rocks.Build.Cli;

/// <summary>
/// Visual Studio 프로젝트 파일의 낡음 경고(Docs/Design/ProjectFiles.ko.md 9절, <c>RPF0001</c>)와 생성 헤더 안내(8.6절)다.
/// </summary>
internal static class ProjectFilesStaleness
{
    /// <summary>경고 코드다(MSBuild 정규 형식이라 Visual Studio 오류 목록에 나온다).</summary>
    public const string WarningCode = "RPF0001";

    /// <summary>
    /// 빌드가 끝난 뒤 그 계획의 지문을 상태 파일의 (타깃|구성) 값과 비교한다. 상태 파일이 없으면(프로젝트 파일을 만든 적 없음) null이다.
    /// 그 키가 없거나(새 타깃) 값이 다르면 경고 한 줄이다. 도구 모음 props는 지금 도구 모음으로 렌더링해 해시한다 — 컴파일러·MSVC 판이
    /// 바뀌어도 낡은 것으로 본다. LLVM 설치 모양이 IntelliSense에 맞지 않으면(<see cref="ProjectFilesCommand.ToolchainOf"/> 오류) 검사하지
    /// 않는다.
    /// </summary>
    public static string? Warning(string root, TargetRules target, NativeConfiguration configuration, NativeToolchain toolchain, NativeBuildResult result)
    {
        ProjectFilesState? state = ProjectFilesOutput.Read(Path.Combine(root, "out", "ProjectFiles", ProjectFilesOutput.StateFileName));
        if (state is null)
        {
            return null;
        }

        ProjectFilesToolchain local;
        try
        {
            local = ProjectFilesCommand.ToolchainOf(toolchain);
        }
        catch (ArgumentException)
        {
            return null;
        }

        (byte[] props, byte[] paths) = VisualStudioProjects.ToolchainFiles(local);
        string toolchainHash = Convert.ToHexStringLower(SHA256.HashData(props)) + Convert.ToHexStringLower(SHA256.HashData(paths));
        string? primary = result.ModuleOutputs.GetValueOrDefault(target.Modules[0]);
        string? executable = primary is not null && primary.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? primary : null;
        string key = target.Name + "|" + configuration;
        string fingerprint = IdePlanner.Fingerprint(result.IdeCompiles, executable, toolchainHash);
        if (state.Fingerprints.TryGetValue(key, out string? recorded) && recorded == fingerprint)
        {
            return null;
        }

        string reason = state.Fingerprints.ContainsKey(key) ? "files or settings changed" : "the target is not in the project files";
        return $"warning {WarningCode}: Visual Studio project files are out of date for {target.Name} {configuration} ({reason}). Run GenerateProjectFiles.bat.";
    }

    /// <summary>
    /// 생성 헤더 안내(8.6절): IntelliSense 원천 명령이 가리키는 출력 아래 include 디렉터리 중 없는 것이 있는 계획의 (타깃, 구성)을 타깃마다
    /// 모은다. 빌드할 타깃은 원천 명령을 낸 계획의 타깃이다(파일이 든 프로젝트의 타깃과 다를 수 있다).
    /// </summary>
    public static IEnumerable<string> GeneratedHeaderNotes(IntelliSenseModel model, IReadOnlyList<IdeTargetPlan> plans)
    {
        var owners = new Dictionary<IdePlan, IdeTargetPlan>(ReferenceEqualityComparer.Instance);
        foreach (IdeTargetPlan plan in plans)
        {
            owners[plan.Plan] = plan;
        }

        var missing = new SortedDictionary<string, SortedSet<string>>(StringComparer.Ordinal);
        var files = new Dictionary<string, int>(StringComparer.Ordinal);
        var checkedDirectories = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
        bool Exists(string directory) => checkedDirectories.TryGetValue(directory, out bool value) ? value : checkedDirectories[directory] = Directory.Exists(directory);
        foreach (IdeProject project in model.Projects)
        {
            foreach (IdeSourceFile file in project.Sources)
            {
                foreach (IdeChoice? choice in file.Choices.Values)
                {
                    if (choice is null)
                    {
                        continue;
                    }

                    bool absent = choice.Compile.Arguments.Where(argument => argument.StartsWith("/I", StringComparison.Ordinal))
                        .Select(argument => argument[2..])
                        .Any(directory => Tooling.Common.DirectoryPath.IsAtOrUnder(directory, choice.Plan.OutputDirectory) && !Exists(directory));
                    if (!absent)
                    {
                        continue;
                    }

                    IdeTargetPlan owner = owners[choice.Plan];
                    (missing.TryGetValue(owner.Target, out var set) ? set : missing[owner.Target] = new(StringComparer.Ordinal)).Add(owner.Configuration);
                    files[owner.Target] = files.GetValueOrDefault(owner.Target) + 1;
                }
            }
        }

        foreach ((string target, SortedSet<string> configurations) in missing)
        {
            yield return $"reflection headers of {files[target]} (file, configuration) IntelliSense entries are not generated yet; build target {target} " +
                $"({string.Join(", ", configurations)}) once.";
        }
    }
}
