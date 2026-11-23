using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 클래스 플래그 지정자(리플렉션 B9h2 — Docs/Design/ReflectionExtensions.ko.md 9.2·15절, 에셋 A4e2a EditorOnly)와 필드
// AssetBundles(에셋 A4e2a — Docs/Design/AssetSystem 10.2절) 시험이다.
internal static partial class MetaTests
{
    // AssetBundles 원본이 쓰는 헤더(소프트 참조·배열·Map·InstancedStruct·AssetId)다.
    private const string BundlePreamble = "#include <Object/SoftObjectRef.h>\n#include <Core/Containers/Array.h>\n#include <Core/Containers/OrderedMap.h>\n" +
        "#include <Object/InstancedStruct.h>\n#include <Object/AssetId.h>\n" + Preamble;

    // 소프트 참조 필드 하나에 AssetBundles 값(따옴표 포함 원문)을 둔 원본이다.
    private static string BundleField(string value) =>
        BundlePreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& a) noexcept : soft(a) {}\n" +
        "ROCKS_PROPERTY(AssetBundles = " + value + ") rocks::SoftObjectRef<rocks::Object> soft;\n};\n}\n";

    // 클래스 Transient·Deprecated와 Within이 descriptor(flags·within)와 생성 스키마(flags 비트·withinType)에 닿고 상속된다. Deprecated
    // 클래스를 가리키는 비Deprecated PROPERTY는 RMC2209, base Within을 좁히지 않는 Within은 RMC2208이다.
    private static void RegisterClassFlags(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.ClassFlagSpecifiersReachDescriptorAndSchema", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(Preamble +
                "ROCKS_CLASS()\nstruct Holder : public rocks::Object {};\n" +
                "ROCKS_CLASS(Transient, Within = \"probe::Holder\")\nstruct Parent : public rocks::Object {};\n" +
                "ROCKS_CLASS(Deprecated)\nstruct Value : public Parent {\nROCKS_PROPERTY() rocks::ObjectRef<Value> next;\n};\n}\n");
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name);
            string[] Flags(string name) => Type(name).GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
            Verify.That(Flags("probe::Parent").SequenceEqual(["Transient"]), "transient class flag");
            Verify.That(Flags("probe::Value").SequenceEqual(["Transient", "Deprecated"]), "inherited transient and deprecated flags");
            Verify.Equal("probe::Holder", Type("probe::Value").GetProperty("within").GetProperty("name").GetString(), "inherited within");
            Verify.Equal(Type("probe::Holder").GetProperty("id").GetString(), Type("probe::Parent").GetProperty("within").GetProperty("id").GetString(),
                "within id");
            Verify.Equal(System.Text.Json.JsonValueKind.Null, Type("probe::Holder").GetProperty("within").ValueKind, "no within");
            string generated = Compact(probe.GeneratedText());
            Verify.Contains(",12u,", generated, "Transient | Deprecated bits");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated class flag header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // 클래스 EditorOnly(에셋 A4e2a — Docs/Design/AssetSystem 10.2절)는 descriptor flags와 생성 스키마 비트 32에 닿고 상속되며,
        // 다른 클래스 플래그와 함께 쓸 수 있다. descriptor로 읽은 import 클래스의 파생도 물려받는다.
        suite.Add("Meta.EditorOnlyClassFlagIsInherited", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(Preamble +
                "ROCKS_CLASS(EditorOnly)\nstruct Parent : public rocks::Object {};\n" +
                "ROCKS_CLASS(WorldOnly)\nstruct Child : public Parent {};\n" +
                "ROCKS_CLASS(Abstract, EditorOnly)\nstruct Tool : public rocks::Object {};\n}\n");
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            string[] Flags(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name).GetProperty("flags").EnumerateArray()
                .Select(flag => flag.GetString() ?? "").ToArray();
            Verify.That(Flags("probe::Parent").SequenceEqual(["EditorOnly"]), "editor-only class flag");
            Verify.That(Flags("probe::Child").SequenceEqual(["WorldOnly", "EditorOnly"]), "inherited editor-only flag");
            Verify.That(Flags("probe::Tool").SequenceEqual(["Abstract", "EditorOnly"]), "abstract editor-only class");
            string generated = Compact(probe.GeneratedText());
            Verify.Contains(",32u,", generated, "EditorOnly bit");
            Verify.Contains(",34u,", generated, "WorldOnly | EditorOnly bits");
            Verify.Contains(",33u,", generated, "Abstract | EditorOnly bits");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated editor-only header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        AddRejectGroup(context, "Meta.EditorOnlyIsClassOnly", [
            Preamble + "ROCKS_STRUCT(EditorOnly)\nstruct Value {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n",
            Preamble + "ROCKS_CLASS(EditorOnly = 1)\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "unsupported");
        // 필드 AssetBundles(에셋 A4e2a)는 소프트 참조·struct·InstancedStruct 값과 Map 소프트 키에 쓰고, 이름 앞뒤 공백을 뗀 정규 텍스트가
        // descriptor(assetBundles)와 생성 스키마(FieldSchema 마지막 칸)에 닿는다. 지정하지 않은 필드는 빈 텍스트다.
        suite.Add("Meta.AssetBundlesReachDescriptorAndSchema", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(BundlePreamble +
                "ROCKS_STRUCT()\nstruct Slot {\nROCKS_GENERATED_BODY()\nexplicit Slot(rocks::Allocator& a) noexcept : icon(a) {}\n" +
                "ROCKS_PROPERTY(AssetBundles = \"UI\") rocks::SoftObjectRef<rocks::Object> icon;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& a) noexcept : portrait(a), levels(a), slot(a), weights(a) {}\n" +
                "ROCKS_PROPERTY(AssetBundles = \" UI , Game \") rocks::SoftObjectRef<rocks::Object> portrait;\n" +
                "ROCKS_PROPERTY(AssetBundles = \"Game\") rocks::Array<rocks::SoftObjectRef<rocks::Object>> levels;\n" +
                "ROCKS_PROPERTY(AssetBundles = \"UI\") Slot slot;\n" +
                "ROCKS_PROPERTY(AssetBundles = \"Audio\") rocks::OrderedMap<rocks::SoftObjectRef<>, rocks::int32> weights;\n" +
                "ROCKS_PROPERTY(AssetBundles = \"Game\") rocks::InstancedStruct<> extra;\n" +
                "ROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n");
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            string Bundles(string type, string field) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == type).GetProperty("fields").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == field).GetProperty("assetBundles").GetString() ?? "";
            Verify.Equal("UI,Game", Bundles("probe::Value", "portrait"), "trimmed canonical list");
            Verify.Equal("Game", Bundles("probe::Value", "levels"), "array list");
            Verify.Equal("UI", Bundles("probe::Value", "slot"), "struct list");
            Verify.Equal("Audio", Bundles("probe::Value", "weights"), "map key list");
            Verify.Equal("Game", Bundles("probe::Value", "extra"), "instanced struct list");
            Verify.Equal("", Bundles("probe::Value", "count"), "no list");
            Verify.Equal("UI", Bundles("probe::Slot", "icon"), "struct member list");
            string generated = Compact(probe.GeneratedText());
            Verify.Contains("\"UI,Game\"", generated, "generated canonical list");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated asset bundle header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        AddRejectGroup(context, "Meta.AssetBundlesRejectWrongFields", [
            BundlePreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY(AssetBundles = \"UI\") rocks::int32 count = 0;\n};\n}\n",
            BundlePreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY(AssetBundles = \"UI\") rocks::AssetId id{};\n};\n}\n"],
            "RMC2202", "AssetBundles applies only");
        AddRejectGroup(context, "Meta.AssetBundlesRejectEditorOnlyFields", [
            BundlePreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& a) noexcept : soft(a) {}\n" +
                "ROCKS_PROPERTY(EditorOnly, AssetBundles = \"UI\") rocks::SoftObjectRef<rocks::Object> soft;\n};\n}\n",
            BundlePreamble + "ROCKS_STRUCT()\nstruct Slot {\nROCKS_GENERATED_BODY()\nexplicit Slot(rocks::Allocator& a) noexcept : icon(a) {}\n" +
                "ROCKS_PROPERTY(AssetBundles = \"UI\") rocks::SoftObjectRef<rocks::Object> icon;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& a) noexcept : slot(a) {}\n" +
                "ROCKS_PROPERTY(EditorOnly) Slot slot;\n};\n}\n"],
            "RMC2202", "inside an EditorOnly field");
        AddRejectGroup(context, "Meta.AssetBundlesRejectBadLists", [
            BundleField("\"\""), BundleField("\"UI,,Game\""), BundleField("\"UI,UI\""), BundleField("\"1UI\""),
            BundleField("\"" + new string('a', 65) + "\""),
            BundleField("\"" + string.Join(',', Enumerable.Range(0, 17).Select(index => "B" + index)) + "\"")],
            "RMC2212", "AssetBundles");
        AddRejectGroup(context, "Meta.DeprecatedClassReferencesAreRejected", [
            Preamble + "ROCKS_CLASS(Deprecated)\nstruct Holder : public rocks::Object {};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::ObjectRef<Holder> next;\n};\n}\n",
            Preamble + "ROCKS_CLASS(Deprecated)\nstruct Holder : public rocks::Object {};\n" +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::WeakObjectRef<Holder> target;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n"],
            "RMC2209", "refers to Deprecated class");
        AddRejectGroup(context, "Meta.WithinMustNarrowBaseWithin", [
            Preamble + "ROCKS_CLASS()\nstruct Holder : public rocks::Object {};\n" +
                "ROCKS_CLASS()\nstruct Other : public rocks::Object {};\n" +
                "ROCKS_CLASS(Within = \"probe::Holder\")\nstruct Parent : public rocks::Object {};\n" +
                "ROCKS_CLASS(Within = \"probe::Other\")\nstruct Value : public Parent {};\n}\n",
            Preamble + "ROCKS_CLASS(Within = \"probe::Missing\")\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2208", "Within");
    }
}
