using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// self 시험 중 CVar 등록 정책 검사다(Docs/Design/CVarSystem.ko.md 5·19절 — cvar-registration·cvar-macro). 같은 모듈의
// Register 인자에 나오는 핸들은 통과하고, 빠뜨린 핸들·함수 본문 안의 핸들·다른 모듈의 등록·CVar 소스의 #define은 거부한다.
internal static partial class SelfTests
{
    private const string CVarModuleRule = "public sealed class Probe : CoreModuleRules { }\n";

    private static void RegisterCVarRegistration(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryCVarRegistrations", () => NoFailures(PolicyChecks.CVarRegistrations(options.Root)));
        suite.Add("Policy.CVarRegistrationControls", () =>
        {
            using TemporaryDirectory temporary = new();
            const string module = "Engine/Source/Runtime/Probe/";
            temporary.Write(module + "Probe.Build.cs", CVarModuleRule);

            // namespace 범위·클래스 밖 static 멤버 정의·클래스 안 inline static 멤버·묶음·명령 핸들이 같은 모듈의 다른 파일에서
            // (한정 이름으로) 등록되면 통과한다. 주석·문자열 안의 정의, 람다 본문, 시험 소스의 지역·미등록 핸들은 보지 않는다.
            temporary.Write(module + "Private/Vars.cpp", string.Join('\n',
                "namespace rocks::probe {",
                "CVar<float> ShadowDistance = cvar::Var(\"render.shadow.distance\", 8000.f)",
                "    .Help(\"Max\").Validate([](const float& value) noexcept -> StringView { return {}; });",
                "BootCVar<Array<int32>> Odd = cvar::Boot(\"render.odd\", 1).Help(\"x\");",
                "struct Holder {",
                "    static CVar<int32> Member;",
                "    static inline CVar<bool> Inline = cvar::Var(\"render.inline\", true).Help(\"x\");",
                "};",
                "CVar<int32> Holder::Member = cvar::Var(\"render.member\", 1).Help(\"x\");",
                "CVarGroup ShadowGroup = cvar::Group(\"quality.shadow\", 2).Help(\"x\");",
                "ConsoleCommand Teleport = cvar::Command(\"cheat.teleport\", &Run).Help(\"x\");",
                "// CVar<int32> Commented = cvar::Var(\"render.c\", 1);",
                "const char* text = \"CVar<int32> Quoted = 1;\";",
                "bool same = Holder::Member == Holder::Member;",
                "} // namespace rocks::probe",
                ""));
            temporary.Write(module + "Private/Register.cpp", string.Join('\n',
                "namespace rocks::probe {",
                "Result<void> RegisterProbeVars(ConfigContext& config) noexcept {",
                "    return config.Register(probe::ShadowDistance, Odd, Holder::Member, Holder::Inline,",
                "                           ShadowGroup, Teleport);",
                "}",
                "} // namespace rocks::probe",
                ""));
            temporary.Write("Engine/Source/Runtime/Probe/Tests/ProbeTests.cpp",
                "CVar<int32> Unregistered = cvar::Var(\"render.t\", 1).Help(\"x\");\n" +
                "void Test() { CVar<int32> local = cvar::Var(\"render.l\", 1).Help(\"x\"); }\n");
            NoFailures(PolicyChecks.CVarRegistrations(temporary.Root));

            // 빠뜨린 등록, 함수 본문(멤버 함수·함수 안 static 포함) 안의 정의, 다른 모듈의 Register는 위반이다.
            string[] rejected =
            [
                "CVar<int32> Missing = cvar::Var(\"render.missing\", 1).Help(\"x\");",
                "void Setup() { CVar<int32> Local = cvar::Var(\"render.local\", 1).Help(\"x\"); }",
                "void Setup() { static CVar<int32> Local = cvar::Var(\"render.local\", 1).Help(\"x\"); }",
                "struct S { void F() { BootCVar<int32> Local = cvar::Boot(\"render.local\", 1).Help(\"x\"); } };",
                "namespace a { ConsoleCommand Other = cvar::Command(\"cheat.other\", &Run).Help(\"x\"); }",
            ];
            temporary.Write("Engine/Source/Runtime/Other/Other.Build.cs", CVarModuleRule);
            temporary.Write("Engine/Source/Runtime/Other/Private/Register.cpp",
                "Result<void> R(ConfigContext& c) noexcept { return c.Register(Missing, Other); }\n");
            foreach (string source in rejected)
            {
                temporary.Write(module + "Private/Extra.cpp", "// first line\n" + source + "\n");
                var failures = PolicyChecks.CVarRegistrations(temporary.Root);
                Verify.That(failures.Count == 1 && failures[0].Line == 2 &&
                    failures[0].ToString().Contains("cvar-registration", StringComparison.Ordinal),
                    $"CVar registration escaped audit: {source}\n{string.Join('\n', failures)}");
                temporary.Write(module + "Private/Extra.cpp", "// clean\n");
            }

            // CVar 구현 소스는 매크로를 정의하지 않는다(#pragma once·다른 지시문·주석 안 철자는 괜찮다).
            const string cvarHeader = "Core/Source/Runtime/Config/Public/Config/CVarProbe.h";
            temporary.Write("Core/Source/Runtime/Config/Config.Build.cs", CVarModuleRule);
            temporary.Write(cvarHeader, "#pragma once\n#include <Config/CVarDefinition.h>\n// #define X 1\n");
            NoFailures(PolicyChecks.CVarRegistrations(temporary.Root));
            temporary.Write(cvarHeader, "#pragma once\n#  define CVAR_HELPER(x) x\n");
            var macro = PolicyChecks.CVarRegistrations(temporary.Root);
            Verify.That(macro.Count == 1 && macro[0].Line == 2 && macro[0].ToString().Contains("cvar-macro", StringComparison.Ordinal),
                "#define in a CVar source escaped audit: " + string.Join('\n', macro));
        });
    }
}
