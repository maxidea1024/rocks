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
                    if (field.IsReference)
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
                json.WriteEndObject();
            }

            json.WriteEndArray();
            json.WriteEndObject();
        }

        string text = new UTF8Encoding(false, true).GetString(stream.ToArray()).Replace("\r\n", "\n", StringComparison.Ordinal) + "\n";
        return new UTF8Encoding(false, true).GetBytes(text);
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

        List<ClassModel> types = [];
        foreach (JsonElement type in root.GetProperty("types").EnumerateArray())
        {
            RequireKeys(type, TypeKeys, Invalid);
            if (type.GetProperty("kind").GetString() != "class")
            {
                throw Invalid("unsupported type kind");
            }

            string name = Name(type.GetProperty("name"), QualifiedName, "type name");
            string id = Hex(type.GetProperty("id"), Invalid);
            string[] flags = type.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
            if (flags.Any(flag => flag is not ("Abstract" or "WorldOnly")) || flags.Distinct(StringComparer.Ordinal).Count() != flags.Length)
            {
                throw Invalid("unknown or duplicate type flag in " + name);
            }

            string visibility = type.GetProperty("visibility").GetString() ?? "";
            if (visibility is not ("public" or "private"))
            {
                throw Invalid("visibility of " + name);
            }

            if (!type.GetProperty("version").TryGetUInt32(out uint version) || version == 0 || version > int.MaxValue)
            {
                throw Invalid("version of " + name);
            }

            string header = type.GetProperty("header").GetString() ?? "";
            if (header.Length == 0 || header.IndexOfAny(['\0', '\r', '\n', '"', '\\', ':', '<', '>']) >= 0 || header.StartsWith('/') ||
                header.Split('/').Any(part => part.Length == 0 || part is "." or "..") || !header.EndsWith(".h", StringComparison.Ordinal))
            {
                throw Invalid($"header spelling of {name}: {header}");
            }

            ClassModel model = new()
            {
                Name = name,
                Id = id,
                Site = new(descriptorPath, 1),
                Version = version,
                Base = Name(type.GetProperty("base"), QualifiedName, "base of " + name),
                Abstract = flags.Contains("Abstract"),
                WorldOnly = flags.Contains("WorldOnly"),
                Header = header,
                Private = visibility == "private",
                Imported = true,
                Module = module
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
                RequireKeys(value, reference ? ["kind", "type"] : ["kind"], Invalid);
                string kind = Name(value.GetProperty("kind"), Identifier, "field value kind");
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

                string? referenceId = null;
                if (reference)
                {
                    string target = value.GetProperty("type").GetString() ?? throw Invalid("reference target");
                    referenceId = target.Length == 0 ? null : Hex(value.GetProperty("type"), Invalid);
                }

                string fieldName = Name(field.GetProperty("name"), Identifier, "field name");
                // toolName은 실효 도구 이름이다(지정하지 않았으면 필드 이름).
                string toolName = Name(field.GetProperty("toolName"), Identifier, "toolName of " + fieldName);

                fields.Add(new FieldModel
                {
                    Id = Hex(field.GetProperty("id"), Invalid),
                    Site = new(descriptorPath, 1),
                    Name = fieldName,
                    Owner = Name(field.GetProperty("owner"), QualifiedName, "field owner"),
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
