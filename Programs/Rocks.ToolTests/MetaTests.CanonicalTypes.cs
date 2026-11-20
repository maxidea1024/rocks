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

    // 레코드 닫힘 리뷰 사례(개정 4)에 쓰는 타입들이다: 바이트 저장소 템플릿(Hide — Optional처럼 T를 바이트에 숨긴다), enum·bool pack·
    // unsigned long long 최댓값·형식 매개변수에 기대는 NTTP 특수화, 클래스 안 템플릿의 enum NTTP, 인자 자리로만 쓰는 불완전한 특수화, typedef
    // 이름 struct, 부분·명시 특수화의 WorldOnly 표지, 템플릿 안 중첩 레코드, 간접 base 이름을 가진 중첩 base, pack 확장 base, base를 가리는
    // 중첩 타입, private base, 선언자가 있는 이름 없는 struct, 문자 리터럴 인자, 템플릿 템플릿 인자, 부분 특수화만 정의한 primary.
    private const string ReviewTypes = "template <class T> struct Hide { alignas(T) unsigned char storage[sizeof(T)]; };\n" +
        "struct Node : rocks::Object {};\nstruct Plain { int x = 0; };\nstruct PtrHolder { Plain* plain = nullptr; };\n" +
        "enum class Kind { A, B };\ntemplate <Kind K> struct ByEnum { int x = 0; };\ntemplate <Kind K> struct ByEnumRef { rocks::ObjectRef<rocks::Object> ref; };\n" +
        "template <bool... Bs> struct Bools { int x = 0; };\ntemplate <class T, T... Values> struct Seq { int x = 0; };\n" +
        "template <unsigned long long N> struct Big { int x = 0; };\n" +
        "struct Nested { enum class E { P, Q }; template <E V> struct In { int x = 0; }; In<E::Q> in; };\n" +
        "enum class Hnd : int {};\nstruct Both { Hide<Hnd> hidden; Hnd value; };\n" +
        "template <class T> struct Tag { int x = 0; };\ntemplate <class T> struct Fwd;\nstruct UsesFwd { Tag<Fwd<int>> tag; };\n" +
        "typedef struct { int y; } TypedefPlain;\ntypedef struct { rocks::ObjectRef<rocks::Object> ref; } TypedefRef;\n" +
        "template <class T> struct Mk { static constexpr bool RocksRequiresWorldOnlyOwner = true; T v{}; };\ntemplate <> struct Mk<char> { char v = 0; };\n" +
        "struct WrapMk { Mk<char> m; };\ntemplate <class T> struct WOuter { Mk<T> m; };\n" +
        "template <class T, class U> struct PM { int x = 0; };\n" +
        "template <class T> struct PM<T, int> { static constexpr bool RocksRequiresWorldOnlyOwner = true; T v{}; };\n" +
        "template <class T> struct OuterP { struct Plain { int x = 0; }; };\n" +
        "template <class T> struct OuterS { struct Inner { rocks::ObjectRef<rocks::Object> ref; }; };\n" +
        "template <class T> struct OuterW { struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; int x = 0; }; };\n" +
        "template <class T> struct Outer9 { struct Inner { rocks::ObjectRef<rocks::Object> ref; }; int plain = 0; };\n" +
        "struct Mid9 : Outer9<int> {};\nstruct D9 : Outer9<int>::Inner, Mid9 {};\n" +
        "template <int I, class T> struct Leaf10 { T value{}; };\ntemplate <> struct Leaf10<1, int> { rocks::ObjectRef<rocks::Object> ref; };\n" +
        "template <class S, class... T> struct Storage10;\ntemplate <int... I, class... T> struct Storage10<Seq<int, I...>, T...> : Leaf10<I, T>... {};\n" +
        "struct Base11 { rocks::ObjectRef<rocks::Object> ref; };\nstruct D11 : Base11 { struct Base11 { int x = 0; }; };\nstruct PrivBase : private Base11 {};\n" +
        "struct WithUnnamed { struct { int x; struct { int deep; } in2; } inner, second; struct { int z; } arr[2]; union { int a; float f; }; };\n" +
        "struct WithUnnamedRef { struct { rocks::ObjectRef<rocks::Object> ref; } inner; };\n" +
        "struct NestedHide { Hide<rocks::ObjectRef<rocks::Object>> hidden; };\n" +
        "template <char C> struct ByChar : rocks::Object {};\nstruct CharPtr { ByChar<'>'>* target = nullptr; };\n" +
        "template <class T> struct Wrap { T v{}; };\ntemplate <template <class> class TT> struct TempT { TT<int> t; };\n" +
        "template <class S> struct DOnly;\ntemplate <class T> struct DOnly<Wrap<T>> { rocks::ObjectRef<rocks::Object> ref; };\n";

    private static string ReviewSource(string member) =>
        CanonicalPreamble + ReviewTypes + $"ROCKS_CLASS()\nstruct Value : public Object {{\nROCKS_PROPERTY() int32 count = 0;\nROCKS_NATIVE() {member}\n}};\n}}\n";

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
                "ROCKS_PROPERTY() LeafAlias leaf;\nROCKS_NATIVE() void (*callbacks[3])(Leaf) = {};\nROCKS_NATIVE() Leaf (*rows[2])[5] = {};\n" +
                "ROCKS_NATIVE() Leaf grid[2][3] = {};\n};\n}\n";
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

            // C 배열 NATIVE의 원소(ElementSpelling)는 모든 차원을 벗긴 형식이다: 원소가 함수 포인터·배열 포인터면 struct가 아니고(Leaf를
            // 인자·대상으로 담아도), 2차원 Leaf 배열의 원소는 Leaf다.
            string text = probe.GeneratedText();
            Verify.That(text.Contains("::grid holds reference struct probe::Leaf", StringComparison.Ordinal), "2D struct array element is the struct");
            foreach (string member in new[] { "callbacks", "rows" })
            {
                Verify.That(!text.Contains($"::{member} holds reference struct", StringComparison.Ordinal), "pointer element is not a struct: " + member);
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
        // 템플릿 템플릿 인자를 가진 특수화(열쇠를 만들 수 없다)도 같다.
        AddRejectGroup(context, "Meta.CanonicalClosureRejectsUnfollowableClasses",
            [CanonicalPreamble + "inline auto MakeLocal() {\nstruct Local { int x = 0; };\nreturn Local{};\n}\n" +
                "ROCKS_CLASS()\nstruct Value : public Object {\nROCKS_PROPERTY() int32 count = 0;\nROCKS_NATIVE() decltype(MakeLocal()) local{};\n};\n}\n",
                ReviewSource("TempT<Wrap> nested;")],
            "RMC2205", "cannot verify");
        // 리뷰 회귀(개정 4): 직접 멤버 포인터와 같은 중첩 포인터(R03), enum·bool pack·최댓값·형식 매개변수 NTTP 특수화(R07·R13), 클래스 안
        // 템플릿의 enum NTTP, 인자 자리 뒤 값 자리(enum), 인자 자리의 불완전한 특수화, typedef 이름 struct, 표지를 없앤 명시 특수화(R06),
        // 템플릿 안 중첩 레코드(R16), 선언자가 있는 이름 없는 struct·익명 union(Y19), 강한 참조가 없는 pack 확장 base를 받는다.
        suite.Add("Meta.CanonicalClosureAcceptsKeyedSpecializationsAndMembers", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ReviewSource("Plain* top = nullptr;\nROCKS_NATIVE() PtrHolder holder;\nROCKS_NATIVE() ByEnum<Kind::B> byEnum;\n" +
                "ROCKS_NATIVE() Bools<true, false> bools;\nROCKS_NATIVE() Big<18446744073709551615ULL> big;\nROCKS_NATIVE() Seq<bool, true, false> seq;\n" +
                "ROCKS_NATIVE() Nested nested;\nROCKS_NATIVE() Both both;\nROCKS_NATIVE() UsesFwd usesFwd;\nROCKS_NATIVE() TypedefPlain typedefPlain;\n" +
                "ROCKS_NATIVE() WrapMk wrapMk;\nROCKS_NATIVE() OuterP<int>::Plain outerPlain;\nROCKS_NATIVE() WithUnnamed unnamed;\n" +
                "ROCKS_NATIVE() Storage10<Seq<int, 0, 1>, int, float> storage;"));
        });
        // 리뷰 회귀(개정 4): 바이트 저장소에 숨긴 enum NTTP 특수화(R08)·특수화 안 중첩 레코드(R01)·typedef 이름 struct(R12), 간접 base 이름을
        // 가진 중첩 base(R09), pack 확장 base(R10), base를 가리는 중첩 타입(R11), private base, 선언자가 있는 이름 없는 struct(Y19),
        // 중첩 레코드의 바이트 저장소 인자(R17), 부분 특수화만 정의한 primary가 강한 참조로 거부된다.
        AddRejectGroup(context, "Meta.CanonicalClosureFindsReviewedStrongReferences",
            new[]
            {
                "Hide<ByEnumRef<Kind::B>> hidden;", "Hide<OuterS<int>::Inner> hidden;", "Hide<TypedefRef> hidden;", "D9 derived;",
                "Storage10<Seq<int, 0, 1>, int, int> storage;", "D11 derived;", "PrivBase derived;", "WithUnnamedRef unnamed;", "NestedHide nested;",
                "DOnly<Wrap<int>> partial;"
            }.Select(ReviewSource), "RMC2205", "ROCKS_NATIVE member holds a strong object reference");
        // 문자 리터럴 인자(`'>'`)를 가진 특수화의 포인터도 포인터로 판정한다(W22).
        AddRejectGroup(context, "Meta.CanonicalClosureJudgesCharLiteralPointer", [ReviewSource("CharPtr pointer;")], "RMC2205",
            "raw pointer to Object-derived probe::ByChar<'>'>");
        // 리뷰 회귀(개정 4): 부분 특수화에만 있는 표지(R05), 템플릿 안 중첩 레코드의 표지(R14), 특수화를 거친 중첩 표지는 WorldOnly 소유자를
        // 요구한다(RMC2206 — 형질 사실로 묻는다).
        AddRejectGroup(context, "Meta.CanonicalClosureFindsWorldOnlyThroughSpecializations",
            new[] { "PM<char, int> partial;", "OuterW<int>::Lease lease;", "WOuter<int> outer;" }.Select(ReviewSource), "RMC2206", "WorldOnly owner");
        // 중첩 레코드의 포인터 대상이 리플렉션 헤더에서는 불완전하고 모듈 public 헤더에서 Object 파생으로 정의되면 직접 멤버처럼 거부한다.
        suite.Add("Meta.CanonicalClosureJudgesNestedPointerWithPublicHeaders", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            probe.WriteHeader("LateDefinition.h", "#pragma once\n#include <Object/Object.h>\nnamespace probe {\nstruct Late : rocks::Object {};\n}\n");
            await probe.RejectAsync(CanonicalPreamble + "struct Late;\nstruct LateHolder { Late* late = nullptr; };\n" +
                "ROCKS_CLASS()\nstruct Value : public Object {\nROCKS_PROPERTY() int32 count = 0;\nROCKS_NATIVE() LateHolder holder;\n};\n}\n", "RMC2205",
                "raw pointer to Object-derived probe::Late");
        });
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
