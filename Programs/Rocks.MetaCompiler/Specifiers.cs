using System.Text.Json;

namespace Rocks.MetaCompiler;

// CLASS 지정자 결과다. Version은 Record schema version(없으면 1), WorldOnly는 이 선언이 직접 쓴 값이다(상속은
// SchemaValidator가 base에서 더한다). ToolName은 Name="…"의 타입 도구 이름(없으면 "" — 기본은 한정하지 않은 C++ 이름,
// Docs/Design/ReflectionExtensions.ko.md 5.2절)이다.
internal sealed record ClassSpec(bool Abstract, uint Version, bool WorldOnly, string ToolName = "")
{
    // 클래스 플래그 지정자다(리플렉션 B9h2 — Docs/Design/ReflectionExtensions.ko.md 9.2절): 저장 불가 클래스(Transient·Deprecated —
    // 상속된다)와 Within="…"(Outer가 그 클래스 또는 파생이어야 한다 — 철자는 Clang이 정규 이름으로 푼다).
    public bool Transient { get; init; }

    public bool Deprecated { get; init; }

    // 쿠킹 데이터에 없는 클래스(TypeFlags::EditorOnly — 상속된다, Docs/Design/AssetSystem 10.2절 A4e2a)다.
    public bool EditorOnly { get; init; }

    public string Within { get; init; } = "";

    // Sparks 노출 지정자다(Docs/Design/SparksBinding.ko.md B1 — UE BlueprintType·NotBlueprintType·Blueprintable·NotBlueprintable).
    // 선언이 직접 쓴 값이고, 상속을 합친 실효 값은 SparksBinding.Resolve가 정한다.
    public ScriptTypeSpec Script { get; init; } = ScriptTypeSpec.None;
}

// 타입의 Sparks 노출 지정자다(Docs/Design/SparksBinding.ko.md B1). Extensible·NotExtensible은 클래스만 쓴다.
internal sealed record ScriptTypeSpec(bool Type, bool NotType, bool Extensible, bool NotExtensible)
{
    public static readonly ScriptTypeSpec None = new(false, false, false, false);
}

// STRUCT 지정자 결과다. Version은 Record schema version(없으면 1)이다. Binds는 미러 선언이 결합하는 실제 C++ 타입의
// 원문 철자다(Core 값 타입 결합 — Docs/Design/ReflectionFoundations.ko.md 4.5절, 없으면 ""). 정규 철자는 Clang이
// 정한다(문자열로 비교하지 않는다). Atomic은 TypeFlags::Atomic(값이 통째 단위 — Docs/Design/ReflectionExtensions.ko.md
// 3.1절)이고 struct 전용이다(CLASS 지정자 표에는 없으므로 ROCKS_CLASS(Atomic)은 모르는 지정자다). ToolName은 Name="…"의
// 타입 도구 이름(없으면 "")이다.
internal sealed record StructSpec(uint Version, string Binds, bool Atomic, string ToolName = "")
{
    // 설정 struct 지정자 Settings(Branch="…", Section="…")다(Docs/Design/ConfigSystem.ko.md 7절). 둘 다 비어 있으면 설정 struct가 아니다.
    // 생성 GeneratedType이 SettingsSchema(Meta/SettingsSchema.h)를 더 낸다.
    public string SettingsBranch { get; init; } = "";

    public string SettingsSection { get; init; } = "";

    // Sparks 노출 지정자다(ScriptType·NotScriptType — Docs/Design/SparksBinding.ko.md B1).
    public ScriptTypeSpec Script { get; init; } = ScriptTypeSpec.None;
}

// PROPERTY 지정자 결과다. ToolName은 Name="…"의 값이고 필드 이름과 같거나 없으면 ""다(기본 도구 이름은 필드
// 이름이다). MaxElements는 배열 필드 전용이라 0이 아니면 SchemaValidator가 배열이 아닌 필드에서 거부한다.
internal sealed record PropertySpec(bool Transient, bool ReadOnly, string Category, string DisplayName)
{
    public bool EditorOnly { get; init; }

    public bool Required { get; init; }

    // 필드 플래그 지정자다(리플렉션 B9h1 — Docs/Design/ReflectionExtensions.ko.md 9.1절): 복제 저장·복사 텍스트에서 뺀다
    // (DuplicateTransient), Tryout이 아닌 복제·텍스트에서 뺀다(NonTryoutDuplicateTransient), 세이브게임 저장 포함 필터(SaveGame — 멤버마다
    // 표시), 복사 텍스트에서 뺀다(TextExportTransient).
    public bool DuplicateTransient { get; init; }

    public bool NonTryoutDuplicateTransient { get; init; }

    public bool SaveGame { get; init; }

    public bool TextExportTransient { get; init; }

    // JSON import가 이 Map을 키별로 병합한다(TextMapMergeByKey — Map 필드만, SchemaValidator RMC2202, Docs/Design/AssetSystem 10.3.1절).
    public bool TextMapMergeByKey { get; init; }

    public string ToolName { get; init; } = "";

    public uint MaxElements { get; init; }

    // 에셋 bundle 이름 목록 AssetBundles="a,b"(Docs/Design/AssetSystem 10.2절 A4e2a — UE meta=(AssetBundles=…))의 정규 텍스트다
    // (이름 앞뒤 공백을 뗀 쉼표 목록, 없으면 ""). 문법은 Specifiers.CanonicalAssetBundles(RMC2212), 필드 종류는 SchemaValidator(RMC2202)가 본다.
    public string AssetBundles { get; init; } = "";

    // 설정 필드 지정자다(Docs/Design/ConfigSystem.ko.md 7절 — 설정 struct의 직접 멤버만, 그 밖은 RMC2210): 사용자 층 쓰기 허용(UserConfig),
    // 콘솔 설정 이름(ConsoleName="render.graph.immediate"), 효력 시점(RestartRequired·StartupOnly — 둘 중 하나), 설정 snapshot에 넣기
    // (RenderRead), 출시 규칙에서 기본값 고정(DevelopmentOnly). 생성 SettingsFieldSchema의 비트는 SettingsFieldFlags다.
    public bool UserConfig { get; init; }

    public string ConsoleName { get; init; } = "";

    public bool RestartRequired { get; init; }

    public bool StartupOnly { get; init; }

    public bool RenderRead { get; init; }

    public bool DevelopmentOnly { get; init; }

    // Sparks 노출 지정자다(Docs/Design/SparksBinding.ko.md B1 — UE BlueprintReadOnly·BlueprintReadWrite, ScriptName은 SL 10.3절).
    // ScriptName은 지정하지 않았으면 ""다.
    public bool ScriptReadOnly { get; init; }

    public bool ScriptWritable { get; init; }

    public string ScriptName { get; init; } = "";

    public bool ScriptVisible => ScriptReadOnly || ScriptWritable;

    public bool HasSettings => UserConfig || ConsoleName.Length != 0 || RestartRequired || StartupOnly || RenderRead || DevelopmentOnly;

    // SettingsFieldFlags 비트: UserConfig 1, RestartRequired 2, StartupOnly 4, RenderRead 8, DevelopmentOnly 16.
    public uint SettingsFlags => (UserConfig ? 1u : 0u) | (RestartRequired ? 2u : 0u) | (StartupOnly ? 4u : 0u) | (RenderRead ? 8u : 0u) |
        (DevelopmentOnly ? 16u : 0u);
}

// FUNCTION 지정자 결과다(Docs/Design/FunctionReflection.ko.md FD12). ToolName은 Name="…"의 값이고 함수 이름과 같거나 없으면 ""다.
internal sealed record FunctionSpec(bool Pure, bool EditorOnly, string ToolName);

// ENUM 지정자 결과다. ToolPrefix는 도구 이름 조회가 앞에 붙어 있어도 받는 접두([A-Za-z0-9_]+, 없으면 "")이고,
// Flags는 비트 플래그 enum(값은 선언 값의 OR)이다.
// Binds는 Core enum 미러가 결합하는 실제 enum의 C++ 철자다(CoreTypeBindings만 — 없으면 "", CurveData CV9).
internal sealed record EnumSpec(string ToolPrefix, bool Flags, string Binds = "")
{
    // Sparks 노출 지정자 ScriptType이다(enum은 자기 것만 본다 — Docs/Design/SparksBinding.ko.md B1).
    public bool ScriptType { get; init; }
}

// ENUMERATOR 지정자 결과다. ToolName은 Name="…"의 값이고 C++ 이름과 같거나 없으면 ""다. Hidden은 저장 값으로는
// 유효하지만 도구 설정이 받지 않는 열거자다.
internal sealed record EnumeratorSpec(string ToolName, string DisplayName, bool Hidden);

// C++ 원문 파서가 아니다. Clang이 확정한 annotation 문자열의 작은 specifier 문법만 처리한다(Docs/Design/
// ReflectionFoundations.ko.md 2.1절). 값은 JSON escape 문자열 또는 부호 없는 10진 정수(앞자리 0 금지, 1–2³¹−1)다.
internal static class Specifiers
{
    // 값은 문자열(Text)·정수(Number)·묶음(Group — `Settings(Branch="…", Section="…")`처럼 괄호 안의 지정자 목록) 중 하나다. 묶음은
    // 받는 지정자(STRUCT Settings)만 쓰고, 그 밖의 자리에서는 Text·Number가 없어 모르는 값으로 거부된다.
    private sealed record Value(string? Text, uint? Number, Dictionary<string, Value?>? Group = null);

    public static ClassSpec Class(string text, string symbol)
    {
        Dictionary<string, Value?> values = Parse(text, symbol);
        foreach ((string key, Value? value) in values)
        {
            bool flag = key is "Abstract" or "WorldOnly" or "Transient" or "Deprecated" or "EditorOnly" or "ScriptType" or "NotScriptType" or
                "ScriptExtensible" or "NotScriptExtensible";
            bool number = key is "Version";
            bool label = key is "Name" or "Within";
            if ((!flag && !number && !label) || (flag && value is not null) || (number && value?.Number is null) || (label && value?.Text is null))
            {
                throw new MetadataException($"{symbol}: unsupported CLASS specifier/value {key}");
            }
        }

        string within = values.GetValueOrDefault("Within")?.Text ?? "";
        if (values.ContainsKey("Within") && within.Trim().Length == 0)
        {
            throw new MetadataException($"{symbol}: Within specifier must name a reflected class", "RMC2208",
                "Write Within = \"ns::OuterClass\" with the class every Outer of this class must be (or derive from).");
        }

        return new(values.ContainsKey("Abstract"), values.GetValueOrDefault("Version")?.Number ?? 1, values.ContainsKey("WorldOnly"),
            TypeName(values, symbol))
        {
            Transient = values.ContainsKey("Transient"),
            Deprecated = values.ContainsKey("Deprecated"),
            EditorOnly = values.ContainsKey("EditorOnly"),
            Within = within,
            Script = ScriptType(values, symbol, classes: true)
        };
    }

    public static StructSpec Struct(string text, string symbol)
    {
        const string hint = "ROCKS_STRUCT takes Version=N (an unsigned decimal integer), Atomic (no value), Name=\"<type tool name>\" and, in CoreTypeBindings only, Binds=\"<C++ type>\"; CLASS specifiers such as Abstract or WorldOnly do not apply to structs.";
        Dictionary<string, Value?> values;
        try
        {
            values = Parse(text, symbol);
        }
        catch (MetadataException error)
        {
            throw new MetadataException(error.Message, error.Code, hint);
        }

        foreach ((string key, Value? value) in values)
        {
            bool flag = key is "Atomic" or "ScriptType" or "NotScriptType";
            bool number = key is "Version";
            bool label = key is "Binds" or "Name";
            bool group = key is "Settings";
            if ((!flag && !number && !label && !group) || (flag && value is not null) || (number && value?.Number is null) || (label && value?.Text is null) ||
                (group && value?.Group is null))
            {
                throw new MetadataException($"{symbol}: unsupported STRUCT specifier/value {key}", hint: hint);
            }
        }

        string binds = values.GetValueOrDefault("Binds")?.Text ?? "";
        if (values.ContainsKey("Binds") && !StructRules.IsBindsSpelling(binds))
        {
            throw new MetadataException($"{symbol}: Binds must be a C++ type spelling (qualified names and template arguments only): \"{binds}\"",
                hint: "Write the real type fully qualified from the global scope, e.g. Binds = \"rocks::Vector3<float>\" or Binds = \"rocks::Vec3\".");
        }

        (string branch, string section) = values.GetValueOrDefault("Settings")?.Group is { } settings ? SettingsGroup(settings, symbol) : ("", "");
        return new(values.GetValueOrDefault("Version")?.Number ?? 1, binds, values.ContainsKey("Atomic"), TypeName(values, symbol))
        {
            SettingsBranch = branch,
            SettingsSection = section,
            Script = ScriptType(values, symbol, classes: false)
        };
    }

    // Settings(Branch="…", Section="…")의 두 값이다(Docs/Design/ConfigSystem.ko.md 3.2·7절). 둘 다 필수인 ASCII 식별자이고 Section
    // `Settings`는 콘솔 설정 이름 → 값 섹션으로 예약이다(대소문자 무시). branch 이름 표 판정은 설정 계층이 등록 때 한다(플러그인이 branch를
    // 더할 수 있다).
    private static (string Branch, string Section) SettingsGroup(Dictionary<string, Value?> group, string symbol)
    {
        const string hint = "Write ROCKS_STRUCT(Settings(Branch = \"Render\", Section = \"RenderGraph\")): Branch is the config file name and Section is the top-level key of that file; both are ASCII identifiers.";
        foreach ((string key, Value? value) in group)
        {
            if (key is not ("Branch" or "Section") || value?.Text is null)
            {
                throw new MetadataException($"{symbol}: unsupported Settings specifier/value {key}", "RMC2210", hint);
            }
        }

        string branch = group.GetValueOrDefault("Branch")?.Text ?? "";
        string section = group.GetValueOrDefault("Section")?.Text ?? "";
        if (!IsAsciiIdentifier(branch) || !IsAsciiIdentifier(section))
        {
            throw new MetadataException($"{symbol}: Settings needs Branch and Section as ASCII identifiers ([A-Za-z_][A-Za-z0-9_]*): Branch=\"{branch}\", Section=\"{section}\"", "RMC2210", hint);
        }

        if (string.Equals(section, "Settings", StringComparison.OrdinalIgnoreCase))
        {
            throw new MetadataException($"{symbol}: Section \"{section}\" is reserved for console setting values", "RMC2210",
                "Choose another section name; the \"Settings\" key of a config file maps console setting names to values.");
        }

        return (branch, section);
    }

    // 콘솔 설정 이름 문법이다(Docs/Design/ConfigSystem.ko.md 5.1절 — 결정 D7): 소문자 점 경로 `[a-z][a-z0-9_]*('.' [a-z][a-z0-9_]*)+`(마디
    // 둘 이상). 첫 마디(영역 이름 공간)가 등록 표에 있는지는 설정 계층이 등록 때 본다. 런타임 IsConsoleSettingName과 같은 규칙이다.
    public static bool IsConsoleName(string text)
    {
        string[] segments = text.Split('.');
        return segments.Length >= 2 && segments.All(segment => segment.Length != 0 && char.IsAsciiLetterLower(segment[0]) &&
            segment.All(c => char.IsAsciiLetterLower(c) || char.IsAsciiDigit(c) || c == '_'));
    }

    public static PropertySpec Property(string text, string symbol, string fieldName)
    {
        Dictionary<string, Value?> values = Parse(text, symbol);
        foreach ((string key, Value? value) in values)
        {
            bool flag = key is "Transient" or "ReadOnly" or "EditorOnly" or "Required" or "DuplicateTransient" or "NonTryoutDuplicateTransient" or
                "SaveGame" or "TextExportTransient" or "TextMapMergeByKey" or "UserConfig" or "RestartRequired" or "StartupOnly" or "RenderRead" or "DevelopmentOnly" or
                "ScriptReadOnly" or "ScriptWritable";
            bool label = key is "Category" or "DisplayName" or "Name" or "ConsoleName" or "AssetBundles" or "ScriptName";
            bool number = key is "MaxElements";
            if ((!flag && !label && !number) || (flag && value is not null) || (label && value?.Text is null) ||
                (number && value?.Number is null))
            {
                throw new MetadataException($"{symbol}: unsupported PROPERTY specifier/value {key}");
            }
        }

        string toolName = values.GetValueOrDefault("Name")?.Text ?? "";
        if (values.ContainsKey("Name") && !IsAsciiIdentifier(toolName))
        {
            throw new MetadataException($"{symbol}: Name specifier must be a non-empty ASCII identifier ([A-Za-z_][A-Za-z0-9_]*): \"{toolName}\"", "RMC2203",
                "Use an ASCII identifier for the tool name, or omit Name to use the field name.");
        }

        string consoleName = values.GetValueOrDefault("ConsoleName")?.Text ?? "";
        if (values.ContainsKey("ConsoleName") && !IsConsoleName(consoleName))
        {
            throw new MetadataException($"{symbol}: ConsoleName must be a lowercase dotted path with at least two segments ([a-z][a-z0-9_]* joined by '.'): \"{consoleName}\"", "RMC2210",
                "Write the console setting name as <area>.<name>, e.g. ConsoleName = \"render.graph.immediate\".");
        }

        string assetBundles = "";
        if (values.GetValueOrDefault("AssetBundles")?.Text is string bundleText)
        {
            assetBundles = CanonicalAssetBundles(bundleText) ?? throw new MetadataException(
                $"{symbol}: AssetBundles must list 1 to {AssetBundleMaxNames} distinct ASCII identifiers of at most {AssetBundleNameMaxBytes} bytes separated by commas: \"{bundleText}\"",
                "RMC2212", "Write AssetBundles = \"UI,Game\" with identifier names ([A-Za-z_][A-Za-z0-9_]*) and no repeated name.");
        }

        // Sparks 노출 지정자다(Docs/Design/SparksBinding.ko.md B1·B10 — RMC2603·RMC2604).
        bool scriptReadOnly = values.ContainsKey("ScriptReadOnly");
        bool scriptWritable = values.ContainsKey("ScriptWritable");
        string scriptName = values.GetValueOrDefault("ScriptName")?.Text ?? "";
        if (scriptReadOnly && scriptWritable)
        {
            throw new MetadataException($"{symbol}: ScriptReadOnly and ScriptWritable cannot be combined", "RMC2603",
                "Choose ScriptReadOnly (scripts read the field) or ScriptWritable (scripts read and write it).");
        }

        if (scriptWritable && values.ContainsKey("ReadOnly"))
        {
            throw new MetadataException($"{symbol}: a ReadOnly field cannot be ScriptWritable", "RMC2603",
                "Reflection writes refuse ReadOnly fields; use ScriptReadOnly, or remove ReadOnly if scripts should write the field.");
        }

        if (values.ContainsKey("ScriptName") && !scriptReadOnly && !scriptWritable)
        {
            throw new MetadataException($"{symbol}: ScriptName needs ScriptReadOnly or ScriptWritable", "RMC2603",
                "ScriptName renames a field that scripts can see; add ScriptReadOnly or ScriptWritable, or remove ScriptName.");
        }

        if (values.ContainsKey("ScriptName") && !IsScriptName(scriptName))
        {
            throw new MetadataException($"{symbol}: ScriptName must be a PascalCase ASCII name (an uppercase letter, then letters and digits): \"{scriptName}\"", "RMC2604",
                "Write the Sparks name as letters and digits starting with an uppercase letter, e.g. ScriptName = \"MaxHealth\".");
        }

        if (values.ContainsKey("RestartRequired") && values.ContainsKey("StartupOnly"))
        {
            throw new MetadataException($"{symbol}: RestartRequired and StartupOnly cannot be combined", "RMC2210",
                "Choose one apply timing: RestartRequired (a change takes effect at the next start) or StartupOnly (the value cannot change after startup).");
        }

        return new(values.ContainsKey("Transient"), values.ContainsKey("ReadOnly"),
            values.GetValueOrDefault("Category")?.Text ?? "", values.GetValueOrDefault("DisplayName")?.Text ?? "")
        {
            EditorOnly = values.ContainsKey("EditorOnly"),
            Required = values.ContainsKey("Required"),
            DuplicateTransient = values.ContainsKey("DuplicateTransient"),
            NonTryoutDuplicateTransient = values.ContainsKey("NonTryoutDuplicateTransient"),
            SaveGame = values.ContainsKey("SaveGame"),
            TextExportTransient = values.ContainsKey("TextExportTransient"),
            TextMapMergeByKey = values.ContainsKey("TextMapMergeByKey"),
            // 필드 이름과 같은 Name은 지정하지 않은 것과 같다(모듈 사이 상속 필드의 도구 이름이 같은 바이트가 되게).
            ToolName = toolName == fieldName ? "" : toolName,
            MaxElements = values.GetValueOrDefault("MaxElements")?.Number ?? 0,
            AssetBundles = assetBundles,
            UserConfig = values.ContainsKey("UserConfig"),
            ConsoleName = consoleName,
            RestartRequired = values.ContainsKey("RestartRequired"),
            StartupOnly = values.ContainsKey("StartupOnly"),
            RenderRead = values.ContainsKey("RenderRead"),
            DevelopmentOnly = values.ContainsKey("DevelopmentOnly"),
            ScriptReadOnly = scriptReadOnly,
            ScriptWritable = scriptWritable,
            ScriptName = scriptName
        };
    }

    // ROCKS_FUNCTION 지정자다(Pure·EditorOnly·Name — 그 밖은 뒤 단계가 받는다: DisplayName·Category·Help(FR2)·ScriptOverridable(FR3)·
    // Cheat·Authority(CVR8)·Sparks 함수 지정자(EI1b) — 지금은 모르는 지정자 RMC2000).
    public static FunctionSpec Function(string text, string symbol, string functionName)
    {
        Dictionary<string, Value?> values = Parse(text, symbol);
        foreach ((string key, Value? value) in values)
        {
            bool flag = key is "Pure" or "EditorOnly";
            bool label = key is "Name";
            if ((!flag && !label) || (flag && value is not null) || (label && value?.Text is null))
            {
                throw new MetadataException($"{symbol}: unsupported FUNCTION specifier/value {key}",
                    hint: "ROCKS_FUNCTION takes Pure, EditorOnly and Name=\"<tool name>\" (display metadata, script overrides and cheat specifiers come with later stages).");
            }
        }

        string toolName = values.GetValueOrDefault("Name")?.Text ?? "";
        if (values.ContainsKey("Name") && !IsAsciiIdentifier(toolName))
        {
            throw new MetadataException($"{symbol}: Name specifier must be a non-empty ASCII identifier ([A-Za-z_][A-Za-z0-9_]*): \"{toolName}\"", "RMC2703",
                "Use an ASCII identifier for the function tool name, or omit Name to use the C++ name.");
        }

        return new(values.ContainsKey("Pure"), values.ContainsKey("EditorOnly"), toolName == functionName ? "" : toolName);
    }

    // ROCKS_NATIVE는 지정자를 받지 않는다.
    public static void Native(string text, string symbol)
    {
        if (Parse(text, symbol).Count != 0)
        {
            throw new MetadataException($"{symbol}: ROCKS_NATIVE takes no specifiers");
        }
    }

    public static EnumSpec Enum(string text, string symbol)
    {
        Dictionary<string, Value?> values = Parse(text, symbol);
        foreach ((string key, Value? value) in values)
        {
            bool flag = key is "Flags" or "ScriptType";
            bool label = key is "ToolPrefix" or "Binds";
            if ((!flag && !label) || (flag && value is not null) || (label && value?.Text is null))
            {
                throw new MetadataException($"{symbol}: unsupported ENUM specifier/value {key}");
            }
        }

        string prefix = values.GetValueOrDefault("ToolPrefix")?.Text ?? "";
        if (values.ContainsKey("ToolPrefix") && !IsEnumToolName(prefix))
        {
            throw new MetadataException($"{symbol}: ToolPrefix must be non-empty [A-Za-z0-9_]+: \"{prefix}\"", "RMC2203",
                "Use letters, digits and underscores for the tool name prefix, or omit ToolPrefix.");
        }

        string binds = values.GetValueOrDefault("Binds")?.Text ?? "";
        if (values.ContainsKey("Binds") && !StructRules.IsBindsSpelling(binds))
        {
            throw new MetadataException($"{symbol}: Binds must be a C++ type spelling (qualified names and template arguments only): \"{binds}\"",
                hint: "Write the real enum fully qualified from the global scope, e.g. Binds = \"rocks::InterpCurveMode\".");
        }

        return new(prefix, values.ContainsKey("Flags"), binds) { ScriptType = values.ContainsKey("ScriptType") };
    }

    public static EnumeratorSpec Enumerator(string text, string symbol, string name)
    {
        Dictionary<string, Value?> values = Parse(text, symbol);
        foreach ((string key, Value? value) in values)
        {
            bool flag = key is "Hidden";
            bool label = key is "Name" or "DisplayName";
            if ((!flag && !label) || (flag && value is not null) || (label && value?.Text is null))
            {
                throw new MetadataException($"{symbol}: unsupported ENUMERATOR specifier/value {key}");
            }
        }

        string toolName = values.GetValueOrDefault("Name")?.Text ?? "";
        if (values.ContainsKey("Name") && !IsEnumToolName(toolName))
        {
            throw new MetadataException($"{symbol}: Name specifier must be non-empty [A-Za-z0-9_]+: \"{toolName}\"", "RMC2203",
                "Use letters, digits and underscores for the enumerator tool name (a leading digit is allowed), or omit Name.");
        }

        // C++ 이름과 같은 Name은 지정하지 않은 것과 같다.
        return new(toolName == name ? "" : toolName, values.GetValueOrDefault("DisplayName")?.Text ?? "", values.ContainsKey("Hidden"));
    }

    // CLASS·STRUCT의 Sparks 노출 지정자다. 같은 축의 켜기·끄기를 함께 쓰면 RMC2603이다(Docs/Design/SparksBinding.ko.md B10).
    private static ScriptTypeSpec ScriptType(Dictionary<string, Value?> values, string symbol, bool classes)
    {
        ScriptTypeSpec spec = new(values.ContainsKey("ScriptType"), values.ContainsKey("NotScriptType"),
            classes && values.ContainsKey("ScriptExtensible"), classes && values.ContainsKey("NotScriptExtensible"));
        if (spec.Type && spec.NotType)
        {
            throw new MetadataException($"{symbol}: ScriptType and NotScriptType cannot be combined", "RMC2603",
                "Keep ScriptType to show the type to scripts, or NotScriptType to hide it and its derived types.");
        }

        if (spec.Extensible && spec.NotExtensible)
        {
            throw new MetadataException($"{symbol}: ScriptExtensible and NotScriptExtensible cannot be combined", "RMC2603",
                "Keep ScriptExtensible to let Sparks classes derive from the class, or NotScriptExtensible to stop that.");
        }

        if (spec.Extensible && spec.NotType)
        {
            throw new MetadataException($"{symbol}: ScriptExtensible and NotScriptType cannot be combined", "RMC2603",
                "A class Sparks can derive from is visible to scripts; remove NotScriptType or ScriptExtensible.");
        }

        return spec;
    }

    // Sparks 이름(PascalCase ASCII — SL 10.2절 schema 이름 규칙)이다: 영문 대문자로 시작하는 영문자·숫자.
    public static bool IsScriptName(string text) =>
        text.Length != 0 && char.IsAsciiLetterUpper(text[0]) && text.All(char.IsAsciiLetterOrDigit);

    // CLASS·STRUCT의 Name="…"(타입 도구 이름)이다. 형식은 TypeToolName.IsValid이고 `none`은 예약이다(RMC2203). 없으면 ""다.
    private static string TypeName(Dictionary<string, Value?> values, string symbol)
    {
        string toolName = values.GetValueOrDefault("Name")?.Text ?? "";
        if (values.ContainsKey("Name") && !TypeToolName.IsValid(toolName))
        {
            throw new MetadataException($"{symbol}: Name specifier must be a type tool name ([A-Za-z_][A-Za-z0-9_]* segments joined by '.', not 'none'): \"{toolName}\"", "RMC2203",
                "Use ASCII identifier segments separated by dots (for example Name=\"Weapons.Sword\"); 'none' is reserved for null in tool text.");
        }

        return toolName;
    }

    // enum 도구 이름·접두의 문자 집합이다: [A-Za-z0-9_]+ (UE의 접두를 뗀 이름 "8BitData"처럼 숫자로 시작할 수 있다).
    public static bool IsEnumToolName(string text) =>
        text.Length != 0 && text.All(c => char.IsAsciiLetterOrDigit(c) || c == '_');

    // bundle 이름 byte 상한과 목록 이름 수 상한이다(런타임 AssetBundleNameMaxBytes·AssetBundleMaxNames와 같은 값).
    public const int AssetBundleNameMaxBytes = 64;

    public const int AssetBundleMaxNames = 16;

    // AssetBundles 값의 정규 텍스트다: 쉼표로 나눈 이름마다 앞뒤 ASCII 공백을 떼고 다시 잇는다(UE ParseIntoArrayWS처럼 공백을 받는다).
    // 이름이 ASCII 식별자가 아니거나 길이·수가 상한을 넘거나 같은 이름이 둘이면 null이다(런타임 IsAssetBundleList와 같은 규칙).
    public static string? CanonicalAssetBundles(string text)
    {
        string[] names = text.Split(',').Select(name => name.Trim(' ', '\t')).ToArray();
        bool valid = names.Length <= AssetBundleMaxNames && names.All(name => name.Length <= AssetBundleNameMaxBytes && IsAsciiIdentifier(name)) &&
            names.Distinct(StringComparer.Ordinal).Count() == names.Length;
        return valid ? string.Join(',', names) : null;
    }

    public static bool IsAsciiIdentifier(string text) =>
        text.Length != 0 && (char.IsAsciiLetter(text[0]) || text[0] == '_') && text.All(c => char.IsAsciiLetterOrDigit(c) || c == '_');

    private static Dictionary<string, Value?> Parse(string text, string symbol, bool nested = false)
    {
        Dictionary<string, Value?> result = new(StringComparer.Ordinal);
        int cursor = 0;
        void Space()
        {
            while (cursor < text.Length && char.IsWhiteSpace(text[cursor]))
            {
                ++cursor;
            }
        }

        Space();
        while (cursor < text.Length)
        {
            int start = cursor;
            while (cursor < text.Length && (char.IsAsciiLetterOrDigit(text[cursor]) || text[cursor] == '_'))
            {
                ++cursor;
            }

            if (start == cursor || !char.IsAsciiLetter(text[start]))
            {
                throw new MetadataException($"{symbol}: invalid specifier syntax");
            }

            string name = text[start..cursor];
            Space();
            Value? value = null;
            if (cursor < text.Length && text[cursor] == '=')
            {
                ++cursor;
                Space();
                if (cursor < text.Length && char.IsAsciiDigit(text[cursor]))
                {
                    value = new(null, Number(ref cursor));
                }
                else if (cursor < text.Length && text[cursor] == '"')
                {
                    value = new(Quoted(ref cursor), null);
                }
                else
                {
                    throw new MetadataException($"{symbol}: specifier values must be quoted strings or unsigned decimal integers");
                }

                Space();
            }
            else if (cursor < text.Length && text[cursor] == '(')
            {
                // 묶음 값 `Name(...)`이다. 한 단계만 받는다(묶음 안의 묶음은 문법 오류다).
                if (nested)
                {
                    throw new MetadataException($"{symbol}: specifier groups cannot be nested");
                }

                int close = GroupEnd(cursor);
                value = new(null, null, Parse(text[(cursor + 1)..close], symbol, true));
                cursor = close + 1;
                Space();
            }

            if (!result.TryAdd(name, value))
            {
                throw new MetadataException($"{symbol}: duplicate specifier {name}");
            }

            if (cursor == text.Length)
            {
                break;
            }

            if (text[cursor++] != ',')
            {
                throw new MetadataException($"{symbol}: expected comma between specifiers");
            }

            Space();
            if (cursor == text.Length)
            {
                throw new MetadataException($"{symbol}: trailing specifier comma");
            }
        }

        return result;

        // 묶음을 여는 '(' 위치에서 짝이 되는 ')' 위치다(따옴표 안의 괄호·escape는 건너뛴다). 짝이 없으면 문법 오류다.
        int GroupEnd(int open)
        {
            bool quoted = false;
            for (int position = open + 1; position < text.Length; ++position)
            {
                char next = text[position];
                if (quoted)
                {
                    if (next == '\\')
                    {
                        ++position;
                    }
                    else if (next == '"')
                    {
                        quoted = false;
                    }
                }
                else if (next == '"')
                {
                    quoted = true;
                }
                else if (next == ')')
                {
                    return position;
                }
            }

            throw new MetadataException($"{symbol}: unterminated specifier group");
        }

        // 부호 없는 10진 정수: 앞자리 0 금지, 1–2³¹−1.
        uint Number(ref int position)
        {
            int begin = position;
            while (position < text.Length && char.IsAsciiDigit(text[position]))
            {
                ++position;
            }

            string digits = text[begin..position];
            if (digits[0] == '0' || digits.Length > 10 || !uint.TryParse(digits, out uint number) || number > int.MaxValue ||
                (position < text.Length && (char.IsAsciiLetter(text[position]) || text[position] is '_' or '.')))
            {
                throw new MetadataException($"{symbol}: integer specifier values must be decimal 1..2147483647 without leading zeros");
            }

            return number;
        }

        string Quoted(ref int position)
        {
            int begin = position++;
            bool closed = false;
            while (position < text.Length)
            {
                char next = text[position++];
                if (next == '\\')
                {
                    if (position == text.Length)
                    {
                        break;
                    }

                    ++position;
                }
                else if (next == '"')
                {
                    closed = true;
                    break;
                }
            }

            if (!closed)
            {
                throw new MetadataException($"{symbol}: unterminated specifier value");
            }

            try
            {
                return JsonSerializer.Deserialize<string>(text[begin..position]) ?? throw new MetadataException($"{symbol}: invalid specifier text");
            }
            catch (JsonException)
            {
                throw new MetadataException($"{symbol}: specifier escape must use JSON string spelling");
            }
        }
    }
}

// 타입 도구 이름 규칙이다(Docs/Design/ReflectionExtensions.ko.md 5.2절 — 런타임 Meta/TypeSchema.h IsTypeToolName과 같은 문법):
// `[A-Za-z_][A-Za-z0-9_]*('.' [A-Za-z_][A-Za-z0-9_]*)*`, `none`은 대소문자 무시로 예약이다. 기본값은 한정하지 않은 C++ 타입 이름이고
// ROCKS_CLASS(Name="…")·ROCKS_STRUCT(Name="…")로 바꾼다. 클래스·struct가 한 이름 공간이며 모듈과 import 범위에서 대소문자 무시로
// 유일하다(RMC2203 — SchemaValidator).
internal static class TypeToolName
{
    public static bool IsValid(string text) =>
        text.Length != 0 && !string.Equals(text, "none", StringComparison.OrdinalIgnoreCase) && text.Split('.').All(Specifiers.IsAsciiIdentifier);

    // 한정하지 않은 C++ 이름(정규 이름의 마지막 :: 뒤)이다.
    public static string Default(string qualifiedName)
    {
        int separator = qualifiedName.LastIndexOf("::", StringComparison.Ordinal);
        return separator < 0 ? qualifiedName : qualifiedName[(separator + 2)..];
    }
}
