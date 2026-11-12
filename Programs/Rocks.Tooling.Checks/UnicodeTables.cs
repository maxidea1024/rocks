using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

/// <summary>
/// Unicode 데이터(UCD, <c>Core/Source/ThirdParty/UnicodeData</c>)에서 Core의 정규화·case folding·General_Category·
/// Default_Ignorable 표(<see cref="OutputPath"/>)를 만든다. 결과는 결정적이다(정렬된 순회·고정 서식·LF). 정책 검사
/// <c>unicode-tables</c>는 저장소의 파일이 지금 원본으로 만든 결과와 byte 단위로 같은지 본다.
/// 설계: Docs/Design/CoreDataFoundations.ko.md 1.3절.
/// </summary>
public static class UnicodeTables
{
    public const string LibraryName = "UnicodeData";
    public const string OutputPath = "Core/Source/Runtime/Core/Private/Text/UnicodeTables.generated.h";
    public const int GeneratorVersion = 1;

    private const int CodePointCount = 0x110000;
    private const int BlockShift = 8;
    private const int BlockSize = 1 << BlockShift;

    // GeneralCategory 열거 순서(Core/Text/Unicode.h와 같다).
    private static readonly string[] Categories =
    [
        "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po",
        "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn"
    ];

    private static readonly string[] Inputs =
    [
        "UnicodeData.txt", "DerivedNormalizationProps.txt", "CompositionExclusions.txt", "CaseFolding.txt",
        "DerivedCoreProperties.txt"
    ];

    /// <summary>원본에서 생성한 헤더 텍스트다. 원본이 계약과 다르면(<see cref="FormatException"/>) 표를 만들지 않는다.</summary>
    public static string Generate(string root)
    {
        string library = Path.Combine(Path.GetFullPath(root), ThirdPartyManifest.ThirdPartyRoot, LibraryName);
        ThirdPartyManifest manifest = ThirdPartyManifest.Read(library);
        if (manifest.Kind != ThirdPartyKind.Data)
            throw new FormatException($"{LibraryName}: manifest must be a data library");
        string upstream = Path.Combine(library, manifest.UpstreamDirectory);
        var hashes = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var texts = new Dictionary<string, string[]>(StringComparer.Ordinal);
        foreach (string name in Inputs)
        {
            byte[] bytes = File.ReadAllBytes(Path.Combine(upstream, name));
            string hash = Convert.ToHexStringLower(SHA256.HashData(bytes));
            if (!manifest.Files.TryGetValue(name, out string? listed) || listed != hash)
                throw new FormatException($"{LibraryName}: {name} differs from the manifest SHA-256");
            hashes[name] = hash;
            texts[name] = new UTF8Encoding(false, true).GetString(bytes).Split('\n');
        }

        var data = new Data();
        ParseUnicodeData(texts["UnicodeData.txt"], data);
        ParseNormalizationProps(texts["DerivedNormalizationProps.txt"], data);
        ParseCaseFolding(texts["CaseFolding.txt"], data);
        ParseDefaultIgnorable(texts["DerivedCoreProperties.txt"], data);
        CheckCompositionExclusions(texts["CompositionExclusions.txt"], data);
        return Emit(manifest.Version, hashes, data);
    }

    /// <summary>저장소의 생성 파일이 원본으로 만든 결과와 같은지 본다(rule id <c>unicode-tables</c>).</summary>
    public static IReadOnlyList<CheckFailure> Check(string root)
    {
        string library = Path.Combine(Path.GetFullPath(root), ThirdPartyManifest.ThirdPartyRoot, LibraryName);
        if (!Directory.Exists(library))
            return [];
        string expected;
        try
        {
            expected = Generate(root);
        }
        catch (Exception error) when (error is FormatException or IOException)
        {
            return [new(OutputPath, 0, "unicode-tables", error.Message)];
        }
        string path = Path.Combine(Path.GetFullPath(root), OutputPath);
        if (!File.Exists(path))
            return [new(OutputPath, 0, "unicode-tables", "generated Unicode tables are missing (run unicode-tables --write)")];
        return File.ReadAllText(path, new UTF8Encoding(false)) == expected
            ? []
            : [new(OutputPath, 0, "unicode-tables", "generated Unicode tables are stale or edited (run unicode-tables --write)")];
    }

    /// <summary>생성 결과를 쓴다(write가 false면 차이만 보고). 바뀌었으면 true다.</summary>
    public static bool Run(string root, bool write)
    {
        string expected = Generate(root);
        string path = Path.Combine(Path.GetFullPath(root), OutputPath);
        if (File.Exists(path) && File.ReadAllText(path, new UTF8Encoding(false)) == expected)
            return false;
        if (write)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            string temporary = path + ".tmp";
            File.WriteAllText(temporary, expected, new UTF8Encoding(false));
            File.Move(temporary, path, overwrite: true);
        }
        return true;
    }

    private sealed class Data
    {
        public readonly byte[] Category = Enumerable.Repeat((byte)(Categories.Length - 1), CodePointCount).ToArray();
        public readonly byte[] CombiningClass = new byte[CodePointCount];
        public readonly Dictionary<int, int[]> Decomposition = new();   // 정규 분해 1단계(호환 분해 제외)
        public readonly bool[] FullCompositionExclusion = new bool[CodePointCount];
        public readonly byte[] NfcQuickCheck = new byte[CodePointCount];  // 0 Yes, 1 No, 2 Maybe
        public readonly byte[] NfdQuickCheck = new byte[CodePointCount];  // 0 Yes, 1 No
        public readonly SortedDictionary<int, int> SimpleFolding = new();
        public readonly bool[] DefaultIgnorable = new bool[CodePointCount];
    }

    private static IEnumerable<string[]> Records(string[] lines)
    {
        foreach (string raw in lines)
        {
            int comment = raw.IndexOf('#');
            string line = (comment >= 0 ? raw[..comment] : raw).Trim();
            if (line.Length != 0)
                yield return line.Split(';').Select(field => field.Trim()).ToArray();
        }
    }

    private static int Hex(string text)
    {
        int value = int.Parse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        if (value < 0 || value >= CodePointCount)
            throw new FormatException($"code point {text} out of range");
        return value;
    }

    private static (int First, int Last) Range(string text)
    {
        int dots = text.IndexOf("..", StringComparison.Ordinal);
        return dots < 0 ? (Hex(text), Hex(text)) : (Hex(text[..dots]), Hex(text[(dots + 2)..]));
    }

    private static void ParseUnicodeData(string[] lines, Data data)
    {
        int? rangeFirst = null;
        foreach (string[] fields in Records(lines))
        {
            if (fields.Length < 15)
                throw new FormatException("UnicodeData.txt: malformed line");
            int code = Hex(fields[0]);
            int category = System.Array.IndexOf(Categories, fields[2]);
            if (category < 0)
                throw new FormatException($"UnicodeData.txt: unknown General_Category {fields[2]}");
            byte combining = byte.Parse(fields[3], CultureInfo.InvariantCulture);
            if (fields[1].EndsWith(", First>", StringComparison.Ordinal))
            {
                rangeFirst = code;
                continue;
            }
            int first = code;
            if (fields[1].EndsWith(", Last>", StringComparison.Ordinal))
            {
                first = rangeFirst ?? throw new FormatException("UnicodeData.txt: range Last without First");
                rangeFirst = null;
            }
            for (int point = first; point <= code; ++point)
            {
                data.Category[point] = (byte)category;
                data.CombiningClass[point] = combining;
            }
            if (fields[5].Length != 0 && !fields[5].StartsWith('<'))
                data.Decomposition[code] = fields[5].Split(' ', StringSplitOptions.RemoveEmptyEntries).Select(Hex).ToArray();
        }
    }

    private static void ParseNormalizationProps(string[] lines, Data data)
    {
        foreach (string[] fields in Records(lines))
        {
            (int first, int last) = Range(fields[0]);
            for (int point = first; point <= last; ++point)
            {
                switch (fields[1])
                {
                    case "Full_Composition_Exclusion":
                        data.FullCompositionExclusion[point] = true;
                        break;
                    case "NFC_QC":
                        data.NfcQuickCheck[point] = fields[2] switch
                        {
                            "N" => 1, "M" => 2, _ => throw new FormatException("NFC_QC: unknown value " + fields[2])
                        };
                        break;
                    case "NFD_QC":
                        data.NfdQuickCheck[point] = fields[2] == "N" ? (byte)1 : throw new FormatException("NFD_QC: unknown value " + fields[2]);
                        break;
                }
            }
        }
    }

    private static void ParseCaseFolding(string[] lines, Data data)
    {
        foreach (string[] fields in Records(lines))
            if (fields[1] is "C" or "S")
                data.SimpleFolding.Add(Hex(fields[0]), Hex(fields[2]));
    }

    private static void ParseDefaultIgnorable(string[] lines, Data data)
    {
        foreach (string[] fields in Records(lines))
        {
            if (fields[1] != "Default_Ignorable_Code_Point")
                continue;
            (int first, int last) = Range(fields[0]);
            for (int point = first; point <= last; ++point)
                data.DefaultIgnorable[point] = true;
        }
    }

    // Full_Composition_Exclusion = CompositionExclusions ∪ singleton 분해 ∪ 비시작 문자 분해여야 한다(UAX #15).
    private static void CheckCompositionExclusions(string[] lines, Data data)
    {
        var derived = new HashSet<int>();
        foreach (string[] fields in Records(lines))
        {
            (int first, int last) = Range(fields[0]);
            for (int point = first; point <= last; ++point)
                derived.Add(point);
        }
        foreach ((int code, int[] mapping) in data.Decomposition)
            if (mapping.Length == 1 || data.CombiningClass[code] != 0 || data.CombiningClass[mapping[0]] != 0)
                derived.Add(code);
        for (int point = 0; point < CodePointCount; ++point)
            if (data.FullCompositionExclusion[point] != derived.Contains(point))
                throw new FormatException($"Full_Composition_Exclusion disagrees with the derivation at U+{point:X4}");
    }

    private static int[] FullDecomposition(Data data, int code)
    {
        if (!data.Decomposition.TryGetValue(code, out int[]? mapping))
            return [code];
        return mapping.SelectMany(point => FullDecomposition(data, point)).ToArray();
    }

    private static string Emit(string version, IReadOnlyDictionary<string, string> hashes, Data data)
    {
        // 코드 포인트별 속성: (ccc, category, NFC_QC, NFD_QC, Default_Ignorable)의 고유 조합 표 + 2단계 색인.
        var records = new List<uint>();
        var recordIndex = new Dictionary<uint, ushort>();
        var blocks = new List<ushort[]>();
        var blockIndex = new Dictionary<string, ushort>(StringComparer.Ordinal);
        var stage1 = new ushort[CodePointCount >> BlockShift];
        for (int block = 0; block < stage1.Length; ++block)
        {
            var values = new ushort[BlockSize];
            for (int offset = 0; offset < BlockSize; ++offset)
            {
                int point = (block << BlockShift) | offset;
                uint record = (uint)data.CombiningClass[point] | ((uint)data.Category[point] << 8) |
                              ((uint)data.NfcQuickCheck[point] << 13) | ((uint)data.NfdQuickCheck[point] << 15) |
                              (data.DefaultIgnorable[point] ? 1u << 16 : 0u);
                if (!recordIndex.TryGetValue(record, out ushort index))
                {
                    index = checked((ushort)records.Count);
                    records.Add(record);
                    recordIndex.Add(record, index);
                }
                values[offset] = index;
            }
            string key = string.Join(',', values);
            if (!blockIndex.TryGetValue(key, out ushort id))
            {
                id = checked((ushort)blocks.Count);
                blocks.Add(values);
                blockIndex.Add(key, id);
            }
            stage1[block] = id;
        }

        // 정규 분해(완전 분해, Hangul 제외): 코드 포인트 정렬 색인 + 펼친 배열.
        var decompositionKeys = data.Decomposition.Keys.Order().ToArray();
        var decompositionData = new List<int>();
        var decompositionEntries = new List<(int Code, int Offset, int Length)>();
        foreach (int code in decompositionKeys)
        {
            int[] full = FullDecomposition(data, code);
            decompositionEntries.Add((code, decompositionData.Count, full.Length));
            decompositionData.AddRange(full);
        }

        // 정규 합성 쌍: 1단계 두 문자 분해 중 Full_Composition_Exclusion이 아닌 것.
        var compositions = data.Decomposition.Where(pair => pair.Value.Length == 2 && !data.FullCompositionExclusion[pair.Key])
            .Select(pair => (Key: ((ulong)(uint)pair.Value[0] << 21) | (uint)pair.Value[1], Composite: pair.Key))
            .OrderBy(pair => pair.Key).ToArray();

        // 단순 case folding 구간: (first, last, stride, delta). 같은 차이로 stride 1 또는 2씩 이어지는 항목을 묶는다.
        var foldingRanges = new List<(int First, int Last, int Stride, int Delta)>();
        foreach ((int code, int folded) in data.SimpleFolding)
        {
            int delta = folded - code;
            if (foldingRanges.Count != 0)
            {
                var last = foldingRanges[^1];
                bool single = last.First == last.Last;
                int stride = single ? code - last.Last : last.Stride;
                if (last.Delta == delta && (stride is 1 or 2) && code - last.Last == stride)
                {
                    foldingRanges[^1] = (last.First, code, stride, delta);
                    continue;
                }
            }
            foldingRanges.Add((code, code, 1, delta));
        }

        int maximumExpansion = decompositionEntries.Count == 0 ? 1 : decompositionEntries.Max(entry => entry.Length);
        var text = new StringBuilder();
        void Line(string value = "") => text.Append(value).Append('\n');
        Line("// 생성 파일이다. 직접 고치지 않는다: Rocks.ToolTests unicode-tables --root . --write");
        Line($"// Unicode {version}, 생성기 버전 {GeneratorVersion}. 원본 SHA-256:");
        foreach ((string name, string hash) in hashes)
            Line($"//   {name} {hash}");
        Line("#pragma once");
        Line();
        Line("#include <Base/Types.h>");
        Line();
        Line("namespace rocks::detail::unicode {");
        Line();
        Line($"inline constexpr char UnicodeVersion[] = \"{version}\";");
        Line($"inline constexpr uint32 MaxDecompositionLength = {maximumExpansion};");
        Line();
        Line("// 코드 포인트 속성 조합: bit 0–7 결합 등급, 8–12 GeneralCategory, 13–14 NFC_QC(0 Yes·1 No·2 Maybe),");
        Line("// 15 NFD_QC(0 Yes·1 No), 16 Default_Ignorable.");
        Line("// clang-format off");
        EmitArray(text, "uint32", "PropertyRecords", records.Select(value => value.ToString(CultureInfo.InvariantCulture)), 8);
        Line($"// 상위 색인: 코드 포인트 >> {BlockShift} → 블록 번호.");
        EmitArray(text, "uint16", "PropertyStage1", stage1.Select(value => value.ToString(CultureInfo.InvariantCulture)), 16);
        Line($"// 블록({BlockSize}칸)마다 속성 조합 번호. 블록 b의 칸 o = PropertyBlocks[b * {BlockSize} + o].");
        EmitArray(text, "uint16", "PropertyBlocks", blocks.SelectMany(block => block).Select(value => value.ToString(CultureInfo.InvariantCulture)), 16);
        Line("// 정규 분해 색인(코드 포인트 순): { 코드 포인트, DecompositionData 시작, 길이 }. Hangul 음절은 알고리즘.");
        Line("struct DecompositionEntry {");
        Line("    uint32 codePoint;");
        Line("    uint32 offset;");
        Line("    uint32 length;");
        Line("};");
        EmitArray(text, "DecompositionEntry", "DecompositionEntries",
            decompositionEntries.Select(entry => $"{{0x{entry.Code:X}, {entry.Offset}, {entry.Length}}}"), 3);
        EmitArray(text, "uint32", "DecompositionData", decompositionData.Select(value => $"0x{value:X}"), 8);
        Line("// 정규 합성 쌍(키 순): 키 = (앞 문자 << 21) | 뒤 문자.");
        Line("struct CompositionEntry {");
        Line("    uint64 key;");
        Line("    uint32 composite;");
        Line("};");
        EmitArray(text, "CompositionEntry", "CompositionEntries",
            compositions.Select(entry => $"{{0x{entry.Key:X}ull, 0x{entry.Composite:X}}}"), 3);
        Line("// 단순 case folding 구간: first..last에서 stride 간격의 코드 포인트를 delta만큼 옮긴다.");
        Line("struct FoldingRange {");
        Line("    uint32 first;");
        Line("    uint32 last;");
        Line("    uint32 stride;");
        Line("    int32 delta;");
        Line("};");
        EmitArray(text, "FoldingRange", "FoldingRanges",
            foldingRanges.Select(range => $"{{0x{range.First:X}, 0x{range.Last:X}, {range.Stride}, {range.Delta}}}"), 3);
        Line("// clang-format on");
        Line();
        Line("} // namespace rocks::detail::unicode");
        return text.ToString();
    }

    private static void EmitArray(StringBuilder text, string type, string name, IEnumerable<string> values, int perLine)
    {
        string[] items = values.ToArray();
        text.Append($"inline constexpr {type} {name}[] = {{\n");
        for (int index = 0; index < items.Length; index += perLine)
            text.Append("    ").Append(string.Join(", ", items.Skip(index).Take(perLine))).Append(",\n");
        text.Append("};\n");
        text.Append($"inline constexpr usize {name}Count = {items.Length};\n\n");
    }
}
