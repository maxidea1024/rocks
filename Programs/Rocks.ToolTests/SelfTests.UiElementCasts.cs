using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// self 시험 중 Element 하향 변환 정책 검사다(Docs/Design/UiToolkit 20.10절 CB2 — ui-element-cast). UI 모듈에서 Element
// 계열(상속으로 판정 — 이름에 Element가 없어도)로의 static_cast를 거부하고, Element 아닌 형식·주석·다른 모듈은 통과한다.
internal static partial class SelfTests
{
    private static void RegisterUiElementCasts(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryUiElementCasts", () => NoFailures(PolicyChecks.UiElementCasts(options.Root)));
        suite.Add("Policy.UiElementCastControls", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/Source/Runtime/UiFramework/Public/UiFramework/Probe.h", string.Join('\n',
                "#pragma once",
                "class ROCKS_UIFRAMEWORK_API Panel : public Element {",
                "};",
                "class ROCKS_UIFRAMEWORK_API ListView final : public Panel {",
                "};",
                "class Shape {",
                "};",
                ""));
            // 정상: Element 아닌 형식, 주석 안의 변환, UI 밖 모듈.
            temporary.Write("Engine/Source/Runtime/UiFramework/Private/Good.cpp", string.Join('\n',
                "auto& shape = static_cast<Shape&>(*value);",
                "// static_cast<ListView&>(element) is what we avoid",
                "auto& base = static_cast<Element&>(other);",
                ""));
            temporary.Write("Engine/Source/Runtime/World/Private/Other.cpp", "auto& list = static_cast<ListView&>(*x);\n");
            NoFailures(PolicyChecks.UiElementCasts(temporary.Root));

            void ExpectOne(string label, string file, string content, int line)
            {
                temporary.Write(file, content);
                var failures = PolicyChecks.UiElementCasts(temporary.Root);
                Verify.That(failures.Count == 1 && failures[0].Rule == "ui-element-cast" && failures[0].Line == line &&
                    failures[0].Path == file, $"ui element cast defect escaped audit: {label}\n{string.Join('\n', failures)}");
                File.Delete(temporary.FilePath(file));
            }

            ExpectOne("indirect element reference cast", "Engine/Source/Runtime/UiWidget/Private/Bad.cpp",
                "void F(Element& e) {\n    auto& list = static_cast<ListView&>(e);\n}\n", 2);
            ExpectOne("const pointer cast", "Programs/UiDemo/Private/Bad.cpp",
                "const auto* panel = static_cast<const Panel*>(element);\n", 1);
        });
    }
}
