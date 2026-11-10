using System.Globalization;
using System.Numerics;
using System.Text.Json;

namespace Rocks.MetaCompiler;

internal sealed class ClangAstParser(string inputFile)
{
    private readonly Dictionary<string, ClassModel> classes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, object?[]> prefixClasses = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> identifiers = new(StringComparer.Ordinal);

    private SourceSite currentSite = new(inputFile, 1);
    private readonly Dictionary<string, byte[]> sourceBytes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, (string? File, int Offset)> locations = new(StringComparer.Ordinal);
    private string? serializedFile;
    private string? serializedParent;

    public SchemaModel Parse(string json)
    {
        using JsonDocument document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 2048 });
        IndexLocations(document.RootElement);
        try { BindPrefixMarkers(document.RootElement); ValidateAnnotationPlacement(document.RootElement, false); currentSite = UnknownSite(); Visit(document.RootElement, [], false, false); }
        catch (MetadataException error) { throw error.At(currentSite); }
        if (classes.Count == 0) throw new MetadataException("no ROCKS_CLASS/ROCKS_REFLECT_TYPE definitions found in the translation unit");
        return SchemaValidator.Validate(classes);
    }

    private static IEnumerable<JsonElement> Children(JsonElement node) =>
        node.TryGetProperty("inner", out JsonElement inner) ? inner.EnumerateArray() : [];
    private static string Text(JsonElement node, string property) =>
        node.TryGetProperty(property, out JsonElement value) ? value.GetString() ?? "" : "";
    private static bool Flag(JsonElement node, string property) =>
        node.TryGetProperty(property, out JsonElement value) && value.ValueKind == JsonValueKind.True;
    private static string TypeSpelling(JsonElement node)
    {
        JsonElement type = node.GetProperty("type");
        string resolved = Text(type, "desugaredQualType");
        return resolved.Length != 0 ? resolved : Text(type, "qualType");
    }

    private static object? Constant(JsonElement node)
    {
        string kind = Text(node, "kind");
        if (kind == "ConstantExpr" && BigInteger.TryParse(Text(node, "value"), NumberStyles.Integer,
                CultureInfo.InvariantCulture, out BigInteger evaluated)) return evaluated;
        if (kind == "StringLiteral") return ClangLiteral.Decode(Text(node, "value"));
        if (kind == "IntegerLiteral") return BigInteger.Parse(Text(node, "value"), CultureInfo.InvariantCulture);
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
            if (Text(child, "kind") != "AnnotateAttr") continue;
            object?[] arguments = Children(child).Select(Constant).ToArray();
            if (arguments.Length == 0 || arguments[0] is not string name || name != marker) continue;
            if (result is not null) throw new MetadataException($"duplicate {marker} annotation on {Text(node, "name")}");
            result = arguments[1..];
        }
        return result;
    }

    private string Identifier(object? value, string symbol)
    {
        if (value is not string literal) throw new MetadataException($"{symbol}: stable ID must be a GUID string literal");
        if (!Guid.TryParse(literal, out Guid guid) || guid == Guid.Empty)
            throw new MetadataException($"{symbol}: invalid or zero stable GUID {literal}");
        string id = guid.ToString("N", CultureInfo.InvariantCulture);
        if (!identifiers.TryAdd(id, symbol))
            throw new MetadataException($"{symbol}: duplicate stable ID already used by {identifiers[id]}");
        return id;
    }

    private void Visit(JsonElement node, string[] scope, bool anonymous, bool templated)
    {
        string kind = Text(node, "kind");
        if (kind == "NamespaceDecl")
        {
            string name = Text(node, "name");
            foreach (JsonElement child in Children(node))
                Visit(child, name.Length == 0 ? scope : [.. scope, name], anonymous || name.Length == 0, templated);
            return;
        }
        if (kind is "ClassTemplateDecl" or "ClassTemplateSpecializationDecl") templated = true;
        if (kind == "CXXRecordDecl" && Flag(node, "completeDefinition"))
        {
            string name = Text(node, "name");
            string qualified = string.Join("::", scope.Append(name));
            object?[]? mark = Annotation(node, "rocks.type");
            if (prefixClasses.TryGetValue(Text(node, "id"), out object?[]? prefix)) mark = prefix;
            if (mark is not null)
            {
                UpdateSite(node);
                if (anonymous || templated || name.Length == 0 || mark.Length is < 2 or > 3)
                    throw new MetadataException($"{qualified}: anonymous/template/invalid reflected type is unsupported");
                string id = Identifier(mark[0], qualified);
                if (mark[1] is not BigInteger version || version < 1 || version > uint.MaxValue)
                    throw new MetadataException($"{qualified}: schema version must be 1..UINT32_MAX");
                JsonElement[] bases = node.TryGetProperty("bases", out JsonElement baseArray) ? baseArray.EnumerateArray().ToArray() : [];
                if (bases.Length != 1 || Text(bases[0], "access") != "public" || Flag(bases[0], "isVirtual"))
                    throw new MetadataException($"{qualified}: exactly one public nonvirtual Object/reflected base is required");
                bool abstractType = node.TryGetProperty("definitionData", out JsonElement definition) && Flag(definition, "isAbstract");
                bool explicitAbstract = Specifiers.Class(Payload(mark, 2, qualified), qualified);
                ClassModel model = new() { Name = qualified, Id = id, Site = currentSite, Version = (uint)version,
                    Base = TypeSpelling(bases[0]), Abstract = abstractType || explicitAbstract };
                JsonElement[] members = Children(node).ToArray();
                bool generatedBody = members.Any(member => Annotation(member, "rocks.generated") is not null);
                string access = Text(node, "tagUsed") == "class" ? "private" : "public";
                foreach (JsonElement member in members)
                {
                    UpdateSite(member);
                    string memberKind = Text(member, "kind");
                    string memberName = Text(member, "name");
                    if (memberKind == "AccessSpecDecl") access = Text(member, "access");
                    if (memberKind == "CXXMethodDecl" && memberName == "Trace")
                        throw new MetadataException($"{qualified}: manual Trace conflicts with complete generated field tracing");
                    if (memberKind == "VarDecl" && Annotation(member, "rocks.field") is not null)
                        throw new MetadataException($"{qualified}: static fields cannot carry reflected field annotations");
                    if (memberKind != "FieldDecl") continue;
                    string symbol = qualified + "::" + memberName;
                    object?[]? fieldMark = Annotation(member, "rocks.field");
                    if (fieldMark is null || fieldMark.Length is < 1 or > 2)
                        throw new MetadataException($"{symbol}: every nonstatic field requires a stable field annotation");
                    if (memberName.Length == 0 || Flag(member, "isBitfield"))
                        throw new MetadataException($"{symbol}: only named non-bitfield members are supported");
                    if (access != "public" && !generatedBody)
                        throw new MetadataException($"{symbol}: private/protected PROPERTY requires ROCKS_GENERATED_BODY");
                    model.OwnFields.Add(new FieldModel { Id = Identifier(fieldMark[0], symbol), Site = currentSite, Name = memberName,
                        Owner = qualified, NativeSpelling = TypeSpelling(member), Spec = Specifiers.Property(Payload(fieldMark, 1, symbol), symbol) });
                }
                classes.Add(qualified, model);
            }
            foreach (JsonElement child in Children(node))
                if (Text(child, "kind") is "CXXRecordDecl" or "ClassTemplateDecl")
                    Visit(child, name.Length == 0 ? scope : [.. scope, name], anonymous, templated);
            return;
        }
        foreach (JsonElement child in Children(node)) Visit(child, scope, anonymous, templated);
    }

    // 마커는 바로 뒤의 완전한 class/struct에만 결합한다. lexical scope·실제 파일과
    // 비활성 전처리 영역을 확인하지 않은 채 AST 이웃만 믿으면 다른 타입을 반영할 수 있다.
    private void BindPrefixMarkers(JsonElement parent)
    {
        JsonElement[] children = Children(parent).ToArray();
        for (int index = 0; index < children.Length; ++index)
        {
            JsonElement marker = children[index];
            if (Text(marker, "kind") == "EnumDecl")
            {
                JsonElement[] marked = Children(marker).Where(child =>
                    Annotation(child, "rocks.class.marker") is not null).ToArray();
                if (marked.Length != 0)
                {
                    UpdateSite(marked[0]);
                    string parentKind = Text(parent, "kind");
                    if (marked.Length != 1 || Text(marker, "name").Length != 0 ||
                        parentKind is not ("TranslationUnitDecl" or "NamespaceDecl" or "CXXRecordDecl"))
                        throw new MetadataException("ROCKS_CLASS marker must be a standalone declaration in a namespace or class scope");
                    if (index + 1 >= children.Length || Text(children[index + 1], "kind") != "CXXRecordDecl" ||
                        !Flag(children[index + 1], "completeDefinition") ||
                        Text(children[index + 1], "tagUsed") is not ("class" or "struct"))
                        throw new MetadataException("ROCKS_CLASS must be immediately followed by a complete class or struct definition",
                            hint: "Place ROCKS_CLASS immediately before its class definition; put both inside the same #if block.");
                    JsonElement target = children[index + 1];
                    if (Annotation(target, "rocks.type") is not null)
                        throw new MetadataException("ROCKS_CLASS and ROCKS_REFLECT_TYPE cannot both annotate the same class");
                    VerifyPrefixSource(marked[0], target);
                    if (!prefixClasses.TryAdd(Text(target, "id"), Annotation(marked[0], "rocks.class.marker")!))
                        throw new MetadataException("duplicate ROCKS_CLASS marker for the same class");
                }
            }
            // 다른 AST 항목도 재귀 검사한다. 함수/지역 class의 마커는 무시하지 않고 거부한다.
            BindPrefixMarkers(marker);
        }
    }

    private void VerifyPrefixSource(JsonElement marker, JsonElement target)
    {
        if (!locations.TryGetValue(Text(marker, "id"), out var markLocation) ||
            !locations.TryGetValue(Text(target, "id"), out var targetLocation) ||
            markLocation.File is null || targetLocation.File is null ||
            !File.Exists(markLocation.File) || !File.Exists(targetLocation.File) ||
            !CanonicalPath.Equals(markLocation.File, targetLocation.File))
            throw new MetadataException("ROCKS_CLASS and class must have the same known physical source file",
                hint: "Place the marker and class directly in one source file; macro-generated or ambiguous locations are unsupported.");
        if (!target.TryGetProperty("range", out JsonElement range) ||
            !range.TryGetProperty("begin", out JsonElement begin) ||
            begin.TryGetProperty("expansionLoc", out _) ||
            !begin.TryGetProperty("offset", out JsonElement classOffset) || !classOffset.TryGetInt32(out int end))
            throw new MetadataException("ROCKS_CLASS target location is macro-generated or ambiguous",
                hint: "Write the class keyword directly after ROCKS_CLASS; wrapper macros are unsupported.");
        if (!sourceBytes.TryGetValue(markLocation.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(markLocation.File);
            sourceBytes.Add(markLocation.File, bytes);
        }
        PrefixSourceVerifier.Verify(bytes, markLocation.Offset, end);
    }

    private void ValidateAnnotationPlacement(JsonElement node, bool reflectedOwner)
    {
        string kind = Text(node, "kind");
        UpdateSite(node);
        bool classAnnotation = Annotation(node, "rocks.type") is not null || prefixClasses.ContainsKey(Text(node, "id"));
        bool fieldAnnotation = Annotation(node, "rocks.field") is not null;
        if (classAnnotation && (kind != "CXXRecordDecl" || !Flag(node, "completeDefinition")))
            throw new MetadataException("ROCKS_CLASS must annotate a complete class definition");
        if (fieldAnnotation && (kind != "FieldDecl" || !reflectedOwner))
            throw new MetadataException("ROCKS_PROPERTY must annotate a nonstatic data field owned by a reflected CLASS");
        // method/parameter/local 선언까지 검사한다. 생성에 필요 없는 AST라도 잘못 붙인 annotation을 버리지 않는다.
        bool childOwner = kind == "CXXRecordDecl" ? classAnnotation : reflectedOwner;
        foreach (JsonElement child in Children(node)) ValidateAnnotationPlacement(child, childOwner);
    }
    // Clang JSON은 연속 위치의 file을 생략한다. AST parent의 파일이 아니라 JSON 직렬화 순서의
    // 마지막 명시 파일을 사용한다. includedFrom은 호출자이므로 원본 파일로 취급하지 않는다.
    private void IndexLocations(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement child in element.EnumerateArray()) IndexLocations(child);
            return;
        }
        if (element.ValueKind != JsonValueKind.Object) return;
        string id = Text(element, "id");
        foreach (JsonProperty property in element.EnumerateObject())
        {
            if (property.Name == "includedFrom") continue;
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
        if (element.TryGetProperty("offset", out _)) UpdateSerializedFile(element);
    }

    private (string? File, int Offset) IndexLocation(JsonElement location)
    {
        if (location.ValueKind != JsonValueKind.Object) return (null, -1);
        (string? File, int Offset) site = (null, -1);
        foreach (JsonProperty property in location.EnumerateObject())
        {
            if (property.Name is "spellingLoc" or "expansionLoc")
            {
                var nested = IndexLocation(property.Value);
                if (property.Name == "expansionLoc" || site.Offset < 0) site = nested;
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
            if (bytes[index] == (byte)'\n') ++line;
        currentSite = new(location.File, line);
    }
    private static string Payload(object?[] mark, int index, string symbol)
    {
        if (mark.Length <= index) return ""; // 기존 annotation 인자 형식도 허용한다.
        return mark[index] as string ?? throw new MetadataException($"{symbol}: specifier payload must be a string");
    }
}
