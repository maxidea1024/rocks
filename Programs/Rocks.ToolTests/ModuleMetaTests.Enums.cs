using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 import enum과 enum descriptor 시험이다(Docs/Design/ReflectionFoundations.ko.md 3.4절).
internal static partial class ModuleMetaTests
{
    // import enum 필드·상속 import enum 필드·enum descriptor 엄격 읽기·private enum 가시성 시험이다.
    private static void RegisterEnums(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        // B3b: 의존 모듈의 ROCKS_ENUM을 descriptor로 import해 필드 타입으로 쓴다. 생성 헤더는 그 enum의 생성 헤더를
        // include하고, 의존 모듈 집계가 enum을 먼저 등록한다.
        suite.Add("Meta.ImportedEnumsResolveAsFieldTypes", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("namespace probe {\n",
                "namespace probe {\nROCKS_ENUM(ToolPrefix = \"MODE_\")\nenum class Mode : rocks::uint16 { Off = 0, On ROCKS_ENUMERATOR(Name = \"Enabled\") = 3 };\n",
                StringComparison.Ordinal));
            layout.Manifest("Lower", ["probe::Mode"], []);
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() Mode mode = Mode::Off;\n", StringComparison.Ordinal));
            layout.Manifest("Upper", [], ["probe::Value::mode"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string lowerAggregate = File.ReadAllText(layout.ModuleSource("Lower"));
            // 생성물은 clang-format을 거치므로 줄바꿈 위치에 기대지 않고 각 호출의 고유 부분을 찾는다.
            int enumRegistration = lowerAggregate.IndexOf("GeneratedType<::probe::Mode>::Schema()", StringComparison.Ordinal);
            int classRegistration = lowerAggregate.IndexOf("GeneratedType<::probe::Parent>::Schema()", StringComparison.Ordinal);
            Verify.That(enumRegistration >= 0 && classRegistration > enumRegistration, "module source registers enums first:\n" + lowerAggregate);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement mode = Type(lowerDescriptor, "probe::Mode");
            Verify.Equal("Enabled", mode.GetProperty("values")[1].GetProperty("toolName").GetString(), "imported enumerator tool name");
            JsonElement field = Type(upperDescriptor, "probe::Value").GetProperty("fields").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == "mode");
            Verify.Equal("Enum", field.GetProperty("value").GetProperty("kind").GetString(), "imported enum field kind");
            Verify.Equal(mode.GetProperty("id").GetString(), field.GetProperty("value").GetProperty("type").GetString(), "imported enum target");
            string generated = layout.GeneratedText("Upper", "Public/Upper/Value.generated.h");
            Verify.Contains("#include <Lower/Parent.generated.h>", generated, "imported enum generated header include");
        });

        // 의존 모듈 클래스의 enum 필드를 상속하면 생성 헤더가 그 enum의 생성 헤더를 include하고 native 키를 싣는다.
        suite.Add("Meta.InheritedImportedEnumFieldsGenerate", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("namespace probe {\n",
                "namespace probe {\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off, On };\n", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY() rocks::int32 value = 0;\n", "ROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() Mode level = Mode::Off;\n", StringComparison.Ordinal));
            layout.Manifest("Lower", ["probe::Mode"], ["probe::Parent::level"]);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            string generated = layout.GeneratedText("Upper", "Public/Upper/Value.generated.h");
            Verify.Contains("#include <Lower/Parent.generated.h>", generated, "inherited enum generated header include");
            Verify.Contains("NativeTypeKey<::probe::Mode>()", generated, "inherited enum field native key");
        });

        // descriptor의 enum 항목도 엄격하게 읽는다(런타임 RegisterEnum과 같은 규칙, 어기면 RMC1000).
        suite.Add("Meta.EnumDescriptorIsReadStrictly", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("namespace probe {\n",
                "namespace probe {\nROCKS_ENUM(ToolPrefix = \"MODE_\")\nenum class Mode : rocks::uint16 { Off = 0, On ROCKS_ENUMERATOR(Name = \"Enabled\") = 3 };\n",
                StringComparison.Ordinal));
            layout.Manifest("Lower", ["probe::Mode"], []);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            foreach ((string name, string from, string to) in new[]
            {
                ("underlying", "\"underlying\": \"UInt16\"", "\"underlying\": \"Float32\""),
                ("alias", "\"value\": 3", "\"value\": 0"),
                ("range", "\"value\": 3", "\"value\": 70000"),
                ("collision", "\"toolName\": \"Enabled\"", "\"toolName\": \"MODE_off\""),
                ("toolName", "\"toolName\": \"Enabled\"", "\"toolName\": \"has space\""),
                ("prefix", "\"toolPrefix\": \"MODE_\"", "\"toolPrefix\": \"MO-DE\""),
                ("extraKey", "\"toolPrefix\": \"MODE_\"", "\"toolPrefix\": \"MODE_\",\n      \"extra\": 1")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }
        });

        // 같은 모듈의 public 클래스는 private enum을 필드로 쓸 수 없다(public 집계는 private enum을 등록하지 않는다).
        suite.Add("Meta.PublicClassCannotUsePrivateEnum", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Write("Upper/Private/Upper/Secret.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
                "ROCKS_ENUM()\nenum class Secret : rocks::uint8 { A };\n}\n");
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("#include <Object/ObjectRefs.h>\n", "#include <Object/ObjectRefs.h>\n#include <Upper/Secret.h>\n", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n", "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() Secret secret = Secret::A;\n", StringComparison.Ordinal));
            layout.Manifest("Upper", ["probe::Secret"], ["probe::Value::secret"]);
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false),
                (layout.Source("Upper/Private/Upper/Secret.h"), "Upper/Secret.h", true)]), "RMC2006", "private enum");
        });
    }
}
