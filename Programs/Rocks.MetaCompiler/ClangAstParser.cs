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

    // 강한 객체 참조를 담는 템플릿이다(StructRefArray는 참조 struct 원소의 목록이다 — Docs/Design/ReflectionFoundations.ko.md
    // 5.6절, ObjectRefMap은 키 → 강한 참조 Map이다 — ReflectionExtensions.ko.md 7.4절).
    private static readonly HashSet<string> StrongReferenceTemplates = new(StringComparer.Ordinal)
    {
        "rocks::ObjectRef", "rocks::RootRef", "rocks::ObjectRefArray", "rocks::StructRefArray", "rocks::ObjectRefMap"
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
                    Transient = classSpec?.Transient ?? false,
                    Deprecated = classSpec?.Deprecated ?? false,
                    WithinSpelling = classSpec?.Within ?? "",
                    Header = owner.Include,
                    Private = owner.Private,
                    IsStruct = isStruct,
                    Atomic = structSpec?.Atomic ?? false,
                    // 기본 도구 이름(한정하지 않은 C++ 이름)과 같은 Name은 지정하지 않은 것과 같다.
                    ToolName = (classSpec?.ToolName ?? structSpec!.ToolName) is string toolName && toolName != TypeToolName.Default(qualified) ? toolName : "",
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
}
