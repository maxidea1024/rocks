using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 변경 통지 flag의 모듈 사이 판정이다(Docs/Design/ChangeNotification.ko.md CN-D1): 기반 모듈의 Notify 필드·함수는 descriptor
// schema 9의 flag로 전해지고, 파생 모듈이 상속 필드를 schema에 다시 실을 때 flag를 잃지 않는다. reader는 EditorOnly 필드의 Notify와 Pure const가
// 아닌 함수의 Notify를 거부한다.
internal static partial class ModuleMetaTests
{
    private const string NotifyLower =
        "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT()\nstruct ROCKS_LOWER_API Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
        "ROCKS_CLASS()\nclass ROCKS_LOWER_API Base : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "ROCKS_PROPERTY(Notify) rocks::int32 shared = 0;\n" +
        "ROCKS_FUNCTION(Pure, Notify) rocks::int32 Count() const noexcept { return shared; }\n};\n}\n";

    private static void RegisterNotify(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.NotifyAcrossModules", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", NotifyLower);
            layout.Manifest("Lower", ["probe::Leaf", "probe::Base"], ["probe::Leaf::count", "probe::Base::shared"], ["probe::Base::Count"]);
            ProcessResult lower = await layout.RunModuleAsync("Lower", "Lower/Parent.h", []);
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string original = File.ReadAllText(layout.Descriptor("Lower"));

            // 파생 모듈의 생성 schema가 상속 필드의 Notify flag(512)를 다시 싣는다(생성 소스는 --verify-sources로 컴파일된다).
            layout.Write("Upper/Public/Upper/Value.h",
                "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Lower/Parent.h>\nnamespace probe {\n" +
                "ROCKS_CLASS()\nclass Derived : public Base {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY(Notify) rocks::int32 extra = 0;\n};\n}\n");
            layout.Manifest("Upper", ["probe::Derived"], ["probe::Derived::extra"], []);
            ProcessResult upper = await layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllText(layout.Descriptor("Upper"))))
            {
                JsonElement[] fields = [.. Type(descriptor, "probe::Derived").GetProperty("fields").EnumerateArray()];
                foreach (string name in new[] { "shared", "extra" })
                {
                    JsonElement field = fields.Single(field => field.GetProperty("name").GetString() == name);
                    Verify.That(field.GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Notify"), name + " keeps Notify");
                }
            }

            string generated = Compact(layout.GeneratedText("Upper", "Public/Upper/Value.generated.h"));
            Verify.That(generated.Split("FieldKind::Value,512u").Length - 1 >= 2, "inherited and own Notify fields:\n" + generated);

            // descriptor reader: EditorOnly 필드의 Notify, Pure 아닌 함수의 Notify는 거부한다.
            foreach ((string what, string from, string to) in new[]
            {
                ("editor only field", "\"Notify\"", "\"EditorOnly\",\n            \"Notify\""),
                ("function without pure", "\"Pure\",", "")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + what);
                string mutated = layout.Generated("Mutated", what.Replace(' ', '_') + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)], imports: [mutated]),
                    "RMC1000", "invalid descriptor");
            }
        });
    }
}
