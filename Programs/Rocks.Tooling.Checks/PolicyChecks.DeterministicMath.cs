using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    private const string DeterministicMathRoot = "Core/Source/Runtime/DeterministicMath";

    // Private 번역 단위가 include해도 되는 헤더다. PlatformMath·CRT 수학 헤더는 header inline 함수와 근사 명령(rsqrt 등)을
    // 끌어오므로 오브젝트의 외부 심볼 검사로는 보이지 않는다 — 그래서 소스에서 막는다.
    private static readonly HashSet<string> DeterministicMathPrivateIncludes = new(StringComparer.Ordinal)
    {
        "\"SrgbTables.generated.h\"", "<DeterministicMath/DeterministicMath.h>", "<Platform/Memory.h>", "<Base/Types.h>",
    };

    private static readonly Regex DeterministicMathInclude = Pattern("^\\s*#\\s*include\\s*([<\"][^>\"]+[>\"])");
    private static readonly Regex ExtendedPrecisionType = Pattern("\\blong\\s+double\\b");
    // 공개 헤더의 함수 본문(선언 뒤 '{'). 본문이 소비자 오브젝트에 inline되면 오브젝트 검사 범위 밖이 된다.
    private static readonly Regex FunctionBody = Pattern("\\)\\s*(?:const\\s*)?(?:noexcept\\s*)?\\{");

    /// <summary>
    /// 결정적 수학 모듈 소스 규칙(rule id deterministic-math-sources, CoreDataFoundations.ko.md 2.1·2.3):
    /// <list type="bullet">
    /// <item>Private은 허용 목록의 헤더만 include한다(생성 표, 자기 공개 헤더, Platform/Memory.h, Base/Types.h).</item>
    /// <item>공개 헤더는 Base/Types.h만 include하고 함수 본문을 두지 않는다(계산은 모두 검사되는 Private 오브젝트에).</item>
    /// <item>확장 정밀도 타입(long double)을 쓰지 않는다(x87·소프트웨어 128비트로 호스트마다 다르다).</item>
    /// </list>
    /// </summary>
    public static IReadOnlyList<CheckFailure> DeterministicMathSources(string root)
    {
        List<CheckFailure> failures = [];
        string moduleRoot = Path.Combine(Path.GetFullPath(root), DeterministicMathRoot);
        foreach (string folder in new[] { "Private", "Public" })
        {
            string directory = Path.Combine(moduleRoot, folder);
            if (!Directory.Exists(directory))
                continue;
            bool isPublic = folder == "Public";
            foreach (string path in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories)
                         .Where(file => file.EndsWith(".h", StringComparison.Ordinal) || file.EndsWith(".cpp", StringComparison.Ordinal))
                         .Order(StringComparer.Ordinal))
            {
                string relative = Relative(root, path);
                int line = 0;
                foreach (string text in File.ReadLines(path, StrictUtf8))
                {
                    ++line;
                    Match include = DeterministicMathInclude.Match(text);
                    if (include.Success && (isPublic ? include.Groups[1].Value != "<Base/Types.h>"
                                                     : !DeterministicMathPrivateIncludes.Contains(include.Groups[1].Value)))
                        failures.Add(new(relative, line, "deterministic-math-sources",
                            $"include {include.Groups[1].Value} is not allowed in deterministic math {folder} sources"));
                    if (ExtendedPrecisionType.IsMatch(text))
                        failures.Add(new(relative, line, "deterministic-math-sources",
                            "extended precision floating type is not deterministic across hosts"));
                    if (isPublic && FunctionBody.IsMatch(text))
                        failures.Add(new(relative, line, "deterministic-math-sources",
                            "deterministic math public headers must not define function bodies"));
                }
            }
        }
        return failures;
    }
}
