using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 로그 구조체 펼치기(LG16)용 필드 방문 함수 시험이다(Docs/Design/LoggingSystem.ko.md 14.1.6절).
internal static partial class MetaTests
{
    // struct·클래스마다 GeneratedType<T>::VisitFields를 낸다: 선언 순서(가장 먼 base 필드부터, 선언 타입 안에서는 소스 순서)대로 visitor.Field("이름", 값)이고, 값은
    // 멤버 포인터 직접 읽기(클래스는 선언 클래스의 멤버 포인터, private 포함), 자기 bitfield는 헤더 안의 직접 읽기(Get_ 정의는 생성 소스에 있다), 상속한 bitfield는 선언 타입으로 바꾼 직접 읽기다. 필드가 없으면
    // 매개변수 이름 없는 빈 본문이다. 생성 헤더를 ObjectRuntime 일반 특수화(Object/LogStructure.h)와 함께 컴파일해 템플릿을
    // 인스턴스화한다.
    private static void RegisterLogFieldVisitors(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.VisitFieldsReadsMembersDirectly", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() rocks::uint8 bit : 1 = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Hollow {\n};\n" +
                "ROCKS_CLASS()\nclass Base : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY() rocks::int32 shared = 0;\n" +
                "private:\nROCKS_PROPERTY() rocks::int32 secret = 0;\n};\n" +
                "ROCKS_CLASS()\nclass Derived final : public Base {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY() Leaf leaf;\n};\n}\n";
            await probe.GenerateAsync(source);
            string generated = Compact(File.ReadAllText(probe.Output));
            foreach ((string expected, string reason) in new[]
            {
                ("template <class Visitor> static constexpr void VisitFields(const ::probe::Leaf& value, Visitor& visitor) noexcept { " +
                    "visitor.Field(\"value\", value.*(&::probe::Leaf::value)); visitor.Field(\"bit\", static_cast<bool>(value.bit != 0)); }",
                    "struct visitor reads its own bitfield inline"),
                ("template <class Visitor> static constexpr void VisitFields(const ::probe::Hollow&, Visitor&) noexcept {}",
                    "struct without fields has an unnamed empty visitor"),
                ("template <class Visitor> static constexpr void VisitFields(const ::probe::Base& value, Visitor& visitor) noexcept { " +
                    "visitor.Field(\"shared\", value.*(&::probe::Base::shared)); visitor.Field(\"secret\", value.*(&::probe::Base::secret)); }",
                    "class visitor reads private fields"),
                ("template <class Visitor> static constexpr void VisitFields(const ::probe::Derived& value, Visitor& visitor) noexcept { " +
                    "visitor.Field(\"shared\", value.*(&::probe::Base::shared)); visitor.Field(\"secret\", value.*(&::probe::Base::secret)); " +
                    "visitor.Field(\"leaf\", value.*(&::probe::Derived::leaf)); }",
                    "derived class visitor reads inherited fields through the declaring class")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            // 일반 특수화로 방문 함수를 인스턴스화한다(멤버 접근·bitfield 접근자·중첩 struct가 컴파일되어야 한다). 생성 헤더는
            // Object/LogStructure.h를 include하지 않으므로 소비 코드가 include한다(Docs/Design/LoggingSystem.ko.md 14.2절 LG16).
            const string consumer = "#include <Object/LogStructure.h>\n" +
                "static_assert(rocks::HasLogStructure<probe::Leaf> && rocks::HasLogStructure<probe::Hollow>);\n" +
                "static_assert(rocks::HasLogStructure<probe::Base> && rocks::HasLogStructure<probe::Derived>);\n" +
                "inline void WriteDerived(rocks::LogStructureWriter& out, const probe::Derived& value) noexcept {\n" +
                "    rocks::LogStructure<probe::Derived>::Write(out, value);\n" +
                "    rocks::LogStructure<probe::Hollow>::Write(out, probe::Hollow{});\n" +
                "}\n";
            ProcessResult compiled = await probe.CompileGeneratedAsync(consumer);
            Verify.Equal(0, compiled.ExitCode, "generated field visitors compile with the log structure specialization: " + compiled.StdOut + compiled.StdErr);
        });

        // 특수화 가시성(Docs/Design/LoggingSystem.ko.md 14.1.6절 L30): 일반 특수화의 선언은 생성 헤더가 include하는
        // Object/GeneratedFieldAccess.h에 있으므로 Object/LogStructure.h 없이도 HasLogStructure 판정이 같고(정의만 안 보인다),
        // 그 상태로 로그 인자 경로(캡처 쓰기 함수)를 인스턴스화하면 static_assert로 거부한다.
        suite.Add("Meta.LogStructureDeclarationWithoutDefinitionHeader", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
                "struct Plain {\nint value = 0;\n};\n}\n";
            await probe.GenerateAsync(source);

            const string declared = "#include <Core/Diagnostics/LogStructure.h>\n" +
                "static_assert(rocks::HasLogStructure<probe::Leaf> && !rocks::detail::LogStructureDefined<probe::Leaf>);\n" +
                "static_assert(!rocks::HasLogStructure<probe::Plain>);\n";
            ProcessResult accepted = await probe.CompileGeneratedAsync(declared);
            Verify.Equal(0, accepted.ExitCode, "reflected type has a declared log structure without the definition header: " + accepted.StdOut + accepted.StdErr);

            const string used = "#include <Core/Diagnostics/LogStructure.h>\n" +
                "constexpr rocks::detail::LogStructureWrite write = rocks::detail::LogStructureWriteOf<probe::Leaf>();\n";
            ProcessResult rejected = await probe.CompileGeneratedAsync(used);
            string output = rejected.StdOut + rejected.StdErr;
            Verify.That(rejected.ExitCode != 0 && output.Contains("is declared but its", StringComparison.Ordinal) &&
                output.Contains("<Object/LogStructure.h>", StringComparison.Ordinal),
                "logging a reflected type without <Object/LogStructure.h> must fail with the definition static_assert: " + output);
        });
    }
}
