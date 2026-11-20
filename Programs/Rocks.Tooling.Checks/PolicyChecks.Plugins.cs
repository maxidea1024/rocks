using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 엔진 플러그인 폴더다(Docs/Design/BuildTargets.ko.md 3.9절). Rocks.Engine.Rules.csproj가 이 아래 규칙을 모은다.
    private const string EnginePluginRoot = "Engine/Plugins";

    private const string PluginModuleRulesRule = "plugin-module-rules-base";

    private static readonly Regex PluginModuleRulesClass = Pattern("\\bclass\\s+(\\w+)\\s*:\\s*PluginModuleRules\\b");

    private static readonly Regex OtherModuleRulesClass = Pattern("\\bclass\\s+\\w+\\s*:\\s*(?:ModuleRules|SourceModuleRules|CoreModuleRules|EngineModuleRules|ThirdPartyModuleRules)\\b");

    /// <summary>
    /// 엔진 플러그인 모듈 규칙이 기술자를 단일 출처로 쓰는지 본다(rule id plugin-module-rules-base, BuildTargets 3.9절 BD37·BD42).
    /// <list type="bullet">
    /// <item><c>Engine/Plugins/**/*.Build.cs</c>는 <c>&lt;플러그인&gt;/Source/&lt;모듈&gt;/&lt;모듈&gt;.Build.cs</c>에 있고, 그 플러그인 폴더에
    /// <c>&lt;플러그인&gt;.rocksplugin</c>이 있어야 한다.</item>
    /// <item>규칙 클래스는 <c>PluginModuleRules</c>를 상속하고 이름이 모듈 이름이다. 다른 규칙 기반 클래스를 상속하면 모듈 폴더·종류·단계를
    /// 기술자와 따로 적게 되므로 거부한다.</item>
    /// </list>
    /// 클래스 선언은 주석·문자열을 지운 텍스트에서 찾는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> PluginModuleRules(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in Files(root, [EnginePluginRoot]).Where(path => path.EndsWith(".Build.cs", StringComparison.Ordinal)))
        {
            string relative = Relative(root, path);
            string module = Path.GetFileName(path)[..^".Build.cs".Length];
            string[] parts = relative.Split('/');
            // Engine/Plugins/[분류/...]<플러그인>/Source/<모듈>/<모듈>.Build.cs
            bool placed = parts.Length >= 6 && parts[^3] == "Source" && parts[^2] == module &&
                File.Exists(Path.Combine(root, string.Join('/', parts[..^3]), parts[^4] + ".rocksplugin"));
            if (!placed)
            {
                failures.Add(new(relative, 0, PluginModuleRulesRule,
                    "plugin module rules must be <Plugin>/Source/<Module>/<Module>.Build.cs next to <Plugin>/<Plugin>.rocksplugin"));
            }

            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            Match match = PluginModuleRulesClass.Match(code);
            if (!match.Success || match.Groups[1].Value != module || OtherModuleRulesClass.IsMatch(code))
            {
                failures.Add(new(relative, 0, PluginModuleRulesRule,
                    $"plugin module rules must declare 'class {module} : PluginModuleRules' (the plugin descriptor gives the folder, type and loading phase)"));
            }
        }

        return failures;
    }
}
