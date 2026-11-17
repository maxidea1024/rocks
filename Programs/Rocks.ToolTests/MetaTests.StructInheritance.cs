using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9k struct 상속·InstancedStruct<Base> 시험이다(Docs/Design/ReflectionExtensions.ko.md 12·15절).
internal static partial class MetaTests
{
    private const string InheritancePreamble = "#include <Core/Containers/Array.h>\n#include <Object/InstancedStruct.h>\n" + Preamble;

    // 리플렉션 base struct 하나와 필드 없는 비리플렉션 base(CRTP 태그)를 상속한 struct가 descriptor(base 이름, 상속 필드는 base
    // 소유)와 생성 코드(스키마 baseType, base 관계·빈 base static_assert, 소유 struct를 명시한 멤버 thunk)에 닿고, Atomic이
    // 상속되며, InstancedStruct<Base>의 value.type이 Base ID이고 생성 헤더가 컴파일된다. Emplace<S>는 Base의 파생이 아닌 S를
    // static_assert로 거부한다.
    private static void RegisterStructInheritance(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.StructInheritanceFlattensBaseFields", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = InheritancePreamble +
                "template <class T> struct Tag {};\n" +
                "ROCKS_STRUCT()\nstruct Base {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Mid : Base {\nROCKS_PROPERTY() rocks::uint32 b = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Tagged : Tag<Tagged>, Mid {\nROCKS_PROPERTY() float c = 0.0f;\n};\n" +
                "ROCKS_STRUCT(Atomic)\nstruct AtomicBase {\nROCKS_PROPERTY() rocks::int32 x = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct AtomicLeaf : AtomicBase {\nROCKS_PROPERTY() rocks::int32 z = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Plain {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : shapes(allocator) {}\n" +
                "ROCKS_PROPERTY() Mid mid;\nROCKS_PROPERTY() Tagged tagged;\nROCKS_PROPERTY() AtomicLeaf atomic;\n" +
                "ROCKS_PROPERTY() rocks::InstancedStruct<Base> shape;\nROCKS_PROPERTY() rocks::Array<rocks::InstancedStruct<Base>> shapes;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement Field(string type, string name) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            Verify.Equal("", Type("probe::Base").GetProperty("base").GetString(), "root struct has no base");
            Verify.Equal("probe::Base", Type("probe::Mid").GetProperty("base").GetString(), "reflected base name");
            Verify.Equal("probe::Mid", Type("probe::Tagged").GetProperty("base").GetString(), "non-reflected empty base is not the base");
            Verify.Equal(3, Type("probe::Tagged").GetProperty("fields").GetArrayLength(), "inherited fields are flattened");
            Verify.Equal("probe::Base", Field("probe::Tagged", "a").GetProperty("owner").GetString(), "inherited field owner");
            Verify.Equal(Field("probe::Base", "a").GetProperty("id").GetString(), Field("probe::Tagged", "a").GetProperty("id").GetString(),
                "inherited field keeps the base field ID");
            Verify.That(Type("probe::AtomicLeaf").GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Atomic"),
                "Atomic is inherited");
            string baseId = Type("probe::Base").GetProperty("id").GetString()!;
            Verify.Equal(baseId, Field("probe::Value", "shape").GetProperty("value").GetProperty("type").GetString(), "InstancedStruct base type");
            Verify.Equal(baseId, Field("probe::Value", "shapes").GetProperty("element").GetProperty("type").GetString(),
                "InstancedStruct element base type");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("static_assert(::rocks::IsBaseOfV<::probe::Base,::probe::Mid>", generated, "reflected base static_assert");
            Verify.Contains("static_assert(::rocks::IsEmptyV<::probe::Tag<probe::Tagged>>&&!::rocks::IsPolymorphicV<::probe::Tag<probe::Tagged>>",
                generated, "empty base static_assert");
            Verify.Contains("detail::StructFieldOps<::probe::Tagged,&::probe::Tagged::a>(", generated, "inherited member thunk names the owner");
            Verify.That(!generated.Contains("{ValueKind::InstancedStruct,Guid{}}", StringComparison.Ordinal), "InstancedStruct<Base> carries the base ID");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated inheritance header compiles: " + compiled.StdOut + compiled.StdErr);

            string original = File.ReadAllText(probe.Output);
            const string accepted = "\nnamespace probe {\ninline void UseShape(rocks::TypeRegistry& types, rocks::Allocator& allocator) noexcept {\n" +
                "rocks::InstancedStruct<Base> value;\n(void)value.Emplace<Tagged>(types, allocator);\n}\n}\n";
            File.WriteAllText(probe.Output, original + accepted);
            ProcessResult derivedCompiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, derivedCompiled.ExitCode, "Emplace<Tagged> compiles: " + derivedCompiled.StdOut + derivedCompiled.StdErr);
            File.WriteAllText(probe.Output, original + accepted.Replace("Emplace<Tagged>", "Emplace<Plain>", StringComparison.Ordinal));
            ProcessResult rejected = await probe.CompileGeneratedAsync();
            Verify.That(rejected.ExitCode != 0, "Emplace<Plain> compiled");
            Verify.Contains("InstancedStruct<Base> holds only Base or a struct derived from Base", rejected.StdOut + rejected.StdErr,
                "Emplace base static_assert diagnostic");
        });
        string Leaf(string bases, string extra = "") => InheritancePreamble + extra +
            "ROCKS_STRUCT()\nstruct A {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n" +
            $"ROCKS_STRUCT()\nstruct Leaf : {bases} {{\nROCKS_PROPERTY() rocks::int32 c = 0;\n}};\n}}\n";
        AddRejectGroup(context, "Meta.StructBaseMustBePublicNonvirtual", [Leaf("virtual A"), Leaf("private A")], "RMC2301", "public and nonvirtual");
        AddRejectGroup(context, "Meta.StructHasOneReflectedBase",
            [Leaf("A, B", "ROCKS_STRUCT()\nstruct B {\nROCKS_PROPERTY() rocks::int32 b = 0;\n};\n")], "RMC2301", "only one reflected base");
        AddRejectGroup(context, "Meta.StructNonReflectedBaseMustBeEmpty",
            [Leaf("Shared", "struct Shared {\nrocks::int32 hidden = 0;\n};\n"), Leaf("Shared", "struct Empty {};\nstruct Shared : Empty {\nrocks::int32 hidden = 0;\n};\n")],
            "RMC2301", "must have no fields and no virtual functions");
        AddRejectGroup(context, "Meta.StructCannotDeriveFromClass", [Leaf("public rocks::Object")], "RMC2301", "reflected class");
        AddRejectGroup(context, "Meta.StructCannotHaveVirtualFunctions",
            [InheritancePreamble + "ROCKS_STRUCT()\nstruct Leaf {\nvirtual void Touch() {}\nROCKS_PROPERTY() rocks::int32 c = 0;\n};\n}\n",
                Leaf("Shared", "struct Shared {\nvirtual ~Shared() = default;\n};\n")],
            "RMC2301", "virtual functions");
        AddRejectGroup(context, "Meta.BindsMirrorAndInheritanceAreExclusive", [
            InheritancePreamble + "struct Real {\nrocks::int32 a = 0;\n};\nROCKS_STRUCT()\nstruct A {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::Real\")\nstruct RealBinding : A {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n}\n",
            InheritancePreamble + "struct Real {\nrocks::int32 a = 0;\n};\nROCKS_STRUCT(Binds = \"probe::Real\")\nstruct RealBinding {\n" +
                "ROCKS_PROPERTY() rocks::int32 a = 0;\n};\nROCKS_STRUCT()\nstruct Leaf : RealBinding {\nROCKS_PROPERTY() rocks::int32 c = 0;\n};\n}\n"],
            "RMC2301", "Binds mirror", BindsModule);
        // 상속 필드와 대소문자 무시로 같은 도구 이름은 RMC2203이다.
        AddRejectGroup(context, "Meta.StructInheritedToolNamesAreUnique", [
            InheritancePreamble + "ROCKS_STRUCT()\nstruct A {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Leaf : A {\nROCKS_PROPERTY() rocks::int32 A = 0;\n};\n}\n"],
            "RMC2203", "collides");
        // InstancedStruct<Base>의 Base는 담을 수 있는 리플렉션 struct여야 한다(RMC2308).
        AddRejectGroup(context, "Meta.InstancedStructBaseMustBeHoldableStruct", [
            Declaration("ROCKS_PROPERTY() rocks::InstancedStruct<rocks::int32> shape;").Replace(Preamble, InheritancePreamble, StringComparison.Ordinal),
            InheritancePreamble + "ROCKS_STRUCT()\nstruct RefBase {\nROCKS_PROPERTY() rocks::ObjectRef<rocks::Object> target;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::InstancedStruct<RefBase> shape;\n};\n}\n"],
            "RMC2308", "InstancedStruct base");
    }
}
