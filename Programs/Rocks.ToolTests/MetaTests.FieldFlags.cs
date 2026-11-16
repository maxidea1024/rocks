using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9h1 필드 플래그 지정자 시험이다(Docs/Design/ReflectionExtensions.ko.md 9.1·15절).
internal static partial class MetaTests
{
    // DuplicateTransient·NonPIEDuplicateTransient·SaveGame·TextExportTransient가 descriptor flags(이름)와 생성 스키마(비트 16·32·64·128)에
    // 닿고 생성 헤더가 컴파일된다. Required와 복제 제외 플래그의 조합은 RMC2202다.
    private static void RegisterFieldFlags(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.FieldFlagSpecifiersReachDescriptorAndSchema", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(Declaration("ROCKS_PROPERTY(DuplicateTransient, SaveGame) rocks::int32 count = 0;\n" +
                "ROCKS_PROPERTY(NonPIEDuplicateTransient) rocks::int32 plain = 0;\n" +
                "ROCKS_PROPERTY(TextExportTransient, Required) rocks::ObjectRef<Value> next;"));
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Field(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == "probe::Value").GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string[] Flags(string name) => Field(name).GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
            Verify.That(Flags("count").SequenceEqual(["DuplicateTransient", "SaveGame"]), "duplicate transient and save game flags");
            Verify.That(Flags("plain").SequenceEqual(["NonPIEDuplicateTransient"]), "non-PIE duplicate transient flag");
            Verify.That(Flags("next").SequenceEqual(["Required", "TextExportTransient"]), "text export transient with Required");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("FieldKind::Value,80u,", generated, "DuplicateTransient | SaveGame bits");
            Verify.Contains("FieldKind::Value,32u,", generated, "NonPIEDuplicateTransient bit");
            Verify.Contains("FieldKind::ObjectReference,136u,", generated, "Required | TextExportTransient bits");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated field flag header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        AddRejectGroup(context, "Meta.RequiredCannotBeDuplicateTransient", [
            Declaration("ROCKS_PROPERTY(Required, DuplicateTransient) rocks::ObjectRef<Value> next;"),
            Declaration("ROCKS_PROPERTY(Required, NonPIEDuplicateTransient) rocks::ObjectRef<Value> next;")],
            "RMC2202", "cannot be combined with DuplicateTransient");
    }
}
