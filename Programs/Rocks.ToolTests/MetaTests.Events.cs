using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 이벤트 필드(ROCKS_PROPERTY(Params = "…") rocks::Event<…> — Docs/Design/EventReflection.ko.md EV2b, ED14·ED18)
// 시험이다: 받는 선언(인자 없음·값·const String&·struct·enum·ObjectHandle 인자, Transient·SaveGame·EditorOnly 블록)의 descriptor schema 9
// 항목·생성 schema·연산표와 두 조합 컴파일, 거부 사례마다 RMC2801–RMC2805 코드.
internal static partial class MetaTests
{
    private const string EventPreamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n" +
        "#include <Object/Event.h>\n#include <Core/Text/String.h>\n#include <Core/Text/StringView.h>\n#include <Core/Containers/Array.h>\n" +
        "namespace probe {\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off, On };\nROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n";

    // 이벤트 members를 둔 클래스 Value 하나의 원본이다(생성자가 이벤트마다 allocator를 넘긴다 — init은 그 초기화 목록).
    private static string EventValue(string members, string init, string after = "", string extra = "") =>
        EventPreamble + extra + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "explicit Value(rocks::Allocator& allocator) noexcept : " + init + " {}\nROCKS_PROPERTY() rocks::int32 count = 0;\n" +
        members + "\n};\n" + after + "}\n";

    private static void RegisterEvents(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.EventsGenerateSchemaAndOps", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(EventValue(
                "ROCKS_PROPERTY(Params = \"amount\") rocks::Event<rocks::int32> changed;\n" +
                "ROCKS_PROPERTY(Params = \"text, times\", Transient) rocks::Event<const rocks::String&, rocks::int32> named;\n" +
                "ROCKS_PROPERTY(SaveGame, Name = \"Ping\") rocks::Event<> pinged;\n" +
                "ROCKS_PROPERTY(Params = \"leaf,mode,who\") rocks::Event<const Leaf&, Mode, rocks::ObjectHandle> placed;\n" +
                EditorDataOpen + "ROCKS_PROPERTY(EditorOnly, Params = \"amount\") rocks::Event<rocks::int32> edited;\n#endif\n" +
                "private:\nROCKS_PROPERTY(Params = \"weak\") rocks::Event<rocks::WeakObjectRef<Value>> secret;",
                "changed(allocator), named(allocator), pinged(allocator), placed(allocator),\n" + EditorDataOpen + "edited(allocator),\n#endif\nsecret(allocator)"));
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor)))
            {
                Verify.Equal(9, descriptor.RootElement.GetProperty("schema").GetInt32(), "descriptor schema 9");
                JsonElement[] fields = [.. DescriptorType(descriptor, "probe::Value").GetProperty("fields").EnumerateArray()];
                JsonElement Field(string name) => fields.Single(field => field.GetProperty("name").GetString() == name);
                Verify.Equal("Value", Field("count").GetProperty("fieldKind").GetString(), "value field kind");
                foreach (string name in new[] { "changed", "named", "pinged", "placed", "edited", "secret" })
                {
                    Verify.Equal("Event", Field(name).GetProperty("fieldKind").GetString(), name + " is an event");
                    Verify.That(!Field(name).TryGetProperty("value", out _), name + " has no value type");
                }

                string[] Params(string name) => [.. Field(name).GetProperty("params").EnumerateArray()
                    .Select(slot => slot.GetProperty("name").GetString() + ":" + slot.GetProperty("value").GetProperty("kind").GetString() +
                        string.Concat(slot.GetProperty("flags").EnumerateArray().Select(flag => "/" + flag.GetString())))];
                Verify.That(Params("changed").SequenceEqual(["amount:Int32"]), "changed params: " + string.Join(",", Params("changed")));
                Verify.That(Params("named").SequenceEqual(["text:Utf8String", "times:Int32"]), "named params (trimmed names)");
                Verify.That(Params("pinged").Length == 0, "no params");
                Verify.That(Params("placed").SequenceEqual(["leaf:Struct", "mode:Enum", "who:WeakObjectReference/ObjectHandle"]),
                    "struct, enum and ObjectHandle params: " + string.Join(",", Params("placed")));
                Verify.That(Strings(Field("named").GetProperty("flags")).SequenceEqual(["Transient"]), "Transient event");
                Verify.That(Strings(Field("pinged").GetProperty("flags")).SequenceEqual(["SaveGame"]), "SaveGame event");
                Verify.Equal("Ping", Field("pinged").GetProperty("toolName").GetString(), "event tool name");
                Verify.That(Strings(Field("edited").GetProperty("flags")).SequenceEqual(["EditorOnly"]), "EditorOnly event");
            }

            string generated = Compact(File.ReadAllText(probe.Source));
            foreach ((string expected, string reason) in new[]
            {
                ("FieldKind::Event", "event field kind"),
                ("#include<Object/EventOps.h>", "event ops header"),
                ("detail::MakeEventOps<&::probe::Value::changed>(", "event ops entry"),
                ("detail::MakeEventOps<&::probe::Value::secret>(", "private event ops entry"),
                ("detail::EventFrameShape<decltype(::probe::Value::placed)>::Value.offsets[2]", "frame offset from the event shape"),
                ("static_cast<uint32>(ParameterFlags::ObjectHandle)", "ObjectHandle argument flag"),
                ("ops.events=", "event ops span"),
                ("ops.fields=Span<constRuntimeFieldOps>(fields,1)", "only the value field has value ops")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            foreach (bool editor in new[] { true, false })
            {
                ProcessResult compiled = await probe.CompileGeneratedAsync(editor: editor);
                Verify.Equal(0, compiled.ExitCode, $"generated events compile with editor {editor}: " + compiled.StdOut + compiled.StdErr);
            }
        });

        // 손자 클래스가 물려받은 이벤트(다른 헤더의 조부모 선언)의 연산표는 조부모의 NativeTypeKey를 쓴다 — 생성 소스가 직접 base뿐
        // 아니라 이벤트를 선언한 조상의 생성 헤더도 include해야 컴파일된다(UiWidget TileViewWidget → ListViewWidget → ListViewBaseWidget).
        suite.Add("Meta.InheritedEventFromAGrandparentHeaderCompiles", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            probe.WriteHeader("Root.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n#include <Object/Event.h>\n" +
                "namespace probe {\nROCKS_CLASS()\nclass Root : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "explicit Root(rocks::Allocator& allocator) noexcept : changed(allocator) {}\n" +
                "ROCKS_PROPERTY(Params = \"amount\") rocks::Event<rocks::int32> changed;\n};\n}\n");
            probe.WriteHeader("Middle.h", "#pragma once\n#include <Root.h>\nnamespace probe {\nROCKS_CLASS()\nclass Middle : public Root {\n" +
                "ROCKS_GENERATED_BODY()\npublic:\nexplicit Middle(rocks::Allocator& allocator) noexcept : Root(allocator) {}\n};\n}\n");
            await probe.GenerateAsync("#pragma once\n#include <Middle.h>\nnamespace probe {\nROCKS_CLASS()\nclass Leaf final : public Middle {\n" +
                "ROCKS_GENERATED_BODY()\npublic:\nexplicit Leaf(rocks::Allocator& allocator) noexcept : Middle(allocator) {}\n};\n}\n",
                extra: ["--input", probe.HeaderPath("Root.h"), "--include", "Root.h", "--input", probe.HeaderPath("Middle.h"), "--include", "Middle.h"]);
            string generated = Compact(File.ReadAllText(probe.Source));
            Verify.That(generated.Contains("detail::MakeEventOps<&::probe::Root::changed>(", StringComparison.Ordinal), "inherited event ops entry");
            Verify.That(generated.Contains("#include<Root.generated.h>", StringComparison.Ordinal), "grandparent generated header");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "inherited grandparent event compiles: " + compiled.StdOut + compiled.StdErr);
        });

        // 다른 헤더의 enum을 이벤트 인자로 쓰면 클래스 생성 헤더가 그 enum 생성 헤더를 include한다(Broadcast를 부르는 구현 TU는 클래스
        // 생성 헤더만 include한다). enum 생성 헤더가 보이지 않는 TU에서 배치를 계산하면 static_assert다(enum을 Opaque로 보는 inline
        // 배치가 링크되는 ODR을 막는다 — UiWidget CheckBoxWidget::OnStateChanged 등록 실패에서 발견).
        suite.Add("Meta.EventEnumArgumentNeedsItsGeneratedHeader", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            probe.WriteHeader("Tone.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Base/Types.h>\nnamespace probe {\n" +
                "ROCKS_ENUM()\nenum class Tone : rocks::uint8 { Low, High };\n}\n");
            await probe.GenerateAsync("#pragma once\n#include <Tone.h>\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "#include <Object/Event.h>\nnamespace probe {\nROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : changed(allocator) {}\n" +
                "ROCKS_PROPERTY(Params = \"tone\") rocks::Event<Tone> changed;\n};\n}\n",
                extra: ["--input", probe.HeaderPath("Tone.h"), "--include", "Tone.h"]);
            Verify.Contains("#include <Tone.generated.h>", File.ReadAllText(probe.Output), "class generated header includes the event enum's");
            const string layout = "\nauto rocksLayout = rocks::detail::FrameLayoutOf<probe::Tone>();\n";
            ProcessResult withClassHeader = await probe.CompileSourceAsync("#include <Probe.generated.h>" + layout);
            Verify.Equal(0, withClassHeader.ExitCode, "class generated header is enough: " + withClassHeader.StdOut + withClassHeader.StdErr);
            ProcessResult without = await probe.CompileSourceAsync("#include <Probe.h>\n#include <Object/FunctionBinding.h>" + layout);
            Verify.That(without.ExitCode != 0, "enum layout without its generated header compiled");
            Verify.Contains("must be a reflected enum", without.StdOut + without.StdErr, "enum layout static_assert");
        });

        // ROCKS_NATIVE 이벤트(RMC2801 — 저장·Sparks·도구가 보지 못한다).
        AddRejectGroup(context, "Meta.EventNeedsProperty", [
            EventValue("ROCKS_NATIVE() rocks::Event<rocks::int32> changed;", "changed(allocator)")],
            "RMC2801", "ROCKS_PROPERTY");
        // 자리(RMC2802): struct 멤버, 정적 배열, 이벤트 인자 안 이벤트, 함수 매개변수의 이벤트(컨테이너 안 이벤트는 컨테이너의 C++
        // 요구(이동)가 Clang 단계에서 먼저 막는다).
        AddRejectGroup(context, "Meta.EventPlacement", [
            EventPreamble + "ROCKS_STRUCT()\nstruct Holder {\nROCKS_PROPERTY(Params = \"amount\") rocks::Event<rocks::int32> changed;\n};\n}\n",
            EventValue("ROCKS_PROPERTY() rocks::Event<rocks::int32> many[2];",
                "many{rocks::Event<rocks::int32>(allocator), rocks::Event<rocks::int32>(allocator)}"),
            EventValue("ROCKS_PROPERTY(Params = \"inner\") rocks::Event<const rocks::Event<rocks::int32>&> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"amount\") rocks::Event<rocks::int32> changed;\n" +
                "ROCKS_FUNCTION() void Touch(const rocks::Event<rocks::int32>& event) noexcept;", "changed(allocator)")],
            "RMC2802", "Event");
        // 지정자 없는 이벤트 멤버(RMC2801 — 일반 RMC2000보다 먼저 알린다).
        AddRejectGroup(context, "Meta.EventWithoutSpecifier", [
            EventValue("rocks::Event<rocks::int32> changed;", "changed(allocator)")],
            "RMC2801", "ROCKS_PROPERTY");
        // Params(RMC2803): 수 불일치·빠짐, lowerCamel 아님, 겹침, 이벤트가 아닌 필드의 Params.
        AddRejectGroup(context, "Meta.EventParams", [
            EventValue("ROCKS_PROPERTY(Params = \"a,b\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY() rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"Amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"a_b\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"a,a\") rocks::Event<rocks::int32, rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"a\") rocks::String other;", "other(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"arga,argb,argc,argd,arge,argf,argg,argh,argi,argj,argk,argl,argm,argn,argo,argp,argq\") rocks::Event<rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32, rocks::int32> many;", "many(allocator)")],
            "RMC2803", "");
        // 인자 형식(RMC2804): 포인터, 강한 참조(const&).
        AddRejectGroup(context, "Meta.EventArgumentTypes", [
            EventValue("ROCKS_PROPERTY(Params = \"amount\") rocks::Event<rocks::int32*> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"target\") rocks::Event<const rocks::ObjectRef<Value>&> changed;", "changed(allocator)")],
            "RMC2804", "");
        // Event 자체의 인자 static_assert가 Clang 단계에서 먼저 막는 형식(RMC3000): StringView(값·참조), 비const 참조, 할당하는 값 복사
        // (String·Array — const&로 받는다).
        AddRejectGroup(context, "Meta.EventArgumentsCheckedByTheEventType", [
            EventValue("ROCKS_PROPERTY(Params = \"text\") rocks::Event<rocks::StringView> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"text\") rocks::Event<const rocks::StringView&> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"amount\") rocks::Event<rocks::int32&> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"text\") rocks::Event<rocks::String> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Params = \"values\") rocks::Event<rocks::Array<rocks::int32>> changed;", "changed(allocator)")],
            "RMC3000", "Event arguments");
        // 지정자(RMC2805): 값 필드 전용·Sparks 노출 지정자.
        AddRejectGroup(context, "Meta.EventSpecifiers", [
            EventValue("ROCKS_PROPERTY(ReadOnly, Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(Required, Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(TextExportTransient, Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(MaxElements = 4, Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(ScriptReadOnly, Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(ScriptWritable, Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(TextMapMergeByKey, Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(AssetBundles = \"Ui\", Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)"),
            EventValue("ROCKS_PROPERTY(UserConfig, Params = \"amount\") rocks::Event<rocks::int32> changed;", "changed(allocator)")],
            "RMC2805", "Event field");
        // 생성 static_assert가 막는 인자(RMC2804 — 생성 코드 검증 컴파일): 강한 참조를 담는 struct.
        AddRejectGroup(context, "Meta.EventArgumentsCheckedByGeneratedCode", [
            EventValue("ROCKS_PROPERTY(Params = \"holder\") rocks::Event<const Holder&> changed;", "changed(allocator)",
                "", "ROCKS_CLASS()\nstruct Node : public rocks::Object {};\nROCKS_STRUCT()\nstruct Holder {\nROCKS_PROPERTY() rocks::ObjectRef<Node> target;\n};\n")],
            "RMC2804", "event argument");
    }
}
