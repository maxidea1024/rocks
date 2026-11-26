using System.Reflection;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Rocks.MetaCompiler;

// Sparks 언어 이름 표다(Docs/Design/SparksBinding.ko.md B6). 정본은 Sparks 컴파일러의 목록이고(LanguageNames.cpp·LanguageCatalog.cpp),
// 이 파일(SparksNames.json — embedded resource)이 그것과 같은지는 C++ 시험 SparksBinding_NamesMatchCompiler가 대조한다.
internal sealed record SparksNames(IReadOnlySet<string> LanguageOwned, IReadOnlyList<string> Hooks, IReadOnlySet<string> UnityHookAliases)
{
    private const int NamesSchema = 1;

    private const string ResourceName = "SparksNames.json";

    private const string OnPrefix = "On";

    public static SparksNames Load()
    {
        using Stream stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName)
            ?? throw new InvalidOperationException($"embedded resource {ResourceName} is missing");
        using JsonDocument document = JsonDocument.Parse(stream);
        JsonElement root = document.RootElement;
        if (root.GetProperty("schema").GetInt32() != NamesSchema)
        {
            throw new InvalidOperationException($"{ResourceName}: unsupported schema");
        }

        static string[] Strings(JsonElement root, string key) =>
            root.GetProperty(key).EnumerateArray().Select(item => item.GetString() ?? "").ToArray();
        return new(Strings(root, "languageOwnedNames").ToHashSet(StringComparer.Ordinal), Strings(root, "hooks"),
            Strings(root, "unityHookAliases").ToHashSet(StringComparer.Ordinal));
    }

    // 수명 클래스 멤버로 쓸 수 없는 hook 계열 이름이면 그 hook 이름을, 아니면 null을 돌려준다(컴파일러 HookForMemberName과 같은 집합 —
    // hook, On + hook, Unity hook 별칭).
    public string? HookFor(string name)
    {
        foreach (string hook in Hooks)
        {
            if (name == hook || name == OnPrefix + hook)
            {
                return hook;
            }
        }

        return UnityHookAliases.Contains(name) ? name : null;
    }
}

// C++ 리플렉션에서 Sparks binding schema(`<모듈>.sparkbind.json` — SL 10.2절 형식 1)를 만든다(Docs/Design/SparksBinding.ko.md EI1a).
// Resolve는 이 모듈 타입의 실효 노출(상속·끊기)을 정하고 노출 규칙(RMC26xx)을 검사한다 — descriptor를 쓰기 전에 부른다(descriptor가
// 실효 노출 flag를 싣는다). Write는 검사를 마친 모델을 schema byte로 쓴다.
internal static class SparksBinding
{
    private const int FormatVersion = 1;

    private const string Package = "Rocks";

    private const string ObjectRoot = "rocks::Object";

    private const string AssetRoot = "rocks::Asset";

    // CoreTypeBindings 수학 미러(Binds 실제 타입의 Clang 정규 철자) → Sparks 내장·prelude 타입이다(B4 — SL 4장, 성분은 float64로 주고받는다).
    private static readonly Dictionary<string, string> MathMirrors = new(StringComparer.Ordinal)
    {
        ["rocks::Vector2<float>"] = "vec2",
        ["rocks::Vector2<double>"] = "vec2",
        ["rocks::Vector3<float>"] = "vec3",
        ["rocks::Vector3<double>"] = "vec3",
        ["rocks::Vector4<float>"] = "vec4",
        ["rocks::Vector4<double>"] = "vec4",
        ["rocks::Quaternion<float>"] = "quat",
        ["rocks::Quaternion<double>"] = "quat",
        ["rocks::Rotator<float>"] = "Rotator",
        ["rocks::Rotator<double>"] = "Rotator",
        ["rocks::Transform"] = "Transform",
        ["rocks::Transform3<float>"] = "Transform",
        ["rocks::Transform3<double>"] = "Transform",
        ["rocks::LinearColor"] = "Color",
    };

    // 값 종류 → Sparks 표기다(B4 표의 스칼라 행).
    private static readonly Dictionary<string, string> Scalars = new(StringComparer.Ordinal)
    {
        ["Bool"] = "bool",
        ["Int8"] = "int8",
        ["Int16"] = "int16",
        ["Int32"] = "int32",
        ["Int64"] = "int64",
        ["UInt8"] = "uint8",
        ["UInt16"] = "uint16",
        ["UInt32"] = "uint32",
        ["UInt64"] = "uint64",
        ["Float32"] = "float32",
        ["Float64"] = "float64",
        ["Utf8String"] = "string",
        ["Text"] = "Text",
    };

    public static void Resolve(SchemaModel schema, SparksNames names)
    {
        Dictionary<string, bool> visiting = new(StringComparer.Ordinal);
        foreach (ClassModel type in schema.Ordered)
        {
            ResolveType(schema, type, visiting);
        }

        foreach (ClassModel type in schema.Ordered.Where(type => type.ScriptType))
        {
            CheckType(type, names);
        }

        CheckTypeNames(schema);
        foreach (ClassModel type in schema.Ordered.Where(type => !type.IsEnum))
        {
            foreach (FieldModel field in type.OwnFields.Where(field => field.Spec.ScriptVisible))
            {
                CheckField(schema, type, field, names);
            }

            if (type.ScriptType)
            {
                CheckFieldNames(schema, type);
            }
        }
    }

    // 실효 노출을 정한다: 클래스·struct는 기반에서 상속되고 NotScriptType이 끊는다(UE IsScriptExposedClass·Struct), enum은 자기 것만 본다.
    // 확장 가능은 노출과 따로 기반에서 내려가고 NotScriptExtensible만 끊는다(UE Blueprintable — NotBlueprintType 아래에서도 이어진다).
    // ScriptExtensible에는 그 확장 상속 값을 두고, schema는 노출된 타입에만 쓴다. EditorOnly 타입은 어느 쪽도 상속받지 않는다(직접 쓰면
    // RMC2607).
    private static void ResolveType(SchemaModel schema, ClassModel type, Dictionary<string, bool> visiting)
    {
        if (type.Imported || visiting.ContainsKey(type.Name))
        {
            return;
        }

        visiting[type.Name] = true;
        ScriptTypeSpec script = type.Script;
        bool declared = script.Type || script.Extensible;
        if (declared && type.EditorOnly)
        {
            throw new MetadataException($"{type.Name}: an EditorOnly type cannot be visible to Sparks", "RMC2607",
                "Editor-only types are not in shipping builds; remove ScriptType/ScriptExtensible or EditorOnly.").At(type.Site);
        }

        if (declared && type.Binds.Length != 0)
        {
            throw new MetadataException($"{type.Name}: a Binds mirror cannot be a Sparks type", "RMC2607",
                "Math mirrors (vectors, quat, Rotator, Transform, LinearColor) map to Sparks builtin types without ScriptType; other mirrors cannot be shown to Sparks.").At(type.Site);
        }

        if (type.IsEnum)
        {
            type.ScriptType = script.Type;
            return;
        }

        bool baseType = false;
        bool baseExtensible = false;
        string baseName = type.Base;
        if (baseName.Length != 0 && baseName != ObjectRoot)
        {
            ClassModel parent = schema.Find(baseName);
            ResolveType(schema, parent, visiting);
            baseType = parent.ScriptType;
            baseExtensible = parent.ScriptExtensible;
        }

        if (type.EditorOnly)
        {
            type.ScriptType = false;
            type.ScriptExtensible = false;
            return;
        }

        type.ScriptType = !script.NotType && (declared || baseType);
        type.ScriptExtensible = !type.IsStruct && !script.NotExtensible && (script.Extensible || baseExtensible);
    }

    // 노출 타입 하나의 이름·종류 규칙이다.
    private static void CheckType(ClassModel type, SparksNames names)
    {
        string name = TypeName(type);
        if (!Specifiers.IsScriptName(name))
        {
            throw new MetadataException($"{type.Name}: Sparks type name '{name}' must be a PascalCase ASCII name (letters and digits)", "RMC2604",
                "Give the type a tool name of letters and digits starting with an uppercase letter (Name = \"...\"), or keep it hidden from Sparks.").At(type.Site);
        }

        if (names.LanguageOwned.Contains(name))
        {
            throw new MetadataException($"{type.Name}: Sparks type name '{name}' is owned by the Sparks language", "RMC2606",
                "Rename the type with Name = \"...\", or keep it hidden from Sparks (NotScriptType).").At(type.Site);
        }

        if (type.IsEnum)
        {
            if (type.FlagsEnum)
            {
                throw new MetadataException($"{type.Name}: a Flags enum cannot be a Sparks type (a Sparks enum value is one case)", "RMC2607",
                    "Remove ScriptType from the Flags enum; expose the individual options as bool fields instead.").At(type.Site);
            }

            List<EnumeratorModel> cases = type.Enumerators.Where(value => !value.Hidden).ToList();
            if (cases.Count == 0)
            {
                throw new MetadataException($"{type.Name}: a Sparks enum needs at least one case that is not Hidden", "RMC2607",
                    "Show at least one enumerator, or remove ScriptType.").At(type.Site);
            }

            foreach (EnumeratorModel value in cases.Where(value => !Specifiers.IsScriptName(value.EffectiveToolName)))
            {
                throw new MetadataException($"{type.Name}: Sparks enum case '{value.EffectiveToolName}' must be a PascalCase ASCII name", "RMC2604",
                    "Name the enumerator (or give it Name = \"...\") with letters and digits starting with an uppercase letter, or mark it Hidden.").At(type.Site);
            }
        }
    }

    // 한 패키지의 노출 타입 이름은 대소문자 무시로 겹치지 않는다(이 모듈과 import한 모듈 — B5).
    private static void CheckTypeNames(SchemaModel schema)
    {
        Dictionary<string, ClassModel> seen = new(StringComparer.OrdinalIgnoreCase);
        foreach (ClassModel type in schema.Imported.Values.Where(type => type.ScriptType).OrderBy(type => type.Name, StringComparer.Ordinal))
        {
            seen.TryAdd(TypeName(type), type);
        }

        foreach (ClassModel type in schema.Ordered.Where(type => type.ScriptType))
        {
            if (!seen.TryAdd(TypeName(type), type))
            {
                ClassModel other = seen[TypeName(type)];
                throw new MetadataException($"{type.Name}: Sparks type name '{TypeName(type)}' is also used by {other.Name} (module {other.Module})", "RMC2605",
                    "Give one of the types a different tool name (Name = \"...\"), or hide one from Sparks.").At(type.Site);
            }
        }
    }

    // 노출 필드 하나의 규칙이다: 노출 타입의 필드, EditorOnly 아님, 이름, hook 이름 충돌, 타입 표기.
    private static void CheckField(SchemaModel schema, ClassModel owner, FieldModel field, SparksNames names)
    {
        string symbol = owner.Name + "::" + field.Name;
        if (owner.EditorOnly)
        {
            throw new MetadataException($"{symbol}: a field of the EditorOnly type {owner.Name} cannot be visible to Sparks", "RMC2607",
                "Editor-only types are not in shipping builds; remove ScriptReadOnly/ScriptWritable from the field.").At(field.Site);
        }

        if (!owner.ScriptType)
        {
            throw new MetadataException($"{symbol}: the field is visible to Sparks but {owner.Name} is not a Sparks type", "RMC2608",
                "Add ScriptType (or ScriptExtensible) to the type, or remove ScriptReadOnly/ScriptWritable from the field.").At(field.Site);
        }

        if (field.Spec.EditorOnly)
        {
            throw new MetadataException($"{symbol}: an EditorOnly field cannot be visible to Sparks", "RMC2607",
                "Editor-only fields are not in shipping builds; remove ScriptReadOnly/ScriptWritable or EditorOnly.").At(field.Site);
        }

        string name = FieldName(field) ?? throw new MetadataException(
            $"{symbol}: Sparks field name from '{field.EffectiveToolName}' must be letters and digits; name it with ScriptName", "RMC2604",
            "Add ScriptName = \"PascalName\" (SL 10.3: underscores other than a trailing one need an explicit Sparks name).").At(field.Site);
        if (Kind(schema, owner) == "object" && names.HookFor(name) is string hook)
        {
            throw new MetadataException($"{symbol}: Sparks field name '{name}' collides with the lifecycle hook '{hook}'", "RMC2606",
                "Rename the field for Sparks with ScriptName = \"...\" (hook names, On + hook and Unity hook names are reserved on engine object types).").At(field.Site);
        }

        _ = Notation(schema, field, symbol, topLevel: true);
    }

    // 노출 타입의 보이는 사슬(자기와 노출된 기반 — 끊긴 곳에서 멈춘다)의 노출 필드 이름은 겹치지 않는다(B5).
    private static void CheckFieldNames(SchemaModel schema, ClassModel type)
    {
        HashSet<string> chain = new(StringComparer.Ordinal) { type.Name };
        for (ClassModel current = type; current.Base.Length != 0 && current.Base != ObjectRoot;)
        {
            ClassModel parent = schema.Find(current.Base);
            if (!parent.ScriptType)
            {
                break;
            }

            chain.Add(parent.Name);
            current = parent;
        }

        Dictionary<string, FieldModel> seen = new(StringComparer.Ordinal);
        foreach (FieldModel field in type.Fields.Where(field => field.Spec.ScriptVisible && chain.Contains(field.Owner)))
        {
            string? name = FieldName(field);
            if (name is null)
            {
                continue;
            }

            if (!seen.TryAdd(name, field))
            {
                FieldModel other = seen[name];
                throw new MetadataException($"{type.Name}: Sparks field name '{name}' is used by {other.Owner}::{other.Name} and {field.Owner}::{field.Name}", "RMC2605",
                    "Give one of the fields a different Sparks name with ScriptName = \"...\".").At(field.Owner == type.Name ? field.Site : type.Site);
            }
        }
    }

    // 타입의 Sparks 이름이다(실효 타입 도구 이름 — B5).
    private static string TypeName(ClassModel type) => type.EffectiveToolName;

    // 필드의 Sparks 이름이다: ScriptName, 없으면 도구 이름(끝의 `_`를 뗀 것)의 첫 글자를 대문자로 바꾼 것. 그 밖의 `_`가 있거나 형식이
    // 맞지 않으면 null이다(B5 — SL 10.3절).
    private static string? FieldName(FieldModel field)
    {
        if (field.Spec.ScriptName.Length != 0)
        {
            return field.Spec.ScriptName;
        }

        string tool = field.EffectiveToolName;
        if (tool.EndsWith('_'))
        {
            tool = tool[..^1];
        }

        if (tool.Length == 0 || !char.IsAsciiLetter(tool[0]))
        {
            return null;
        }

        string name = char.ToUpperInvariant(tool[0]) + tool[1..];
        return Specifiers.IsScriptName(name) ? name : null;
    }

    // 필드 타입의 Sparks 표기다(B4 표). 최상위에서만 `?`가 붙는다 — 타입 인자 안의 참조는 `T`·`Soft<T>`다.
    private static string Notation(SchemaModel schema, FieldModel field, string symbol, bool topLevel)
    {
        string Unsupported(string what) => throw new MetadataException($"{symbol}: {what} cannot be shown to Sparks", "RMC2601",
            "Sparks has no matching type (SparksBinding B4); remove ScriptReadOnly/ScriptWritable from the field.").At(field.Site);

        string Element(string kind, string? reference, bool top)
        {
            if (Scalars.TryGetValue(kind, out string? scalar))
            {
                return scalar;
            }

            switch (kind)
            {
                case "Enum":
                    return Target(reference, requireAsset: false);
                case "Struct":
                    {
                        ClassModel target = schema.Find(reference ?? "");
                        if (target.Binds.Length != 0)
                        {
                            return MathMirrors.TryGetValue(target.Binds, out string? builtin) ? builtin : Unsupported($"the value type {target.Binds}");
                        }

                        return Target(reference, requireAsset: false);
                    }

                case "ObjectReference":
                case "WeakObjectReference":
                    return Target(reference, requireAsset: false) + (top ? "?" : "");
                case "SoftObjectReference":
                    return "Soft<" + Target(reference, requireAsset: true) + ">" + (top ? "?" : "");
                case "ClassReference":
                    return Unsupported("a class reference (Class<T> is full-profile only and has no host yet)");
                default:
                    return Unsupported($"a {kind} value");
            }
        }

        string Target(string? reference, bool requireAsset)
        {
            if (reference is null || reference == ObjectRoot)
            {
                throw new MetadataException($"{symbol}: the referenced type rocks::Object is not a Sparks type", "RMC2602",
                    "Reference a Sparks-visible class instead of rocks::Object.").At(field.Site);
            }

            ClassModel target = schema.Find(reference);
            if (target.Binds.Length != 0)
            {
                return Unsupported($"the value type {target.Binds}");
            }

            if (!target.ScriptType)
            {
                throw new MetadataException($"{symbol}: the field type {target.Name} is not a Sparks type", "RMC2602",
                    $"Add ScriptType to {target.Name}, or remove ScriptReadOnly/ScriptWritable from the field.").At(field.Site);
            }

            if (requireAsset && Kind(schema, target) != "asset")
            {
                throw new MetadataException($"{symbol}: Soft<T> needs an asset type, but {target.Name} is not an asset", "RMC2602",
                    "Use a soft reference to a class derived from rocks::Asset, or a plain object reference.").At(field.Site);
            }

            return TypeName(target);
        }

        switch (field.Kind)
        {
            case "Optional":
                return topLevel ? Element(field.ElementKind ?? "", field.Reference, top: false) + "?" : Unsupported("a nested Optional");
            case "Array":
                return topLevel ? "Array<" + Element(field.ElementKind ?? "", field.Reference, top: false) + ">" : Unsupported("a nested container");
            case "Set":
                return topLevel ? "Set<" + Element(field.ElementKind ?? "", field.Reference, top: false) + ">" : Unsupported("a nested container");
            case "Map":
                return topLevel
                    ? "Map<" + Element(field.KeyKind ?? "", field.KeyReference, top: false) + ", " + Element(field.ElementKind ?? "", field.Reference, top: false) + ">"
                    : Unsupported("a nested container");
            default:
                return Element(field.Kind, field.Reference, topLevel);
        }
    }

    // schema 타입 종류다(B3): 클래스는 기반 사슬에 rocks::Asset이 있으면 asset, 아니면 object.
    private static string Kind(SchemaModel schema, ClassModel type)
    {
        if (type.IsEnum)
        {
            return "enum";
        }

        if (type.IsStruct)
        {
            return "struct";
        }

        for (ClassModel current = type; ;)
        {
            if (current.Name == AssetRoot)
            {
                return "asset";
            }

            if (current.Base.Length == 0 || current.Base == ObjectRoot)
            {
                return "object";
            }

            current = schema.Find(current.Base);
        }
    }

    // 검사를 마친 모델의 schema byte다(B8 — 타입은 Sparks 이름 byte 순, 필드는 선언 순서, 기본값인 선택 key는 쓰지 않는다).
    public static byte[] Write(string module, SchemaModel schema)
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter json = new(stream, new JsonWriterOptions { Indented = true, NewLine = "\n", Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
        {
            json.WriteStartObject();
            json.WriteNumber("format", FormatVersion);
            json.WriteString("package", Package);
            json.WriteString("module", module);
            json.WriteStartArray("types");
            foreach (ClassModel type in schema.Ordered.Where(type => type.ScriptType).OrderBy(TypeName, StringComparer.Ordinal))
            {
                WriteType(json, schema, type);
            }

            json.WriteEndArray();
            json.WriteEndObject();
        }

        string text = new UTF8Encoding(false, true).GetString(stream.ToArray()) + "\n";
        return new UTF8Encoding(false, true).GetBytes(text);
    }

    private static void WriteType(Utf8JsonWriter json, SchemaModel schema, ClassModel type)
    {
        json.WriteStartObject();
        json.WriteString("name", TypeName(type));
        json.WriteString("id", type.Id);
        json.WriteString("kind", Kind(schema, type));
        if (!type.IsEnum && type.Base.Length != 0 && type.Base != ObjectRoot && schema.Find(type.Base).ScriptType)
        {
            json.WriteString("base", TypeName(schema.Find(type.Base)));
        }

        if (type.ScriptExtensible && !type.IsEnum)
        {
            json.WriteBoolean("scriptExtensible", true);
        }

        // 엔진 struct의 이름 지정 생성은 host의 struct 초기화(EI2)와 함께 정한다(B3).
        if (type.IsStruct)
        {
            json.WriteBoolean("constructible", false);
        }

        if (type.IsEnum)
        {
            json.WriteStartArray("cases");
            foreach (EnumeratorModel value in type.Enumerators.Where(value => !value.Hidden))
            {
                json.WriteStringValue(value.EffectiveToolName);
            }

            json.WriteEndArray();
        }
        else
        {
            List<FieldModel> fields = type.OwnFields.Where(field => field.Spec.ScriptVisible).ToList();
            if (fields.Count != 0)
            {
                json.WriteStartArray("fields");
                foreach (FieldModel field in fields)
                {
                    json.WriteStartObject();
                    json.WriteString("name", FieldName(field));
                    json.WriteString("id", field.Id);
                    json.WriteString("type", Notation(schema, field, type.Name + "::" + field.Name, topLevel: true));
                    if (field.Spec.ScriptWritable)
                    {
                        json.WriteString("access", "readWrite");
                    }

                    json.WriteEndObject();
                }

                json.WriteEndArray();
            }
        }

        json.WriteEndObject();
    }
}
