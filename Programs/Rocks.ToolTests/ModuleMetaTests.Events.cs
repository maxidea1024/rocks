using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 이벤트 필드의 모듈 사이 판정이다(Docs/Design/EventReflection.ko.md ED18): 기반 모듈의 이벤트는 descriptor schema 7의
// 이벤트 필드 항목(params — ObjectHandle flag 포함)으로 전해지고, 파생 모듈이 상속 이벤트를 schema·연산표에 다시 실어 C++ 철자 없이
// 생성한다(생성 소스는 --verify-sources로 게시 전에 컴파일된다). descriptor reader는 이벤트 항목을 엄격히 읽는다.
internal static partial class ModuleMetaTests
{
    private const string EventLower =
        "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n#include <Object/Event.h>\n" +
        "#include <Core/Text/String.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT()\nstruct ROCKS_LOWER_API Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
        "ROCKS_CLASS()\nclass ROCKS_LOWER_API Base : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "explicit Base(rocks::Allocator& allocator) noexcept : changed(allocator), placed(allocator) {}\n" +
        "ROCKS_PROPERTY(Params = \"amount\", Transient) rocks::Event<rocks::int32> changed;\n" +
        "ROCKS_PROPERTY(Params = \"leaf,who,text\") rocks::Event<const Leaf&, rocks::ObjectHandle, const rocks::String&> placed;\n};\n}\n";

    private const string EventUpper =
        "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Lower/Parent.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nclass Derived : public Base {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "explicit Derived(rocks::Allocator& allocator) noexcept : Base(allocator), extra(allocator) {}\n" +
        "ROCKS_PROPERTY(Params = \"amount\") rocks::Event<rocks::int32> extra;\n};\n}\n";

    private static void RegisterEvents(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.EventsAcrossModules", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", EventLower);
            layout.Manifest("Lower", ["probe::Leaf", "probe::Base"], ["probe::Leaf::count", "probe::Base::changed", "probe::Base::placed"], []);
            ProcessResult lower = await layout.RunModuleAsync("Lower", "Lower/Parent.h", []);
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using (JsonDocument descriptor = JsonDocument.Parse(original))
            {
                JsonElement placed = Type(descriptor, "probe::Base").GetProperty("fields").EnumerateArray()
                    .Single(field => field.GetProperty("name").GetString() == "placed");
                Verify.Equal("Event", placed.GetProperty("fieldKind").GetString(), "event field kind");
                Verify.Equal("ObjectHandle", placed.GetProperty("params")[1].GetProperty("flags")[0].GetString(), "ObjectHandle flag");
            }

            // 파생 모듈은 상속 이벤트를 다시 싣고 자기 이벤트를 더한다(생성 소스 컴파일까지).
            layout.Write("Upper/Public/Upper/Value.h", EventUpper);
            layout.Manifest("Upper", ["probe::Derived"], ["probe::Derived::extra"], []);
            ProcessResult upper = await layout.RunModuleAsync("Upper", "Upper/Value.h", ["Lower"]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr + upper.StdOut);
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper"))))
            {
                string[] events = [.. Type(descriptor, "probe::Derived").GetProperty("fields").EnumerateArray()
                    .Where(field => field.GetProperty("fieldKind").GetString() == "Event").Select(field => field.GetProperty("name").GetString() ?? "")
                    .Order(StringComparer.Ordinal)];
                Verify.That(events.SequenceEqual(["changed", "extra", "placed"]), "derived events: " + string.Join(",", events));
                JsonElement inherited = Type(descriptor, "probe::Derived").GetProperty("fields").EnumerateArray()
                    .Single(field => field.GetProperty("name").GetString() == "placed");
                Verify.Equal("ObjectHandle", inherited.GetProperty("params")[1].GetProperty("flags")[0].GetString(),
                    "the derived descriptor keeps the inherited ObjectHandle flag");
            }

            string generated = Compact(layout.GeneratedText("Upper", "Public/Upper/Value.generated.h"));
            Verify.That(generated.Contains("detail::MakeEventOps<&::probe::Base::placed>(", StringComparison.Ordinal), "inherited event ops");
            Verify.That(generated.Contains("static_cast<uint32>(ParameterFlags::ObjectHandle)", StringComparison.Ordinal),
                "inherited ObjectHandle argument flag");

            // descriptor reader: 이벤트 항목의 flag·인자 이름·인자 flag·키 집합.
            foreach ((string what, string from, string to) in new[]
            {
                ("value flag on event", "\"Transient\"", "\"ReadOnly\""),
                ("argument name", "\"name\": \"leaf\"", "\"name\": \"Leaf\""),
                ("argument flag", "\"ObjectHandle\"", "\"Other\""),
                ("extra key", "\"fieldKind\": \"Event\",", "\"fieldKind\": \"Event\", \"bitfield\": false,"),
                ("repeated argument name", "\"name\": \"text\"", "\"name\": \"leaf\"")
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

        // public 클래스의 이벤트 인자는 private struct를 쓸 수 없다(RMC2006 — 파생 모듈이 인자 schema와 연산표를 다시 낸다).
        suite.Add("Meta.PublicEventCannotUsePrivateStruct", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Write("Upper/Private/Upper/Secret.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
                "ROCKS_STRUCT()\nstruct Secret {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n");
            layout.Write("Upper/Public/Upper/Value.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n#include <Object/Event.h>\n" +
                "#include <Upper/Secret.h>\nnamespace probe {\nROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : changed(allocator) {}\n" +
                "ROCKS_PROPERTY(Params = \"secret\") rocks::Event<const Secret&> changed;\n};\n}\n");
            layout.Manifest("Upper", ["probe::Secret"], ["probe::Secret::count", "probe::Value::changed"]);
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false),
                (layout.Source("Upper/Private/Upper/Secret.h"), "Upper/Secret.h", true)]), "RMC2006", "private struct");
        });
    }
}
