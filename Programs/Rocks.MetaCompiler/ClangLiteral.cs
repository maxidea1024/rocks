using System.Globalization;
using System.Text;

namespace Rocks.MetaCompiler;

// Clang AST StringLiteral.value의 escaped 표기만 해제한다. 원본 C++ 토큰이나 선언은 읽지 않는다.
internal static class ClangLiteral
{
    public static string Decode(string text)
    {
        if (text.Length < 2 || text[0] != '"' || text[^1] != '"')
            throw new MetadataException("annotation strings must be narrow UTF-8 literals");
        List<byte> bytes = [];
        for (int index = 1; index < text.Length - 1; ++index)
        {
            char value = text[index];
            if (value != '\\')
            {
                string scalar = char.IsHighSurrogate(value) && index + 1 < text.Length - 1
                    ? text.Substring(index++, 2) : value.ToString();
                bytes.AddRange(Encoding.UTF8.GetBytes(scalar));
                continue;
            }
            if (++index >= text.Length - 1) throw new MetadataException("invalid Clang string literal");
            char escape = text[index];
            if (escape is >= '0' and <= '7')
            {
                int number = escape - '0';
                int count = 1;
                while (count < 3 && index + 1 < text.Length - 1 && text[index + 1] is >= '0' and <= '7')
                {
                    number = number * 8 + text[++index] - '0';
                    ++count;
                }
                if (number > 255) throw new MetadataException("annotation byte exceeds 255");
                bytes.Add((byte)number);
            }
            else if (escape is 'u' or 'U')
            {
                int count = escape == 'u' ? 4 : 8;
                if (index + count >= text.Length - 1 || !int.TryParse(text.AsSpan(index + 1, count),
                        NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int scalar) || !Rune.IsValid(scalar))
                    throw new MetadataException("invalid annotation Unicode escape");
                bytes.AddRange(Encoding.UTF8.GetBytes(new Rune(scalar).ToString()));
                index += count;
            }
            else if (escape == 'x')
            {
                int number = 0;
                int count = 0;
                while (index + 1 < text.Length - 1 && char.IsAsciiHexDigit(text[index + 1]))
                {
                    int digit = int.Parse(text[++index].ToString(), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                    if (number > 15) throw new MetadataException("annotation byte exceeds 255");
                    number = number * 16 + digit;
                    ++count;
                }
                if (count == 0) throw new MetadataException("invalid annotation hex escape");
                bytes.Add((byte)number);
            }
            else
            {
                bytes.Add(escape switch
                {
                    '\\' => (byte)'\\', '"' => (byte)'"', '\'' => (byte)'\'', '?' => (byte)'?',
                    'n' => 10, 'r' => 13, 't' => 9, 'a' => 7, 'b' => 8, 'f' => 12, 'v' => 11,
                    _ => throw new MetadataException("unsupported annotation escape")
                });
            }
        }
        try { return new UTF8Encoding(false, true).GetString(bytes.ToArray()); }
        catch (DecoderFallbackException) { throw new MetadataException("annotation is not valid UTF-8"); }
    }
}
