using System.Numerics;
using System.Text;

namespace Rocks.Tooling.Checks;

/// <summary>
/// Core의 CRT 없는 10진 ↔ 2진 실수 변환이 쓰는 5의 거듭제곱 표(<see cref="OutputPath"/>)를 BigInteger 정확 계산으로
/// 만든다(rule id <c>number-tables</c>). 설계: Docs/Design/CoreDataFoundations.ko.md 3.2절.
/// <list type="bullet">
/// <item>Pow5Significand[q + 342] (q ∈ [-342, 308]): 5^q·2^s의 내림(floor)이 [2^127, 2^128)에 들도록 s를 고른 128비트 값과
/// 그 2진 지수 −s(Pow5Exponent). 10진 → 2진 변환(Eisel-Lemire 방식)이 w·10^q를 이 값으로 근사한다. 내림이므로 참값은
/// 표 값 이상, 표 값 + 1 미만이다.</item>
/// <item>Pow5InvSplit[q] (q ∈ [0, 342)): ⌊2^(B(q) − 1 + 125) / 5^q⌋ + 1, Pow5Split[i] (i ∈ [0, 326)): 5^i를 125비트로 맞춘
/// 값(넘치면 내림). B(n)는 5^n의 비트 길이다. 최단 왕복 표기(Ryu)의 표다.</item>
/// </list>
/// 128비트 값은 {하위 64비트, 상위 64비트} 순서다.
/// </summary>
public static class NumberTables
{
    public const string OutputPath = "Core/Source/Runtime/Core/Private/Text/DecimalTables.generated.h";
    public const int GeneratorVersion = 1;
    public const int SmallestPowerOfTen = -342;
    public const int LargestPowerOfTen = 308;
    public const int Pow5InvSplitCount = 342;
    public const int Pow5SplitCount = 326;
    public const int RyuBitCount = 125;

    /// <summary>생성한 헤더 텍스트다.</summary>
    public static string Generate()
    {
        var text = new StringBuilder();
        text.Append("// 생성 파일이다. 직접 고치지 않는다: Rocks.ToolTests number-tables --root . --write\n");
        text.Append($"// 생성기 버전 {GeneratorVersion}. 128비트 값은 {{하위 64비트, 상위 64비트}} 순서다.\n");
        text.Append("#pragma once\n\n#include <Base/Types.h>\n\nnamespace rocks::detail::decimal {\n\n");
        text.Append($"inline constexpr int32 SmallestPowerOfTen = {SmallestPowerOfTen};\n");
        text.Append($"inline constexpr int32 LargestPowerOfTen = {LargestPowerOfTen};\n");
        text.Append($"inline constexpr int32 RyuBitCount = {RyuBitCount};\n\n");
        text.Append("// clang-format off\n");

        var significands = new List<BigInteger>();
        var exponents = new List<int>();
        for (int q = SmallestPowerOfTen; q <= LargestPowerOfTen; ++q)
        {
            (BigInteger value, int exponent) = Pow5Significand(q);
            significands.Add(value);
            exponents.Add(exponent);
        }
        text.Append("// 5^q ≈ Pow5Significand[q + 342] · 2^Pow5Exponent[q + 342](내림, 상위 비트 2^127이 켜져 있다).\n");
        Emit128(text, "Pow5Significand", significands);
        text.Append("inline constexpr int16 Pow5Exponent[] = {\n");
        for (int index = 0; index < exponents.Count; index += 12)
            text.Append("    ").Append(string.Join(", ", exponents.Skip(index).Take(12))).Append(",\n");
        text.Append("};\n\n");

        var inverse = new List<BigInteger>();
        for (int q = 0; q < Pow5InvSplitCount; ++q)
        {
            BigInteger power = BigInteger.Pow(5, q);
            inverse.Add((BigInteger.One << ((int)power.GetBitLength() - 1 + RyuBitCount)) / power + 1);
        }
        text.Append("// Ryu: ⌊2^(B(q) − 1 + 125) / 5^q⌋ + 1(B(q) = 5^q의 비트 길이).\n");
        Emit128(text, "Pow5InvSplit", inverse);

        var split = new List<BigInteger>();
        for (int i = 0; i < Pow5SplitCount; ++i)
        {
            BigInteger power = BigInteger.Pow(5, i);
            int length = (int)power.GetBitLength();
            split.Add(length >= RyuBitCount ? power >> (length - RyuBitCount) : power << (RyuBitCount - length));
        }
        text.Append("// Ryu: 5^i를 125비트로 맞춘 값(넘치는 비트는 버린다).\n");
        Emit128(text, "Pow5Split", split);
        text.Append("// clang-format on\n\n} // namespace rocks::detail::decimal\n");
        return text.ToString();
    }

    // 5^q·2^s의 내림이 [2^127, 2^128)에 들도록 하는 128비트 값과 지수 −s.
    private static (BigInteger Value, int Exponent) Pow5Significand(int q)
    {
        if (q >= 0)
        {
            BigInteger power = BigInteger.Pow(5, q);
            int shift = 128 - (int)power.GetBitLength(); // s
            BigInteger value = shift >= 0 ? power << shift : power >> -shift;
            return (Validate(value), -shift);
        }
        else
        {
            BigInteger power = BigInteger.Pow(5, -q);
            int shift = 127 + (int)power.GetBitLength(); // s: 2^s / 5^-q ∈ (2^127, 2^128)
            return (Validate((BigInteger.One << shift) / power), -shift);
        }
    }

    private static BigInteger Validate(BigInteger value) =>
        value >= (BigInteger.One << 127) && value < (BigInteger.One << 128)
            ? value
            : throw new InvalidOperationException("power-of-five significand is not normalized to 128 bits");

    /// <summary>저장소의 생성 파일이 지금 만든 결과와 같은지 본다.</summary>
    public static IReadOnlyList<CheckFailure> Check(string root)
    {
        string path = Path.Combine(Path.GetFullPath(root), OutputPath);
        if (!Directory.Exists(Path.GetDirectoryName(path)!))
            return [];
        if (!File.Exists(path))
            return [new(OutputPath, 0, "number-tables", "generated decimal tables are missing (run number-tables --write)")];
        return File.ReadAllText(path, new UTF8Encoding(false)) == Generate()
            ? []
            : [new(OutputPath, 0, "number-tables", "generated decimal tables are stale or edited (run number-tables --write)")];
    }

    /// <summary>생성 결과를 쓴다(write가 false면 차이만 본다). 바뀌었으면 true다.</summary>
    public static bool Run(string root, bool write)
    {
        string expected = Generate();
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

    private static void Emit128(StringBuilder text, string name, IReadOnlyList<BigInteger> values)
    {
        BigInteger mask = (BigInteger.One << 64) - 1;
        text.Append($"inline constexpr uint64 {name}[][2] = {{\n");
        foreach (BigInteger value in values)
        {
            if (value.Sign < 0 || value.GetBitLength() > 128)
                throw new InvalidOperationException(name + " value does not fit in 128 bits");
            text.Append($"    {{0x{(ulong)(value & mask):X16}u, 0x{(ulong)(value >> 64):X16}u}},\n");
        }
        text.Append("};\n\n");
    }
}
