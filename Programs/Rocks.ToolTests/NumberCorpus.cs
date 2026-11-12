using System.Globalization;
using System.Numerics;
using System.Text;

namespace Rocks.ToolTests;

/// <summary>
/// Core CRT 없는 숫자 변환의 .NET 대조 말뭉치(CoreDataFoundations.ko.md 3.2절). .NET(Core 3.0+)의 기본 ToString()은 최단
/// 왕복 표기이고 double.Parse·float.Parse는 IEEE 정확 반올림이다. 형식은 FoundationTests의 NumberCorpus.cpp 주석과 같다.
/// 표본은 고정 시드 xorshift(.NET 버전과 무관)로 만든다.
/// </summary>
internal static class NumberCorpus
{
    /// <summary>
    /// .NET 10의 최단 표기가 틀리는 값(.NET 자신의 Parse로도 왕복하지 않는 16자리를 낸다 — 2의 거듭제곱 전체 중 이 둘뿐,
    /// 직접 확인)과 올바른 최단 표기(Python repr과 같은 17자리)다. 이 값들은 올바른 문자열로 대조한다. 표에 없는 값이 왕복하지
    /// 않으면 .NET이 퇴행한 것이므로 말뭉치 생성이 실패한다(조용히 빼지 않는다).
    /// </summary>
    public static readonly IReadOnlyDictionary<ulong, string> KnownDotNetDefects = new Dictionary<ulong, string>
    {
        [0x3E60000000000000ul] = "2.9802322387695312E-08",   // 2^-25(.NET: 2.980232238769531E-08)
        [0x0410000000000000ul] = "4.1045368012983762E-289",  // 2^-958(.NET: 4.104536801298376E-289)
    };

    /// <summary>말뭉치 파일을 쓰고 사례 수를 돌려준다.</summary>
    public static int Write(string path, ulong seed, int shortestDoubles, int shortestFloats, int parses)
    {
        var random = new XorShift(seed);
        var text = new StringBuilder();
        int cases = 0;
        void Line(string line)
        {
            text.Append(line).Append('\n');
            ++cases;
        }

        var invariant = CultureInfo.InvariantCulture;
        // .NET 결함 값은 항상 넣는다.
        foreach (var (bits, shortest) in KnownDotNetDefects)
            Line($"D {bits:X16} {shortest}");
        for (int i = 0; i < shortestDoubles; ++i)
        {
            ulong bits = random.Next() & 0x7FFFFFFFFFFFFFFFul;
            if (i % 8 == 1) bits = (ulong)random.Next(1, 2047) << 52; // 2의 거듭제곱(비대칭 구간), 2^-1022 … 2^1023
            if (i % 8 == 2) bits = random.Next() & 0x000FFFFFFFFFFFFFul; // 비정규
            if (i % 8 == 3) bits = BitConverter.DoubleToUInt64Bits((double)(long)(random.Next() >> random.Next(11, 63)));
            if (i % 8 == 4) bits = ((ulong)random.Next(1, 2047) << 52) - 1; // 2의 거듭제곱 바로 아래(가수 전부 1)
            double value = BitConverter.UInt64BitsToDouble(bits);
            if (!double.IsFinite(value) || value == 0) continue;
            Line($"D {bits:X16} {Shortest(bits, value, invariant)}");
        }
        for (int i = 0; i < shortestFloats; ++i)
        {
            uint bits = (uint)random.Next() & 0x7FFFFFFFu;
            if (i % 8 == 1) bits = (uint)random.Next(1, 255) << 23;
            if (i % 8 == 2) bits = (uint)random.Next() & 0x007FFFFFu;
            if (i % 8 == 4) bits = ((uint)random.Next(1, 255) << 23) - 1;
            float value = BitConverter.UInt32BitsToSingle(bits);
            if (!float.IsFinite(value) || value == 0) continue;
            string shortest = value.ToString(invariant);
            if (float.Parse(shortest, NumberStyles.Float, invariant) != value)
                throw new InvalidOperationException($".NET float shortest does not round-trip (new .NET defect): {bits:X8} {shortest}");
            Line($"F {bits:X8} {shortest}");
        }
        for (int i = 0; i < parses; ++i)
        {
            bool single = i % 2 == 1;
            string decimalText = (i % 8) switch
            {
                0 or 1 or 4 or 5 => RandomDecimal(random, single),
                _ => Midpoint(random, single),
            };
            if (!single)
            {
                double parsed = double.Parse(decimalText, NumberStyles.Float, invariant);
                ulong expected = double.IsInfinity(parsed) ? 0x7FF0000000000000ul : BitConverter.DoubleToUInt64Bits(parsed);
                Line($"P {decimalText} {expected:X16}");
            }
            else
            {
                float parsed = float.Parse(decimalText, NumberStyles.Float, invariant);
                uint expected = float.IsInfinity(parsed) ? 0x7F800000u : BitConverter.SingleToUInt32Bits(parsed);
                Line($"Q {decimalText} {expected:X8}");
            }
        }
        File.WriteAllText(path, text.ToString(), new UTF8Encoding(false));
        return cases;
    }

    // .NET 기본 ToString()의 최단 표기. 알려진 결함 값은 올바른 문자열로 바꾸고, 그 밖에 .NET 자신의 Parse로 왕복하지 않으면
    // 실패한다.
    private static string Shortest(ulong bits, double value, CultureInfo invariant)
    {
        if (KnownDotNetDefects.TryGetValue(bits, out string? known))
            return known;
        string text = value.ToString(invariant);
        if (double.Parse(text, NumberStyles.Float, invariant) != value)
            throw new InvalidOperationException($".NET double shortest does not round-trip (new .NET defect): {bits:X16} {text}");
        return text;
    }

    // 무작위 10진 문자열: 부호·자릿수(대부분 1–25, 가끔 수백)·소수점 위치·지수(float는 float 범위 근처).
    private static string RandomDecimal(XorShift random, bool single)
    {
        var text = new StringBuilder();
        if (random.Next(0, 4) == 0) text.Append('-');
        int digits = random.Next(0, 16) == 0 ? random.Next(26, 900) : random.Next(1, 26);
        int point = random.Next(0, digits + 1);
        for (int i = 0; i < digits; ++i)
        {
            if (i == point && i != 0) text.Append('.');
            text.Append((char)('0' + random.Next(0, 10)));
        }
        // 값의 10진 지수가 대략 [-330, 310](double)·[-48, 40](float)에 들도록 소수점 위치를 보정한다.
        int exponent = single ? random.Next(-48, 40) : random.Next(-330, 310);
        text.Append('e').Append(exponent - point);
        return text.ToString();
    }

    // 이웃 두 값의 정확한 중간점과 그 바로 아래·위(마지막 자리 −1, 먼 꼬리 1). 1/8은 가수를 전부 1(2의 거듭제곱 바로 아래의
    // 비대칭 중간점, 최대 지수면 넘침 경계)이나 0으로 강제한다.
    private static string Midpoint(XorShift random, bool single)
    {
        int mantissaBits = single ? 23 : 52;
        int maxBiased = single ? 254 : 2046;
        int bias = single ? 127 : 1023;
        long biased = random.Next(0, maxBiased + 1);
        BigInteger fraction = new BigInteger(random.Next()) & ((BigInteger.One << mantissaBits) - 1);
        switch (random.Next(0, 16))
        {
            case 0: fraction = (BigInteger.One << mantissaBits) - 1; break;
            case 1: fraction = 0; break;
            case 2: fraction = (BigInteger.One << mantissaBits) - 1; biased = maxBiased; break; // max와 2^(최대+1) 사이
        }
        BigInteger m = biased == 0 ? fraction : fraction | (BigInteger.One << mantissaBits);
        long e = (biased == 0 ? 1 : biased) - bias - mantissaBits;
        // 중간점 = (2m + 1)·2^(e − 1).
        BigInteger numerator = 2 * m + 1;
        long shift = e - 1;
        BigInteger digits;
        long decimalExponent;
        if (shift >= 0)
        {
            digits = numerator << (int)shift;
            decimalExponent = 0;
        }
        else
        {
            digits = numerator * BigInteger.Pow(5, (int)-shift);
            decimalExponent = shift;
        }
        switch (random.Next(0, 3))
        {
            case 1: digits -= 1; break;
            case 2: digits = digits * 10 + 1; --decimalExponent; break;
        }
        return digits.ToString(CultureInfo.InvariantCulture) + "e" + decimalExponent.ToString(CultureInfo.InvariantCulture);
    }

    private sealed class XorShift(ulong seed)
    {
        // splitmix로 섞어 상태가 0이 되지 않게 한다(0이면 xorshift가 0만 낸다).
        private ulong state = Mix(seed) | 1;

        private static ulong Mix(ulong value)
        {
            value += 0x9E3779B97F4A7C15ul;
            value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ul;
            value = (value ^ (value >> 27)) * 0x94D049BB133111EBul;
            return value ^ (value >> 31);
        }

        public ulong Next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }

        public int Next(int low, int high) => low + (int)(Next() % (ulong)(high - low));
    }
}
