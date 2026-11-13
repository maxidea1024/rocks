using System.Globalization;
using System.Numerics;
using System.Text.Json;

namespace Rocks.MetaCompiler;

// inputs는 이 모듈의 리플렉션 헤더다. 그 헤더에 물리적으로 있는 선언만 이 모듈 타입이 되고, 나머지 리플렉션
// 선언은 imported(의존 모듈 descriptor)에 같은 이름·같은 헤더 파일로 있어야 한다(RMC2003).
// resolveHeader는 descriptor의 header 철자를 이번 include 검색 순서로 찾은 정규 경로다(없으면 null).
internal sealed class ClangAstParser(IReadOnlyList<ModuleInput> inputs, StableIdManifest? manifest,
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

    // ROCKS_NATIVE 재귀 검사용 레코드 색인이다(정규 이름 → 필드·base 타입 철자, WorldOnly 소유자 형질). 클래스
    // 템플릿은 패턴 레코드를 템플릿 이름으로 둔다(특수화의 필드는 패턴 필드와 템플릿 인자로 근사한다).
    private sealed record RecordInfo(List<string> Fields, List<string> Bases, bool RequiresWorldOnlyOwner);

    private readonly Dictionary<string, RecordInfo> records = new(StringComparer.Ordinal);

    private static readonly HashSet<string> StrongReferenceTemplates = new(StringComparer.Ordinal)
    {
        "rocks::ObjectRef", "rocks::RootRef", "rocks::ObjectRefArray"
    };

    // 대상 객체를 담지도 추적하지도 않는 참조 템플릿이다. 인자 타입을 따라가지 않는다(WeakRef<T>는 T의 필드를 담지 않고,
    // SoftObjectRef<T>는 에셋 ID와 경로만 담는다).
    private static readonly HashSet<string> NonOwningTemplates = new(StringComparer.Ordinal)
    {
        "rocks::WeakRef",
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

        if (kind == "CXXRecordDecl" && Flag(node, "completeDefinition") && name.Length != 0)
        {
            string qualified = string.Join("::", scope.Append(name));
            List<string> fields = [];
            bool worldOnly = false;
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
                    else if (childKind == "VarDecl" && Text(child, "name") == "RocksRequiresWorldOnlyOwner" && ContainsTrue(child))
                    {
                        worldOnly = true;
                    }
                }
            }

            Collect(node);

            List<string> bases = node.TryGetProperty("bases", out JsonElement baseArray)
                ? baseArray.EnumerateArray().Select(TypeSpelling).ToList() : [];
            records.TryAdd(qualified, new(fields, bases, worldOnly));
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

    private static bool ContainsTrue(JsonElement node) =>
        (Text(node, "kind") == "CXXBoolLiteralExpr" && node.TryGetProperty("value", out JsonElement value) && value.ValueKind == JsonValueKind.True) ||
        Children(node).Any(ContainsTrue);

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

    // 깊이 0의 template 인자 목록을 나눈다. 템플릿이 아니면 null이다.
    private static (string Name, List<string> Arguments)? SplitTemplate(string type)
    {
        int open = type.IndexOf('<');
        if (open < 0 || !type.EndsWith('>'))
        {
            return null;
        }

        List<string> arguments = [];
        int depth = 0;
        int start = open + 1;
        for (int index = open + 1; index < type.Length - 1; ++index)
        {
            char c = type[index];
            if (c is '<' or '(' or '[')
            {
                ++depth;
            }
            else if (c is '>' or ')' or ']')
            {
                --depth;
            }
            else if (c == ',' && depth == 0)
            {
                arguments.Add(type[start..index].Trim());
                start = index + 1;
            }
        }

        arguments.Add(type[start..^1].Trim());
        return (type[..open].Trim(), arguments);
    }

    // 템플릿 인자 밖(깊이 0)에서 token이 나오면 참이다(함수 포인터·참조 판정용).
    private static bool TopLevelContains(string type, string token)
    {
        int depth = 0;
        for (int index = 0; index < type.Length; ++index)
        {
            char c = type[index];
            if (c == '<')
            {
                ++depth;
            }
            else if (c == '>')
            {
                --depth;
            }
            else if (depth == 0 && string.CompareOrdinal(type, index, token, 0, token.Length) == 0)
            {
                return true;
            }
        }

        return false;
    }

    private bool IsObjectDerived(string record, HashSet<string> visited)
    {
        if (record == "rocks::Object")
        {
            return true;
        }

        return visited.Add(record) && records.TryGetValue(record, out RecordInfo? info) &&
            info.Bases.Any(baseType => IsObjectDerived(CleanType(baseType), visited));
    }

    // NATIVE 멤버 타입이 강한 객체 참조를 담으면 그 경로를, 아니면 null을 돌려준다. 값으로 담는 필드·base·배열 원소·
    // 템플릿 인자와 패턴 필드를 따라간다. 포인터·참조는 대상이 Object 파생이거나 대상이 다시 강한 참조(T**,
    // ObjectRef<T>*)인지만 본다 — 빌린 포인터가 가리키는 비-Object 레코드의 필드는 그 소유자가 추적한다. Object 파생
    // 타입은 값이나 템플릿 인자로 닿아도 펼치지 않는다(객체는 Registry에 살고, 템플릿 인자로는 handle·borrow의 대상이다).
    private string? StrongReferencePath(string type, HashSet<string> visited, bool throughPointer = false)
    {
        string text = CleanType(type);
        if (text.Length == 0 || !visited.Add((throughPointer ? "*" : "") + text) || visited.Count > 512)
        {
            return null;
        }

        if (TopLevelContains(text, "(*)") || TopLevelContains(text, "(&)"))
        {
            return null; // 함수 포인터·참조
        }

        if (text.EndsWith(']'))
        {
            int bracket = text.LastIndexOf('[');
            return bracket > 0 ? StrongReferencePath(text[..bracket], visited, throughPointer) : null;
        }

        if (text.EndsWith('*') || text.EndsWith('&'))
        {
            string stripped = text.TrimEnd('&').TrimEnd();
            if (stripped.EndsWith('*'))
            {
                stripped = stripped[..^1];
            }

            string pointee = CleanType(stripped);
            if (IsObjectDerived(pointee, []))
            {
                return $"raw pointer to Object-derived {pointee}";
            }

            return StrongReferencePath(pointee, visited, throughPointer: true) is string inner ? $"{text} -> {inner}" : null;
        }

        if (SplitTemplate(text) is (string name, List<string> arguments))
        {
            string templateName = CleanType(name);
            if (StrongReferenceTemplates.Contains(templateName))
            {
                return templateName;
            }

            if (NonOwningTemplates.Contains(templateName) || throughPointer)
            {
                return null;
            }

            foreach (string argument in arguments)
            {
                if (StrongReferencePath(argument, visited) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            text = templateName;
        }

        if (throughPointer || IsObjectDerived(text, []))
        {
            return null;
        }

        if (records.TryGetValue(text, out RecordInfo? info))
        {
            foreach (string member in info.Fields.Concat(info.Bases))
            {
                if (StrongReferencePath(member, visited) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }
        }

        return null;
    }

    // NATIVE 멤버 타입이 (값으로 담는 타입까지) RocksRequiresWorldOnlyOwner 형질을 가지면 그 경로를 돌려준다.
    private string? RequiresWorldOnlyOwnerPath(string type, HashSet<string> visited)
    {
        string text = CleanType(type);
        if (text.Length == 0 || !visited.Add(text) || visited.Count > 512 || text.EndsWith('*') || text.EndsWith('&') ||
            TopLevelContains(text, "(*)") || TopLevelContains(text, "(&)"))
        {
            return null;
        }

        if (text.EndsWith(']'))
        {
            int bracket = text.LastIndexOf('[');
            return bracket > 0 ? RequiresWorldOnlyOwnerPath(text[..bracket], visited) : null;
        }

        if (SplitTemplate(text) is (string name, List<string> arguments))
        {
            if (NonOwningTemplates.Contains(CleanType(name)))
            {
                return null;
            }

            foreach (string argument in arguments)
            {
                if (RequiresWorldOnlyOwnerPath(argument, visited) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }

            text = CleanType(name);
        }

        if (records.TryGetValue(text, out RecordInfo? info))
        {
            if (info.RequiresWorldOnlyOwner)
            {
                return text;
            }

            foreach (string member in info.Fields.Concat(info.Bases))
            {
                if (RequiresWorldOnlyOwnerPath(member, visited) is string inner)
                {
                    return $"{text} -> {inner}";
                }
            }
        }

        return null;
    }

    // 선언의 물리 파일이 이 모듈 입력이면 그 입력을, 아니면 null을 돌려준다.
    private ModuleInput? OwningInput(string? file) =>
        file is null ? null : inputs.FirstOrDefault(input => CanonicalPath.Equals(input.Path, file));

    private static IEnumerable<JsonElement> Children(JsonElement node) =>
        node.TryGetProperty("inner", out JsonElement inner) ? inner.EnumerateArray() : [];

    private static string Text(JsonElement node, string property) =>
        node.TryGetProperty(property, out JsonElement value) ? value.GetString() ?? "" : "";

    private static bool Flag(JsonElement node, string property) =>
        node.TryGetProperty(property, out JsonElement value) && value.ValueKind == JsonValueKind.True;

    // 노드 타입의 Clang 정규 철자다(별칭은 desugar한 철자). CompilerDriver가 Binds 별칭을 풀 때도 쓴다.
    internal static string TypeSpelling(JsonElement node)
    {
        JsonElement type = node.GetProperty("type");
        string resolved = Text(type, "desugaredQualType");
        return resolved.Length != 0 ? resolved : Text(type, "qualType");
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
                if (isStruct && bases.Length != 0)
                {
                    // struct 상속은 v1에서 금지다(Docs/Design/ReflectionFoundations.ko.md 4.1절).
                    throw new MetadataException($"{qualified}: a ROCKS_STRUCT cannot have a base class", "RMC2301",
                        "Remove the base and hold the shared members as a nested ROCKS_STRUCT field instead.");
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
                    Header = owner.Include,
                    Private = owner.Private,
                    IsStruct = isStruct,
                    BindsSpelling = structSpec?.Binds ?? "",
                    BindsSite = structSpec is { Binds.Length: > 0 } ? prefix.Site : null
                };
                JsonElement[] members = Children(node).ToArray();
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
                        if (memberName.Length == 0 || Flag(member, "isBitfield"))
                        {
                            throw new MetadataException($"{symbol}: only named non-bitfield members are supported");
                        }

                        if (access != "public" && !generatedBody)
                        {
                            throw new MetadataException($"{symbol}: private/protected NATIVE requires ROCKS_GENERATED_BODY");
                        }

                        string nativeType = TypeSpelling(member);
                        if (StrongReferencePath(nativeType, []) is string path)
                        {
                            throw new MetadataException($"{symbol}: ROCKS_NATIVE member holds a strong object reference ({path})", "RMC2205",
                                "Native members are not traced; store ObjectRef/RootRef/Object pointers in ROCKS_PROPERTY fields or use WeakRef/ObjectHandle.");
                        }

                        if (RequiresWorldOnlyOwnerPath(nativeType, []) is string worldOnlyPath)
                        {
                            model.WorldOnlyNatives.Add((symbol, worldOnlyPath, currentSite));
                        }

                        model.NativeMembers.Add(new NativeMemberModel { Name = memberName, Spelling = nativeType, Site = currentSite });
                        continue;
                    }

                    string fieldSpecifiers = FieldArguments(member, symbol);
                    if (memberName.Length == 0 || Flag(member, "isBitfield"))
                    {
                        throw new MetadataException($"{symbol}: only named non-bitfield members are supported");
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
                        NativeSpelling = TypeSpelling(member),
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

    // ROCKS_ENUM 정의 하나를 모델로 만든다(Docs/Design/ReflectionFoundations.ko.md 3.4절). underlying은 원문에
    // 명시한 uint8·uint16·uint32·int32만, 열거자는 하나 이상이고 값 별칭이 없어야 하며 Flags enum은 음수 값을 둘
    // 수 없다(RMC2401). 접두 유무를 합친 도구 이름은 대소문자 무시로 유일하다(RMC2203).
    private void VisitEnum(JsonElement node, string[] scope, bool anonymous, bool templated, PrefixBinding prefix)
    {
        string name = Text(node, "name");
        string qualified = string.Join("::", scope.Append(name));
        UpdateSite(node);
        SourceSite enumSite = currentSite;
        currentSite = prefix.Site;
        if (anonymous || templated || name.Length == 0)
        {
            throw new MetadataException($"{qualified}: anonymous/template/invalid reflected enum is unsupported");
        }

        if (prefix.Arguments.Length != 1 || prefix.Arguments[0] is not string enumSpecifiers)
        {
            throw new MetadataException($"{qualified}: invalid ENUM annotation arguments");
        }

        // 의존 모듈의 리플렉션 enum은 그 모듈 descriptor의 같은 이름·같은 헤더 선언이어야 한다(클래스와 같다).
        string? physical = locations.TryGetValue(Text(node, "id"), out var declared) ? declared.File : null;
        ModuleInput? owner = OwningInput(physical);
        if (owner is null)
        {
            if (!imported.TryGetValue(qualified, out ClassModel? external) || !external.IsEnum || physical is null ||
                resolveHeader(external.Header) is not string externalFile || !CanonicalPath.Equals(externalFile, physical))
            {
                throw new MetadataException($"{qualified}: reflected declaration is neither in this module's Metadata headers nor in an imported descriptor (same name and header)",
                    "RMC2003", "Add the header to the owning module's Metadata list and depend on that module; do not redeclare reflected types.");
            }

            return;
        }

        EnumSpec spec = Specifiers.Enum(enumSpecifiers, qualified);
        string id = Identity("type", qualified);
        currentSite = enumSite;
        if (Text(node, "scopedEnumTag") is not ("class" or "struct"))
        {
            throw new MetadataException($"{qualified}: ROCKS_ENUM requires a scoped enum (enum class)", "RMC2401",
                "Declare the reflected enum as 'enum class Name : rocks::uint8 { ... }'.");
        }

        string underlyingSpelling = "";
        if (node.TryGetProperty("fixedUnderlyingType", out JsonElement fixedType))
        {
            string desugared = Text(fixedType, "desugaredQualType");
            underlyingSpelling = desugared.Length != 0 ? desugared : Text(fixedType, "qualType");
        }

        if (!HasExplicitUnderlying(node))
        {
            throw new MetadataException($"{qualified}: ROCKS_ENUM requires an explicitly written uint8/uint16/uint32/int32 underlying type", "RMC2401",
                "Write the underlying type directly after the enum name, e.g. 'enum class Name : rocks::uint8'.");
        }

        string? underlying = underlyingSpelling switch
        {
            "unsigned char" => "UInt8",
            "unsigned short" => "UInt16",
            "unsigned int" => "UInt32",
            "int" => "Int32",
            _ => null
        };
        if (underlying is null)
        {
            throw new MetadataException($"{qualified}: unsupported ROCKS_ENUM underlying type '{underlyingSpelling}'", "RMC2401",
                "Use rocks::uint8, uint16, uint32 or int32; int8/int16 and 64-bit enums are unsupported.");
        }

        (long minimum, long maximum) = EnumRules.Range(underlying)!.Value;
        List<EnumeratorModel> enumerators = [];
        BigInteger next = BigInteger.Zero;
        foreach (JsonElement constant in Children(node))
        {
            if (Text(constant, "kind") != "EnumConstantDecl")
            {
                continue;
            }

            UpdateSite(constant);
            string constantName = Text(constant, "name");
            string symbol = qualified + "::" + constantName;
            // 런타임 스키마는 열거자 이름을 ASCII 식별자로 받는다(Clang은 비ASCII·$ 이름도 받는다).
            if (!Specifiers.IsAsciiIdentifier(constantName))
            {
                throw new MetadataException($"{symbol}: reflected enumerator names must be ASCII identifiers", "RMC2401",
                    "Rename the enumerator with [A-Za-z_][A-Za-z0-9_]*; use DisplayName for localized text.");
            }

            if (enumerators.Count == EnumRules.MaxEnumerators)
            {
                throw new MetadataException($"{qualified}: a reflected enum has at most {EnumRules.MaxEnumerators} enumerators", "RMC2401",
                    "Split the enum or keep it unreflected.");
            }

            // 초기값이 없는 열거자는 이전 값 + 1이다(첫 열거자는 0). underlying이 고정된 enum의 초기값은 Clang이 평가한
            // 값을 가진 ConstantExpr이다. 다른 모양이면 값을 추측하지 않고 거부한다(생성 코드의 static_assert가 한 번 더
            // 대조한다).
            BigInteger value = next;
            JsonElement[] initializers = Children(constant).Where(child => !Text(child, "kind").EndsWith("Attr", StringComparison.Ordinal)).ToArray();
            if (initializers.Length != 0)
            {
                if (initializers.Length != 1 || Text(initializers[0], "kind") != "ConstantExpr" ||
                    !BigInteger.TryParse(Text(initializers[0], "value"), NumberStyles.Integer, CultureInfo.InvariantCulture, out value))
                {
                    throw new MetadataException($"{symbol}: unsupported enumerator initializer AST shape", "RMC2001",
                        "Use the supported Clang/toolchain; add a reviewed parser case before accepting a new AST shape.");
                }
            }

            if (value < minimum || value > maximum || (spec.Flags && value < 0))
            {
                throw new MetadataException($"{symbol}: value {value} is outside the {underlying} range{(spec.Flags ? " or negative in a Flags enum" : "")}", "RMC2401",
                    "Keep enumerator values within the underlying type; Flags enums use non-negative bit values.");
            }

            object?[]? annotation = Annotation(constant, "rocks.enumerator");
            if (annotation is not null && (annotation.Length != 1 || annotation[0] is not string))
            {
                throw new MetadataException($"{symbol}: invalid ENUMERATOR annotation arguments");
            }

            EnumeratorSpec enumeratorSpec = Specifiers.Enumerator(annotation is null ? "" : (string)annotation[0]!, symbol, constantName);
            EnumeratorModel model = new(constantName, enumeratorSpec.ToolName, enumeratorSpec.DisplayName, (long)value, enumeratorSpec.Hidden);
            EnumeratorModel? alias = enumerators.FirstOrDefault(other => other.Value == model.Value);
            if (alias is not null)
            {
                throw new MetadataException($"{symbol}: value {value} aliases {qualified}::{alias.Name}", "RMC2401",
                    "Give every reflected enumerator a distinct value; aliases make stored values ambiguous for tools.");
            }

            EnumeratorModel? collision = enumerators.FirstOrDefault(other => EnumRules.ToolNamesCollide(other.EffectiveToolName, model.EffectiveToolName, spec.ToolPrefix));
            if (collision is not null)
            {
                throw new MetadataException($"{symbol}: tool name '{model.EffectiveToolName}' collides with {qualified}::{collision.Name} (case-insensitive, with or without ToolPrefix)", "RMC2203",
                    "Give the enumerator a distinct Name=\"...\" specifier.");
            }

            enumerators.Add(model);
            next = value + 1;
        }

        if (enumerators.Count == 0)
        {
            currentSite = enumSite;
            throw new MetadataException($"{qualified}: ROCKS_ENUM requires an enum definition with at least one enumerator", "RMC2401",
                "Define the enumerators directly after ROCKS_ENUM; forward declarations and empty enums are unsupported.");
        }

        currentSite = enumSite;
        classes.Add(qualified, new ClassModel
        {
            Name = qualified,
            Id = id,
            Site = enumSite,
            Version = 1,
            Base = "",
            Abstract = false,
            Header = owner.Include,
            Private = owner.Private,
            IsEnum = true,
            Underlying = underlying,
            FlagsEnum = spec.Flags,
            ToolPrefix = spec.ToolPrefix,
            Enumerators = enumerators
        });
    }

    // Clang은 underlying을 생략한 enum class에도 int를 fixedUnderlyingType으로 준다. 원문에서 이름 바로 뒤(공백·주석
    // 다음)에 ':'가 있는지로 명시 여부를 본다. 이름이 그 위치에 그대로 쓰여 있지 않거나(매크로·UCN) 판단이 모호하면
    // (주석 안 줄 이음) 명시하지 않은 것으로 본다 — 거부 쪽으로만 틀린다.
    private bool HasExplicitUnderlying(JsonElement node)
    {
        if (!node.TryGetProperty("loc", out JsonElement nameLocation) || nameLocation.TryGetProperty("expansionLoc", out _) ||
            !locations.TryGetValue(Text(node, "id"), out var location) || location.File is null || location.Offset < 0 ||
            !File.Exists(location.File))
        {
            return false;
        }

        if (!sourceBytes.TryGetValue(location.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(location.File);
            sourceBytes.Add(location.File, bytes);
        }

        byte[] name = System.Text.Encoding.UTF8.GetBytes(Text(node, "name"));
        if (location.Offset + name.Length > bytes.Length || !bytes.AsSpan(location.Offset, name.Length).SequenceEqual(name))
        {
            return false;
        }

        int cursor = location.Offset + name.Length;
        while (cursor < bytes.Length)
        {
            byte value = bytes[cursor];
            if (value is (byte)' ' or (byte)'\t' or (byte)'\r' or (byte)'\n')
            {
                ++cursor;
            }
            else if (value == (byte)'/' && cursor + 1 < bytes.Length && bytes[cursor + 1] == (byte)'/')
            {
                while (cursor < bytes.Length && bytes[cursor] != (byte)'\n')
                {
                    // 줄 이음(\ + 줄바꿈)은 다음 줄까지 주석으로 만든다.
                    if (bytes[cursor] == (byte)'\\' && (cursor + 1 < bytes.Length && (bytes[cursor + 1] is (byte)'\n' or (byte)'\r')))
                    {
                        return false;
                    }

                    ++cursor;
                }
            }
            else if (value == (byte)'/' && cursor + 1 < bytes.Length && bytes[cursor + 1] == (byte)'*')
            {
                int end = bytes.AsSpan(cursor + 2).IndexOf("*/"u8);
                if (end < 0)
                {
                    return false;
                }

                cursor += end + 4;
            }
            else
            {
                return value == (byte)':' && (cursor + 1 >= bytes.Length || bytes[cursor + 1] != (byte)':');
            }
        }

        return false;
    }

    // ROCKS_CLASS·ROCKS_STRUCT 마커는 바로 뒤의 완전한 class/struct에, ROCKS_ENUM 마커는 바로 뒤의 enum 정의에만 결합한다. lexical scope·실제 파일과
    // 비활성 전처리 영역을 확인하지 않은 채 AST 이웃만 믿으면 다른 타입을 리플렉션할 수 있다.
    private void BindPrefixMarkers(JsonElement parent)
    {
        JsonElement[] children = Children(parent).ToArray();
        for (int index = 0; index < children.Length; ++index)
        {
            JsonElement marker = children[index];
            if (Text(marker, "kind") == "EnumDecl")
            {
                JsonElement[] enumMarked = Children(marker).Where(child =>
                    Annotation(child, "rocks.enum.marker") is not null).ToArray();
                if (enumMarked.Length != 0)
                {
                    UpdateSite(enumMarked[0]);
                    string enumParentKind = Text(parent, "kind");
                    if (enumMarked.Length != 1 || Children(marker).Count() != 1 || Text(marker, "name").Length != 0 ||
                        enumParentKind is not ("TranslationUnitDecl" or "NamespaceDecl" or "CXXRecordDecl"))
                    {
                        throw new MetadataException("ROCKS_ENUM marker must be a standalone declaration in a namespace or class scope");
                    }

                    if (index + 1 >= children.Length || Text(children[index + 1], "kind") != "EnumDecl" ||
                        Text(children[index + 1], "name").Length == 0)
                    {
                        throw new MetadataException("ROCKS_ENUM must be immediately followed by a named enum definition",
                            hint: "Place ROCKS_ENUM immediately before its enum class definition; put both inside the same #if block.");
                    }

                    JsonElement enumTarget = children[index + 1];
                    SourceSite enumMarkerSite = currentSite;
                    VerifyPrefixSource(enumMarked[0], enumTarget, PrefixMarker.Enum);
                    if (!prefixEnums.TryAdd(Text(enumTarget, "id"), new PrefixBinding(Annotation(enumMarked[0], "rocks.enum.marker")!, enumMarkerSite)))
                    {
                        throw new MetadataException("duplicate ROCKS_ENUM marker for the same enum");
                    }
                }

                // ROCKS_CLASS·ROCKS_STRUCT 마커는 같은 규칙으로 바로 뒤의 완전한 class/struct 정의에 결합한다. 한
                // 레코드에는 둘 중 하나만 붙는다.
                foreach (PrefixMarker kind in new[] { PrefixMarker.Class, PrefixMarker.Struct })
                {
                    string annotation = kind == PrefixMarker.Struct ? "rocks.struct.marker" : "rocks.class.marker";
                    string macro = PrefixSourceVerifier.Macro(kind);
                    JsonElement[] marked = Children(marker).Where(child =>
                        Annotation(child, annotation) is not null).ToArray();
                    if (marked.Length == 0)
                    {
                        continue;
                    }

                    UpdateSite(marked[0]);
                    string parentKind = Text(parent, "kind");
                    if (marked.Length != 1 || Text(marker, "name").Length != 0 ||
                        parentKind is not ("TranslationUnitDecl" or "NamespaceDecl" or "CXXRecordDecl"))
                    {
                        throw new MetadataException($"{macro} marker must be a standalone declaration in a namespace or class scope");
                    }

                    // 마커를 겹쳐 쓰면 첫 마커의 바로 다음 선언이 다른 레코드 마커다.
                    if (index + 1 < children.Length && IsRecordMarker(children[index + 1]))
                    {
                        throw new MetadataException($"{macro} cannot be stacked with another ROCKS_CLASS or ROCKS_STRUCT marker; a record takes exactly one",
                            hint: "Keep exactly one ROCKS_CLASS (Object type) or ROCKS_STRUCT (value type) immediately before the definition.");
                    }

                    if (index + 1 >= children.Length || Text(children[index + 1], "kind") != "CXXRecordDecl" ||
                        !Flag(children[index + 1], "completeDefinition") ||
                        Text(children[index + 1], "tagUsed") is not ("class" or "struct"))
                    {
                        throw new MetadataException($"{macro} must be immediately followed by a complete class or struct definition",
                            hint: $"Place {macro} immediately before its class or struct definition; put both inside the same #if block.");
                    }

                    JsonElement target = children[index + 1];
                    if (Annotation(target, "rocks.type") is not null)
                    {
                        throw new MetadataException($"{macro} prefix marker and direct rocks.type annotation cannot both annotate the same class");
                    }

                    SourceSite markerSite = currentSite;
                    VerifyPrefixSource(marked[0], target, kind);
                    PrefixBinding binding = new(Annotation(marked[0], annotation)!, markerSite, kind == PrefixMarker.Struct);
                    if (!prefixClasses.TryAdd(Text(target, "id"), binding))
                    {
                        throw new MetadataException(prefixClasses[Text(target, "id")].Struct != binding.Struct
                            ? "a record takes either ROCKS_CLASS or ROCKS_STRUCT, not both"
                            : $"duplicate {macro} marker for the same record");
                    }
                }
            }
            // 다른 AST 항목도 재귀 검사한다. 함수/지역 class의 마커는 무시하지 않고 거부한다.
            BindPrefixMarkers(marker);
        }
    }

    // ROCKS_CLASS·ROCKS_STRUCT 마커(마커 enum 정의)인지다.
    private static bool IsRecordMarker(JsonElement node) =>
        Text(node, "kind") == "EnumDecl" && Children(node).Any(child =>
            Annotation(child, "rocks.class.marker") is not null || Annotation(child, "rocks.struct.marker") is not null);

    private void VerifyPrefixSource(JsonElement marker, JsonElement target, PrefixMarker kind = PrefixMarker.Class)
    {
        string macro = PrefixSourceVerifier.Macro(kind);
        string noun = PrefixSourceVerifier.Noun(kind);
        if (!locations.TryGetValue(Text(marker, "id"), out var markLocation) ||
            !locations.TryGetValue(Text(target, "id"), out var targetLocation) ||
            markLocation.File is null || targetLocation.File is null ||
            !File.Exists(markLocation.File) || !File.Exists(targetLocation.File) ||
            !CanonicalPath.Equals(markLocation.File, targetLocation.File))
        {
            throw new MetadataException($"{macro} and {noun} must have the same known physical source file",
                hint: $"Place the marker and {noun} directly in one source file; macro-generated or ambiguous locations are unsupported.");
        }

        if (!target.TryGetProperty("range", out JsonElement range) ||
            !range.TryGetProperty("begin", out JsonElement begin) ||
            begin.TryGetProperty("expansionLoc", out _) ||
            !begin.TryGetProperty("offset", out JsonElement classOffset) || !classOffset.TryGetInt32(out int end))
        {
            throw new MetadataException($"{macro} target location is macro-generated or ambiguous",
                hint: $"Write the {noun} keyword directly after {macro}; wrapper macros are unsupported.");
        }

        if (!sourceBytes.TryGetValue(markLocation.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(markLocation.File);
            sourceBytes.Add(markLocation.File, bytes);
        }

        PrefixSourceVerifier.Verify(bytes, markLocation.Offset, end, kind);
    }

    private void ValidateAnnotationPlacement(JsonElement node, bool reflectedOwner,
        bool directReflectedMember = false, string? directOwnerFile = null, bool reflectedEnum = false)
    {
        string kind = Text(node, "kind");
        UpdateSite(node);
        if (Annotation(node, "rocks.type") is not null || Annotation(node, "rocks.field") is not null)
        {
            throw new MetadataException("legacy direct reflection annotations are unsupported; use ROCKS_CLASS/ROCKS_PROPERTY");
        }

        bool classAnnotation = prefixClasses.ContainsKey(Text(node, "id"));
        bool fieldAnnotation = HasFieldAnnotation(node);
        object?[]? generated = Annotation(node, "rocks.generated");
        if (classAnnotation && (kind != "CXXRecordDecl" || !Flag(node, "completeDefinition")))
        {
            throw new MetadataException("ROCKS_CLASS/ROCKS_STRUCT must annotate a complete class definition");
        }

        if (fieldAnnotation && (kind != "FieldDecl" || !reflectedOwner))
        {
            throw new MetadataException("ROCKS_PROPERTY must annotate a nonstatic data field owned by a reflected CLASS or STRUCT");
        }

        if (Annotation(node, "rocks.field.native") is not null && (kind != "FieldDecl" || !reflectedOwner))
        {
            throw new MetadataException("ROCKS_NATIVE must annotate a nonstatic data field owned by a reflected CLASS or STRUCT");
        }

        if (Annotation(node, "rocks.enumerator") is not null && (kind != "EnumConstantDecl" || !reflectedEnum))
        {
            throw new MetadataException("ROCKS_ENUMERATOR must annotate an enumerator of a ROCKS_ENUM enum");
        }

        if (fieldAnnotation && Annotation(node, "rocks.field.native") is not null)
        {
            throw new MetadataException("a field is either ROCKS_PROPERTY or ROCKS_NATIVE, not both", "RMC2204",
                "Keep ROCKS_PROPERTY for persistent reflected fields and ROCKS_NATIVE for runtime-only members.");
        }

        if (fieldAnnotation)
        {
            VerifyPropertySource(node);
        }

        if (generated is not null)
        {
            string signature = node.TryGetProperty("type", out JsonElement type) ? Text(type, "qualType") : "";
            if (kind != "CXXMethodDecl" || !directReflectedMember ||
                Text(node, "name") != "rocksGeneratedAccessMarker" ||
                Text(node, "storageClass") != "static" || signature != "void () noexcept" || generated.Length != 0)
            {
                throw new MetadataException("ROCKS_GENERATED_BODY marker must be a static noexcept marker directly owned by a reflected CLASS or STRUCT",
                    hint: "Place exactly one ROCKS_GENERATED_BODY() directly inside a reflected class or struct; do not hand-write or attach its annotation elsewhere.");
            }

            VerifyGeneratedBodySource(node, directOwnerFile);
        }

        JsonElement[] children = Children(node).ToArray();
        if (kind == "CXXRecordDecl")
        {
            JsonElement[] markers = children.Where(child => Annotation(child, "rocks.generated") is not null).ToArray();
            if (markers.Length > 1)
            {
                UpdateSite(markers[1]);
                throw new MetadataException("duplicate ROCKS_GENERATED_BODY marker in the same class",
                    hint: "Remove the duplicate and leave one ROCKS_GENERATED_BODY() in the reflected class.");
            }
        }

        // 직접 소유자가 리플렉션 클래스인 경우에만 generated marker를 허용한다. 중첩 클래스나
        // method-local 선언에 조상 클래스의 권한이 전파되지 않도록 별도로 구분한다.
        bool childOwner = kind == "CXXRecordDecl" ? classAnnotation : reflectedOwner;
        bool directMember = kind == "CXXRecordDecl" && classAnnotation && Flag(node, "completeDefinition");
        string? ownerFile = directMember && locations.TryGetValue(Text(node, "id"), out var ownerLocation)
            ? ownerLocation.File : null;
        bool enumOwner = kind == "EnumDecl" && prefixEnums.ContainsKey(Text(node, "id"));
        foreach (JsonElement child in children)
        {
            ValidateAnnotationPlacement(child, childOwner, directMember, ownerFile, enumOwner);
        }
    }

    // attribute의 expansion과 필드 시작이 직접 작성된 동일 매크로에서 시작하는지 확인한다.
    // 불분명한 위치를 추측하지 않고 거부하며, 하나의 매크로로 여러 필드를 선언하지 않는다.
    private void VerifyPropertySource(JsonElement field)
    {
        if (!locations.TryGetValue(Text(field, "id"), out var declaration) ||
            declaration.File is null || !File.Exists(declaration.File) || declaration.Offset < 0 ||
            !field.TryGetProperty("loc", out JsonElement location) || location.TryGetProperty("expansionLoc", out _))
        {
            throw InvalidPropertySource();
        }

        JsonElement attribute = Children(field).FirstOrDefault(child => Text(child, "kind") == "AnnotateAttr" &&
            Children(child).Select(Constant).FirstOrDefault() is string marker && marker == "rocks.field.arguments");
        if (attribute.ValueKind == JsonValueKind.Undefined ||
            !TryExpansion(attribute, out string? attrFile, out int begin) || attrFile is null ||
            !CanonicalPath.Equals(attrFile, declaration.File) ||
            begin < 0 || begin >= declaration.Offset)
        {
            throw InvalidPropertySource();
        }

        // LLVM Clang은 필드 범위 시작을 매크로 확장으로, Apple Clang은 매크로 바로 뒤
        // 타입의 직접 위치로 기록한다. 후자의 경우 실제 확장 위치는 attribute에 명시된
        // 동일 파일이어야 하며, 필드의 이름과 타입 시작은 같은 include 문맥에 있어야 한다.
        int directFieldBegin = -1;
        if (TryExpansion(field, out string? fieldFile, out int fieldExpansion))
        {
            if (fieldFile is null || !CanonicalPath.Equals(fieldFile, declaration.File) || fieldExpansion != begin)
            {
                throw InvalidPropertySource();
            }
        }
        else
        {
            if (!field.TryGetProperty("range", out JsonElement range) ||
                !range.TryGetProperty("begin", out JsonElement direct) ||
                direct.TryGetProperty("expansionLoc", out _) || direct.TryGetProperty("spellingLoc", out _) ||
                !direct.TryGetProperty("offset", out JsonElement offset) || !offset.TryGetInt32(out directFieldBegin) ||
                directFieldBegin <= begin || directFieldBegin > declaration.Offset)
            {
                throw InvalidPropertySource();
            }

            string directFile = Text(direct, "file");
            if (directFile.Length != 0 && (!File.Exists(directFile) || !CanonicalPath.Equals(directFile, declaration.File)))
            {
                throw InvalidPropertySource();
            }

            string locationParent = location.TryGetProperty("includedFrom", out JsonElement locationInclude)
                ? Text(locationInclude, "file") : "";
            string directParent = direct.TryGetProperty("includedFrom", out JsonElement directInclude)
                ? Text(directInclude, "file") : "";
            if (!string.Equals(locationParent.Replace('\\', '/'), directParent.Replace('\\', '/'),
                    OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
            {
                throw InvalidPropertySource();
            }
        }

        if (!sourceBytes.TryGetValue(declaration.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(declaration.File);
            sourceBytes.Add(declaration.File, bytes);
        }

        if (declaration.Offset >= bytes.Length)
        {
            throw InvalidPropertySource();
        }

        int markerLine = 1;
        for (int index = 0; index < begin; ++index)
        {
            if (bytes[index] == (byte)'\n')
            {
                ++markerLine;
            }
        }

        currentSite = new(declaration.File, markerLine);
        string key = CanonicalPath.Resolve(declaration.File) + "\0" + begin.ToString(CultureInfo.InvariantCulture);
        if (!propertyMarkers.Add(key))
        {
            throw new MetadataException("each PROPERTY must directly annotate exactly one field",
                hint: "Declare each field separately with its own ROCKS_PROPERTY macro.");
        }

        ReadOnlySpan<byte> macro = "ROCKS_PROPERTY"u8;
        int cursor = begin;
        if (cursor > bytes.Length - macro.Length || !bytes.AsSpan(cursor, macro.Length).SequenceEqual(macro))
        {
            throw InvalidPropertySource();
        }

        cursor += macro.Length;
        SkipTrivia();
        if (cursor >= declaration.Offset || bytes[cursor++] != (byte)'(')
        {
            throw InvalidPropertySource();
        }

        int depth = 1;
        while (cursor < declaration.Offset && depth != 0)
        {
            byte value = bytes[cursor++];
            if (value == (byte)'(')
            {
                ++depth;
            }
            else if (value == (byte)')')
            {
                --depth;
            }
            else if (value is (byte)'"' or (byte)'\'')
            {
                SkipQuoted(value);
            }
            else if (value == (byte)'/' && cursor < declaration.Offset && bytes[cursor] is (byte)'/' or (byte)'*')
            {
                --cursor;
                SkipTrivia();
            }
            else if (value is (byte)'#' or (byte)'\\')
            {
                throw InvalidPropertySource();
            }
        }

        if (depth != 0)
        {
            throw InvalidPropertySource();
        }

        if (directFieldBegin >= 0)
        {
            // 직접 타입 위치인 경우 매크로에서 타입까지는 공백/주석만 있어야 한다.
            // 다른 매크로·attribute·#include를 중간에 끼워 넣어 출처를 위조하지 못한다.
            SkipTrivia();
            if (cursor != directFieldBegin)
            {
                throw InvalidPropertySource();
            }
        }
        // 전처리 지시문은 CLASS marker처럼 매크로와 선언 사이에 넣지 않는다.
        // 타입의 모든 구문은 Clang이 판단하고, 이 경계는 토큰 출처만 보수적으로 점검한다.
        while (cursor < declaration.Offset)
        {
            byte value = bytes[cursor++];
            if (value is (byte)'"' or (byte)'\'')
            {
                SkipQuoted(value);
            }
            else if (value == (byte)'/' && cursor < declaration.Offset && bytes[cursor] is (byte)'/' or (byte)'*')
            {
                --cursor;
                SkipTrivia();
            }
            else if (value == (byte)'#' || (value == (byte)'\\' && cursor < declaration.Offset &&
                     bytes[cursor] is (byte)'\r' or (byte)'\n'))
            {
                throw InvalidPropertySource();
            }
        }

        void SkipQuoted(byte quote)
        {
            if (quote == (byte)'"' && cursor >= 2 && bytes[cursor - 2] == (byte)'R')
            {
                throw InvalidPropertySource();
            }

            while (cursor < declaration.Offset)
            {
                byte next = bytes[cursor++];
                if (next == quote)
                {
                    return;
                }

                if (next == (byte)'\\')
                {
                    if (cursor >= declaration.Offset || bytes[cursor] is (byte)'\r' or (byte)'\n')
                    {
                        throw InvalidPropertySource();
                    }

                    ++cursor;
                }
                else if (next is (byte)'\r' or (byte)'\n')
                {
                    throw InvalidPropertySource();
                }
            }

            throw InvalidPropertySource();
        }

        void SkipTrivia()
        {
            while (cursor < declaration.Offset)
            {
                byte value = bytes[cursor];
                if (value is (byte)' ' or (byte)'\t' or (byte)'\r' or (byte)'\n' or (byte)'\f' or (byte)'\v')
                {
                    ++cursor;
                    continue;
                }

                if (value != (byte)'/' || cursor + 1 >= declaration.Offset)
                {
                    return;
                }

                if (bytes[cursor + 1] == (byte)'/')
                {
                    cursor += 2;
                    while (cursor < declaration.Offset && bytes[cursor] is not ((byte)'\r' or (byte)'\n'))
                    {
                        RejectSplice();
                        ++cursor;
                    }
                }
                else if (bytes[cursor + 1] == (byte)'*')
                {
                    cursor += 2;
                    while (cursor + 1 < declaration.Offset && !(bytes[cursor] == (byte)'*' && bytes[cursor + 1] == (byte)'/'))
                    {
                        RejectSplice();
                        ++cursor;
                    }

                    if (cursor + 1 >= declaration.Offset)
                    {
                        throw InvalidPropertySource();
                    }

                    cursor += 2;
                }
                else
                {
                    return;
                }
            }
        }

        void RejectSplice()
        {
            if (bytes[cursor] == (byte)'\\' && cursor + 1 < declaration.Offset &&
                bytes[cursor + 1] is (byte)'\r' or (byte)'\n')
            {
                throw InvalidPropertySource();
            }
        }
    }

    private static bool TryExpansion(JsonElement node, out string? file, out int offset)
    {
        file = null;
        offset = -1;
        if (!node.TryGetProperty("range", out JsonElement range) ||
            !range.TryGetProperty("begin", out JsonElement begin) ||
            !begin.TryGetProperty("expansionLoc", out JsonElement expansion) ||
            !expansion.TryGetProperty("offset", out JsonElement value) || !value.TryGetInt32(out offset))
        {
            return false;
        }

        string path = Text(expansion, "file");
        if (path.Length == 0 || !File.Exists(path))
        {
            return false;
        }

        file = path;
        return true;
    }

    private static MetadataException InvalidPropertySource() => new(
        "ROCKS_PROPERTY must be a direct prefix on one field in the same source file",
        hint: "Write ROCKS_PROPERTY(...) immediately before the field declaration, with no wrapper macro or intervening preprocessor directive.");

    // 두 Clang 계열이 실제로 출력하는 location 변형과 위조 사례를 프로세스에 독립적으로 검증한다.
    internal void VerifyPropertySourceForTest(JsonElement field)
    {
        IndexLocations(field);
        VerifyPropertySource(field);
    }

    private void VerifyGeneratedBodySource(JsonElement node, string? ownerFile)
    {
        // Clang의 expansion location과 실제 원문을 모두 확인하여 사용자가 수동으로 붙인
        // annotation이나 다른 매크로의 우회 확장을 GENERATED_BODY로 오인하지 않는다.
        if (ownerFile is null || !File.Exists(ownerFile) ||
            !node.TryGetProperty("loc", out JsonElement location) ||
            !location.TryGetProperty("expansionLoc", out _) ||
            !locations.TryGetValue(Text(node, "id"), out var marker) || marker.File is null ||
            !File.Exists(marker.File) || !CanonicalPath.Equals(ownerFile, marker.File))
        {
            throw InvalidGeneratedBodySource();
        }

        if (!sourceBytes.TryGetValue(marker.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(marker.File);
            sourceBytes.Add(marker.File, bytes);
        }

        ReadOnlySpan<byte> keyword = "ROCKS_GENERATED_BODY"u8;
        int cursor = marker.Offset;
        if (cursor < 0 || cursor > bytes.Length - keyword.Length ||
            !bytes.AsSpan(cursor, keyword.Length).SequenceEqual(keyword))
        {
            throw InvalidGeneratedBodySource();
        }

        cursor += keyword.Length;
        while (cursor < bytes.Length && bytes[cursor] is (byte)' ' or (byte)'\t' or (byte)'\r' or (byte)'\n')
        {
            ++cursor;
        }

        if (cursor >= bytes.Length || bytes[cursor++] != (byte)'(')
        {
            throw InvalidGeneratedBodySource();
        }

        while (cursor < bytes.Length && bytes[cursor] is (byte)' ' or (byte)'\t' or (byte)'\r' or (byte)'\n')
        {
            ++cursor;
        }

        if (cursor >= bytes.Length || bytes[cursor] != (byte)')')
        {
            throw InvalidGeneratedBodySource();
        }
    }

    private static MetadataException InvalidGeneratedBodySource() => new(
        "ROCKS_GENERATED_BODY must be a direct invocation in the same source file as its class",
        hint: "Write ROCKS_GENERATED_BODY() directly inside the reflected class; hand-written annotations and wrapper macros are unsupported.");

    // Clang JSON은 연속 위치의 file을 생략한다. AST parent의 파일이 아니라 JSON 직렬화 순서의
    // 마지막 명시 파일을 사용한다. includedFrom은 호출자이므로 원본 파일로 취급하지 않는다.
    private void IndexLocations(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement child in element.EnumerateArray())
            {
                IndexLocations(child);
            }

            return;
        }

        if (element.ValueKind != JsonValueKind.Object)
        {
            return;
        }

        string id = Text(element, "id");
        foreach (JsonProperty property in element.EnumerateObject())
        {
            if (property.Name == "includedFrom")
            {
                continue;
            }

            if (property.Name == "loc" && id.Length != 0)
            {
                var site = IndexLocation(property.Value);
                locations[id] = site;
            }
            else if (property.Value.ValueKind is JsonValueKind.Object or JsonValueKind.Array)
            {
                IndexLocations(property.Value);
            }
        }
        // range begin/end와 macro spelling/expansion도 파일 생략 상태를 갱신한다.
        if (element.TryGetProperty("offset", out _))
        {
            UpdateSerializedFile(element);
        }
    }

    private (string? File, int Offset) IndexLocation(JsonElement location)
    {
        if (location.ValueKind != JsonValueKind.Object)
        {
            return (null, -1);
        }

        (string? File, int Offset) site = (null, -1);
        foreach (JsonProperty property in location.EnumerateObject())
        {
            if (property.Name is "spellingLoc" or "expansionLoc")
            {
                var nested = IndexLocation(property.Value);
                if (property.Name == "expansionLoc" || site.Offset < 0)
                {
                    site = nested;
                }
            }
        }

        if (location.TryGetProperty("offset", out JsonElement offset) && offset.TryGetInt32(out int value))
        {
            UpdateSerializedFile(location);
            site = (serializedFile, value);
        }

        return site;
    }

    private void UpdateSerializedFile(JsonElement location)
    {
        string file = Text(location, "file");
        string? parent = location.TryGetProperty("includedFrom", out JsonElement included)
            ? Text(included, "file") : null;
        if (file.Length != 0)
        {
            serializedFile = file;
            serializedParent = parent;
        }
        else if (!string.Equals(serializedParent?.Replace('\\', '/'), parent?.Replace('\\', '/'),
                     OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
        {
            // 다른 include 문맥에서 파일명이 생략되면 임의로 이전 파일을 지목하지 않는다.
            serializedFile = null;
            serializedParent = null;
        }
    }

    private static SourceSite UnknownSite() => new("<unknown-location>", 1);

    private void UpdateSite(JsonElement node)
    {
        string id = Text(node, "id");
        if (!locations.TryGetValue(id, out var location) || location.File is null ||
            !File.Exists(location.File) || location.Offset < 0)
        {
            currentSite = UnknownSite();
            return;
        }

        if (!sourceBytes.TryGetValue(location.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(location.File);
            sourceBytes.Add(location.File, bytes);
        }

        if (location.Offset >= bytes.Length)
        {
            currentSite = UnknownSite();
            return;
        }

        int line = 1;
        for (int index = 0; index < location.Offset; ++index)
        {
            if (bytes[index] == (byte)'\n')
            {
                ++line;
            }
        }

        currentSite = new(location.File, line);
    }
}
