using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.MetaCompiler;

// 모듈 descriptor(<Module>.rocksmeta.json)다. 의존 모듈이 상속 필드를 펼치고 참조 대상 ID를 해석하는 데
// 필요한 것을 모두 담는다(Docs/Design/ReflectionFoundations.ko.md 1.4절). 읽기는 모르는 속성을 거부하고
// schema 1만 받는다. 쓰기는 결정적이다(타입 이름 순, 필드는 생성 순서, 고정 서식, LF).
internal static class ModuleDescriptor
{
    // 클래스 항목이다. toolName은 실효 타입 도구 이름(Docs/Design/ReflectionExtensions.ko.md 1.8·5.2절 — 지정하지 않았으면 한정하지
    // 않은 C++ 이름)이다.
    private static readonly string[] TypeKeys =
        ["kind", "name", "id", "version", "flags", "within", "base", "header", "visibility", "toolName", "fields"];

    // struct 항목: base(리플렉션 base struct의 정규 이름, 없으면 "" — 리플렉션 B9k, ReflectionExtensions.ko.md 12절), flags(선언
    // 플래그 Atomic(base에서 상속된 것 포함) 다음 생성 형질 — 쓰기 순서 Atomic → ContainsStrongReference → RequiresWorldOnlyOwner →
    // ContainsReferenceArray)와 binds(Binds 실제 타입의 Clang 정규 철자, 미러가 아니면 "")다. fields는 상속 필드를 포함한 전체다
    // (Docs/Design/ReflectionFoundations.ko.md 1.4·4.6·5.6절, Atomic은 ReflectionExtensions.ko.md 1.8·3.1절).
    private static readonly string[] StructKeys =
        ["kind", "name", "id", "version", "base", "header", "visibility", "flags", "binds", "toolName", "fields"];

    // enum 항목: 필드·base 대신 underlying·열거자다(Docs/Design/ReflectionFoundations.ko.md 3.4절).
    private static readonly string[] EnumKeys =
        ["kind", "name", "id", "header", "visibility", "underlying", "flags", "toolPrefix", "binds", "values"];

    private static readonly string[] EnumeratorKeys = ["name", "toolName", "displayName", "value", "flags"];

    private static readonly string[] FieldKeys =
        ["owner", "name", "id", "fieldKind", "value", "element", "key", "flags", "toolName", "category", "displayName", "maxElements", "bitfield"];

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

                if (type.Transient)
                {
                    json.WriteStringValue("Transient");
                }

                if (type.Deprecated)
                {
                    json.WriteStringValue("Deprecated");
                }

                json.WriteEndArray();
                // 실효 Within(상속 포함 — 리플렉션 B9h2)이다. 없으면 null이다.
                if (type.Within is null)
                {
                    json.WriteNull("within");
                }
                else
                {
                    json.WriteStartObject("within");
                    json.WriteString("name", type.Within);
                    json.WriteString("id", type.WithinId ?? "");
                    json.WriteEndObject();
                }

                json.WriteString("base", type.Base);
                json.WriteString("header", type.Header);
                json.WriteString("visibility", type.Private ? "private" : "public");
                json.WriteString("toolName", type.EffectiveToolName);
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
        // 리플렉션 base struct의 정규 이름이다(없으면 "" — 리플렉션 B9k, 비리플렉션 빈 base는 쓰지 않는다).
        json.WriteString("base", type.Base);
        json.WriteString("header", type.Header);
        json.WriteString("visibility", type.Private ? "private" : "public");
        json.WriteStartArray("flags");
        if (type.Atomic)
        {
            json.WriteStringValue("Atomic");
        }

        if (type.ContainsStrongReference)
        {
            json.WriteStringValue("ContainsStrongReference");
        }

        if (type.RequiresWorldOnlyOwner)
        {
            json.WriteStringValue("RequiresWorldOnlyOwner");
        }

        if (type.ContainsReferenceArray)
        {
            json.WriteStringValue("ContainsReferenceArray");
        }

        json.WriteEndArray();
        json.WriteString("binds", type.Binds);
        json.WriteString("toolName", type.EffectiveToolName);
        WriteFields(json, type);
        json.WriteEndObject();
    }

    // 클래스·struct의 fields 배열이다(클래스는 상속 포함 전체, 생성 순서 그대로). 배열 필드는 value가 {"kind": "Array"}이고
    // 원소 형식이 element(대상 종류일 때만 type), 길이 상한이 maxElements다. Optional 필드(리플렉션 B9e)는 value가
    // {"kind": "Optional"}이고 내부 형식이 element, maxElements는 0이다. Map 필드(리플렉션 B9f2)는 value가 {"kind": "Map"}이고 값
    // 형식이 element, 키 형식이 key(대상 종류일 때만 type), 원소 수 상한이 maxElements이고, Set 필드는 value가 {"kind": "Set"}이고
    // 원소 형식이 element, 상한이 maxElements다(그 밖의 필드는 element null·maxElements 0, Map이 아니면 key null).
    // 배열 컨테이너 종류(Array·ObjectRefArray·StructRefArray)를 적는 칸은 두지 않는다(원소 종류와 원소 struct 형질로 정해진다 —
    // Docs/Design/ReflectionFoundations.ko.md 5.6절. Map 키 형식 칸 key와 다른 이야기다).
    private static void WriteFields(Utf8JsonWriter json, ClassModel type)
    {
        json.WriteStartArray("fields");
        foreach (FieldModel field in type.Fields)
        {
            json.WriteStartObject();
            json.WriteString("owner", field.Owner);
            json.WriteString("name", field.Name);
            json.WriteString("id", field.Id);
            json.WriteString("fieldKind", field.IsObjectReferenceFieldKind ? "ObjectReference" : "Value");
            json.WriteStartObject("value");
            json.WriteString("kind", field.Kind);
            if (!field.HasElement && field.HasTargetType)
            {
                json.WriteString("type", field.ReferenceId ?? "");
            }

            json.WriteEndObject();
            if (field.HasElement)
            {
                json.WriteStartObject("element");
                json.WriteString("kind", field.ElementKind);
                if (field.HasTargetType)
                {
                    json.WriteString("type", field.ReferenceId ?? "");
                }

                json.WriteEndObject();
            }
            else
            {
                json.WriteNull("element");
            }

            if (field.IsMap)
            {
                json.WriteStartObject("key");
                json.WriteString("kind", field.KeyKind);
                if (field.KeyHasTargetType)
                {
                    json.WriteString("type", field.KeyReferenceId ?? "");
                }

                json.WriteEndObject();
            }
            else
            {
                json.WriteNull("key");
            }

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

            // 필드 플래그(리플렉션 B9h1)는 비트 순서로 적는다.
            foreach ((bool set, string flag) in new[] { (field.Spec.DuplicateTransient, "DuplicateTransient"),
                (field.Spec.NonPIEDuplicateTransient, "NonPIEDuplicateTransient"), (field.Spec.SaveGame, "SaveGame"),
                (field.Spec.TextExportTransient, "TextExportTransient") })
            {
                if (set)
                {
                    json.WriteStringValue(flag);
                }
            }

            json.WriteEndArray();
            json.WriteString("toolName", field.EffectiveToolName);
            json.WriteString("category", field.Spec.Category);
            json.WriteString("displayName", field.Spec.DisplayName);
            json.WriteNumber("maxElements", field.IsCounted || field.IsStaticArray ? field.MaxElements : 0);
            // 폭 1 bitfield bool(리플렉션 B9m)이면 참이다: 의존 모듈이 상속 필드 함수표에 base의 생성 접근자를 쓴다.
            json.WriteBoolean("bitfield", field.Bitfield);
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
        // Binds 미러 enum(CurveData CV9)의 실제 enum 정규 철자다(미러가 아니면 "").
        json.WriteString("binds", type.Binds);
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
            // struct는 선언 플래그 Atomic과 생성 형질, 클래스는 Abstract·WorldOnly만 받는다(struct 전용 Atomic이 클래스에
            // 있거나 클래스 플래그가 struct에 있으면 모르는 플래그다).
            bool KnownFlag(string flag) => isStruct ? flag is "Atomic" or "ContainsStrongReference" or "RequiresWorldOnlyOwner" or "ContainsReferenceArray"
                : flag is "Abstract" or "WorldOnly" or "Transient" or "Deprecated";
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
            // toolName은 실효 타입 도구 이름이다(형식은 런타임 등록과 같은 TypeToolName — 모듈·import 범위의 유일성은
            // SchemaValidator가 RMC2203으로 본다).
            string typeToolName = type.GetProperty("toolName").GetString() ?? "";
            if (!TypeToolName.IsValid(typeToolName))
            {
                throw Invalid("toolName of " + name + ": " + typeToolName);
            }

            ClassModel model = new()
            {
                Name = name,
                Id = id,
                Site = new(descriptorPath, 1),
                Version = version,
                Base = isStruct && type.GetProperty("base").GetString() is "" ? "" : Name(type.GetProperty("base"), QualifiedName, "base of " + name),
                Abstract = flags.Contains("Abstract"),
                WorldOnly = flags.Contains("WorldOnly"),
                Transient = flags.Contains("Transient"),
                Deprecated = flags.Contains("Deprecated"),
                Header = header,
                Private = isPrivate,
                Imported = true,
                Module = module,
                IsStruct = isStruct,
                Atomic = flags.Contains("Atomic"),
                ContainsStrongReference = flags.Contains("ContainsStrongReference"),
                RequiresWorldOnlyOwner = flags.Contains("RequiresWorldOnlyOwner"),
                ContainsReferenceArray = flags.Contains("ContainsReferenceArray"),
                Binds = binds,
                ToolName = typeToolName == TypeToolName.Default(name) ? "" : typeToolName
            };
            // 클래스의 실효 Within(리플렉션 B9h2)은 {name, id} 또는 null이다.
            if (!isStruct)
            {
                JsonElement within = type.GetProperty("within");
                if (within.ValueKind == JsonValueKind.Object)
                {
                    model.Within = Name(within.GetProperty("name"), QualifiedName, "within of " + name);
                    model.WithinId = Hex(within.GetProperty("id"), Invalid);
                }
                else if (within.ValueKind != JsonValueKind.Null)
                {
                    throw Invalid("within of " + name);
                }
            }

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

                // 값 종류는 성질 표(ValueKindTable — Docs/Design/ReflectionExtensions.ko.md 1.3·1.8절)가 그 자리(클래스 필드·struct
                // 멤버)에 허용하는 종류만 받는다(그 밖은 생성 코드가 컴파일되어도 런타임 등록에서 실패한다).
                string kind = Name(value.GetProperty("kind"), Identifier, "field value kind");
                if (!ValueKindTable.Has(kind, isStruct ? ValueKindProperty.StructMember : ValueKindProperty.Field))
                {
                    throw Invalid("field value kind " + kind);
                }

                // 배열 필드는 value에 대상이 없고(value.type 0) element가 원소 형식이다. Optional 필드(리플렉션 B9e)도 value에 대상이
                // 없고 element가 내부 형식(성질 표의 Optional 내부 종류)이며 maxElements가 0이다. 그 밖의 필드는 element가 null이고
                // maxElements가 0이다(Docs/Design/ReflectionFoundations.ko.md 5.6절 descriptor).
                bool array = kind == ArrayRules.Array;
                bool optional = kind == OptionalRules.Optional;
                bool map = kind == MapRules.Map;
                bool set = kind == MapRules.Set;
                bool fixedArray = kind == StaticArrayRules.StaticArray;
                RequireKeys(value, !array && !optional && !map && !set && !fixedArray && ValueKindTable.Has(kind, ValueKindProperty.HasTargetType) ? ["kind", "type"] : ["kind"], Invalid);
                JsonElement element = field.GetProperty("element");
                JsonElement keyElement = field.GetProperty("key");
                if (!field.GetProperty("maxElements").TryGetUInt32(out uint maxElements))
                {
                    throw Invalid("maxElements");
                }

                string? elementKind = null;
                JsonElement targetHolder = value;
                if (array)
                {
                    if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty("kind", out JsonElement elementKindValue))
                    {
                        throw Invalid("element of an array field");
                    }

                    elementKind = Name(elementKindValue, Identifier, "element kind");
                    if (!ValueKindTable.Has(elementKind, ValueKindProperty.ArrayElement))
                    {
                        throw Invalid("element kind " + elementKind);
                    }

                    RequireKeys(element, ValueKindTable.Has(elementKind, ValueKindProperty.HasTargetType) ? ["kind", "type"] : ["kind"], Invalid);
                    if (maxElements < 1 || maxElements > ArrayRules.MaxArrayElements)
                    {
                        throw Invalid($"maxElements {maxElements} of an array field");
                    }

                    targetHolder = element;
                }
                else if (fixedArray)
                {
                    // 정적 배열(리플렉션 B9g)은 element가 원소 형식(성질 표의 정적 배열 원소 종류)이고 maxElements가 정확한 원소 수 N이다.
                    if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty("kind", out JsonElement fixedKindValue))
                    {
                        throw Invalid("element of a static array field");
                    }

                    elementKind = Name(fixedKindValue, Identifier, "static array element kind");
                    if (!ValueKindTable.Has(elementKind, ValueKindProperty.StaticArrayElement))
                    {
                        throw Invalid("static array element kind " + elementKind);
                    }

                    RequireKeys(element, ValueKindTable.Has(elementKind, ValueKindProperty.HasTargetType) ? ["kind", "type"] : ["kind"], Invalid);
                    if (maxElements < 1 || maxElements > StaticArrayRules.MaxStaticArrayElements)
                    {
                        throw Invalid($"maxElements {maxElements} of a static array field");
                    }

                    targetHolder = element;
                }
                else if (optional)
                {
                    if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty("kind", out JsonElement innerKindValue))
                    {
                        throw Invalid("element of an Optional field");
                    }

                    elementKind = Name(innerKindValue, Identifier, "Optional inner kind");
                    if (!ValueKindTable.Has(elementKind, ValueKindProperty.OptionalInner))
                    {
                        throw Invalid("Optional inner kind " + elementKind);
                    }

                    RequireKeys(element, ValueKindTable.Has(elementKind, ValueKindProperty.HasTargetType) ? ["kind", "type"] : ["kind"], Invalid);
                    if (maxElements != 0)
                    {
                        throw Invalid($"maxElements {maxElements} of an Optional field");
                    }

                    targetHolder = element;
                }
                else if (map || set)
                {
                    // Map 값은 성질 표의 Map 값 종류(강한 참조 값은 ObjectRefMap — 리플렉션 B9f4), Set 원소는 Set 원소 종류다(리플렉션 B9f2).
                    string what = map ? "map value" : "set element";
                    if (element.ValueKind != JsonValueKind.Object || !element.TryGetProperty("kind", out JsonElement mapElementKind))
                    {
                        throw Invalid("element of a map or set field");
                    }

                    elementKind = Name(mapElementKind, Identifier, what + " kind");
                    if (!ValueKindTable.Has(elementKind, map ? ValueKindProperty.MapValue : ValueKindProperty.SetElement))
                    {
                        throw Invalid(what + " kind " + elementKind);
                    }

                    RequireKeys(element, ValueKindTable.Has(elementKind, ValueKindProperty.HasTargetType) ? ["kind", "type"] : ["kind"], Invalid);
                    if (maxElements < 1 || maxElements > ArrayRules.MaxArrayElements)
                    {
                        throw Invalid($"maxElements {maxElements} of a map or set field");
                    }

                    targetHolder = element;
                }
                else if (element.ValueKind != JsonValueKind.Null || maxElements != 0)
                {
                    throw Invalid("element or maxElements on a field that is not an array, static array, Optional, map or set");
                }

                // Map 키는 성질 표의 Map 키 종류이고 대상 종류(Enum·ClassReference·SoftObjectReference)면 type을 싣는다. Map이 아닌 필드의
                // key는 null이다.
                string? keyKind = null;
                string? keyReferenceId = null;
                if (map)
                {
                    if (keyElement.ValueKind != JsonValueKind.Object || !keyElement.TryGetProperty("kind", out JsonElement keyKindValue))
                    {
                        throw Invalid("key of a map field");
                    }

                    keyKind = Name(keyKindValue, Identifier, "map key kind");
                    if (!ValueKindTable.Has(keyKind, ValueKindProperty.MapKey))
                    {
                        throw Invalid("map key kind " + keyKind);
                    }

                    bool keyTarget = ValueKindTable.Has(keyKind, ValueKindProperty.HasTargetType);
                    RequireKeys(keyElement, keyTarget ? ["kind", "type"] : ["kind"], Invalid);
                    if (keyTarget)
                    {
                        string keyTargetText = keyElement.GetProperty("type").GetString() ?? throw Invalid("map key target");
                        keyReferenceId = keyTargetText.Length == 0 && !ValueKindTable.Has(keyKind, ValueKindProperty.RequiresTargetId)
                            ? null : Hex(keyElement.GetProperty("type"), Invalid);
                    }
                }
                else if (keyElement.ValueKind != JsonValueKind.Null)
                {
                    throw Invalid("key on a field that is not a map");
                }

                // 대상 종류는 배열이면 원소, Optional이면 내부 종류, Map이면 값 종류다. FieldKind::ObjectReference ⇔ 대상이
                // ObjectReference(ObjectRefArray·ObjectRefMap 포함).
                string targetKind = elementKind ?? kind;
                if (reference != (targetKind == "ObjectReference"))
                {
                    throw Invalid("fieldKind and value kind disagree");
                }

                string[] fieldFlags = field.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
                if (fieldFlags.Any(flag => flag is not ("Transient" or "ReadOnly" or "EditorOnly" or "Required" or "DuplicateTransient" or
                        "NonPIEDuplicateTransient" or "SaveGame" or "TextExportTransient")) ||
                    fieldFlags.Distinct(StringComparer.Ordinal).Count() != fieldFlags.Length)
                {
                    throw Invalid("unknown or duplicate field flag");
                }

                // Required는 성질 표가 허용하는 대상 종류(강한·소프트 참조 필드와 그 원소의 배열)에만 쓴다(원본 선언의 RMC2202와
                // 같은 규칙).
                if (!ValueKindTable.Has(targetKind, ValueKindProperty.AcceptsRequired) && fieldFlags.Contains("Required", StringComparer.Ordinal))
                {
                    throw Invalid("Required on a field that is not an object, soft object or class reference or an array of them");
                }

                // Required와 복제 제외 플래그의 조합은 원본 선언의 RMC2202와 같이 거부한다(리플렉션 B9h1).
                if (fieldFlags.Contains("Required", StringComparer.Ordinal) &&
                    (fieldFlags.Contains("DuplicateTransient", StringComparer.Ordinal) || fieldFlags.Contains("NonPIEDuplicateTransient", StringComparer.Ordinal)))
                {
                    throw Invalid("Required combined with DuplicateTransient or NonPIEDuplicateTransient");
                }

                string? referenceId = null;
                if (ValueKindTable.Has(targetKind, ValueKindProperty.HasTargetType))
                {
                    string target = targetHolder.GetProperty("type").GetString() ?? throw Invalid("reference target");
                    // 대상 ID 필수 종류(Enum·Struct)는 ID가 있어야 한다(참조의 ""는 Object다).
                    referenceId = target.Length == 0 && !ValueKindTable.Has(targetKind, ValueKindProperty.RequiresTargetId) ? null : Hex(targetHolder.GetProperty("type"), Invalid);
                }

                string fieldName = Name(field.GetProperty("name"), Identifier, "field name");
                // toolName은 실효 도구 이름이다(지정하지 않았으면 필드 이름).
                string toolName = Name(field.GetProperty("toolName"), Identifier, "toolName of " + fieldName);
                string owner = Name(field.GetProperty("owner"), QualifiedName, "field owner");
                // base 없는 struct는 모든 필드의 owner가 그 struct다(상속 struct의 owner는 base 사슬 — 리플렉션 B9k).
                if (isStruct && owner != name && type.GetProperty("base").GetString() is "")
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
                    Bitfield = ReadBitfield(field, kind, Invalid),
                    Spec = new(fieldFlags.Contains("Transient"), fieldFlags.Contains("ReadOnly"),
                        field.GetProperty("category").GetString() ?? throw Invalid("category"),
                        field.GetProperty("displayName").GetString() ?? throw Invalid("displayName"))
                    {
                        EditorOnly = fieldFlags.Contains("EditorOnly"),
                        Required = fieldFlags.Contains("Required"),
                        DuplicateTransient = fieldFlags.Contains("DuplicateTransient"),
                        NonPIEDuplicateTransient = fieldFlags.Contains("NonPIEDuplicateTransient"),
                        SaveGame = fieldFlags.Contains("SaveGame"),
                        TextExportTransient = fieldFlags.Contains("TextExportTransient"),
                        ToolName = toolName == fieldName ? "" : toolName,
                        // 정적 배열의 maxElements는 지정자가 아니라 N이다(리플렉션 B9g).
                        MaxElements = fixedArray ? 0 : maxElements
                    },
                    Kind = kind,
                    ElementKind = elementKind,
                    KeyKind = keyKind,
                    KeyReferenceId = keyReferenceId,
                    MaxElements = maxElements,
                    ReferenceId = referenceId
                });
            }

            model.Fields = fields;
            types.Add(model);
        }

        return (module, types);
    }

    // 필드의 bitfield 표시다(리플렉션 B9m): bool이어야 하고, 참이면 값 종류가 Bool이어야 한다(아니면 InvalidDescriptor).
    private static bool ReadBitfield(JsonElement field, string kind, Func<string, Exception> invalid)
    {
        JsonElement bitfield = field.GetProperty("bitfield");
        if (bitfield.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
        {
            throw invalid("bitfield");
        }

        if (bitfield.GetBoolean() && kind != "Bool")
        {
            throw invalid("bitfield of a non-Bool field");
        }

        return bitfield.GetBoolean();
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

        // binds는 struct와 같은 문법의 실제 enum 정규 철자이거나 ""다(생성 C++에 그대로 들어간다).
        string binds = type.GetProperty("binds").GetString() ?? throw Invalid("binds of " + name);
        if (binds.Length != 0 && (!StructRules.IsBindsSpelling(binds) || binds.StartsWith("::", StringComparison.Ordinal)))
        {
            throw Invalid("binds of " + name + ": " + binds);
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
            Enumerators = values,
            Binds = binds
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
