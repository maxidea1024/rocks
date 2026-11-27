using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// ui-destructor-user-code 자체 시험이다(Docs/Design/UiToolkit 21.4절): 소멸자의 사용자 hook·delegate 호출을 거부하고,
// 엔진 정리·대기열에 넣는 lambda 안·소멸자 밖의 호출·UI 밖 모듈은 통과한다.
internal static partial class SelfTests
{
    private static void RegisterUiDestructors(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryUiDestructorUserCode", () => NoFailures(PolicyChecks.UiDestructorUserCode(options.Root)));
        suite.Add("Policy.UiDestructorUserCodeControls", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/Source/Runtime/UiFramework/Private/Good.cpp", string.Join('\n',
                "Panel::~Panel() noexcept {",
                "    DisconnectMenu();",
                "    (void)app_.DeferTeardown(Delegate<void()>::CreateLambda([this]() noexcept { screen->OnDeactivated(); }));",
                "}",
                "void Panel::Close() noexcept {",
                "    onClosed_.ExecuteIfBound();",
                "}",
                "// ~Panel() noexcept { OnDestruct(); }",
                ""));
            temporary.Write("Engine/Source/Runtime/World/Private/Other.cpp", "Actor::~Actor() noexcept {\n    OnDestruct();\n}\n");
            NoFailures(PolicyChecks.UiDestructorUserCode(temporary.Root));

            void ExpectOne(string label, string file, string content, int line)
            {
                temporary.Write(file, content);
                var failures = PolicyChecks.UiDestructorUserCode(temporary.Root);
                Verify.That(failures.Count == 1 && failures[0].Rule == "ui-destructor-user-code" && failures[0].Line == line &&
                    failures[0].Path == file, $"ui destructor user code escaped audit: {label}\n{string.Join('\n', failures)}");
                File.Delete(temporary.FilePath(file));
            }

            ExpectOne("out-of-line hook", "Engine/Source/Runtime/UiScreens/Private/Bad.cpp",
                "Container::~Container() noexcept {\n    for (auto& page : pages_) {\n        page->OnDeactivated();\n    }\n}\n", 3);
            ExpectOne("inline delegate", "Engine/Source/Runtime/UiWidget/Public/UiWidget/Bad.h",
                "class Root {\n    ~Root() noexcept override {\n        released_.ExecuteIfBound();\n    }\n};\n", 3);
            ExpectOne("execute through pointer", "Programs/UiDemo/Private/Bad.cpp",
                "Host::~Host() noexcept {\n    callback->Execute();\n}\n", 2);
        });
    }
}
