using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 세 모듈 배열 필드와 배열 descriptor 시험이다(Docs/Design/ReflectionFoundations.ko.md 5.6절).
internal static partial class ModuleMetaTests
{
    // 세 모듈 배열 필드·배열 descriptor 엄격 읽기·위조 대상 종류·배열 descriptor 형질과 가시성 시험이다.
    private static void RegisterArrays(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        // B5c 세 모듈 구성: 원소 struct·enum 모듈(Elements) -> 그 원소의 StructRefArray<SA>·Array<EA>·Array<Plain>·ObjectRefArray를
        // 가진 base 모듈(Lower) -> 그 배열 필드를 상속하고 다른 모듈 struct의 배열 멤버를 가진 import struct(Group)를 필드로 둔
        // 파생 모듈(Upper)이 모두 컴파일된다. 상속 배열 필드는 descriptor 항목이 그대로(maxElements 포함) 옮겨지고, 파생 클래스의
        // 생성 Trace가 import base의 배열 필드를 방문하며, 생성 헤더가 상속 import 필드의 원소 struct 생성 헤더를 include한다
        // (Docs/Design/ReflectionFoundations.ko.md 5.6절).
        suite.Add("Meta.ImportedArrayFieldsAcrossThreeModules", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteArrayModules();
            foreach (string module in new[] { "Elements", "Lower", "Upper" })
            {
                ProcessResult result = await layout.RunArrayModuleAsync(module);
                Verify.Equal(0, result.ExitCode, module + " module: " + result.StdErr);
            }

            using JsonDocument elements = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Elements")));
            using JsonDocument lower = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upper = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            string saId = Type(elements, "probe::SA").GetProperty("id").GetString()!;
            JsonElement slots = FieldNodeOf(lower, "probe::Parent", "slots");
            Verify.Equal("Value", slots.GetProperty("fieldKind").GetString(), "StructRefArray is a value field");
            Verify.Equal(saId, slots.GetProperty("element").GetProperty("type").GetString(), "element struct of another module");
            Verify.Equal("ObjectReference", FieldNodeOf(lower, "probe::Parent", "links").GetProperty("fieldKind").GetString(), "ObjectRefArray field kind");
            Verify.Equal(7u, FieldNodeOf(lower, "probe::Parent", "plains").GetProperty("maxElements").GetUInt32(), "explicit MaxElements");
            Verify.That(Type(lower, "probe::Group").GetProperty("flags").EnumerateArray().Select(flag => flag.GetString())
                .SequenceEqual(["ContainsStrongReference", "ContainsReferenceArray"]), "imported element struct traits propagate");
            foreach (string name in new[] { "slots", "modes", "plains", "links" })
            {
                Verify.Equal(FieldNodeOf(lower, "probe::Parent", name).GetRawText(), FieldNodeOf(upper, "probe::Value", name).GetRawText(),
                    "inherited array field entry (maxElements included) is preserved: " + name);
            }

            string value = Compact(layout.GeneratedText("Upper", "Public/Upper/Value.generated.h"));
            foreach ((string expected, string reason) in new[]
            {
                ("#include<Elements/Items.generated.h>", "element struct header of an inherited import field"),
                ("#include<Lower/Parent.generated.h>", "import struct header"),
                ("detail::TraceArrayField(value.*(&::probe::Parent::slots), visitor);", "class Trace visits the import base StructRefArray"),
                ("detail::TraceArrayField(value.*(&::probe::Parent::links), visitor);", "class Trace visits the import base ObjectRefArray"),
                ("GeneratedType<::probe::Group>::Trace(value.*(&::probe::Value::group), visitor);", "class Trace calls the import struct Trace"),
                ("NativeTypeKey<::probe::SA>()", "inherited struct element native key"),
                ("NativeTypeKey<::probe::EA>()", "inherited enum element native key"),
                ("static_assert(GeneratedType<::probe::SA>::ContainsStrongReference && !GeneratedType<::probe::SA>::ContainsReferenceArray", "inherited StructRefArray element check"),
                (", 7u}", "inherited maxElements in the derived schema"),
                // B6b: 상속 import 필드의 path thunk는 소유자가 파생 클래스(함수표를 내는 Value), 멤버 포인터가 선언 클래스(Parent)다.
                // 이 생성 헤더는 MetaCompiler 검증 컴파일을 통과해야 게시된다.
                ("::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Parent::slots)", "inherited StructRefArray path thunk"),
                ("::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Enum>(cursor, &::probe::Parent::modes)", "inherited enum element array path thunk"),
                ("::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Parent::plains)", "inherited Array<S> path thunk"),
                ("::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::ObjectReference>(cursor, &::probe::Parent::links)", "inherited ObjectRefArray path thunk"),
                ("NativeTypeKey<::probe::Group>(), +[](const ::rocks::RuntimeFieldOps&, ::rocks::detail::PathCursor& cursor) noexcept -> ::rocks::Result<void> { " +
                    "return ::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::group); }", "import struct field path thunk")
            })
            {
                Verify.That(value.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            // 단일 값 필드도 path thunk를 싣는다(리플렉션 B9i3c0 — staged 경로·Detach·Attach용).
            Verify.That(value.Contains("PathClassField<::probe::Value,::rocks::ValueKind::ObjectReference>(cursor,&::probe::Value::next)", StringComparison.Ordinal),
                "single reference field has a path thunk");
            Verify.That(!value.Contains("PathStructMember<", StringComparison.Ordinal), "import struct members are not regenerated in the derived module");
            string parent = Compact(layout.GeneratedText("Lower", "Public/Lower/Parent.generated.h"));
            foreach ((string expected, string reason) in new[]
            {
                ("::rocks::detail::PathClassField<::probe::Parent, ::rocks::ValueKind::Struct>(cursor, &::probe::Parent::slots)", "base class path thunk"),
                ("&detail::PathStructMember<::probe::Group, &::probe::Group::slots, ValueKind::Struct>)", "struct member StructRefArray of another module's element"),
                ("&detail::PathStructMember<::probe::Group, &::probe::Group::plains, ValueKind::Struct>)", "struct member Array<S> of another module's element")
            })
            {
                Verify.That(parent.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            string items = Compact(layout.GeneratedText("Elements", "Public/Elements/Items.generated.h"));
            Verify.That(items.Contains(Compact("&detail::PathStructMember<::probe::SA, &::probe::SA::target, ValueKind::ObjectReference>)"), StringComparison.Ordinal) &&
                items.Contains(Compact("&detail::PathStructMember<::probe::Plain, &::probe::Plain::x, ValueKind::UInt16>)"), StringComparison.Ordinal), "element module struct member path thunks");

            string aggregate = Compact(File.ReadAllText(layout.ModuleSource("Upper")));
            int elementsModule = aggregate.IndexOf("::rocks::generated::Elements::RegisterModuleTypes", StringComparison.Ordinal);
            int lowerModule = aggregate.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int own = aggregate.IndexOf("RegisterIfAbsent(::rocks::GeneratedType<::probe::Value>::Schema()", StringComparison.Ordinal);
            Verify.That(elementsModule >= 0 && lowerModule >= 0 && own > elementsModule && own > lowerModule, "dependency modules are registered first:\n" + aggregate);
        });

        // descriptor의 배열 필드 항목도 엄격하게 읽는다(Docs/Design/ReflectionFoundations.ko.md 5.6절 descriptor): element·maxElements는
        // 배열 필드에만, 원소 키는 종류별로 정확히, element.type은 Enum·Struct면 ID 필수, maxElements 1..1,048,576, fieldKind·Required는
        // 원소 종류와 대응, 원소 대상의 종류(RMC1000)와 존재(RMC2003).
        suite.Add("Meta.ArrayDescriptorIsReadStrictly", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteArrayModules();
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Elements")).ExitCode, "elements module");
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Lower")).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument elements = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Elements")));
            string saId = Type(elements, "probe::SA").GetProperty("id").GetString()!;
            System.Text.Json.Nodes.JsonObject Element(string kind, string? type = null)
            {
                System.Text.Json.Nodes.JsonObject element = new() { ["kind"] = kind };
                if (type is not null)
                {
                    element["type"] = type;
                }

                return element;
            }

            foreach ((string name, string code, string reason, Action<System.Text.Json.Nodes.JsonObject> change) in new (string, string, string, Action<System.Text.Json.Nodes.JsonObject>)[]
            {
                ("elementOnScalar", "RMC1000", "element or maxElements on a field that is not an array", parent => FieldNode(parent, "slots")["value"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "Int32" }),
                ("arrayValueType", "RMC1000", "unexpected or missing properties: type", parent => FieldNode(parent, "modes")["value"]!["type"] = ""),
                ("missingElement", "RMC1000", "element of an array field", parent => FieldNode(parent, "modes")["element"] = null),
                ("nestedElement", "RMC1000", "element kind Array", parent => FieldNode(parent, "modes")["element"] = Element("Array")),
                ("opaqueElement", "RMC1000", "element kind Opaque", parent => FieldNode(parent, "modes")["element"] = Element("Opaque")),
                ("elementWithoutType", "RMC1000", "unexpected or missing properties: type", parent => FieldNode(parent, "modes")["element"] = Element("Enum")),
                ("elementEmptyType", "RMC1000", "identifier must be 32 lowercase hex digits", parent => FieldNode(parent, "modes")["element"] = Element("Enum", "")),
                ("scalarElementType", "RMC1000", "unexpected or missing properties: type", parent => FieldNode(parent, "modes")["element"] = Element("Int32", "")),
                ("elementExtraKey", "RMC1000", "unexpected or missing properties: extra", parent => FieldNode(parent, "plains")["element"]!["extra"] = 1),
                ("zeroMaxElements", "RMC1000", "maxElements 0 of an array field", parent => FieldNode(parent, "plains")["maxElements"] = 0),
                ("largeMaxElements", "RMC1000", "maxElements 1048577 of an array field", parent => FieldNode(parent, "plains")["maxElements"] = 1048577),
                ("referenceArrayAsValue", "RMC1000", "fieldKind and value kind disagree", parent => FieldNode(parent, "links")["fieldKind"] = "Value"),
                ("structArrayAsReference", "RMC1000", "fieldKind and value kind disagree", parent => FieldNode(parent, "slots")["fieldKind"] = "ObjectReference"),
                ("requiredScalarArray", "RMC1000", "Required on a field that is not an object, soft object or class reference or an array of them", parent => FieldNode(parent, "modes")["flags"] = new System.Text.Json.Nodes.JsonArray("Required")),
                ("enumElementToStruct", "RMC1000", "targets probe::SA, which is not a reflected enum", parent => FieldNode(parent, "modes")["element"] = Element("Enum", saId)),
                ("referenceElementToStruct", "RMC1000", "reference field probe::Parent::links targets struct probe::SA", parent => FieldNode(parent, "links")["element"] = Element("ObjectReference", saId)),
                ("unknownElementStruct", "RMC2003", "not in an imported descriptor", parent => FieldNode(parent, "slots")["element"] = Element("Struct", "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c01")),
                // 리플렉션 B9e: Optional 필드는 element가 내부 형식(Optional 내부 종류)이고 maxElements 0, Required를 받지 않는다.
                ("optionalMaxElements", "RMC1000", "of an Optional field", parent => FieldNode(parent, "modes")["value"] = Element("Optional")),
                ("optionalWithoutElement", "RMC1000", "element of an Optional field", parent =>
                {
                    System.Text.Json.Nodes.JsonObject field = FieldNode(parent, "modes");
                    field["value"] = Element("Optional");
                    field["element"] = null;
                }),
                ("optionalReferenceInner", "RMC1000", "Optional inner kind ObjectReference", parent =>
                {
                    System.Text.Json.Nodes.JsonObject field = FieldNode(parent, "links");
                    field["value"] = Element("Optional");
                    field["maxElements"] = 0;
                }),
                ("optionalRequired", "RMC1000", "Required on a field that is not an object, soft object or class reference or an array of them", parent =>
                {
                    System.Text.Json.Nodes.JsonObject field = FieldNode(parent, "modes");
                    field["value"] = Element("Optional");
                    field["maxElements"] = 0;
                    field["flags"] = new System.Text.Json.Nodes.JsonArray("Required");
                }),
                // 리플렉션 B9f2: Map 필드는 key가 Map 키 종류이고 값은 Map 값 종류, Map이 아닌 필드의 key는 null이다. 강한 참조 값 Map은
                // ObjectRefMap(리플렉션 B9f4)이라 descriptor로는 읽히고, C++ 멤버(ObjectRefArray)와 어긋나 생성 Trace의 TraceMapField가
                // 검증 컴파일에서 실패한다(RMC3000 — 위조 descriptor는 게시되지 않는다).
                ("keyOnArray", "RMC1000", "key on a field that is not a map", parent => FieldNode(parent, "modes")["key"] = Element("UInt32")),
                ("mapWithoutKey", "RMC1000", "key of a map field", parent => FieldNode(parent, "modes")["value"] = Element("Map")),
                ("mapFloatKey", "RMC1000", "map key kind Float32", parent =>
                {
                    System.Text.Json.Nodes.JsonObject field = FieldNode(parent, "modes");
                    field["value"] = Element("Map");
                    field["key"] = Element("Float32");
                }),
                ("mapReferenceValue", "RMC3000", "TraceMapField", parent =>
                {
                    System.Text.Json.Nodes.JsonObject field = FieldNode(parent, "links");
                    field["value"] = Element("Map");
                    field["key"] = Element("UInt32");
                })
            })
            {
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, MutateType(original, "probe::Parent", change));
                await Reject(layout.RunArrayModuleAsync("Upper", [mutated, layout.Descriptor("Elements")]), code, reason);
            }
        });

        // B6b: descriptor 문법으로는 맞지만 상속 필드의 원소 종류를 C++ 형식과 다르게 위조하면(Array<EA>를 UInt8 원소로) 파생 모듈의
        // 생성 path thunk가 그 종류를 명시하므로 TargetValueCheck static_assert로 검증 컴파일이 실패하고 출력을 게시하지 않는다(RMC3000).
        suite.Add("Meta.ForgedInheritedTargetKindFailsPathThunkCompile", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteArrayModules();
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Elements")).ExitCode, "elements module");
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Lower")).ExitCode, "lower module");
            string mutated = layout.Generated("Mutated", "byteModes.rocksmeta.json");
            Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
            File.WriteAllText(mutated, MutateType(File.ReadAllText(layout.Descriptor("Lower")), "probe::Parent",
                parent => FieldNode(parent, "modes")["element"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "UInt8" }));
            await Reject(layout.RunArrayModuleAsync("Upper", [mutated, layout.Descriptor("Elements")]), "RMC3000",
                "path thunk target kind does not match the C++ member type");
            Verify.That(!File.Exists(layout.Generated("Upper", "Public/Upper/Value.generated.h")), "rejected module published a generated header");
        });

        // import struct 형질 대조(ContainsReferenceArray — 필드와 어긋나면 RMC1000)와 StructRefArray 원소 struct 규칙(Registry 컨테이너를
        // 담는 참조 struct면 RMC1000), 다른 모듈 private 원소 struct(RMC2006 — 이 모듈 선언과 import 필드).
        suite.Add("Meta.ArrayDescriptorTraitsAndVisibilityAreChecked", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteArrayModules();
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Elements")).ExitCode, "elements module");
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Lower")).ExitCode, "lower module");
            string lower = File.ReadAllText(layout.Descriptor("Lower"));
            string elements = File.ReadAllText(layout.Descriptor("Elements"));
            string Mutate(string descriptor, string name, string type, Action<System.Text.Json.Nodes.JsonObject> change)
            {
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, MutateType(descriptor, type, change));
                return mutated;
            }

            string missingTrait = Mutate(lower, "missingReferenceArray", "probe::Group", group => group["flags"] = new System.Text.Json.Nodes.JsonArray("ContainsStrongReference"));
            await Reject(layout.RunArrayModuleAsync("Upper", [missingTrait, layout.Descriptor("Elements")]), "RMC1000", "ContainsReferenceArray flag");
            string spuriousTrait = Mutate(elements, "spuriousReferenceArray", "probe::Plain", plain => plain["flags"] = new System.Text.Json.Nodes.JsonArray("ContainsReferenceArray"));
            await Reject(layout.RunArrayModuleAsync("Upper", [layout.Descriptor("Lower"), spuriousTrait]), "RMC1000", "ContainsReferenceArray flag");
            string nested = Mutate(elements, "nestedReferenceArray", "probe::SA", sa =>
            {
                sa["flags"] = new System.Text.Json.Nodes.JsonArray("ContainsStrongReference", "ContainsReferenceArray");
                sa["fields"]!.AsArray().Add(new System.Text.Json.Nodes.JsonObject
                {
                    ["owner"] = "probe::SA",
                    ["name"] = "refs",
                    ["id"] = "0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d01",
                    ["fieldKind"] = "ObjectReference",
                    ["value"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "Array" },
                    ["element"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "ObjectReference", ["type"] = "" },
                    ["key"] = null,
                    ["flags"] = new System.Text.Json.Nodes.JsonArray(),
                    ["toolName"] = "refs",
                    ["category"] = "",
                    ["displayName"] = "",
                    ["maxElements"] = 65536,
                    ["bitfield"] = false,
                    ["assetBundles"] = "",
                    ["scriptName"] = ""
                });
            });
            await Reject(layout.RunArrayModuleAsync("Upper", [layout.Descriptor("Lower"), nested]), "RMC1000", "holds a reference array");

            // import DFS의 간선에 struct 원소 배열이 들어간다: 자기 자신을 원소로 담는 Plain은 RMC2302다.
            using (JsonDocument parsed = JsonDocument.Parse(elements))
            {
                string plainId = Type(parsed, "probe::Plain").GetProperty("id").GetString()!;
                string cycle = Mutate(elements, "arrayCycle", "probe::Plain", plain => plain["fields"]!.AsArray().Add(new System.Text.Json.Nodes.JsonObject
                {
                    ["owner"] = "probe::Plain",
                    ["name"] = "children",
                    ["id"] = "0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e01",
                    ["fieldKind"] = "Value",
                    ["value"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "Array" },
                    ["element"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "Struct", ["type"] = plainId },
                    ["key"] = null,
                    ["flags"] = new System.Text.Json.Nodes.JsonArray(),
                    ["toolName"] = "children",
                    ["category"] = "",
                    ["displayName"] = "",
                    ["maxElements"] = 65536,
                    ["bitfield"] = false,
                    ["assetBundles"] = "",
                    ["scriptName"] = ""
                }));
                await Reject(layout.RunArrayModuleAsync("Upper", [layout.Descriptor("Lower"), cycle]), "RMC2302", "cycle");
            }

            string hidden = Mutate(elements, "privateElement", "probe::SA", sa => sa["visibility"] = "private");
            await Reject(layout.RunArrayModuleAsync("Lower", [hidden]), "RMC2006", "private reflected type");
            await Reject(layout.RunArrayModuleAsync("Upper", [layout.Descriptor("Lower"), hidden]), "RMC2006", "uses private struct");
        });
    }
}
