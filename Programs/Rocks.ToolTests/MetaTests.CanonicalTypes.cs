using System.Text.Json;

namespace Rocks.ToolTests;

// meta 시험 중 정규 형식 탐침 시험이다(Docs/Design/ClangVersionSupport.ko.md 4·8절). 형식 철자를 첫 실행 JSON의 desugar 철자가 아니라
// Clang 정규 철자로 읽으므로, Clang 22(한정 없이 쓴 비템플릿 레코드의 desugar 철자를 내지 않는다)와 그 전 버전에서 결과가 같아야 한다.
internal static partial class MetaTests
{
    private const string CanonicalPreamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n" +
        "#include <Core/Text/String.h>\n#include <Core/Tuple.h>\nnamespace probe {\nusing rocks::Object;\nusing rocks::String;\nusing rocks::int32;\nusing rocks::uint8;\n";

    // 레코드 닫힘 시험에 쓰는 타입들이다: Object 파생 클래스, 부분·명시 특수화, inline 이름공간 레코드, 재귀 base, 같은 식별자 base 둘.
    private const string ClosureTypes = "struct Node : rocks::Object {};\nstruct Holder { Node* node = nullptr; };\n" +
        "template <class T, class A = void> struct Box { T* data = nullptr; };\n" +
        "template <class T> struct Box<T, int> { rocks::ObjectRef<rocks::Object> strong; T value{}; };\n" +
        "template <> struct Box<char> { rocks::ObjectRef<rocks::Object> special; };\n" +
        "inline namespace v1 { struct Inl { rocks::ObjectRef<rocks::Object> ref; }; struct PlainInl { int x = 0; }; }\n" +
        "template <int N> struct Rec : Rec<N - 1> { int v = 0; };\ntemplate <> struct Rec<0> { rocks::ObjectRef<rocks::Object> ref; };\n" +
        "template <int I, class T> struct Slot { T value{}; };\nstruct Multi : Slot<1, int>, Slot<2, rocks::ObjectRef<rocks::Object>> {};\n" +
        "struct Later;\nstruct Back { Later* later = nullptr; int x = 0; };\n";

    private static string ClosureSource(string member) =>
        CanonicalPreamble + ClosureTypes + $"ROCKS_CLASS()\nstruct Value : public Object {{\nROCKS_PROPERTY() int32 count = 0;\nROCKS_NATIVE() {member}\n}};\n}}\n";

    private static void RegisterCanonicalTypes(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        // 한정 없이 쓴 레코드·enum 필드(using 선언), 별칭 base, 별칭 템플릿·멤버 typedef·decltype으로 쓴 struct base, 같은 식별자의 비리플렉션
        // 빈 base 둘, 이름공간 안의 비리플렉션 빈 base, 함수 포인터·배열 포인터를 원소로 둔 C 배열 NATIVE가 모두 정규 철자로 풀린다(4.3·4.7절).
        suite.Add("Meta.CanonicalSpellingsNameUnqualifiedTypesAndBases", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = CanonicalPreamble +
                "ROCKS_ENUM()\nenum class Mode : uint8 { Off = 0, On = 1 };\n" +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() int32 count = 0;\n};\n" +
                "using LeafAlias = Leaf;\ntemplate <class T> using Same = T;\nstruct Traits { using BaseType = Leaf; };\n" +
                "namespace a { struct Tag {}; }\nnamespace b { struct Tag {}; }\nnamespace detail { struct Marker {}; }\n" +
                "ROCKS_STRUCT()\nstruct ViaAliasTemplate : Same<Leaf> {\nROCKS_PROPERTY() int32 extra = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct ViaMemberTypedef : Traits::BaseType {\nROCKS_PROPERTY() int32 extra = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct ViaDecltype : decltype(Leaf{}) {\nROCKS_PROPERTY() int32 extra = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct TwoTags : a::Tag, b::Tag, detail::Marker {\nROCKS_PROPERTY() int32 extra = 0;\n};\n" +
                "using ObjectAlias = Object;\n" +
                "ROCKS_CLASS()\nstruct Value : public ObjectAlias {\nROCKS_PROPERTY() Mode mode = Mode::Off;\n" +
                "ROCKS_PROPERTY() LeafAlias leaf;\nROCKS_NATIVE() void (*callbacks[3])(int) = {};\nROCKS_NATIVE() int (*rows[2])[5] = {};\n};\n}\n";
            await probe.GenerateAsync(source);
            using JsonDocument descriptor = JsonDocument.Parse(File.ReadAllText(probe.Descriptor));
            foreach (string derived in new[] { "probe::ViaAliasTemplate", "probe::ViaMemberTypedef", "probe::ViaDecltype" })
            {
                Verify.Equal("probe::Leaf", DescriptorBase(descriptor.RootElement, derived), derived + " reflected base");
            }

            string generated = Compact(probe.GeneratedText());
            foreach (string empty in new[] { "::probe::a::Tag", "::probe::b::Tag", "::probe::detail::Marker" })
            {
                Verify.That(generated.Contains($"IsEmptyV<{empty}>", StringComparison.Ordinal), "non-reflected empty base checked by its canonical name: " + empty);
            }
        });
        // 레코드 닫힘(4.4절): 값으로 담은 레코드의 완전한 Object 파생 포인터, 부분·명시 특수화의 필드, inline 이름공간 레코드, 재귀 base·같은
        // 식별자 base, 템플릿 인자로만 보이는 강한 참조(Tuple)가 모두 강한 참조로 거부된다.
        AddRejectGroup(context, "Meta.CanonicalClosureFindsNestedStrongReferences",
            new[] { "Box<int, int> box;", "Box<char> box;", "Inl inl;", "Rec<2> rec;", "Multi multi;", "rocks::Tuple<rocks::ObjectRef<rocks::Object>, int> tuple;" }
                .Select(ClosureSource), "RMC2205", "strong object reference");
        AddRejectGroup(context, "Meta.CanonicalClosureJudgesNestedObjectPointers", [ClosureSource("Holder holder;")], "RMC2205",
            "raw pointer to Object-derived probe::Node");
        // 불완전한 대상을 가리키는 중첩 포인터는 직접 멤버처럼 허용되고(빌린 back-pointer), 강한 참조가 없는 특수화·inline 이름공간 레코드·Tuple도 받는다.
        suite.Add("Meta.CanonicalClosureAllowsIncompleteTargetsAndPlainValues", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(CanonicalPreamble + ClosureTypes + "ROCKS_CLASS()\nstruct Value : public Object {\nROCKS_PROPERTY() int32 count = 0;\n" +
                "ROCKS_NATIVE() Back back;\nROCKS_NATIVE() Box<int> box;\nROCKS_NATIVE() PlainInl plain;\nROCKS_NATIVE() rocks::Tuple<int, float> tuple;\n};\n}\n");
        });
        // 전역 한정 철자로 부를 수 없는 class(함수 안 지역 클래스)를 값으로 담으면 따라갈 수 없으므로 거부한다(조용히 건너뛰지 않는다).
        AddRejectGroup(context, "Meta.CanonicalClosureRejectsUnfollowableClasses",
            [CanonicalPreamble + "inline auto MakeLocal() {\nstruct Local { int x = 0; };\nreturn Local{};\n}\n" +
                "ROCKS_CLASS()\nstruct Value : public Object {\nROCKS_PROPERTY() int32 count = 0;\nROCKS_NATIVE() decltype(MakeLocal()) local{};\n};\n}\n"],
            "RMC2205", "cannot verify");
    }

    // descriptor에서 이름이 name인 타입의 base다(없으면 "").
    private static string DescriptorBase(JsonElement node, string name)
    {
        if (node.ValueKind == JsonValueKind.Object)
        {
            if (node.TryGetProperty("name", out JsonElement value) && value.ValueKind == JsonValueKind.String && value.GetString() == name &&
                node.TryGetProperty("base", out JsonElement baseValue) && baseValue.ValueKind == JsonValueKind.String)
            {
                return baseValue.GetString() ?? "";
            }

            foreach (JsonProperty property in node.EnumerateObject())
            {
                if (DescriptorBase(property.Value, name) is { Length: > 0 } found)
                {
                    return found;
                }
            }
        }
        else if (node.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement element in node.EnumerateArray())
            {
                if (DescriptorBase(element, name) is { Length: > 0 } found)
                {
                    return found;
                }
            }
        }

        return "";
    }
}
