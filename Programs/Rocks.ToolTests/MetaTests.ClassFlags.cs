using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9h2 클래스 플래그 지정자 시험이다(Docs/Design/ReflectionExtensions.ko.md 9.2·15절).
internal static partial class MetaTests
{
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
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains(",12u,", generated, "Transient | Deprecated bits");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated class flag header compiles: " + compiled.StdOut + compiled.StdErr);
        });
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
