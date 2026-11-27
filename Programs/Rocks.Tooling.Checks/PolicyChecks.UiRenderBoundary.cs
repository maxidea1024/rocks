using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // UI Element·Widget 층 모듈이다(Docs/Design/UiToolkit 18.2절). 이 모듈의 공개 헤더는 그래픽 계층을 모른다 — 그리기는
    // UiCore UiRenderer 계약으로만 넘기고, 장치·그래프 타입은 UiRender(Render lane)에만 있다.
    private static readonly string[] UiElementLayerPublicRoots =
    [
        "Engine/Source/Runtime/UiCore/Public",
        "Engine/Source/Runtime/UiFramework/Public",
        "Engine/Source/Runtime/UiWidget/Public",
        "Engine/Source/Runtime/UiScreens/Public",
    ];

    // 그래픽 계층 이름공간(rocks::graphics·rocks::render)의 사용·선언과 그 모듈 헤더 include다.
    private static readonly Regex UiGraphicsName = Pattern(
        "\\b(?:graphics|render)\\s*::|\\bnamespace\\s+(?:rocks\\s*::\\s*)?(?:graphics|render)\\b|" +
        "#\\s*include\\s*<\\s*(?:Graphics|RenderBase|RenderGraph|UiRender)/");

    private const string UiRenderBoundaryRule = "ui-render-boundary";

    // UI Element·Widget 층 모듈의 빌드 규칙 파일이다(의존 목록에 그래픽 모듈이 들어가면 경계가 무너진다).
    private static readonly string[] UiElementLayerRules =
    [
        "Engine/Source/Runtime/UiCore/UiCore.Build.cs",
        "Engine/Source/Runtime/UiFramework/UiFramework.Build.cs",
        "Engine/Source/Runtime/UiWidget/UiWidget.Build.cs",
        "Engine/Source/Runtime/UiScreens/UiScreens.Build.cs",
    ];

    // 의존 목록의 그래픽 계층 모듈 이름(문자열 리터럴)이다.
    private static readonly Regex UiGraphicsModule = Pattern("\"(?:Graphics|RenderBase|RenderGraph|UiRender)\"");

    /// <summary>
    /// UI Element·Widget 층 공개 헤더에 그래픽 이름이 없는지 본다(rule id ui-render-boundary, UiToolkit 18.2절). 앞 선언도 막는다
    /// (지금까지 앞 선언만으로 경계를 넘었다 — 18.1절). 주석·문자열은 지운 뒤 찾는다(설명 주석에 계층 이름을 적는 것은 허용).
    /// </summary>
    public static IReadOnlyList<CheckFailure> UiRenderBoundary(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in Files(root, UiElementLayerPublicRoots))
        {
            if (!path.EndsWith(".h", StringComparison.Ordinal))
            {
                continue;
            }

            string code = WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            int line = 0;
            foreach (string text in code.Split('\n'))
            {
                ++line;
                if (UiGraphicsName.IsMatch(text))
                {
                    failures.Add(new(Relative(root, path), line, UiRenderBoundaryRule,
                        "UI element and widget layer public headers must not name graphics or render types; pass draws through " +
                        "UiCore UiRenderer (Docs/Design/UiToolkit 18.2)"));
                }
            }
        }

        // 모듈 의존: 문자열 리터럴로 쓰는 모듈 이름을 찾으므로 주석만 지운다(의존 목록은 문자열이다).
        foreach (string rules in UiElementLayerRules)
        {
            string path = Path.Combine(root, rules);
            if (!File.Exists(path))
            {
                continue;
            }

            int line = 0;
            foreach (string text in File.ReadAllText(path, StrictUtf8).Split('\n'))
            {
                ++line;
                string code = text.Contains("//", StringComparison.Ordinal) ? text[..text.IndexOf("//", StringComparison.Ordinal)] : text;
                if (UiGraphicsModule.IsMatch(code))
                {
                    failures.Add(new(rules, line, UiRenderBoundaryRule,
                        "UI element and widget layer modules must not depend on graphics or render modules; only UiRender and " +
                        "UiWorld link the render lane (Docs/Design/UiToolkit 18.2)"));
                }
            }
        }

        return failures;
    }
}
