using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 Sparks binding schema의 이벤트(Docs/Design/SparksBinding.ko.md EI1c B20–B26) 시험이다: ScriptAssignable 이벤트의 schema 항목(이름 —
// 소문자 C++ 이름의 대문자 변환·ScriptName, 인자 표기 — 정수·string·노출 struct·노출 enum·WeakObjectRef<T> → T?·Soft<T>?·Array<T>, 인자 없음,
// 선언 순서, 노출하지 않은 이벤트 제외, fields·functions 뒤), descriptor schema 8의 ScriptAssignable flag·scriptName, 거부 사례마다
// RMC2601–RMC2608.
internal static partial class MetaTests
{
    private const string SparksEventPreamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n" +
        "#include <Object/Event.h>\n#include <Object/SoftObjectRef.h>\n#include <Core/Text/String.h>\n#include <Core/Containers/Array.h>\n#include <Core/Guid.h>\n" +
        "namespace rocks {\nROCKS_CLASS()\nclass Asset : public Object {};\n}\nnamespace probe {\n" +
        "ROCKS_ENUM(ScriptType)\nenum class Mode : rocks::uint8 { Off, On };\nROCKS_ENUM()\nenum class Secret : rocks::uint8 { A };\n" +
        "ROCKS_STRUCT(ScriptType)\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
        "ROCKS_CLASS(ScriptType)\nstruct Sound : public rocks::Asset {};\nROCKS_CLASS()\nstruct Plain : public rocks::Object {};\n";

    // 노출 클래스 Thing(확장 가능)에 이벤트 members를 둔 원본이다(생성자가 이벤트마다 allocator를 넘긴다 — init은 그 초기화 목록, 비우면
    // 이벤트가 없다).
    private static string SparksEventThing(string members, string init, string after = "", string extra = "") =>
        SparksEventPreamble + extra + "ROCKS_CLASS(ScriptExtensible)\nclass Thing : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "explicit Thing(rocks::Allocator& allocator) noexcept" + (init.Length == 0 ? "" : " : " + init) + " {\n(void)allocator;\n}\n" +
        members + "\n};\n" + after + "}\n";

    // 이벤트 하나(pinged)만 둔 원본이다.
    private static string SparksEventOne(string specifiers, string type = "rocks::Event<>", string name = "pinged", string extra = "") =>
        SparksEventThing($"ROCKS_PROPERTY({specifiers}) {type} {name};", name + "(allocator)", extra: extra);

    private static void RegisterSparksEvents(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.SparksBindingEvents", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(SparksEventThing(
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 count = 0;\n" +
                "ROCKS_FUNCTION(ScriptCallable) void Act() noexcept;\n" +
                "ROCKS_PROPERTY(Params = \"amount\", ScriptAssignable) rocks::Event<rocks::int32> scored;\n" +
                "ROCKS_PROPERTY(ScriptAssignable) rocks::Event<> pinged;\n" +
                "ROCKS_PROPERTY(Params = \"text, leaf, mode, who, sound, values\", ScriptAssignable, ScriptName = \"Placed\") " +
                "rocks::Event<const rocks::String&, const Leaf&, Mode, rocks::WeakObjectRef<Thing>, const rocks::SoftObjectRef<Sound>&, " +
                "const rocks::Array<rocks::int32>&> place_it;\n" +
                "ROCKS_PROPERTY(Params = \"amount\") rocks::Event<rocks::int32> hidden;\n" +
                "ROCKS_PROPERTY(Transient, SaveGame, ScriptAssignable) rocks::Event<> quiet;",
                "scored(allocator), pinged(allocator), place_it(allocator), hidden(allocator), quiet(allocator)"));
            using (JsonDocument schema = JsonDocument.Parse(File.ReadAllBytes(probe.BindingSchema)))
            {
                JsonElement thing = BindingType(schema, "Thing");
                Verify.That(thing.EnumerateObject().Select(property => property.Name).SequenceEqual(
                    ["name", "id", "kind", "scriptExtensible", "fields", "functions", "events"]), "events follow fields and functions");
                JsonElement[] events = [.. thing.GetProperty("events").EnumerateArray()];
                string Shape(JsonElement entry)
                {
                    Verify.Equal(32, entry.GetProperty("id").GetString()!.Length, "event ID");
                    string[] keys = [.. entry.EnumerateObject().Select(property => property.Name)];
                    Verify.That(keys.SequenceEqual(new[] { "name", "id", "parameters" }.Take(keys.Length)), "event key order: " + string.Join(",", keys));
                    return Compact(string.Join(",", entry.EnumerateObject().Where(property => property.Name != "id")
                        .Select(property => property.Name + "=" + property.Value.GetRawText())));
                }

                string[] shapes = [.. events.Select(Shape)];
                string[] expected =
                [
                    "name=\"Scored\",parameters=[{\"name\":\"amount\",\"type\":\"int32\"}]",
                    "name=\"Pinged\"",
                    "name=\"Placed\",parameters=[{\"name\":\"text\",\"type\":\"string\"},{\"name\":\"leaf\",\"type\":\"Leaf\"},{\"name\":\"mode\",\"type\":\"Mode\"}," +
                        "{\"name\":\"who\",\"type\":\"Thing?\"},{\"name\":\"sound\",\"type\":\"Soft<Sound>?\"},{\"name\":\"values\",\"type\":\"Array<int32>\"}]",
                    "name=\"Quiet\""
                ];
                Verify.That(shapes.SequenceEqual(expected.Select(Compact)), "events:\n" + string.Join("\n", shapes));
                Verify.That(BindingFields(thing).Select(field => field.Name).SequenceEqual(["Count"]), "events are not fields");
            }

            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor)))
            {
                Verify.Equal(10, descriptor.RootElement.GetProperty("schema").GetInt32(), "descriptor schema 10");
                JsonElement[] fields = [.. DescriptorType(descriptor, "probe::Thing").GetProperty("fields").EnumerateArray()];
                JsonElement Field(string name) => fields.Single(field => field.GetProperty("name").GetString() == name);
                Verify.Equal("Placed", Field("place_it").GetProperty("scriptName").GetString(), "descriptor scriptName from ScriptName");
                Verify.Equal("Scored", Field("scored").GetProperty("scriptName").GetString(), "descriptor scriptName from the C++ name");
                Verify.Equal("", Field("hidden").GetProperty("scriptName").GetString(), "unexposed event");
                Verify.That(Strings(Field("pinged").GetProperty("flags")).SequenceEqual(["ScriptAssignable"]), "ScriptAssignable flag");
                Verify.That(Strings(Field("quiet").GetProperty("flags")).SequenceEqual(["Transient", "SaveGame", "ScriptAssignable"]), "flag order");
                Verify.That(Strings(Field("hidden").GetProperty("flags")).Length == 0, "no flags");
            }

            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated code compiles: " + compiled.StdOut + compiled.StdErr);
        });

        // Sparks 대응이 없는 인자(RMC2601).
        AddRejectGroup(context, "Meta.SparksEventTypes", [
            SparksEventOne("Params = \"key\", ScriptAssignable", "rocks::Event<rocks::Guid>")],
            "RMC2601", "");
        // 노출하지 않은 인자 대상(RMC2602): ObjectHandle(rocks::Object), 비노출 클래스·enum.
        AddRejectGroup(context, "Meta.SparksEventTargets", [
            SparksEventOne("Params = \"who\", ScriptAssignable", "rocks::Event<rocks::ObjectHandle>"),
            SparksEventOne("Params = \"who\", ScriptAssignable", "rocks::Event<rocks::WeakObjectRef<Plain>>"),
            SparksEventOne("Params = \"kind\", ScriptAssignable", "rocks::Event<Secret>")],
            "RMC2602", "");
        // 지정자(RMC2603): 값 필드의 ScriptAssignable, 이벤트의 ScriptName 단독, ScriptAssignable과 ScriptReadOnly·ScriptWritable.
        AddRejectGroup(context, "Meta.SparksEventSpecifiers", [
            SparksEventThing("ROCKS_PROPERTY(ScriptAssignable) rocks::int32 value = 0;", ""),
            SparksEventOne("ScriptName = \"Go\""),
            SparksEventOne("ScriptAssignable, ScriptReadOnly"),
            SparksEventOne("ScriptAssignable, ScriptWritable")],
            "RMC2603", "");
        // 이름 형식(RMC2604): `_`가 든 이름, ScriptName 형식.
        AddRejectGroup(context, "Meta.SparksEventNames", [
            SparksEventOne("ScriptAssignable", name: "was_hit"),
            SparksEventOne("ScriptAssignable, ScriptName = \"go\"")],
            "RMC2604", "");
        // 한 이름 공간(RMC2605): 필드와 이벤트, 함수와 이벤트, 기반 이벤트와 파생 필드.
        AddRejectGroup(context, "Meta.SparksEventNameCollisions", [
            SparksEventThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 struck = 0;\nROCKS_PROPERTY(ScriptAssignable, ScriptName = \"Struck\") rocks::Event<> was_hit;",
                "was_hit(allocator)"),
            SparksEventThing("ROCKS_FUNCTION(ScriptCallable) void Pinged() noexcept;\nROCKS_PROPERTY(ScriptAssignable) rocks::Event<> pinged;", "pinged(allocator)"),
            SparksEventThing("ROCKS_PROPERTY(ScriptAssignable) rocks::Event<> pinged;\nROCKS_PROPERTY(ScriptAssignable, ScriptName = \"Pinged\") rocks::Event<> pinged2;",
                "pinged(allocator), pinged2(allocator)"),
            SparksEventThing("ROCKS_PROPERTY(ScriptAssignable) rocks::Event<> pinged;", "pinged(allocator)",
                "ROCKS_CLASS()\nclass Child : public Thing {\nROCKS_GENERATED_BODY()\npublic:\nexplicit Child(rocks::Allocator& allocator) noexcept : Thing(allocator) {}\n" +
                "ROCKS_PROPERTY(ScriptReadOnly, ScriptName = \"Pinged\") rocks::int32 other = 0;\n};\n")],
            "RMC2605", "");
        // 언어 이름(RMC2606): Changed(필드 변경 예약), hook 이름, 예약 이름, 키워드 인자.
        AddRejectGroup(context, "Meta.SparksEventReservedNames", [
            SparksEventOne("ScriptAssignable", name: "changed"),
            SparksEventOne("ScriptAssignable", name: "update"),
            SparksEventOne("ScriptAssignable, ScriptName = \"World\""),
            SparksEventOne("Params = \"open\", ScriptAssignable", "rocks::Event<rocks::int32>", "opened")],
            "RMC2606", "");
        // 노출할 수 없는 이벤트(RMC2607): EditorOnly 이벤트, asset 종류 타입의 이벤트.
        AddRejectGroup(context, "Meta.SparksEventCannotBeShown", [
            SparksEventThing("ROCKS_PROPERTY() rocks::Event<> pinged;\n" + EditorDataOpen +
                "ROCKS_PROPERTY(EditorOnly, ScriptAssignable) rocks::Event<> edited;\n#endif",
                "pinged(allocator)\n" + EditorDataOpen + ", edited(allocator)\n#endif\n"),
            SparksEventThing("", "", extra: "ROCKS_CLASS(ScriptType)\nclass Bell : public rocks::Asset {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "explicit Bell(rocks::Allocator& allocator) noexcept : rang(allocator) {}\nROCKS_PROPERTY(ScriptAssignable) rocks::Event<> rang;\n};\n"),
            SparksEventThing("", "", extra: "ROCKS_CLASS(EditorOnly)\nclass Note : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "explicit Note(rocks::Allocator& allocator) noexcept : rang(allocator) {}\nROCKS_PROPERTY(ScriptAssignable) rocks::Event<> rang;\n};\n")],
            "RMC2607", "");
        AddRejectGroup(context, "Meta.SparksEventOnHiddenType", [
            SparksEventThing("", "", extra: "ROCKS_CLASS()\nclass Quiet : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "explicit Quiet(rocks::Allocator& allocator) noexcept : rang(allocator) {}\nROCKS_PROPERTY(ScriptAssignable) rocks::Event<> rang;\n};\n")],
            "RMC2608", "not a Sparks type");
    }
}
