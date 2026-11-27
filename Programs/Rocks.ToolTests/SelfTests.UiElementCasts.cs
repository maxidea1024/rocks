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
                "class Mixed : public Shape, public Element {",
                "};",
                "struct Plain : protected Panel {",
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
            // 둘째 기반이 Element인 형식, struct·protected 상속, 이름공간 한정·뒤쪽 const.
            ExpectOne("second base is element", "Engine/Source/Runtime/UiCore/Private/Bad.cpp",
                "auto& mixed = static_cast<ui::Mixed&>(e);\n", 1);
            ExpectOne("protected struct base, trailing const", "Engine/Source/Runtime/UiCore/Private/Bad.cpp",
                "\nconst auto* plain = static_cast<::rocks::ui::Plain const*>(e);\n", 2);

            // 형식 판정이 이름으로 물러나므로 같은 이름의 ROCKS_UI_ELEMENT 선언은 거부한다(주석 안은 셈하지 않음).
            temporary.Write("Engine/Source/Runtime/UiFramework/Tests/FirstTests.cpp",
                "class Twin : public Element {\n    ROCKS_UI_ELEMENT(Twin, Element)\n};\n// ROCKS_UI_ELEMENT(Twin, Element)\n");
            NoFailures(PolicyChecks.UiElementCasts(temporary.Root));
            temporary.Write("Engine/Source/Runtime/UiWidget/Tests/SecondTests.cpp",
                "\nclass Twin : public Element {\n    ROCKS_UI_ELEMENT(Twin, Element)\n};\n");
            var twins = PolicyChecks.UiElementCasts(temporary.Root);
            Verify.That(twins.Count == 1 && twins[0].Rule == "ui-element-name" &&
                (twins[0].Line == 2 || twins[0].Line == 3),
                $"duplicate element type name escaped audit\n{string.Join('\n', twins)}");
        });
    }
}
