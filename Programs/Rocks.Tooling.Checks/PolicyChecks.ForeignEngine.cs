using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    /// <summary>
    /// 다른 엔진(UE)의 코드를 두는 저장소 최상위 폴더다. Rocks 빌드 규칙·정책·서식 검사가 보지 않는다
    /// (Docs/Design/UnrealExchange 11.1절 — UE 추출 플러그인 RocksExport가 여기 산다).
    /// </summary>
    public const string ForeignEngineRoot = "Extras";

    // Rocks 규칙 프로젝트가 *.Build.cs·*.Target.cs를 모으는 경로다. Build/Rules/Rocks.Engine.Rules.csproj(Core/Source·
    // Engine/Source·Engine/Plugins·Programs, 단 C# 도구 Programs/Rocks.* 제외)와 Build/Format/Rocks.Template.Rules.csproj
    // (Templates)를 따른다. 두 csproj의 glob을 바꾸면 이 목록도 바꾼다.
    private static readonly string[] RocksRuleRoots = ["Core/Source", "Engine/Source", "Engine/Plugins", "Programs", "Templates"];

    private const string RocksToolProgramsPrefix = "Programs/Rocks.";

    private static readonly string[] RocksDescriptorExtensions = [".rocksproject", ".rocksplugin"];

    private static readonly string[] UnrealDescriptorExtensions = [".uplugin", ".uproject"];

    private static readonly Regex RocksRulesNamespace = Pattern("\\bRocks\\s*\\.\\s*Build\\b");

    private static readonly Regex UnrealRulesNamespace = Pattern("\\bUnrealBuildTool\\b");

    private const string ForeignEngineRule = "foreign-engine-boundary";

    /// <summary>
    /// 두 엔진의 빌드 파일이 서로의 자리에 들어가지 않았는지 본다(rule id foreign-engine-boundary, UnrealExchange 11.1절).
    /// <list type="bullet">
    /// <item><c>Extras/</c> 아래: Rocks 서술자(.rocksproject·.rocksplugin)가 있거나, 규칙 파일(*.Build.cs·*.Target.cs)이 Rocks 빌드
    /// 네임스페이스를 참조하면 실패한다. 거기 둔 Rocks 모듈은 Rocks 빌드가 조용히 빼먹는다.</item>
    /// <item>Rocks 규칙 경로 아래: UE 서술자(.uplugin·.uproject)가 있거나, 규칙 파일이 UnrealBuildTool을 참조하면 실패한다. 거기 둔
    /// UE 플러그인은 Rocks 규칙 프로젝트가 UE ModuleRules를 컴파일하다 원인이 흐린 오류로 멈춘다.</item>
    /// </list>
    /// 네임스페이스는 주석·문자열을 지운 텍스트에서 찾는다(설명 주석에 상대 엔진 이름을 적는 것은 허용). C# verbatim 문자열의
    /// "" 이스케이프는 C++ 기준 lexical 근사라 정확히 지우지 못할 수 있다. 서술자 확장자는 대소문자를 무시한다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> ForeignEngineBoundary(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in Files(root, [ForeignEngineRoot]))
        {
            string relative = Relative(root, path);
            if (HasExtension(path, RocksDescriptorExtensions))
            {
                failures.Add(new(relative, 0, ForeignEngineRule,
                    $"Rocks descriptor under {ForeignEngineRoot}/ is not built; Rocks plugins and projects belong under the Rocks rule roots"));
            }
            else if (IsBuildRulesFile(path))
            {
                AddNamespaceFailures(path, relative, RocksRulesNamespace,
                    $"rules file under {ForeignEngineRoot}/ references the Rocks build namespace; Rocks modules belong under the Rocks rule roots",
                    failures);
            }
        }

        foreach (string path in Files(root, RocksRuleRoots))
        {
            string relative = Relative(root, path);
            if (relative.StartsWith(RocksToolProgramsPrefix, StringComparison.Ordinal))
            {
                continue;
            }

            if (HasExtension(path, UnrealDescriptorExtensions))
            {
                failures.Add(new(relative, 0, ForeignEngineRule,
                    $"Unreal descriptor under a Rocks rule root; Unreal plugins belong under {ForeignEngineRoot}/"));
            }
            else if (IsBuildRulesFile(path))
            {
                AddNamespaceFailures(path, relative, UnrealRulesNamespace,
                    $"rules file under a Rocks rule root references UnrealBuildTool; Unreal modules belong under {ForeignEngineRoot}/",
                    failures);
            }
        }

        return failures;
    }

    private static bool HasExtension(string path, IReadOnlyList<string> extensions) =>
        extensions.Any(extension => path.EndsWith(extension, StringComparison.OrdinalIgnoreCase));

    private static bool IsBuildRulesFile(string path) =>
        path.EndsWith(".Build.cs", StringComparison.Ordinal) || path.EndsWith(".Target.cs", StringComparison.Ordinal);

    private static void AddNamespaceFailures(string path, string relative, Regex pattern, string message, List<CheckFailure> failures)
    {
        string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
        int line = 0;
        foreach (string text in code.Split('\n'))
        {
            ++line;
            if (pattern.IsMatch(text))
            {
                failures.Add(new(relative, line, ForeignEngineRule, message));
            }
        }
    }
}
