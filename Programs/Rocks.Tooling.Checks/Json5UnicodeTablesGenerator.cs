using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

/// <summary>
/// C# JSON5 reader(<see cref="Json5Reader"/>)가 공백·식별자 글자를 판정하는 General_Category 구간 표(<see cref="OutputPath"/>)를
/// Core와 같은 UCD 원본(<c>Core/Source/ThirdParty/UnicodeData</c>의 UnicodeData.txt)에서 만든다. .NET의
/// <c>char.GetUnicodeCategory</c>는 런타임마다 Unicode 버전이 달라 C++ Core(<c>GetGeneralCategory</c>, 같은 UCD로 생성한 표)와
/// 판정이 갈릴 수 있으므로 쓰지 않는다. 결과는 결정적이다(코드 포인트 순서·고정 서식·LF). 정책 검사 <c>json5-unicode-tables</c>는
/// 저장소의 파일이 지금 원본으로 만든 결과와 byte 단위로 같은지 본다.
/// </summary>
public static class Json5UnicodeTablesGenerator
{
    public const string OutputPath = "Programs/Rocks.Tooling.Common/Json5UnicodeTables.generated.cs";

    public const int GeneratorVersion = 1;

    private const string InputName = "UnicodeData.txt";

    private const int CodePointCount = 0x110000;

    private const int PairsPerLine = 6;

    // 표 이름, 설명, 담는 General_Category다(Json5Parser가 쓰는 묶음).
    private static readonly (string Name, string Description, string[] Categories)[] Tables =
    [
        ("SpaceSeparator", "General_Category Zs (JSON5 white space besides the listed code points)", ["Zs"]),
        ("IdentifierStartLetter", "UnicodeLetter of ECMAScript 5.1 IdentifierStart: Lu Ll Lt Lm Lo Nl", ["Lu", "Ll", "Lt", "Lm", "Lo", "Nl"]),
        ("IdentifierPartOther", "IdentifierPart categories besides IdentifierStart: Mn Mc Nd Pc", ["Mn", "Mc", "Nd", "Pc"]),
    ];

    /// <summary>원본에서 생성한 C# 소스 텍스트다. 원본이 manifest와 다르거나 형식이 틀리면 <see cref="FormatException"/>이다.</summary>
    public static string Generate(string root)
    {
        string library = Path.Combine(Path.GetFullPath(root), ThirdPartyManifest.CoreThirdPartyRoot, UnicodeTables.LibraryName);
        ThirdPartyManifest manifest = ThirdPartyManifest.Read(library);
        if (manifest.Kind != ThirdPartyKind.Data)
        {
            throw new FormatException($"{UnicodeTables.LibraryName}: manifest must be a data library");
        }

        byte[] bytes = File.ReadAllBytes(Path.Combine(library, manifest.UpstreamDirectory, InputName));
        string hash = Convert.ToHexStringLower(SHA256.HashData(bytes));
        if (!manifest.Files.TryGetValue(InputName, out string? listed) || listed != hash)
        {
            throw new FormatException($"{UnicodeTables.LibraryName}: {InputName} differs from the manifest SHA-256");
        }

        string[] categories = ReadCategories(new UTF8Encoding(false, true).GetString(bytes).Split('\n'));
        var text = new StringBuilder();
        void Line(string value = "") => text.Append(value).Append('\n');
        Line("// Generated file. Do not edit by hand: Rocks.ToolTests json5-unicode-tables --root . --write");
        Line($"// Unicode {manifest.Version}, generator version {GeneratorVersion}. Source SHA-256:");
        Line($"//   {InputName} {hash}");
        Line("namespace Rocks.Tooling.Common;");
        Line();
        Line("/// <summary>");
        Line("/// General_Category code point ranges for the JSON5 reader, generated from the same UCD as the C++ Core tables.");
        Line("/// Each table is a sorted list of inclusive (first, last) pairs.");
        Line("/// </summary>");
        Line("internal static class Json5UnicodeTables");
        Line("{");
        Line($"    public const string UnicodeVersion = \"{manifest.Version}\";");
        foreach ((string name, string description, string[] members) in Tables)
        {
            var ranges = new List<(int First, int Last)>();
            for (int point = 0; point < CodePointCount; ++point)
            {
                if (!members.Contains(categories[point]))
                {
                    continue;
                }

                if (ranges.Count != 0 && ranges[^1].Last == point - 1)
                {
                    ranges[^1] = (ranges[^1].First, point);
                }
                else
                {
                    ranges.Add((point, point));
                }
            }

            Line();
            Line($"    // {description}.");
            Line($"    public static ReadOnlySpan<int> {name} =>");
            Line("    [");
            for (int start = 0; start < ranges.Count; start += PairsPerLine)
            {
                IEnumerable<string> pairs = ranges.Skip(start).Take(PairsPerLine)
                    .Select(range => string.Create(CultureInfo.InvariantCulture, $"0x{range.First:X4}, 0x{range.Last:X4}"));
                bool last = start + PairsPerLine >= ranges.Count;
                Line("        " + string.Join(", ", pairs) + (last ? "" : ","));
            }

            Line("    ];");
        }

        Line("}");
        return text.ToString();
    }

    /// <summary>저장소의 생성 파일이 원본으로 만든 결과와 같은지 본다(rule id <c>json5-unicode-tables</c>).</summary>
    public static IReadOnlyList<CheckFailure> Check(string root)
    {
        string library = Path.Combine(Path.GetFullPath(root), ThirdPartyManifest.CoreThirdPartyRoot, UnicodeTables.LibraryName);
        if (!Directory.Exists(library))
        {
            return [];
        }

        string expected;
        try
        {
            expected = Generate(root);
        }
        catch (Exception error) when (error is FormatException or IOException)
        {
            return [new(OutputPath, 0, "json5-unicode-tables", error.Message)];
        }

        string path = Path.Combine(Path.GetFullPath(root), OutputPath);
        if (!File.Exists(path))
        {
            return [new(OutputPath, 0, "json5-unicode-tables", "generated JSON5 Unicode tables are missing (run json5-unicode-tables --write)")];
        }

        return File.ReadAllText(path, new UTF8Encoding(false)) == expected
            ? []
            : [new(OutputPath, 0, "json5-unicode-tables", "generated JSON5 Unicode tables are stale or edited (run json5-unicode-tables --write)")];
    }

    /// <summary>생성 결과를 쓴다(write가 false면 차이만 본다). 바뀌었으면(쓰기 전 기준) true다.</summary>
    public static bool Run(string root, bool write)
    {
        string expected = Generate(root);
        string path = Path.Combine(Path.GetFullPath(root), OutputPath);
        if (File.Exists(path) && File.ReadAllText(path, new UTF8Encoding(false)) == expected)
        {
            return false;
        }

        if (write)
        {
            string temporary = path + ".tmp";
            File.WriteAllText(temporary, expected, new UTF8Encoding(false));
            File.Move(temporary, path, overwrite: true);
        }

        return true;
    }

    // 코드 포인트마다 General_Category 약어다(목록에 없으면 Cn). "<..., First>"·"<..., Last>" 구간을 펼친다.
    private static string[] ReadCategories(string[] lines)
    {
        string[] categories = Enumerable.Repeat("Cn", CodePointCount).ToArray();
        int? rangeFirst = null;
        foreach (string raw in lines)
        {
            string line = raw.Trim();
            if (line.Length == 0)
            {
                continue;
            }

            string[] fields = line.Split(';');
            if (fields.Length < 15)
            {
                throw new FormatException($"{InputName}: malformed line");
            }

            int code = int.Parse(fields[0], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            if (code < 0 || code >= CodePointCount || fields[2].Length != 2)
            {
                throw new FormatException($"{InputName}: invalid record {fields[0]}");
            }

            if (fields[1].EndsWith(", First>", StringComparison.Ordinal))
            {
                rangeFirst = code;
                continue;
            }

            int first = code;
            if (fields[1].EndsWith(", Last>", StringComparison.Ordinal))
            {
                first = rangeFirst ?? throw new FormatException($"{InputName}: range Last without First");
                rangeFirst = null;
            }

            for (int point = first; point <= code; ++point)
            {
                categories[point] = fields[2];
            }
        }

        return categories;
    }
}
