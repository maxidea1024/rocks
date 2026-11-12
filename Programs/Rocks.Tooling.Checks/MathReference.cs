using System.Numerics;
using System.Text;

namespace Rocks.Tooling.Checks;

/// <summary>
/// 결정적 수학 정확도 시험의 기준값 표(<see cref="OutputPath"/>)를 BigInteger 고정소수점(400비트)으로 만든다.
/// 각 사례는 (입력 비트, 둘째 입력 비트, 참값을 가장 가깝게 반올림한 double 비트)다. 표본은 고정 시드 xorshift
/// (.NET 버전과 무관)와 검토에서 찾은 최악 입력이다. 설계: Docs/Design/CoreDataFoundations.ko.md 2.3절.
/// </summary>
public static class MathReference
{
    public const string OutputPath = "Core/Source/Runtime/DeterministicMath/Tests/DeterministicMathReference.generated.h";
    private const int Precision = 400;
    private static readonly BigInteger One = BigInteger.One << Precision;
    private static readonly BigInteger Ln2Fixed = 2 * AtanhInverse(3);
    private static readonly BigInteger PiFixed = 16 * AtanInverse(5) - 4 * AtanInverse(239);

    public static string Generate()
    {
        var text = new StringBuilder();
        text.Append("// 생성 파일이다. 직접 고치지 않는다: Rocks.ToolTests math-tables --root . --write\n");
        text.Append("// 정확도 기준값 {입력, 둘째 입력(Pow 지수), 정확히 반올림한 참값}의 double 비트.\n");
        text.Append("// 참값: 400비트 고정소수점(Rocks.Tooling.Checks/MathReference.cs).\n");
        text.Append("#pragma once\n\n#include <Base/Types.h>\n\nnamespace rocks::deterministic::reference {\n\n");
        text.Append("struct ReferenceCase {\n    uint64 input;\n    uint64 second;\n    uint64 expected;\n};\n\n");
        text.Append("// clang-format off\n");
        var random = new XorShift(0x5EED2026u);

        var exp2 = new List<double> { 0.5, -0.25, 10.3, -1074.5, -1022.25, 1023.75 };
        for (int i = 0; i < 80; ++i) exp2.Add(random.Uniform(-1, 1));
        for (int i = 0; i < 60; ++i) exp2.Add(random.Uniform(-60, 60));
        for (int i = 0; i < 40; ++i) exp2.Add(random.Uniform(-1075, -1020));
        for (int i = 0; i < 20; ++i) exp2.Add(random.Uniform(1000, 1024));
        Emit(text, "Exp2Reference", exp2.Select(x => (x, 0.0, Exp2(x))));

        var log2 = new List<double> { 3.0, 0.1, 1.0000001, 1.0890875381237388, 5e-324, 1.5e-310 };
        for (int i = 0; i < 80; ++i) log2.Add(random.Uniform(0.5, 2));
        for (int i = 0; i < 40; ++i) log2.Add(1 + random.Uniform(-1e-6, 1e-6));
        for (int i = 0; i < 80; ++i) log2.Add(random.WideDouble(-1074, 1023));
        Emit(text, "Log2Reference", log2.Select(x => (x, 0.0, Log2(x))));

        var pow = new List<(double, double)>
        {
            (0.5, 2.4), (10.0, 0.3), (0.0535106097531263, 2.4), (0.0020289703374550827, 4.181753663104813),
            (1e300, 1), (10, -300), (0.7702913350633479, 2701.285042709936), (3, 3), (2, 0.5)
        };
        for (int i = 0; i < 100; ++i) pow.Add((random.Uniform(1e-3, 1), random.Uniform(0.1, 5)));
        for (int i = 0; i < 60; ++i) pow.Add((random.Uniform(0.5, 2), random.Uniform(-100, 100)));
        for (int i = 0; i < 60; ++i)
        {
            double x = random.WideDouble(-300, 300);
            double limit = 1000 / Math.Max(1e-9, Math.Abs(Math.Log2(x)));
            pow.Add((x, random.Uniform(-limit, limit)));
        }
        Emit(text, "PowReference", pow.Select(p => (p.Item1, p.Item2, Pow(p.Item1, p.Item2))));

        var trig = new List<double> { 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.4999999999999999, 1e-9, 1234567.125 };
        for (int i = 0; i < 120; ++i) trig.Add(random.Uniform(-4, 4));
        for (int i = 0; i < 30; ++i) trig.Add(random.Uniform(-1e12, 1e12));
        for (int i = 0; i < 30; ++i) trig.Add(Math.Round(random.Uniform(-100, 100)) + 0.5 + random.Uniform(-1e-12, 1e-12));
        Emit(text, "SinPiReference", trig.Select(x => (x, 0.0, SinPi(x, cosine: false))));
        Emit(text, "CosPiReference", trig.Select(x => (x, 0.0, SinPi(x, cosine: true))));

        var srgb = new List<double> { 0.5, 0.047568846871931615, 0.04045000000000001, 1.0, 1.5 };
        for (int i = 0; i < 150; ++i) srgb.Add(random.Uniform(0.0405, 1.5));
        Emit(text, "SrgbToLinearReference", srgb.Select(x => (x, 0.0, SrgbToLinear(x))));

        var linear = new List<double> { 0.5, 0.0032992206618530657, 0.0031308000000000003, 1.0, 2.0 };
        for (int i = 0; i < 150; ++i) linear.Add(random.Uniform(0.00314, 1.5));
        Emit(text, "LinearToSrgbReference", linear.Select(x => (x, 0.0, LinearToSrgb(x))));

        var gamma = new List<double> { 0.5, 0.0006876114417719847, 2.0 };
        for (int i = 0; i < 100; ++i) gamma.Add(random.Uniform(1e-4, 1));
        for (int i = 0; i < 50; ++i) gamma.Add(random.WideDouble(-200, 200));
        Emit(text, "Gamma22Reference", gamma.Select(x => (x, 0.0, PowRational(x, 11, 5))));

        text.Append("// clang-format on\n\n} // namespace rocks::deterministic::reference\n");
        return text.ToString();
    }

    private static void Emit(StringBuilder text, string name, IEnumerable<(double Input, double Second, double Expected)> cases)
    {
        text.Append($"inline constexpr ReferenceCase {name}[] = {{\n");
        foreach (var (input, second, expected) in cases)
            text.Append($"    {{0x{Bits(input):X16}u, 0x{Bits(second):X16}u, 0x{Bits(expected):X16}u}},\n");
        text.Append("};\n\n");
    }

    private static ulong Bits(double value) => BitConverter.DoubleToUInt64Bits(value);

    // ---- 고정밀 함수(값 = 고정소수점 N/2^Precision 또는 (N, 2진 지수)) ----

    // 정확한 double = m·2^e(m 정수).
    private static (BigInteger M, int E) Exact(double value)
    {
        ulong bits = Bits(value) & 0x7FFFFFFFFFFFFFFFul;
        int biased = (int)(bits >> 52);
        BigInteger m = bits & 0xFFFFFFFFFFFFFul;
        if (biased == 0) return (m, -1074);
        return (m | (BigInteger.One << 52), biased - 1075);
    }

    // log2(x)의 고정소수점(x > 0 유한).
    private static BigInteger Log2Fixed(BigInteger numerator, BigInteger denominator)
    {
        // x = numerator/denominator = 2^k·m, m ∈ [1, 2).
        int k = (int)(numerator.GetBitLength() - denominator.GetBitLength());
        BigInteger n = numerator, d = denominator;
        if (k >= 0) d <<= k; else n <<= -k;
        if (n < d) { n <<= 1; --k; }
        // ln m = 2·atanh((m − 1)/(m + 1)) = 2·atanh((n − d)/(n + d)).
        BigInteger lnM = 2 * Atanh(n - d, n + d);
        return k * One + lnM * One / Ln2Fixed;
    }

    private static BigInteger Log2Fixed(double x)
    {
        var (m, e) = Exact(x);
        return e >= 0 ? Log2Fixed(m << e, 1) : Log2Fixed(m, BigInteger.One << -e);
    }

    // atanh(p/q)의 고정소수점(|p/q| < 1).
    private static BigInteger Atanh(BigInteger p, BigInteger q)
    {
        BigInteger sum = 0;
        BigInteger power = p * One / q; // (p/q)^(2k+1)
        BigInteger square = p * p * One / (q * q);
        for (int k = 0; power != 0; ++k)
        {
            sum += power / (2 * k + 1);
            power = power * square >> Precision;
        }
        return sum;
    }

    private static BigInteger AtanhInverse(int n) => Atanh(1, n);

    private static BigInteger AtanInverse(int n)
    {
        BigInteger sum = 0;
        BigInteger power = One / n;
        for (int k = 0; power != 0; ++k)
        {
            sum += (k % 2 == 0 ? power : -power) / (2 * k + 1);
            power /= n * n;
        }
        return sum;
    }

    // 2^t(t 고정소수점) = mantissa·2^exponent로 돌려준다(mantissa 고정소수점 ∈ [1, 2)).
    private static (BigInteger Mantissa, BigInteger Exponent) Exp2Fixed(BigInteger t)
    {
        BigInteger n = BigInteger.Divide(t, One);
        if (t.Sign < 0 && n * One != t) n -= 1; // floor
        BigInteger r = t - n * One;           // [0, 1)
        BigInteger a = r * Ln2Fixed >> Precision; // e^a, a ∈ [0, ln2)
        BigInteger sum = One, term = One;
        for (int k = 1; term != 0; ++k)
        {
            term = term * a / (k * One);
            sum += term;
        }
        return (sum, n);
    }

    // 고정소수점 mantissa·2^exponent(양수)를 가장 가까운 double로(동점 짝수, 비정규·넘침 처리).
    private static double Round(BigInteger fixedMantissa, BigInteger exponent, bool negative = false)
    {
        if (fixedMantissa.Sign == 0) return negative ? -0.0 : 0.0;
        // 값 = fixedMantissa·2^(exponent − Precision).
        long exp2 = (long)exponent - Precision;
        long length = (long)fixedMantissa.GetBitLength();
        long top = exp2 + length - 1;
        if (top > 1023) return negative ? double.NegativeInfinity : double.PositiveInfinity;
        long shift = top >= -1022 ? length - 53 : -1074 - exp2;
        BigInteger m;
        if (shift <= 0) m = fixedMantissa << (int)-shift;
        else
        {
            if (shift > length + 1) return negative ? -0.0 : 0.0;
            m = fixedMantissa >> (int)shift;
            BigInteger remainder = fixedMantissa - (m << (int)shift);
            BigInteger half = BigInteger.One << (int)(shift - 1);
            if (remainder > half || (remainder == half && !m.IsEven)) m += 1;
        }
        long finalExp = exp2 + shift; // 값 = m·2^finalExp
        if (m.GetBitLength() > 53) { m >>= 1; ++finalExp; }
        // m ≤ 2^53이고 결과가 표현 가능하면 ScaleB는 정확하다(넘치면 무한대).
        double result = Math.ScaleB((double)(ulong)m, (int)finalExp);
        return negative ? -result : result;
    }

    private static double Exp2(double x)
    {
        var (m, e) = Exact(Math.Abs(x));
        BigInteger t = e >= 0 ? (m << e) * One : (m * One) >> -e;
        if (x < 0) t = -t;
        var (mantissa, exponent) = Exp2Fixed(t);
        return Round(mantissa, exponent);
    }

    private static double Log2(double x)
    {
        BigInteger value = Log2Fixed(x);
        return Round(BigInteger.Abs(value), 0, value.Sign < 0);
    }

    private static double Pow(double x, double y)
    {
        var (m, e) = Exact(Math.Abs(y));
        BigInteger t = Log2Fixed(x) * m;
        t = e >= 0 ? t << e : t >> -e;
        if (y < 0) t = -t;
        var (mantissa, exponent) = Exp2Fixed(t);
        return Round(mantissa, exponent);
    }

    // x^(p/q).
    private static double PowRational(double x, int p, int q)
    {
        var (mantissa, exponent) = Exp2Fixed(Log2Fixed(x) * p / q);
        return Round(mantissa, exponent);
    }

    private static double SrgbToLinear(double x)
    {
        // ((x + 0.055)/1.055)^(12/5) = ((200x + 11)/211)^(12/5).
        var (m, e) = Exact(x);
        BigInteger numerator = e >= 0 ? (200 * m << e) + 11 : 200 * m + (11 * (BigInteger.One << -e));
        BigInteger denominator = e >= 0 ? 211 : 211 * (BigInteger.One << -e);
        var (mantissa, exponent) = Exp2Fixed(Log2Fixed(numerator, denominator) * 12 / 5);
        return Round(mantissa, exponent);
    }

    private static double LinearToSrgb(double x)
    {
        var (mantissa, exponent) = Exp2Fixed(Log2Fixed(x) * 5 / 12);
        // 1.055·p − 0.055(고정소수점, p = mantissa·2^exponent).
        BigInteger p = exponent >= 0 ? mantissa << (int)exponent : mantissa >> (int)-exponent;
        BigInteger value = (211 * p - 11 * One) / 200;
        return Round(value, 0);
    }

    // sin(πx) 또는 cos(πx).
    private static double SinPi(double x, bool cosine)
    {
        var (m, e) = Exact(x);
        // x mod 2를 정확히: x = m·2^e.
        BigInteger r;
        if (e >= 0) r = (m << e) % 2 * One;
        else
        {
            // 고정소수점이 입력 비트를 모두 담아야 한다(표본은 |x| ≥ 2^−60 정도).
            if (-e > Precision - 100) throw new InvalidOperationException("SinPi reference input is too small");
            BigInteger period = BigInteger.One << (1 - e); // 2·2^−e
            r = (m % period) * One >> -e;                // [0, 2)·One
        }
        if (x < 0) r = (2 * One - r) % (2 * One);
        if (cosine) r = (r + One / 2) % (2 * One); // cos(πx) = sin(π(x + ½))
        // sin(πr), r ∈ [0, 2).
        bool negative = false;
        if (r >= One) { r -= One; negative = true; }   // sin(π(r + 1)) = −sin(πr)
        if (r > One / 2) r = One - r;                   // sin(π(1 − r)) = sin(πr)
        BigInteger angle = r * PiFixed >> Precision;
        BigInteger sum = 0, term = angle, square = angle * angle >> Precision;
        for (int k = 1; term != 0; k += 2)
        {
            sum += term;
            term = -(term * square >> Precision) / ((k + 1) * (k + 2));
        }
        if (sum.Sign < 0) { sum = -sum; negative = !negative; }
        return Round(sum, 0, negative && sum.Sign != 0);
    }

    private sealed class XorShift(ulong state)
    {
        private ulong state = state;

        public ulong Next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        }

        // [low, high)의 double(53비트 균등).
        public double Uniform(double low, double high) => low + (high - low) * ((Next() >> 11) * (1.0 / (1ul << 53)));

        // 지수가 [minExponent, maxExponent]에 고르게 퍼진 양의 double.
        public double WideDouble(int minExponent, int maxExponent)
        {
            int exponent = minExponent + (int)(Next() % (ulong)(maxExponent - minExponent + 1));
            double mantissa = 1 + (Next() >> 11) * (1.0 / (1ul << 53));
            return Math.ScaleB(mantissa, exponent);
        }
    }
}
