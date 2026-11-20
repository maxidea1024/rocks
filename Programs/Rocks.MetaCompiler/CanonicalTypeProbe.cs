using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Rocks.MetaCompiler;

// 정규 형식 탐침의 형판과 질문 문법이다(Docs/Design/ClangVersionSupport.ko.md 4.2절). 첫 실행과 같은 번역 단위 끝에 질문마다
// 명시적 인스턴스화 `template struct ::rocks_meta_canon::RocksCanonType<N, 형식>;`을 붙여 Clang을 다시 실행하고, 특수화의 템플릿 인자로
// 저장된 정규 형식(완전 한정 철자 — Clang 21과 22의 출력이 같다)을 읽는다. 명시적 인스턴스화 선언의 이름은 접근 검사를 받지 않으므로
// private 멤버·비리플렉션 레코드의 private 필드·private base에도 닿는다. 사실(0·1·정수)은 `int` 대신 `long long` 인자로 넘긴다(JSON
// 덤퍼는 `bool` 인자 true를 -1로 낸다). 질문 줄은 모두 전역 범위의 한 줄이다(ClangProbe가 `<stdin>` 줄로 실패를 질문에 대응시킨다).
internal static class CanonicalTypeProbe
{
    // 탐침 형판이다. RocksStaticShape·RocksStaticBase는 C 배열 원소·크기와 모든 차원을 벗긴 원소를 Clang이 나누게 한다(문자열로
    // 나누면 `int (*[2])[5]` 같은 철자에서 틀린다 — 4.7절).
    public const string Templates = """
        #include <Base/TypeTraits.h>
        namespace rocks_meta_canon {
        template <int RocksCanonN, class RocksCanonT>
        struct RocksCanonType {};
        template <int RocksCanonN, class RocksCanonD, class RocksCanonB>
        struct RocksCanonBase {
            static_assert(::rocks::IsBaseOfV<RocksCanonB, RocksCanonD> && !::rocks::IsSameV<RocksCanonB, RocksCanonD>, "rocks canonical base probe");
        };
        template <int RocksCanonN, long long RocksCanonV>
        struct RocksCanonFact {};
        template <class RocksCanonT>
        struct RocksStaticShape {
            using rocksStaticElement = void;
            static constexpr long long rocksStaticExtent = 0;
        };
        template <class RocksCanonE, decltype(sizeof(0)) RocksCanonN>
        struct RocksStaticShape<RocksCanonE[RocksCanonN]> {
            using rocksStaticElement = RocksCanonE;
            static constexpr long long rocksStaticExtent = static_cast<long long>(RocksCanonN);
        };
        template <class RocksCanonT>
        struct RocksStaticBase {
            using rocksStaticType = RocksCanonT;
        };
        template <class RocksCanonE, decltype(sizeof(0)) RocksCanonN>
        struct RocksStaticBase<RocksCanonE[RocksCanonN]> : RocksStaticBase<RocksCanonE> {};
        template <class RocksCanonE>
        struct RocksStaticBase<RocksCanonE[]> : RocksStaticBase<RocksCanonE> {};
        }

        """;

    // 탐침의 -ast-dump-filter다(형판과 특수화가 모두 이 이름공간 안에 있다).
    public const string Filter = "rocks_meta_canon";

    // 형식 질문: 형식 식(`decltype(::Owner::member)`, `::ns::Name<…>` 등)의 정규 철자.
    public static string TypeLine(int index, string type) =>
        string.Create(CultureInfo.InvariantCulture, $"template struct ::rocks_meta_canon::RocksCanonType<{index}, {type}>;");

    // base 질문: derived의 base로 확인한 base 형식의 정규 철자. prelude는 같은 줄 앞에 둘 이름공간 별칭 선언이다(4.3절의 2).
    public static string BaseLine(int index, string derived, string baseType, string prelude = "") =>
        string.Create(CultureInfo.InvariantCulture, $"{prelude}template struct ::rocks_meta_canon::RocksCanonBase<{index}, {derived}, {baseType}>;");

    // 사실 질문: 정수 상수식의 값(참·거짓은 `(식 ? 1 : 0)`으로 넘긴다).
    public static string FactLine(int index, string constant) =>
        string.Create(CultureInfo.InvariantCulture, $"template struct ::rocks_meta_canon::RocksCanonFact<{index}, {constant}>;");

    // 정규 선언 이름(익명 이름공간 포함)을 C++ 전역 한정 철자로 바꾼다. 익명 이름공간은 바깥 이름공간에서 한정 조회로 보인다.
    public static string Spell(string qualified) =>
        "::" + qualified.Replace("(anonymous namespace)::", "", StringComparison.Ordinal);

    // 정규 형식 철자(builtin·cv·포인터를 담을 수 있다)를 전역 범위의 질문 줄에 쓸 철자로 바꾼다. 질문 줄은 전역 범위라 앞에 `::`를 붙이지
    // 않아도 이름이 전역에서 찾아진다. 익명 이름공간은 한 칸을 뺀다.
    public static string SpellType(string canonical) =>
        canonical.Replace("(anonymous namespace)::", "", StringComparison.Ordinal);

    // 덤프(JSON 값 여러 개)에서 질문 번호별 답을 읽는다. 형식 질문·base 질문은 정규 철자, 사실 질문은 10진 정수 문자열이다.
    // 덤프를 해석하지 못하면 null이다. 같은 특수화가 클래스 템플릿 노드 안과 최상위에 두 번 나오므로 번호로 한 번만 받는다.
    public static Dictionary<int, string>? Read(string output)
    {
        Dictionary<int, string> answers = [];
        try
        {
            Utf8JsonReader reader = new(Encoding.UTF8.GetBytes(output), new JsonReaderOptions { AllowMultipleValues = true, MaxDepth = 2048 });
            while (reader.Read())
            {
                using JsonDocument document = JsonDocument.ParseValue(ref reader);
                Collect(document.RootElement, answers);
            }
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException)
        {
            return null;
        }

        return answers;
    }

    private static void Collect(JsonElement node, Dictionary<int, string> answers)
    {
        if (node.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        string kind = node.TryGetProperty("kind", out JsonElement kindValue) ? kindValue.GetString() ?? "" : "";
        string name = node.TryGetProperty("name", out JsonElement nameValue) ? nameValue.GetString() ?? "" : "";
        if (kind == "ClassTemplateSpecializationDecl" && name is "RocksCanonType" or "RocksCanonBase" or "RocksCanonFact" &&
            node.TryGetProperty("inner", out JsonElement inner))
        {
            JsonElement[] arguments = inner.EnumerateArray()
                .Where(child => child.TryGetProperty("kind", out JsonElement childKind) && childKind.GetString() == "TemplateArgument").ToArray();
            int answer = name == "RocksCanonBase" ? 2 : 1;
            if (arguments.Length > answer && arguments[0].TryGetProperty("value", out JsonElement number) && number.ValueKind == JsonValueKind.Number &&
                number.TryGetInt32(out int index) && index >= 0 && !answers.ContainsKey(index))
            {
                if (name == "RocksCanonFact")
                {
                    if (arguments[1].TryGetProperty("value", out JsonElement fact) && fact.ValueKind == JsonValueKind.Number && fact.TryGetInt64(out long value))
                    {
                        answers[index] = value.ToString(CultureInfo.InvariantCulture);
                    }
                }
                else if (arguments[answer].TryGetProperty("type", out JsonElement type) && type.TryGetProperty("qualType", out JsonElement spelling) &&
                    spelling.GetString() is string text)
                {
                    answers[index] = text;
                }
            }
        }

        if (node.TryGetProperty("inner", out JsonElement children))
        {
            foreach (JsonElement child in children.EnumerateArray())
            {
                Collect(child, answers);
            }
        }
    }

    // 별칭 탐침(Binds·Within)에 붙이는 정규 형식 래퍼 선언 한 줄이다. 이름공간 이름(`rocksBinds_canon`)이 그 탐침의 -ast-dump-filter
    // 접두를 담으므로 필터를 바꾸지 않는다(4.7절).
    public static string WrapperTemplate(string space) =>
        $"namespace {space} {{ template <int RocksCanonN, class RocksCanonT> struct RocksCanonType {{}}; }}\n";

    // 별칭 탐침의 래퍼 인스턴스화 한 줄이다.
    public static string WrapperLine(string space, int index, string type) =>
        string.Create(CultureInfo.InvariantCulture, $"template struct {space}::RocksCanonType<{index}, {type}>;\n");
}
