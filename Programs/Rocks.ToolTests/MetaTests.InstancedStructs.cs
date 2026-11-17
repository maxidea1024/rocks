using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9j InstancedStruct PROPERTY 시험이다(Docs/Design/ReflectionExtensions.ko.md 11·15절).
internal static partial class MetaTests
{
    private const string InstancedPreamble = "#include <Core/Containers/Array.h>\n#include <Core/Containers/OrderedMap.h>\n" +
        "#include <Core/Containers/OrderedSet.h>\n#include <Core/Optional.h>\n#include <Object/InstancedStruct.h>\n" + Preamble;

    // InstancedStruct<> 클래스 필드·Array 원소·정적 배열 원소·Map 값·struct 멤버가 descriptor(value {InstancedStruct}, 대상 type 없음)와
    // 생성 코드(스키마 {InstancedStruct, 0}, read·write 없는 aggregate 함수표와 경로 thunk, struct 형질 ContainsInstancedStruct·
    // ContainsRequired)에 닿고 생성 헤더가 컴파일된다. InstancedStruct::Emplace<S>는 InstancedStruct를 담은 struct를 static_assert로
    // 거부한다(생성 형질 ContainsInstancedStruct).
    private static void RegisterInstancedStructs(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.InstancedStructFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = InstancedPreamble +
                "ROCKS_STRUCT()\nstruct Host {\nROCKS_PROPERTY() rocks::InstancedStruct<> row;\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Plain {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : rows(allocator), table(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::InstancedStruct<> row;\nROCKS_PROPERTY() rocks::Array<rocks::InstancedStruct<>> rows;\n" +
                "ROCKS_PROPERTY() rocks::InstancedStruct<> fixed[2];\n" +
                "ROCKS_PROPERTY() rocks::OrderedMap<rocks::uint32, rocks::InstancedStruct<>> table;\nROCKS_PROPERTY() Host host;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Field(string type, string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement row = Field("probe::Value", "row");
            Verify.Equal("InstancedStruct", row.GetProperty("value").GetProperty("kind").GetString(), "instanced field kind");
            Verify.Equal("", row.GetProperty("value").GetProperty("type").GetString(), "InstancedStruct<> has no base type");
            Verify.Equal("Value", row.GetProperty("fieldKind").GetString(), "an instanced struct is a value field");
            Verify.Equal("InstancedStruct", Field("probe::Value", "rows").GetProperty("element").GetProperty("kind").GetString(), "array element");
            Verify.Equal("InstancedStruct", Field("probe::Value", "fixed").GetProperty("element").GetProperty("kind").GetString(), "static array element");
            Verify.Equal("InstancedStruct", Field("probe::Value", "table").GetProperty("element").GetProperty("kind").GetString(), "map value");
            Verify.Equal("InstancedStruct", Field("probe::Host", "row").GetProperty("value").GetProperty("kind").GetString(), "struct member");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("{ValueKind::InstancedStruct,Guid{}}", generated, "generated instanced schema");
            Verify.Contains("PathClassField<::probe::Value,::rocks::ValueKind::InstancedStruct>(cursor,&::probe::Value::row)", generated,
                "an InstancedStruct field has a path thunk");
            Verify.Contains("PathStructMember<::probe::Host,&::probe::Host::row,ValueKind::InstancedStruct>", generated, "struct member path thunk");
            Verify.That(!generated.Contains("ReadGeneratedField<::probe::Value>(registry,access,object,&::probe::Value::row", StringComparison.Ordinal),
                "an InstancedStruct field is not read through FieldValue");
            int host = generated.IndexOf("structGeneratedType<::probe::Host>", StringComparison.Ordinal);
            int plain = generated.IndexOf("structGeneratedType<::probe::Plain>", StringComparison.Ordinal);
            Verify.That(host >= 0 && plain >= 0, "generated struct specializations");
            Verify.Contains("staticconstexprboolContainsInstancedStruct=true;", generated[host..], "Host holds an InstancedStruct");
            Verify.Contains("staticconstexprboolContainsInstancedStruct=false;", generated[plain..], "Plain holds no InstancedStruct");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated instanced header compiles: " + compiled.StdOut + compiled.StdErr);

            // Emplace<S>는 InstancedStruct를 담은 struct를 컴파일에서 거부하고 담을 수 있는 struct는 받는다.
            string original = File.ReadAllText(probe.Output);
            const string accepted = "\nnamespace probe {\ninline void UsePlain(rocks::TypeRegistry& types, rocks::Allocator& allocator) noexcept {\n" +
                "rocks::InstancedStruct<> value;\n(void)value.Emplace<Plain>(types, allocator);\n}\n}\n";
            File.WriteAllText(probe.Output, original + accepted);
            ProcessResult plainCompiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, plainCompiled.ExitCode, "Emplace<Plain> compiles: " + plainCompiled.StdOut + plainCompiled.StdErr);
            File.WriteAllText(probe.Output, original + accepted.Replace("Emplace<Plain>", "Emplace<Host>", StringComparison.Ordinal));
            ProcessResult rejected = await probe.CompileGeneratedAsync();
            Verify.That(rejected.ExitCode != 0, "Emplace<Host> compiled");
            Verify.Contains("InstancedStruct cannot hold a struct that holds an InstancedStruct", rejected.StdOut + rejected.StdErr,
                "Emplace static_assert diagnostic");
        });
        // Required는 InstancedStruct 자리(필드·원소)에 쓸 수 없다(성질 표 AcceptsRequired — RMC2202).
        AddRejectGroup(context, "Meta.InstancedStructRejectsRequired", [
            Declaration("ROCKS_PROPERTY(Required) rocks::InstancedStruct<> row;").Replace(Preamble, InstancedPreamble, StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY(Required) rocks::Array<rocks::InstancedStruct<>> rows;",
                extra: "explicit Value(rocks::Allocator& allocator) noexcept : rows(allocator) {}").Replace(Preamble, InstancedPreamble, StringComparison.Ordinal)],
            "RMC2202", "Required");
        // Optional 내부(RMC2306)·Set 원소·Map 키(RMC2305)는 InstancedStruct 자리가 아니다.
        AddRejectGroup(context, "Meta.InstancedStructOptionalInnerIsRejected", [
            Declaration("ROCKS_PROPERTY() rocks::Optional<rocks::InstancedStruct<>> row;").Replace(Preamble, InstancedPreamble, StringComparison.Ordinal)],
            "RMC2306", "InstancedStruct cannot be an Optional inner type");
        AddRejectGroup(context, "Meta.InstancedStructKeyPositionsAreRejected", [
            Declaration("ROCKS_PROPERTY() rocks::OrderedSet<rocks::InstancedStruct<>> rows;",
                extra: "explicit Value(rocks::Allocator& allocator) noexcept : rows(allocator) {}").Replace(Preamble, InstancedPreamble, StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::InstancedStruct<>, rocks::int32> rows;",
                extra: "explicit Value(rocks::Allocator& allocator) noexcept : rows(allocator) {}").Replace(Preamble, InstancedPreamble, StringComparison.Ordinal)],
            "RMC2305", "InstancedStruct cannot be a");
    }
}
