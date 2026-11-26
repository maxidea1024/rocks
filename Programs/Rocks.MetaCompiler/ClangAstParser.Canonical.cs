using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.MetaCompiler;

// ClangAstParser 중 정규 형식 탐침을 쓰는 부분이다(Docs/Design/ClangVersionSupport.ko.md 4절). 1단계(CanonicalizeAsync)는 이 모듈
// 리플렉션 타입의 PROPERTY·NATIVE 형식, 정적 배열 원소·크기, base를 Clang 정규 철자로 바꾼다. 레코드 닫힘(VerifyNativeMembersAsync)은
// NATIVE 멤버가 값으로 닿는 레코드의 필드·base·형질을 라운드마다 물어 강한 참조(RMC2205)·WorldOnly 소유자 형질 경로를 찾는다. 형식에
// 관한 판단은 탐침이 돌려준 정규 철자와 사실로만 하고, 첫 실행 JSON은 선언 구조(이름·필드 이름·base 목록·템플릿 인자·완전한지)만 준다.
internal sealed partial class ClangAstParser
{
    // 레코드 닫힘이 NATIVE 멤버 하나에서 걷는 형식 수와 전체 라운드 수의 상한이다(넘으면 그 NATIVE가 cannot verify — 결정적으로 멈춘다).
    private const int MaxClosureTypes = 4096;

    private const int MaxClosureRounds = 32;

    // 특수화 철자 안에서 NTTP 값 한 칸(ParamCast)의 자리를 가리키는 표지 글자다. 별칭 이름이 질문 번호마다 달라 질문을 만들 때 채운다.
    private const char CastMarker = '\u0001';

    // 열쇠를 만들 수 없는 레코드의 이유다(4.4절 부를 수 없는 특수화).
    private const string UnspellableClass =
        "the class cannot be spelled from its declaration (a template template, address or class-type template argument, or a parameter type that depends on another parameter)";

    // 둘러싼 이름공간 한 칸이다(4.3절 base 질문 2와 NTTP 별칭을 그 이름공간에 다시 열어 둔다).
    private sealed record NamespacePart(string Name, bool Anonymous);

    // 클래스 템플릿 매개변수 하나다(템플릿의 첫 선언에서). Kind는 "type"·"value"·"template", Written은 NTTP의 쓴 형식이다.
    private sealed record TemplateParameter(string Kind, string Name, string Written, bool Pack);

    // 특수화 철자 안의 NTTP 값 한 칸이다(4.4절 열쇠). 쓴 매개변수 형식(enum·typedef)을 템플릿을 둘러싼 이름공간의 별칭으로, 템플릿이
    // 클래스 안에 있으면 둘러싼 클래스 철자(Owner)로 한정해서도 부른다. Literal은 `값LL`이다.
    private sealed record ParamCast(IReadOnlyList<NamespacePart> Namespaces, string? Owner, string Written, string Literal);

    // 첫 실행 JSON의 이름 있는 레코드 하나다(비템플릿 레코드, 특수화 노드, typedef 이름을 받은 이름 없는 class — 패턴 레코드는 넣지 않는다).
    private sealed class ClosureRecord
    {
        // 식별자다(특수화는 템플릿 이름).
        public required string Name { get; init; }

        // 선언 경로다(익명 이름공간은 "(anonymous namespace)").
        public required string DeclPath { get; init; }

        // 전역 한정 C++ 철자다(특수화는 `::ns::Name<인자…>`, NTTP 값 자리는 Casts의 표지). 부를 수 없으면 null이다.
        public required string? Spelling { get; init; }

        public required IReadOnlyList<ParamCast> Casts { get; init; }

        // 선언 경로가 정규 철자와 같다(경로에 inline 이름공간·특수화가 없다). 아니면 정규 철자를 열쇠 질문으로 얻는다.
        public required bool DirectKeyed { get; init; }

        public required IReadOnlyList<NamespacePart> Namespaces { get; init; }

        public required bool Specialization { get; init; }

        // 특수화이거나 특수화 안의 레코드다(base 목록이 치환된 형식이다).
        public required bool Substituted { get; init; }

        // 첫 실행에서 완전한 정의다. 불완전한 레코드(앞 선언, 인스턴스화되지 않은 특수화)는 필드·base가 없다.
        public required bool Complete { get; init; }

        // 필드 멤버 경로다(`x`, 이름 없는 struct 멤버는 `inner.x`·`arr[0].x`).
        public List<string> Fields { get; } = [];

        // 특수화의 템플릿 인자(pack은 펼친 순서)마다 형식 인자인지다. 정규 철자의 인자와 같은 순서다(정규 출력은 끝의 기본 인자만 생략한다).
        public List<bool> ArgumentTypes { get; } = [];

        // base의 TypeSpelling(desugar 철자 우선)과 쓴 철자다.
        public List<(string Resolved, string Written)> Bases { get; } = [];

        // 따라갈 수 없는 이유다(base가 있는 이름 없는 struct 멤버). 닿으면 cannot verify다.
        public string? Unfollowable { get; set; }

        // 필드·base·사실 질문에 쓸 철자다. NTTP 별칭이 없으면 Spelling이고, 있으면 열쇠 답(정규 철자)이다.
        public string? Query { get; set; }

        public bool KeyAsked { get; set; }

        // 열쇠를 얻지 못한 이유다(부를 수 없음, 열쇠 질문 실패).
        public string? KeyFailure { get; set; }
    }

    // 레코드 닫힘이 물어 얻은 레코드 하나의 사실이다. null 칸은 그 질문이 실패한 것이다(Failures에 Clang error 줄).
    private sealed class ClosureFacts
    {
        public string?[] FieldTypes { get; init; } = [];

        public string?[] BaseTypes { get; init; } = [];

        // base 질문 단계(0: 1로 묻는 중, 1: 1이 실패해 2로 묻는 중, 2: 둘 다 실패).
        public int[] BaseSteps { get; init; } = [];

        public bool? ObjectDerived { get; set; }

        public bool? WorldOnly { get; set; }

        public Dictionary<string, string> Failures { get; } = new(StringComparer.Ordinal);
    }

    // 색인 중의 선언 범위다. Spelling은 그 범위의 전역 한정 철자(NTTP 표지 포함), Exact는 선언 경로가 정규 철자와 같은지, InClass는
    // 클래스 안인지, Substituted는 특수화 안인지다.
    private readonly record struct ClosureScope(IReadOnlyList<NamespacePart> Namespaces, string DeclPath, string? Spelling, IReadOnlyList<ParamCast> Casts,
        bool Exact, bool InClass, bool Substituted);

    private readonly List<ClosureRecord> closureRecords = [];

    // 노드 id → closureRecords 칸이다(같은 선언이 여러 번 나오면 완전한 쪽, 부를 수 있는 쪽이 이긴다).
    private readonly Dictionary<string, int> closureSlots = new(StringComparer.Ordinal);

    private readonly Dictionary<string, ClosureRecord> closureByDecl = new(StringComparer.Ordinal);

    // 클래스 템플릿(선언 경로) → 첫 선언의 템플릿 매개변수다.
    private readonly Dictionary<string, List<TemplateParameter>> templateParameters = new(StringComparer.Ordinal);

    // 정규 형식 탐침의 형식 철자가 builtin이면 걷기를 끝낸다(class 여부를 묻지 않는다).
    private static readonly HashSet<string> BuiltinSpellings = new(StringComparer.Ordinal)
    {
        "void", "bool", "char", "signed char", "unsigned char", "wchar_t", "char8_t", "char16_t", "char32_t", "short", "unsigned short", "int",
        "unsigned int", "long", "unsigned long", "long long", "unsigned long long", "__int128", "unsigned __int128", "float", "double", "long double",
        "std::nullptr_t", "decltype(nullptr)", "_Float16", "__bf16"
    };

    private static readonly Regex Identifier = new(@"[A-Za-z_]\w*", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private void IndexClosure(JsonElement root) => IndexChildren(root, new([], "", "", [], true, false, false));

    // 자식 선언을 색인한다. typedef로 이름을 받은 이름 없는 class(`typedef struct { … } Name;` — 정규 철자가 `ns::Name`이다)는 바로 뒤
    // typedef의 이름으로 색인하고, 그 밖의 이름 없는 class는 넣지 않는다(익명·선언자 멤버는 부모 필드로 묻는다).
    private void IndexChildren(JsonElement parent, ClosureScope scope)
    {
        JsonElement[] children = Children(parent).ToArray();
        for (int index = 0; index < children.Length; ++index)
        {
            JsonElement child = children[index];
            if (Text(child, "kind") == "CXXRecordDecl" && Text(child, "name").Length == 0)
            {
                if (index + 1 < children.Length && TypedefName(children[index + 1], Text(child, "id")) is string typedefName)
                {
                    IndexClosureRecord(child, scope, typedefName, typedefName, scope.Casts, false);
                }

                continue;
            }

            IndexClosure(child, scope);
        }
    }

    // 선언 하나를 색인한다. 함수 본문(지역 클래스)과 부분 특수화(패턴)에는 들어가지 않는다 — 지역 클래스는 전역 한정 철자로 부를 수
    // 없으므로 색인에 없고, 닿으면 IsClassV로 가려 cannot verify다.
    private void IndexClosure(JsonElement node, ClosureScope scope)
    {
        string kind = Text(node, "kind");
        string name = Text(node, "name");
        switch (kind)
        {
            case "LinkageSpecDecl" or "ExportDecl":
                IndexChildren(node, scope);
                return;
            case "NamespaceDecl":
                {
                    bool anonymous = name.Length == 0;
                    IndexChildren(node, scope with
                    {
                        Namespaces = [.. scope.Namespaces, new(name, anonymous)],
                        DeclPath = Join(scope.DeclPath, anonymous ? "(anonymous namespace)" : name),
                        Spelling = scope.Spelling is null ? null : anonymous ? scope.Spelling : scope.Spelling + "::" + name,
                        Exact = scope.Exact && !Flag(node, "isInline")
                    });
                    return;
                }

            case "ClassTemplateDecl":
                {
                    string path = Join(scope.DeclPath, name);
                    List<TemplateParameter> parameters = [];
                    foreach (JsonElement child in Children(node))
                    {
                        string? parameterKind = Text(child, "kind") switch
                        {
                            "TemplateTypeParmDecl" => "type",
                            "NonTypeTemplateParmDecl" => "value",
                            "TemplateTemplateParmDecl" => "template",
                            _ => null
                        };
                        if (parameterKind is not null)
                        {
                            parameters.Add(new(parameterKind, Text(child, "name"), child.TryGetProperty("type", out JsonElement type) ? Text(type, "qualType") : "",
                                Flag(child, "isParameterPack")));
                        }
                    }

                    templateParameters.TryAdd(path, parameters);
                    foreach (JsonElement child in Children(node))
                    {
                        if (Text(child, "kind") == "ClassTemplateSpecializationDecl")
                        {
                            IndexSpecialization(child, scope, name, path);
                        }
                    }

                    return;
                }

            case "ClassTemplateSpecializationDecl":
                IndexSpecialization(node, scope, name, Join(scope.DeclPath, name));
                return;
            case "CXXRecordDecl":
                // 주입 클래스 이름(암시적 CXXRecordDecl)은 그 클래스 자신이다.
                if (name.Length != 0 && !Flag(node, "isImplicit"))
                {
                    IndexClosureRecord(node, scope, name, name, scope.Casts, false);
                }

                return;
            default:
                return;
        }
    }

    // 특수화 노드 하나를 색인한다. 형식 인자는 정규 qualType을, NTTP 값은 패턴의 매개변수 형식으로 쓴다(SpellValue). 템플릿 템플릿·주소·
    // class 값 인자가 있으면 부를 수 없는 레코드다(열쇠 없음).
    private void IndexSpecialization(JsonElement node, ClosureScope scope, string name, string templatePath)
    {
        List<TemplateParameter>? parameters = templateParameters.GetValueOrDefault(templatePath);
        JsonElement[] arguments = Children(node).Where(child => Text(child, "kind") == "TemplateArgument").ToArray();
        bool spellable = parameters is not null && parameters.Count == arguments.Length;
        List<string> spelled = [];
        List<bool> argumentTypes = [];
        List<ParamCast> casts = [.. scope.Casts];
        Dictionary<string, string> typeArguments = new(StringComparer.Ordinal);
        for (int position = 0; position < arguments.Length; ++position)
        {
            TemplateParameter? parameter = spellable ? parameters![position] : null;
            JsonElement argument = arguments[position];
            IEnumerable<JsonElement> elements = Flag(argument, "isPack") ? Children(argument).Where(child => Text(child, "kind") == "TemplateArgument") : [argument];
            foreach (JsonElement element in elements)
            {
                string? text = null;
                bool isType = element.TryGetProperty("type", out JsonElement type);
                if (isType)
                {
                    text = SpellArgument(Text(type, "qualType"));
                    if (text is not null && parameter is { Kind: "type", Pack: false } && parameter.Name.Length != 0)
                    {
                        typeArguments[parameter.Name] = text;
                    }
                }
                else if (parameter is { Kind: "value" } && element.TryGetProperty("value", out JsonElement value) && value.ValueKind == JsonValueKind.Number)
                {
                    text = SpellValue(parameter, value.GetRawText(), parameters!, typeArguments, scope, casts);
                }

                spellable &= text is not null;
                spelled.Add(text ?? "");
                argumentTypes.Add(isType);
            }
        }

        IndexClosureRecord(node, scope, name, spellable ? $"{name}<{string.Join(", ", spelled)}>" : null, spellable ? casts : scope.Casts, true)?
            .ArgumentTypes.AddRange(argumentTypes);
    }

    // 특수화 노드의 형식 인자(정규 철자)를 전역 범위에서 부를 철자로 바꾼다. 익명 이름공간은 한 칸을 빼고, 람다·이름 없는 타입은 부를 수 없다.
    private static string? SpellArgument(string type)
    {
        string text = type.Replace("(anonymous namespace)::", "", StringComparison.Ordinal);
        return text.Length == 0 || text.Contains("(lambda", StringComparison.Ordinal) || text.Contains("(unnamed", StringComparison.Ordinal) ||
            text.Contains("(anonymous", StringComparison.Ordinal) || text.Contains("<dependent type>", StringComparison.Ordinal) ? null : text;
    }

    // NTTP 값 한 칸의 철자다(4.4절 열쇠). 덤퍼의 value는 64비트 2의 보수(bool true·unsigned long long 최댓값이 -1)라 매개변수 형식으로
    // 캐스트해 쓴다: bool은 true·false, builtin은 그대로, 템플릿의 형식 매개변수면 그 인자의 정규 철자, enum·typedef면 ParamCast 표지.
    // 매개변수 형식이 다른 매개변수에 기대면(`typename T::X`, `auto`) 부를 수 없다(null).
    private static string? SpellValue(TemplateParameter parameter, string raw, List<TemplateParameter> parameters, Dictionary<string, string> typeArguments,
        ClosureScope scope, List<ParamCast> casts)
    {
        string written = CleanType(parameter.Written);
        if (written.StartsWith("typename ", StringComparison.Ordinal))
        {
            written = written["typename ".Length..].TrimStart();
        }

        string literal = raw == long.MinValue.ToString(CultureInfo.InvariantCulture) ? "(-9223372036854775807LL - 1)" : raw + "LL";
        string? substituted = typeArguments.GetValueOrDefault(written);
        string type = substituted ?? written;
        if (type == "bool")
        {
            return raw == "0" ? "false" : "true";
        }

        if (substituted is not null || IsBuiltinSpelling(type))
        {
            return $"static_cast<{type}>({literal})";
        }

        if (written.Length == 0 || Identifier.Matches(written).Any(match => match.Value == "auto" || parameters.Any(other => other.Name.Length != 0 && other.Name == match.Value)))
        {
            return null;
        }

        bool qualified = written.StartsWith("::", StringComparison.Ordinal) || parameter.Written.TrimStart().StartsWith("::", StringComparison.Ordinal);
        casts.Add(new(scope.Namespaces, scope.InClass && scope.Casts.Count == 0 && !qualified ? scope.Spelling : null, qualified ? "::" + written : written, literal));
        return string.Create(CultureInfo.InvariantCulture, $"{CastMarker}{casts.Count - 1}{CastMarker}");
    }

    private ClosureRecord? IndexClosureRecord(JsonElement node, ClosureScope scope, string name, string? segment, IReadOnlyList<ParamCast> casts, bool specialization)
    {
        string id = Text(node, "id");
        bool complete = Flag(node, "completeDefinition");
        string? spelling = scope.Spelling is null || segment is null ? null : scope.Spelling + "::" + segment;
        int slot = id.Length != 0 && closureSlots.TryGetValue(id, out int found) ? found : -1;
        if (slot >= 0)
        {
            ClosureRecord existing = closureRecords[slot];
            if (existing.Complete || (!complete && (existing.Spelling is not null || spelling is null)))
            {
                return null;
            }
        }

        string declPath = Join(scope.DeclPath, name);
        ClosureRecord record = new()
        {
            Name = name,
            DeclPath = declPath,
            Spelling = spelling,
            Casts = casts,
            DirectKeyed = scope.Exact && !specialization,
            Namespaces = scope.Namespaces,
            Specialization = specialization,
            Substituted = specialization || scope.Substituted,
            Complete = complete,
            Query = casts.Count == 0 ? spelling : null
        };

        if (slot >= 0)
        {
            closureRecords[slot] = record;
        }
        else
        {
            if (id.Length != 0)
            {
                closureSlots.Add(id, closureRecords.Count);
            }

            closureRecords.Add(record);
        }

        if (!complete)
        {
            return record;
        }

        // 필드를 멤버 경로로 모은다(4.4절). 익명 struct·union(바로 뒤에 암시적인 이름 없는 필드)의 필드는 부모 범위로 주입되므로 같은 경로
        // 앞에 펼치고, 선언자가 있는 이름 없는 struct는 그 필드의 멤버 경로(`inner.x`, 배열이면 `inner[0].x`)로 펼친다.
        void Collect(JsonElement owner, string prefix)
        {
            JsonElement[] children = Children(owner).ToArray();
            JsonElement? unnamed = null;
            for (int index = 0; index < children.Length; ++index)
            {
                JsonElement child = children[index];
                string childKind = Text(child, "kind");
                string childName = Text(child, "name");
                if (childKind == "CXXRecordDecl" && childName.Length == 0 && Flag(child, "completeDefinition"))
                {
                    JsonElement? next = index + 1 < children.Length ? children[index + 1] : null;
                    if (next is JsonElement field && Text(field, "kind") == "FieldDecl" && Text(field, "name").Length == 0 && Flag(field, "isImplicit"))
                    {
                        Collect(child, prefix);
                    }
                    else
                    {
                        unnamed = child;
                    }
                }
                else if (childKind == "FieldDecl" && childName.Length != 0)
                {
                    string path = prefix + childName;
                    if (unnamed is JsonElement inner && child.TryGetProperty("type", out JsonElement type) && UnnamedMemberSuffix(Text(type, "qualType")) is string suffix)
                    {
                        if (inner.TryGetProperty("bases", out JsonElement innerBases) && innerBases.GetArrayLength() != 0)
                        {
                            record.Unfollowable ??= $"member '{path}' is an unnamed class with a base";
                        }

                        Collect(inner, path + suffix + ".");
                    }
                    else
                    {
                        record.Fields.Add(path);
                    }
                }
            }
        }

        Collect(node, "");
        if (node.TryGetProperty("bases", out JsonElement bases))
        {
            foreach (JsonElement entry in bases.EnumerateArray())
            {
                record.Bases.Add((TypeSpelling(entry), entry.TryGetProperty("type", out JsonElement type) ? Text(type, "qualType") : ""));
            }
        }

        if (!specialization)
        {
            closureByDecl.TryAdd(declPath, record);
        }

        IndexChildren(node, new(scope.Namespaces, declPath, spelling, casts, scope.Exact && !specialization, true, record.Substituted));
        return record;
    }

    // typedef 선언이 바로 앞의 이름 없는 class(id)에 링크 이름을 주면 그 이름을, 아니면 null을 돌려준다(Clang 22는 RecordType의 decl,
    // 21은 ElaboratedType의 ownedTagDecl로 가리킨다).
    private static string? TypedefName(JsonElement node, string id)
    {
        string name = Text(node, "name");
        return Text(node, "kind") == "TypedefDecl" && name.Length != 0 && id.Length != 0 && node.TryGetProperty("type", out JsonElement type) &&
            CleanType(Text(type, "qualType")) == name && RefersTo(node, id) ? name : null;
    }

    private static bool RefersTo(JsonElement node, string id) =>
        (node.TryGetProperty("decl", out JsonElement decl) && decl.ValueKind == JsonValueKind.Object && Text(decl, "id") == id) ||
        (node.TryGetProperty("ownedTagDecl", out JsonElement owned) && owned.ValueKind == JsonValueKind.Object && Text(owned, "id") == id) ||
        Children(node).Any(child => RefersTo(child, id));

    // 선언자가 있는 이름 없는 struct 필드의 형식(`struct (unnamed at …)`, 배열 `…[2]`)이면 멤버 경로에 붙일 첨자(`""`, 차원마다 `"[0]"`)를,
    // 아니면(포인터·참조, 이름 있는 형식) null을 돌려준다.
    private static string? UnnamedMemberSuffix(string type)
    {
        string text = CleanType(type);
        if (!text.Contains("(unnamed", StringComparison.Ordinal) && !text.Contains("(anonymous struct", StringComparison.Ordinal) &&
            !text.Contains("(anonymous union", StringComparison.Ordinal) && !text.Contains("(anonymous class", StringComparison.Ordinal))
        {
            return null;
        }

        string suffix = "";
        while (StripArray(text) is string element)
        {
            text = element;
            suffix += "[0]";
        }

        return text.EndsWith(')') ? suffix : null;
    }

    private static string Join(string scope, string name) => scope.Length == 0 ? name : scope + "::" + name;

    // 이름공간 칸들 안에 별칭을 여는 선언(같은 줄)과 그 별칭을 전역에서 부르는 철자다. 익명 이름공간은 바깥에서 한정 조회로 보이므로
    // 철자에서 뺀다.
    private static (string Declaration, string Reference) NamespaceAlias(IReadOnlyList<NamespacePart> namespaces, string alias, string type)
    {
        StringBuilder declaration = new();
        foreach (NamespacePart part in namespaces)
        {
            declaration.Append(part.Anonymous ? "namespace { " : $"namespace {part.Name} {{ ");
        }

        declaration.Append($"using {alias} = {type}; ");
        declaration.Append(string.Concat(Enumerable.Repeat("} ", namespaces.Count)));
        return (declaration.ToString(), string.Concat(namespaces.Where(part => !part.Anonymous).Select(part => "::" + part.Name)) + "::" + alias);
    }

    // 열쇠 질문 줄이다: NTTP 표지를 `static_cast<P>(값)`으로 채운 철자의 형식 질문. owner가 참이면 P를 둘러싼 클래스로 한정한 철자로,
    // 아니면 템플릿 이름공간의 별칭으로 부른다(4.4절 — 틀린 쪽은 변환 실패로 거부되므로 두 답이 갈리지 않는다).
    private static string KeyQuestion(int index, ClosureRecord record, bool owner)
    {
        StringBuilder prelude = new();
        string text = record.Spelling!;
        for (int position = 0; position < record.Casts.Count; ++position)
        {
            ParamCast cast = record.Casts[position];
            string type;
            if (owner && cast.Owner is not null)
            {
                type = cast.Owner + "::" + cast.Written;
            }
            else
            {
                (string declaration, string reference) = NamespaceAlias(cast.Namespaces,
                    string.Create(CultureInfo.InvariantCulture, $"rocksCanonParam_{index}_{position}"), cast.Written);
                prelude.Append(declaration);
                type = reference;
            }

            text = text.Replace(string.Create(CultureInfo.InvariantCulture, $"{CastMarker}{position}{CastMarker}"), $"static_cast<{type}>({cast.Literal})",
                StringComparison.Ordinal);
        }

        return prelude.ToString() + CanonicalTypeProbe.TypeLine(index, text);
    }

    // base 질문 줄이다. alias가 거짓이면 정교한 형식 지정자 `struct ::Owner::식별자`(1), 참이면 base 철자를 소유자를 둘러싼 이름공간의
    // 별칭으로 부른다(2 — 특수화와 그 안의 레코드의 base는 치환된 철자, 그 밖에는 쓴 철자).
    private static string BaseQuestion(int index, ClosureRecord owner, int baseIndex, bool alias)
    {
        (string resolved, string written) = owner.Bases[baseIndex];
        if (!alias)
        {
            return CanonicalTypeProbe.BaseLine(index, owner.Query!, $"struct {owner.Query}::{LastIdentifier(resolved)}");
        }

        (string declaration, string reference) = NamespaceAlias(owner.Namespaces, string.Create(CultureInfo.InvariantCulture, $"rocksCanonAlias_{index}"),
            owner.Substituted ? resolved : written);
        return CanonicalTypeProbe.BaseLine(index, owner.Query!, reference, declaration);
    }

    // 파생 이름(템플릿이면 템플릿 이름)이 base 식별자와 같거나 같은 식별자 base가 여럿이면 1이 파생 자신이나 모호한 이름을 찾으므로
    // 처음부터 2로 묻는다.
    private static bool BaseNeedsAlias(ClosureRecord owner, int baseIndex)
    {
        string identifier = LastIdentifier(owner.Bases[baseIndex].Resolved);
        return identifier == owner.Name || owner.Bases.Count(entry => LastIdentifier(entry.Resolved) == identifier) > 1;
    }

    // 1의 답은 마지막 식별자가 물은 식별자와 같을 때만 받는다(4.3절 — 같은 이름의 멤버 typedef가 다른 class를 가리키면 2로 간다).
    private static bool BaseAnswerMatches(ClosureRecord owner, int baseIndex, string answer) =>
        LastIdentifier(answer) == LastIdentifier(owner.Bases[baseIndex].Resolved);

    // 질문 묶음이다(번호는 실행마다 0부터).
    private sealed class ProbeBatch
    {
        public List<ClangProbe.Question> Questions { get; } = [];

        public int Add(Func<int, string> line)
        {
            int index = Questions.Count;
            Questions.Add(new(index, line(index)));
            return index;
        }
    }

    // 묶음을 실행해 질문별 답·실패를 얻는다. 어느 질문에도 대응하지 않는 실패가 형판만으로도 나면 탐침 자체의 실패(RMC3000)다.
    private static async Task<ClangProbe.Outcome<string>> AskAsync(ClangProbe.Session<string> session, string prelude, ProbeBatch batch, SourceSite site)
    {
        ClangProbe.Outcome<string> outcome = new();
        if (batch.Questions.Count == 0)
        {
            return outcome;
        }

        ClangProbe.Run<string> first = await session.RunAsync(prelude, batch.Questions, "");
        if (first.Answers is null && first.Unattributed)
        {
            ClangProbe.Run<string> bare = await session.RunAsync(prelude, [], "");
            if (bare.Answers is null)
            {
                throw new MetadataException($"Clang rejected the canonical type probe itself, before any question (Clang: {bare.Failure})", "RMC3000",
                    "The probe appended after the reflected headers failed with no question in it; check for a macro or declaration in the reflected headers that collides with the probe (rocks_meta_canon, RocksCanon*, rocksCanonAlias_*, rocksCanonParam_*) and use the supported Clang toolchain.")
                    .At(site);
            }
        }

        await session.JudgeAsync(prelude, batch.Questions, "", outcome, first);
        return outcome;
    }

    // 1단계: 이 모듈 리플렉션 타입의 PROPERTY 필드(bitfield 제외)·NATIVE 멤버 형식, C 배열 원소·크기, 클래스·struct base를 정규 철자로
    // 바꾼다. 질문이 실패하면 그 선언 위치의 RMC2001이다(base는 1이 실패하거나 다른 식별자를 답하면 2로 한 번 더 묻는다).
    public async Task CanonicalizeAsync(Dictionary<string, ClassModel> declared, ClangProbe.Session<string> session)
    {
        ClassModel[] models = declared.Values.Where(model => !model.IsEnum).OrderBy(model => model.Name, StringComparer.Ordinal).ToArray();
        if (models.Length == 0)
        {
            return;
        }

        const string hint = "Use the supported Clang toolchain; the metadata compiler names every reflected member and base through Clang's canonical types (Docs/Design/ClangVersionSupport.ko.md).";
        ProbeBatch batch = new();
        List<(int Index, Action<string> Apply, Func<string, MetadataException> Fail)> answers = [];
        List<(ClassModel Model, ClosureRecord Owner, int Base)> bases = [];
        foreach (ClassModel model in models)
        {
            string owner = CanonicalTypeProbe.Spell(model.Name);
            foreach (FieldModel field in model.OwnFields.Where(field => !field.Bitfield))
            {
                string member = $"decltype({owner}::{field.Name})";
                string written = field.NativeSpelling;
                MetadataException Fail(string clang) =>
                    new MetadataException($"{model.Name}::{field.Name}: Clang did not name the field type '{written}' (Clang: {clang})", "RMC2001", hint).At(field.Site);
                answers.Add((batch.Add(index => CanonicalTypeProbe.TypeLine(index, member)), answer => field.NativeSpelling = answer, Fail));
                if (StaticArrayRules.Split(written) is not null)
                {
                    string? element = null;
                    answers.Add((batch.Add(index => CanonicalTypeProbe.TypeLine(index, $"typename ::rocks_meta_canon::RocksStaticShape<{member}>::rocksStaticElement")),
                        answer => element = answer, Fail));
                    answers.Add((batch.Add(index => CanonicalTypeProbe.FactLine(index, $"::rocks_meta_canon::RocksStaticShape<{member}>::rocksStaticExtent")),
                        answer => field.CanonicalSpelling = $"{element}[{answer}]", Fail));
                }
            }

            // 리플렉션 함수의 매개변수·반환 형식(Docs/Design/FunctionReflection.ko.md FD4·FD5)은 멤버 함수 포인터 형식을 RocksFunctionShape로
            // 나눠 묻는다(이름에 오버로드가 없다 — RMC2702). 답은 cv·참조를 담은 정규 철자다(값 종류 판정이 벗긴다).
            foreach (FunctionModel function in model.Functions)
            {
                string shape = $"typename ::rocks_meta_canon::RocksFunctionShape<decltype(&{owner}::{function.Name})>";
                for (int index = 0; index < function.Parameters.Count; ++index)
                {
                    FieldModel slot = function.Parameters[index];
                    string written = slot.NativeSpelling;
                    MetadataException Fail(string clang) =>
                        new MetadataException($"{model.Name}::{function.Name}: Clang did not name the parameter type '{written}' (Clang: {clang})", "RMC2001", hint).At(function.Site);
                    string question = $"{shape}::template rocksParameter<{index}>";
                    answers.Add((batch.Add(position => CanonicalTypeProbe.TypeLine(position, question)), answer => slot.NativeSpelling = answer, Fail));
                }

                if (function.Result is FieldModel result)
                {
                    string written = result.NativeSpelling;
                    MetadataException Fail(string clang) =>
                        new MetadataException($"{model.Name}::{function.Name}: Clang did not name the return type '{written}' (Clang: {clang})", "RMC2001", hint).At(function.Site);
                    answers.Add((batch.Add(position => CanonicalTypeProbe.TypeLine(position, $"{shape}::rocksResult")), answer => result.NativeSpelling = answer, Fail));
                }
            }

            foreach (NativeMemberModel native in model.NativeMembers)
            {
                string member = $"decltype({owner}::{native.Name})";
                string written = native.Spelling;
                MetadataException Fail(string clang) =>
                    new MetadataException($"{model.Name}::{native.Name}: Clang did not name the member type '{written}' (Clang: {clang})", "RMC2001", hint).At(native.Site);
                answers.Add((batch.Add(index => CanonicalTypeProbe.TypeLine(index, member)), answer => native.Spelling = answer, Fail));
                if (StaticArrayRules.Split(CleanType(written)) is not null)
                {
                    answers.Add((batch.Add(index => CanonicalTypeProbe.TypeLine(index, $"typename ::rocks_meta_canon::RocksStaticBase<{member}>::rocksStaticType")),
                        answer => native.ElementSpelling = answer, Fail));
                }
            }

            if (closureByDecl.TryGetValue(model.Name, out ClosureRecord? record) && record.Query is not null)
            {
                for (int index = 0; index < record.Bases.Count; ++index)
                {
                    bases.Add((model, record, index));
                }
            }
        }

        // base 답을 모델에 옮긴다: 클래스는 첫(유일한) base, struct는 순서대로 StructBaseSpellings다.
        void ApplyBase(ClassModel model, int index, string answer)
        {
            if (model.IsStruct)
            {
                if (index < model.StructBaseSpellings.Count)
                {
                    model.StructBaseSpellings[index] = answer;
                }
            }
            else if (index == 0)
            {
                model.Base = answer;
            }
        }

        Dictionary<int, (ClassModel Model, ClosureRecord Owner, int Base)> baseQuestions = [];
        foreach ((ClassModel model, ClosureRecord owner, int index) in bases)
        {
            baseQuestions.Add(batch.Add(question => BaseQuestion(question, owner, index, BaseNeedsAlias(owner, index))), (model, owner, index));
        }

        string prelude = CanonicalTypeProbe.Templates;
        ClangProbe.Outcome<string> outcome = await AskAsync(session, prelude, batch, models[0].Site);
        foreach ((int index, Action<string> apply, Func<string, MetadataException> fail) in answers)
        {
            if (outcome.Failures.TryGetValue(index, out string? failure))
            {
                throw fail(failure);
            }

            apply(outcome.Answers[index]);
        }

        // 1로 실패했거나 다른 식별자를 답한 base는 2(이름공간 별칭)로 한 번 더 묻는다. 그래도 실패하면 RMC2001이다.
        ProbeBatch retry = new();
        Dictionary<int, (ClassModel Model, ClosureRecord Owner, int Base, string First)> retried = [];
        foreach ((int index, (ClassModel model, ClosureRecord owner, int position)) in baseQuestions)
        {
            bool alias = BaseNeedsAlias(owner, position);
            if (outcome.Answers.TryGetValue(index, out string? answer) && (alias || BaseAnswerMatches(owner, position, answer)))
            {
                ApplyBase(model, position, answer);
                continue;
            }

            string first = outcome.Failures.GetValueOrDefault(index) ?? $"Clang named '{answer}' for a base named {LastIdentifier(owner.Bases[position].Resolved)}";
            if (alias)
            {
                throw BaseFailure(model, owner, position, first);
            }

            retried.Add(retry.Add(question => BaseQuestion(question, owner, position, true)), (model, owner, position, first));
        }

        ClangProbe.Outcome<string> second = await AskAsync(session, prelude, retry, models[0].Site);
        foreach ((int index, (ClassModel model, ClosureRecord owner, int position, string first)) in retried)
        {
            if (!second.Answers.TryGetValue(index, out string? answer))
            {
                throw BaseFailure(model, owner, position, first);
            }

            ApplyBase(model, position, answer);
        }

        static MetadataException BaseFailure(ClassModel model, ClosureRecord owner, int position, string failure) =>
            new MetadataException($"{model.Name}: Clang did not name base '{owner.Bases[position].Written}' through the derived class or its namespace (Clang: {failure})",
                "RMC2001", "Spell the base so that it is a direct base visible from the class's namespace (not a protected or private nested type of another class, not two bases with the same name nested in an outer class).")
                .At(model.Site);
    }

    // 레코드 닫힘(4.4절): NATIVE 멤버가 값으로 닿는 레코드의 필드·base·형질을 라운드마다 묻고, 강한 참조 경로면 RMC2205, 따라갈 수 없는
    // class(색인에 없음·부를 수 없음·열쇠나 필드 질문 실패)면 RMC2205 cannot verify, WorldOnly 소유자 형질 경로면 WorldOnlyNatives에 더한다.
    // session은 판정 형판을 쓰는 탐침(공개 헤더를 더해 다시 묻는 실행 포함)이고 publicHeaders는 불완전한 포인터 대상을 다시 물을 때만 부른다.
    public async Task VerifyNativeMembersAsync(Dictionary<string, ClassModel> declared, ClangProbe.Session<string> session, Func<IReadOnlyList<string>> publicHeaders)
    {
        List<(ClassModel Model, NativeMemberModel Native)> natives = declared.Values.Where(model => !model.IsEnum).OrderBy(model => model.Name, StringComparer.Ordinal)
            .SelectMany(model => model.NativeMembers.Where(native => !native.Bitfield).Select(native => (model, native))).ToList();
        if (natives.Count == 0)
        {
            return;
        }

        // 열쇠(정규 철자) → 레코드다. 같은 열쇠에 완전한 레코드가 있으면 그것이 이긴다(앞 선언과 정의).
        Dictionary<string, ClosureRecord> byKey = new(StringComparer.Ordinal);
        Dictionary<string, List<ClosureRecord>> unkeyed = new(StringComparer.Ordinal);
        void AddKey(string key, ClosureRecord record)
        {
            if (!byKey.TryGetValue(key, out ClosureRecord? existing) || (!existing.Complete && record.Complete))
            {
                byKey[key] = record;
            }
        }

        foreach (ClosureRecord record in closureRecords)
        {
            if (record.DirectKeyed)
            {
                AddKey(record.DeclPath, record);
                continue;
            }

            if (record.Spelling is null)
            {
                record.KeyAsked = true;
                record.KeyFailure = UnspellableClass;
            }

            if (!unkeyed.TryGetValue(record.Name, out List<ClosureRecord>? list))
            {
                unkeyed.Add(record.Name, list = []);
            }

            list.Add(record);
        }

        Dictionary<ClosureRecord, ClosureFacts> facts = [];
        Dictionary<string, string?> classFacts = new(StringComparer.Ordinal);
        Dictionary<string, NativeMemberProbe.Verdict?> verdicts = new(StringComparer.Ordinal);
        Dictionary<string, string> verdictFailures = new(StringComparer.Ordinal);
        Dictionary<NativeMemberModel, string> overflow = [];
        string prelude = NativeMemberProbe.VerdictTemplates + CanonicalTypeProbe.Templates;
        string Prelude(IReadOnlyList<string> headers) =>
            string.Concat(headers.Select(header => $"#include \"{header.Replace('\\', '/')}\"\n")) + prelude;

        ClosureRecord? Lookup(string text) => byKey.GetValueOrDefault(text);

        // 열쇠로 찾지 못한 형식과 식별자가 같은 레코드 가운데 아직 열쇠를 묻지 않은 것이다.
        List<ClosureRecord> PendingKeys(string text) => unkeyed.GetValueOrDefault(LastIdentifier(text))?.Where(candidate => !candidate.KeyAsked).ToList() ?? [];

        // 열쇠로 찾지 못한 형식과 식별자가 같은데 열쇠가 없는 레코드의 이유다(진단 문구).
        string? Unmatched(string text) => unkeyed.GetValueOrDefault(LastIdentifier(text))?.Where(candidate => candidate.KeyFailure is not null)
            .OrderBy(candidate => candidate.DeclPath, StringComparer.Ordinal).Select(candidate => $"{candidate.DeclPath}: {candidate.KeyFailure}").FirstOrDefault();

        for (int round = 0; ; ++round)
        {
            // 지금 아는 사실로 NATIVE마다 값으로 닿는 형식을 걷고, 모르는 것을 모은다.
            HashSet<ClosureRecord> keyNeeded = [];
            HashSet<ClosureRecord> recordNeeded = [];
            HashSet<(ClosureRecord Record, int Base)> baseRetry = [];
            HashSet<string> classNeeded = new(StringComparer.Ordinal);
            HashSet<string> verdictNeeded = new(StringComparer.Ordinal);
            List<NativeMemberModel> waiting = [];
            foreach ((ClassModel _, NativeMemberModel native) in natives)
            {
                if (overflow.ContainsKey(native))
                {
                    continue;
                }

                HashSet<string> walked = new(StringComparer.Ordinal);
                bool needs = false;
                bool full = false;

                // top은 NATIVE 멤버 자신의 형식(포인터는 NativeMemberProbe가 본다), argument는 템플릿 인자로 닿은 형식이다.
                void Explore(string type, bool top, bool argument)
                {
                    string text = CleanType(type);
                    if (full || text.Length == 0 || !walked.Add(WalkKey(text, top, argument)))
                    {
                        return;
                    }

                    if (walked.Count > MaxClosureTypes)
                    {
                        full = true;
                        return;
                    }

                    if (IsIndirect(text))
                    {
                        if (!top && !verdicts.ContainsKey(text) && !verdictFailures.ContainsKey(text))
                        {
                            verdictNeeded.Add(text);
                            needs = true;
                        }

                        return;
                    }

                    if (StripArray(text) is string element)
                    {
                        Explore(element, top, argument);
                        return;
                    }

                    if (IsBuiltinSpelling(text) || definedEnums.ContainsKey(text))
                    {
                        return;
                    }

                    (string Name, List<string> Arguments)? split = SplitTemplate(text);
                    if (split is (string name, _) && (StrongReferenceTemplates.Contains(CleanType(name)) || NonOwningTemplates.Contains(CleanType(name))))
                    {
                        return;
                    }

                    if (Lookup(text) is not ClosureRecord record)
                    {
                        List<ClosureRecord> pending = PendingKeys(text);
                        if (pending.Count != 0)
                        {
                            keyNeeded.UnionWith(pending);
                            needs = true;
                        }
                        else if (!classFacts.ContainsKey(text))
                        {
                            classNeeded.Add(text);
                            needs = true;
                        }

                        return;
                    }

                    // 불완전한 레코드는 인자 자리면 건너뛰고(값으로 담기지 않았다), 값 자리면 Resolve가 cannot verify로 본다.
                    if (!record.Complete || record.Unfollowable is not null || record.Query is null)
                    {
                        return;
                    }

                    if (split is (_, List<string> arguments))
                    {
                        foreach (string typeArgument in TypeArguments(arguments, record))
                        {
                            Explore(typeArgument, false, true);
                        }
                    }

                    if (!facts.TryGetValue(record, out ClosureFacts? known))
                    {
                        recordNeeded.Add(record);
                        needs = true;
                        return;
                    }

                    foreach (string? field in known.FieldTypes)
                    {
                        if (field is not null)
                        {
                            Explore(field, false, false);
                        }
                    }

                    for (int index = 0; index < known.BaseTypes.Length; ++index)
                    {
                        if (known.BaseTypes[index] is string baseType)
                        {
                            Explore(baseType, false, false);
                        }
                        else if (known.BaseSteps[index] == 1)
                        {
                            baseRetry.Add((record, index));
                            needs = true;
                        }
                    }
                }

                Explore(native.Spelling, true, false);
                if (full)
                {
                    overflow[native] = $"{native.Spelling} reaches more than {MaxClosureTypes} types";
                }
                else if (needs)
                {
                    waiting.Add(native);
                }
            }

            if (waiting.Count == 0)
            {
                break;
            }

            if (round >= MaxClosureRounds)
            {
                foreach (NativeMemberModel native in waiting)
                {
                    overflow[native] = $"{native.Spelling} needs more than {MaxClosureRounds} probe rounds";
                }

                break;
            }

            ProbeBatch batch = new();
            List<(int Index, Action<string?, string?> Apply)> results = [];
            List<(ClosureRecord Record, List<int> Questions)> keyQuestions = [];
            foreach (ClosureRecord record in keyNeeded.OrderBy(record => record.Spelling, StringComparer.Ordinal).ThenBy(record => record.DeclPath, StringComparer.Ordinal))
            {
                record.KeyAsked = true;
                bool[] variants = record.Casts.Any(cast => cast.Owner is not null) ? [false, true] : [false];
                keyQuestions.Add((record, variants.Select(owner => batch.Add(index => KeyQuestion(index, record, owner))).ToList()));
            }

            foreach (ClosureRecord record in recordNeeded.OrderBy(record => record.Query, StringComparer.Ordinal))
            {
                ClosureFacts known = new()
                {
                    FieldTypes = new string?[record.Fields.Count],
                    BaseTypes = new string?[record.Bases.Count],
                    BaseSteps = new int[record.Bases.Count]
                };
                facts.Add(record, known);
                for (int index = 0; index < record.Fields.Count; ++index)
                {
                    int field = index;
                    results.Add((batch.Add(question => CanonicalTypeProbe.TypeLine(question, $"decltype({record.Query}::{record.Fields[field]})")), (answer, failure) =>
                    {
                        known.FieldTypes[field] = answer;
                        if (failure is not null)
                        {
                            known.Failures[record.Fields[field]] = failure;
                        }
                    }
                    ));
                }

                for (int index = 0; index < record.Bases.Count; ++index)
                {
                    int position = index;
                    bool alias = BaseNeedsAlias(record, position);
                    known.BaseSteps[position] = alias ? 1 : 0;
                    results.Add((batch.Add(question => BaseQuestion(question, record, position, alias)),
                        (answer, failure) => ApplyClosureBase(known, record, position, answer, failure, !alias)));
                }

                results.Add((batch.Add(question => CanonicalTypeProbe.FactLine(question, $"(::rocks::IsBaseOfV<::rocks::Object, {record.Query}> ? 1 : 0)")), (answer, failure) =>
                {
                    known.ObjectDerived = answer is null ? null : answer != "0";
                    if (failure is not null)
                    {
                        known.Failures["(Object)"] = failure;
                    }
                }
                ));
                results.Add((batch.Add(question => CanonicalTypeProbe.FactLine(question, $"(::rocks::RequiresWorldOnlyOwner<{record.Query}> ? 1 : 0)")), (answer, failure) =>
                {
                    known.WorldOnly = answer is null ? null : answer != "0";
                    if (failure is not null)
                    {
                        known.Failures["(WorldOnly)"] = failure;
                    }
                }
                ));
            }

            foreach ((ClosureRecord record, int position) in baseRetry.OrderBy(entry => entry.Record.Query, StringComparer.Ordinal).ThenBy(entry => entry.Base))
            {
                ClosureFacts known = facts[record];
                results.Add((batch.Add(question => BaseQuestion(question, record, position, true)),
                    (answer, failure) => ApplyClosureBase(known, record, position, answer, failure, false)));
            }

            foreach (string text in classNeeded.Order(StringComparer.Ordinal))
            {
                results.Add((batch.Add(question => CanonicalTypeProbe.FactLine(question, $"(::rocks::IsClassV<{CanonicalTypeProbe.SpellType(text)}> ? 1 : 0)")),
                    (answer, failure) => classFacts[text] = answer ?? "?" + failure));
            }

            List<(int Index, string Text)> verdictQuestions = [];
            foreach (string text in verdictNeeded.Order(StringComparer.Ordinal))
            {
                int question = batch.Add(index => CanonicalTypeProbe.TypeLine(index, $"::rocks_meta_probe::RocksProbeVerdict<{CanonicalTypeProbe.SpellType(text)}>"));
                verdictQuestions.Add((question, text));
                results.Add((question, (answer, failure) =>
                {
                    if (answer is not null && NativeMemberProbe.ParseVerdict(answer) is NativeMemberProbe.Verdict verdict)
                    {
                        verdicts[text] = verdict;
                    }
                    else
                    {
                        verdictFailures[text] = failure ?? "Clang reported no verdict for " + text;
                    }
                }
                ));
            }

            ClangProbe.Outcome<string> outcome = await AskAsync(session, prelude, batch, natives[0].Native.Site);
            foreach ((ClosureRecord record, List<int> questions) in keyQuestions)
            {
                if (questions.Select(index => outcome.Answers.GetValueOrDefault(index)).FirstOrDefault(answer => answer is not null) is string answer)
                {
                    record.Query ??= "::" + CanonicalTypeProbe.SpellType(answer);
                    AddKey(answer, record);
                }
                else
                {
                    record.KeyFailure = questions.Select(index => outcome.Failures.GetValueOrDefault(index)).FirstOrDefault(failure => failure is not null) ??
                        "Clang did not name the class";
                }
            }

            foreach ((int index, Action<string?, string?> apply) in results)
            {
                apply(outcome.Answers.GetValueOrDefault(index), outcome.Failures.GetValueOrDefault(index));
            }

            // 불완전한 대상(판정 2)은 직접 멤버처럼 모듈 public 헤더를 더해 다시 묻는다. 그래도 불완전하면 허용한다(빌린 back-pointer).
            List<(int Index, string Text)> incomplete = verdictQuestions.Where(entry => verdicts.TryGetValue(entry.Text, out NativeMemberProbe.Verdict? verdict) &&
                verdict?.Code == 2).ToList();
            if (incomplete.Count != 0)
            {
                ProbeBatch wide = new();
                Dictionary<int, string> wideTexts = [];
                foreach ((int _, string text) in incomplete)
                {
                    wideTexts.Add(wide.Add(index => CanonicalTypeProbe.TypeLine(index, $"::rocks_meta_probe::RocksProbeVerdict<{CanonicalTypeProbe.SpellType(text)}>")), text);
                }

                if (await ClangProbe.WidenAsync(session, Prelude, "", wide.Questions, publicHeaders()) is ClangProbe.Outcome<string> widened)
                {
                    foreach ((int index, string answer) in widened.Answers)
                    {
                        if (NativeMemberProbe.ParseVerdict(answer) is NativeMemberProbe.Verdict verdict)
                        {
                            verdicts[wideTexts[index]] = verdict;
                        }
                    }
                }
            }
        }

        foreach ((ClassModel model, NativeMemberModel native) in natives)
        {
            string symbol = model.Name + "::" + native.Name;
            string? cannot = overflow.GetValueOrDefault(native);
            string? strong = Strong(native.Spelling, [], true, false, ref cannot);
            if (strong is not null)
            {
                throw new MetadataException($"{symbol}: ROCKS_NATIVE member holds a strong object reference ({strong})", "RMC2205",
                    "Native members are not traced; store ObjectRef/RootRef/Object pointers in ROCKS_PROPERTY fields or use WeakObjectRef/ObjectHandle.").At(native.Site);
            }

            string? worldOnly = WorldOnly(native.Spelling, [], false, ref cannot);
            if (cannot is not null)
            {
                throw new MetadataException($"{symbol}: cannot verify that ROCKS_NATIVE member holds no strong object reference ({cannot})", "RMC2205",
                    "The metadata compiler follows types held by value through Clang's canonical types; hold only classes that are defined where the reflected header sees them and can be named from the global scope (no local or unnamed classes, no specializations with template template or class-type arguments).")
                    .At(native.Site);
            }

            if (worldOnly is not null)
            {
                model.WorldOnlyNatives.Add((symbol, worldOnly, native.Site));
            }
        }

        // 강한 참조 경로다(없으면 null). 값으로 담는 필드·base·배열 원소·템플릿 인자를 따라가고, 포인터·참조는 판정(Object 파생 대상 —
        // 1, 강한 참조 래퍼 — 3)으로 본다. Object 파생 타입은 값으로 닿아도 펼치지 않는다. 직접 멤버의 포인터는 NativeMemberProbe가 본다.
        string? Strong(string type, HashSet<string> visited, bool top, bool argument, ref string? cannot)
        {
            string text = CleanType(type);
            if (text.Length == 0 || !visited.Add(WalkKey(text, top, argument)))
            {
                return null;
            }

            if (visited.Count > MaxClosureTypes)
            {
                cannot ??= $"{text}: the walk reaches more than {MaxClosureTypes} types";
                return null;
            }

            if (IsIndirect(text))
            {
                if (top)
                {
                    return null;
                }

                if (verdicts.TryGetValue(text, out NativeMemberProbe.Verdict? verdict) && verdict is NativeMemberProbe.Verdict known)
                {
                    return known.Code switch
                    {
                        1 => "raw pointer to Object-derived " + known.Target,
                        3 => $"{text} -> {known.Target}",
                        _ => null
                    };
                }

                cannot ??= $"{text}: {verdictFailures.GetValueOrDefault(text, "no verdict")}";
                return null;
            }

            if (StripArray(text) is string element)
            {
                return Strong(element, visited, top, argument, ref cannot);
            }

            if (IsBuiltinSpelling(text) || definedEnums.ContainsKey(text))
            {
                return null;
            }

            (string Name, List<string> Arguments)? split = SplitTemplate(text);
            if (split is (string name, _))
            {
                string template = CleanType(name);
                if (StrongReferenceTemplates.Contains(template))
                {
                    return template;
                }

                if (NonOwningTemplates.Contains(template))
                {
                    return null;
                }
            }

            if (Resolve(text, argument, ref cannot) is not { } resolved)
            {
                return null;
            }

            (ClosureRecord record, ClosureFacts facts2) = resolved;
            if (split is (_, List<string> arguments))
            {
                foreach (string typeArgument in TypeArguments(arguments, record))
                {
                    if (Strong(typeArgument, visited, false, true, ref cannot) is string inner)
                    {
                        return $"{text} -> {inner}";
                    }
                }
            }

            if (facts2.ObjectDerived == true)
            {
                return null;
            }

            if (facts2.ObjectDerived is null)
            {
                cannot ??= $"{text}: {facts2.Failures.GetValueOrDefault("(Object)", "no Object fact")}";
            }

            for (int index = 0; index < record.Fields.Count; ++index)
            {
                if (facts2.FieldTypes[index] is not string field)
                {
                    cannot ??= $"{text}::{record.Fields[index]}: {facts2.Failures.GetValueOrDefault(record.Fields[index], "no type")}";
                    continue;
                }

                if (Strong(field, visited, false, false, ref cannot) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            for (int index = 0; index < record.Bases.Count; ++index)
            {
                if (facts2.BaseTypes[index] is not string baseType)
                {
                    cannot ??= $"{text}: base '{record.Bases[index].Written}': {facts2.Failures.GetValueOrDefault("(base)" + index, "not named")}";
                    continue;
                }

                if (Strong(baseType, visited, false, false, ref cannot) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            return null;
        }

        // WorldOnly 소유자 형질 경로다(없으면 null). 포인터·참조는 따라가지 않는다. 형질은 생성 static_assert와 같은 RequiresWorldOnlyOwner<R>
        // 사실이다.
        string? WorldOnly(string type, HashSet<string> visited, bool argument, ref string? cannot)
        {
            string text = CleanType(type);
            if (text.Length == 0 || IsIndirect(text) || !visited.Add(WalkKey(text, false, argument)))
            {
                return null;
            }

            if (visited.Count > MaxClosureTypes)
            {
                cannot ??= $"{text}: the walk reaches more than {MaxClosureTypes} types";
                return null;
            }

            if (StripArray(text) is string element)
            {
                return WorldOnly(element, visited, argument, ref cannot);
            }

            if (IsBuiltinSpelling(text) || definedEnums.ContainsKey(text))
            {
                return null;
            }

            (string Name, List<string> Arguments)? split = SplitTemplate(text);
            if (split is (string name, _) && (NonOwningTemplates.Contains(CleanType(name)) || StrongReferenceTemplates.Contains(CleanType(name))))
            {
                return null;
            }

            if (Resolve(text, argument, ref cannot) is not { } resolved)
            {
                return null;
            }

            (ClosureRecord record, ClosureFacts known) = resolved;
            if (known.WorldOnly == true)
            {
                return text;
            }

            if (known.WorldOnly is null)
            {
                cannot ??= $"{text}: {known.Failures.GetValueOrDefault("(WorldOnly)", "no WorldOnly owner fact")}";
            }

            IEnumerable<(string? Type, bool Argument)> members = known.FieldTypes.Concat(known.BaseTypes).Select(member => (member, false));
            if (split is (_, List<string> arguments))
            {
                members = TypeArguments(arguments, record).Select(argument => ((string?)argument, true)).Concat(members);
            }

            foreach ((string? member, bool memberArgument) in members)
            {
                if (member is not null && WorldOnly(member, visited, memberArgument, ref cannot) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            return null;
        }

        // 값으로(또는 템플릿 인자로) 닿은 형식의 레코드와 사실이다. builtin·enum·class 아닌 형식, 인자 자리의 불완전한 레코드는 null이고,
        // 따라갈 수 없는 class면 cannot을 채운다.
        (ClosureRecord, ClosureFacts)? Resolve(string text, bool argument, ref string? cannot)
        {
            if (Lookup(text) is ClosureRecord record)
            {
                if (!record.Complete)
                {
                    if (!argument)
                    {
                        cannot ??= $"{text} has no complete definition in the reflected headers";
                    }

                    return null;
                }

                if (record.Unfollowable is string reason)
                {
                    cannot ??= $"{text}: {reason}";
                    return null;
                }

                if (facts.TryGetValue(record, out ClosureFacts? known))
                {
                    return (record, known);
                }

                cannot ??= $"{text} was not probed";
                return null;
            }

            string? fact = classFacts.GetValueOrDefault(text);
            if (fact == "0")
            {
                return null;
            }

            cannot ??= fact is not null && fact != "1" ? $"{text}: {fact[1..]}"
                : Unmatched(text) is string unmatched ? $"{text} could not be matched to its definition ({unmatched})"
                : $"{text} is a class without a definition the metadata compiler can name from the global scope";
            return null;
        }
    }

    private static void ApplyClosureBase(ClosureFacts known, ClosureRecord record, int position, string? answer, string? failure, bool checkIdentifier)
    {
        if (answer is not null && (!checkIdentifier || BaseAnswerMatches(record, position, answer)))
        {
            known.BaseTypes[position] = answer;
            return;
        }

        known.BaseSteps[position] = known.BaseSteps[position] == 0 ? 1 : 2;
        known.Failures["(base)" + position] = failure ??
            (answer is not null ? $"Clang named '{answer}' for a base named {LastIdentifier(record.Bases[position].Resolved)}" : "Clang did not name the base");
    }

    // 걷기 중복 제거 열쇠다(4.4절 — 형식, 직접 멤버인지, 템플릿 인자인지).
    private static string WalkKey(string text, bool top, bool argument) => (top ? "T" : "-") + (argument ? "A" : "-") + text;

    // 템플릿 철자의 인자 가운데 형식 인자다(색인한 특수화 노드의 인자 종류로 가른다).
    private static IEnumerable<string> TypeArguments(List<string> arguments, ClosureRecord specialization)
    {
        for (int index = 0; index < arguments.Count && index < specialization.ArgumentTypes.Count; ++index)
        {
            if (specialization.ArgumentTypes[index])
            {
                yield return arguments[index];
            }
        }
    }

    // 포인터·참조·멤버 포인터·함수 포인터처럼 판정 형판으로 보는 형식이다(템플릿 인자·리터럴 밖의 `*`·`&`).
    private static bool IsIndirect(string text) => TopLevelContains(text, "*") || TopLevelContains(text, "&");

    // C 배열이면 원소(한 차원)를, 아니면 null을 돌려준다.
    private static string? StripArray(string text)
    {
        if (!text.EndsWith(']'))
        {
            return null;
        }

        int bracket = text.LastIndexOf('[');
        return bracket > 0 ? text[..bracket].TrimEnd() : null;
    }

    private static bool IsBuiltinSpelling(string text) =>
        BuiltinSpellings.Contains(text) || text.StartsWith("__attribute__", StringComparison.Ordinal) || text.StartsWith("_Atomic(", StringComparison.Ordinal) ||
        text.StartsWith("_Complex ", StringComparison.Ordinal);
}
