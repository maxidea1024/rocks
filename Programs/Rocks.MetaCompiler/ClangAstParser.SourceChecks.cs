using System.Globalization;
using System.Text.Json;

namespace Rocks.MetaCompiler;

// ClangAstParser 중 원문 위치 검사다: 마커 결합(BindPrefixMarkers·VerifyPrefixSource), annotation 배치
// (ValidateAnnotationPlacement), PROPERTY 원문 검사(VerifyPropertySource), 실패한 첫 Clang 실행의 필드
// 찾기(FindFieldAt), ROCKS_GENERATED_BODY 원문 검사(VerifyGeneratedBodySource).
internal sealed partial class ClangAstParser
{
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

    // 실패한 첫 Clang 실행에서 찾은 필드다(CompilerDriver의 rocks::Array 인스턴스화 실패 변환). Symbol은 정규 이름 owner::name,
    // Spelling은 desugar 철자, StrongPath는 강한 참조 경로(PROPERTY는 배열 원소, NATIVE는 필드 형식 전체 — 없으면 null)다.
    internal sealed record FailedArrayField(string Symbol, string Spelling, bool Property, bool Native, SourceSite Site, string? StrongPath);

    // 실패한 첫 Clang 실행의 AST(오류가 있어도 stdout에 나온다)에서 이름 위치가 file의 (line, column)인 필드를 찾는다
    // (Docs/Design/ReflectionFoundations.ko.md 5.6절). PROPERTY·NATIVE annotation이 있는 필드만 돌려주고 없으면 null이다.
    // column은 Clang처럼 1부터 센 줄 안 byte 위치이며 파일 offset으로 한 번 바꿔 비교한다. 같은 AST로 레코드 색인을 만들어
    // NATIVE 재귀 검사(RMC2205)와 같은 규칙으로 원소의 강한 참조 경로를 계산한다 — 경로가 없으면 Array 실패의 원인이 참조가
    // 아니므로(이동·소멸 noexcept 위반 등) 호출자가 Clang 원문을 유지한다.
    internal FailedArrayField? FindFieldAt(string json, string file, int line, int column)
    {
        if (!File.Exists(file) || line < 1 || column < 1)
        {
            return null;
        }

        byte[] target = File.ReadAllBytes(file);
        int lineStart = 0;
        for (int current = 1; current < line; ++current)
        {
            int next = Array.IndexOf(target, (byte)'\n', lineStart);
            if (next < 0)
            {
                return null;
            }

            lineStart = next + 1;
        }

        int offset = lineStart + column - 1;
        using JsonDocument document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 2048 });
        IndexLocations(document.RootElement);
        IndexRecords(document.RootElement, []);
        Dictionary<string, bool> sameFile = new(StringComparer.Ordinal);
        bool Matches(string candidate)
        {
            if (!sameFile.TryGetValue(candidate, out bool same))
            {
                same = File.Exists(candidate) && CanonicalPath.Equals(candidate, file);
                sameFile.Add(candidate, same);
            }

            return same;
        }

        FailedArrayField? Search(JsonElement node, string[] scope)
        {
            string kind = Text(node, "kind");
            string name = Text(node, "name");
            if (kind == "FieldDecl" && locations.TryGetValue(Text(node, "id"), out var location) && location.File is not null &&
                location.Offset == offset && Matches(location.File))
            {
                bool native = Annotation(node, "rocks.field.native") is not null;
                bool property = HasFieldAnnotation(node);
                if (!native && !property)
                {
                    return null;
                }

                string spelling = TypeSpelling(node);
                string checkedType = !native && ArrayRules.Split(spelling) is (_, string element) ? element : spelling;
                return new(string.Join("::", scope.Append(name)), spelling, property, native, new SourceSite(location.File, line),
                    StrongReferencePath(checkedType, []));
            }

            // 이름 표기는 레코드 색인(IndexRecords)과 같다(익명 namespace는 "(anonymous namespace)").
            string[] inner = kind == "NamespaceDecl" ? [.. scope, name.Length == 0 ? "(anonymous namespace)" : name]
                : kind == "CXXRecordDecl" && name.Length != 0 ? [.. scope, name] : scope;
            foreach (JsonElement child in Children(node))
            {
                if (Search(child, inner) is { } result)
                {
                    return result;
                }
            }

            return null;
        }

        return Search(document.RootElement, []);
    }

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
}
