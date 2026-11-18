using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using System.Text.Json;
using System.Text.Json.Nodes;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

/// <summary>
/// C# JSON5 reader(<see cref="Json5Reader"/>) 시험이다. C++ Core JSON5 시험(Core/Source/Runtime/Core/Tests/Json5Tests.cpp)과 같은 표 두 개
/// (Json5ParityCases.h — 손으로 쓴 입력의 값·오류 코드·위치, Json5SuiteExpectations.h — json5-tests 받은 사례의 값)를 C 헤더에서
/// 그대로 읽어 같은 결과를 내는지 본다. 두 구현이 같은 입력 집합을 받고 같은 값을 만드는지의 교차 확인이다.
/// </summary>
internal static class Json5ReaderTests
{
    private const string TestsDirectory = "Core/Source/Runtime/Core/Tests";

    private const string SuiteRoot = "Core/Source/ThirdParty/Json5Tests";

    private const string SuiteDirectory = SuiteRoot + "/json5-tests-ceb24d408013";

    // Rocks 정책으로 확장자 규칙과 판정이 다른 사례다(C++ Json5Tests.cpp의 SuiteExceptions와 같다).
    private static readonly (string Path, bool Accept)[] SuiteExceptions =
    [
        ("objects/duplicate-keys.json", false),
    ];

    // 확장자는 .json이지만 RFC 8259가 아닌 사례다(원본이 주석을 담고 있다 — C++ NotStrictJson과 같다).
    private static readonly string[] NotStrictJson = ["comments/irregular-block-comment.json"];

    public static void Register(TestSuite suite, Options options)
    {
        suite.Add("Json5.ParityCasesMatchCpp", () => ParityCases(options.Root));
        suite.Add("Json5.TestSuiteCases", () => SuiteCases(options.Root));
        suite.Add("Json5.DocumentsNumbersAndLimits", DocumentsNumbersAndLimits);
        suite.Add("Json5.UnicodeTablesCurrent", () => UnicodeTablesCurrent(options.Root));
    }

    private static void ParityCases(string root)
    {
        string header = Path.Combine(root, TestsDirectory, "Json5ParityCases.h");
        var cases = ReadTable(header, "Json5ParityCases");
        Verify.That(cases.Count > 100, "parity table must be read from the C++ header");
        Compare(cases, null);
        // 문자열·키 길이 한도 경계 사례는 헤더의 Json5StringLimit로 읽는다.
        Match limit = Regex.Match(File.ReadAllText(header), @"Json5StringLimit = (\d+);");
        Verify.That(limit.Success, "Json5StringLimit must be read from the C++ header");
        var limitCases = ReadTable(header, "Json5StringLimitCases");
        Verify.That(limitCases.Count > 10, "string limit table must be read from the C++ header");
        Compare(limitCases, new Json5ReadOptions { MaxStringBytes = long.Parse(limit.Groups[1].Value, CultureInfo.InvariantCulture) });
    }

    private static void Compare(List<(byte[] First, byte[] Second)> cases, Json5ReadOptions? options)
    {
        var failures = new List<string>();
        foreach ((byte[] input, byte[] expected) in cases)
        {
            string actual = DumpResult(input, options);
            string wanted = Encoding.UTF8.GetString(expected);
            if (actual != wanted)
            {
                failures.Add($"{Convert.ToHexString(input)}: got {actual}, expected {wanted}");
            }
        }

        Verify.That(failures.Count == 0, string.Join('\n', failures));
    }

    private static void SuiteCases(string root)
    {
        var expectations = ReadTable(Path.Combine(root, TestsDirectory, "Json5SuiteExpectations.h"), "Json5SuiteExpectations")
            .ToDictionary(entry => Encoding.UTF8.GetString(entry.First), entry => Encoding.UTF8.GetString(entry.Second), StringComparer.Ordinal);
        using JsonDocument manifest = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(root, SuiteRoot, ThirdPartyManifest.FileName)));
        var failures = new List<string>();
        var used = new HashSet<string>(StringComparer.Ordinal);
        int cases = 0;
        foreach (JsonProperty file in manifest.RootElement.GetProperty("files").EnumerateObject())
        {
            string name = file.Name;
            bool json = name.EndsWith(".json", StringComparison.Ordinal);
            bool json5 = name.EndsWith(".json5", StringComparison.Ordinal);
            bool invalid = name.EndsWith(".js", StringComparison.Ordinal) || name.EndsWith(".txt", StringComparison.Ordinal);
            if (!json && !json5 && !invalid)
            {
                continue;
            }

            ++cases;
            bool expectAccept = json || json5;
            foreach ((string path, bool accept) in SuiteExceptions)
            {
                if (path == name)
                {
                    expectAccept = accept;
                }
            }

            byte[] bytes = File.ReadAllBytes(Path.Combine(root, SuiteDirectory, name));
            JsonNode? node;
            try
            {
                node = Json5Reader.Parse(bytes);
            }
            catch (Json5Exception error)
            {
                if (expectAccept)
                {
                    failures.Add($"{name}: rejected ({error.Code} at {error.Line}:{error.Column})");
                }

                continue;
            }

            if (!expectAccept)
            {
                failures.Add($"{name}: accepted");
                continue;
            }

            string dump = Dump(node);
            if (!expectations.TryGetValue(name, out string? expected))
            {
                failures.Add($"{name}: no expectation (got {dump})");
                continue;
            }

            used.Add(name);
            if (dump != expected)
            {
                failures.Add($"{name}: got {dump}, expected {expected}");
            }

            // .json 사례는 System.Text.Json 엄격 읽기와 같은 문서다.
            if (json && !NotStrictJson.Contains(name) && Dump(JsonNode.Parse(bytes)) != dump)
            {
                failures.Add($"{name}: strict System.Text.Json reading differs");
            }

            // JsonDocument로 옮긴 결과도 같은 값이다(비유한 수는 NonFiniteNumber).
            bool nonFinite = HasNonFinite(node);
            try
            {
                using JsonDocument document = Json5Reader.ParseDocument(bytes);
                if (nonFinite || Dump(JsonNode.Parse(document.RootElement.GetRawText())) != dump)
                {
                    failures.Add($"{name}: ParseDocument result differs");
                }
            }
            catch (Json5Exception error) when (error.Code == Json5ErrorCode.NonFiniteNumber)
            {
                if (!nonFinite)
                {
                    failures.Add($"{name}: unexpected NonFiniteNumber");
                }
            }
        }

        failures.AddRange(expectations.Keys.Where(name => !used.Contains(name)).Select(name => $"{name}: unused expectation"));
        Verify.Equal(114, cases, "json5-tests case count");
        Verify.That(failures.Count == 0, string.Join('\n', failures));
    }

    private static void DocumentsNumbersAndLimits()
    {
        // 숫자는 RFC 8259 꼴로 정규화한 JsonElement라 형 변환 읽기가 된다.
        JsonNode root = Json5Reader.Parse("{a: 0x10, b: .5, c: +7, d: 5., e: -0x0, f: [Infinity, NaN],}"u8)!;
        Verify.Equal(16, root["a"]!.GetValue<int>(), "hexadecimal as int");
        Verify.Equal(0.5, root["b"]!.GetValue<double>(), "leading decimal point");
        Verify.Equal("7", root["c"]!.ToJsonString(), "leading plus is dropped");
        Verify.Equal("5.0", root["d"]!.ToJsonString(), "trailing decimal point gets a zero");
        Verify.Equal("-0", root["e"]!.ToJsonString(), "negative zero hexadecimal");
        Verify.That(double.IsPositiveInfinity(root["f"]![0]!.GetValue<double>()) && double.IsNaN(root["f"]![1]!.GetValue<double>()),
            "Infinity and NaN are double values");

        // JsonDocument: 엄격한 JSON으로 옮긴다. 비유한 수는 담을 수 없어 첫 위치의 NonFiniteNumber다.
        using (JsonDocument document = Json5Reader.ParseDocument("// c\n{name: 'High', list: [1, 2,],}"u8))
        {
            Verify.Equal("High", document.RootElement.GetProperty("name").GetString(), "document string");
            Verify.Equal(2, document.RootElement.GetProperty("list").GetArrayLength(), "document array");
        }

        Json5Exception nonFinite = Catch(() => Json5Reader.ParseDocument("[1,\n -Infinity]"u8));
        Verify.That(nonFinite.Code == Json5ErrorCode.NonFiniteNumber && nonFinite.Line == 2 && nonFinite.Column == 2,
            $"non-finite position {nonFinite.Line}:{nonFinite.Column}");
        using (JsonDocument nullDocument = Json5Reader.ParseDocument("null"u8))
        {
            Verify.Equal(JsonValueKind.Null, nullDocument.RootElement.ValueKind, "top-level null document");
        }

        Verify.That(Json5Reader.Parse("/* x */ null"u8) is null, "top-level null node");

        // NUL을 포함한 제어 문자는 문자열에 그대로 올 수 있다. 맨 앞 BOM은 건너뛴다.
        Verify.Equal("a\0b", Json5Reader.Parse("'a\0b'"u8)!.GetValue<string>(), "raw NUL in a string");
        Verify.Equal(1, Json5Reader.Parse("﻿{a: 1}"u8)!["a"]!.GetValue<int>(), "leading BOM");

        // 한도는 C++ JsonReadOptions 기본값과 같다.
        string deep = new string('[', 128) + new string(']', 128);
        Verify.That(Json5Reader.Parse(Encoding.ASCII.GetBytes(deep)) is JsonArray, "depth 128");
        Json5Exception tooDeep = Catch(() => Json5Reader.Parse(Encoding.ASCII.GetBytes(new string('[', 129))));
        Verify.That(tooDeep.Code == Json5ErrorCode.DepthLimitExceeded && tooDeep.Column == 129, "depth 129");
        using (JsonDocument deepDocument = Json5Reader.ParseDocument(Encoding.ASCII.GetBytes(deep)))
        {
            Verify.Equal(JsonValueKind.Array, deepDocument.RootElement.ValueKind, "depth 128 document");
        }

        var strings = new Json5ReadOptions { MaxStringBytes = 4 };
        Verify.That(Json5Reader.Parse("['abcd']"u8, strings) is JsonArray, "string at the limit");
        Verify.Equal(Json5ErrorCode.StringTooLong, Catch(() => Json5Reader.Parse("['abcde']"u8, strings)).Code, "string over the limit");
        Json5Exception longKey = Catch(() => Json5Reader.Parse("{abcde: 1}"u8, strings));
        Verify.That(longKey.Code == Json5ErrorCode.StringTooLong && longKey.Column == 2, "identifier key over the limit");
        Verify.That(Json5Reader.Parse("['\\xe9\\xe9']"u8, strings) is JsonArray, "two-byte hexadecimal escapes at the limit");
        Verify.Equal(Json5ErrorCode.StringTooLong, Catch(() => Json5Reader.Parse("['\\xe9\\xe9\\x41']"u8, strings)).Code,
            "hexadecimal escapes over the limit");
        Verify.That(Json5Reader.Parse("['ab\\\ncd']"u8, strings) is JsonArray, "line continuation is not counted");
        var nodes = new Json5ReadOptions { MaxNodes = 3 };
        Verify.That(Json5Reader.Parse("[1, 2,]"u8, nodes) is JsonArray, "node count at the limit");
        Verify.Equal(Json5ErrorCode.TooManyNodes, Catch(() => Json5Reader.Parse("[1, 2, 3]"u8, nodes)).Code, "node count over the limit");
        var input = new Json5ReadOptions { MaxInputBytes = 4 };
        Verify.That(Json5Reader.Parse("[1,]"u8, input) is JsonArray, "input at the limit");
        Verify.Equal(Json5ErrorCode.InputTooLarge, Catch(() => Json5Reader.Parse("[11,]"u8, input)).Code, "input over the limit");
        Verify.Contains("2:4: unexpected character", Catch(() => Json5Reader.Parse("{\r\n a b}"u8)).Message, "message has the position");
    }

    private static void UnicodeTablesCurrent(string root)
    {
        IReadOnlyList<CheckFailure> failures = Json5UnicodeTablesGenerator.Check(root);
        Verify.That(failures.Count == 0, string.Join('\n', failures));
        Verify.Equal(Json5UnicodeTablesGenerator.Generate(root), Json5UnicodeTablesGenerator.Generate(root), "JSON5 Unicode table generator is not deterministic");
    }

    private static bool HasNonFinite(JsonNode? node) => node switch
    {
        JsonArray array => array.Any(HasNonFinite),
        JsonObject value => value.Any(member => HasNonFinite(member.Value)),
        JsonValue scalar => !scalar.TryGetValue(out JsonElement _) && scalar.TryGetValue(out double special) && !double.IsFinite(special),
        _ => false,
    };

    private static Json5Exception Catch(Action action)
    {
        try
        {
            action();
        }
        catch (Json5Exception error)
        {
            return error;
        }

        throw new InvalidOperationException("expected a Json5Exception");
    }

    private static string DumpResult(byte[] input, Json5ReadOptions? options)
    {
        try
        {
            return Dump(Json5Reader.Parse(input, options));
        }
        catch (Json5Exception error)
        {
            return $"!{error.Code}@{error.Line}:{error.Column}";
        }
    }

    // 공용 기대값 표기다(Json5ParityCases.h 머리 주석, C++ Json5Tests.cpp의 Dump와 같다).
    private static string Dump(JsonNode? node)
    {
        var text = new StringBuilder();
        Append(node, text);
        return text.ToString();
    }

    private static void Append(JsonNode? node, StringBuilder text)
    {
        switch (node)
        {
            case null:
                text.Append("null");
                return;
            case JsonArray array:
                text.Append('[');
                for (int index = 0; index < array.Count; ++index)
                {
                    if (index != 0)
                    {
                        text.Append(',');
                    }

                    Append(array[index], text);
                }

                text.Append(']');
                return;
            case JsonObject value:
                text.Append('{');
                bool first = true;
                foreach ((string key, JsonNode? member) in value)
                {
                    if (!first)
                    {
                        text.Append(',');
                    }

                    first = false;
                    AppendString(key, text);
                    text.Append(':');
                    Append(member, text);
                }

                text.Append('}');
                return;
        }

        JsonValue scalar = (JsonValue)node;
        switch (scalar.GetValueKind())
        {
            case JsonValueKind.True:
                text.Append("true");
                return;
            case JsonValueKind.False:
                text.Append("false");
                return;
            case JsonValueKind.String:
                AppendString(scalar.GetValue<string>(), text);
                return;
            case JsonValueKind.Number:
                AppendNumber(scalar, text);
                return;
            default:
                throw new InvalidOperationException("unexpected JSON value kind");
        }
    }

    // 숫자: 원문 토큰을 C++과 같은 규칙(소수부·지수 없음 → int64 → uint64, 그 밖·"-0" → double)으로 판정한다. 비유한 수는
    // JsonValue&lt;double&gt;의 비트 그대로다(NaN도 정규화하지 않아 C++과 비트가 다르면 대조가 잡는다).
    private static void AppendNumber(JsonValue scalar, StringBuilder text)
    {
        if (!scalar.TryGetValue(out JsonElement element))
        {
            double special = scalar.GetValue<double>();
            ulong bits = BitConverter.DoubleToUInt64Bits(special);
            text.Append('d').Append(bits.ToString("x16", CultureInfo.InvariantCulture));
            return;
        }

        string token = element.GetRawText();
        bool integer = token.IndexOfAny(['.', 'e', 'E']) < 0 && token != "-0";
        if (integer && long.TryParse(token, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out long signed))
        {
            text.Append('i').Append(signed.ToString(CultureInfo.InvariantCulture));
            return;
        }

        if (integer && token[0] != '-' && ulong.TryParse(token, NumberStyles.None, CultureInfo.InvariantCulture, out ulong unsigned))
        {
            text.Append('u').Append(unsigned.ToString(CultureInfo.InvariantCulture));
            return;
        }

        double value = double.Parse(token, NumberStyles.Float, CultureInfo.InvariantCulture);
        text.Append('d').Append(BitConverter.DoubleToUInt64Bits(value).ToString("x16", CultureInfo.InvariantCulture));
    }

    private static void AppendString(string value, StringBuilder text)
    {
        text.Append('"');
        foreach (Rune rune in value.EnumerateRunes())
        {
            if (rune.Value == '"' || rune.Value == '\\')
            {
                text.Append('\\').Append((char)rune.Value);
            }
            else if (rune.Value >= 0x20 && rune.Value <= 0x7E)
            {
                text.Append((char)rune.Value);
            }
            else
            {
                text.Append("\\u{").Append(rune.Value.ToString("X", CultureInfo.InvariantCulture)).Append('}');
            }
        }

        text.Append('"');
    }

    // C++ 헤더의 표 `name[] = { {"...", "..."}, ... };`를 읽는다. 표 안의 C 문법은 문자열(이웃 문자열 이어 붙이기)·중괄호·쉼표·
    // 줄 주석뿐이다(Json5ParityCases.h 머리 주석).
    private static List<(byte[] First, byte[] Second)> ReadTable(string path, string name)
    {
        string source = File.ReadAllText(path, new UTF8Encoding(false, true));
        int start = source.IndexOf(name + "[] = {", StringComparison.Ordinal);
        if (start < 0)
        {
            throw new FormatException($"{path}: table {name} not found");
        }

        int index = start + name.Length + "[] = {".Length;
        var entries = new List<(byte[] First, byte[] Second)>();
        while (true)
        {
            char token = NextToken(source, ref index);
            if (token == '}')
            {
                return entries;
            }

            Expect(token == '{', path, index);
            byte[] first = ReadStrings(source, ref index, path);
            Expect(NextToken(source, ref index) == ',', path, index);
            byte[] second = ReadStrings(source, ref index, path);
            Expect(NextToken(source, ref index) == '}', path, index);
            Expect(NextToken(source, ref index) == ',', path, index);
            entries.Add((first, second));
        }
    }

    private static void Expect(bool condition, string path, int index)
    {
        if (!condition)
        {
            throw new FormatException($"{path}: unexpected table syntax at offset {index}");
        }
    }

    // 공백·줄 주석을 건너뛰고 다음 글자를 보여 준다. '"'가 아니면 소비한다.
    private static char NextToken(string source, ref int index)
    {
        while (index < source.Length)
        {
            if (char.IsWhiteSpace(source[index]))
            {
                ++index;
                continue;
            }

            if (source[index] == '/' && index + 1 < source.Length && source[index + 1] == '/')
            {
                while (index < source.Length && source[index] != '\n')
                {
                    ++index;
                }

                continue;
            }

            char token = source[index];
            if (token != '"')
            {
                ++index;
            }

            return token;
        }

        throw new FormatException("unexpected end of table");
    }

    // 이웃한 C 문자열 하나 이상을 풀어 이어 붙인다(\xHH는 C처럼 16진수 글자를 모두 읽는다).
    private static byte[] ReadStrings(string source, ref int index, string path)
    {
        var bytes = new List<byte>();
        Expect(NextToken(source, ref index) == '"', path, index);
        while (index < source.Length && source[index] == '"')
        {
            ++index;
            while (source[index] != '"')
            {
                char value = source[index++];
                if (value != '\\')
                {
                    bytes.AddRange(Encoding.UTF8.GetBytes(value.ToString()));
                    continue;
                }

                char escape = source[index++];
                switch (escape)
                {
                    case '\\':
                    case '"':
                    case '\'':
                        bytes.Add((byte)escape);
                        break;
                    case 'n':
                        bytes.Add((byte)'\n');
                        break;
                    case 'r':
                        bytes.Add((byte)'\r');
                        break;
                    case 't':
                        bytes.Add((byte)'\t');
                        break;
                    case 'v':
                        bytes.Add((byte)'\v');
                        break;
                    case 'f':
                        bytes.Add((byte)'\f');
                        break;
                    case 'x':
                        {
                            int digits = index;
                            while (index < source.Length && Uri.IsHexDigit(source[index]))
                            {
                                ++index;
                            }

                            int parsed = int.Parse(source.AsSpan(digits, index - digits), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                            Expect(index > digits && parsed <= 0xFF, path, index);
                            bytes.Add((byte)parsed);
                            break;
                        }

                    default:
                        throw new FormatException($"{path}: unsupported escape \\{escape} at offset {index}");
                }
            }

            ++index;
            int next = index;
            if (NextToken(source, ref next) != '"')
            {
                break;
            }

            index = next;
        }

        return bytes.ToArray();
    }
}
