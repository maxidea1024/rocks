using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// self 시험 중 UI 렌더 경계 정책 검사다(Docs/Design/UiToolkit 18.2절 — ui-render-boundary). UI Element·Widget 층 공개 헤더가
// 그래픽 이름공간을 쓰거나 앞 선언하거나 그래픽 모듈 헤더를 include하면 거부하고, 주석·문자열·다른 모듈·Private 파일은 통과한다.
internal static partial class SelfTests
{
    private static void RegisterUiRenderBoundary(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryUiRenderBoundary", () => NoFailures(PolicyChecks.UiRenderBoundary(options.Root)));
        suite.Add("Policy.UiRenderBoundaryControls", () =>
        {
            using TemporaryDirectory temporary = new();

            // 정상: 설명 주석과 문자열의 계층 이름, UiRender 공개 헤더, Element 층의 Private 소스는 대상이 아니다.
            temporary.Write("Engine/Source/Runtime/UiCore/Public/UiCore/Probe.h", string.Join('\n',
                "#pragma once",
                "// The render lane records graphics::CommandContext passes (UiRender owns them).",
                "inline constexpr char Name[] = \"render::RenderLane\";",
                ""));
            temporary.Write("Engine/Source/Runtime/UiRender/Public/UiRender/Probe.h", "#include <Graphics/Device.h>\nclass graphics::Device;\n");
            temporary.Write("Engine/Source/Runtime/UiFramework/Private/Probe.cpp", "#include <RenderBase/RenderLane.h>\n");
            NoFailures(PolicyChecks.UiRenderBoundary(temporary.Root));

            void ExpectOne(string label, string file, string content, int line)
            {
                temporary.Write(file, content);
                var failures = PolicyChecks.UiRenderBoundary(temporary.Root);
                Verify.That(failures.Count == 1 && failures[0].Rule == "ui-render-boundary" && failures[0].Line == line &&
                    failures[0].Path == file, $"ui render boundary defect escaped audit: {label}\n{string.Join('\n', failures)}");
                File.Delete(temporary.FilePath(file));
            }

            ExpectOne("forward declaration", "Engine/Source/Runtime/UiCore/Public/UiCore/Bad.h",
                "#pragma once\nnamespace rocks::graphics {\nclass Device;\n}\n", 2);
            ExpectOne("nested namespace declaration", "Engine/Source/Runtime/UiWidget/Public/UiWidget/Bad.h",
                "#pragma once\nnamespace rocks {\nnamespace render {\n}\n}\n", 3);
            ExpectOne("qualified type use", "Engine/Source/Runtime/UiFramework/Public/UiFramework/Bad.h",
                "#pragma once\nvoid Draw(rocks::graphics :: CommandContext& commands);\n", 2);
            ExpectOne("graphics header include", "Engine/Source/Runtime/UiScreens/Public/UiScreens/Bad.h",
                "#pragma once\n#include <RenderGraph/RenderGraph.h>\n", 2);
            ExpectOne("ui render header include", "Engine/Source/Runtime/UiCore/Public/UiCore/Sub/Bad.h",
                "#pragma once\n\n# include <UiRender/UiLaneRenderer.h>\n", 3);

            // 모듈 의존: 주석의 이름은 통과하고, 의존 목록의 그래픽 모듈은 거부한다.
            temporary.Write("Engine/Source/Runtime/UiWidget/UiWidget.Build.cs", string.Join('\n',
                "// Draws go through UiRenderer; \"Graphics\" stays out.",
                "public sealed class UiWidget : EngineModuleRules { }",
                ""));
            NoFailures(PolicyChecks.UiRenderBoundary(temporary.Root));
            ExpectOne("graphics module dependency", "Engine/Source/Runtime/UiFramework/UiFramework.Build.cs",
                "public sealed class UiFramework : EngineModuleRules\n{\n    protected override string[] PublicDependencies => [\"UiCore\", \"RenderBase\"];\n}\n", 3);
        });
    }
}
