using System.Globalization;
using System.Numerics;
using System.Text.Json;

namespace Rocks.MetaCompiler;

// inputs는 이 모듈의 리플렉션 헤더다. 그 헤더에 물리적으로 있는 선언만 이 모듈 타입이 되고, 나머지 리플렉션
// 선언은 imported(의존 모듈 descriptor)에 같은 이름·같은 헤더 파일로 있어야 한다(RMC2003).
// resolveHeader는 descriptor의 header 철자를 이번 include 검색 순서로 찾은 정규 경로다(없으면 null).
internal sealed partial class ClangAstParser(IReadOnlyList<ModuleInput> inputs, StableIdManifest? manifest,
    IReadOnlyDictionary<string, ClassModel> imported, Func<string, string?> resolveHeader)
{
    // 진단의 기본 위치로 쓰는 첫 입력이다.
    private readonly string inputFile = inputs[0].Path;
    private readonly Dictionary<string, ClassModel> classes = new(StringComparer.Ordinal);

    // Struct이면 ROCKS_STRUCT 마커, 아니면 ROCKS_CLASS 마커의 결합이다.
    private sealed record PrefixBinding(object?[] Arguments, SourceSite Site, bool Struct = false);

    // ROCKS_CLASS·ROCKS_STRUCT 마커가 결합한 레코드 정의(AST id → 지정자)다. 필드 annotation의 소유자 판정은 둘을
    // 같이 본다.
    private readonly Dictionary<string, PrefixBinding> prefixClasses = new(StringComparer.Ordinal);

    // ROCKS_ENUM 마커가 결합한 enum 정의(AST id → 지정자)다.
    private readonly Dictionary<string, PrefixBinding> prefixEnums = new(StringComparer.Ordinal);

    private readonly Dictionary<string, string> identifiers = new(StringComparer.Ordinal);

    private readonly HashSet<string> propertyMarkers = new(OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);

    private SourceSite currentSite = new(inputs[0].Path, 1);
    private readonly Dictionary<string, byte[]> sourceBytes = new(StringComparer.Ordinal);
    // 주석을 공백으로 바꾼 원문이다(DeclarationPrefixHas — 다른 원문 검사는 원래 byte를 쓰는 sourceBytes다).
    private readonly Dictionary<string, byte[]> uncommentedSourceBytes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, (string? File, int Offset)> locations = new(StringComparer.Ordinal);
    private string? serializedFile;
    private string? serializedParent;

    // 이 모듈 입력의 리플렉션 선언을 모은다. 해석·검증(SchemaValidator.Validate)은 호출자가 Binds 정규 철자를 채운
    // 뒤에 한다(필드 형식이 Binds 실제 타입으로 미러를 찾으므로).
    public Dictionary<string, ClassModel> Parse(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 2048 });
        IndexLocations(document.RootElement);
        IndexRecords(document.RootElement, []);
        IndexClosure(document.RootElement);
        try
        {
            BindPrefixMarkers(document.RootElement);
            ValidateAnnotationPlacement(document.RootElement, false);
            currentSite = UnknownSite();
            Visit(document.RootElement, [], false, false);
        }
        catch (MetadataException error)
        {
            throw error.At(currentSite);
        }

        if (classes.Count == 0)
        {
            throw new MetadataException("no ROCKS_CLASS, ROCKS_STRUCT or ROCKS_ENUM definitions found in the module's reflected headers");
        }

        foreach (ModuleInput input in inputs)
        {
            if (!classes.Values.Any(model => model.Header == input.Include))
            {
                throw new MetadataException($"reflected header declares no ROCKS_CLASS, ROCKS_STRUCT or ROCKS_ENUM: {input.Include}", "RMC2000",
                    "Remove the header from the module's Metadata list or declare its reflected types.").At(new(input.Path, 1));
            }
        }

        return classes;
    }

    // 배열 필드 실패 진단(FindFieldAt)의 쓴 철자 재귀용 레코드 색인이다(선언 이름 → 필드·base 타입 철자). 클래스
    // 템플릿은 패턴 레코드를 템플릿 이름으로 둔다. 판정이 아니라 문구에만 쓴다 — NATIVE 멤버 판정은 정규 형식 탐침의 레코드 닫힘
    // (ClangAstParser.Canonical.cs)이 한다.
    private sealed record RecordInfo(List<string> Fields, List<string> Bases);

    private readonly Dictionary<string, RecordInfo> records = new(StringComparer.Ordinal);

    // 번역 단위의 모든 정의된 enum(정규 이름 → 열거자 이름·값, 값을 읽지 못한 열거자는 null)이다. Binds enum 미러의
    // 1:1 대조(VerifyEnumMirrors)가 쓴다(CurveData CV10 묶음 리뷰 2).
    private readonly Dictionary<string, List<(string Name, System.Numerics.BigInteger? Value)>> definedEnums = new(StringComparer.Ordinal);

    // 강한 객체 참조를 담는 템플릿이다(StructRefArray는 참조 struct 원소의 목록이다 — Docs/Design/ReflectionFoundations.ko.md
    // 5.6절, ObjectRefMap은 키 → 강한 참조 Map이다 — ReflectionExtensions.ko.md 7.4절).
    private static readonly HashSet<string> StrongReferenceTemplates = new(StringComparer.Ordinal)
    {
        "rocks::ObjectRef", "rocks::RootRef", "rocks::ObjectRefArray", "rocks::StructRefArray", "rocks::ObjectRefMap"
    };

    // 대상 객체를 담지도 추적하지도 않는 참조 템플릿이다. 인자 타입을 따라가지 않는다(WeakObjectRef<T>는 T의 필드를 담지 않고,
    // SoftObjectRef<T>는 에셋 ID와 경로만 담는다).
    private static readonly HashSet<string> NonOwningTemplates = new(StringComparer.Ordinal)
    {
        "rocks::WeakObjectRef",
        "rocks::SoftObjectRef"
    };

    private void IndexRecords(JsonElement node, string[] scope)
    {
        string kind = Text(node, "kind");
        string name = Text(node, "name");
        if (kind == "NamespaceDecl")
        {
            string[] inner = [.. scope, name.Length == 0 ? "(anonymous namespace)" : name];
            foreach (JsonElement child in Children(node))
            {
                IndexRecords(child, inner);
            }

            return;
        }

        if (kind == "ClassTemplateDecl")
        {
            foreach (JsonElement child in Children(node))
            {
                if (Text(child, "kind") == "CXXRecordDecl")
                {
                    IndexRecords(child, scope);
                }
            }

            return;
        }

        if (kind == "EnumDecl")
        {
            IndexEnum(node, scope, name);
            return;
        }

        if (kind == "CXXRecordDecl" && Flag(node, "completeDefinition") && name.Length != 0)
        {
            string qualified = string.Join("::", scope.Append(name));
            List<string> fields = [];
            // 이름 없는 struct/union 멤버의 필드는 부모가 값으로 담으므로 부모 필드로 본다.
            void Collect(JsonElement record)
            {
                foreach (JsonElement child in Children(record))
                {
                    string childKind = Text(child, "kind");
                    if (childKind == "FieldDecl" && child.TryGetProperty("type", out _))
                    {
                        fields.Add(TypeSpelling(child));
                    }
                    else if (childKind == "CXXRecordDecl" && Text(child, "name").Length == 0 && Flag(child, "completeDefinition"))
                    {
                        Collect(child);
                    }
                }
            }

            Collect(node);

            List<string> bases = node.TryGetProperty("bases", out JsonElement baseArray)
                ? baseArray.EnumerateArray().Select(TypeSpelling).ToList() : [];
            records.TryAdd(qualified, new(fields, bases));
            foreach (JsonElement child in Children(node))
            {
                IndexRecords(child, [.. scope, name]);
            }

            return;
        }

        foreach (JsonElement child in Children(node))
        {
            IndexRecords(child, scope);
        }
    }

    // 정의된 enum 하나의 열거자 목록을 색인한다(VisitEnum과 같은 값 규칙 — 초기값이 없으면 이전 값 + 1, 초기값은 Clang이
    // 평가한 ConstantExpr 값이다. 다른 모양이면 그 열거자 값을 null로 둔다). 앞 선언(열거자 없음)은 건너뛴다.
    private void IndexEnum(JsonElement node, string[] scope, string name)
    {
        JsonElement[] constants = Children(node).Where(child => Text(child, "kind") == "EnumConstantDecl").ToArray();
        if (name.Length == 0 || constants.Length == 0)
        {
            return;
        }

        string qualified = string.Join("::", scope.Append(name));
        List<(string Name, System.Numerics.BigInteger? Value)> values = [];
        System.Numerics.BigInteger? next = System.Numerics.BigInteger.Zero;
        foreach (JsonElement constant in constants)
        {
            JsonElement[] initializers = Children(constant).Where(child => !Text(child, "kind").EndsWith("Attr", StringComparison.Ordinal)).ToArray();
            System.Numerics.BigInteger? value = next;
            if (initializers.Length != 0)
            {
                value = initializers.Length == 1 && Text(initializers[0], "kind") == "ConstantExpr" &&
                    System.Numerics.BigInteger.TryParse(Text(initializers[0], "value"), System.Globalization.NumberStyles.Integer,
                        System.Globalization.CultureInfo.InvariantCulture, out System.Numerics.BigInteger parsed)
                    ? parsed : null;
            }

            values.Add((Text(constant, "name"), value));
            next = value + 1;
        }

        definedEnums.TryAdd(qualified, values);
    }

    // Binds enum 미러가 실제 enum의 열거자를 이름·값 1:1로 나열하는지 본다(CurveData CV10 묶음 리뷰 2 — 생성 static_assert는
    // 미러 쪽 열거자만 대조하므로 실제 enum에만 있는 열거자를 놓친다). 어기면 RMC2401이다. ResolveBinds가 정규 철자(Binds)를
    // 채운 뒤 부른다.
    public void VerifyEnumMirrors(IEnumerable<ClassModel> declared)
    {
        foreach (ClassModel mirror in declared.Where(model => model.IsEnum && model.Binds.Length != 0).OrderBy(model => model.Name, StringComparer.Ordinal))
        {
            const string hint = "List every enumerator of the real enum in the mirror with the same name and value (no more, no fewer).";
            if (!definedEnums.TryGetValue(mirror.Binds, out List<(string Name, System.Numerics.BigInteger? Value)>? real))
            {
                throw new MetadataException($"{mirror.Name}: the enumerators of {mirror.Binds} are not visible from the reflected headers, so the mirror cannot list every enumerator",
                    "RMC2401", hint).At(mirror.BindsSite ?? mirror.Site);
            }

            List<string> problems = [];
            foreach ((string name, System.Numerics.BigInteger? value) in real)
            {
                EnumeratorModel? listed = mirror.Enumerators.FirstOrDefault(item => item.Name == name);
                if (listed is null)
                {
                    problems.Add($"{name} is missing");
                }
                else if (value is null || value.Value != listed.Value)
                {
                    problems.Add($"{name} has value {(value is null ? "?" : value.Value.ToString(System.Globalization.CultureInfo.InvariantCulture))} in the real enum");
                }
            }

            problems.AddRange(mirror.Enumerators.Where(item => real.All(entry => entry.Name != item.Name)).Select(item => $"{item.Name} is not in the real enum"));
            if (problems.Count != 0)
            {
                throw new MetadataException($"{mirror.Name}: a Binds enum mirror must list every enumerator of {mirror.Binds} with the same name and value ({string.Join(", ", problems)})",
                    "RMC2401", hint).At(mirror.BindsSite ?? mirror.Site);
            }
        }
    }

    // 타입 철자를 cv·elaborated 키워드·앞의 ::없이 다듬는다(SchemaValidator가 NATIVE 멤버의 직접 타입을 찾을 때도 쓴다).
    internal static string CleanType(string type)
    {
        string text = type.Trim();
        foreach (string prefix in new[] { "const ", "volatile ", "struct ", "class ", "union ", "enum " })
        {
            while (text.StartsWith(prefix, StringComparison.Ordinal))
            {
                text = text[prefix.Length..].TrimStart();
            }
        }

        foreach (string suffix in new[] { " const", " volatile" })
        {
            while (text.EndsWith(suffix, StringComparison.Ordinal))
            {
                text = text[..^suffix.Length].TrimEnd();
            }
        }

        return text.StartsWith("::", StringComparison.Ordinal) ? text[2..] : text;
    }

    // 철자에서 문자·문자열 리터럴 밖의 글자를 차례로 낸다: 위치, 글자, 꺾쇠 깊이, 괄호(`()`·`[]`) 깊이. 여는 글자는 열기 전, 닫는
    // 글자는 닫은 뒤의 깊이라 바깥 템플릿 인자 목록의 `<`·`>`는 꺾쇠 깊이 0이다. Clang 정규 철자는 char 템플릿 인자를 문자 리터럴로
    // 쓰므로(`ByChar<'>'>`) 리터럴 안의 글자는 세지 않는다.
    private static IEnumerable<(int Index, char Character, int Angle, int Group)> CodeCharacters(string type)
    {
        int angle = 0;
        int group = 0;
        for (int index = 0; index < type.Length; ++index)
        {
            char c = type[index];
            if (c is '\'' or '"')
            {
                for (++index; index < type.Length && type[index] != c; ++index)
                {
                    if (type[index] == '\\')
                    {
                        ++index;
                    }
                }

                continue;
            }

            if (c == '>')
            {
                --angle;
            }
            else if (c is ')' or ']')
            {
                --group;
            }

            yield return (index, c, angle, group);
            if (c == '<')
            {
                ++angle;
            }
            else if (c is '(' or '[')
            {
                ++group;
            }
        }
    }

    // 끝의 템플릿 인자 목록을 나눈다(`Outer<int>::Inner<T>` → `Outer<int>::Inner`와 `T`). 끝 `>`와 짝인 `<`에서 자르고, 인자는 그
    // 목록 안의 꺾쇠·괄호 깊이 0 쉼표에서 나눈다. 끝이 템플릿 인자 목록이 아니면 null이다.
    private static (string Name, List<string> Arguments)? SplitTemplate(string type)
    {
        if (!type.EndsWith('>'))
        {
            return null;
        }

        int open = -1;
        foreach ((int index, char c, int angle, int group) in CodeCharacters(type))
        {
            if (c == '<' && angle == 0 && group == 0)
            {
                open = index;
            }
        }

        if (open < 0)
        {
            return null;
        }

        List<string> arguments = [];
        int start = open + 1;
        foreach ((int index, char c, int angle, int group) in CodeCharacters(type))
        {
            if (index > open && index < type.Length - 1 && c == ',' && angle == 1 && group == 0)
            {
                arguments.Add(type[start..index].Trim());
                start = index + 1;
            }
        }

        arguments.Add(type[start..^1].Trim());
        return (type[..open].Trim(), arguments);
    }

    // 정규·desugar 철자의 마지막 식별자다: 꼬리 템플릿 인자를 걷고 꺾쇠·괄호 깊이 0의 마지막 `::` 뒤를 자른다
    // (`probe::Outer<int>::Inner` → `Inner`, `rocks::Leaf<0, int>` → `Leaf`).
    private static string LastIdentifier(string type)
    {
        string text = CleanType(type);
        if (SplitTemplate(text) is (string name, _))
        {
            text = name;
        }

        int cut = 0;
        foreach ((int index, char c, int angle, int group) in CodeCharacters(text))
        {
            if (c == ':' && angle == 0 && group == 0 && index + 2 < text.Length && text[index + 1] == ':')
            {
                cut = index + 2;
            }
        }

        return text[cut..].Trim();
    }

    // 템플릿 인자 밖(꺾쇠 깊이 0, 리터럴 밖)에서 token이 나오면 참이다(포인터·참조·함수 포인터 판정용).
    private static bool TopLevelContains(string type, string token) =>
        CodeCharacters(type).Any(entry => entry.Angle == 0 && string.CompareOrdinal(type, entry.Index, token, 0, token.Length) == 0);

    // 선언의 물리 파일이 이 모듈 입력이면 그 입력을, 아니면 null을 돌려준다.
    private ModuleInput? OwningInput(string? file) =>
        file is null ? null : inputs.FirstOrDefault(input => CanonicalPath.Equals(input.Path, file));

    private static IEnumerable<JsonElement> Children(JsonElement node) =>
        node.TryGetProperty("inner", out JsonElement inner) ? inner.EnumerateArray() : [];

    private static string Text(JsonElement node, string property) =>
        node.TryGetProperty(property, out JsonElement value) ? value.GetString() ?? "" : "";

    private static bool Flag(JsonElement node, string property) =>
        node.TryGetProperty(property, out JsonElement value) && value.ValueKind == JsonValueKind.True;

    // struct 검증 hook 선언의 Clang 함수 형식 철자다: `Result<void> (const S &) noexcept`(이름공간 한정은 선택).
    private static readonly System.Text.RegularExpressions.Regex ValidateSignature = new(
        @"^(?:::)?(?:[A-Za-z_][\w]*::)*Result<void> \(const (?:struct |class )?(?:::)?(?:[A-Za-z_][\w]*::)*(?<type>[A-Za-z_][\w]*) &\) noexcept$",
        System.Text.RegularExpressions.RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    // struct의 Validate 멤버 하나를 검증 hook으로 받는다(RMC2309): 함수 템플릿·두 번째 선언(overload)·비정적·시그니처가 다른 선언과
    // GENERATED_BODY 없는 private·protected 선언을 거부한다.
    private void CheckValidateHook(ClassModel model, JsonElement member, string memberKind, string qualified, string access, bool generatedBody)
    {
        const string hint = "Declare exactly one `static rocks::Result<void> Validate(const S& value) noexcept;` (public, or private with ROCKS_GENERATED_BODY).";
        string unqualified = qualified[(qualified.LastIndexOf("::", StringComparison.Ordinal) + 2)..];
        if (memberKind == "FunctionTemplateDecl")
        {
            throw new MetadataException($"{qualified}::Validate: the struct validation hook cannot be a function template", "RMC2309", hint);
        }

        if (model.HasValidate)
        {
            throw new MetadataException($"{qualified}::Validate: the struct validation hook cannot be overloaded", "RMC2309", hint);
        }

        if (Text(member, "storageClass") != "static")
        {
            throw new MetadataException($"{qualified}::Validate: the struct validation hook must be a static member function", "RMC2309", hint);
        }

        // consteval 함수는 런타임 함수표가 부를 수 없다(생성 코드 컴파일 오류 대신 RMC로 거부한다).
        if (DeclarationPrefixHas(member, "consteval"))
        {
            throw new MetadataException($"{qualified}::Validate: the struct validation hook cannot be consteval", "RMC2309", hint);
        }

        // Binds 미러의 Validate는 SchemaValidator가 미러 규칙으로 거부한다(인자가 실제 타입이라 모양을 여기서 보지 않는다). 철자
        // (`::rocks::Result<void>`·별칭 `const Self&`)에 기대지 않도록 쓴 철자와 desugar한 철자 중 하나가 맞으면 받는다.
        string spelling = member.TryGetProperty("type", out JsonElement type) ? Text(type, "qualType") : "";
        string desugared = member.TryGetProperty("type", out JsonElement sugarType) ? Text(sugarType, "desugaredQualType") : "";
        bool Matches(string text)
        {
            System.Text.RegularExpressions.Match match = ValidateSignature.Match(text);
            return match.Success && match.Groups["type"].Value == unqualified;
        }

        if (model.BindsSpelling.Length == 0 && !Matches(spelling) && !(desugared.Length != 0 && Matches(desugared)) &&
            !Matches(SelfAliasResolved(spelling, member)))
        {
            throw new MetadataException($"{qualified}::Validate: the struct validation hook signature is `{spelling}`, not `Result<void> (const {unqualified} &) noexcept`",
                "RMC2309", hint);
        }

        if (access != "public" && !generatedBody)
        {
            throw new MetadataException($"{qualified}::Validate: a private/protected struct validation hook requires ROCKS_GENERATED_BODY", "RMC2309", hint);
        }

        model.HasValidate = true;
        model.ValidateSite ??= currentSite;
    }

    // Validate 인자 철자의 별칭(`using Self = S;`처럼 struct 안에서 선언한 TypeAliasDecl·TypedefDecl)을 그 대상 철자로 바꾼다(Clang JSON의
    // 함수 형식은 desugaredQualType을 주지 않는 경우가 있다). 별칭이 없으면 그대로다.
    private string SelfAliasResolved(string spelling, JsonElement member)
    {
        foreach ((string alias, string target) in currentAliases)
        {
            string written = $"(const {alias} &)";
            if (spelling.Contains(written, StringComparison.Ordinal))
            {
                return spelling.Replace(written, $"(const {target} &)", StringComparison.Ordinal);
            }
        }

        _ = member;
        return spelling;
    }

    // 지금 struct 안의 타입 별칭(이름 → 대상 철자)이다(Validate 시그니처 대조용 — 멤버 순회가 채운다).
    private readonly List<(string Alias, string Target)> currentAliases = [];

    // 선언 node의 이름 앞(같은 선언의 시작 — 앞의 ';'·'{'·'}' 뒤부터)에 word가 낱말로 쓰여 있는지다(consteval처럼 Clang JSON이
    // 구별하지 않는 지정자). 주석은 걷어낸 원문(WithoutComments)에서 보므로 앞 주석의 낱말·기호는 판정에 들지 않는다. 원문 위치를 모르면
    // 거짓이다.
    private bool DeclarationPrefixHas(JsonElement node, string word)
    {
        if (!locations.TryGetValue(Text(node, "id"), out var location) || location.File is null || location.Offset < 0 ||
            !File.Exists(location.File))
        {
            return false;
        }

        if (!uncommentedSourceBytes.TryGetValue(location.File, out byte[]? bytes))
        {
            bytes = WithoutComments(File.ReadAllBytes(location.File));
            uncommentedSourceBytes.Add(location.File, bytes);
        }

        int end = Math.Min(location.Offset, bytes.Length);
        int start = end;
        while (start > 0 && bytes[start - 1] is not ((byte)';' or (byte)'{' or (byte)'}'))
        {
            --start;
        }

        string prefix = System.Text.Encoding.UTF8.GetString(bytes, start, end - start);
        return System.Text.RegularExpressions.Regex.IsMatch(prefix, $@"\b{word}\b", System.Text.RegularExpressions.RegexOptions.CultureInvariant,
            TimeSpan.FromSeconds(1));
    }

    // C++ 원문에서 주석(`//`·`/* */`)을 공백으로 바꾼 사본이다(줄바꿈과 byte 위치는 그대로). 문자열·문자 상수 안의 `//`는 주석이 아니다
    // (raw 문자열은 일반 문자열처럼 본다 — 선언 앞 지정자 판정에는 충분하다). 숫자 구분자 `1'000`의 `'`는 문자 상수가 아니다(IsDigitSeparator).
    private static byte[] WithoutComments(byte[] source)
    {
        byte[] result = (byte[])source.Clone();
        int index = 0;
        while (index < result.Length)
        {
            byte current = result[index];
            byte next = index + 1 < result.Length ? result[index + 1] : (byte)0;
            if (current == (byte)'/' && next == (byte)'/')
            {
                while (index < result.Length && result[index] != (byte)'\n')
                {
                    result[index] = (byte)' ';
                    ++index;
                }

                continue;
            }

            if (current == (byte)'/' && next == (byte)'*')
            {
                result[index] = (byte)' ';
                result[index + 1] = (byte)' ';
                index += 2;
                while (index < result.Length && !(result[index] == (byte)'*' && index + 1 < result.Length && result[index + 1] == (byte)'/'))
                {
                    if (result[index] != (byte)'\n')
                    {
                        result[index] = (byte)' ';
                    }

                    ++index;
                }

                if (index < result.Length)
                {
                    result[index] = (byte)' ';
                    result[index + 1] = (byte)' ';
                    index += 2;
                }

                continue;
            }

            if (current == (byte)'\'' && IsDigitSeparator(result, index))
            {
                ++index;
                continue;
            }

            if (current == (byte)'"' || current == (byte)'\'')
            {
                ++index;
                while (index < result.Length && result[index] != current && result[index] != (byte)'\n')
                {
                    index += result[index] == (byte)'\\' ? 2 : 1;
                }

                ++index;
                continue;
            }

            ++index;
        }

        return result;
    }

    // index의 `'`가 숫자 구분자(1'000·0xFF'FF)인지다: 앞이 수 토큰(숫자로 시작해 영숫자·`_`·`'`·`.`로 이어지는 토큰)의 영숫자이고 뒤도
    // 영숫자다. u8'a'처럼 문자로 시작하는 토큰 뒤의 `'`는 문자 상수다.
    private static bool IsDigitSeparator(byte[] source, int index)
    {
        static bool IsWord(byte value) => value is >= (byte)'0' and <= (byte)'9' or >= (byte)'a' and <= (byte)'z' or >= (byte)'A' and <= (byte)'Z' or (byte)'_';

        if (index == 0 || index + 1 >= source.Length || !IsWord(source[index - 1]) || !IsWord(source[index + 1]))
        {
            return false;
        }

        int start = index - 1;
        while (start > 0 && (IsWord(source[start - 1]) || source[start - 1] == (byte)'\'' || source[start - 1] == (byte)'.'))
        {
            --start;
        }

        return source[start] is >= (byte)'0' and <= (byte)'9';
    }

    // 노드 타입의 Clang 정규 철자다(별칭은 desugar한 철자). CompilerDriver가 Binds 별칭을 풀 때도 쓴다.
    internal static string TypeSpelling(JsonElement node)
    {
        JsonElement type = node.GetProperty("type");
        string resolved = Text(type, "desugaredQualType");
        return resolved.Length != 0 ? resolved : Text(type, "qualType");
    }

    // 폭 1 bitfield bool로 받는 부호 없는 정수 철자다(Clang 정규 철자 — uint8·16·32·64, 리플렉션 B9m).
    private static readonly string[] UnsignedBitfieldSpellings =
        ["unsigned char", "unsigned short", "unsigned int", "unsigned long", "unsigned long long"];

    // NATIVE bitfield로 받는 정수·bool 철자다(리플렉션 B9m).
    private static bool IsIntegerSpelling(string spelling) => spelling is "bool" or "char" or "signed char" or "unsigned char" or "short"
        or "unsigned short" or "int" or "unsigned int" or "long" or "unsigned long" or "long long" or "unsigned long long";

    // PROPERTY bitfield 규칙(RMC2207 — 리플렉션 B9m)이다: Binds 미러 안이 아니고, 폭(FieldDecl 안 상수 식)이 1이며, 형식이 부호 없는
    // 정수 4종이다(`bool b : 1`은 UE와 같이 거부한다).
    private static void CheckPropertyBitfield(JsonElement member, string symbol, string spelling, bool mirror)
    {
        if (mirror)
        {
            throw new MetadataException($"{symbol}: a Binds mirror cannot declare a bitfield", "RMC2207",
                "Mirror the real type's members as plain members; Core value types have no bitfields.");
        }

        BigInteger? width = Children(member).Select(Constant).OfType<BigInteger>().Cast<BigInteger?>().FirstOrDefault();
        if (width != 1)
        {
            throw new MetadataException($"{symbol}: a ROCKS_PROPERTY bitfield must have width 1", "RMC2207",
                "Declare reflected bitfields as `uint8 name : 1` (reflected as bool); use a plain integer member for wider values.");
        }

        if (spelling == "bool")
        {
            throw new MetadataException($"{symbol}: `bool name : 1` cannot be a ROCKS_PROPERTY (as in UE)", "RMC2207",
                "Declare the bitfield as `uint8 name : 1`; it is reflected as bool.");
        }

        if (!UnsignedBitfieldSpellings.Contains(spelling, StringComparer.Ordinal))
        {
            throw new MetadataException($"{symbol}: a ROCKS_PROPERTY bitfield must be an unsigned integer (uint8, uint16, uint32 or uint64)", "RMC2207",
                "Declare the bitfield as `uint8 name : 1`; signed integers, enums and floating types cannot be reflected bitfields.");
        }
    }

    private static object? Constant(JsonElement node)
    {
        string kind = Text(node, "kind");
        if (kind == "ConstantExpr" && BigInteger.TryParse(Text(node, "value"), NumberStyles.Integer,
                CultureInfo.InvariantCulture, out BigInteger evaluated))
        {
            return evaluated;
        }

        if (kind == "StringLiteral")
        {
            return ClangLiteral.Decode(Text(node, "value"));
        }

        if (kind == "IntegerLiteral")
        {
            return BigInteger.Parse(Text(node, "value"), CultureInfo.InvariantCulture);
        }

        if (kind is "ConstantExpr" or "ImplicitCastExpr" or "ParenExpr")
        {
            JsonElement[] children = Children(node).ToArray();
            return children.Length == 1 ? Constant(children[0]) : null;
        }

        return null;
    }

    private static object?[]? Annotation(JsonElement node, string marker)
    {
        object?[]? result = null;
        foreach (JsonElement child in Children(node))
        {
            if (Text(child, "kind") != "AnnotateAttr")
            {
                continue;
            }

            object?[] arguments = Children(child).Select(Constant).ToArray();
            if (arguments.Length == 0 || arguments[0] is not string name || name != marker)
            {
                continue;
            }

            if (result is not null)
            {
                throw new MetadataException($"duplicate {marker} annotation on {Text(node, "name")}");
            }

            result = arguments[1..];
        }

        return result;
    }

    private string Identity(string kind, string symbol)
    {
        if (manifest is null)
        {
            throw new MetadataException($"{symbol}: stable identity requires an identity manifest", "RMC2100",
                "Pass --identity-manifest and register this declaration with the identity authoring tool.");
        }

        string id;
        try
        {
            id = manifest.Resolve(kind, symbol);
        }
        catch (InvalidDataException error)
        {
            throw new MetadataException($"{symbol}: stable identity unavailable: {error.Message}", "RMC2100",
                "Use the identity authoring tool to register or explicitly rename the declaration; ordinary builds never modify the manifest.");
        }

        if (!identifiers.TryAdd(id, symbol))
        {
            throw new MetadataException($"{symbol}: duplicate stable ID already used by {identifiers[id]}");
        }

        return id;
    }

    private static string FieldArguments(JsonElement member, string symbol)
    {
        object?[]? modern = Annotation(member, "rocks.field.arguments");
        if (modern is null || modern.Length != 1 || modern[0] is not string raw)
        {
            throw new MetadataException($"{symbol}: every nonstatic field requires a PROPERTY or NATIVE annotation");
        }

        return raw;
    }

    private static bool HasFieldAnnotation(JsonElement node) =>
        Annotation(node, "rocks.field.arguments") is not null;

    private void Visit(JsonElement node, string[] scope, bool anonymous, bool templated)
    {
        string kind = Text(node, "kind");
        if (kind == "NamespaceDecl")
        {
            string name = Text(node, "name");
            foreach (JsonElement child in Children(node))
            {
                Visit(child, name.Length == 0 ? scope : [.. scope, name], anonymous || name.Length == 0, templated);
            }

            return;
        }

        if (kind is "ClassTemplateDecl" or "ClassTemplateSpecializationDecl")
        {
            templated = true;
        }

        if (kind == "EnumDecl" && prefixEnums.TryGetValue(Text(node, "id"), out PrefixBinding? enumPrefix))
        {
            VisitEnum(node, scope, anonymous, templated, enumPrefix);
            return;
        }

        if (kind == "CXXRecordDecl" && Flag(node, "completeDefinition"))
        {
            string name = Text(node, "name");
            string qualified = string.Join("::", scope.Append(name));
            prefixClasses.TryGetValue(Text(node, "id"), out PrefixBinding? prefix);
            if (prefix is not null)
            {
                object?[] mark = prefix.Arguments;
                bool isStruct = prefix.Struct;
                string label = isStruct ? "STRUCT" : "CLASS";
                UpdateSite(node);
                SourceSite classSite = currentSite;
                currentSite = prefix.Site;
                if (anonymous || templated || name.Length == 0)
                {
                    throw new MetadataException($"{qualified}: anonymous/template/invalid reflected type is unsupported");
                }

                if (mark.Length != 1 || mark[0] is not string classSpecifiers)
                {
                    throw new MetadataException($"{qualified}: invalid {label} annotation arguments");
                }

                // identity 조회보다 먼저 소유 모듈을 가린다: 의존 모듈의 리플렉션 선언을 이 모듈 명세에서 찾지 않는다.
                string? physical = locations.TryGetValue(Text(node, "id"), out var declared) ? declared.File : null;
                ModuleInput? owner = OwningInput(physical);
                if (owner is null)
                {
                    if (!imported.TryGetValue(qualified, out ClassModel? external) || external.IsEnum || external.IsStruct != isStruct || physical is null ||
                        resolveHeader(external.Header) is not string externalFile || !CanonicalPath.Equals(externalFile, physical))
                    {
                        throw new MetadataException($"{qualified}: reflected declaration is neither in this module's Metadata headers nor in an imported descriptor (same name and header)",
                            "RMC2003", "Add the header to the owning module's Metadata list and depend on that module; do not redeclare reflected types.");
                    }

                    foreach (JsonElement child in Children(node))
                    {
                        if (Text(child, "kind") is "CXXRecordDecl" or "ClassTemplateDecl" or "EnumDecl")
                        {
                            Visit(child, name.Length == 0 ? scope : [.. scope, name], anonymous, templated);
                        }
                    }

                    return;
                }

                ClassSpec? classSpec = isStruct ? null : Specifiers.Class(classSpecifiers, qualified);
                StructSpec? structSpec = isStruct ? Specifiers.Struct(classSpecifiers, qualified) : null;
                string id = Identity("type", qualified);
                currentSite = classSite;
                JsonElement[] bases = node.TryGetProperty("bases", out JsonElement baseArray) ? baseArray.EnumerateArray().ToArray() : [];
                if (isStruct)
                {
                    // struct 상속(리플렉션 B9k — Docs/Design/ReflectionExtensions.ko.md 12절): base는 public 비가상이고, Binds 미러는
                    // base를 가질 수 없으며, 리플렉션 struct는 virtual 함수를 가질 수 없다(다형성은 InstancedStruct<Base>). 리플렉션
                    // base 하나와 필드 없는 비리플렉션 base의 판별은 SchemaValidator가 한다.
                    if (bases.Any(item => Text(item, "access") != "public" || Flag(item, "isVirtual")))
                    {
                        throw new MetadataException($"{qualified}: a ROCKS_STRUCT base must be public and nonvirtual", "RMC2301",
                            "Derive publicly and without virtual: struct Derived : public Base { ... }.");
                    }

                    if (structSpec!.Binds.Length != 0 && bases.Length != 0)
                    {
                        throw new MetadataException($"{qualified}: a Binds mirror cannot have a base", "RMC2301",
                            "Declare every mirrored member in the mirror itself; a Binds mirror describes one real type without inheritance.");
                    }

                    if (node.TryGetProperty("definitionData", out JsonElement structDefinition) && Flag(structDefinition, "isPolymorphic"))
                    {
                        throw new MetadataException($"{qualified}: a ROCKS_STRUCT cannot have virtual functions", "RMC2301",
                            "Reflected structs are plain values; hold polymorphic values in rocks::InstancedStruct<Base> instead.");
                    }
                }

                if (!isStruct && (bases.Length != 1 || Text(bases[0], "access") != "public" || Flag(bases[0], "isVirtual")))
                {
                    throw new MetadataException($"{qualified}: exactly one public nonvirtual Object/reflected base is required");
                }

                bool abstractType = !isStruct && node.TryGetProperty("definitionData", out JsonElement definition) && Flag(definition, "isAbstract");

                ClassModel model = new()
                {
                    Name = qualified,
                    Id = id,
                    Site = currentSite,
                    Version = classSpec?.Version ?? structSpec!.Version,
                    Base = isStruct ? "" : TypeSpelling(bases[0]),
                    Abstract = abstractType || (classSpec?.Abstract ?? false),
                    WorldOnly = classSpec?.WorldOnly ?? false,
                    Transient = classSpec?.Transient ?? false,
                    Deprecated = classSpec?.Deprecated ?? false,
                    WithinSpelling = classSpec?.Within ?? "",
                    Header = owner.Include,
                    Private = owner.Private,
                    IsStruct = isStruct,
                    Atomic = structSpec?.Atomic ?? false,
                    SettingsBranch = structSpec?.SettingsBranch ?? "",
                    SettingsSection = structSpec?.SettingsSection ?? "",
                    // 기본 도구 이름(한정하지 않은 C++ 이름)과 같은 Name은 지정하지 않은 것과 같다.
                    ToolName = (classSpec?.ToolName ?? structSpec!.ToolName) is string toolName && toolName != TypeToolName.Default(qualified) ? toolName : "",
                    BindsSpelling = structSpec?.Binds ?? "",
                    BindsSite = structSpec is { Binds.Length: > 0 } ? prefix.Site : null
                };
                if (isStruct)
                {
                    model.StructBaseSpellings.AddRange(bases.Select(TypeSpelling));
                }

                JsonElement[] members = Children(node).ToArray();
                currentAliases.Clear();
                bool generatedBody = members.Any(member => Annotation(member, "rocks.generated") is not null);
                string access = Text(node, "tagUsed") == "class" ? "private" : "public";
                foreach (JsonElement member in members)
                {
                    UpdateSite(member);
                    string memberKind = Text(member, "kind");
                    string memberName = Text(member, "name");
                    if (memberKind == "AccessSpecDecl")
                    {
                        access = Text(member, "access");
                    }

                    // 클래스의 Trace는 생성 코드가 만든다. struct의 Trace는 GeneratedType<S>::Trace라 struct 자신의
                    // 멤버 함수와 겹치지 않는다.
                    if (!isStruct && memberKind == "CXXMethodDecl" && memberName == "Trace")
                    {
                        throw new MetadataException($"{qualified}: manual Trace conflicts with complete generated field tracing");
                    }

                    if (memberKind == "VarDecl" && HasFieldAnnotation(member))
                    {
                        throw new MetadataException($"{qualified}: static fields cannot carry reflected field annotations");
                    }

                    // struct의 Migrate(메서드·함수 템플릿, 오버로드 포함)는 생성 Ops가 `&S::Migrate`로 연결한다. 접근·
                    // 시그니처를 추측하지 않고 생성 코드 컴파일이 확인한다 — private이면 friend인 GeneratedType이
                    // 접근하도록 ROCKS_GENERATED_BODY가 필요하다.
                    if (isStruct && memberName == "Migrate" && memberKind is ("CXXMethodDecl" or "FunctionTemplateDecl"))
                    {
                        if (access != "public" && !generatedBody)
                        {
                            throw new MetadataException($"{qualified}::Migrate: private/protected Migrate requires ROCKS_GENERATED_BODY",
                                hint: "Make Migrate public or add ROCKS_GENERATED_BODY() to the struct so the generated registration can take its address.");
                        }

                        model.HasMigrate = true;
                        model.MigrateSite ??= currentSite;
                    }

                    // struct의 검증 hook(Docs/Design/CurveData.ko.md 9.1절)은 `static Result<void> Validate(const S&) noexcept` 한 개다.
                    // 모양이 다르면 조용히 hook 없이 등록되지 않게 RMC2309로 거부한다(정확한 형식은 생성 static_assert도 대조한다).
                    if (isStruct && memberName == "Validate" && memberKind is ("CXXMethodDecl" or "FunctionTemplateDecl"))
                    {
                        CheckValidateHook(model, member, memberKind, qualified, access, generatedBody);
                    }

                    if (isStruct && memberKind is "TypeAliasDecl" or "TypedefDecl" && member.TryGetProperty("type", out JsonElement aliasType))
                    {
                        currentAliases.Add((memberName, Text(aliasType, "qualType")));
                    }

                    // 숨은 friend `Validate`와 `using Base::Validate`는 hook이 아니다(조용히 무시되거나 자기 hook과 overload를 만든다).
                    if (isStruct && memberKind == "FriendDecl" &&
                        Children(member).Any(inner => Text(inner, "kind") is "FunctionDecl" or "FunctionTemplateDecl" && Text(inner, "name") == "Validate"))
                    {
                        throw new MetadataException($"{qualified}: a friend Validate is not a struct validation hook", "RMC2309",
                            "Declare `static rocks::Result<void> Validate(const S& value) noexcept;` as a member instead.");
                    }

                    if (isStruct && memberKind == "UsingDecl" && (memberName == "Validate" || memberName.EndsWith("::Validate", StringComparison.Ordinal)))
                    {
                        throw new MetadataException($"{qualified}: a using-declaration of Validate is not supported", "RMC2309",
                            "Remove it: a reflected base's Validate hook already runs before the derived struct's own hook.");
                    }

                    if (memberKind != "FieldDecl")
                    {
                        continue;
                    }

                    string symbol = qualified + "::" + memberName;
                    object?[]? native = Annotation(member, "rocks.field.native");
                    if (native is not null)
                    {
                        if (HasFieldAnnotation(member))
                        {
                            throw new MetadataException($"{symbol}: a field is either ROCKS_PROPERTY or ROCKS_NATIVE, not both", "RMC2204",
                                "Keep ROCKS_PROPERTY for persistent reflected fields and ROCKS_NATIVE for runtime-only members.");
                        }

                        if (native.Length != 1 || native[0] is not string nativeSpecifiers)
                        {
                            throw new MetadataException($"{symbol}: invalid ROCKS_NATIVE annotation arguments");
                        }

                        Specifiers.Native(nativeSpecifiers, symbol);
                        if (memberName.Length == 0)
                        {
                            throw new MetadataException($"{symbol}: only named members are supported");
                        }

                        // NATIVE bitfield(리플렉션 B9m — Docs/Design/ReflectionExtensions.ko.md 14절)는 정수·bool이면 받는다(강한 참조를
                        // 담을 수 없으므로 탐침은 쓰지 않는다). 그 밖의 형식(enum 등) bitfield는 받지 않는다.
                        bool nativeBitfield = Flag(member, "isBitfield");
                        if (nativeBitfield && !IsIntegerSpelling(TypeSpelling(member)))
                        {
                            throw new MetadataException($"{symbol}: a ROCKS_NATIVE bitfield must be an integer or bool", "RMC2207",
                                "Use an integer or bool bitfield, or a plain member.");
                        }

                        if (access != "public" && !generatedBody)
                        {
                            throw new MetadataException($"{symbol}: private/protected NATIVE requires ROCKS_GENERATED_BODY");
                        }

                        // 강한 참조(RMC2205)·WorldOnly 소유자 형질 경로는 정규 형식 탐침 뒤 레코드 닫힘이 본다(VerifyNativeMembersAsync —
                        // Docs/Design/ClangVersionSupport.ko.md 4.4절). 여기 철자는 탐침이 정규 철자로 바꾼다.
                        string nativeType = TypeSpelling(member);
                        model.NativeMembers.Add(new NativeMemberModel { Name = memberName, Spelling = nativeType, Site = currentSite, Bitfield = nativeBitfield });
                        continue;
                    }

                    string fieldSpecifiers = FieldArguments(member, symbol);
                    if (memberName.Length == 0)
                    {
                        throw new MetadataException($"{symbol}: only named members are supported");
                    }

                    // PROPERTY bitfield(리플렉션 B9m — Docs/Design/ReflectionExtensions.ko.md 14절)는 UE와 같이 폭 1의 부호 없는 정수
                    // (uint8·uint16·uint32·uint64)만 Bool 값으로 받는다. `bool b : 1`·폭 ≠ 1·부호 있는 정수·enum·Binds 미러 안
                    // bitfield는 RMC2207이다. 접근은 생성 접근자(GeneratedType<Owner>::Get_·Set_)가 한다.
                    string spelling = TypeSpelling(member);
                    bool bitfield = Flag(member, "isBitfield");
                    if (bitfield)
                    {
                        CheckPropertyBitfield(member, symbol, spelling, structSpec is { Binds.Length: > 0 });
                        spelling = "bool";
                    }

                    if (access != "public" && !generatedBody)
                    {
                        throw new MetadataException($"{symbol}: private/protected PROPERTY requires ROCKS_GENERATED_BODY");
                    }

                    model.OwnFields.Add(new FieldModel
                    {
                        Id = Identity("field", symbol),
                        Site = currentSite,
                        Name = memberName,
                        Owner = qualified,
                        NativeSpelling = spelling,
                        Bitfield = bitfield,
                        Spec = Specifiers.Property(fieldSpecifiers, symbol, memberName)
                    });
                }

                classes.Add(qualified, model);
            }

            // 생성 특수화와 집계 함수가 바깥에서 이름을 쓰므로 클래스 안의 리플렉션 enum·class·struct는 public이어야 한다.
            string childAccess = Text(node, "tagUsed") == "class" ? "private" : "public";
            foreach (JsonElement child in Children(node))
            {
                string childKind = Text(child, "kind");
                if (childKind == "AccessSpecDecl")
                {
                    childAccess = Text(child, "access");
                }

                if (childKind == "EnumDecl" && childAccess != "public" && prefixEnums.ContainsKey(Text(child, "id")))
                {
                    UpdateSite(child);
                    throw new MetadataException($"{qualified}::{Text(child, "name")}: a reflected enum nested in a class must be public", "RMC2401",
                        "Declare the nested ROCKS_ENUM in a public section or move it to namespace scope.");
                }

                if (childKind == "CXXRecordDecl" && childAccess != "public" && prefixClasses.TryGetValue(Text(child, "id"), out PrefixBinding? nested))
                {
                    UpdateSite(child);
                    string macro = PrefixSourceVerifier.Macro(nested.Struct ? PrefixMarker.Struct : PrefixMarker.Class);
                    throw new MetadataException($"{qualified}::{Text(child, "name")}: a reflected {PrefixSourceVerifier.Noun(nested.Struct ? PrefixMarker.Struct : PrefixMarker.Class)} nested in a class must be public",
                        hint: $"Declare the nested {macro} type in a public section or move it to namespace scope.");
                }

                if (childKind is "CXXRecordDecl" or "ClassTemplateDecl" or "EnumDecl")
                {
                    Visit(child, name.Length == 0 ? scope : [.. scope, name], anonymous, templated);
                }
            }

            return;
        }

        foreach (JsonElement child in Children(node))
        {
            Visit(child, scope, anonymous, templated);
        }
    }
}
