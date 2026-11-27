using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // Element 하향 변환을 검사하는 UI 모듈이다(Docs/Design/UiToolkit 20.10절 CB2).
    private static readonly string[] UiElementCastRoots =
    [
        "Engine/Source/Runtime/UiCore",
        "Engine/Source/Runtime/UiFramework",
        "Engine/Source/Runtime/UiWidget",
        "Engine/Source/Runtime/UiScreens",
        "Engine/Source/Runtime/UiWorld",
        "Engine/Source/Runtime/UiRender",
        "Programs/UiDemo",
    ];

    // class·struct 정의와 기반 목록이다(기반은 UiBaseName으로 하나씩 꺼낸다).
    private static readonly Regex UiClassDefinition = Pattern(
        @"\b(?:class|struct)\s+(?:ROCKS_\w+_API\s+)?(\w+)(?:\s+final)?\s*:([^{;]*)\{");

    // 기반 목록의 한 항목이다(접근 지정·virtual 생략 가능, 이름공간 한정은 마지막 이름만 본다).
    private static readonly Regex UiBaseName = Pattern(
        @"(?:^|,)\s*(?:(?:public|protected|private|virtual)\s+)*(?:::\s*)?(?:\w+\s*::\s*)*(\w+)\s*(?:<[^,]*>)?\s*(?=,|$)");

    // 참조·포인터로의 static_cast다(이름공간 한정·뒤쪽 const 포함).
    private static readonly Regex UiStaticCast = Pattern(
        @"static_cast\s*<\s*(?:const\s+)?(?:::\s*)?(?:\w+\s*::\s*)*(\w+)\s*(?:const\s*)?[&*]\s*>");

    // ROCKS_UI_ELEMENT 선언의 클래스 이름이다.
    private static readonly Regex UiElementDeclaration = Pattern(@"\bROCKS_UI_ELEMENT\s*\(\s*(\w+)\s*,");

    private const string UiElementCastRule = "ui-element-cast";

    private const string UiElementNameRule = "ui-element-name";

    /// <summary>
    /// UI 모듈에서 Element 파생 형식으로의 static_cast를 막는다(rule id ui-element-cast, UiToolkit 20.10절). Element 계열
    /// 판정은 형식 이름이 아니라 UI 모듈 안의 상속 관계로 한다(ListView처럼 이름에 Element가 없는 형식도 잡는다).
    /// 형식 검사가 있는 StaticElementCast·ElementCast를 쓴다. Element.h 안의 변환 함수 본문은 대상이 아니다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> UiElementCasts(string root)
    {
        List<string> files = [.. Files(root, UiElementCastRoots).Where(path =>
            path.EndsWith(".h", StringComparison.Ordinal) || path.EndsWith(".cpp", StringComparison.Ordinal))];
        Dictionary<string, string> codes = [];
        Dictionary<string, List<string>> bases = [];
        foreach (string path in files)
        {
            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            codes[path] = code;
            foreach (Match match in UiClassDefinition.Matches(code))
            {
                if (!bases.TryGetValue(match.Groups[1].Value, out var list))
                {
                    bases[match.Groups[1].Value] = list = [];
                }

                foreach (Match baseMatch in UiBaseName.Matches(match.Groups[2].Value.Trim()))
                {
                    list.Add(baseMatch.Groups[1].Value);
                }
            }
        }

        HashSet<string> elements = ["Element"];
        for (bool changed = true; changed;)
        {
            changed = false;
            foreach (var (name, parents) in bases)
            {
                if (!elements.Contains(name) && parents.Any(elements.Contains))
                {
                    elements.Add(name);
                    changed = true;
                }
            }
        }

        List<CheckFailure> failures = [];
        // 형식 판정이 이름으로 물러나므로(ElementType.h) 같은 이름의 ROCKS_UI_ELEMENT 선언은 하나여야 한다.
        Dictionary<string, string> declared = [];
        foreach (string path in files)
        {
            int declarationLine = 0;
            foreach (string text in codes[path].Split('\n'))
            {
                ++declarationLine;
                foreach (Match match in UiElementDeclaration.Matches(text))
                {
                    string name = match.Groups[1].Value;
                    string where = $"{Relative(root, path)}:{declarationLine}";
                    if (declared.TryGetValue(name, out var first))
                    {
                        failures.Add(new(Relative(root, path), declarationLine, UiElementNameRule,
                            $"ROCKS_UI_ELEMENT name '{name}' is already declared at {first}; Element type names must be " +
                            "unique across UI modules (Docs/Design/UiToolkit 20.10 CB2)"));
                    }
                    else
                    {
                        declared[name] = where;
                    }
                }
            }
        }

        foreach (string path in files)
        {
            if (Relative(root, path) == "Engine/Source/Runtime/UiCore/Public/UiCore/Element.h")
            {
                continue;
            }

            int line = 0;
            foreach (string text in codes[path].Split('\n'))
            {
                ++line;
                foreach (Match match in UiStaticCast.Matches(text))
                {
                    string target = match.Groups[1].Value;
                    if (target != "Element" && elements.Contains(target))
                    {
                        failures.Add(new(Relative(root, path), line, UiElementCastRule,
                            $"downcast to Element type '{target}' must use StaticElementCast or ElementCast " +
                            "(Docs/Design/UiToolkit 20.10 CB2)"));
                    }
                }
            }
        }

        return failures;
    }
}
