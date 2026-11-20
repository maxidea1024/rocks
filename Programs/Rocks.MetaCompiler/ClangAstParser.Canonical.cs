using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Rocks.MetaCompiler;

// ClangAstParser 중 정규 형식 탐침을 쓰는 부분이다(Docs/Design/ClangVersionSupport.ko.md 4절). 1단계(CanonicalizeAsync)는 이 모듈
// 리플렉션 타입의 PROPERTY·NATIVE 형식, 정적 배열 원소·크기, base를 Clang 정규 철자로 바꾼다. 레코드 닫힘(VerifyNativeMembersAsync)은
// NATIVE 멤버가 값으로 닿는 레코드의 필드·base를 라운드마다 물어 강한 참조(RMC2205)·WorldOnly 소유자 형질 경로를 찾는다. 형식에 관한
// 판단은 탐침이 돌려준 정규 철자로만 하고, 첫 실행 JSON은 선언 구조(이름·필드 이름·base 목록·표지 변수)만 준다.
internal sealed partial class ClangAstParser
{
    // 레코드 닫힘이 한 NATIVE 멤버에서 걷는 형식 수와 라운드 수의 상한이다(넘으면 cannot verify — 결정적으로 멈춘다).
    private const int MaxClosureTypes = 4096;

    private const int MaxClosureRounds = 32;

    // 둘러싼 이름공간 한 칸이다(4.3절 base 질문 2의 별칭을 그 이름공간에 다시 열어 둔다).
    private sealed record NamespacePart(string Name, bool Anonymous);

    // 첫 실행 JSON의 이름 있는 완전한 레코드 하나다(비템플릿 레코드와 특수화 노드 — 패턴 레코드는 넣지 않는다).
    private sealed class ClosureRecord
    {
        // 식별자다(특수화는 템플릿 이름).
        public required string Name { get; init; }

        // 선언 경로다(익명 이름공간은 "(anonymous namespace)").
        public required string DeclPath { get; init; }

        // 전역 한정 C++ 철자다(특수화는 `::ns::Name<인자…>`). 부를 수 없으면 null이다.
        public required string? Spelling { get; init; }

        // 선언 경로가 정규 철자와 같다(경로에 inline 이름공간·특수화가 없다). 아니면 정규 철자를 형식 철자 질문으로 얻는다.
        public required bool DirectKeyed { get; init; }

        public required IReadOnlyList<NamespacePart> Namespaces { get; init; }

        public required bool Specialization { get; init; }

        public List<string> Fields { get; } = [];

        // 특수화의 템플릿 인자(pack은 펼친 순서)마다 형식 인자인지다. 정규 철자의 인자와 같은 순서다(정규 출력은 끝의 기본 인자만 생략한다).
        public List<bool> ArgumentTypes { get; } = [];

        // base의 TypeSpelling(desugar 철자 우선)과 쓴 철자다.
        public List<(string Resolved, string Written)> Bases { get; } = [];

        public bool WorldOnly { get; set; }

        public bool KeyAsked { get; set; }
    }

    // 레코드 닫힘이 물어 얻은 레코드 하나의 사실이다. null 칸은 그 질문이 실패한 것이다(Failures에 Clang error 줄).
    private sealed class ClosureFacts
    {
        public string?[] FieldTypes { get; init; } = [];

        public string?[] BaseTypes { get; init; } = [];

        // base 질문 단계(0: 1로 묻는 중, 1: 1이 실패해 2로 묻는 중, 2: 둘 다 실패).
        public int[] BaseSteps { get; init; } = [];

        public bool? ObjectDerived { get; set; }

        public Dictionary<string, string> Failures { get; } = new(StringComparer.Ordinal);
    }

    private readonly record struct ClosureScope(IReadOnlyList<NamespacePart> Namespaces, string DeclPath, string? Spelling, bool Exact);

    private readonly List<ClosureRecord> closureRecords = [];

    private readonly Dictionary<string, ClosureRecord> closureByDecl = new(StringComparer.Ordinal);

    private readonly HashSet<string> closureIds = new(StringComparer.Ordinal);

    // 클래스 템플릿(선언 경로) → 패턴 레코드의 WorldOnly 표지와 템플릿 매개변수 종류("bool"·"pack"·"other")다.
    private readonly Dictionary<string, bool> patternWorldOnly = new(StringComparer.Ordinal);

    private readonly Dictionary<string, List<string>> templateParameters = new(StringComparer.Ordinal);

    // 함수 안에 정의된 클래스의 식별자다(색인하지 않는다 — 전역 한정 철자로 부를 수 없다). 템플릿 인자로 닿은 형식이 색인에 없을 때 지역
    // 클래스인지 가른다.
    private readonly HashSet<string> localClassNames = new(StringComparer.Ordinal);

    // 정규 형식 탐침의 형식 철자가 builtin이면 걷기를 끝낸다(class 여부를 묻지 않는다).
    private static readonly HashSet<string> BuiltinSpellings = new(StringComparer.Ordinal)
    {
        "void", "bool", "char", "signed char", "unsigned char", "wchar_t", "char8_t", "char16_t", "char32_t", "short", "unsigned short", "int",
        "unsigned int", "long", "unsigned long", "long long", "unsigned long long", "__int128", "unsigned __int128", "float", "double", "long double",
        "std::nullptr_t", "decltype(nullptr)", "_Float16", "__bf16"
    };

    private void IndexClosure(JsonElement root) => IndexClosure(root, new([], "", "", true));

    private void IndexClosure(JsonElement node, ClosureScope scope)
    {
        string kind = Text(node, "kind");
        string name = Text(node, "name");
        switch (kind)
        {
            case "TranslationUnitDecl" or "LinkageSpecDecl" or "ExportDecl":
                foreach (JsonElement child in Children(node))
                {
                    IndexClosure(child, scope);
                }

                return;
            case "NamespaceDecl":
                {
                    bool anonymous = name.Length == 0;
                    ClosureScope inner = new([.. scope.Namespaces, new(name, anonymous)], Join(scope.DeclPath, anonymous ? "(anonymous namespace)" : name),
                        scope.Spelling is null ? null : anonymous ? scope.Spelling : scope.Spelling + "::" + name, scope.Exact && !Flag(node, "isInline"));
                    foreach (JsonElement child in Children(node))
                    {
                        IndexClosure(child, inner);
                    }

                    return;
                }

            case "ClassTemplateDecl":
                {
                    string path = Join(scope.DeclPath, name);
                    List<string> parameters = [];
                    foreach (JsonElement child in Children(node))
                    {
                        string childKind = Text(child, "kind");
                        if (childKind is "TemplateTypeParmDecl" or "NonTypeTemplateParmDecl" or "TemplateTemplateParmDecl")
                        {
                            parameters.Add(Flag(child, "isParameterPack") ? "pack"
                                : childKind == "NonTypeTemplateParmDecl" && child.TryGetProperty("type", out JsonElement type) && Text(type, "qualType") == "bool" ? "bool" : "other");
                        }
                        else if (childKind == "CXXRecordDecl" && Flag(child, "completeDefinition"))
                        {
                            patternWorldOnly[path] = HasWorldOnlyMarker(child);
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
                if (Flag(node, "completeDefinition") && name.Length != 0)
                {
                    IndexClosureRecord(node, scope, name, name, false, null);
                }

                return;
            case "ClassTemplatePartialSpecializationDecl":
                // 부분 특수화는 패턴이다(그 인스턴스화가 특수화 노드로 색인된다).
                return;
            default:
                // 함수 본문의 지역 클래스는 전역 한정 철자로 부를 수 없으므로 색인하지 않고 이름만 남긴다.
                CollectLocalClasses(node);
                return;
        }
    }

    private void IndexSpecialization(JsonElement node, ClosureScope scope, string name, string templatePath)
    {
        if (!Flag(node, "completeDefinition"))
        {
            return;
        }

        templateParameters.TryGetValue(templatePath, out List<string>? parameters);
        List<string> arguments = [];
        List<bool> argumentTypes = [];
        bool spellable = true;
        int position = 0;
        void Add(JsonElement argument, bool inPack)
        {
            string? kind = parameters is not null && position < parameters.Count ? parameters[position] : null;
            if (argument.TryGetProperty("type", out JsonElement type))
            {
                string? spelled = SpellArgument(Text(type, "qualType"));
                spellable &= spelled is not null;
                arguments.Add(spelled ?? "");
                argumentTypes.Add(true);
            }
            else if (argument.TryGetProperty("value", out JsonElement value) && value.ValueKind == JsonValueKind.Number)
            {
                string raw = value.GetRawText();
                arguments.Add(kind == "bool" && !inPack ? (raw == "0" ? "false" : "true") : raw);
                argumentTypes.Add(false);
            }
            else if (Children(argument).Any(child => Text(child, "kind") == "TemplateArgument") || kind == "pack")
            {
                foreach (JsonElement element in Children(argument).Where(child => Text(child, "kind") == "TemplateArgument"))
                {
                    Add(element, true);
                }
            }
            else
            {
                spellable = false;
            }

            if (!inPack)
            {
                ++position;
            }
        }

        foreach (JsonElement argument in Children(node).Where(child => Text(child, "kind") == "TemplateArgument"))
        {
            Add(argument, false);
        }

        IndexClosureRecord(node, scope, name, spellable ? $"{name}<{string.Join(", ", arguments)}>" : null, true, templatePath)?.ArgumentTypes.AddRange(argumentTypes);
    }

    // 특수화 노드의 형식 인자(정규 철자)를 전역 범위에서 부를 철자로 바꾼다. 익명 이름공간은 한 칸을 빼고, 람다·이름 없는 타입은 부를 수 없다.
    private static string? SpellArgument(string type)
    {
        string text = type.Replace("(anonymous namespace)::", "", StringComparison.Ordinal);
        return text.Length == 0 || text.Contains("(lambda", StringComparison.Ordinal) || text.Contains("(unnamed", StringComparison.Ordinal) ||
            text.Contains("(anonymous", StringComparison.Ordinal) || text.Contains("<dependent type>", StringComparison.Ordinal) ? null : text;
    }

    private ClosureRecord? IndexClosureRecord(JsonElement node, ClosureScope scope, string name, string? segment, bool specialization, string? templatePath)
    {
        string id = Text(node, "id");
        if (id.Length != 0 && !closureIds.Add(id))
        {
            return null;
        }

        string declPath = Join(scope.DeclPath, name);
        string? spelling = scope.Spelling is null || segment is null ? null : scope.Spelling + "::" + segment;
        ClosureRecord record = new()
        {
            Name = name,
            DeclPath = declPath,
            Spelling = spelling,
            DirectKeyed = scope.Exact && !specialization,
            Namespaces = scope.Namespaces,
            Specialization = specialization,
            WorldOnly = HasWorldOnlyMarker(node) || (templatePath is not null && patternWorldOnly.GetValueOrDefault(templatePath))
        };

        // 이름 없는 struct/union 멤버의 필드는 부모 범위로 주입되므로 부모 필드로 묻는다.
        void Collect(JsonElement owner)
        {
            foreach (JsonElement child in Children(owner))
            {
                string childKind = Text(child, "kind");
                if (childKind == "FieldDecl" && Text(child, "name").Length != 0)
                {
                    record.Fields.Add(Text(child, "name"));
                }
                else if (childKind == "CXXRecordDecl" && Text(child, "name").Length == 0 && Flag(child, "completeDefinition"))
                {
                    Collect(child);
                }
            }
        }

        Collect(node);
        if (node.TryGetProperty("bases", out JsonElement bases))
        {
            foreach (JsonElement entry in bases.EnumerateArray())
            {
                record.Bases.Add((TypeSpelling(entry), entry.TryGetProperty("type", out JsonElement type) ? Text(type, "qualType") : ""));
            }
        }

        closureRecords.Add(record);
        if (!specialization)
        {
            closureByDecl.TryAdd(declPath, record);
        }

        ClosureScope inner = new(scope.Namespaces, declPath, spelling, scope.Exact && !specialization);
        foreach (JsonElement child in Children(node))
        {
            string childKind = Text(child, "kind");
            if (childKind is "CXXRecordDecl" or "ClassTemplateDecl" or "ClassTemplateSpecializationDecl")
            {
                IndexClosure(child, inner);
            }
        }

        return record;
    }

    private void CollectLocalClasses(JsonElement node)
    {
        foreach (JsonElement child in Children(node))
        {
            if (Text(child, "kind") == "CXXRecordDecl" && Text(child, "name").Length != 0)
            {
                localClassNames.Add(Text(child, "name"));
            }

            CollectLocalClasses(child);
        }
    }

    private static bool HasWorldOnlyMarker(JsonElement record) =>
        Children(record).Any(child => Text(child, "kind") == "VarDecl" && Text(child, "name") == "RocksRequiresWorldOnlyOwner" && ContainsTrue(child));

    private static string Join(string scope, string name) => scope.Length == 0 ? name : scope + "::" + name;

    // base 질문에 쓸 식별자다: TypeSpelling(desugar 철자 우선)에서 템플릿 인자와 한정을 걷은 마지막 식별자(4.3절).
    private static string BaseIdentifier(string resolved)
    {
        string text = CleanType(resolved);
        int open = text.IndexOf('<', StringComparison.Ordinal);
        string name = open < 0 ? text : text[..open].TrimEnd();
        int colon = name.LastIndexOf("::", StringComparison.Ordinal);
        return colon < 0 ? name : name[(colon + 2)..];
    }

    // base 질문 줄이다. alias가 거짓이면 정교한 형식 지정자 `struct ::Owner::식별자`(1), 참이면 base 철자를 소유자를 둘러싼 이름공간의
    // 별칭으로 부른다(2 — 특수화의 base는 치환된 정규 철자, 그 밖에는 쓴 철자).
    private static string BaseQuestion(int index, ClosureRecord owner, int baseIndex, bool alias)
    {
        (string resolved, string written) = owner.Bases[baseIndex];
        if (!alias)
        {
            return CanonicalTypeProbe.BaseLine(index, owner.Spelling!, $"struct {owner.Spelling}::{BaseIdentifier(resolved)}");
        }

        string aliasName = string.Create(CultureInfo.InvariantCulture, $"rocksCanonAlias_{index}");
        StringBuilder prelude = new();
        foreach (NamespacePart part in owner.Namespaces)
        {
            prelude.Append(part.Anonymous ? "namespace { " : $"namespace {part.Name} {{ ");
        }

        prelude.Append($"using {aliasName} = {(owner.Specialization ? resolved : written)}; ");
        prelude.Append(string.Concat(Enumerable.Repeat("} ", owner.Namespaces.Count)));
        string reference = string.Concat(owner.Namespaces.Where(part => !part.Anonymous).Select(part => "::" + part.Name)) + "::" + aliasName;
        return CanonicalTypeProbe.BaseLine(index, owner.Spelling!, reference, prelude.ToString());
    }

    // 파생 이름(템플릿이면 템플릿 이름)이 base 식별자와 같거나 같은 식별자 base가 여럿이면 1이 파생 자신이나 모호한 이름을 찾으므로
    // 처음부터 2로 묻는다.
    private static bool BaseNeedsAlias(ClosureRecord owner, int baseIndex)
    {
        string identifier = BaseIdentifier(owner.Bases[baseIndex].Resolved);
        return identifier == owner.Name || owner.Bases.Count(entry => BaseIdentifier(entry.Resolved) == identifier) > 1;
    }

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
                    "The probe appended after the reflected headers failed with no question in it; check for a macro or declaration in the reflected headers that collides with the probe (rocks_meta_canon, RocksCanon*, rocksCanonAlias_*) and use the supported Clang toolchain.")
                    .At(site);
            }
        }

        await session.JudgeAsync(prelude, batch.Questions, "", outcome, first);
        return outcome;
    }

    // 1단계: 이 모듈 리플렉션 타입의 PROPERTY 필드(bitfield 제외)·NATIVE 멤버 형식, C 배열 원소·크기, 클래스·struct base를 정규 철자로
    // 바꾼다. 질문이 실패하면 그 선언 위치의 RMC2001이다(base는 1이 실패하면 2로 한 번 더 묻는다).
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

            if (closureByDecl.TryGetValue(model.Name, out ClosureRecord? record) && record.Spelling is not null)
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

        // 1로 실패한 base는 2(이름공간 별칭)로 한 번 더 묻는다. 그래도 실패하면 RMC2001이다.
        ProbeBatch retry = new();
        Dictionary<int, (ClassModel Model, ClosureRecord Owner, int Base, string First)> retried = [];
        foreach ((int index, (ClassModel model, ClosureRecord owner, int position)) in baseQuestions)
        {
            if (outcome.Answers.TryGetValue(index, out string? answer))
            {
                ApplyBase(model, position, answer);
            }
            else if (!BaseNeedsAlias(owner, position))
            {
                retried.Add(retry.Add(question => BaseQuestion(question, owner, position, true)), (model, owner, position, outcome.Failures[index]));
            }
            else
            {
                throw BaseFailure(model, owner, position, outcome.Failures[index]);
            }
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

    // 레코드 닫힘(4.4절): NATIVE 멤버가 값으로 닿는 레코드의 필드·base를 라운드마다 묻고, 강한 참조 경로면 RMC2205, 따라갈 수 없는
    // class(색인에 없음·부를 수 없음·질문 실패)면 RMC2205 cannot verify, WorldOnly 소유자 형질 경로면 WorldOnlyNatives에 더한다.
    // session은 판정 형판을 쓰는 탐침(공개 헤더를 더해 다시 묻는 실행 포함)이고 publicHeaders는 불완전한 포인터 대상을 다시 물을 때만 부른다.
    public async Task VerifyNativeMembersAsync(Dictionary<string, ClassModel> declared, ClangProbe.Session<string> session, Func<IReadOnlyList<string>> publicHeaders)
    {
        List<(ClassModel Model, NativeMemberModel Native)> natives = declared.Values.Where(model => !model.IsEnum).OrderBy(model => model.Name, StringComparer.Ordinal)
            .SelectMany(model => model.NativeMembers.Where(native => !native.Bitfield).Select(native => (model, native))).ToList();
        if (natives.Count == 0)
        {
            return;
        }

        Dictionary<string, ClosureRecord> byKey = new(StringComparer.Ordinal);
        Dictionary<string, List<ClosureRecord>> unkeyed = new(StringComparer.Ordinal);
        foreach (ClosureRecord record in closureRecords)
        {
            if (record.DirectKeyed)
            {
                byKey.TryAdd(record.DeclPath, record);
            }
            else if (record.Spelling is not null)
            {
                if (!unkeyed.TryGetValue(record.Name, out List<ClosureRecord>? list))
                {
                    unkeyed.Add(record.Name, list = []);
                }

                list.Add(record);
            }
        }

        Dictionary<ClosureRecord, ClosureFacts> facts = [];
        Dictionary<string, string?> classFacts = new(StringComparer.Ordinal);
        Dictionary<string, NativeMemberProbe.Verdict?> verdicts = new(StringComparer.Ordinal);
        Dictionary<string, string> verdictFailures = new(StringComparer.Ordinal);
        string prelude = NativeMemberProbe.VerdictTemplates + CanonicalTypeProbe.Templates;
        string Prelude(IReadOnlyList<string> headers) =>
            string.Concat(headers.Select(header => $"#include \"{header.Replace('\\', '/')}\"\n")) + prelude;

        ClosureRecord? Lookup(string text) => byKey.GetValueOrDefault(text);

        bool overflow = false;
        for (int round = 0; ; ++round)
        {
            if (round >= MaxClosureRounds)
            {
                overflow = true;
                break;
            }

            // 지금 아는 사실로 값으로 닿는 형식을 걷고, 모르는 것을 모은다.
            HashSet<ClosureRecord> keyNeeded = [];
            HashSet<ClosureRecord> recordNeeded = [];
            List<(ClosureRecord Record, int Base)> baseRetry = [];
            HashSet<string> classNeeded = new(StringComparer.Ordinal);
            HashSet<string> verdictNeeded = new(StringComparer.Ordinal);
            HashSet<string> walked = new(StringComparer.Ordinal);
            // argument는 템플릿 인자로 닿은 형식이다. 값으로 담긴 인자는 첫 실행에서 완전해야 하므로 색인에 있다 — 색인에 없는 인자(인스턴스화되지
            // 않은 태그 특수화, 선언만 있는 태그 클래스, enum 상수)는 값으로 담기지 않은 것이라 걷지 않는다(지역 클래스는 Resolve가 거부한다).
            void Explore(string type, bool top, bool argument = false)
            {
                string text = CleanType(type);
                if (text.Length == 0 || !walked.Add(text))
                {
                    return;
                }

                if (walked.Count > MaxClosureTypes)
                {
                    overflow = true;
                    return;
                }

                if (IsIndirect(text))
                {
                    if (!top && !verdicts.ContainsKey(text) && !verdictFailures.ContainsKey(text))
                    {
                        verdictNeeded.Add(text);
                    }

                    return;
                }

                if (StripArray(text) is string element)
                {
                    Explore(element, top, argument);
                    return;
                }

                if (SplitTemplate(text) is (string name, List<string> arguments))
                {
                    string template = CleanType(name);
                    if (StrongReferenceTemplates.Contains(template) || NonOwningTemplates.Contains(template))
                    {
                        return;
                    }

                    // 특수화의 열쇠를 먼저 얻는다(인자 가운데 형식 인자를 그 특수화 노드로 가른다).
                    ClosureRecord? specialization = Lookup(text);
                    if (specialization is null && unkeyed.GetValueOrDefault(SimpleName(text))?.Where(candidate => !candidate.KeyAsked).ToList() is { Count: > 0 } pending)
                    {
                        keyNeeded.UnionWith(pending);
                        return;
                    }

                    foreach (string typeArgument in TypeArguments(arguments, specialization))
                    {
                        Explore(typeArgument, false, true);
                    }
                }

                if (IsBuiltinSpelling(text) || definedEnums.ContainsKey(text))
                {
                    return;
                }

                if (Lookup(text) is not ClosureRecord record)
                {
                    List<ClosureRecord> candidates = unkeyed.GetValueOrDefault(SimpleName(text))?.Where(candidate => !candidate.KeyAsked).ToList() ?? [];
                    if (candidates.Count != 0)
                    {
                        keyNeeded.UnionWith(candidates);
                    }
                    else if (!argument && !classFacts.ContainsKey(text))
                    {
                        classNeeded.Add(text);
                    }

                    return;
                }

                if (!facts.TryGetValue(record, out ClosureFacts? known))
                {
                    recordNeeded.Add(record);
                    return;
                }

                foreach (string? field in known.FieldTypes)
                {
                    if (field is not null)
                    {
                        Explore(field, false);
                    }
                }

                for (int index = 0; index < known.BaseTypes.Length; ++index)
                {
                    if (known.BaseTypes[index] is string baseType)
                    {
                        Explore(baseType, false);
                    }
                    else if (known.BaseSteps[index] == 1)
                    {
                        baseRetry.Add((record, index));
                    }
                }
            }

            foreach ((ClassModel _, NativeMemberModel native) in natives)
            {
                Explore(native.Spelling, true);
            }

            if (overflow || (keyNeeded.Count == 0 && recordNeeded.Count == 0 && baseRetry.Count == 0 && classNeeded.Count == 0 && verdictNeeded.Count == 0))
            {
                break;
            }

            ProbeBatch batch = new();
            List<(int Index, Action<string?, string?> Apply)> results = [];
            foreach (ClosureRecord record in keyNeeded.OrderBy(record => record.Spelling, StringComparer.Ordinal))
            {
                record.KeyAsked = true;
                results.Add((batch.Add(index => CanonicalTypeProbe.TypeLine(index, record.Spelling!)), (answer, _) =>
                {
                    if (answer is not null)
                    {
                        byKey.TryAdd(answer, record);
                    }
                }
                ));
            }

            foreach (ClosureRecord record in recordNeeded.OrderBy(record => record.Spelling ?? record.DeclPath, StringComparer.Ordinal))
            {
                ClosureFacts known = new()
                {
                    FieldTypes = new string?[record.Fields.Count],
                    BaseTypes = new string?[record.Bases.Count],
                    BaseSteps = new int[record.Bases.Count]
                };
                facts.Add(record, known);
                if (record.Spelling is null)
                {
                    known.Failures["(name)"] = "the class cannot be named from the global scope";
                    continue;
                }

                for (int index = 0; index < record.Fields.Count; ++index)
                {
                    int field = index;
                    results.Add((batch.Add(question => CanonicalTypeProbe.TypeLine(question, $"decltype({record.Spelling}::{record.Fields[field]})")), (answer, failure) =>
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
                    results.Add((batch.Add(question => BaseQuestion(question, record, position, alias)), (answer, failure) => ApplyClosureBase(known, position, answer, failure)));
                }

                results.Add((batch.Add(question => CanonicalTypeProbe.FactLine(question, $"(::rocks::IsBaseOfV<::rocks::Object, {record.Spelling}> ? 1 : 0)")), (answer, failure) =>
                {
                    known.ObjectDerived = answer is null ? null : answer != "0";
                    if (failure is not null)
                    {
                        known.Failures["(Object)"] = failure;
                    }
                }
                ));
            }

            foreach ((ClosureRecord record, int position) in baseRetry)
            {
                ClosureFacts known = facts[record];
                results.Add((batch.Add(question => BaseQuestion(question, record, position, true)), (answer, failure) => ApplyClosureBase(known, position, answer, failure)));
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
            string? cannot = overflow ? $"{native.Spelling} reaches more than {MaxClosureTypes} types or {MaxClosureRounds} probe rounds" : null;
            string? strong = Strong(native.Spelling, [], true, ref cannot);
            if (strong is not null)
            {
                throw new MetadataException($"{symbol}: ROCKS_NATIVE member holds a strong object reference ({strong})", "RMC2205",
                    "Native members are not traced; store ObjectRef/RootRef/Object pointers in ROCKS_PROPERTY fields or use WeakObjectRef/ObjectHandle.").At(native.Site);
            }

            string? worldOnly = WorldOnly(native.Spelling, [], ref cannot);
            if (cannot is not null)
            {
                throw new MetadataException($"{symbol}: cannot verify that ROCKS_NATIVE member holds no strong object reference ({cannot})", "RMC2205",
                    "The metadata compiler follows types held by value through Clang's canonical types; hold only classes that are defined where the reflected header sees them and can be named from the global scope (no local or unnamed classes, no declared-only templates).")
                    .At(native.Site);
            }

            if (worldOnly is not null)
            {
                model.WorldOnlyNatives.Add((symbol, worldOnly, native.Site));
            }
        }

        // 강한 참조 경로다(없으면 null). 값으로 담는 필드·base·배열 원소·템플릿 인자를 따라가고, 포인터·참조는 판정(Object 파생 대상 —
        // 1, 강한 참조 래퍼 — 3)으로 본다. Object 파생 타입은 값으로 닿아도 펼치지 않는다. 직접 멤버의 포인터는 NativeMemberProbe가 본다.
        string? Strong(string type, HashSet<string> visited, bool top, ref string? cannot, bool argument = false)
        {
            string text = CleanType(type);
            if (text.Length == 0 || !visited.Add(text) || visited.Count > MaxClosureTypes)
            {
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
                return Strong(element, visited, top, ref cannot, argument);
            }

            if (SplitTemplate(text) is (string name, List<string> arguments))
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

                foreach (string typeArgument in TypeArguments(arguments, Lookup(text)))
                {
                    if (Strong(typeArgument, visited, false, ref cannot, true) is string inner)
                    {
                        return $"{text} -> {inner}";
                    }
                }
            }

            if (Resolve(text, argument, ref cannot) is not { } resolvedStrong)
            {
                return null;
            }

            (ClosureRecord record, ClosureFacts known2) = resolvedStrong;
            if (known2.ObjectDerived == true)
            {
                return null;
            }

            for (int index = 0; index < record.Fields.Count; ++index)
            {
                if (known2.FieldTypes[index] is not string field)
                {
                    cannot ??= $"{text}::{record.Fields[index]}: {known2.Failures.GetValueOrDefault(record.Fields[index], "no type")}";
                    continue;
                }

                if (Strong(field, visited, false, ref cannot) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            for (int index = 0; index < record.Bases.Count; ++index)
            {
                if (known2.BaseTypes[index] is not string baseType)
                {
                    cannot ??= $"{text}: base '{record.Bases[index].Written}': {known2.Failures.GetValueOrDefault("(base)" + index, "not named")}";
                    continue;
                }

                if (Strong(baseType, visited, false, ref cannot) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            return null;
        }

        // WorldOnly 소유자 형질 경로다(없으면 null). 포인터·참조는 따라가지 않는다.
        string? WorldOnly(string type, HashSet<string> visited, ref string? cannot, bool argument = false)
        {
            string text = CleanType(type);
            if (text.Length == 0 || !visited.Add(text) || visited.Count > MaxClosureTypes || IsIndirect(text))
            {
                return null;
            }

            if (StripArray(text) is string element)
            {
                return WorldOnly(element, visited, ref cannot, argument);
            }

            if (SplitTemplate(text) is (string name, List<string> arguments))
            {
                if (NonOwningTemplates.Contains(CleanType(name)))
                {
                    return null;
                }

                foreach (string typeArgument in TypeArguments(arguments, Lookup(text)))
                {
                    if (WorldOnly(typeArgument, visited, ref cannot, true) is string inner)
                    {
                        return $"{text} -> {inner}";
                    }
                }
            }

            if (Resolve(text, argument, ref cannot) is not { } resolvedWorldOnly)
            {
                return null;
            }

            (ClosureRecord record, ClosureFacts known) = resolvedWorldOnly;

            if (record.WorldOnly)
            {
                return text;
            }

            foreach (string? member in known.FieldTypes.Concat(known.BaseTypes))
            {
                if (member is not null && WorldOnly(member, visited, ref cannot) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            return null;
        }

        // 값으로 닿은 형식의 레코드와 사실이다. builtin·enum·class 아닌 형식은 null이고, 따라갈 수 없는 class면 cannot을 채운다.
        (ClosureRecord, ClosureFacts)? Resolve(string text, bool argument, ref string? cannot)
        {
            if (IsBuiltinSpelling(text) || definedEnums.ContainsKey(text))
            {
                return null;
            }

            if (Lookup(text) is ClosureRecord record)
            {
                if (facts.TryGetValue(record, out ClosureFacts? known) && !known.Failures.ContainsKey("(name)"))
                {
                    return (record, known);
                }

                cannot ??= $"{text} cannot be named from the global scope";
                return null;
            }

            if (argument)
            {
                // 템플릿 인자로만 닿은, 색인에 없는 형식은 값으로 담기지 않았다(Explore와 같은 규칙). 지역 클래스만 거부한다.
                if (localClassNames.Contains(SimpleName(text)))
                {
                    cannot ??= $"{text} is a local class the metadata compiler cannot name";
                }

                return null;
            }

            string? fact = classFacts.GetValueOrDefault(text);
            if (fact == "0")
            {
                return null;
            }

            cannot ??= fact is null || fact == "1"
                ? $"{text} is a class without a complete definition the metadata compiler can follow"
                : $"{text}: {fact[1..]}";
            return null;
        }
    }

    private static void ApplyClosureBase(ClosureFacts known, int position, string? answer, string? failure)
    {
        if (answer is not null)
        {
            known.BaseTypes[position] = answer;
            return;
        }

        known.BaseSteps[position] = known.BaseSteps[position] == 0 ? 1 : 2;
        known.Failures["(base)" + position] = failure ?? "Clang did not name the base";
    }

    // 템플릿 철자의 인자 가운데 형식 인자다. 색인한 특수화면 그 노드의 인자 종류를, 아니면 철자 모양(정수·bool·nullptr·주소·캐스트가
    // 아님)을 쓴다.
    private static IEnumerable<string> TypeArguments(List<string> arguments, ClosureRecord? specialization)
    {
        for (int index = 0; index < arguments.Count; ++index)
        {
            string argument = arguments[index];
            bool type = specialization is not null && index < specialization.ArgumentTypes.Count ? specialization.ArgumentTypes[index]
                : argument.Length != 0 && !(char.IsAsciiDigit(argument[0]) || argument[0] is '-' or '+' or '&' or '(' or '\'' or '"') &&
                    argument is not ("true" or "false" or "nullptr");
            if (type)
            {
                yield return argument;
            }
        }
    }

    // 포인터·참조·멤버 포인터·함수 포인터처럼 판정 형판으로 보는 형식이다(템플릿 인자 밖의 `*`·`&`).
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

    // 정규 철자의 식별자다(템플릿 인자와 한정을 걷은 마지막 칸).
    private static string SimpleName(string text) => BaseIdentifier(text);
}
