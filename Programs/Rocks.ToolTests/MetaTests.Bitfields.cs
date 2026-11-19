using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9m bitfield bool 시험이다(Docs/Design/ReflectionExtensions.ko.md 14·15절).
internal static partial class MetaTests
{
    // 폭 1 부호 없는 정수 bitfield 4종(uint8·16·32·64)이 Bool 필드로 descriptor(`"bitfield": true`)와 생성 코드(접근자
    // Get_·Set_, 접근자 판 함수표)에 닿고, NATIVE 정수·bool bitfield와 ReadOnly·private bitfield도 받으며 생성 헤더가 컴파일된다.
    private static void RegisterBitfields(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.BitfieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble +
                "ROCKS_STRUCT()\nstruct Flags {\nROCKS_PROPERTY() rocks::uint8 on : 1 = 0;\nROCKS_PROPERTY() rocks::uint16 wide : 1 = 0;\n" +
                "ROCKS_PROPERTY() rocks::uint32 wider : 1 = 0;\nROCKS_PROPERTY() rocks::uint64 widest : 1 = 0;\nROCKS_NATIVE() rocks::uint8 spare : 4 = 0;\n" +
                "ROCKS_NATIVE() bool cached : 1 = false;\nROCKS_PROPERTY() rocks::uint8 plain = 0;\n};\n" +
                "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "ROCKS_PROPERTY() rocks::uint8 visible : 1 = 0;\nROCKS_PROPERTY(ReadOnly) rocks::uint8 locked : 1 = 0;\n" +
                "ROCKS_PROPERTY() Flags flags;\nprivate:\nROCKS_PROPERTY() rocks::uint8 hidden : 1 = 0;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Field(string type, string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            foreach ((string type, string name) in new[] { ("probe::Flags", "on"), ("probe::Flags", "wide"), ("probe::Flags", "wider"),
                ("probe::Flags", "widest"), ("probe::Value", "visible"), ("probe::Value", "hidden") })
            {
                Verify.Equal("Bool", Field(type, name).GetProperty("value").GetProperty("kind").GetString(), "bitfield value kind: " + name);
                Verify.That(Field(type, name).GetProperty("bitfield").GetBoolean(), "descriptor bitfield: " + name);
            }

            Verify.That(!Field("probe::Flags", "plain").GetProperty("bitfield").GetBoolean(), "a plain member is not a bitfield");
            string generated = Compact(probe.GeneratedText());
            foreach ((string expected, string reason) in new[]
            {
                ("#include <Object/GeneratedBitfieldAccess.h>", "bitfield function table header"),
                ("static bool Get_on(const ::probe::Flags& value) noexcept;", "struct getter declaration"),
                ("bool GeneratedType<::probe::Flags>::Get_on(const ::probe::Flags& value) noexcept { return value.on != 0; }", "struct getter"),
                ("static void Set_hidden(::probe::Value& value, bool bit) noexcept;", "private class setter declaration"),
                ("void GeneratedType<::probe::Value>::Set_hidden(::probe::Value& value, bool bit) noexcept {", "private class setter"),
                ("detail::BitfieldStructFieldOps<::probe::Flags, &GeneratedType<::probe::Flags>::Get_on, &GeneratedType<::probe::Flags>::Set_on>(", "struct member ops"),
                ("detail::BitfieldFieldOps<::probe::Value, &GeneratedType<::probe::Value>::Get_visible, &GeneratedType<::probe::Value>::Set_visible, false>(", "class field ops"),
                ("detail::BitfieldFieldOps<::probe::Value, &GeneratedType<::probe::Value>::Get_locked, &GeneratedType<::probe::Value>::Set_locked, true>(", "ReadOnly class field ops"),
                ("copy.spare = source.spare;", "NATIVE bitfield copy is an assignment"),
                ("copy.on = source.on;", "PROPERTY bitfield copy is an assignment")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated bitfield header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // PROPERTY bitfield는 폭 1 부호 없는 정수만이다: `bool b : 1`(UE와 같이 거부)·폭 3·부호 있는 정수·enum은 RMC2207이다.
        AddRejectGroup(context, "Meta.BitfieldPropertyRules", [
            Declaration("ROCKS_PROPERTY() bool flag : 1 = false;"),
            Declaration("ROCKS_PROPERTY() rocks::uint8 flag : 3 = 0;"),
            Declaration("ROCKS_PROPERTY() rocks::int8 flag : 1 = 0;"),
            Declaration("ROCKS_PROPERTY() Mode flag : 1 = Mode::A;").Replace("namespace probe {", "namespace probe { enum class Mode : rocks::uint8 { A };", StringComparison.Ordinal)],
            "RMC2207", "bitfield");
        // Binds 미러 안 bitfield는 RMC2207이다(미러 대조가 멤버 포인터·구조적 바인딩을 쓴다).
        AddRejectGroup(context, "Meta.BitfieldInBindsMirror", [
            Preamble + "struct Real {\nrocks::uint8 a : 1;\n};\nROCKS_STRUCT(Binds = \"probe::Real\")\nstruct RealBinding {\nROCKS_PROPERTY() rocks::uint8 a : 1;\n};\n}\n"],
            "RMC2207", "Binds mirror", BindsModule);
        // NATIVE bitfield는 정수·bool만이다(enum bitfield는 RMC2207).
        AddRejectGroup(context, "Meta.NativeBitfieldMustBeInteger", [
            Declaration("ROCKS_NATIVE() Mode cached : 2 = Mode::A;").Replace("namespace probe {", "namespace probe { enum class Mode : rocks::uint8 { A };", StringComparison.Ordinal)],
            "RMC2207", "integer or bool");
    }
}
