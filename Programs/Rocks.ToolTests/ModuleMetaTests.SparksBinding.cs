using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 Sparks binding의 모듈 사이 판정이다(Docs/Design/SparksBinding.ko.md B2·B5): import 기반의 노출·확장 가능은 descriptor
// schema 4의 flag로 전해지고(상속), 기반 모듈의 Sparks 필드 이름(scriptName 포함)과 파생 모듈의 필드 이름이 겹치면 RMC2605, 노출하지 않은
// import 타입을 필드 대상으로 쓰면 RMC2602다.
internal static partial class ModuleMetaTests
{
    private const string BindingLower =
        "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_ENUM(ScriptType)\nenum class Kind : rocks::uint8 { Small, Large };\n" +
        "ROCKS_CLASS()\nstruct ROCKS_LOWER_API Hidden : public rocks::Object {};\n" +
        "ROCKS_CLASS(ScriptExtensible)\nclass ROCKS_LOWER_API Base : public rocks::Object {\npublic:\n" +
        "ROCKS_PROPERTY(ScriptReadOnly, ScriptName = \"Total\") rocks::int32 count_ = 0;\n};\n}\n";

    // Upper의 Derived(Base 파생) 본문 members를 둔 원본이다.
    private static string BindingUpper(string members) =>
        "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Lower/Parent.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Derived : public Base {\n" + members + "\n};\n}\n";

    private static void RegisterSparksBinding(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.SparksBindingAcrossModules", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", BindingLower);
            layout.Manifest("Lower", ["probe::Kind", "probe::Hidden", "probe::Base"], ["probe::Base::count_"]);
            ProcessResult lower = await layout.RunModuleAsync("Lower", "Lower/Parent.h", []);
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);

            // import 기반에서 노출·확장 가능을 물려받고, import enum을 필드 타입으로 쓴다.
            layout.Write("Upper/Public/Upper/Value.h", BindingUpper("ROCKS_PROPERTY(ScriptReadOnly) Kind kind = Kind::Small;"));
            layout.Manifest("Upper", ["probe::Derived"], ["probe::Derived::kind"]);
            ProcessResult upper = await layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            using (JsonDocument schema = JsonDocument.Parse(File.ReadAllBytes(layout.BindingSchema("Upper"))))
            {
                JsonElement derived = schema.RootElement.GetProperty("types").EnumerateArray().Single();
                Verify.Equal("Derived", derived.GetProperty("name").GetString(), "derived type");
                Verify.Equal("Base", derived.GetProperty("base").GetString(), "imported base");
                Verify.That(derived.GetProperty("scriptExtensible").GetBoolean(), "inherited from an imported base");
                JsonElement field = derived.GetProperty("fields").EnumerateArray().Single();
                Verify.Equal("Kind", field.GetProperty("type").GetString(), "imported enum type");
            }

            using (JsonDocument schema = JsonDocument.Parse(File.ReadAllBytes(layout.BindingSchema("Lower"))))
            {
                string[] names = schema.RootElement.GetProperty("types").EnumerateArray().Select(type => type.GetProperty("name").GetString() ?? "").ToArray();
                Verify.That(names.SequenceEqual(["Base", "Kind"]), "lower exposes Base and Kind only");
            }

            // 기반 모듈 필드의 Sparks 이름(ScriptName "Total")과 겹친다.
            layout.Write("Upper/Public/Upper/Value.h", BindingUpper("ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 total = 0;"));
            layout.Manifest("Upper", [], ["probe::Derived::total"]);
            await Reject(layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]), "RMC2605", "Total");

            // 노출하지 않은 import 타입은 필드 대상이 될 수 없다.
            layout.Write("Upper/Public/Upper/Value.h", BindingUpper("ROCKS_PROPERTY(ScriptReadOnly) rocks::ObjectRef<Hidden> hidden;"));
            layout.Manifest("Upper", [], ["probe::Derived::hidden"]);
            await Reject(layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]), "RMC2602", "probe::Hidden");
        });
    }
}
