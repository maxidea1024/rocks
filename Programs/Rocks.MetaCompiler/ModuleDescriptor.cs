using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.MetaCompiler;

// 모듈 descriptor(<Module>.rocksmeta.json)다. 의존 모듈이 상속 필드를 펼치고 참조 대상 ID를 해석하는 데
// 필요한 것을 모두 담는다(Docs/Design/ReflectionFoundations.ko.md 1.4절). 읽기는 모르는 속성을 거부하고
// schema 1만 받는다. 쓰기는 결정적이다(타입 이름 순, 필드는 생성 순서, 고정 서식, LF).
internal static class ModuleDescriptor
{
    // descriptor 형식 판이다. 2: 클래스 flags에 Exported(Docs/Design/BuildTargets 3.8절 BS8d3). 다른 판은 다시 만들어야 한다.
    // 3: 필드 assetBundles(에셋 A4e2a)를 더했다.
    // 4: 타입 flags ScriptType·ScriptExtensible(실효 — 상속 포함), 필드 flags ScriptReadOnly·ScriptWritable과 필드 scriptName(Sparks binding —
    // Docs/Design/SparksBinding.ko.md B2).
    // 5: 클래스 functions(리플렉션 함수 — 이름·ID·도구 이름·flags·매개변수·반환, Docs/Design/FunctionReflection.ko.md FD10). 다른 모듈의 상속 함수
    // 이름 겹침 검사(RMC2703)와 Sparks binding이 쓴다.
    // 6: 함수 flag ScriptCallable과 함수 scriptName(노출 함수의 최종 Sparks 이름 — Docs/Design/SparksBinding.ko.md B15).
    // 7: 이벤트 필드(fieldKind "Event" — 키 EventFieldKeys, 인자 params — Docs/Design/EventReflection.ko.md ED18). 파생 모듈이 상속 이벤트를
    // schema·연산표에 다시 싣는다.
    private const int DescriptorSchema = 7;

    // 클래스 항목이다. toolName은 실효 타입 도구 이름(Docs/Design/ReflectionExtensions.ko.md 1.8·5.2절 — 지정하지 않았으면 한정하지
    // 않은 C++ 이름)이다.
    private static readonly string[] TypeKeys =
        ["kind", "name", "id", "version", "flags", "within", "base", "header", "visibility", "toolName", "fields", "functions"];

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
        ["owner", "name", "id", "fieldKind", "value", "element", "key", "flags", "toolName", "category", "displayName", "maxElements", "bitfield", "assetBundles",
            "scriptName"];

    // 이벤트 필드 항목이다(값 형식 칸 대신 인자 params — 칸마다 함수 매개변수 칸과 flags(ObjectHandle)). flags는 Transient →
    // EditorOnly → DuplicateTransient → NonTryoutDuplicateTransient → SaveGame 순서다.
    private static readonly string[] EventFieldKeys = ["owner", "name", "id", "fieldKind", "flags", "toolName", "category", "displayName", "params"];

    private static readonly string[] EventParameterKeys = ["name", "value", "element", "key", "flags", "maxElements"];

    // 클래스의 자기 리플렉션 함수 항목이다(상속 함수는 기반 클래스 항목에 있다). flags는 Static → Const → Pure → EditorOnly → ReturnsResult →
    // ScriptCallable 순서다.
    private static readonly string[] FunctionKeys = ["name", "id", "toolName", "flags", "parameters", "result", "scriptName"];

    // 함수 매개변수 칸(name과 필드와 같은 형식 칸 넷)과 반환 칸(형식 칸 넷)이다.
    private static readonly string[] ParameterKeys = ["name", "value", "element", "key", "maxElements"];

    private static readonly string[] ResultKeys = ["value", "element", "key", "maxElements"];

    public static byte[] Write(string module, IEnumerable<ClassModel> types)
    {
        using MemoryStream stream = new();
        // 줄바꿈 기본값은 플랫폼마다 다르므로(Windows CRLF) LF로 고정한다.
        using (Utf8JsonWriter json = new(stream, new JsonWriterOptions { Indented = true, NewLine = "\n", Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
        {
            json.WriteStartObject();
            json.WriteNumber("schema", DescriptorSchema);
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

                if (type.EditorOnly)
                {
                    json.WriteStringValue("EditorOnly");
                }

                // 클래스 전체 export(BS8d3) — 다른 모듈의 파생 클래스가 이 클래스를 상속할 수 있다.
                if (type.Exported)
                {
                    json.WriteStringValue("Exported");
                }

                WriteScriptFlags(json, type);
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
                WriteFunctions(json, type);
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

        WriteScriptFlags(json, type);
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
            if (field.IsEvent)
            {
                WriteEventField(json, field);
                continue;
            }

            json.WriteStartObject();
            json.WriteString("owner", field.Owner);
            json.WriteString("name", field.Name);
            json.WriteString("id", field.Id);
            json.WriteString("fieldKind", field.IsObjectReferenceFieldKind ? "ObjectReference" : "Value");
            WriteSlotType(json, field);
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
                (field.Spec.NonTryoutDuplicateTransient, "NonTryoutDuplicateTransient"), (field.Spec.SaveGame, "SaveGame"),
                (field.Spec.TextExportTransient, "TextExportTransient"), (field.Spec.TextMapMergeByKey, "TextMapMergeByKey"),
                (field.Spec.ScriptReadOnly, "ScriptReadOnly"), (field.Spec.ScriptWritable, "ScriptWritable") })
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
            // bundle 이름 목록의 정규 텍스트다(에셋 A4e2a — 없으면 ""). 상속 필드 일치 비교와 EditorOnly 필드 안 목록 거부가 쓴다.
            json.WriteString("assetBundles", field.Spec.AssetBundles);
            // Sparks 스크립트 이름 지정(ScriptName — 없으면 "")이다. 파생 모듈이 상속 필드의 Sparks 이름 겹침을 본다(SparksBinding B5).
            json.WriteString("scriptName", field.Spec.ScriptName);
            json.WriteEndObject();
        }

        json.WriteEndArray();
    }

    // 이벤트 필드 항목이다(EventFieldKeys — ED18).
    private static void WriteEventField(Utf8JsonWriter json, FieldModel field)
    {
        json.WriteStartObject();
        json.WriteString("owner", field.Owner);
        json.WriteString("name", field.Name);
        json.WriteString("id", field.Id);
        json.WriteString("fieldKind", "Event");
        json.WriteStartArray("flags");
        foreach ((bool set, string flag) in new[] { (field.Spec.Transient, "Transient"), (field.Spec.EditorOnly, "EditorOnly"),
            (field.Spec.DuplicateTransient, "DuplicateTransient"), (field.Spec.NonTryoutDuplicateTransient, "NonTryoutDuplicateTransient"),
            (field.Spec.SaveGame, "SaveGame") })
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
        json.WriteStartArray("params");
        foreach (FieldModel slot in field.EventParameters)
        {
            json.WriteStartObject();
            json.WriteString("name", slot.Name);
            WriteSlotType(json, slot);
            json.WriteStartArray("flags");
            if (slot.NativeSpelling == FunctionRules.ObjectHandleSpelling)
            {
                json.WriteStringValue("ObjectHandle");
            }

            json.WriteEndArray();
            json.WriteNumber("maxElements", slot.IsCounted ? slot.MaxElements : 0);
            json.WriteEndObject();
        }

        json.WriteEndArray();
        json.WriteEndObject();
    }

    // 필드·함수 칸의 형식 칸 value·element·key다(maxElements는 부르는 쪽이 적는다).
    private static void WriteSlotType(Utf8JsonWriter json, FieldModel field)
    {
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
    }

    // 클래스의 자기 리플렉션 함수 배열이다(선언 순서 그대로).
    private static void WriteFunctions(Utf8JsonWriter json, ClassModel type)
    {
        json.WriteStartArray("functions");
        foreach (FunctionModel function in type.Functions)
        {
            json.WriteStartObject();
            json.WriteString("name", function.Name);
            json.WriteString("id", function.Id);
            json.WriteString("toolName", function.EffectiveToolName);
            json.WriteStartArray("flags");
            foreach ((bool set, string flag) in new[] { (function.IsStatic, "Static"), (function.IsConst, "Const"), (function.Spec.Pure, "Pure"),
                (function.Spec.EditorOnly, "EditorOnly"), (function.ReturnsResult, "ReturnsResult"), (function.Spec.ScriptCallable, "ScriptCallable") })
            {
                if (set)
                {
                    json.WriteStringValue(flag);
                }
            }

            json.WriteEndArray();
            json.WriteStartArray("parameters");
            foreach (FieldModel slot in function.Parameters)
            {
                json.WriteStartObject();
                json.WriteString("name", slot.Name);
                WriteSlotType(json, slot);
                json.WriteNumber("maxElements", slot.IsCounted ? slot.MaxElements : 0);
                json.WriteEndObject();
            }

            json.WriteEndArray();
            if (function.Result is FieldModel result)
            {
                json.WriteStartObject("result");
                WriteSlotType(json, result);
                json.WriteNumber("maxElements", result.IsCounted ? result.MaxElements : 0);
                json.WriteEndObject();
            }
            else
            {
                json.WriteNull("result");
            }

            // 노출 함수의 최종 Sparks 이름이다(B15 — 파생 모듈이 기반 체인의 멤버 이름 겹침을 본다).
            json.WriteString("scriptName", SparksBinding.ScriptFunctionName(function));
            json.WriteEndObject();
        }

        json.WriteEndArray();
    }

    // 실효 Sparks 노출 flag를 기존 flag 뒤에 ScriptType → ScriptExtensible 순서로 적는다(Docs/Design/SparksBinding.ko.md B2).
    private static void WriteScriptFlags(Utf8JsonWriter json, ClassModel type)
    {
        if (type.ScriptType)
        {
            json.WriteStringValue("ScriptType");
        }

        if (type.ScriptExtensible)
        {
            json.WriteStringValue("ScriptExtensible");
        }
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

        WriteScriptFlags(json, type);
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
        if (!root.GetProperty("schema").TryGetInt32(out int schema) || schema != DescriptorSchema)
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
            bool KnownFlag(string flag) => isStruct ? flag is "Atomic" or "ContainsStrongReference" or "RequiresWorldOnlyOwner" or "ContainsReferenceArray" or "ScriptType"
                : flag is "Abstract" or "WorldOnly" or "Transient" or "Deprecated" or "EditorOnly" or "Exported" or "ScriptType" or "ScriptExtensible";
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
                EditorOnly = flags.Contains("EditorOnly"),
                Exported = flags.Contains("Exported"),
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
                ToolName = typeToolName == TypeToolName.Default(name) ? "" : typeToolName,
                ScriptType = flags.Contains("ScriptType"),
                ScriptExtensible = flags.Contains("ScriptExtensible")
            };
            // EditorOnly 타입은 Sparks에 보이지 않는다(원본 규칙 RMC2607 — Docs/Design/SparksBinding.ko.md B1). ScriptExtensible은 확장 상속
            // 값이라 NotScriptType 타입에도 있을 수 있다(UE Blueprintable처럼 노출과 따로 내려간다 — B1).
            if (model.EditorOnly && (model.ScriptType || model.ScriptExtensible))
            {
                throw Invalid("Sparks exposure on the EditorOnly type " + name);
            }
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
                if (field.ValueKind == JsonValueKind.Object && field.TryGetProperty("fieldKind", out JsonElement declaredKind) &&
                    declaredKind.ValueKind == JsonValueKind.String && declaredKind.GetString() == "Event")
                {
                    // 이벤트 필드는 클래스에만 있다(원본 선언의 RMC2802).
                    if (isStruct)
                    {
                        throw Invalid($"event field in struct {name}");
                    }

                    fields.Add(ReadEventField(field, descriptorPath, Invalid));
                    continue;
                }

                RequireKeys(field, FieldKeys, Invalid);
                bool reference = field.GetProperty("fieldKind").GetString() switch
                {
                    "ObjectReference" => true,
                    "Value" => false,
                    _ => throw Invalid("fieldKind")
                };
                SlotType slotType = ReadSlotType(field, isStruct ? ValueKindProperty.StructMember : ValueKindProperty.Field, Invalid);
                string kind = slotType.Kind;
                string? elementKind = slotType.ElementKind;
                string? keyKind = slotType.KeyKind;
                uint maxElements = slotType.MaxElements;
                bool fixedArray = kind == StaticArrayRules.StaticArray;
                // 대상 종류는 배열이면 원소, Optional이면 내부 종류, Map이면 값 종류다. FieldKind::ObjectReference ⇔ 대상이
                // ObjectReference(ObjectRefArray·ObjectRefMap 포함).
                string targetKind = elementKind ?? kind;
                if (reference != (targetKind == "ObjectReference"))
                {
                    throw Invalid("fieldKind and value kind disagree");
                }

                string[] fieldFlags = field.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
                if (fieldFlags.Any(flag => flag is not ("Transient" or "ReadOnly" or "EditorOnly" or "Required" or "DuplicateTransient" or
                        "NonTryoutDuplicateTransient" or "SaveGame" or "TextExportTransient" or "TextMapMergeByKey" or "ScriptReadOnly" or "ScriptWritable")) ||
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

                // TextMapMergeByKey는 Map 필드에만 쓴다(원본 선언의 RMC2202와 같은 규칙 — AssetSystem 10.3.1절).
                if (fieldFlags.Contains("TextMapMergeByKey", StringComparer.Ordinal) && kind != "Map")
                {
                    throw Invalid("TextMapMergeByKey on a field that is not a map");
                }

                // Required와 복제 제외 플래그의 조합은 원본 선언의 RMC2202와 같이 거부한다(리플렉션 B9h1).
                if (fieldFlags.Contains("Required", StringComparer.Ordinal) &&
                    (fieldFlags.Contains("DuplicateTransient", StringComparer.Ordinal) || fieldFlags.Contains("NonTryoutDuplicateTransient", StringComparer.Ordinal)))
                {
                    throw Invalid("Required combined with DuplicateTransient or NonTryoutDuplicateTransient");
                }

                // bundle 목록은 정규 텍스트(""이거나 같은 목록으로 되돌아오는 텍스트)이고 원본 선언의 RMC2202와 같이 종류·EditorOnly를 본다.
                string assetBundles = field.GetProperty("assetBundles").GetString() ?? throw Invalid("assetBundles");
                bool bundleCarrier = targetKind is "SoftObjectReference" or "Struct" or "InstancedStruct" || keyKind == "SoftObjectReference";
                if (assetBundles.Length != 0 && (Specifiers.CanonicalAssetBundles(assetBundles) != assetBundles || !bundleCarrier ||
                        fieldFlags.Contains("EditorOnly", StringComparer.Ordinal)))
                {
                    throw Invalid("assetBundles");
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
                        NonTryoutDuplicateTransient = fieldFlags.Contains("NonTryoutDuplicateTransient"),
                        SaveGame = fieldFlags.Contains("SaveGame"),
                        TextExportTransient = fieldFlags.Contains("TextExportTransient"),
                        TextMapMergeByKey = fieldFlags.Contains("TextMapMergeByKey"),
                        ToolName = toolName == fieldName ? "" : toolName,
                        // 정적 배열의 maxElements는 지정자가 아니라 N이다(리플렉션 B9g).
                        MaxElements = fixedArray ? 0 : maxElements,
                        AssetBundles = assetBundles,
                        ScriptReadOnly = fieldFlags.Contains("ScriptReadOnly"),
                        ScriptWritable = fieldFlags.Contains("ScriptWritable"),
                        ScriptName = ScriptNameOf(field, fieldFlags, Invalid)
                    },
                    Kind = kind,
                    ElementKind = elementKind,
                    KeyKind = keyKind,
                    KeyReferenceId = slotType.KeyReferenceId,
                    MaxElements = maxElements,
                    ReferenceId = slotType.ReferenceId
                });
            }

            model.Fields = fields;
            if (!isStruct)
            {
                model.Functions.AddRange(ReadFunctions(type, name, model.ScriptType, descriptorPath, Invalid));
            }

            types.Add(model);
        }

        return (module, types);
    }

    // 필드·함수 칸의 형식 칸 value·element·key·maxElements를 읽는다. place는 값 종류가 놓이는 자리(클래스 필드·struct 멤버)의 성질이다.
    private sealed record SlotType(string Kind, string? ElementKind, string? KeyKind, string? KeyReferenceId, uint MaxElements, string? ReferenceId)
    {
        // 대상 종류다(배열이면 원소, Optional이면 내부 종류, Map이면 값 종류).
        public string TargetKind => ElementKind ?? Kind;
    }

    private static SlotType ReadSlotType(JsonElement field, ValueKindProperty place, Func<string, MetadataException> Invalid)
    {
        string Name(JsonElement element, Regex pattern, string what)
        {
            string text = element.GetString() ?? "";
            return pattern.IsMatch(text) ? text : throw Invalid($"invalid {what}: {text}");
        }

        JsonElement value = field.GetProperty("value");
        if (value.ValueKind != JsonValueKind.Object)
        {
            throw Invalid("field value");
        }

        // 값 종류는 성질 표(ValueKindTable — Docs/Design/ReflectionExtensions.ko.md 1.3·1.8절)가 그 자리(클래스 필드·struct
        // 멤버)에 허용하는 종류만 받는다(그 밖은 생성 코드가 컴파일되어도 런타임 등록에서 실패한다).
        string kind = Name(value.GetProperty("kind"), Identifier, "field value kind");
        if (!ValueKindTable.Has(kind, place))
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

        string targetKind = elementKind ?? kind;
        string? referenceId = null;
        if (ValueKindTable.Has(targetKind, ValueKindProperty.HasTargetType))
        {
            string target = targetHolder.GetProperty("type").GetString() ?? throw Invalid("reference target");
            // 대상 ID 필수 종류(Enum·Struct)는 ID가 있어야 한다(참조의 ""는 Object다).
            referenceId = target.Length == 0 && !ValueKindTable.Has(targetKind, ValueKindProperty.RequiresTargetId) ? null : Hex(targetHolder.GetProperty("type"), Invalid);
        }

        return new(kind, elementKind, keyKind, keyReferenceId, maxElements, referenceId);
    }

    // 클래스의 functions 배열을 읽는다(Docs/Design/FunctionReflection.ko.md FD10). 칸 형식은 필드와 같은 규칙에 호출 frame에 둘 수 없는 종류
    // (정적 배열·InstancedStruct·강한 참조와 그것을 담는 컨테이너 — 원본 선언의 RMC2704)를 더 거부한다.
    private static List<FunctionModel> ReadFunctions(JsonElement type, string owner, bool scriptType, string descriptorPath, Func<string, MetadataException> Invalid)
    {
        List<FunctionModel> functions = [];
        // C++ 이름은 오버로드가 없어 정확히 유일하고, 실효 도구 이름은 대소문자 무시로 유일하다(원본 선언의 RMC2702·RMC2703과 같다).
        HashSet<string> names = new(StringComparer.Ordinal);
        HashSet<string> toolNames = new(StringComparer.OrdinalIgnoreCase);
        if (type.GetProperty("functions").GetArrayLength() > FunctionRules.MaxFunctions)
        {
            throw Invalid("too many functions in " + owner);
        }

        foreach (JsonElement function in type.GetProperty("functions").EnumerateArray())
        {
            RequireKeys(function, FunctionKeys, Invalid);
            string name = function.GetProperty("name").GetString() ?? "";
            string toolName = function.GetProperty("toolName").GetString() ?? "";
            if (!Identifier.IsMatch(name) || !Identifier.IsMatch(toolName) || !names.Add(name) || !toolNames.Add(toolName))
            {
                throw Invalid($"function name of {owner}: {name}");
            }

            string[] flags = function.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
            if (flags.Any(flag => flag is not ("Static" or "Const" or "Pure" or "EditorOnly" or "ReturnsResult" or "ScriptCallable")) ||
                flags.Distinct(StringComparer.Ordinal).Count() != flags.Length || (flags.Contains("Static") && flags.Contains("Const")))
            {
                throw Invalid($"unknown, duplicate or conflicting function flag in {owner}::{name}");
            }

            // 노출 함수는 최종 Sparks 이름(PascalCase)이 있고, 그 밖은 빈 글자다. EditorOnly 함수는 노출되지 않는다(원본 RMC2607).
            JsonElement scriptNameValue = function.GetProperty("scriptName");
            string scriptName = scriptNameValue.ValueKind == JsonValueKind.String ? scriptNameValue.GetString() ?? "" : throw Invalid("scriptName of " + owner + "::" + name);
            bool callable = flags.Contains("ScriptCallable");
            // 생성기가 만들지 않는 조합(Try 접두 이름, 노출하지 않은 타입의 노출 함수)도 거부한다(원본 선언의 RMC2604·RMC2608).
            if (callable != (scriptName.Length != 0) || (callable && (!Specifiers.IsScriptName(scriptName) || SparksBinding.StartsWithTry(scriptName) ||
                    flags.Contains("EditorOnly") || !scriptType)))
            {
                throw Invalid($"Sparks exposure of {owner}::{name}");
            }

            SourceSite site = new(descriptorPath, 1);
            FieldModel Slot(JsonElement slot, string slotName)
            {
                SlotType slotType = ReadSlotType(slot, ValueKindProperty.Field, Invalid);
                if (slotType.Kind is StaticArrayRules.StaticArray or "InstancedStruct" or "ObjectReference" ||
                    slotType.TargetKind is "InstancedStruct" or "ObjectReference" || slotType.KeyKind == "ObjectReference")
                {
                    throw Invalid($"function slot kind {slotType.Kind} in {owner}::{name}");
                }

                return new FieldModel
                {
                    Id = "",
                    Site = site,
                    Name = slotName,
                    Owner = owner,
                    NativeSpelling = "",
                    Spec = new(false, false, "", "") { MaxElements = slotType.MaxElements },
                    Kind = slotType.Kind,
                    ElementKind = slotType.ElementKind,
                    KeyKind = slotType.KeyKind,
                    KeyReferenceId = slotType.KeyReferenceId,
                    MaxElements = slotType.MaxElements,
                    ReferenceId = slotType.ReferenceId
                };
            }

            FunctionModel model = new()
            {
                Id = Hex(function.GetProperty("id"), Invalid),
                Site = site,
                Name = name,
                Owner = owner,
                Spec = new(flags.Contains("Pure"), flags.Contains("EditorOnly"), toolName == name ? "" : toolName)
                {
                    ScriptCallable = flags.Contains("ScriptCallable"),
                    ScriptName = scriptName
                },
                IsStatic = flags.Contains("Static"),
                IsConst = flags.Contains("Const"),
                ReturnsResult = flags.Contains("ReturnsResult"),
                Imported = true
            };
            JsonElement parameters = function.GetProperty("parameters");
            if (parameters.GetArrayLength() > FunctionRules.MaxParameters)
            {
                throw Invalid($"too many parameters in {owner}::{name}");
            }

            HashSet<string> parameterNames = new(StringComparer.Ordinal);
            foreach (JsonElement parameter in parameters.EnumerateArray())
            {
                RequireKeys(parameter, ParameterKeys, Invalid);
                string parameterName = parameter.GetProperty("name").GetString() ?? "";
                if (!Identifier.IsMatch(parameterName) || !parameterNames.Add(parameterName))
                {
                    throw Invalid($"parameter name in {owner}::{name}: {parameterName}");
                }

                model.Parameters.Add(Slot(parameter, parameterName));
            }

            JsonElement result = function.GetProperty("result");
            if (result.ValueKind == JsonValueKind.Object)
            {
                RequireKeys(result, ResultKeys, Invalid);
                model.Result = Slot(result, "");
            }
            else if (result.ValueKind != JsonValueKind.Null)
            {
                throw Invalid($"result of {owner}::{name}");
            }

            // Pure는 반환 값이 있어야 한다(원본 선언의 RMC2705).
            if (model.Spec.Pure && model.Result is null)
            {
                throw Invalid($"Pure function without a result: {owner}::{name}");
            }

            functions.Add(model);
        }

        return functions;
    }

    // 이벤트 필드 항목이다(EventFieldKeys — 원본 선언의 ED14 규칙: flag, lowerCamel 인자 이름, 인자 칸은 함수 매개변수 칸 규칙). ObjectHandle
    // 인자는 칸 철자를 rocks::ObjectHandle로 되살린다(생성 schema의 ParameterFlags::ObjectHandle).
    private static FieldModel ReadEventField(JsonElement field, string descriptorPath, Func<string, MetadataException> invalid)
    {
        string Name(JsonElement element, Regex pattern, string what)
        {
            string text = element.GetString() ?? "";
            return pattern.IsMatch(text) ? text : throw invalid($"invalid {what}: {text}");
        }

        RequireKeys(field, EventFieldKeys, invalid);
        string fieldName = Name(field.GetProperty("name"), Identifier, "field name");
        string toolName = Name(field.GetProperty("toolName"), Identifier, "toolName of " + fieldName);
        string owner = Name(field.GetProperty("owner"), QualifiedName, "field owner");
        string[] flags = field.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
        if (flags.Any(flag => flag is not ("Transient" or "EditorOnly" or "DuplicateTransient" or "NonTryoutDuplicateTransient" or "SaveGame")) ||
            flags.Distinct(StringComparer.Ordinal).Count() != flags.Length)
        {
            throw invalid($"unknown or duplicate event field flag in {owner}::{fieldName}");
        }

        FieldModel model = new()
        {
            Id = Hex(field.GetProperty("id"), invalid),
            Site = new(descriptorPath, 1),
            Name = fieldName,
            Owner = owner,
            NativeSpelling = "",
            Spec = new(flags.Contains("Transient"), false,
                field.GetProperty("category").GetString() ?? throw invalid("category"),
                field.GetProperty("displayName").GetString() ?? throw invalid("displayName"))
            {
                EditorOnly = flags.Contains("EditorOnly"),
                DuplicateTransient = flags.Contains("DuplicateTransient"),
                NonTryoutDuplicateTransient = flags.Contains("NonTryoutDuplicateTransient"),
                SaveGame = flags.Contains("SaveGame"),
                ToolName = toolName == fieldName ? "" : toolName
            },
            Kind = "Opaque",
            IsEvent = true
        };
        JsonElement parameters = field.GetProperty("params");
        if (parameters.ValueKind != JsonValueKind.Array || parameters.GetArrayLength() > FunctionRules.MaxParameters)
        {
            throw invalid($"params of {owner}::{fieldName}");
        }

        HashSet<string> names = new(StringComparer.Ordinal);
        foreach (JsonElement parameter in parameters.EnumerateArray())
        {
            RequireKeys(parameter, EventParameterKeys, invalid);
            string parameterName = parameter.GetProperty("name").GetString() ?? "";
            if (!EventRules.IsParameterName(parameterName) || !names.Add(parameterName))
            {
                throw invalid($"event argument name in {owner}::{fieldName}: {parameterName}");
            }

            string[] parameterFlags = parameter.GetProperty("flags").EnumerateArray().Select(flag => flag.GetString() ?? "").ToArray();
            bool objectHandle = parameterFlags.Length == 1 && parameterFlags[0] == "ObjectHandle";
            if (parameterFlags.Length > 1 || (parameterFlags.Length == 1 && !objectHandle))
            {
                throw invalid($"event argument flags in {owner}::{fieldName}");
            }

            SlotType slotType = ReadSlotType(parameter, ValueKindProperty.Field, invalid);
            if (slotType.Kind is StaticArrayRules.StaticArray or "InstancedStruct" or "ObjectReference" ||
                slotType.TargetKind is "InstancedStruct" or "ObjectReference" || slotType.KeyKind == "ObjectReference" ||
                (objectHandle && (slotType.Kind != "WeakObjectReference" || slotType.ReferenceId is not null)))
            {
                throw invalid($"event argument kind {slotType.Kind} in {owner}::{fieldName}");
            }

            model.EventParameters.Add(new FieldModel
            {
                Id = "",
                Site = model.Site,
                Name = parameterName,
                Owner = owner,
                NativeSpelling = objectHandle ? FunctionRules.ObjectHandleSpelling : "",
                Spec = new(false, false, "", "") { MaxElements = slotType.MaxElements },
                Kind = slotType.Kind,
                ElementKind = slotType.ElementKind,
                KeyKind = slotType.KeyKind,
                KeyReferenceId = slotType.KeyReferenceId,
                MaxElements = slotType.MaxElements,
                ReferenceId = slotType.ReferenceId
            });
        }

        return model;
    }

    // 필드 scriptName(Sparks 이름 지정 — 없으면 "")이다. 지정은 Sparks 노출 필드에만 있고 PascalCase ASCII다(원본 선언의 RMC2603·RMC2604와
    // 같은 규칙 — Docs/Design/SparksBinding.ko.md B1). ReadOnly 필드의 ScriptWritable도 원본 규칙대로 거부한다.
    private static string ScriptNameOf(JsonElement field, string[] flags, Func<string, Exception> invalid)
    {
        JsonElement value = field.GetProperty("scriptName");
        string name = value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : throw invalid("scriptName");
        bool visible = flags.Contains("ScriptReadOnly", StringComparer.Ordinal) || flags.Contains("ScriptWritable", StringComparer.Ordinal);
        if (name.Length != 0 && (!visible || !Specifiers.IsScriptName(name)))
        {
            throw invalid("scriptName " + name);
        }

        if (flags.Contains("ScriptReadOnly", StringComparer.Ordinal) && flags.Contains("ScriptWritable", StringComparer.Ordinal))
        {
            throw invalid("ScriptReadOnly with ScriptWritable");
        }

        if (flags.Contains("ReadOnly", StringComparer.Ordinal) && flags.Contains("ScriptWritable", StringComparer.Ordinal))
        {
            throw invalid("ReadOnly with ScriptWritable");
        }

        return name;
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
        if (flags.Any(flag => flag is not ("Flags" or "ScriptType")) || flags.Distinct(StringComparer.Ordinal).Count() != flags.Length)
        {
            throw Invalid("unknown or duplicate enum flag in " + name);
        }

        bool flagsEnum = flags.Contains("Flags");
        // Flags enum은 Sparks 타입이 될 수 없다(원본 규칙 RMC2607).
        if (flagsEnum && flags.Contains("ScriptType"))
        {
            throw Invalid("ScriptType on the Flags enum " + name);
        }

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
            ScriptType = flags.Contains("ScriptType"),
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
