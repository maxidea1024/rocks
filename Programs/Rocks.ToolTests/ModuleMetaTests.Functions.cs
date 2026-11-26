using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 리플렉션 함수의 모듈 사이 판정이다(Docs/Design/FunctionReflection.ko.md FD10·FD14): 기반 모듈의 함수는 descriptor schema 5의
// functions로 전해지고(왕복), 파생 모듈의 함수 이름이 기반 체인의 함수 이름과 대소문자 무시로 겹치면 RMC2703이다. 생성 소스는 --verify-sources로
// 게시 전에 컴파일된다.
internal static partial class ModuleMetaTests
{
    private const string FunctionLower =
        "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n#include <Core/Text/String.h>\n" +
        "#include <Core/Result.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nclass ROCKS_LOWER_API Base : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "ROCKS_FUNCTION(Pure) rocks::int32 Count() const noexcept { return 0; }\n" +
        "ROCKS_FUNCTION(Name = \"Make\") static rocks::Result<rocks::String> Build(const rocks::String& label) noexcept;\n" +
        "#if ROCKS_WITH_EDITOR\nROCKS_FUNCTION(EditorOnly) void Inspect() noexcept;\n#endif\n};\n}\n";

    // Upper의 Derived(Base 파생) 본문 members를 둔 원본이다.
    private static string FunctionUpper(string members) =>
        "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Lower/Parent.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nclass Derived : public Base {\nROCKS_GENERATED_BODY()\npublic:\n" + members + "\n};\n}\n";

    // Sparks 노출 기반(ScriptExtensible)과 노출 함수 TryShoot(Sparks 이름 Shoot)를 둔 Lower다(SparksBinding B13·B15).
    private const string SparksFunctionLower =
        "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n#include <Core/Result.h>\nnamespace probe {\n" +
        "ROCKS_CLASS(ScriptExtensible)\nclass ROCKS_LOWER_API Base : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "ROCKS_FUNCTION(ScriptCallable) rocks::Result<void> TryShoot() noexcept;\n};\n}\n";

    private static void RegisterFunctions(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.SparksFunctionsAcrossModules", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", SparksFunctionLower);
            layout.Manifest("Lower", ["probe::Base"], [], ["probe::Base::TryShoot"]);
            ProcessResult lower = await layout.RunModuleAsync("Lower", "Lower/Parent.h", []);
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using (JsonDocument descriptor = JsonDocument.Parse(original))
            {
                JsonElement shoot = Type(descriptor, "probe::Base").GetProperty("functions")[0];
                Verify.Equal("Shoot", shoot.GetProperty("scriptName").GetString(), "descriptor carries the final Sparks name");
                Verify.That(shoot.GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "ScriptCallable"), "ScriptCallable flag");
            }

            // 다른 이름의 노출 멤버, 노출하지 않은 같은 이름 함수는 받는다.
            layout.Write("Upper/Public/Upper/Value.h", FunctionUpper("ROCKS_FUNCTION(ScriptCallable) void Aim() noexcept;\nROCKS_FUNCTION() void Shoot() noexcept;"));
            layout.Manifest("Upper", ["probe::Derived"], [], ["probe::Derived::Aim", "probe::Derived::Shoot"]);
            ProcessResult upper = await layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            // import 기반의 노출 함수 Sparks 이름과 겹치는 노출 필드·함수(RMC2605).
            layout.Manifest("Upper", [], ["probe::Derived::shoot"], []);
            foreach ((string what, string member) in new[] { ("function", "ROCKS_FUNCTION(ScriptCallable) void Shoot() noexcept;"),
                ("field", "ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 shoot = 0;") })
            {
                layout.Write("Upper/Public/Upper/Value.h", FunctionUpper(member));
                await Reject(layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]), "RMC2605", "Shoot");
            }

            // descriptor reader: 노출 flag와 scriptName은 함께 있다.
            foreach ((string what, string from, string to) in new[]
            {
                ("name without flag", "\"ScriptCallable\"", "\"ReturnsResult\""),
                ("flag without name", "\"scriptName\": \"Shoot\"", "\"scriptName\": \"\""),
                ("bad name", "\"scriptName\": \"Shoot\"", "\"scriptName\": \"shoot\""),
                ("try prefix", "\"scriptName\": \"Shoot\"", "\"scriptName\": \"TryShoot\""),
                ("hidden type", "\"ScriptType\",", "")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + what);
                string mutated = layout.Generated("Mutated", what.Replace(' ', '_') + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                layout.Write("Upper/Public/Upper/Value.h", FunctionUpper("ROCKS_FUNCTION() void Touch() noexcept;"));
                await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)], imports: [mutated]),
                    "RMC1000", "invalid descriptor");
            }
        });

        suite.Add("Meta.FunctionsAcrossModules", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", FunctionLower);
            layout.Manifest("Lower", ["probe::Base"], [], ["probe::Base::Count", "probe::Base::Build", "probe::Base::Inspect"]);
            ProcessResult lower = await layout.RunModuleAsync("Lower", "Lower/Parent.h", []);
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower"))))
            {
                JsonElement[] functions = [.. Type(descriptor, "probe::Base").GetProperty("functions").EnumerateArray()];
                Verify.That(functions.Select(function => function.GetProperty("name").GetString()).SequenceEqual(["Count", "Build", "Inspect"]),
                    "descriptor functions");
                JsonElement build = functions[1];
                Verify.Equal("Make", build.GetProperty("toolName").GetString(), "tool name");
                Verify.That(build.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString()).SequenceEqual(["Static", "ReturnsResult"]), "flags");
                Verify.Equal("label", build.GetProperty("parameters")[0].GetProperty("name").GetString(), "parameter name");
                Verify.Equal("Utf8String", build.GetProperty("result").GetProperty("value").GetProperty("kind").GetString(), "result kind");
            }

            // 기반 함수와 다른 이름의 함수는 받는다(import descriptor를 읽는다).
            layout.Write("Upper/Public/Upper/Value.h", FunctionUpper("ROCKS_FUNCTION() void Touch() noexcept;"));
            layout.Manifest("Upper", ["probe::Derived"], [], ["probe::Derived::Touch"]);
            ProcessResult upper = await layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            // 기반 모듈 함수의 C++ 이름(대소문자 무시)과 겹친다. EditorOnly 기반 함수도 같은 이름 공간이다.
            foreach ((string name, string member) in new[] { ("count", "ROCKS_FUNCTION() void count() noexcept;"),
                ("inspect", "ROCKS_FUNCTION() void inspect() noexcept;"), ("make", "ROCKS_FUNCTION() void make() noexcept;") })
            {
                layout.Write("Upper/Public/Upper/Value.h", FunctionUpper(member));
                layout.Manifest("Upper", [], [], ["probe::Derived::" + name]);
                await Reject(layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]), "RMC2703", name);
            }

            // descriptor reader는 함수 항목을 엄격히 읽는다.
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            foreach ((string what, string from, string to) in new[]
            {
                ("function flag", "\"Static\"", "\"Virtual\""),
                ("function key", "\"toolName\": \"Make\"", "\"toolName\": \"Make\",\n          \"extra\": 1"),
                ("tool name", "\"toolName\": \"Make\"", "\"toolName\": \"1bad\""),
                ("result kind", "\"kind\": \"Utf8String\"", "\"kind\": \"StaticArray\""),
                ("static const", "\"Static\",", "\"Static\",\n            \"Const\","),
                ("pure without result", "\"EditorOnly\"", "\"Pure\",\n            \"EditorOnly\""),
                ("same name", "\"name\": \"Inspect\"", "\"name\": \"Count\""),
                ("same tool name", "\"toolName\": \"Inspect\"", "\"toolName\": \"count\"")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + what);
                string mutated = layout.Generated("Mutated", what.Replace(' ', '_') + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                layout.Write("Upper/Public/Upper/Value.h", FunctionUpper("ROCKS_FUNCTION() void Touch() noexcept;"));
                await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)], imports: [mutated]),
                    "RMC1000", "invalid descriptor");
            }
        });
    }
}
