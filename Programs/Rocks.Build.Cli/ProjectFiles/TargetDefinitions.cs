using System.Text.RegularExpressions;
using Rocks.Build.Rules;

namespace Rocks.Build.Cli;

/// <summary>
/// 타깃 정의 디렉터리를 찾는다(Docs/Design/ProjectFiles.ko.md 6.2절): 규칙 어셈블리와 같은 glob의 <c>*.Target.cs</c> 중 그 타깃 규칙 타입의
/// <c>class</c> 선언이 정확히 한 파일에 있어야 한다.
/// </summary>
internal static class TargetDefinitions
{
    // 규칙 어셈블리(Build/Rules/Rocks.Engine.Rules.csproj)가 *.Target.cs를 모으는 루트다. Programs/Rocks.*(C# 도구)는 제외한다.
    private static readonly string[] Roots = ["Core/Source", "Engine/Source", "Engine/Plugins", "Programs"];

    // 타깃 규칙 파일 이름의 끝이다.
    private const string TargetSuffix = ".Target.cs";

    /// <summary>타깃 이름 → (정의 디렉터리, 정의 파일 이름에서 <c>.Target.cs</c>를 뗀 것)이다.</summary>
    /// <exception cref="ArgumentException">규칙 타입의 선언이 정확히 한 파일에 있지 않다.</exception>
    public static IReadOnlyDictionary<string, (string Directory, string Name)> Find(string root, IReadOnlyList<TargetRules> targets) =>
        FromFiles([.. Roots.Select(relative => Path.Combine(root, relative)).Where(Directory.Exists)
            .SelectMany(directory => Directory.EnumerateFiles(directory, "*" + TargetSuffix, SearchOption.AllDirectories))
            .Where(file => !Path.GetRelativePath(root, file).Replace('\\', '/').StartsWith("Programs/Rocks.", StringComparison.Ordinal))], targets);

    /// <summary>
    /// 주어진 규칙 파일(<c>*.Target.cs</c>가 아닌 것은 무시) 중에서 찾는다. 게임 모드는 게임 규칙 탐색 결과(<see cref="GameProjectLayout.RuleFiles"/>)를
    /// 준다(Docs/Design/ProjectFiles.ko.md 10.4절).
    /// </summary>
    public static IReadOnlyDictionary<string, (string Directory, string Name)> FromFiles(IEnumerable<string> ruleFiles, IReadOnlyList<TargetRules> targets)
    {
        string[] files = [.. ruleFiles.Where(file => file.EndsWith(TargetSuffix, StringComparison.Ordinal)).Order(StringComparer.Ordinal)];
        var texts = files.ToDictionary(file => file, File.ReadAllText, StringComparer.Ordinal);
        var result = new Dictionary<string, (string Directory, string Name)>(StringComparer.Ordinal);
        foreach (TargetRules target in targets)
        {
            var declaration = new Regex(@"\bclass\s+" + Regex.Escape(target.GetType().Name) + @"\b", RegexOptions.CultureInvariant);
            string[] declaring = [.. files.Where(file => declaration.IsMatch(texts[file]))];
            if (declaring.Length != 1)
            {
                throw new ArgumentException($"Target {target.Name}: rule type {target.GetType().Name} must be declared in exactly one *.Target.cs " +
                    $"(found {declaring.Length}).");
            }

            result[target.Name] = (Path.GetDirectoryName(declaring[0])!, Path.GetFileName(declaring[0])[..^TargetSuffix.Length]);
        }

        return result;
    }
}
