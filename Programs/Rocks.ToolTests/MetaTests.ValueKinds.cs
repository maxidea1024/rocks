using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 지정자와 값 종류(enum·정수 폭·Name·WeakRef) 시험이다.
internal static partial class MetaTests
{
    // CLASS·PROPERTY 지정자(B2), enum·정수 폭·Name·WeakRef·평범한 char 등 값 종류 시험이다.
    private static void RegisterValueKinds(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        // B2: CLASS·PROPERTY 지정자가 descriptor와 생성 스키마에 닿는다(Docs/Design/ReflectionFoundations.ko.md 2절).
        suite.Add("Meta.ClassAndPropertySpecifiersReachTheSchema", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble.Replace("namespace probe {", "#include <Object/SoftObjectRef.h>\nnamespace probe {", StringComparison.Ordinal) +
                "struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
                "struct Cache { rocks::ObjectRef<rocks::Object> ref; };\n" +
                "struct Holder { rocks::ObjectRef<rocks::Object> ref; Lease lease; };\n" +
                "ROCKS_CLASS(WorldOnly, Version=2147483647)\nstruct Parent : public rocks::Object {};\n" +
                "ROCKS_CLASS(Version=3)\nstruct Value : public Parent {\n" +
                "ROCKS_PROPERTY(EditorOnly, Name=\"Hits\") rocks::int32 count = 0;\n" +
                "ROCKS_PROPERTY(Required) rocks::ObjectRef<Value> next;\n" +
                "ROCKS_PROPERTY(Name=\"other\") rocks::int32 other = 0;\n" +
                "ROCKS_NATIVE() rocks::int32 cache = 0;\n" +
                "ROCKS_NATIVE() rocks::WeakRef<Value> weak;\n" +
                "ROCKS_NATIVE() int* raw = nullptr;\n" +
                "ROCKS_NATIVE() Cache* borrowed = nullptr;\n" +
                "ROCKS_NATIVE() Lease lease;\n" +
                // 소프트 참조는 대상을 담지 않으므로 인자(Holder)의 강한 참조를 따라가지 않는다(RMC2205 아님).
                "ROCKS_NATIVE() rocks::SoftObjectRef<Holder> soft;\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : soft(allocator) {}\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement value = Type("probe::Value");
            Verify.Equal(3, value.GetProperty("version").GetInt32(), "Version specifier");
            Verify.Equal("WorldOnly", value.GetProperty("flags")[0].GetString(), "WorldOnly is inherited from the base");
            Verify.Equal("WorldOnly", Type("probe::Parent").GetProperty("flags")[0].GetString(), "WorldOnly specifier");
            Verify.Equal(int.MaxValue, Type("probe::Parent").GetProperty("version").GetInt32(), "largest Version value");
            System.Text.Json.JsonElement Field(string name) => value.GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            Verify.Equal("EditorOnly", Field("count").GetProperty("flags")[0].GetString(), "EditorOnly flag");
            Verify.Equal("Hits", Field("count").GetProperty("toolName").GetString(), "Name specifier");
            Verify.Equal("Required", Field("next").GetProperty("flags")[0].GetString(), "Required flag");
            Verify.Equal("other", Field("other").GetProperty("toolName").GetString(), "Name equal to the field name is the default");
            Verify.Equal(3, value.GetProperty("fields").GetArrayLength(), "NATIVE members are not schema fields");
            string generated = File.ReadAllText(probe.Output);
            Verify.Contains("IsNativeStrongReference", generated, "NATIVE strong reference static_assert");
            Verify.Contains("::probe::Value::lease", generated, "NATIVE member static_assert target");
            Verify.That(!generated.Contains("RequiresWorldOnlyOwner<", StringComparison.Ordinal), "WorldOnly owner needs no RMC2206 static_assert");
        });
        // B3b: ROCKS_ENUM이 descriptor와 생성 스키마·검증기·집계 등록 순서에 닿는다(Docs/Design/ReflectionFoundations.ko.md 3.4절).
        suite.Add("Meta.EnumSchemaReachesDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "ROCKS_ENUM(ToolPrefix = \"MODE_\")\nenum class Mode : rocks::uint8 {\n" +
                "Off = 0,\nEight ROCKS_ENUMERATOR(Name = \"8Bit\", DisplayName = \"여덟\") = 8,\nNext,\nSecret ROCKS_ENUMERATOR(Hidden) = 200,\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "ROCKS_ENUM(Flags)\nenum class Nested : rocks::uint32 { None = 0, A = 1, B = 0x80000000u };\n" +
                "ROCKS_PROPERTY() Mode mode = Mode::Off;\nROCKS_PROPERTY() Nested nested = Nested::None;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement mode = Type("probe::Mode");
            Verify.Equal("enum", mode.GetProperty("kind").GetString(), "enum descriptor kind");
            Verify.Equal("UInt8", mode.GetProperty("underlying").GetString(), "underlying");
            Verify.Equal("MODE_", mode.GetProperty("toolPrefix").GetString(), "tool prefix");
            System.Text.Json.JsonElement[] values = mode.GetProperty("values").EnumerateArray().ToArray();
            Verify.Equal(4, values.Length, "enumerator count");
            Verify.Equal("8Bit", values[1].GetProperty("toolName").GetString(), "enumerator tool name");
            Verify.Equal(9, values[2].GetProperty("value").GetInt32(), "implicit value is previous + 1");
            Verify.Equal("Hidden", values[3].GetProperty("flags")[0].GetString(), "hidden enumerator");
            Verify.Equal("Flags", Type("probe::Value::Nested").GetProperty("flags")[0].GetString(), "nested Flags enum");
            System.Text.Json.JsonElement field = Type("probe::Value").GetProperty("fields").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == "mode");
            Verify.Equal("Enum", field.GetProperty("value").GetProperty("kind").GetString(), "enum field kind");
            Verify.Equal(mode.GetProperty("id").GetString(), field.GetProperty("value").GetProperty("type").GetString(), "enum field target");
            string generated = File.ReadAllText(probe.Output);
            Verify.Contains("struct GeneratedType<::probe::Mode>", generated, "enum specialization");
            Verify.Contains("static constexpr bool IsDeclared", generated, "constexpr validator");
            Verify.Contains("NativeTypeKey<::probe::Mode>()", generated, "enum field native key");
            Verify.That(generated.IndexOf("GeneratedType<::probe::Mode>", StringComparison.Ordinal) <
                generated.IndexOf("GeneratedType<::probe::Value>", StringComparison.Ordinal), "enums are emitted before classes");
        });
        // underlying은 UE와 같은 정수 8종만 받는다(리플렉션 B9a2 — Docs/Design/ReflectionExtensions.ko.md 2절): 평범한 char(부호가
        // 구현 정의)·bool·long(폭이 플랫폼마다 다르다)·wchar_t는 거부한다.
        AddRejectGroup(context, "Meta.EnumUnderlyingAndValuesAreRestricted", new[]
            {
                "ROCKS_ENUM()\nenum class Mode { A };",
                "ROCKS_ENUM()\nenum class Mode : char { A };",
                "ROCKS_ENUM()\nenum class Mode : bool { A };",
                "ROCKS_ENUM()\nenum class Mode : long { A };",
                "ROCKS_ENUM()\nenum class Mode : wchar_t { A };",
                "ROCKS_ENUM()\nenum Mode : rocks::uint8 { A };",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A = 1, B = 1 };",
                "ROCKS_ENUM(Flags)\nenum class Mode : rocks::int32 { A = -1 };",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 {};",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8;",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint16 { " + string.Join(", ", Enumerable.Range(0, 4097).Select(index => "E" + index)) + " };"
            }.Select(declaration => Preamble + declaration + "\n" + "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"), "RMC2401", "probe::Mode");
        // uint64 열거자 값은 int64 최댓값 이하다(Flags는 비트 0–62 — 저장 값이 int64). 일반 범위 검사도 같은 RMC2401을 내므로 전용
        // 문구로 전용 검사를 고정한다(UE는 넘는 값을 (int64) 변환으로 음수로 감싸 받는다 — Docs/Design/ReflectionExtensions.ko.md 18.1절).
        AddRejectGroup(context, "Meta.EnumUint64ValuesAboveInt64AreRejected", new[]
            {
                "ROCKS_ENUM()\nenum class Mode : rocks::uint64 { A = 0x8000000000000000ull };",
                "ROCKS_ENUM(Flags)\nenum class Mode : rocks::uint64 { None = 0, Top = 0x8000000000000000ull };"
            }.Select(declaration => Preamble + declaration + "\n" + "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"), "RMC2401",
            "exceeds the int64 maximum");
        AddRejectGroup(context, "Meta.EnumToolNamesAreValidAndUnique", new[]
            {
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A ROCKS_ENUMERATOR(Name = \"b\"), B };",
                "ROCKS_ENUM(ToolPrefix = \"M_\")\nenum class Mode : rocks::uint8 { A, M_A };",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A ROCKS_ENUMERATOR(Name = \"has space\") };",
                "ROCKS_ENUM(ToolPrefix = \"bad-\")\nenum class Mode : rocks::uint8 { A };"
            }.Select(declaration => Preamble + declaration + "\n}\n"), "RMC2203", "probe::Mode");
        AddRejectGroup(context, "Meta.EnumMarkerMustPrecedeAnEnum", [Preamble + "ROCKS_ENUM()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "immediately followed by a named enum");
        AddRejectGroup(context, "Meta.EnumeratorAnnotationNeedsReflectedEnum", [
            Preamble + "enum class Mode : rocks::uint8 { A ROCKS_ENUMERATOR(Hidden) };\nROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "ROCKS_ENUMERATOR must annotate");
        AddRejectGroup(context, "Meta.NestedReflectedEnumMustBePublic", [
            Preamble + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_ENUM()\nenum class Nested : rocks::uint8 { A };\n};\n}\n"],
            "RMC2401", "must be public");
        // underlying 명시 판정(주석·줄바꿈 사이, enum struct)과 식 초기값·int32 최솟값·uint32 최댓값을 받는다. 리플렉션 B9a2의
        // underlying 8종(int8·int16·int64·uint64 — 경계값, int64 최솟값 리터럴, uint64 최댓값 = int64 최댓값, Flags uint64 비트 62)도
        // descriptor와 생성 코드에 닿고 생성 헤더가 컴파일된다(static_assert·IsDeclared).
        suite.Add("Meta.EnumValuesFollowClangEvaluation", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "constexpr int Base = 21;\n" +
                "ROCKS_ENUM()\nenum struct Mode /* explicit */\n    : rocks::int32 { Min = -2147483647 - 1, A = -3, B, C = B + 10, D = Base * 2 };\n" +
                "struct Outer {\nROCKS_ENUM()\nenum class Inner : rocks::uint32 { Max = 0xffffffffu };\n};\n" +
                "ROCKS_ENUM()\nenum class Tiny : rocks::int8 { Low = -128, Zero, High = 127 };\n" +
                "ROCKS_ENUM()\nenum class Signed : signed char { Only = -1 };\n" +
                "ROCKS_ENUM()\nenum class Short : rocks::int16 { Low = -32768, High = 32767 };\n" +
                "ROCKS_ENUM()\nenum class Wide : rocks::int64 { Low = -9223372036854775807ll - 1, High = 9223372036854775807ll };\n" +
                "ROCKS_ENUM()\nenum class Huge : rocks::uint64 { Zero, Top = 0x7fffffffffffffffull };\n" +
                "ROCKS_ENUM(Flags)\nenum class Bits : rocks::uint64 { None = 0, Low = 1, Bit62 = 1ull << 62 };\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Mode mode = Mode::A;\nROCKS_PROPERTY() Outer::Inner nested = Outer::Inner::Max;\n" +
                "ROCKS_PROPERTY() Tiny tiny = Tiny::Zero;\nROCKS_PROPERTY() Signed signedValue = Signed::Only;\nROCKS_PROPERTY() Short shortValue = Short::Low;\n" +
                "ROCKS_PROPERTY() Wide wide = Wide::Low;\nROCKS_PROPERTY() Huge huge = Huge::Zero;\nROCKS_PROPERTY() Bits bits = Bits::None;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            long[] Values(string name) => Type(name).GetProperty("values").EnumerateArray().Select(value => value.GetProperty("value").GetInt64()).ToArray();
            Verify.That(Values("probe::Mode").SequenceEqual(new long[] { -2147483648, -3, -2, 8, 42 }), "evaluated enumerator values");
            Verify.That(Values("probe::Outer::Inner").SequenceEqual(new long[] { 4294967295 }), "uint32 maximum");
            Verify.That(Values("probe::Tiny").SequenceEqual(new long[] { -128, -127, 127 }), "int8 range");
            Verify.That(Values("probe::Short").SequenceEqual(new long[] { -32768, 32767 }), "int16 range");
            Verify.That(Values("probe::Wide").SequenceEqual(new long[] { long.MinValue, long.MaxValue }), "int64 range");
            Verify.That(Values("probe::Huge").SequenceEqual(new long[] { 0, long.MaxValue }), "uint64 maximum is the int64 maximum");
            Verify.That(Values("probe::Bits").SequenceEqual(new long[] { 0, 1, 1L << 62 }), "uint64 Flags bit 62");
            foreach ((string name, string underlying) in new[]
            {
                ("probe::Tiny", "Int8"), ("probe::Signed", "Int8"), ("probe::Short", "Int16"), ("probe::Wide", "Int64"), ("probe::Huge", "UInt64"),
                ("probe::Bits", "UInt64")
            })
            {
                Verify.Equal(underlying, Type(name).GetProperty("underlying").GetString(), name + " underlying");
            }

            string generated = File.ReadAllText(probe.Output);
            Verify.Contains("-2147483648ll", generated, "int32 minimum literal");
            Verify.Contains("static_assert(static_cast<int64>(::probe::Mode::D) == 42ll", generated, "value static_assert");
            string compact = Compact(generated);
            Verify.Contains("(-9223372036854775807ll-1)", compact, "int64 minimum is written as an expression");
            Verify.Contains("__underlying_type(::probe::Wide),int64)", compact, "int64 underlying static_assert");
            Verify.Contains("__underlying_type(::probe::Tiny),int8)", compact, "int8 underlying static_assert");
            Verify.Contains("ValueKind::UInt64", generated, "uint64 underlying schema");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated 8-underlying enum header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        AddRejectGroup(context, "Meta.EnumFieldsRejectRequired", [
            Preamble + "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A };\nROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY(Required) Mode count = Mode::A;\n};\n}\n"],
            "RMC2202", "applies only to");
        AddRejectGroup(context, "Meta.EnumIsNotABaseOrReferenceTarget", [
            Preamble.Replace("namespace probe {", "#include <Object/SoftObjectRef.h>\nnamespace probe {", StringComparison.Ordinal) +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A };\nROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : next(allocator) {}\nROCKS_PROPERTY() rocks::SoftObjectRef<Mode> next;\n};\n}\n"],
            "RMC2003", "enum");
        AddRejectGroup(context, "Meta.UnreflectedEnumFieldIsUnsupported", [
            Preamble + "enum class Mode : rocks::uint8 { A };\nROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Mode count = Mode::A;\n};\n}\n"],
            "RMC2000", "unsupported field type");
        // 리플렉션 B9a2: int8·int16(별칭과 Clang 철자 signed char·short) 클래스 필드·struct 멤버·배열 원소가 Int8·Int16 종류로
        // descriptor·생성 스키마·경로 thunk에 닿고 생성 헤더가 컴파일된다(Docs/Design/ReflectionExtensions.ko.md 2절).
        suite.Add("Meta.IntegerWidthFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = "#include <Core/Containers/Array.h>\n" + Preamble +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int8 tiny = 0;\nROCKS_PROPERTY() short narrow = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : tinies(allocator), narrows(allocator) {}\n" +
                "ROCKS_PROPERTY() signed char tiny = 0;\nROCKS_PROPERTY() rocks::int16 narrow = 0;\nROCKS_PROPERTY() Leaf leaf;\n" +
                "ROCKS_PROPERTY() rocks::Array<rocks::int8> tinies;\nROCKS_PROPERTY() rocks::Array<rocks::int16> narrows;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Field(string type, string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            Verify.Equal("Int8", Field("probe::Value", "tiny").GetProperty("value").GetProperty("kind").GetString(), "signed char field");
            Verify.Equal("Int16", Field("probe::Value", "narrow").GetProperty("value").GetProperty("kind").GetString(), "int16 field");
            Verify.Equal("Int8", Field("probe::Leaf", "tiny").GetProperty("value").GetProperty("kind").GetString(), "int8 struct member");
            Verify.Equal("Int16", Field("probe::Leaf", "narrow").GetProperty("value").GetProperty("kind").GetString(), "short struct member");
            Verify.Equal("Int8", Field("probe::Value", "tinies").GetProperty("element").GetProperty("kind").GetString(), "int8 array element");
            Verify.Equal("Int16", Field("probe::Value", "narrows").GetProperty("element").GetProperty("kind").GetString(), "int16 array element");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("PathStructMember<&::probe::Leaf::tiny,ValueKind::Int8>", generated, "int8 member path thunk");
            Verify.Contains("{ValueKind::Int16,Guid{}},65536u}", generated, "int16 array element schema");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated integer width header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // 리플렉션 B9c2: rocks::Name 클래스 필드·struct 멤버·Array<Name> 원소가 descriptor의 Name 종류와 생성 코드(스키마·경로 thunk)에
        // 닿고 생성 헤더가 컴파일된다(Docs/Design/ReflectionExtensions.ko.md 4절).
        suite.Add("Meta.NameFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = "#include <Core/Containers/Array.h>\n#include <Core/Text/Name.h>\n" + Preamble +
                "ROCKS_STRUCT()\nstruct Leaf {\nexplicit Leaf(rocks::Allocator& allocator) noexcept : aliases(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::Name socket;\nROCKS_PROPERTY() rocks::Array<rocks::Name> aliases;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : leaf(allocator), names(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::Name label;\nROCKS_PROPERTY() Leaf leaf;\nROCKS_PROPERTY() rocks::Array<rocks::Name> names;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Field(string type, string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            Verify.Equal("Name", Field("probe::Value", "label").GetProperty("value").GetProperty("kind").GetString(), "Name class field");
            Verify.Equal("Name", Field("probe::Leaf", "socket").GetProperty("value").GetProperty("kind").GetString(), "Name struct member");
            Verify.Equal("Name", Field("probe::Value", "names").GetProperty("element").GetProperty("kind").GetString(), "Name array element");
            Verify.Equal("Name", Field("probe::Leaf", "aliases").GetProperty("element").GetProperty("kind").GetString(), "Name member array element");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("PathStructMember<&::probe::Leaf::socket,ValueKind::Name>", generated, "Name member path thunk");
            Verify.Contains("{ValueKind::Name,Guid{}},65536u}", generated, "Name array element schema");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated Name header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // Name은 Required를 받지 않는다(성질 표 AcceptsRequired 없음 — 단일 필드·Array<Name> 모두 RMC2202).
        AddRejectGroup(context, "Meta.NameRejectsRequired", [
            Declaration("ROCKS_PROPERTY(Required) rocks::Name label;")
                .Replace("namespace probe {", "#include <Core/Text/Name.h>\nnamespace probe {", StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY(Required) rocks::Array<rocks::Name> names;",
                    extra: "explicit Value(rocks::Allocator& allocator) noexcept : names(allocator) {}")
                .Replace("namespace probe {", "#include <Core/Containers/Array.h>\n#include <Core/Text/Name.h>\nnamespace probe {",
                    StringComparison.Ordinal)],
            "RMC2202", "applies only to");
        // 리플렉션 B9d1: rocks::WeakRef<T> 클래스 필드·struct 멤버·Array<WeakRef<T>> 원소가 descriptor의 WeakObjectReference 종류(값 필드 —
        // fieldKind Value, 대상 ID는 기대 타입·Object는 비어 있음)와 생성 코드(스키마·경로 thunk)에 닿고, 생성 Trace는 weak 필드를 방문하지
        // 않으며 생성 헤더가 컴파일된다(Docs/Design/ReflectionExtensions.ko.md 5.1절).
        suite.Add("Meta.WeakReferenceFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = "#include <Core/Containers/Array.h>\n" + Preamble +
                "ROCKS_STRUCT()\nstruct Leaf {\nexplicit Leaf(rocks::Allocator& allocator) noexcept : links(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::WeakRef<> anything;\nROCKS_PROPERTY() rocks::Array<rocks::WeakRef<rocks::Object>> links;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : leaf(allocator), peers(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::WeakRef<Value> target;\nROCKS_PROPERTY() Leaf leaf;\n" +
                "ROCKS_PROPERTY() rocks::Array<rocks::WeakRef<Value>> peers;\nROCKS_PROPERTY() rocks::ObjectRef<Value> strong;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement Field(string type, string name) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string valueId = Type("probe::Value").GetProperty("id").GetString()!;
            System.Text.Json.JsonElement target = Field("probe::Value", "target");
            Verify.Equal("WeakObjectReference", target.GetProperty("value").GetProperty("kind").GetString(), "weak class field");
            Verify.Equal(valueId, target.GetProperty("value").GetProperty("type").GetString(), "weak field expected type");
            Verify.Equal("Value", target.GetProperty("fieldKind").GetString(), "a weak field is not a strong GC edge");
            Verify.Equal("WeakObjectReference", Field("probe::Leaf", "anything").GetProperty("value").GetProperty("kind").GetString(), "weak struct member");
            System.Text.Json.JsonElement peers = Field("probe::Value", "peers");
            Verify.Equal("WeakObjectReference", peers.GetProperty("element").GetProperty("kind").GetString(), "weak array element");
            Verify.Equal(valueId, peers.GetProperty("element").GetProperty("type").GetString(), "weak element expected type");
            Verify.Equal("Value", peers.GetProperty("fieldKind").GetString(), "a weak array is a value field");
            Verify.Equal("WeakObjectReference", Field("probe::Leaf", "links").GetProperty("element").GetProperty("kind").GetString(), "weak member array element");
            Verify.That(!Type("probe::Leaf").GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "ContainsStrongReference"),
                "weak members do not make a reference struct");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("PathStructMember<&::probe::Leaf::anything,ValueKind::WeakObjectReference>", generated, "weak member path thunk");
            // 스키마의 GC 간선은 강한 참조 strong 하나뿐이고(weak 필드·배열은 FieldKind::Value), 생성 Trace는 strong만 방문한다.
            Verify.Equal(1, Regex.Count(generated, "FieldKind::ObjectReference", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10)),
                "only the strong field is an ObjectReference field kind");
            Verify.Contains("visitor.Visit(value.*(&::probe::Value::strong))", generated, "generated Trace visits the strong field");
            Verify.That(!generated.Contains("visitor.Visit(value.*(&::probe::Value::target))", StringComparison.Ordinal), "generated Trace does not visit weak fields");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated weak reference header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // WeakRef는 Required를 받지 않는다(대상이 죽으면 저절로 null — 단일 필드·Array<WeakRef> 모두 RMC2202).
        AddRejectGroup(context, "Meta.WeakReferenceRejectsRequired", [
            Declaration("ROCKS_PROPERTY(Required) rocks::WeakRef<Value> target;"),
            "#include <Core/Containers/Array.h>\n" + Declaration("ROCKS_PROPERTY(Required) rocks::Array<rocks::WeakRef<Value>> targets;",
                extra: "explicit Value(rocks::Allocator& allocator) noexcept : targets(allocator) {}")],
            "RMC2202", "cannot be Required");
        // WeakRef의 대상은 리플렉션 클래스(또는 Object)다: 리플렉션 struct·enum·리플렉션되지 않은 타입은 RMC2003.
        AddRejectGroup(context, "Meta.WeakReferenceTargetMustBeReflectedClass", [
            Preamble + "ROCKS_STRUCT()\nstruct Leaf { ROCKS_PROPERTY() rocks::int32 count = 0; };\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::WeakRef<Leaf> target;\n};\n}\n",
            Preamble + "struct Plain : public rocks::Object {};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::WeakRef<Plain> target;\n};\n}\n"],
            "RMC2003", "reference");
        // 평범한 char는 부호가 구현 정의라 필드·배열 원소 모두 받지 않고 int8·uint8을 안내한다.
        AddRejectGroup(context, "Meta.PlainCharFieldIsUnsupported", [
            Declaration("ROCKS_PROPERTY() char letter = 0;"),
            "#include <Core/Containers/Array.h>\n" + Declaration("ROCKS_PROPERTY() rocks::Array<char> letters;",
                extra: "explicit Value(rocks::Allocator& allocator) noexcept : letters(allocator) {}")],
            "RMC2000", "implementation-defined signedness");
    }
}
