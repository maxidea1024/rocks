using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 import struct·Binds·struct 깊이·순환 시험이다(Docs/Design/ReflectionFoundations.ko.md
// 4.5–4.6절).
internal static partial class ModuleMetaTests
{
    // import struct 필드·가시성·descriptor·Atomic·형질·Binds 실제 타입·깊이·순환 시험이다.
    private static void RegisterStructs(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        // B4b: 의존 모듈의 ROCKS_STRUCT를 descriptor로 import해 클래스·struct 필드로 쓴다. Lower 등록 함수(모듈 생성 소스)는 enum -> struct ->
        // 클래스로 등록하고, Upper 생성 헤더는 import struct(상속 필드 포함)와 같은 모듈 다른 헤더 struct의 생성 헤더를
        // include하며, 참조 형질이 import struct에서 전파된다(Docs/Design/ReflectionFoundations.ko.md 4.6절).
        suite.Add("Meta.ImportedStructsResolveAsFieldTypes", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string lowerAggregate = Compact(File.ReadAllText(layout.ModuleSource("Lower")));
            int point = lowerAggregate.IndexOf("RegisterStructIfAbsent(::rocks::GeneratedType<::probe::Point>::Schema()", StringComparison.Ordinal);
            int link = lowerAggregate.IndexOf("RegisterStructIfAbsent(::rocks::GeneratedType<::probe::Link>::Schema()", StringComparison.Ordinal);
            int parent = lowerAggregate.IndexOf("RegisterIfAbsent(::rocks::GeneratedType<::probe::Parent>::Schema()", StringComparison.Ordinal);
            Verify.That(point >= 0 && link > point && parent > link, "aggregate registers member structs first, then classes:\n" + lowerAggregate);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false),
                (layout.Source("Upper/Public/Upper/Shape.h"), "Upper/Shape.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            string pointId = Type(lowerDescriptor, "probe::Point").GetProperty("id").GetString()!;
            string linkId = Type(lowerDescriptor, "probe::Link").GetProperty("id").GetString()!;
            Verify.Equal("struct", Type(lowerDescriptor, "probe::Link").GetProperty("kind").GetString(), "struct descriptor kind");
            Verify.Equal("ContainsStrongReference", Type(lowerDescriptor, "probe::Link").GetProperty("flags")[0].GetString(), "reference struct flag");
            JsonElement Field(JsonDocument descriptor, string type, string name) => Type(descriptor, type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string TargetOf(JsonDocument descriptor, string type, string name) => Field(descriptor, type, name).GetProperty("value").GetProperty("type").GetString()!;
            Verify.Equal(pointId, TargetOf(upperDescriptor, "probe::Value", "at"), "own field of an imported struct");
            Verify.Equal(pointId, TargetOf(upperDescriptor, "probe::Value", "origin"), "inherited imported struct field");
            Verify.Equal(Type(upperDescriptor, "probe::Shape").GetProperty("id").GetString(), TargetOf(upperDescriptor, "probe::Value", "shape"), "same-module struct field");
            Verify.Equal(linkId, TargetOf(upperDescriptor, "probe::Shape", "link"), "struct member of an imported struct");
            Verify.Equal("ContainsStrongReference", Type(upperDescriptor, "probe::Shape").GetProperty("flags")[0].GetString(), "reference trait propagates from an imported struct");

            string value = Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Value.generated.h")));
            Verify.That(value.Contains("#include<Lower/Parent.generated.h>", StringComparison.Ordinal), "imported struct generated header include");
            Verify.That(value.Contains("#include<Upper/Shape.generated.h>", StringComparison.Ordinal), "same-module struct generated header include");
            Verify.That(value.Contains(Compact("GeneratedType<::probe::Point>::Trace(value.*(&::probe::Parent::origin), visitor);"), StringComparison.Ordinal), "inherited struct field Trace");
            Verify.That(value.Contains(Compact("NativeTypeKey<::probe::Shape>()"), StringComparison.Ordinal), "same-module struct field native key");
            Verify.That(!value.Contains("structGeneratedType<::probe::Shape>", StringComparison.Ordinal), "struct specialization is defined once (in its own header)");
            string shape = Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Shape.generated.h")));
            Verify.That(shape.Contains("#include<Lower/Parent.generated.h>", StringComparison.Ordinal), "struct header includes imported member struct header");
            Verify.That(shape.Contains(Compact("GeneratedType<::probe::Link>::Trace(value.*(&::probe::Shape::link), visitor);"), StringComparison.Ordinal), "struct traces imported reference struct");
            string aggregate = Compact(File.ReadAllText(layout.ModuleSource("Upper")));
            int dependency = aggregate.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int own = aggregate.IndexOf("RegisterStructIfAbsent(::rocks::GeneratedType<::probe::Shape>::Schema()", StringComparison.Ordinal);
            int owner = aggregate.IndexOf("RegisterIfAbsent(::rocks::GeneratedType<::probe::Value>::Schema()", StringComparison.Ordinal);
            Verify.That(dependency >= 0 && own > dependency && owner > own, "upper aggregate registration order:\n" + aggregate);
        });

        // public 타입은 private struct를 필드·NATIVE 멤버로 쓸 수 없다(같은 모듈 — public 집계·생성 헤더가 private 것을 볼 수
        // 없다). 다른 모듈의 private struct도 쓸 수 없다.
        suite.Add("Meta.PublicTypeCannotUsePrivateStruct", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Write("Upper/Private/Upper/Secret.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
                "ROCKS_STRUCT()\nstruct Secret {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n}\n");
            layout.Manifest("Upper", ["probe::Secret"], ["probe::Secret::a", "probe::Value::secret", "probe::Shape::secret"]);
            // 배열 원소 struct도 같다(B5c — 대상 종류가 원소 종류다).
            foreach (string member in new[] { "ROCKS_PROPERTY() Secret secret;\n", "ROCKS_NATIVE() Secret secret;\n", "ROCKS_PROPERTY() rocks::Array<Secret> secret;\n" })
            {
                layout.Write("Upper/Public/Upper/Value.h", Value.Replace("#include <Object/ObjectRefs.h>\n",
                        "#include <Object/ObjectRefs.h>\n#include <Core/Containers/Array.h>\n#include <Upper/Secret.h>\n", StringComparison.Ordinal)
                    .Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n", "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n" + member, StringComparison.Ordinal));
                await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false),
                    (layout.Source("Upper/Private/Upper/Secret.h"), "Upper/Secret.h", true)]), "RMC2006", "private struct");
            }

            // public struct도 같은 모듈의 private struct를 멤버로 쓸 수 없다.
            layout.Write("Upper/Public/Upper/Shape.h", "#pragma once\n#include <Upper/Secret.h>\nnamespace probe {\n" +
                "ROCKS_STRUCT()\nstruct Shape {\nROCKS_PROPERTY() Secret secret;\n};\n}\n");
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Shape.h"), "Upper/Shape.h", false),
                (layout.Source("Upper/Private/Upper/Secret.h"), "Upper/Secret.h", true)]), "RMC2006", "private struct");
            layout.Write("Upper/Public/Upper/Shape.h", Shape);

            Verify.Equal(0, (await layout.RunLowerAsync(privateParent: true)).ExitCode, "private lower module");
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Shape.h"), "Upper/Shape.h", false)]), "RMC2006", "private reflected type");
        });

        // descriptor의 struct 항목도 엄격하게 읽는다(키·flags·binds·필드 owner·대상 ID, 형질과 필드의 일치 — 어기면 RMC1000).
        suite.Add("Meta.StructDescriptorIsReadStrictly", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument parsed = JsonDocument.Parse(original);
            string pointId = Type(parsed, "probe::Point").GetProperty("id").GetString()!;
            foreach ((string name, string from, string to) in new[]
            {
                ("extraKey", "\"binds\": \"\"", "\"binds\": \"\",\n      \"base\": \"rocks::Object\""),
                ("binds", "\"binds\": \"\"", "\"binds\": \"rocks::Vector3<float\""),
                ("unknownFlag", "\"ContainsStrongReference\"", "\"Abstract\""),
                ("missingTrait", "\"ContainsStrongReference\"", ""),
                ("owner", "\"owner\": \"probe::Point\"", "\"owner\": \"probe::Link\""),
                ("emptyTarget", $"\"type\": \"{pointId}\"", "\"type\": \"\"")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }

            // 대상 ID가 다른 종류의 타입을 가리키거나(Struct 필드 -> enum, 참조 -> struct) 멤버 struct의 WorldOnly 소유자 형질이
            // 전파되지 않은 descriptor도 망가진 것이다.
            string modeId = Type(parsed, "probe::Mode").GetProperty("id").GetString()!;
            foreach ((string name, string type, Action<System.Text.Json.Nodes.JsonObject> change) in new (string, string, Action<System.Text.Json.Nodes.JsonObject>)[]
            {
                ("structToEnum", "probe::Link", link => FieldNode(link, "at")["value"]!["type"] = modeId),
                ("referenceToStruct", "probe::Link", link => FieldNode(link, "target")["value"]!["type"] = pointId),
                ("missingWorldOnlyTrait", "probe::Outer", outer => outer["flags"] = new System.Text.Json.Nodes.JsonArray())
            })
            {
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, MutateType(original, type, change));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }
        });

        // 리플렉션 B9b2: struct flags의 Atomic(Docs/Design/ReflectionExtensions.ko.md 1.8절)은 descriptor를 왕복한다 — import 모델의
        // Atomic이 되고(뒤의 형질도 그대로 읽힌다) 그 모델을 다시 쓰면 같은 byte이며, 의존 모듈이 Atomic struct를 import해 생성한다.
        // Atomic은 struct 전용이라 클래스 항목의 Atomic과 중복 Atomic은 모르는·중복 flag(RMC1000)다.
        suite.Add("Meta.StructAtomicDescriptorRoundTrips", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            layout.Write("Lower/Public/Lower/Parent.h", StructParent
                .Replace("ROCKS_STRUCT()\nstruct Point {", "ROCKS_STRUCT(Atomic)\nstruct Point {", StringComparison.Ordinal)
                .Replace("ROCKS_STRUCT()\nstruct Link {", "ROCKS_STRUCT(Atomic)\nstruct Link {", StringComparison.Ordinal));
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            byte[] original = File.ReadAllBytes(layout.Descriptor("Lower"));
            (string module, List<ClassModel> types) = ModuleDescriptor.Read(layout.Descriptor("Lower"));
            ClassModel Find(string name) => types.Single(type => type.Name == name);
            Verify.That(Find("probe::Point").Atomic && Find("probe::Link").Atomic, "imported Atomic structs");
            Verify.That(!Find("probe::Outer").Atomic && !Find("probe::WorldBits").Atomic && !Find("probe::Parent").Atomic, "imported types without Atomic");
            Verify.That(Find("probe::Link").ContainsStrongReference && Find("probe::Outer").RequiresWorldOnlyOwner, "generated traits are read next to Atomic");
            Verify.Equal(16u, Find("probe::Point").Flags, "imported Atomic is TypeFlags::Atomic");
            Verify.That(ModuleDescriptor.Write(module, types).AsSpan().SequenceEqual(original), "descriptor written from the imported models must keep its bytes");
            using (JsonDocument parsed = JsonDocument.Parse(original))
            {
                Verify.Equal("Atomic", Type(parsed, "probe::Link").GetProperty("flags")[0].GetString(), "Atomic is written before generated traits");
            }

            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false),
                (layout.Source("Upper/Public/Upper/Shape.h"), "Upper/Shape.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module imports Atomic structs: " + upper.StdErr);

            string text = System.Text.Encoding.UTF8.GetString(original);
            foreach ((string name, string type, Action<System.Text.Json.Nodes.JsonObject> change) in new (string, string, Action<System.Text.Json.Nodes.JsonObject>)[]
            {
                ("classAtomic", "probe::Parent", parent => parent["flags"] = new System.Text.Json.Nodes.JsonArray("Atomic")),
                ("duplicateAtomic", "probe::Point", point => point["flags"] = new System.Text.Json.Nodes.JsonArray("Atomic", "Atomic"))
            })
            {
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, MutateType(text, type, change));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }
        });

        // import한 struct의 형질: Lower의 WorldBits(RequiresWorldOnlyOwner)를 필드로 둔 WorldOnly 아닌 Upper 클래스는 RMC2206이고,
        // descriptor가 public struct에서 private struct를 쓰거나 다른 모듈의 private struct를 쓰면 RMC2006이다.
        suite.Add("Meta.ImportedStructTraitsAndVisibilityAreEnforced", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Manifest("Upper", [], ["probe::Value::bits"]);
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() WorldBits bits;\n", StringComparison.Ordinal));
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]), "RMC2206", "requires a WorldOnly owner");

            string original = File.ReadAllText(layout.Descriptor("Lower"));
            string hidden = layout.Generated("Mutated", "privatePoint.rocksmeta.json");
            Directory.CreateDirectory(Path.GetDirectoryName(hidden)!);
            File.WriteAllText(hidden, MutateType(original, "probe::Point", point => point["visibility"] = "private"));
            await Reject(layout.RunUpperAsync(imports: [hidden]), "RMC2006", "uses private struct");

            // 다른 모듈(Other)의 private 타입이 Lower의 private struct를 쓴다(손으로 만든 descriptor).
            Verify.Equal(0, (await layout.RunLowerAsync(privateParent: true)).ExitCode, "private lower module");
            string privateLower = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument parsed = JsonDocument.Parse(privateLower);
            string pointId = Type(parsed, "probe::Point").GetProperty("id").GetString()!;
            System.Text.Json.Nodes.JsonNode other = System.Text.Json.Nodes.JsonNode.Parse(privateLower)!;
            System.Text.Json.Nodes.JsonObject box = other["types"]!.AsArray().Select(node => node!.AsObject())
                .Single(node => (string?)node["name"] == "probe::Point").DeepClone().AsObject();
            box["name"] = "probe::OtherBox";
            box["id"] = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b01";
            box["header"] = "Other/Box.h";
            box["toolName"] = "OtherBox";
            System.Text.Json.Nodes.JsonObject inner = FieldNode(box, "x");
            inner["owner"] = "probe::OtherBox";
            inner["name"] = "inner";
            inner["toolName"] = "inner";
            inner["id"] = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b02";
            inner["value"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "Struct", ["type"] = pointId };
            other["module"] = "Other";
            other["types"] = new System.Text.Json.Nodes.JsonArray(box);
            string otherPath = layout.Generated("Mutated", "Other.rocksmeta.json");
            File.WriteAllText(otherPath, other.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            await Reject(layout.RunUpperAsync(imports: [layout.Descriptor("Lower"), otherPath]), "RMC2006", "of module Lower");
        });

        // B4c: CoreTypeBindings descriptor의 Binds 미러는 실제 타입 철자(binds)로 다른 모듈의 필드 형식을 푼다. 생성 헤더는
        // 미러의 생성 헤더를 include하고 실제 타입의 native 키를 싣는다. descriptor의 binds는 철자 문법(RMC1000)과 모듈·중복·
        // 리플렉션 타입 결합 규칙(RMC2004)을 검사하고, 미러 타입을 필드 형식으로 쓰면 거부한다.
        suite.Add("Meta.ImportedBindsResolveRealTypes", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", BindsParent);
            layout.Manifest("Lower", ["probe::Vec3Binding", "probe::Vec3dBinding"],
                ["probe::Vec3Binding::x", "probe::Vec3Binding::y", "probe::Vec3Binding::z", "probe::Vec3dBinding::x", "probe::Vec3dBinding::y", "probe::Vec3dBinding::z"]);
            Directory.CreateDirectory(layout.Generated(BindingsModule, "Public"));
            Directory.CreateDirectory(layout.Generated(BindingsModule, "Private"));
            ProcessResult lower = await layout.RunLowerAsync(module: BindingsModule);
            Verify.Equal(0, lower.ExitCode, "bindings module: " + lower.StdErr);
            layout.Manifest("Upper", [], ["probe::Value::position", "probe::Value::mirror"]);
            string ValueWith(string member) => Value.Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n" + member, StringComparison.Ordinal);
            Task<ProcessResult> RunUpper(string descriptor) => layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)],
                imports: [descriptor], dependencyModule: BindingsModule);
            // 묶인 실제 타입을 NATIVE 멤버로 두면 생성 코드가 그 미러의 생성 헤더를 include하고 형질을 확인한다.
            layout.Write("Upper/Public/Upper/Value.h", ValueWith("ROCKS_PROPERTY() rocks::Vec3 position;\nROCKS_NATIVE() rocks::Vec3 cache;\n"));
            ProcessResult upper = await RunUpper(layout.Descriptor(BindingsModule));
            Verify.Equal(0, upper.ExitCode, "consumer module: " + upper.StdErr);

            using JsonDocument bindings = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor(BindingsModule)));
            using JsonDocument consumer = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            Verify.Equal("rocks::Vector3<float>", Type(bindings, "probe::Vec3Binding").GetProperty("binds").GetString(), "descriptor binds");
            Verify.Equal(Type(bindings, "probe::Vec3Binding").GetProperty("id").GetString(),
                FieldNodeOf(consumer, "probe::Value", "position").GetProperty("value").GetProperty("type").GetString(), "imported real type resolves to the mirror");
            string generated = Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Value.generated.h")));
            Verify.That(generated.Contains("#include<Lower/Parent.generated.h>", StringComparison.Ordinal), "mirror generated header include");
            Verify.That(generated.Contains(Compact("NativeTypeKey<::rocks::Vector3<float>>()"), StringComparison.Ordinal), "real type native key");
            Verify.That(generated.Contains(Compact("static_assert(!GeneratedType<::rocks::Vector3<float>>::ContainsStrongReference"), StringComparison.Ordinal),
                "NATIVE member of a bound real type is checked by its generated trait");
            Verify.That(Compact(File.ReadAllText(layout.ModuleSource("Upper")))
                .Contains("::rocks::generated::CoreTypeBindings::RegisterModuleTypes", StringComparison.Ordinal), "bindings module is registered first");

            string original = File.ReadAllText(layout.Descriptor(BindingsModule));
            string vec3Id = Type(bindings, "probe::Vec3Binding").GetProperty("id").GetString()!;
            foreach ((string name, string code, string reason, Func<string> mutate) in new (string, string, string, Func<string>)[]
            {
                ("bindsReflected", "RMC2004", "binds reflected type", () => MutateType(original, "probe::Vec3Binding", mirror => mirror["binds"] = "probe::Parent")),
                ("bindsGlobal", "RMC1000", "invalid descriptor", () => MutateType(original, "probe::Vec3Binding", mirror => mirror["binds"] = "::rocks::Vector3<float>")),
                ("bindsCycle", "RMC2302", "cycle", () => MutateType(original, "probe::Vec3Binding",
                    mirror => FieldNode(mirror, "x")["value"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "Struct", ["type"] = vec3Id })),
                ("bindsTwice", "RMC2004", "both bind", () => MutateType(original, "probe::Vec3dBinding", mirror => mirror["binds"] = "rocks::Vector3<float>")),
                ("bindsSpelling", "RMC1000", "invalid descriptor", () => MutateType(original, "probe::Vec3Binding", mirror => mirror["binds"] = "rocks::Vec3; int x")),
                ("bindsModule", "RMC2004", "only CoreTypeBindings", () => original.Replace($"\"module\": \"{BindingsModule}\"", "\"module\": \"Other\"", StringComparison.Ordinal))
            })
            {
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, mutate());
                await Reject(RunUpper(mutated), code, reason);
            }

            layout.Write("Upper/Public/Upper/Value.h", ValueWith("ROCKS_PROPERTY() Vec3Binding mirror;\n"));
            await Reject(RunUpper(layout.Descriptor(BindingsModule)), "RMC2000", "is a Binds mirror declaration");
        });

        // CoreTypeBindings를 import했는데 그 모듈이 묶지 않은 인스턴스(여기서는 Vec3만 묶고 Vec3d를 쓴다)를 쓰면 의존을 더하라는
        // 안내가 아니라 묶인 인스턴스를 알려 준다.
        suite.Add("Meta.UnboundCoreValueInstantiationHint", async () =>
        {
            using Layout layout = new(options, environment);
            int vec3d = BindsParent.IndexOf("ROCKS_STRUCT(Binds = \"rocks::Vec3d\")", StringComparison.Ordinal);
            int parent = BindsParent.IndexOf("ROCKS_CLASS()", StringComparison.Ordinal);
            layout.Write("Lower/Public/Lower/Parent.h", BindsParent[..vec3d] + BindsParent[parent..]);
            layout.Manifest("Lower", ["probe::Vec3Binding"], ["probe::Vec3Binding::x", "probe::Vec3Binding::y", "probe::Vec3Binding::z"]);
            Directory.CreateDirectory(layout.Generated(BindingsModule, "Public"));
            Directory.CreateDirectory(layout.Generated(BindingsModule, "Private"));
            ProcessResult lower = await layout.RunLowerAsync(module: BindingsModule);
            Verify.Equal(0, lower.ExitCode, "bindings module: " + lower.StdErr);
            layout.Manifest("Upper", [], ["probe::Value::precise"]);
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() rocks::Vec3d precise;\n", StringComparison.Ordinal));
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)],
                imports: [layout.Descriptor(BindingsModule)], dependencyModule: BindingsModule);
            Verify.That(upper.ExitCode != 0, "unbound Core value instantiation was accepted");
            Verify.Contains("RMC2000", upper.StdErr, "unsupported field type code");
            Verify.Contains("not bound by CoreTypeBindings", upper.StdErr, "unbound instantiation hint");
            Verify.Contains("rocks::Vector3<float>", upper.StdErr, "bound instantiation in the hint");
            Verify.That(!upper.StdErr.Contains("RequiredModules", StringComparison.Ordinal), "dependency hint is not shown when the descriptor is imported");
        });

        // 깊이는 import struct까지 이어서 센다: 위조한 import 사슬 17단(descriptor), 이 모듈 struct가 import한 깊이 16 사슬을
        // 담는 경우는 RMC2303이고 깊이 15 사슬을 담으면 깊이 16으로 받는다.
        suite.Add("Meta.ImportedStructDepthIsChecked", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", ChainParent);
            layout.Manifest("Lower", Enumerable.Range(1, 16).Select(level => $"probe::Chain{level}").Append("probe::Top").ToArray(),
                Enumerable.Range(2, 15).Select(level => $"probe::Chain{level}::inner").Concat(["probe::Chain1::value", "probe::Top::x"]).ToArray());
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument parsed = JsonDocument.Parse(original);
            string chain16 = Type(parsed, "probe::Chain16").GetProperty("id").GetString()!;
            string forged = layout.Generated("Mutated", "deep.rocksmeta.json");
            Directory.CreateDirectory(Path.GetDirectoryName(forged)!);
            File.WriteAllText(forged, original.Replace("\"kind\": \"UInt16\"", $"\"kind\": \"Struct\", \"type\": \"{chain16}\"", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(imports: [forged]), "RMC2303", "nesting depth 17");

            layout.Manifest("Upper", ["probe::Deep"], ["probe::Deep::inner"]);
            string Deep(int level) => $"#pragma once\n#include <Lower/Parent.h>\nnamespace probe {{\nROCKS_STRUCT()\nstruct Deep {{\nROCKS_PROPERTY() Chain{level} inner;\n}};\n}}\n";
            layout.Write("Upper/Public/Upper/Deep.h", Deep(15));
            ProcessResult accepted = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Deep.h"), "Upper/Deep.h", false)]);
            Verify.Equal(0, accepted.ExitCode, "struct over an imported depth-15 chain: " + accepted.StdErr);
            Verify.That(Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Deep.generated.h"))).Contains("staticconstexpruint32Depth=16u;", StringComparison.Ordinal),
                "depth continues through imported structs");
            layout.Write("Upper/Public/Upper/Deep.h", Deep(16));
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Deep.h"), "Upper/Deep.h", false)]), "RMC2303", "nesting depth 17");
        });

        // struct 순환은 C++ 값 멤버로는 만들 수 없으므로(불완전 타입) 조작한 descriptor로 import 쪽 DFS를 시험한다(RMC2302).
        suite.Add("Meta.ImportedStructCycleIsRejected", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument parsed = JsonDocument.Parse(original);
            string linkId = Type(parsed, "probe::Link").GetProperty("id").GetString()!;
            string pointId = Type(parsed, "probe::Point").GetProperty("id").GetString()!;
            // Point -> Link -> Point, 그리고 Point -> Point(자기 자신).
            foreach ((string name, string target) in new[] { ("mutual", linkId), ("self", pointId) })
            {
                string mutatedText = original.Replace("\"kind\": \"UInt16\"", $"\"kind\": \"Struct\", \"type\": \"{target}\"", StringComparison.Ordinal);
                Verify.That(mutatedText != original, "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, mutatedText);
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC2302", "cycle");
            }
        });
    }
}
