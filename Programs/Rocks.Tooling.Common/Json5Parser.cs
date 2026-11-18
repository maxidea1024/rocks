using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Rocks.Tooling.Common;

/// <summary>
/// <see cref="Json5Reader"/>의 구현이다. C++ Core의 JsonParser(Private/Text/JsonParser.cpp·Json5Parser.cpp)를 같은 구조·같은 검사
/// 순서로 옮겼다 — 받는 입력·오류 코드·오류 위치가 같아야 하므로 한쪽을 바꾸면 다른 쪽과 공용 대조 사례
/// (Core/Source/Runtime/Core/Tests/Json5ParityCases.h)를 함께 바꾼다. 입력은 UTF-8 byte로 다루고, 첫 오류 하나만 기록한다.
/// </summary>
internal ref struct Json5Parser
{
    // 입력 상한: C++ 파서와 같다(노드·문자열 위치가 uint32).
    private const long HardInputLimit = 1L << 30;

    private const int NoBreakSpace = 0xA0;

    private const int ZeroWidthNonJoiner = 0x200C;

    private const int ZeroWidthJoiner = 0x200D;

    private const int LineSeparator = 0x2028;

    private const int ParagraphSeparator = 0x2029;

    private const int ByteOrderMark = 0xFEFF;

    // 값 자리의 단어 리터럴이다(C++ ParseJson5Word와 같은 순서).
    private static readonly string[] Literals = ["true", "false", "null"];

    private readonly ReadOnlySpan<byte> data;

    private readonly Json5ReadOptions options;

    private readonly List<Frame> frames = [];

    private readonly List<byte> text = [];

    private int position;

    private int bomLength;

    private int nodeCount;

    private Json5ErrorCode? errorCode;

    private int errorOffset;

    private int nonFiniteOffset = -1;

    public Json5Parser(ReadOnlySpan<byte> data, Json5ReadOptions options)
    {
        this.data = data;
        this.options = options;
    }

    private enum WordMatch
    {
        Match,
        Truncated,
        Mismatch,
    }

    /// <summary>문서를 읽는다. 실패하면 첫 오류의 <see cref="Json5Exception"/>을 던진다. nonFinite는 첫 Infinity·NaN 위치의 오류다(없으면 null).</summary>
    public JsonNode? Run(out Json5Exception? nonFinite)
    {
        nonFinite = null;
        if (data.Length > options.MaxInputBytes || data.Length > HardInputLimit)
        {
            throw new Json5Exception(Json5ErrorCode.InputTooLarge, 1, 1, 0);
        }

        if (data.Length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        {
            position = 3;
            bomLength = 3;
        }

        bool ok = true;
        bool haveValue = false;
        JsonNode? value = null;
        SkipWhitespace();
        while (ok)
        {
            if (!haveValue)
            {
                if (position >= data.Length)
                {
                    ok = Fail(Json5ErrorCode.UnexpectedEnd, position);
                    break;
                }

                byte lead = data[position];
                if (lead == '{' || lead == '[')
                {
                    if (frames.Count >= options.MaxDepth)
                    {
                        ok = Fail(Json5ErrorCode.DepthLimitExceeded, position);
                        break;
                    }

                    bool isObject = lead == '{';
                    if (!AddNode(position))
                    {
                        ok = false;
                        break;
                    }

                    JsonNode container = isObject ? new JsonObject() : new JsonArray();
                    frames.Add(new Frame(container, isObject));
                    ++position;
                    SkipWhitespace();
                    if (position < data.Length && data[position] == (isObject ? '}' : ']'))
                    {
                        ++position;
                        value = CloseContainer();
                        haveValue = true;
                        continue;
                    }

                    if (isObject)
                    {
                        ok = BeginKey();
                    }

                    continue;
                }

                ok = ParseValue(out value);
                haveValue = ok;
                continue;
            }

            // 값 하나가 끝났다.
            if (frames.Count == 0)
            {
                break;
            }

            Frame frame = frames[^1];
            if (frame.IsObject)
            {
                ((JsonObject)frame.Container).Add(frame.PendingKey!, value);
                frame.PendingKey = null;
            }
            else
            {
                ((JsonArray)frame.Container).Add(value);
            }

            SkipWhitespace();
            if (position >= data.Length)
            {
                ok = Fail(Json5ErrorCode.UnexpectedEnd, position);
                break;
            }

            byte next = data[position];
            byte closer = (byte)(frame.IsObject ? '}' : ']');
            if (next == ',')
            {
                ++position;
                SkipWhitespace();
                // 원소·멤버 뒤 끝 쉼표 하나를 받는다.
                if (position < data.Length && data[position] == closer)
                {
                    ++position;
                    value = CloseContainer();
                    continue;
                }

                haveValue = false;
                if (frame.IsObject)
                {
                    ok = BeginKey();
                }

                continue;
            }

            if (next == closer)
            {
                ++position;
                value = CloseContainer();
                continue;
            }

            ok = Fail(Json5ErrorCode.UnexpectedCharacter, position);
        }

        if (ok)
        {
            SkipWhitespace();
            if (position != data.Length)
            {
                ok = Fail(Json5ErrorCode.TrailingContent, position);
            }
        }

        // 공백·주석 안의 잘못된 UTF-8은 기록만 하고 해석을 멈추지 않으므로, 기록된 오류가 있으면 실패다.
        if (ok && errorCode is not null)
        {
            ok = false;
        }

        if (!ok)
        {
            throw ErrorAt(errorCode!.Value, errorOffset);
        }

        if (nonFiniteOffset >= 0)
        {
            nonFinite = ErrorAt(Json5ErrorCode.NonFiniteNumber, nonFiniteOffset);
        }

        return value;
    }

    private static bool IsDigit(byte value) => value >= '0' && value <= '9';

    private static int HexDigitValue(byte value)
    {
        if (value >= '0' && value <= '9')
        {
            return value - '0';
        }

        if (value >= 'a' && value <= 'f')
        {
            return value - 'a' + 10;
        }

        if (value >= 'A' && value <= 'F')
        {
            return value - 'A' + 10;
        }

        return -1;
    }

    private static bool InRanges(ReadOnlySpan<int> ranges, int codePoint)
    {
        // (first, last) 쌍의 정렬된 목록에서 이분 탐색한다.
        int low = 0;
        int high = ranges.Length / 2 - 1;
        while (low <= high)
        {
            int middle = (low + high) / 2;
            if (codePoint < ranges[middle * 2])
            {
                high = middle - 1;
            }
            else if (codePoint > ranges[middle * 2 + 1])
            {
                low = middle + 1;
            }
            else
            {
                return true;
            }
        }

        return false;
    }

    // ASCII 밖 JSON5 공백: NBSP·BOM·LS·PS와 General_Category Zs.
    private static bool IsJson5Space(int codePoint) =>
        codePoint == NoBreakSpace || codePoint == ByteOrderMark || codePoint == LineSeparator || codePoint == ParagraphSeparator ||
        InRanges(Json5UnicodeTables.SpaceSeparator, codePoint);

    // ECMAScript 5.1 IdentifierStart(이스케이프 제외): Lu Ll Lt Lm Lo Nl·'$'·'_'.
    private static bool IsIdentifierStart(int codePoint) =>
        codePoint == '$' || codePoint == '_' || InRanges(Json5UnicodeTables.IdentifierStartLetter, codePoint);

    // ECMAScript 5.1 IdentifierPart(이스케이프 제외): IdentifierStart·Mn·Mc·Nd·Pc·ZWNJ·ZWJ.
    private static bool IsIdentifierPart(int codePoint) =>
        IsIdentifierStart(codePoint) || codePoint == ZeroWidthNonJoiner || codePoint == ZeroWidthJoiner ||
        InRanges(Json5UnicodeTables.IdentifierPartOther, codePoint);

    private static int Utf8EncodedLength(int codePoint)
    {
        if (codePoint < 0x80)
        {
            return 1;
        }

        if (codePoint < 0x800)
        {
            return 2;
        }

        return codePoint < 0x10000 ? 3 : 4;
    }

    // data[at]에서 UTF-8 한 글자를 읽어 길이(잘못되면 0)를 돌려준다(C++ detail::DecodeUtf8과 같다).
    private readonly int DecodeUtf8(int at, out int codePoint)
    {
        codePoint = 0;
        byte lead = data[at];
        int length;
        int minimum;
        int value;
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            length = 2;
            minimum = 0x80;
            value = lead & 0x1F;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            length = 3;
            minimum = 0x800;
            value = lead & 0x0F;
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            length = 4;
            minimum = 0x10000;
            value = lead & 0x07;
        }
        else
        {
            return 0;
        }

        if (at + length > data.Length)
        {
            return 0;
        }

        for (int index = 1; index < length; ++index)
        {
            byte next = data[at + index];
            if ((next & 0xC0) != 0x80)
            {
                return 0;
            }

            value = (value << 6) | (next & 0x3F);
        }

        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        {
            return 0;
        }

        codePoint = value;
        return length;
    }

    private bool Fail(Json5ErrorCode code, int offset)
    {
        if (errorCode is null)
        {
            errorCode = code;
            errorOffset = offset;
        }

        return false;
    }

    // offset의 줄·열이다(C++ JsonParser::Advance와 같다: CRLF는 줄바꿈 1개, CR 단독도 줄바꿈, column은 code point, BOM은 세지 않는다).
    private readonly Json5Exception ErrorAt(Json5ErrorCode code, int offset)
    {
        int line = 1;
        int column = 1;
        for (int index = bomLength; index < offset && index < data.Length; ++index)
        {
            byte value = data[index];
            if (value == '\n' || (value == '\r' && !(index + 1 < data.Length && data[index + 1] == '\n')))
            {
                ++line;
                column = 1;
            }
            else if (value == '\r')
            {
                continue;
            }
            else if ((value & 0xC0) != 0x80)
            {
                ++column;
            }
        }

        return new Json5Exception(code, line, column, offset);
    }

    private bool AddNode(int at)
    {
        if (nodeCount >= options.MaxNodes)
        {
            return Fail(Json5ErrorCode.TooManyNodes, at);
        }

        ++nodeCount;
        return true;
    }

    private bool WithinStringLimit(int adding) => text.Count + (long)adding <= options.MaxStringBytes;

    private void AppendCodePoint(int codePoint)
    {
        if (codePoint < 0x80)
        {
            text.Add((byte)codePoint);
        }
        else if (codePoint < 0x800)
        {
            text.Add((byte)(0xC0 | (codePoint >> 6)));
            text.Add((byte)(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint < 0x10000)
        {
            text.Add((byte)(0xE0 | (codePoint >> 12)));
            text.Add((byte)(0x80 | ((codePoint >> 6) & 0x3F)));
            text.Add((byte)(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            text.Add((byte)(0xF0 | (codePoint >> 18)));
            text.Add((byte)(0x80 | ((codePoint >> 12) & 0x3F)));
            text.Add((byte)(0x80 | ((codePoint >> 6) & 0x3F)));
            text.Add((byte)(0x80 | (codePoint & 0x3F)));
        }
    }

    private void AppendBytes(int at, int count)
    {
        for (int index = 0; index < count; ++index)
        {
            text.Add(data[at + index]);
        }
    }

    private readonly string TakeText()
    {
        string value = Encoding.UTF8.GetString(System.Runtime.InteropServices.CollectionsMarshal.AsSpan(text));
        text.Clear();
        return value;
    }

    private JsonNode CloseContainer()
    {
        JsonNode container = frames[^1].Container;
        frames.RemoveAt(frames.Count - 1);
        return container;
    }

    // 공백·주석을 건너뛴다. 잘못된 UTF-8·닫히지 않은 블록 주석은 기록하고 멈춘다.
    private void SkipWhitespace()
    {
        while (position < data.Length)
        {
            byte value = data[position];
            if (value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\v' || value == '\f')
            {
                ++position;
                continue;
            }

            if (value == '/' && position + 1 < data.Length && (data[position + 1] == '/' || data[position + 1] == '*'))
            {
                bool block = data[position + 1] == '*';
                position += 2;
                if (!SkipComment(block))
                {
                    return;
                }

                continue;
            }

            if (value < 0x80)
            {
                return;
            }

            int count = DecodeUtf8(position, out int codePoint);
            if (count == 0)
            {
                Fail(Json5ErrorCode.InvalidUtf8, position);
                return;
            }

            if (!IsJson5Space(codePoint))
            {
                return;
            }

            position += count;
        }
    }

    private bool SkipComment(bool block)
    {
        while (true)
        {
            if (position >= data.Length)
            {
                return !block || Fail(Json5ErrorCode.UnexpectedEnd, data.Length);
            }

            byte value = data[position];
            if (block && value == '*' && position + 1 < data.Length && data[position + 1] == '/')
            {
                position += 2;
                return true;
            }

            if (!block && (value == '\n' || value == '\r'))
            {
                return true;
            }

            if (value < 0x80)
            {
                ++position;
                continue;
            }

            int count = DecodeUtf8(position, out int codePoint);
            if (count == 0)
            {
                return Fail(Json5ErrorCode.InvalidUtf8, position);
            }

            if (!block && (codePoint == LineSeparator || codePoint == ParagraphSeparator))
            {
                return true;
            }

            position += count;
        }
    }

    private readonly bool IsIdentifierStartAt(int at)
    {
        if (at >= data.Length)
        {
            return false;
        }

        byte value = data[at];
        if (value == '\\')
        {
            return true;
        }

        if (value < 0x80)
        {
            return IsIdentifierStart(value);
        }

        return DecodeUtf8(at, out int codePoint) != 0 && IsIdentifierStart(codePoint);
    }

    private readonly bool IsIdentifierContinuation(int at)
    {
        if (at >= data.Length)
        {
            return false;
        }

        byte value = data[at];
        if (value == '\\')
        {
            return true;
        }

        if (value < 0x80)
        {
            return IsIdentifierPart(value);
        }

        return DecodeUtf8(at, out int codePoint) != 0 && IsIdentifierPart(codePoint);
    }

    private readonly WordMatch MatchWord(int at, string word)
    {
        int available = data.Length - at;
        int compared = Math.Min(available, word.Length);
        for (int index = 0; index < compared; ++index)
        {
            if (data[at + index] != word[index])
            {
                return WordMatch.Mismatch;
            }
        }

        if (available < word.Length)
        {
            return WordMatch.Truncated;
        }

        return IsIdentifierContinuation(at + word.Length) ? WordMatch.Mismatch : WordMatch.Match;
    }

    // 객체 안에서 키와 ':'까지 읽는다(position은 공백 뒤).
    private bool BeginKey()
    {
        if (position >= data.Length)
        {
            return Fail(Json5ErrorCode.UnexpectedEnd, position);
        }

        int keyStart = position;
        byte quote = data[position];
        bool parsed = quote == '"' || quote == '\'' ? ParseString() : ParseIdentifier();
        if (!parsed)
        {
            return false;
        }

        string key = TakeText();
        Frame frame = frames[^1];
        if (((JsonObject)frame.Container).ContainsKey(key))
        {
            return Fail(Json5ErrorCode.DuplicateKey, keyStart);
        }

        frame.PendingKey = key;
        SkipWhitespace();
        if (position >= data.Length)
        {
            return Fail(Json5ErrorCode.UnexpectedEnd, position);
        }

        if (data[position] != ':')
        {
            return Fail(Json5ErrorCode.UnexpectedCharacter, position);
        }

        ++position;
        SkipWhitespace();
        return true;
    }

    private bool ParseValue(out JsonNode? value)
    {
        value = null;
        int start = position;
        byte lead = data[position];
        if (lead == '"' || lead == '\'')
        {
            if (!ParseString() || !AddNode(start))
            {
                return false;
            }

            value = JsonValue.Create(TakeText());
            return true;
        }

        if (lead == '+' || lead == '-' || lead == '.' || IsDigit(lead) || lead == 'I' || lead == 'N')
        {
            return ParseNumber(out value);
        }

        return ParseWord(out value);
    }

    // 문자열 하나(position은 여는 따옴표)를 풀어 text에 담는다.
    private bool ParseString()
    {
        int start = position;
        byte quote = data[position];
        ++position;
        text.Clear();
        while (true)
        {
            int run = position;
            while (run < data.Length && data[run] != quote && data[run] != '\\' && data[run] != '\n' && data[run] != '\r' && data[run] < 0x80)
            {
                ++run;
            }

            if (run != position)
            {
                if (!WithinStringLimit(run - position))
                {
                    return Fail(Json5ErrorCode.StringTooLong, start);
                }

                AppendBytes(position, run - position);
                position = run;
                continue;
            }

            if (position >= data.Length)
            {
                return Fail(Json5ErrorCode.UnexpectedEnd, position);
            }

            byte value = data[position];
            if (value == quote)
            {
                ++position;
                break;
            }

            if (value == '\n' || value == '\r')
            {
                return Fail(Json5ErrorCode.ControlCharacter, position);
            }

            if (value >= 0x80)
            {
                int count = DecodeUtf8(position, out _);
                if (count == 0)
                {
                    return Fail(Json5ErrorCode.InvalidUtf8, position);
                }

                if (!WithinStringLimit(count))
                {
                    return Fail(Json5ErrorCode.StringTooLong, start);
                }

                AppendBytes(position, count);
                position += count;
                continue;
            }

            if (!ParseEscape(start))
            {
                return false;
            }
        }

        return true;
    }

    private bool ParseEscape(int stringStart)
    {
        int escape = position;
        if (position + 1 >= data.Length)
        {
            return Fail(Json5ErrorCode.UnexpectedEnd, data.Length);
        }

        byte kind = data[position + 1];
        position += 2;
        byte simple;
        switch (kind)
        {
            case (byte)'\'':
            case (byte)'"':
            case (byte)'\\':
                simple = kind;
                break;
            case (byte)'b':
                simple = (byte)'\b';
                break;
            case (byte)'f':
                simple = (byte)'\f';
                break;
            case (byte)'n':
                simple = (byte)'\n';
                break;
            case (byte)'r':
                simple = (byte)'\r';
                break;
            case (byte)'t':
                simple = (byte)'\t';
                break;
            case (byte)'v':
                simple = (byte)'\v';
                break;
            case (byte)'0':
                if (position < data.Length && IsDigit(data[position]))
                {
                    return Fail(Json5ErrorCode.InvalidEscape, escape);
                }

                simple = 0;
                break;
            case >= (byte)'1' and <= (byte)'9':
                return Fail(Json5ErrorCode.InvalidEscape, escape);
            case (byte)'x':
                {
                    int value = 0;
                    for (int index = 0; index < 2; ++index)
                    {
                        if (position + index >= data.Length)
                        {
                            return Fail(Json5ErrorCode.UnexpectedEnd, data.Length);
                        }

                        int digit = HexDigitValue(data[position + index]);
                        if (digit < 0)
                        {
                            return Fail(Json5ErrorCode.InvalidEscape, escape);
                        }

                        value = (value << 4) | digit;
                    }

                    position += 2;
                    if (!WithinStringLimit(Utf8EncodedLength(value)))
                    {
                        return Fail(Json5ErrorCode.StringTooLong, stringStart);
                    }

                    AppendCodePoint(value);
                    return true;
                }

            case (byte)'u':
                {
                    if (!ReadUnicodeEscape(escape, out int codePoint))
                    {
                        return false;
                    }

                    if (!WithinStringLimit(4))
                    {
                        return Fail(Json5ErrorCode.StringTooLong, stringStart);
                    }

                    AppendCodePoint(codePoint);
                    return true;
                }

            case (byte)'\n':
                return true;
            case (byte)'\r':
                if (position < data.Length && data[position] == '\n')
                {
                    ++position;
                }

                return true;
            default:
                if (kind < 0x80)
                {
                    simple = kind;
                    break;
                }

                {
                    int count = DecodeUtf8(escape + 1, out int codePoint);
                    if (count == 0)
                    {
                        return Fail(Json5ErrorCode.InvalidUtf8, escape + 1);
                    }

                    position = escape + 1 + count;
                    if (codePoint == LineSeparator || codePoint == ParagraphSeparator)
                    {
                        return true;
                    }

                    if (!WithinStringLimit(count))
                    {
                        return Fail(Json5ErrorCode.StringTooLong, stringStart);
                    }

                    AppendBytes(escape + 1, count);
                    return true;
                }
        }

        if (!WithinStringLimit(1))
        {
            return Fail(Json5ErrorCode.StringTooLong, stringStart);
        }

        text.Add(simple);
        return true;
    }

    // \u 뒤 16진수 4자리(position은 첫 자리). 잘못된 자리는 escape 위치의 InvalidEscape, 모두 16진수인데 입력이 먼저 끝나면 UnexpectedEnd다.
    private bool ReadHex(int escape, out int unit)
    {
        unit = 0;
        for (int index = 0; index < 4; ++index)
        {
            if (position + index >= data.Length)
            {
                return Fail(Json5ErrorCode.UnexpectedEnd, data.Length);
            }

            int digit = HexDigitValue(data[position + index]);
            if (digit < 0)
            {
                return Fail(Json5ErrorCode.InvalidEscape, escape);
            }

            unit = (unit << 4) | digit;
        }

        position += 4;
        return true;
    }

    private bool ReadUnicodeEscape(int escape, out int codePoint)
    {
        codePoint = 0;
        if (!ReadHex(escape, out int unit))
        {
            return false;
        }

        if (unit >= 0xDC00 && unit <= 0xDFFF)
        {
            return Fail(Json5ErrorCode.UnpairedSurrogate, escape);
        }

        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            if (position >= data.Length || (data[position] == '\\' && position + 1 >= data.Length))
            {
                return Fail(Json5ErrorCode.UnexpectedEnd, data.Length);
            }

            if (data[position] != '\\' || data[position + 1] != 'u')
            {
                return Fail(Json5ErrorCode.UnpairedSurrogate, escape);
            }

            int second = position;
            position += 2;
            if (!ReadHex(second, out int low))
            {
                return false;
            }

            if (low < 0xDC00 || low > 0xDFFF)
            {
                return Fail(Json5ErrorCode.UnpairedSurrogate, second);
            }

            unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        }

        codePoint = unit;
        return true;
    }

    // ECMAScript 5.1 IdentifierName 키를 text에 담는다. 이스케이프는 \uXXXX 하나씩이고 푼 글자도 그 자리의 식별자 글자여야 한다.
    private bool ParseIdentifier()
    {
        int start = position;
        text.Clear();
        bool first = true;
        while (position < data.Length)
        {
            int at = position;
            byte value = data[at];
            int codePoint;
            if (value == '\\')
            {
                if (at + 1 >= data.Length)
                {
                    return Fail(Json5ErrorCode.UnexpectedEnd, data.Length);
                }

                if (data[at + 1] != 'u')
                {
                    return Fail(Json5ErrorCode.InvalidEscape, at);
                }

                position = at + 2;
                if (!ReadHex(at, out codePoint))
                {
                    return false;
                }

                if (!(first ? IsIdentifierStart(codePoint) : IsIdentifierPart(codePoint)))
                {
                    return Fail(Json5ErrorCode.InvalidEscape, at);
                }
            }
            else
            {
                int count = 1;
                codePoint = value;
                if (value >= 0x80)
                {
                    count = DecodeUtf8(at, out codePoint);
                    if (count == 0)
                    {
                        return Fail(Json5ErrorCode.InvalidUtf8, at);
                    }
                }

                if (!(first ? IsIdentifierStart(codePoint) : IsIdentifierPart(codePoint)))
                {
                    if (first)
                    {
                        return Fail(Json5ErrorCode.UnexpectedCharacter, at);
                    }

                    break;
                }

                position = at + count;
            }

            if (!WithinStringLimit(Utf8EncodedLength(codePoint)))
            {
                return Fail(Json5ErrorCode.StringTooLong, start);
            }

            AppendCodePoint(codePoint);
            first = false;
        }

        return true;
    }

    // [+-]?(Infinity | NaN | 0[xX]hex+ | (0|[1-9]digit*)(.digit*)?exponent? | .digit+ exponent?). 유한한 수의 토큰은 C++과 같이
    // RFC 8259 꼴로 정규화한다.
    private bool ParseNumber(out JsonNode? value)
    {
        value = null;
        int start = position;
        int index = position;
        bool negative = false;
        if (data[index] == '+' || data[index] == '-')
        {
            negative = data[index] == '-';
            ++index;
        }

        if (index >= data.Length)
        {
            return Fail(Json5ErrorCode.UnexpectedEnd, index);
        }

        byte lead = data[index];
        if (lead == 'I' || lead == 'N')
        {
            bool infinity = lead == 'I';
            string word = infinity ? "Infinity" : "NaN";
            switch (MatchWord(index, word))
            {
                case WordMatch.Truncated:
                    return Fail(Json5ErrorCode.UnexpectedEnd, data.Length);
                case WordMatch.Mismatch:
                    return Fail(Json5ErrorCode.InvalidLiteral, index);
            }

            position = index + word.Length;
            if (!AddNode(start))
            {
                return false;
            }

            if (nonFiniteOffset < 0)
            {
                nonFiniteOffset = start;
            }

            value = JsonValue.Create(infinity ? (negative ? double.NegativeInfinity : double.PositiveInfinity) : double.NaN);
            return true;
        }

        bool hex = lead == '0' && index + 1 < data.Length && (data[index + 1] == 'x' || data[index + 1] == 'X');
        if (hex)
        {
            index += 2;
            int digitsStart = index;
            ulong magnitude = 0;
            bool overflow = false;
            for (; index < data.Length; ++index)
            {
                int digit = HexDigitValue(data[index]);
                if (digit < 0)
                {
                    break;
                }

                if (magnitude > (ulong.MaxValue >> 4))
                {
                    overflow = true;
                }

                magnitude = (magnitude << 4) | (uint)digit;
            }

            if (index == digitsStart)
            {
                return Fail(index >= data.Length ? Json5ErrorCode.UnexpectedEnd : Json5ErrorCode.InvalidNumber, index);
            }

            if (IsIdentifierContinuation(index))
            {
                return Fail(Json5ErrorCode.InvalidNumber, index);
            }

            if (overflow)
            {
                return Fail(Json5ErrorCode.NumberOutOfRange, start);
            }

            position = index;
            string decimalText = magnitude.ToString(CultureInfo.InvariantCulture);
            return AddNumber(negative ? "-" + decimalText : decimalText, integer: true, start, out value);
        }

        int integerStart = index;
        if (lead == '0')
        {
            ++index;
            if (index < data.Length && IsDigit(data[index]))
            {
                return Fail(Json5ErrorCode.InvalidNumber, index);
            }
        }
        else
        {
            while (index < data.Length && IsDigit(data[index]))
            {
                ++index;
            }
        }

        int integerEnd = index;
        bool integer = true;
        bool point = false;
        int fractionStart = index;
        int fractionEnd = index;
        if (index < data.Length && data[index] == '.')
        {
            integer = false;
            point = true;
            ++index;
            fractionStart = index;
            while (index < data.Length && IsDigit(data[index]))
            {
                ++index;
            }

            fractionEnd = index;
        }

        if (integerStart == integerEnd && fractionStart == fractionEnd)
        {
            return Fail(index >= data.Length ? Json5ErrorCode.UnexpectedEnd : Json5ErrorCode.InvalidNumber, index);
        }

        int exponentStart = index;
        if (index < data.Length && (data[index] == 'e' || data[index] == 'E'))
        {
            integer = false;
            ++index;
            if (index < data.Length && (data[index] == '+' || data[index] == '-'))
            {
                ++index;
            }

            if (index >= data.Length || !IsDigit(data[index]))
            {
                return Fail(index >= data.Length ? Json5ErrorCode.UnexpectedEnd : Json5ErrorCode.InvalidNumber, index);
            }

            while (index < data.Length && IsDigit(data[index]))
            {
                ++index;
            }
        }

        if (IsIdentifierContinuation(index))
        {
            return Fail(Json5ErrorCode.InvalidNumber, index);
        }

        position = index;
        var token = new StringBuilder();
        if (negative)
        {
            token.Append('-');
        }

        token.Append(integerStart == integerEnd ? "0" : Encoding.ASCII.GetString(data[integerStart..integerEnd]));
        if (point)
        {
            token.Append('.');
            token.Append(fractionStart == fractionEnd ? "0" : Encoding.ASCII.GetString(data[fractionStart..fractionEnd]));
        }

        token.Append(Encoding.ASCII.GetString(data[exponentStart..index]));
        return AddNumber(token.ToString(), integer, start, out value);
    }

    // RFC 8259 토큰을 C++과 같은 규칙(int64 → uint64 → double, "-0"은 double, double 범위 밖은 NumberOutOfRange)으로 판정해 값을 만든다.
    private bool AddNumber(string token, bool integer, int start, out JsonNode? value)
    {
        value = null;
        bool representable = integer && token != "-0" &&
            (long.TryParse(token, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out _) ||
             (token[0] != '-' && ulong.TryParse(token, NumberStyles.None, CultureInfo.InvariantCulture, out _)));
        if (!representable)
        {
            bool parsed = double.TryParse(token, NumberStyles.AllowLeadingSign | NumberStyles.AllowDecimalPoint | NumberStyles.AllowExponent,
                CultureInfo.InvariantCulture, out double result);
            if (!parsed || double.IsInfinity(result))
            {
                return Fail(Json5ErrorCode.NumberOutOfRange, start);
            }
        }

        if (!AddNode(start))
        {
            return false;
        }

        using JsonDocument number = JsonDocument.Parse(token);
        value = JsonValue.Create(number.RootElement.Clone());
        return true;
    }

    private bool ParseWord(out JsonNode? value)
    {
        value = null;
        foreach (string word in Literals)
        {
            if (data[position] != word[0])
            {
                continue;
            }

            switch (MatchWord(position, word))
            {
                case WordMatch.Truncated:
                    return Fail(Json5ErrorCode.UnexpectedEnd, data.Length);
                case WordMatch.Mismatch:
                    return Fail(Json5ErrorCode.InvalidLiteral, position);
            }

            int start = position;
            position += word.Length;
            if (!AddNode(start))
            {
                return false;
            }

            value = word == "null" ? null : JsonValue.Create(word == "true");
            return true;
        }

        return Fail(IsIdentifierStartAt(position) ? Json5ErrorCode.InvalidLiteral : Json5ErrorCode.UnexpectedCharacter, position);
    }

    private sealed class Frame(JsonNode container, bool isObject)
    {
        public JsonNode Container { get; } = container;

        public bool IsObject { get; } = isObject;

        public string? PendingKey { get; set; }
    }
}
