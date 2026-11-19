using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9e Optional PROPERTY 시험이다(Docs/Design/ReflectionExtensions.ko.md 6·15절).
internal static partial class MetaTests
{
    private const string OptionalPreamble = "#include <Core/Containers/Array.h>\n#include <Core/Optional.h>\n#include <Core/Text/Name.h>\n" + Preamble;

    // Optional 클래스 필드(단일 값·String·Name·enum·값 struct)와 struct 멤버가 descriptor(value {Optional}·element 내부 형식·
    // maxElements 0)와 생성 코드(스키마 {Optional, 0} + 내부 형식, 단일 값 내부는 read·write, 값 struct 내부는 경로 thunk,
    // struct 멤버 경로 thunk의 대상 종류는 내부 종류)에 닿고 생성 헤더가 컴파일된다.
    private static void RegisterOptionals(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.OptionalFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = OptionalPreamble +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { First = 1, Second = 2 };\n" +
                "ROCKS_STRUCT()\nstruct Leaf {\nexplicit Leaf(rocks::Allocator& allocator) noexcept : label(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::String label;\nROCKS_PROPERTY() rocks::Optional<rocks::uint32> weight;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "ROCKS_PROPERTY() rocks::Optional<rocks::int32> count;\nROCKS_PROPERTY() rocks::Optional<rocks::String> text;\n" +
                "ROCKS_PROPERTY() rocks::Optional<rocks::Name> name;\nROCKS_PROPERTY() rocks::Optional<Mode> mode;\n" +
                "ROCKS_PROPERTY() rocks::Optional<Leaf> leaf;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement Field(string type, string name) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement count = Field("probe::Value", "count");
            Verify.Equal("Optional", count.GetProperty("value").GetProperty("kind").GetString(), "optional field kind");
            Verify.That(!count.GetProperty("value").TryGetProperty("type", out _), "optional value has no target type");
            Verify.Equal("Int32", count.GetProperty("element").GetProperty("kind").GetString(), "optional inner kind");
            Verify.Equal(0, count.GetProperty("maxElements").GetInt32(), "optional maxElements");
            Verify.Equal("Value", count.GetProperty("fieldKind").GetString(), "an optional is a value field");
            Verify.Equal("Utf8String", Field("probe::Value", "text").GetProperty("element").GetProperty("kind").GetString(), "optional string");
            Verify.Equal("Name", Field("probe::Value", "name").GetProperty("element").GetProperty("kind").GetString(), "optional name");
            System.Text.Json.JsonElement mode = Field("probe::Value", "mode").GetProperty("element");
            Verify.Equal("Enum", mode.GetProperty("kind").GetString(), "optional enum");
            Verify.Equal(Type("probe::Mode").GetProperty("id").GetString(), mode.GetProperty("type").GetString(), "optional enum target");
            System.Text.Json.JsonElement leaf = Field("probe::Value", "leaf").GetProperty("element");
            Verify.Equal("Struct", leaf.GetProperty("kind").GetString(), "optional struct");
            Verify.Equal(Type("probe::Leaf").GetProperty("id").GetString(), leaf.GetProperty("type").GetString(), "optional struct target");
            Verify.Equal("UInt32", Field("probe::Leaf", "weight").GetProperty("element").GetProperty("kind").GetString(), "optional struct member");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("{ValueKind::Optional,Guid{}}", generated, "generated optional schema");
            Verify.Contains("PathStructMember<::probe::Leaf,&::probe::Leaf::weight,ValueKind::UInt32>", generated, "optional member path thunk targets the inner kind");
            Verify.Contains("PathClassField<::probe::Value,::rocks::ValueKind::Struct>(cursor,&::probe::Value::leaf)", generated,
                "an Optional<struct> field has a path thunk");
            Verify.Contains("ReadGeneratedField<::probe::Value>(registry,access,object,&::probe::Value::count,out)", generated,
                "an Optional<scalar> field is read through FieldValue");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated optional header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // 생성 static_assert가 Optional<S>의 S가 값 struct인지 생성 형질로 다시 확인한다: 생성 헤더의 Leaf 형질
        // ContainsStrongReference를 참으로 바꾸면(import descriptor flags를 믿지 않는 확인) 컴파일이 실패한다.
        suite.Add("Meta.OptionalStructTraitStaticAssertIsChecked", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(OptionalPreamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Optional<Leaf> leaf;\n};\n}\n");
            string original = File.ReadAllText(probe.Output);
            ProcessResult control = await probe.CompileGeneratedAsync();
            Verify.Equal(0, control.ExitCode, "generated header compiles: " + control.StdOut + control.StdErr);
            const string from = "static constexpr bool ContainsStrongReference = false;";
            int at = original.IndexOf(from, original.IndexOf("struct GeneratedType<::probe::Leaf>", StringComparison.Ordinal), StringComparison.Ordinal);
            Verify.That(at >= 0, "generated Leaf trait missing");
            File.WriteAllText(probe.Output, original[..at] + "static constexpr bool ContainsStrongReference = true;" + original[(at + from.Length)..]);
            ProcessResult rejected = await probe.CompileGeneratedAsync();
            Verify.That(rejected.ExitCode != 0, "tampered Optional inner trait compiled");
            Verify.Contains("holds reference struct", rejected.StdOut + rejected.StdErr, "Optional inner trait static_assert diagnostic");
        });
        // Optional 내부 형식 오류는 RMC2306이다: 중첩 Optional, 배열, 참조(weak·소프트·클래스), noexcept 이동을 직접 정의해 Core Optional의
        // static_assert를 지나는 참조 struct, cv 내부, 지원하지 않는 형식. 강한 참조·보통의 참조 struct 내부는 그 static_assert에서 Clang이
        // 먼저 실패하고 멤버 위치의 RMC2306으로 바뀐다(Meta.OptionalStaticAssertFailureIsReportedAtTheMember).
        AddRejectGroup(context, "Meta.OptionalInnerTypeRules", [
            Declaration("ROCKS_PROPERTY() rocks::Optional<rocks::Optional<rocks::int32>> nested;").Replace(Preamble, OptionalPreamble, StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::Optional<rocks::Array<rocks::int32>> list;").Replace(Preamble, OptionalPreamble, StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::Optional<rocks::WeakObjectRef<Value>> target;").Replace(Preamble, OptionalPreamble, StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::Optional<rocks::SoftObjectRef<>> target;")
                .Replace(Preamble, "#include <Object/SoftObjectRef.h>\n" + OptionalPreamble, StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::Optional<rocks::ClassRef<Value>> kind;")
                .Replace(Preamble, "#include <Object/ClassRef.h>\n" + OptionalPreamble, StringComparison.Ordinal),
            OptionalPreamble + "ROCKS_CLASS()\nstruct Other : public rocks::Object {};\n" +
                "ROCKS_STRUCT()\nstruct Holder {\nHolder() noexcept = default;\nHolder(Holder&&) noexcept {}\nROCKS_PROPERTY() rocks::ObjectRef<Other> target;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Optional<Holder> holder;\n};\n}\n",
            Declaration("ROCKS_PROPERTY() rocks::Optional<const rocks::int32> label;").Replace(Preamble, OptionalPreamble, StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::Optional<rocks::int32*> pointer;").Replace(Preamble, OptionalPreamble, StringComparison.Ordinal)],
            "RMC2306", "Optional");
        // Optional<ObjectRef<T>>·Optional<보통의 참조 struct>·Optional<Wrap<ObjectRef>>는 멤버 선언에서 Core Optional의 noexcept 이동
        // static_assert로 Clang이 먼저 실패한다. 그 진단을 멤버 선언 위치의 RMC2306(클래스 필드·struct 멤버), NATIVE 멤버는 RMC2205, 둘 다면
        // RMC2204로 바꾼다(Array의 Meta.ArrayStaticAssertFailureIsReportedAtTheMember와 같은 변환 — 위치 해석은
        // Meta.ArrayInstantiationSiteParsesClangAndClangClDiagnostics).
        suite.Add("Meta.OptionalStaticAssertFailureIsReportedAtTheMember", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            const string wrap = "template <class T>\nstruct Wrap {\nT value;\n};\n";
            const string holder = "ROCKS_STRUCT()\nstruct Holder {\nROCKS_PROPERTY() rocks::ObjectRef<Value> target;\n};\n";
            foreach ((string declaration, string name, string code, string reason) in new[]
            {
                ("ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Optional<rocks::ObjectRef<Value>> target;\n};\n", "target", "RMC2306",
                    "probe::Value::target: Optional inner type 'rocks::ObjectRef<probe::Value>' holds a strong object reference"),
                ("ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY()\nrocks::Optional<Holder>\n    holder;\n};\n", "holder", "RMC2306",
                    "probe::Value::holder: Optional inner type 'probe::Holder' holds a strong object reference"),
                ("ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Optional<Wrap<rocks::ObjectRef<Value>>> target;\n};\n", "target", "RMC2306",
                    "probe::Value::target: Optional inner type 'probe::Wrap<rocks::ObjectRef<probe::Value>>' holds a strong object reference (probe::Wrap<rocks::ObjectRef<probe::Value>> -> rocks::ObjectRef)"),
                ("ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::Optional<rocks::ObjectRef<Value>> target;\n};\nROCKS_CLASS()\nstruct Value : public rocks::Object {};\n", "target", "RMC2306",
                    "probe::Leaf::target: Optional inner type 'rocks::ObjectRef<probe::Value>' holds a strong object reference"),
                ("ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_NATIVE() rocks::Optional<rocks::ObjectRef<Value>> cache;\n};\n", "cache", "RMC2205",
                    "probe::Value::cache: ROCKS_NATIVE member"),
                ("ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() ROCKS_NATIVE() rocks::Optional<rocks::ObjectRef<Value>> both;\n};\n", "both", "RMC2204",
                    "probe::Value::both: a field is either ROCKS_PROPERTY or ROCKS_NATIVE")
            })
            {
                string head = OptionalPreamble + "struct Value;\n" + wrap + holder;
                string source = head + declaration + "}\n";
                int line = 1 + source[..(head.Length + declaration.LastIndexOf(name, StringComparison.Ordinal))].Count(c => c == '\n');
                await probe.RejectAsync(source, code, $"Probe.h:{line}: error {code}: {reason}");
            }
        });
        // struct 깊이(RMC2303)는 Optional<struct> 간선도 센다: 1 + 내부 struct 깊이다(16은 받고 17은 거부).
        suite.Add("Meta.OptionalNestingDepthIsLimited", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            static string OptionalChain(int depth) => OptionalPreamble + "ROCKS_STRUCT()\nstruct Chain1 {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
                string.Concat(Enumerable.Range(2, depth - 1).Select(level => $"ROCKS_STRUCT()\nstruct Chain{level} {{\nROCKS_PROPERTY() rocks::Optional<Chain{level - 1}> inner;\n}};\n")) + "}\n";
            await probe.GenerateAsync(OptionalChain(16));
            Verify.Contains("staticconstexpruint32Depth=16u;", Compact(File.ReadAllText(probe.Output)), "optional chain depth");
            await probe.RejectAsync(OptionalChain(17), "RMC2303", "nesting depth 17");
        });
        // 원소 위치의 Optional은 컨테이너 쪽 코드다(Array 원소 RMC2304 — Docs/Design/ReflectionExtensions.ko.md 15절).
        AddRejectGroup(context, "Meta.OptionalElementUsesContainerCode", [
            OptionalPreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : values(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::Array<rocks::Optional<rocks::int32>> values;\n};\n}\n"],
            "RMC2304", "is an Optional");
        // Required는 Optional 필드에 쓸 수 없다(unset이 정상 값이다 — RMC2202).
        AddRejectGroup(context, "Meta.OptionalRejectsRequired", [
            Declaration("ROCKS_PROPERTY(Required) rocks::Optional<rocks::int32> count;").Replace(Preamble, OptionalPreamble, StringComparison.Ordinal)],
            "RMC2202", "Required cannot apply to an Optional field");
        // struct 순환 DFS는 Optional<struct> 간선도 따라간다: 미러가 자기가 묶는 실제 타입을 Optional로 담으면 RMC2302다(값 멤버
        // 순환은 C++ 불완전 타입이 먼저 막는다). 미러 멤버 inner는 Real에 없는 멤버라 Binds 대조(생성 코드 검증 컴파일)라면 다른 오류가
        // 나야 한다 — 이 시험은 순환 DFS(RMC2302)가 Binds 대조보다 먼저 돈다는 순서에도 기댄다(DFS가 Optional 간선을 따르지 않으면
        // RMC2302 대신 Binds 대조 오류가 나므로 구별력은 있다).
        AddRejectGroup(context, "Meta.OptionalStructCycleIsRejected", [
            "#include <Core/Optional.h>\n" + BindsPreamble + "struct Real {\nrocks::int32 a = 0;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::Real\")\nstruct RealBinding {\nROCKS_PROPERTY() rocks::Optional<Real> inner;\n};\n}\n"],
            "RMC2302", "cycle", BindsModule);
    }
}
