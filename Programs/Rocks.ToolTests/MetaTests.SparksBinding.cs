using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 Sparks binding schema 생성(Docs/Design/SparksBinding.ko.md EI1a) 시험이다: 노출 지정자의 상속·끊기, schema 종류·base·
// 필드 타입 표기(B4 표), 이름(B5), descriptor schema 4의 노출 flag(B2), 오류 코드 RMC2601–RMC2608(B10).
internal static partial class MetaTests
{
    // Sparks binding 원본의 머리다. rocks::Asset은 AssetRuntime 타입이지만 probe는 Core만 include하므로 같은 이름의 최소 선언을 둔다
    // (asset 종류 판정은 기반 사슬의 rocks::Asset 이름으로 한다 — B3).
    private const string BindingPreamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n" +
        "#include <Object/ObjectRefArray.h>\n#include <Object/SoftObjectRef.h>\n#include <Object/ClassRef.h>\n#include <Core/Text/String.h>\n" +
        "#include <Core/Text/Name.h>\n#include <Core/Containers/Array.h>\n#include <Core/Containers/OrderedMap.h>\n#include <Core/Containers/OrderedSet.h>\n" +
        "#include <Core/Optional.h>\n#include <Core/Guid.h>\n" +
        "namespace rocks {\nROCKS_CLASS()\nclass Asset : public Object {};\n}\nnamespace probe {\n";

    // 노출 클래스 Thing(확장 가능) 하나와 members를 둔 원본이다.
    private static string BindingThing(string members, string extra = "") =>
        BindingPreamble + "ROCKS_CLASS()\nstruct Plain : public rocks::Object {};\nROCKS_ENUM()\nenum class Secret : rocks::uint8 { A };\n" + extra +
        "ROCKS_CLASS(ScriptExtensible)\nstruct Thing : public rocks::Object {\n" + members + "\n};\n}\n";

    private static JsonElement BindingType(JsonDocument schema, string name) =>
        schema.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == name);

    // 필드 이름 → (type, access 또는 "")이다(선언 순서).
    private static (string Name, string Type, string Access)[] BindingFields(JsonElement type) =>
        type.TryGetProperty("fields", out JsonElement fields)
            ? fields.EnumerateArray().Select(field => (field.GetProperty("name").GetString() ?? "", field.GetProperty("type").GetString() ?? "",
                field.TryGetProperty("access", out JsonElement access) ? access.GetString() ?? "" : "")).ToArray()
            : [];

    private static string[] DescriptorFlags(JsonDocument descriptor, string name) =>
        descriptor.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == name)
            .GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();

    private static void RegisterSparksBinding(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        // 노출 지정자가 schema에 닿는다: 클래스·struct는 상속되고 NotScriptType·NotScriptExtensible이 끊으며, enum은 자기 것만, EditorOnly
        // 파생은 빠진다. 타입은 이름 byte 순, 필드는 자기 필드만 선언 순서, 표기는 B4 표대로다.
        suite.Add("Meta.SparksBindingSchemaFromSpecifiers", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(BindingPreamble +
                "ROCKS_ENUM(ScriptType)\nenum class Mood : rocks::uint8 { Calm, Angry, Count ROCKS_ENUMERATOR(Hidden) };\n" +
                "ROCKS_ENUM()\nenum class Secret : rocks::uint8 { A };\n" +
                "ROCKS_STRUCT(ScriptType)\nstruct Base {\nROCKS_PROPERTY(ScriptReadOnly) rocks::int32 count = 0;\nROCKS_PROPERTY() rocks::int32 hidden = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Leaf : public Base {\nROCKS_PROPERTY(ScriptWritable) float weight = 0;\n};\n" +
                "ROCKS_STRUCT(NotScriptType)\nstruct Cut : public Base {\nROCKS_PROPERTY() rocks::int32 extra = 0;\n};\n" +
                "ROCKS_CLASS(ScriptType)\nstruct Sound : public rocks::Asset {\n" +
                "explicit Sound(rocks::Allocator& a) noexcept : variant(a) {}\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::SoftObjectRef<Sound> variant;\n};\n" +
                "ROCKS_CLASS(ScriptExtensible)\nstruct Thing : public rocks::Object {\n" +
                "explicit Thing(rocks::Allocator& a) noexcept : label_(a), sound(a), others(a), names(a), scores(a), tags(a), byMood(a), sounds(a), leaves(a) {}\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) bool alive_ = true;\n" +
                "ROCKS_PROPERTY(ScriptWritable) rocks::int64 score = 0;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly, ScriptName = \"Title\") rocks::String label_;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) Mood mood = Mood::Calm;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::ObjectRef<Thing> next;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::WeakObjectRef<Thing> owner;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::SoftObjectRef<Sound> sound;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::ObjectRefArray<Thing> others;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::Array<rocks::String> names;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::OrderedMap<rocks::String, rocks::int32> scores;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::OrderedSet<rocks::int32> tags;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::Optional<rocks::int32> limit;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) Leaf leaf;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) double ratio = 0;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::int8 tiny = 0;\nROCKS_PROPERTY(ScriptReadOnly) rocks::int16 small = 0;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::uint16 wide = 0;\nROCKS_PROPERTY(ScriptReadOnly) rocks::uint32 big = 0;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::uint64 huge = 0;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::OrderedMap<Mood, rocks::String> byMood;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::Array<rocks::SoftObjectRef<Sound>> sounds;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::Optional<Mood> maybeMood;\n" +
                "ROCKS_PROPERTY(ScriptReadOnly) rocks::OrderedMap<rocks::String, Leaf> leaves;\n" +
                "ROCKS_PROPERTY() rocks::int32 secret = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Child : public Thing {\nusing Thing::Thing;\nROCKS_PROPERTY(ScriptReadOnly) rocks::uint8 level = 0;\n};\n" +
                "ROCKS_CLASS(NotScriptExtensible)\nstruct Sealed : public Thing {\nusing Thing::Thing;\n};\n" +
                "ROCKS_CLASS(NotScriptType)\nstruct Internal : public Thing {\nusing Thing::Thing;\n};\n" +
                "ROCKS_CLASS(ScriptType)\nstruct Again : public Internal {\nusing Internal::Internal;\n};\n" +
                "ROCKS_CLASS()\nstruct Plain : public rocks::Object {};\n" +
                "ROCKS_CLASS(EditorOnly)\nstruct Tool : public Thing {\nusing Thing::Thing;\n};\n}\n");
            string text = File.ReadAllText(probe.BindingSchema);
            Verify.That(text.EndsWith("}\n", StringComparison.Ordinal) && !text.Contains('\r') && !text.Contains("\\u003C", StringComparison.Ordinal),
                "LF, one final newline, unescaped type notation");
            Verify.Contains("\n  \"types\": [\n    {\n      \"name\": \"Again\",", text, "two-space indentation and the first type");
            using JsonDocument schema = JsonDocument.Parse(text);
            Verify.Equal(1, schema.RootElement.GetProperty("format").GetInt32(), "format");
            Verify.Equal("Rocks", schema.RootElement.GetProperty("package").GetString(), "package");
            Verify.Equal("Probe", schema.RootElement.GetProperty("module").GetString(), "module");
            string[] names = schema.RootElement.GetProperty("types").EnumerateArray().Select(type => type.GetProperty("name").GetString() ?? "").ToArray();
            Verify.That(names.SequenceEqual(["Again", "Base", "Child", "Leaf", "Mood", "Sealed", "Sound", "Thing"]),
                "exposed types by name (Asset, Cut, Internal, Plain, Secret and the EditorOnly Tool are hidden): " + string.Join(",", names));

            JsonElement thing = BindingType(schema, "Thing");
            Verify.Equal("object", thing.GetProperty("kind").GetString(), "object kind");
            Verify.That(!thing.TryGetProperty("base", out _), "rocks::Object is not a Sparks base");
            Verify.That(thing.GetProperty("scriptExtensible").GetBoolean(), "extensible");
            Verify.That(BindingFields(thing).SequenceEqual([("Alive", "bool", ""), ("Score", "int64", "readWrite"), ("Title", "string", ""),
                ("Mood", "Mood", ""), ("Next", "Thing?", ""), ("Owner", "Thing?", ""), ("Sound", "Soft<Sound>?", ""), ("Others", "Array<Thing>", ""),
                ("Names", "Array<string>", ""), ("Scores", "Map<string, int32>", ""), ("Tags", "Set<int32>", ""), ("Limit", "int32?", ""),
                ("Leaf", "Leaf", ""), ("Ratio", "float64", ""), ("Tiny", "int8", ""), ("Small", "int16", ""), ("Wide", "uint16", ""),
                ("Big", "uint32", ""), ("Huge", "uint64", ""), ("ByMood", "Map<Mood, string>", ""), ("Sounds", "Array<Soft<Sound>>", ""),
                ("MaybeMood", "Mood?", ""), ("Leaves", "Map<string, Leaf>", "")]), "Thing fields: " + string.Join(",", BindingFields(thing)));
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor)))
            {
                JsonElement field = descriptor.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == "probe::Thing")
                    .GetProperty("fields").EnumerateArray().Single(item => item.GetProperty("name").GetString() == "label_");
                Verify.That(field.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString()).SequenceEqual(["ScriptReadOnly"]), "field script flag");
                Verify.Equal("Title", field.GetProperty("scriptName").GetString(), "descriptor scriptName");
                Verify.That(DescriptorFlags(descriptor, "probe::Thing").SequenceEqual(["ScriptType", "ScriptExtensible"]), "Thing flags");
                Verify.That(DescriptorFlags(descriptor, "probe::Child").SequenceEqual(["ScriptType", "ScriptExtensible"]), "inherited flags");
                Verify.That(DescriptorFlags(descriptor, "probe::Sealed").SequenceEqual(["ScriptType"]), "NotScriptExtensible stops extension");
                Verify.That(DescriptorFlags(descriptor, "probe::Internal").SequenceEqual(["ScriptExtensible"]),
                    "NotScriptType stops exposure; extension keeps flowing (UE Blueprintable)");
                Verify.That(DescriptorFlags(descriptor, "probe::Tool").SequenceEqual(["EditorOnly"]), "EditorOnly types are not exposed by inheritance");
                Verify.That(DescriptorFlags(descriptor, "probe::Leaf").SequenceEqual(["ScriptType"]), "struct exposure is inherited");
                Verify.That(DescriptorFlags(descriptor, "probe::Cut").Length == 0, "NotScriptType on a struct");
                Verify.That(DescriptorFlags(descriptor, "probe::Mood").SequenceEqual(["ScriptType"]), "enum flag");
                Verify.That(DescriptorFlags(descriptor, "probe::Secret").Length == 0, "hidden enum");
            }

            JsonElement child = BindingType(schema, "Child");
            Verify.Equal("Thing", child.GetProperty("base").GetString(), "exposed base");
            Verify.That(child.GetProperty("scriptExtensible").GetBoolean(), "inherited extensible");
            Verify.That(BindingFields(child).SequenceEqual([("Level", "uint8", "")]), "own fields only");
            JsonElement sealedType = BindingType(schema, "Sealed");
            Verify.That(!sealedType.TryGetProperty("scriptExtensible", out _) && sealedType.GetProperty("base").GetString() == "Thing", "sealed");
            JsonElement again = BindingType(schema, "Again");
            Verify.That(!again.TryGetProperty("base", out _), "the exposure chain is cut below NotScriptType");
            Verify.That(again.GetProperty("scriptExtensible").GetBoolean(), "extension is inherited through a NotScriptType class (UE Blueprintable)");
            JsonElement sound = BindingType(schema, "Sound");
            Verify.Equal("asset", sound.GetProperty("kind").GetString(), "asset kind from the rocks::Asset chain");
            Verify.That(!sound.TryGetProperty("base", out _), "rocks::Asset is not a Sparks base");
            JsonElement baseStruct = BindingType(schema, "Base");
            Verify.Equal("struct", baseStruct.GetProperty("kind").GetString(), "struct kind");
            Verify.That(!baseStruct.GetProperty("constructible").GetBoolean(), "engine structs are not constructible yet");
            Verify.That(BindingFields(baseStruct).SequenceEqual([("Count", "int32", "")]), "struct fields");
            JsonElement leaf = BindingType(schema, "Leaf");
            Verify.Equal("Base", leaf.GetProperty("base").GetString(), "struct base");
            Verify.That(BindingFields(leaf).SequenceEqual([("Weight", "float32", "readWrite")]), "struct own fields");
            JsonElement mood = BindingType(schema, "Mood");
            Verify.Equal("enum", mood.GetProperty("kind").GetString(), "enum kind");
            Verify.That(mood.GetProperty("cases").EnumerateArray().Select(item => item.GetString()).SequenceEqual(["Calm", "Angry"]), "hidden case removed");
            Verify.Equal(mood.GetProperty("id").GetString(), JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor)).RootElement.GetProperty("types")
                .EnumerateArray().Single(type => type.GetProperty("name").GetString() == "probe::Mood").GetProperty("id").GetString(), "TypeId");
        });

        // --check는 binding schema도 대조한다(다르면 RMC4000이고 파일을 고치지 않는다).
        suite.Add("Meta.SparksBindingSchemaIsChecked", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 count = 0;");
            await probe.GenerateAsync(source);
            await probe.GenerateAsync(source, extra: ["--check"]);
            File.WriteAllText(probe.BindingSchema, "stale\n");
            await probe.GenerateAsync(source, extra: ["--check"], expectedCode: "RMC4000", reason: "binding schema is stale");
            Verify.Equal("stale\n", File.ReadAllText(probe.BindingSchema), "stale check overwrote the binding schema");
        });

        // 노출 타입이 없는 모듈도 빈 types를 쓴다(빌드 출력 목록이 지정자에 따라 바뀌지 않게 — B2).
        suite.Add("Meta.SparksBindingSchemaForModuleWithoutExposedTypes", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(Preamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n");
            Verify.Equal("{\n  \"format\": 1,\n  \"package\": \"Rocks\",\n  \"module\": \"Probe\",\n  \"types\": []\n}\n", File.ReadAllText(probe.BindingSchema),
                "empty binding schema");
        });

        // 수학 미러(CoreTypeBindings)는 Sparks 내장·prelude 타입으로 보인다(B4). 미러 자신에 ScriptType은 RMC2607이다.
        suite.Add("Meta.SparksBindingMathMirrors", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler, BindsModule);
            await probe.GenerateAsync(BindsPreamble +
                "ROCKS_STRUCT(Binds = \"rocks::Vector3<double>\")\nstruct Vec3d {\nROCKS_PROPERTY() double x = 0;\nROCKS_PROPERTY() double y = 0;\nROCKS_PROPERTY() double z = 0;\n};\n" +
                "ROCKS_CLASS(ScriptType)\nstruct Value : public rocks::Object {\nROCKS_PROPERTY(ScriptReadOnly) rocks::Vector3<double> where;\n};\n}\n");
            using JsonDocument schema = JsonDocument.Parse(File.ReadAllBytes(probe.BindingSchema));
            Verify.That(BindingFields(BindingType(schema, "Value")).SequenceEqual([("Where", "vec3", "")]), "Vector3<double> is vec3");
            Verify.That(schema.RootElement.GetProperty("types").GetArrayLength() == 1, "the mirror itself is not a Sparks type");
            await probe.RejectAsync(BindsPreamble +
                "ROCKS_STRUCT(Binds = \"rocks::Vector3<double>\", ScriptType)\nstruct Vec3d {\nROCKS_PROPERTY() double x = 0;\nROCKS_PROPERTY() double y = 0;\nROCKS_PROPERTY() double z = 0;\n};\n}\n",
                "RMC2607", "mirror");
        });

        // RMC2601: Sparks에 대응 타입이 없는 필드(Name·ClassRef·Guid 계열·중첩 Optional은 리플렉션에 없다)다.
        AddRejectGroup(context, "Meta.SparksBindingRejectsUnsupportedFieldTypes", [
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::Name label;"),
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::ClassRef<Thing> kind;"),
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::Guid key;"),
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 grid[3] = {};")],
            "RMC2601", "cannot be shown to Sparks");
        // RMC2602: 노출하지 않은 대상 타입(클래스·enum·rocks::Object)과 asset이 아닌 Soft 대상이다.
        AddRejectGroup(context, "Meta.SparksBindingRejectsHiddenTargets", [
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::ObjectRef<Plain> plain;"),
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) Secret secret = Secret::A;"),
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::ObjectRef<rocks::Object> any;"),
            BindingThing("explicit Thing(rocks::Allocator& a) noexcept : self(a) {}\nROCKS_PROPERTY(ScriptReadOnly) rocks::SoftObjectRef<Thing> self;")],
            "RMC2602", "not");
        // RMC2603: 함께 쓸 수 없는 노출 지정자다.
        AddRejectGroup(context, "Meta.SparksBindingRejectsConflictingSpecifiers", [
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly, ScriptWritable) rocks::int32 count = 0;"),
            BindingThing("ROCKS_PROPERTY(ReadOnly, ScriptWritable) rocks::int32 count = 0;"),
            BindingThing("ROCKS_PROPERTY(ScriptName = \"Total\") rocks::int32 count = 0;"),
            BindingThing("", "ROCKS_CLASS(ScriptType, NotScriptType)\nstruct Both : public rocks::Object {};\n"),
            BindingThing("", "ROCKS_CLASS(ScriptExtensible, NotScriptExtensible)\nstruct Both : public rocks::Object {};\n"),
            BindingThing("", "ROCKS_CLASS(ScriptExtensible, NotScriptType)\nstruct Both : public rocks::Object {};\n")],
            "RMC2603", "");
        // RMC2604: Sparks 이름 형식(필드의 `_`·ScriptName·enum case·점이 든 타입 도구 이름)이다.
        AddRejectGroup(context, "Meta.SparksBindingRejectsBadNames", [
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 max_count = 0;"),
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly, ScriptName = \"total\") rocks::int32 count = 0;"),
            BindingThing("", "ROCKS_ENUM(ScriptType)\nenum class Mode : rocks::uint8 { first };\n"),
            BindingThing("", "ROCKS_CLASS(ScriptType, Name = \"Weapons.Sword\")\nstruct Sword : public rocks::Object {};\n")],
            "RMC2604", "");
        // RMC2605: 보이는 사슬 안의 Sparks 필드 이름 겹침이다.
        AddRejectGroup(context, "Meta.SparksBindingRejectsDuplicateNames", [
            BindingPreamble + "ROCKS_CLASS(ScriptType)\nstruct Thing : public rocks::Object {\nROCKS_PROPERTY(ScriptReadOnly) rocks::int32 score = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Child : public Thing {\nROCKS_PROPERTY(ScriptReadOnly, ScriptName = \"Score\") rocks::int32 other = 0;\n};\n}\n",
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) rocks::int32 total = 0;\nROCKS_PROPERTY(ScriptReadOnly, ScriptName = \"Total\") rocks::int32 sum = 0;")],
            "RMC2605", "Sparks member name");
        // RMC2606: 언어 소유 타입 이름(rocks::Asset의 Asset 포함)과 object 필드의 hook 계열 이름(hook·On + hook·Unity 이름)이다.
        AddRejectGroup(context, "Meta.SparksBindingRejectsLanguageNames", [
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) bool tick = false;"),
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) bool onStart = false;"),
            BindingThing("ROCKS_PROPERTY(ScriptReadOnly) bool update = false;"),
            BindingThing("", "ROCKS_CLASS(ScriptType, Name = \"Text\")\nstruct Label : public rocks::Object {};\n"),
            BindingPreamble.Replace("ROCKS_CLASS()\nclass Asset", "ROCKS_CLASS(ScriptType)\nclass Asset", StringComparison.Ordinal) + "}\n"],
            "RMC2606", "");
        // RMC2607: 출시 빌드에 없거나 Sparks 값이 될 수 없는 타입·필드(EditorOnly·Flags enum·보이는 case 없음)다. EditorOnly 클래스의 노출
        // 필드도 RMC2607이다(RMC2608의 "ScriptType을 붙인다"는 안내가 맞지 않는다).
        AddRejectGroup(context, "Meta.SparksBindingRejectsUnexposableTypes", [
            BindingPreamble + "ROCKS_CLASS(EditorOnly)\nstruct Tool : public rocks::Object {\nROCKS_PROPERTY(ScriptReadOnly) rocks::int32 count = 0;\n};\n}\n",
            BindingThing("", "ROCKS_CLASS(EditorOnly, ScriptType)\nstruct Tool : public rocks::Object {};\n"),
            BindingThing("#if ROCKS_WITH_EDITOR_DATA\nROCKS_PROPERTY(EditorOnly, ScriptReadOnly) rocks::int32 note = 0;\n#endif"),
            BindingThing("", "ROCKS_ENUM(Flags, ScriptType)\nenum class Bits : rocks::uint8 { A = 1, B = 2 };\n"),
            BindingThing("", "ROCKS_ENUM(ScriptType)\nenum class Empty : rocks::uint8 { Count ROCKS_ENUMERATOR(Hidden) };\n")],
            "RMC2607", "");
        // RMC2608: 노출하지 않은 타입의 노출 필드다.
        AddRejectGroup(context, "Meta.SparksBindingRejectsFieldOnHiddenType", [
            BindingPreamble + "ROCKS_CLASS()\nstruct Plain : public rocks::Object {\nROCKS_PROPERTY(ScriptReadOnly) rocks::int32 count = 0;\n};\n}\n",
            BindingPreamble + "ROCKS_STRUCT()\nstruct Point {\nROCKS_PROPERTY(ScriptReadOnly) rocks::int32 x = 0;\n};\n}\n"],
            "RMC2608", "not a Sparks type");
    }
}
