using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.MetaCompiler;

// 모듈 descriptor(<Module>.rocksmeta.json)다. 의존 모듈이 상속 필드를 펼치고 참조 대상 ID를 해석하는 데
// 필요한 것을 모두 담는다(Docs/Design/ReflectionFoundations.ko.md 1.4절). 읽기는 모르는 속성을 거부하고
// schema 1만 받는다. 쓰기는 결정적이다(타입 이름 순, 필드는 생성 순서, 고정 서식, LF).
internal static class ModuleDescriptor
{
    private static readonly string[] TypeKeys =
        ["kind", "name", "id", "version", "flags", "base", "header", "visibility", "fields"];

    // struct 항목: base 대신 생성 형질 flags(ContainsStrongReference·RequiresWorldOnlyOwner)와 binds(Binds 실제
    // 타입의 Clang 정규 철자, 미러가 아니면 "")다(Docs/Design/ReflectionFoundations.ko.md 1.4·4.6절).
    private static readonly string[] StructKeys =
        ["kind", "name", "id", "version", "header", "visibility", "flags", "binds", "fields"];

    // enum 항목: 필드·base 대신 underlying·열거자다(Docs/Design/ReflectionFoundations.ko.md 3.4절).
    private static readonly string[] EnumKeys =
        ["kind", "name", "id", "header", "visibility", "underlying", "flags", "toolPrefix", "values"];

    private static readonly string[] EnumeratorKeys = ["name", "toolName", "displayName", "value", "flags"];

    private static readonly string[] FieldKeys =
        ["owner", "name", "id", "fieldKind", "value", "element", "flags", "toolName", "category", "displayName", "maxElements"];

    public static byte[] Write(string module, IEnumerable<ClassModel> types)
    {
        using MemoryStream stream = new();
        // 줄바꿈 기본값은 플랫폼마다 다르므로(Windows CRLF) LF로 고정한다.
        using (Utf8JsonWriter json = new(stream, new JsonWriterOptions { Indented = true, NewLine = "\n", Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
        {
            json.WriteStartObject();
            json.WriteNumber("schema", 1);
            json.WriteString("module", module);
            json.WriteStartArray("types");
            foreach (ClassModel type in types.OrderBy(type => type.Name, StringComparer.Ordinal))
            {
                if (type.IsEnum)
                {
                    WriteEnum(json, type);
                    continue;
                }

                if (type.IsStruct)
                {
                    WriteStruct(json, type);
                    continue;
                }

                json.WriteStartObject();
                json.WriteString("kind", "class");
                json.WriteString("name", type.Name);
                json.WriteString("id", type.Id);
                json.WriteNumber("version", type.Version);
                json.WriteStartArray("flags");
                if (type.Abstract)
                {
                    json.WriteStringValue("Abstract");
                }

                if (type.WorldOnly)
                {
                    json.WriteStringValue("WorldOnly");
                }

                json.WriteEndArray();
                json.WriteString("base", type.Base);
                json.WriteString("header", type.Header);
                json.WriteString("visibility", type.Private ? "private" : "public");
                WriteFields(json, type);
                json.WriteEndObject();
            }

            json.WriteEndArray();
            json.WriteEndObject();
        }

        string text = new UTF8Encoding(false, true).GetString(stream.ToArray()).Replace("\r\n", "\n", StringComparison.Ordinal) + "\n";
        return new UTF8Encoding(false, true).GetBytes(text);
    }

    private static void WriteStruct(Utf8JsonWriter json, ClassModel type)
    {
        json.WriteStartObject();
        json.WriteString("kind", "struct");
        json.WriteString("name", type.Name);
        json.WriteString("id", type.Id);
        json.WriteNumber("version", type.Version);
        json.WriteString("header", type.Header);
        json.WriteString("visibility", type.Private ? "private" : "public");
        json.WriteStartArray("flags");
        if (type.ContainsStrongReference)
        {
            json.WriteStringValue("ContainsStrongReference");
        }

        if (type.RequiresWorldOnlyOwner)
        {
            json.WriteStringValue("RequiresWorldOnlyOwner");
        }

        json.WriteEndArray();
        json.WriteString("binds", type.Binds);
        WriteFields(json, type);
        json.WriteEndObject();
    }

    // 클래스·struct의 fields 배열이다(클래스는 상속 포함 전체, 생성 순서 그대로).
    private static void WriteFields(Utf8JsonWriter json, ClassModel type)
    {
        json.WriteStartArray("fields");
        foreach (FieldModel field in type.Fields)
        {
            json.WriteStartObject();
            json.WriteString("owner", field.Owner);
            json.WriteString("name", field.Name);
            json.WriteString("id", field.Id);
            json.WriteString("fieldKind", field.IsReference ? "ObjectReference" : "Value");
            json.WriteStartObject("value");
            json.WriteString("kind", field.Kind);
            if (field.HasTargetType)
            {
                json.WriteString("type", field.ReferenceId ?? "");
            }

            json.WriteEndObject();
            json.WriteNull("element");
            json.WriteStartArray("flags");
            if (field.Spec.Transient)
            {
                json.WriteStringValue("Transient");
            }

            if (field.Spec.ReadOnly)
            {
                json.WriteStringValue("ReadOnly");
            }

            if (field.Spec.EditorOnly)
            {
                json.WriteStringValue("EditorOnly");
            }

            if (field.Spec.Required)
            {
                json.WriteStringValue("Required");
            }

            json.WriteEndArray();
            json.WriteString("toolName", field.EffectiveToolName);
            json.WriteString("category", field.Spec.Category);
            json.WriteString("displayName", field.Spec.DisplayName);
            json.WriteNumber("maxElements", 0);
            json.WriteEndObject();
        }

        json.WriteEndArray();
    }

    private static void WriteEnum(Utf8JsonWriter json, ClassModel type)
    {
        json.WriteStartObject();
        json.WriteString("kind", "enum");
        json.WriteString("name", type.Name);
        json.WriteString("id", type.Id);
        json.WriteString("header", type.Header);
        json.WriteString("visibility", type.Private ? "private" : "public");
        json.WriteString("underlying", type.Underlying);
        json.WriteStartArray("flags");
        if (type.FlagsEnum)
        {
            json.WriteStringValue("Flags");
        }

        json.WriteEndArray();
        json.WriteString("toolPrefix", type.ToolPrefix);
        json.WriteStartArray("values");
        foreach (EnumeratorModel value in type.Enumerators)
        {
            json.WriteStartObject();
            json.WriteString("name", value.Name);
            json.WriteString("toolName", value.EffectiveToolName);
            json.WriteString("displayName", value.DisplayName);
            json.WriteNumber("value", value.Value);
            json.WriteStartArray("flags");
            if (value.Hidden)
            {
                json.WriteStringValue("Hidden");
            }

            json.WriteEndArray();
            json.WriteEndObject();
        }

        json.WriteEndArray();
        json.WriteEndObject();
    }

    // descriptor 하나를 읽어 import 모델을 돌려준다. descriptorPath는 진단용이다. 이름·철자는 생성 C++과 경로에
    // 그대로 들어가므로 문법을 검사하고, 형식 오류는 모두 RMC1000이다(SDK descriptor도 이 경로의 입력이다).
    public static (string Module, List<ClassModel> Types) Read(string descriptorPath)
    {
        byte[] bytes = BoundedRead(descriptorPath);
        MetadataException Invalid(string reason) => new($"invalid descriptor {descriptorPath}: {reason}", "RMC1000",
            "Regenerate the dependency module's metadata; descriptors are build outputs and must not be edited.");
        try
        {
            return Parse(descriptorPath, bytes, Invalid);
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or KeyNotFoundException or FormatException or OverflowException)
        {
            throw Invalid(error.Message);
        }
    }

    private static (string Module, List<ClassModel> Types) Parse(string descriptorPath, byte[] bytes, Func<string, MetadataException> Invalid)
    {
        // 같은 속성이 두 번 있으면(마지막 값이 이기는 모호한 입력) 거부한다.
        using JsonDocument document = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 16, AllowDuplicateProperties = false });
        JsonElement root = document.RootElement;
        RequireKeys(root, ["schema", "module", "types"], Invalid);
        if (!root.GetProperty("schema").TryGetInt32(out int schema) || schema != 1)
        {
            throw Invalid("unsupported schema");
        }

        string module = root.GetProperty("module").GetString() ?? "";
        if (!ModuleName.IsMatch(module))
        {
            throw Invalid("module name must be an ASCII identifier: " + module);
        }

        string Name(JsonElement element, Regex pattern, string what)
        {
            string text = element.GetString() ?? "";
            return pattern.IsMatch(text) ? text : throw Invalid($"invalid {what}: {text}");
        }

        string Header(JsonElement type, string name)
        {
            string header = type.GetProperty("header").GetString() ?? "";
            if (header.Length == 0 || header.IndexOfAny(['\0', '\r', '\n', '"', '\\', ':', '<', '>']) >= 0 || header.StartsWith('/') ||
                header.Split('/').Any(part => part.Length == 0 || part is "." or "..") || !header.EndsWith(".h", StringComparison.Ordinal))
            {
                throw Invalid($"header spelling of {name}: {header}");
            }

            return header;
        }

        bool PrivateVisibility(JsonElement type, string name) => type.GetProperty("visibility").GetString() switch
        {
            "public" => false,
            "private" => true,
            _ => throw Invalid("visibility of " + name)
        };

        List<ClassModel> types = [];
        foreach (JsonElement type in root.GetProperty("types").EnumerateArray())
        {
            if (type.ValueKind != JsonValueKind.Object || !type.TryGetProperty("kind", out JsonElement typeKind))
            {
                throw Invalid("type entry");
            }

            if (typeKind.GetString() == "enum")
            {
                RequireKeys(type, EnumKeys, Invalid);
                string enumName = Name(type.GetProperty("name"), QualifiedName, "type name");
                types.Add(ParseEnum(type, enumName, Hex(type.GetProperty("id"), Invalid), Header(type, enumName),
                    PrivateVisibility(type, enumName), module, descriptorPath, Invalid));
                continue;
            }

            bool isStruct = typeKind.GetString() == "struct";
            RequireKeys(type, isStruct ? StructKeys : TypeKeys, Invalid);
            if (!isStruct && typeKind.GetString() != "class")
            {
                throw Invalid("unsupported type kind");
            }

            string name = Name(type.GetProperty("name"), QualifiedName, "type name");
            string id = Hex(type.GetProperty("id"), Invalid);
            string[] flags = type.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
            bool KnownFlag(string flag) => isStruct ? flag is "ContainsStrongReference" or "RequiresWorldOnlyOwner" : flag is "Abstract" or "WorldOnly";
            if (flags.Any(flag => !KnownFlag(flag)) || flags.Distinct(StringComparer.Ordinal).Count() != flags.Length)
            {
                throw Invalid("unknown or duplicate type flag in " + name);
            }

            bool isPrivate = PrivateVisibility(type, name);
            if (!type.GetProperty("version").TryGetUInt32(out uint version) || version == 0 || version > int.MaxValue)
            {
                throw Invalid("version of " + name);
            }

            // binds는 Binds 실제 타입의 Clang 정규 철자(템플릿 인자 포함)이거나 ""다. 생성 C++에 그대로 들어가므로 철자
            // 문법을 검사한다(모듈·중복·리플렉션 타입 결합 규칙은 LinkImported가 RMC2004로 본다).
            string binds = "";
            if (isStruct)
            {
                JsonElement bindsValue = type.GetProperty("binds");
                binds = bindsValue.ValueKind == JsonValueKind.String ? bindsValue.GetString() ?? "" : throw Invalid("binds of " + name);
                if (binds.Length != 0 && (!StructRules.IsBindsSpelling(binds) || binds.StartsWith("::", StringComparison.Ordinal)))
                {
                    throw Invalid("binds of " + name + ": " + binds);
                }
            }

            string header = Header(type, name);
            ClassModel model = new()
            {
                Name = name,
                Id = id,
                Site = new(descriptorPath, 1),
                Version = version,
                Base = isStruct ? "" : Name(type.GetProperty("base"), QualifiedName, "base of " + name),
                Abstract = flags.Contains("Abstract"),
                WorldOnly = flags.Contains("WorldOnly"),
                Header = header,
                Private = isPrivate,
                Imported = true,
                Module = module,
                IsStruct = isStruct,
                ContainsStrongReference = flags.Contains("ContainsStrongReference"),
                RequiresWorldOnlyOwner = flags.Contains("RequiresWorldOnlyOwner"),
                Binds = binds
            };
            List<FieldModel> fields = [];
            foreach (JsonElement field in type.GetProperty("fields").EnumerateArray())
            {
                RequireKeys(field, FieldKeys, Invalid);
                bool reference = field.GetProperty("fieldKind").GetString() switch
                {
                    "ObjectReference" => true,
                    "Value" => false,
                    _ => throw Invalid("fieldKind")
                };
                JsonElement value = field.GetProperty("value");
                if (value.ValueKind != JsonValueKind.Object)
                {
                    throw Invalid("field value");
                }

                // 값 종류는 MetaCompiler가 만드는 종류만 받는다(그 밖은 생성 코드가 컴파일되어도 런타임 등록에서 실패한다).
                string kind = Name(value.GetProperty("kind"), Identifier, "field value kind");
                if (!SchemaValidator.IsSupportedValueKind(kind))
                {
                    throw Invalid("field value kind " + kind);
                }

                bool targetType = FieldModel.KindHasTargetType(kind);
                RequireKeys(value, targetType ? ["kind", "type"] : ["kind"], Invalid);
                if (reference != (kind == "ObjectReference"))
                {
                    throw Invalid("fieldKind and value kind disagree");
                }

                if (field.GetProperty("element").ValueKind != JsonValueKind.Null || !field.GetProperty("maxElements").TryGetUInt32(out uint maxElements) || maxElements != 0)
                {
                    throw Invalid("array fields are not supported by schema 1");
                }

                string[] fieldFlags = field.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
                if (fieldFlags.Any(flag => flag is not ("Transient" or "ReadOnly" or "EditorOnly" or "Required")) ||
                    fieldFlags.Distinct(StringComparer.Ordinal).Count() != fieldFlags.Length)
                {
                    throw Invalid("unknown or duplicate field flag");
                }

                // Required는 강한·소프트 참조 필드에만 쓴다(원본 선언의 RMC2202와 같은 규칙).
                if (!FieldModel.KindAcceptsRequired(kind) && fieldFlags.Contains("Required", StringComparer.Ordinal))
                {
                    throw Invalid("Required on a field that is not an object or soft object reference");
                }

                string? referenceId = null;
                if (targetType)
                {
                    string target = value.GetProperty("type").GetString() ?? throw Invalid("reference target");
                    // Enum·Struct 필드는 대상 ID가 있어야 한다(참조의 ""는 Object다).
                    referenceId = target.Length == 0 && !FieldModel.KindRequiresTargetId(kind) ? null : Hex(value.GetProperty("type"), Invalid);
                }

                string fieldName = Name(field.GetProperty("name"), Identifier, "field name");
                // toolName은 실효 도구 이름이다(지정하지 않았으면 필드 이름).
                string toolName = Name(field.GetProperty("toolName"), Identifier, "toolName of " + fieldName);
                string owner = Name(field.GetProperty("owner"), QualifiedName, "field owner");
                // struct는 상속이 없으므로 모든 필드의 owner가 그 struct다.
                if (isStruct && owner != name)
                {
                    throw Invalid($"owner of struct field {name}::{fieldName}");
                }

                fields.Add(new FieldModel
                {
                    Id = Hex(field.GetProperty("id"), Invalid),
                    Site = new(descriptorPath, 1),
                    Name = fieldName,
                    Owner = owner,
                    NativeSpelling = "",
                    Spec = new(fieldFlags.Contains("Transient"), fieldFlags.Contains("ReadOnly"),
                        field.GetProperty("category").GetString() ?? throw Invalid("category"),
                        field.GetProperty("displayName").GetString() ?? throw Invalid("displayName"))
                    {
                        EditorOnly = fieldFlags.Contains("EditorOnly"),
                        Required = fieldFlags.Contains("Required"),
                        ToolName = toolName == fieldName ? "" : toolName
                    },
                    Kind = kind,
                    ReferenceId = referenceId
                });
            }

            model.Fields = fields;
            types.Add(model);
        }

        return (module, types);
    }

    // enum 항목 하나를 읽는다. 런타임 RegisterEnum과 같은 규칙(underlying 범위·별칭·Flags의 음수·도구 이름 문자 집합과
    // 접두 유무를 합친 유일성)을 검사하고 어기면 RMC1000이다.
    private static ClassModel ParseEnum(JsonElement type, string name, string id, string header, bool isPrivate, string module,
        string descriptorPath, Func<string, MetadataException> Invalid)
    {
        string underlying = type.GetProperty("underlying").GetString() ?? "";
        (long minimum, long maximum) = EnumRules.Range(underlying) ?? throw Invalid("underlying of " + name);
        string[] flags = type.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
        if (flags.Any(flag => flag != "Flags") || flags.Length > 1)
        {
            throw Invalid("unknown or duplicate enum flag in " + name);
        }

        bool flagsEnum = flags.Length == 1;
        string prefix = type.GetProperty("toolPrefix").GetString() ?? throw Invalid("toolPrefix of " + name);
        if (prefix.Length != 0 && !Specifiers.IsEnumToolName(prefix))
        {
            throw Invalid("toolPrefix of " + name);
        }

        List<EnumeratorModel> values = [];
        foreach (JsonElement entry in type.GetProperty("values").EnumerateArray())
        {
            // 충돌 검사가 열거자 수의 제곱이므로 상한을 먼저 본다.
            if (values.Count == EnumRules.MaxEnumerators)
            {
                throw Invalid("enumerator count of " + name);
            }

            RequireKeys(entry, EnumeratorKeys, Invalid);
            string valueName = entry.GetProperty("name").GetString() ?? "";
            string toolName = entry.GetProperty("toolName").GetString() ?? "";
            string[] valueFlags = entry.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
            if (!Specifiers.IsAsciiIdentifier(valueName) || !Specifiers.IsEnumToolName(toolName) ||
                !entry.GetProperty("value").TryGetInt64(out long value) || value < minimum || value > maximum || (flagsEnum && value < 0) ||
                valueFlags.Any(flag => flag != "Hidden") || valueFlags.Length > 1)
            {
                throw Invalid($"enumerator {valueName} of {name}");
            }

            string displayName = entry.GetProperty("displayName").GetString() ?? throw Invalid($"displayName of {valueName} in {name}");
            EnumeratorModel model = new(valueName, toolName == valueName ? "" : toolName, displayName, value, valueFlags.Length == 1);
            if (values.Any(other => other.Value == value || EnumRules.ToolNamesCollide(other.EffectiveToolName, model.EffectiveToolName, prefix)))
            {
                throw Invalid($"duplicate enumerator value or tool name {valueName} in {name}");
            }

            values.Add(model);
        }

        if (values.Count == 0)
        {
            throw Invalid("enumerator count of " + name);
        }

        return new ClassModel
        {
            Name = name,
            Id = id,
            Site = new(descriptorPath, 1),
            Version = 1,
            Base = "",
            Abstract = false,
            Header = header,
            Private = isPrivate,
            Imported = true,
            Module = module,
            IsEnum = true,
            Underlying = underlying,
            FlagsEnum = flagsEnum,
            ToolPrefix = prefix,
            Enumerators = values
        };
    }

    // .NET의 $는 끝의 \n 하나를 허용하므로 문자열 끝은 \z로 맞춘다.
    private static readonly Regex ModuleName = new(@"^[A-Za-z][A-Za-z0-9_]*\z", RegexOptions.CultureInvariant);

    private static readonly Regex Identifier = new(@"^[A-Za-z_][A-Za-z0-9_]*\z", RegexOptions.CultureInvariant);

    private static readonly Regex QualifiedName = new(@"^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*\z", RegexOptions.CultureInvariant);

    private static void RequireKeys(JsonElement element, string[] keys, Func<string, MetadataException> invalid)
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            throw invalid("expected object");
        }

        HashSet<string> present = element.EnumerateObject().Select(property => property.Name).ToHashSet(StringComparer.Ordinal);
        if (!present.SetEquals(keys))
        {
            throw invalid("unexpected or missing properties: " + string.Join(",", present.Except(keys).Concat(keys.Except(present))));
        }
    }

    private static string Hex(JsonElement element, Func<string, MetadataException> invalid)
    {
        string text = element.GetString() ?? "";
        if (text.Length != 32 || !text.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f') || text.All(c => c == '0'))
        {
            throw invalid("identifier must be 32 lowercase hex digits and nonzero");
        }

        return text;
    }

    private static byte[] BoundedRead(string path)
    {
        const long Limit = 16L * 1024 * 1024;
        FileInfo info = new(path);
        if (!info.Exists || info.Length > Limit)
        {
            throw new MetadataException("descriptor missing or too large: " + path, "RMC1000");
        }

        return File.ReadAllBytes(path);
    }
}
