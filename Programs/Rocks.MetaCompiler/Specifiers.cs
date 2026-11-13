using System.Text.Json;

namespace Rocks.MetaCompiler;

// CLASS 지정자 결과다. Version은 Record schema version(없으면 1), WorldOnly는 이 선언이 직접 쓴 값이다(상속은
// SchemaValidator가 base에서 더한다).
internal sealed record ClassSpec(bool Abstract, uint Version, bool WorldOnly);

// STRUCT 지정자 결과다. Version은 Record schema version(없으면 1)이다. Binds(Core 값 타입 결합 — Docs/Design/
// ReflectionFoundations.ko.md 4.5절)는 B4c에서 받으므로 지금은 지정하면 오류다.
internal sealed record StructSpec(uint Version);

// PROPERTY 지정자 결과다. ToolName은 Name="…"의 값이고 필드 이름과 같거나 없으면 ""다(기본 도구 이름은 필드
// 이름이다). MaxElements는 배열 필드 전용이라 0이 아니면 SchemaValidator가 배열이 아닌 필드에서 거부한다.
internal sealed record PropertySpec(bool Transient, bool ReadOnly, string Category, string DisplayName)
{
    public bool EditorOnly { get; init; }

    public bool Required { get; init; }

    public string ToolName { get; init; } = "";

    public uint MaxElements { get; init; }
}

// ENUM 지정자 결과다. ToolPrefix는 도구 이름 조회가 앞에 붙어 있어도 받는 접두([A-Za-z0-9_]+, 없으면 "")이고,
// Flags는 비트 플래그 enum(값은 선언 값의 OR)이다.
internal sealed record EnumSpec(string ToolPrefix, bool Flags);

// ENUMERATOR 지정자 결과다. ToolName은 Name="…"의 값이고 C++ 이름과 같거나 없으면 ""다. Hidden은 저장 값으로는
// 유효하지만 도구 설정이 받지 않는 열거자다.
internal sealed record EnumeratorSpec(string ToolName, string DisplayName, bool Hidden);

// C++ 원문 파서가 아니다. Clang이 확정한 annotation 문자열의 작은 specifier 문법만 처리한다(Docs/Design/
// ReflectionFoundations.ko.md 2.1절). 값은 JSON escape 문자열 또는 부호 없는 10진 정수(앞자리 0 금지, 1–2³¹−1)다.
internal static class Specifiers
{
    private sealed record Value(string? Text, uint? Number);

    public static ClassSpec Class(string text, string symbol)
    {
        Dictionary<string, Value?> values = Parse(text, symbol);
        foreach ((string key, Value? value) in values)
        {
            bool flag = key is "Abstract" or "WorldOnly";
            bool number = key is "Version";
            if ((!flag && !number) || (flag && value is not null) || (number && value?.Number is null))
            {
                throw new MetadataException($"{symbol}: unsupported CLASS specifier/value {key}");
            }
        }

        return new(values.ContainsKey("Abstract"), values.GetValueOrDefault("Version")?.Number ?? 1, values.ContainsKey("WorldOnly"));
    }

    public static StructSpec Struct(string text, string symbol)
    {
        const string hint = "ROCKS_STRUCT takes only Version=N (an unsigned decimal integer); CLASS specifiers such as Abstract or WorldOnly do not apply to structs.";
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
            bool number = key is "Version";
            bool label = key is "Binds";
            if ((!number && !label) || (number && value?.Number is null) || (label && value?.Text is null))
            {
                throw new MetadataException($"{symbol}: unsupported STRUCT specifier/value {key}", hint: hint);
            }
        }

        if (values.ContainsKey("Binds"))
        {
            throw new MetadataException($"{symbol}: STRUCT specifier Binds is not supported yet (Core value type bindings arrive with CoreTypeBindings)",
                hint: "Remove Binds; reflect a plain struct with ROCKS_PROPERTY members instead of binding a Core value type.");
        }

        return new(values.GetValueOrDefault("Version")?.Number ?? 1);
    }

    public static PropertySpec Property(string text, string symbol, string fieldName)
    {
        Dictionary<string, Value?> values = Parse(text, symbol);
        foreach ((string key, Value? value) in values)
        {
            bool flag = key is "Transient" or "ReadOnly" or "EditorOnly" or "Required";
            bool label = key is "Category" or "DisplayName" or "Name";
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

        return new(values.ContainsKey("Transient"), values.ContainsKey("ReadOnly"),
            values.GetValueOrDefault("Category")?.Text ?? "", values.GetValueOrDefault("DisplayName")?.Text ?? "")
        {
            EditorOnly = values.ContainsKey("EditorOnly"),
            Required = values.ContainsKey("Required"),
            // 필드 이름과 같은 Name은 지정하지 않은 것과 같다(모듈 사이 상속 필드의 도구 이름이 같은 바이트가 되게).
            ToolName = toolName == fieldName ? "" : toolName,
            MaxElements = values.GetValueOrDefault("MaxElements")?.Number ?? 0
        };
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
            bool flag = key is "Flags";
            bool label = key is "ToolPrefix";
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

        return new(prefix, values.ContainsKey("Flags"));
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

    // enum 도구 이름·접두의 문자 집합이다: [A-Za-z0-9_]+ (UE의 접두를 뗀 이름 "8BitData"처럼 숫자로 시작할 수 있다).
    public static bool IsEnumToolName(string text) =>
        text.Length != 0 && text.All(c => char.IsAsciiLetterOrDigit(c) || c == '_');

    public static bool IsAsciiIdentifier(string text) =>
        text.Length != 0 && (char.IsAsciiLetter(text[0]) || text[0] == '_') && text.All(c => char.IsAsciiLetterOrDigit(c) || c == '_');

    private static Dictionary<string, Value?> Parse(string text, string symbol)
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
