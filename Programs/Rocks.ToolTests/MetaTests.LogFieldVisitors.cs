using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 로그 구조체 펼치기(LG16)용 필드 방문 함수 시험이다(Docs/Design/LoggingSystem.ko.md 14.1.6절).
internal static partial class MetaTests
{
    // struct·클래스마다 GeneratedType<T>::VisitFields를 낸다: 선언 순서(가장 먼 base 필드부터, 선언 타입 안에서는 소스 순서)대로 visitor.Field("이름", 값)이고, 값은
    // 멤버 포인터 직접 읽기(클래스는 선언 클래스의 멤버 포인터, private 포함), bitfield는 생성 접근자 Get_의 값이다. 필드가 없으면
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
                    "visitor.Field(\"value\", value.*(&::probe::Leaf::value)); visitor.Field(\"bit\", GeneratedType<::probe::Leaf>::Get_bit(value)); }",
                    "struct visitor with a bitfield accessor"),
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
    }
}
