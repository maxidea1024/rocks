using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9d2 ClassRef PROPERTY와 타입 도구 이름 시험이다(Docs/Design/ReflectionExtensions.ko.md 5.2절).
internal static partial class MetaTests
{
    private const string ClassRefPreamble = "#include <Core/Containers/Array.h>\n#include <Object/ClassRef.h>\n" + Preamble;

    // ClassRef 클래스 필드·struct 멤버·Array<ClassRef> 원소(Required 포함)가 descriptor와 생성 코드에 닿고, 클래스·struct의 타입
    // 도구 이름(기본은 한정하지 않은 C++ 이름, Name 지정자)이 descriptor toolName과 생성 스키마에 나간다. 자기 클래스를 Base로 둔
    // ClassRef<Value>도 생성 헤더가 컴파일된다(Base의 생성 헤더를 쓰지 않는다).
    private static void RegisterClassRefs(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.ClassReferenceFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = ClassRefPreamble +
                "ROCKS_CLASS()\nstruct Derived : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 extra = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Leaf {\nexplicit Leaf(rocks::Allocator& allocator) noexcept : kinds(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::ClassRef<Derived> kind;\nROCKS_PROPERTY() rocks::Array<rocks::ClassRef<Derived>> kinds;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public Derived {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : leaf(allocator), kinds(allocator) {}\n" +
                "ROCKS_PROPERTY(Required) rocks::ClassRef<Derived> kind;\nROCKS_PROPERTY() rocks::ClassRef<Value> self;\n" +
                "ROCKS_PROPERTY() Leaf leaf;\nROCKS_PROPERTY(Required) rocks::Array<rocks::ClassRef<Derived>> kinds;\n};\n" +
                "ROCKS_CLASS(Name = \"Weapons.Other\")\nstruct Other : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement Field(string type, string name) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string derivedId = Type("probe::Derived").GetProperty("id").GetString()!;
            string valueId = Type("probe::Value").GetProperty("id").GetString()!;
            System.Text.Json.JsonElement kind = Field("probe::Value", "kind");
            Verify.Equal("ClassReference", kind.GetProperty("value").GetProperty("kind").GetString(), "class reference field");
            Verify.Equal(derivedId, kind.GetProperty("value").GetProperty("type").GetString(), "class reference base");
            Verify.Equal("Value", kind.GetProperty("fieldKind").GetString(), "a class reference is a value field");
            Verify.Equal("Required", kind.GetProperty("flags")[0].GetString(), "Required class reference");
            Verify.Equal(valueId, Field("probe::Value", "self").GetProperty("value").GetProperty("type").GetString(), "self base");
            System.Text.Json.JsonElement kinds = Field("probe::Value", "kinds");
            Verify.Equal("ClassReference", kinds.GetProperty("element").GetProperty("kind").GetString(), "class reference array element");
            Verify.Equal(derivedId, kinds.GetProperty("element").GetProperty("type").GetString(), "class reference element base");
            Verify.Equal("Required", kinds.GetProperty("flags")[0].GetString(), "Required class reference array");
            Verify.Equal("ClassReference", Field("probe::Leaf", "kind").GetProperty("value").GetProperty("kind").GetString(), "struct member");
            Verify.Equal("ClassReference", Field("probe::Leaf", "kinds").GetProperty("element").GetProperty("kind").GetString(),
                "struct member array element");
            Verify.Equal("Value", Type("probe::Value").GetProperty("toolName").GetString(), "default class tool name");
            Verify.Equal("Leaf", Type("probe::Leaf").GetProperty("toolName").GetString(), "default struct tool name");
            Verify.Equal("Weapons.Other", Type("probe::Other").GetProperty("toolName").GetString(), "Name specifier tool name");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("PathStructMember<&::probe::Leaf::kind,ValueKind::ClassReference>", generated, "class reference member path thunk");
            Verify.Contains("\"Weapons.Other\",13)", generated, "generated schema tool name");
            Verify.Contains("0u,Span<constchar>(\"Leaf\",4)", generated, "struct schema tool name after flags");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated class reference header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // ClassRef의 Base는 리플렉션 클래스다: 리플렉션 struct·리플렉션되지 않은 타입·rocks::Object는 RMC2003.
        AddRejectGroup(context, "Meta.ClassReferenceBaseMustBeReflectedClass", [
            ClassRefPreamble + "ROCKS_STRUCT()\nstruct Leaf { ROCKS_PROPERTY() rocks::int32 count = 0; };\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::ClassRef<Leaf> kind;\n};\n}\n",
            ClassRefPreamble + "struct Plain : public rocks::Object {};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::ClassRef<Plain> kind;\n};\n}\n",
            ClassRefPreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::ClassRef<rocks::Object> kind;\n};\n}\n"],
            "RMC2003", "reflected");
        // 타입 도구 이름의 형식(빈 이름·none 예약 — Name 지정자와 기본 이름인 C++ 이름 자체·빈 마디·':')과 모듈 안 대소문자 무시 유일성(클래스끼리·클래스와 struct, 기본
        // 이름과 Name 지정자 사이)은 RMC2203이다.
        AddRejectGroup(context, "Meta.TypeToolNameRules", [
            Declaration().Replace("ROCKS_CLASS()", "ROCKS_CLASS(Name = \"\")", StringComparison.Ordinal),
            Declaration().Replace("ROCKS_CLASS()", "ROCKS_CLASS(Name = \"NONE\")", StringComparison.Ordinal),
            Declaration().Replace("ROCKS_CLASS()", "ROCKS_CLASS(Name = \"Weapons..Sword\")", StringComparison.Ordinal),
            Declaration().Replace("ROCKS_CLASS()", "ROCKS_CLASS(Name = \"Weapons:Sword\")", StringComparison.Ordinal),
            Preamble + "ROCKS_STRUCT(Name = \"bad name\")\nstruct Leaf { ROCKS_PROPERTY() rocks::int32 count = 0; };\n}\n",
            Preamble + "ROCKS_CLASS()\nstruct none : public rocks::Object {};\n}\n",
            "#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace a {\nROCKS_CLASS()\nstruct Thing : public rocks::Object {};\n}\n" +
                "namespace b {\nROCKS_CLASS()\nstruct Thing : public rocks::Object {};\n}\n",
            Preamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n" +
                "ROCKS_CLASS(Name = \"value\")\nstruct Other : public rocks::Object {};\n}\n",
            Preamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n" +
                "ROCKS_STRUCT(Name = \"VALUE\")\nstruct Leaf { ROCKS_PROPERTY() rocks::int32 count = 0; };\n}\n"],
            "RMC2203", "type tool name");
    }
}
