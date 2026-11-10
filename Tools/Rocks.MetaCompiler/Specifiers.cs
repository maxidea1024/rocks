using System.Text.Json;

namespace Rocks.MetaCompiler;

// C++ 원문 파서가 아니다. Clang이 확정한 annotation 문자열의 작은 specifier 문법만 처리한다.
internal static class Specifiers
{
    public static bool Class(string text, string symbol)
    {
        Dictionary<string, string?> values = Parse(text, symbol);
        foreach ((string key, string? value) in values)
        {
            if (key != "Abstract" || value is not null)
                throw new MetadataException($"{symbol}: unsupported CLASS specifier {key}");
        }
        return values.ContainsKey("Abstract");
    }

    public static PropertySpec Property(string text, string symbol)
    {
        Dictionary<string, string?> values = Parse(text, symbol);
        foreach ((string key, string? value) in values)
        {
            bool flag = key is "Transient" or "ReadOnly";
            bool label = key is "Category" or "DisplayName";
            if ((!flag && !label) || (flag && value is not null) || (label && value is null))
                throw new MetadataException($"{symbol}: unsupported PROPERTY specifier/value {key}");
        }
        return new(values.ContainsKey("Transient"), values.ContainsKey("ReadOnly"),
            values.GetValueOrDefault("Category") ?? "", values.GetValueOrDefault("DisplayName") ?? "");
    }

    private static Dictionary<string, string?> Parse(string text, string symbol)
    {
        Dictionary<string, string?> result = new(StringComparer.Ordinal);
        int cursor = 0;
        void Space() { while (cursor < text.Length && char.IsWhiteSpace(text[cursor])) ++cursor; }
        Space();
        while (cursor < text.Length)
        {
            int start = cursor;
            while (cursor < text.Length && (char.IsAsciiLetterOrDigit(text[cursor]) || text[cursor] == '_')) ++cursor;
            if (start == cursor || !char.IsAsciiLetter(text[start]))
                throw new MetadataException($"{symbol}: invalid specifier syntax");
            string name = text[start..cursor];
            Space();
            string? value = null;
            if (cursor < text.Length && text[cursor] == '=')
            {
                ++cursor;
                Space();
                if (cursor == text.Length || text[cursor] != '"')
                    throw new MetadataException($"{symbol}: specifier values must be quoted strings");
                start = cursor++;
                bool closed = false;
                while (cursor < text.Length)
                {
                    char next = text[cursor++];
                    if (next == '\\')
                    {
                        if (cursor == text.Length) break;
                        ++cursor;
                    }
                    else if (next == '"') { closed = true; break; }
                }
                if (!closed) throw new MetadataException($"{symbol}: unterminated specifier value");
                try { value = JsonSerializer.Deserialize<string>(text[start..cursor]); }
                catch (JsonException) { throw new MetadataException($"{symbol}: specifier escape must use JSON string spelling"); }
                if (value is null || value.Contains('\0'))
                    throw new MetadataException($"{symbol}: invalid specifier text");
                Space();
            }
            if (!result.TryAdd(name, value)) throw new MetadataException($"{symbol}: duplicate specifier {name}");
            if (cursor == text.Length) break;
            if (text[cursor++] != ',') throw new MetadataException($"{symbol}: expected comma between specifiers");
            Space();
            if (cursor == text.Length) throw new MetadataException($"{symbol}: trailing specifier comma");
        }
        return result;
    }
}
