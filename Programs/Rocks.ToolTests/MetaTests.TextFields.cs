using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9l Text PROPERTY 시험이다(Docs/Design/ReflectionExtensions.ko.md 13·15절).
internal static partial class MetaTests
{
    private const string TextFieldPreamble = "#include <Core/Containers/Array.h>\n#include <Core/Containers/OrderedMap.h>\n" +
        "#include <Core/Containers/OrderedSet.h>\n#include <Core/Optional.h>\n#include <Localization/Text.h>\n" + Preamble;

    // rocks::Text 클래스 필드·Array 원소·정적 배열 원소·Map 값·struct 멤버가 descriptor(value {Text})와 생성 코드(스키마
    // {Text, 0}, 통째 FieldValue 함수표와 경로 thunk)에 닿고 생성 헤더가 컴파일된다. Required·Optional 내부·Set 원소·Map 키는
    // 성질 표로 거부된다.
    private static void RegisterTextFields(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.TextFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = TextFieldPreamble +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::Text label;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : lines(allocator), byKey(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::Text title;\nROCKS_PROPERTY() rocks::Array<rocks::Text> lines;\n" +
                "ROCKS_PROPERTY() rocks::Text fixed[2];\nROCKS_PROPERTY() rocks::OrderedMap<rocks::uint32, rocks::Text> byKey;\n" +
                "ROCKS_PROPERTY() Leaf leaf;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Field(string type, string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            Verify.Equal("Text", Field("probe::Value", "title").GetProperty("value").GetProperty("kind").GetString(), "text field kind");
            Verify.Equal("Text", Field("probe::Value", "lines").GetProperty("element").GetProperty("kind").GetString(), "array element");
            Verify.Equal("Text", Field("probe::Value", "fixed").GetProperty("element").GetProperty("kind").GetString(), "static array element");
            Verify.Equal("Text", Field("probe::Value", "byKey").GetProperty("element").GetProperty("kind").GetString(), "map value");
            Verify.Equal("Text", Field("probe::Leaf", "label").GetProperty("value").GetProperty("kind").GetString(), "struct member");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("{ValueKind::Text,Guid{}}", generated, "generated text schema");
            Verify.Contains("PathStructMember<::probe::Leaf,&::probe::Leaf::label,ValueKind::Text>", generated, "struct member path thunk");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated text header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        AddRejectGroup(context, "Meta.TextRejectsRequired", [
            Declaration("ROCKS_PROPERTY(Required) rocks::Text title;").Replace(Preamble, TextFieldPreamble, StringComparison.Ordinal)],
            "RMC2202", "Required");
        AddRejectGroup(context, "Meta.TextOptionalInnerIsRejected", [
            Declaration("ROCKS_PROPERTY() rocks::Optional<rocks::Text> title;").Replace(Preamble, TextFieldPreamble, StringComparison.Ordinal)],
            "RMC2306", "Optional inner type");
        AddRejectGroup(context, "Meta.TextKeyPositionsAreRejected", [
            Declaration("ROCKS_PROPERTY() rocks::OrderedSet<rocks::Text> titles;",
                extra: "explicit Value(rocks::Allocator& allocator) noexcept : titles(allocator) {}").Replace(Preamble, TextFieldPreamble, StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::Text, rocks::int32> titles;",
                extra: "explicit Value(rocks::Allocator& allocator) noexcept : titles(allocator) {}").Replace(Preamble, TextFieldPreamble, StringComparison.Ordinal)],
            "RMC2305", "Text cannot be a");
    }
}
