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
        text.Append("// Generated file. Do not edit by hand: Rocks.ToolTests math-tables --root . --write\n");
        text.Append("// Accuracy reference cases as double bits:\n");
        text.Append("// {input, second input (Pow exponent), correctly rounded exact value}.\n");
        text.Append("// Exact values: 400-bit fixed point (Rocks.Tooling.Checks/MathReference.cs).\n");
        text.Append("#pragma once\n\n#include <Base/Types.h>\n\nnamespace rocks::deterministic::reference {\n\n");
        text.Append("struct ReferenceCase {\n    uint64 input;\n    uint64 second;\n    uint64 expected;\n};\n\n");
        text.Append("// clang-format off\n");
        var random = new XorShift(0x5EED2026u);

        var exp2 = new List<double> { 0.5, -0.25, 10.3, -1074.5, -1022.25, 1023.75 };
        for (int i = 0; i < 80; ++i)
        {
            exp2.Add(random.Uniform(-1, 1));
        }

        for (int i = 0; i < 60; ++i)
        {
            exp2.Add(random.Uniform(-60, 60));
        }

        for (int i = 0; i < 40; ++i)
        {
            exp2.Add(random.Uniform(-1075, -1020));
        }

        for (int i = 0; i < 20; ++i)
        {
            exp2.Add(random.Uniform(1000, 1024));
        }

        Emit(text, "Exp2Reference", exp2.Select(x => (x, 0.0, Exp2(x))));

        var log2 = new List<double> { 3.0, 0.1, 1.0000001, 1.0890875381237388, 5e-324, 1.5e-310 };
        for (int i = 0; i < 80; ++i)
        {
            log2.Add(random.Uniform(0.5, 2));
        }

        for (int i = 0; i < 40; ++i)
        {
            log2.Add(1 + random.Uniform(-1e-6, 1e-6));
        }

        for (int i = 0; i < 80; ++i)
        {
            log2.Add(random.WideDouble(-1074, 1023));
        }

        Emit(text, "Log2Reference", log2.Select(x => (x, 0.0, Log2(x))));

        var pow = new List<(double, double)>
        {
            (0.5, 2.4), (10.0, 0.3), (0.0535106097531263, 2.4), (0.0020289703374550827, 4.181753663104813),
            (1e300, 1), (10, -300), (0.7702913350633479, 2701.285042709936), (3, 3), (2, 0.5)
        };
        for (int i = 0; i < 100; ++i)
        {
            pow.Add((random.Uniform(1e-3, 1), random.Uniform(0.1, 5)));
        }

        for (int i = 0; i < 60; ++i)
        {
            pow.Add((random.Uniform(0.5, 2), random.Uniform(-100, 100)));
        }

        for (int i = 0; i < 60; ++i)
        {
            double x = random.WideDouble(-300, 300);
            double limit = 1000 / Math.Max(1e-9, Math.Abs(Math.Log2(x)));
            pow.Add((x, random.Uniform(-limit, limit)));
        }

        Emit(text, "PowReference", pow.Select(p => (p.Item1, p.Item2, Pow(p.Item1, p.Item2))));

        var trig = new List<double> { 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.4999999999999999, 1e-9, 1234567.125 };
        for (int i = 0; i < 120; ++i)
        {
            trig.Add(random.Uniform(-4, 4));
        }

        for (int i = 0; i < 30; ++i)
        {
            trig.Add(random.Uniform(-1e12, 1e12));
        }

        for (int i = 0; i < 30; ++i)
        {
            trig.Add(Math.Round(random.Uniform(-100, 100)) + 0.5 + random.Uniform(-1e-12, 1e-12));
        }

        Emit(text, "SinPiReference", trig.Select(x => (x, 0.0, SinPi(x, cosine: false))));
        Emit(text, "CosPiReference", trig.Select(x => (x, 0.0, SinPi(x, cosine: true))));

        var srgb = new List<double> { 0.5, 0.047568846871931615, 0.04045000000000001, 1.0, 1.5 };
        for (int i = 0; i < 150; ++i)
        {
            srgb.Add(random.Uniform(0.0405, 1.5));
        }

        Emit(text, "SrgbToLinearReference", srgb.Select(x => (x, 0.0, SrgbToLinear(x))));

        var linear = new List<double> { 0.5, 0.0032992206618530657, 0.0031308000000000003, 1.0, 2.0 };
        for (int i = 0; i < 150; ++i)
        {
            linear.Add(random.Uniform(0.00314, 1.5));
        }

        Emit(text, "LinearToSrgbReference", linear.Select(x => (x, 0.0, LinearToSrgb(x))));

        var gamma = new List<double> { 0.5, 0.0006876114417719847, 2.0 };
        for (int i = 0; i < 100; ++i)
        {
            gamma.Add(random.Uniform(1e-4, 1));
        }

        for (int i = 0; i < 50; ++i)
        {
            gamma.Add(random.WideDouble(-200, 200));
        }

        Emit(text, "Gamma22Reference", gamma.Select(x => (x, 0.0, PowRational(x, 11, 5))));

        // Acos: 분기 경계(±½)·끝점(±1)·0 근처, 고른 표본, ±1 근처(√ 보정 경로), ½ 근처.
        var acos = new List<double>
        {
            0.5, -0.5, 0.0, 1.0, -1.0, 1e-9, -1e-9, 0.9999999999, -0.9999999999, 1 - Math.ScaleB(1, -53),
            -1 + Math.ScaleB(1, -53), 0.7071067811865476, -0.7071067811865476, 0.5000000000000001, -0.5000000000000001
        };
        for (int i = 0; i < 150; ++i)
        {
            acos.Add(random.Uniform(-1, 1));
        }

        for (int i = 0; i < 30; ++i)
        {
            acos.Add(1 - random.Uniform(0, 1e-6));
            acos.Add(-1 + random.Uniform(0, 1e-6));
        }

        for (int i = 0; i < 20; ++i)
        {
            acos.Add(random.Uniform(0.49, 0.51));
            acos.Add(random.Uniform(-0.51, -0.49));
        }

        Emit(text, "AcosReference", acos.Select(x => (x, 0.0, Acos(x))));

        // Cos(2026-11-27 — 커브 쿠킹 평가): 0·n·π/4와 n·π/2 근처(축소 경계·0에 가까운 결과), 작은
        // |x|, 고른 표본, 정확한 축소 범위(|x| ≤ 2^20·π/2)의 큰 |x|.
        var cos = new List<double>
        {
            0.0, 1e-9, -1e-9, 0.7853981633974483, -0.7853981633974483, 1.5707963267948966, -1.5707963267948966,
            3.141592653589793, 4.71238898038469, 6.283185307179586, 2.356194490192345, 1647099.3291652855,
            1647099.329165285 * 0.5
        };
        for (int n = 1; n <= 16; ++n)
        {
            cos.Add(n * 0.7853981633974483);
        }

        for (int i = 0; i < 120; ++i)
        {
            cos.Add(random.Uniform(-10, 10));
        }

        for (int i = 0; i < 40; ++i)
        {
            cos.Add(random.Uniform(-1647099.3291652855, 1647099.3291652855));
        }

        Emit(text, "CosReference", cos.Select(x => (x, 0.0, Cos(x))));

        // Atan: ±1, k/8 경계와 그 이웃, 1/x 분기 경계(1 바로 위), 작은·큰 |x|, 고른 표본, 넓은 지수.
        var atan = new List<double>
        {
            1.0, -1.0, 1e-9, -1e-9, 1e300, -1e300, 1 + Math.ScaleB(1, -52), 1 - Math.ScaleB(1, -53), 0.0625, 0.0625000000000001,
            0.1875, 0.1874999999999999, 7.99999999, 8.00000001
        };
        for (int k = 1; k <= 8; ++k)
        {
            atan.Add(k / 8.0);
            atan.Add(-k / 8.0);
            atan.Add(8.0 / k);
        }

        for (int i = 0; i < 100; ++i)
        {
            atan.Add(random.Uniform(-1, 1));
        }

        for (int i = 0; i < 40; ++i)
        {
            atan.Add(random.Uniform(-100, 100));
        }

        for (int i = 0; i < 40; ++i)
        {
            atan.Add(random.WideDouble(-25, 60));
        }

        Emit(text, "AtanReference", atan.Select(x => (x, 0.0, Atan(x))));

        text.Append("// clang-format on\n\n} // namespace rocks::deterministic::reference\n");
        return text.ToString();
    }

    private static void Emit(StringBuilder text, string name, IEnumerable<(double Input, double Second, double Expected)> cases)
    {
        text.Append($"inline constexpr ReferenceCase {name}[] = {{\n");
        foreach (var (input, second, expected) in cases)
        {
            text.Append($"    {{0x{Bits(input):X16}u, 0x{Bits(second):X16}u, 0x{Bits(expected):X16}u}},\n");
        }

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
        if (biased == 0)
        {
            return (m, -1074);
        }

        return (m | (BigInteger.One << 52), biased - 1075);
    }

    // log2(x)의 고정소수점(x > 0 유한).
    private static BigInteger Log2Fixed(BigInteger numerator, BigInteger denominator)
    {
        // x = numerator/denominator = 2^k·m, m ∈ [1, 2).
        int k = (int)(numerator.GetBitLength() - denominator.GetBitLength());
        BigInteger n = numerator, d = denominator;
        if (k >= 0)
        {
            d <<= k;
        }
        else
        {
            n <<= -k;
        }

        if (n < d)
        {
            n <<= 1;
            --k;
        }
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
        if (t.Sign < 0 && n * One != t)
        {
            n -= 1; // floor
        }

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
        if (fixedMantissa.Sign == 0)
        {
            return negative ? -0.0 : 0.0;
        }
        // 값 = fixedMantissa·2^(exponent − Precision).
        long exp2 = (long)exponent - Precision;
        long length = (long)fixedMantissa.GetBitLength();
        long top = exp2 + length - 1;
        if (top > 1023)
        {
            return negative ? double.NegativeInfinity : double.PositiveInfinity;
        }

        long shift = top >= -1022 ? length - 53 : -1074 - exp2;
        BigInteger m;
        if (shift <= 0)
        {
            m = fixedMantissa << (int)-shift;
        }
        else
        {
            if (shift > length + 1)
            {
                return negative ? -0.0 : 0.0;
            }

            m = fixedMantissa >> (int)shift;
            BigInteger remainder = fixedMantissa - (m << (int)shift);
            BigInteger half = BigInteger.One << (int)(shift - 1);
            if (remainder > half || (remainder == half && !m.IsEven))
            {
                m += 1;
            }
        }

        long finalExp = exp2 + shift; // 값 = m·2^finalExp
        if (m.GetBitLength() > 53)
        {
            m >>= 1;
            ++finalExp;
        }
        // m ≤ 2^53이고 결과가 표현 가능하면 ScaleB는 정확하다(넘치면 무한대).
        double result = Math.ScaleB((double)(ulong)m, (int)finalExp);
        return negative ? -result : result;
    }

    private static double Exp2(double x)
    {
        var (m, e) = Exact(Math.Abs(x));
        BigInteger t = e >= 0 ? (m << e) * One : (m * One) >> -e;
        if (x < 0)
        {
            t = -t;
        }

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
        if (y < 0)
        {
            t = -t;
        }

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
        if (e >= 0)
        {
            r = (m << e) % 2 * One;
        }
        else
        {
            // 고정소수점이 입력 비트를 모두 담아야 한다(표본은 |x| ≥ 2^−60 정도).
            if (-e > Precision - 100)
            {
                throw new InvalidOperationException("SinPi reference input is too small");
            }

            BigInteger period = BigInteger.One << (1 - e); // 2·2^−e
            r = (m % period) * One >> -e;                // [0, 2)·One
        }

        if (x < 0)
        {
            r = (2 * One - r) % (2 * One);
        }

        if (cosine)
        {
            r = (r + One / 2) % (2 * One); // cos(πx) = sin(π(x + ½))
        }
        // sin(πr), r ∈ [0, 2).
        bool negative = false;
        if (r >= One)
        {
            r -= One;
            negative = true;
        }   // sin(π(r + 1)) = −sin(πr)

        if (r > One / 2)
        {
            r = One - r;                   // sin(π(1 − r)) = sin(πr)
        }

        BigInteger angle = r * PiFixed >> Precision;
        BigInteger sum = 0, term = angle, square = angle * angle >> Precision;
        for (int k = 1; term != 0; k += 2)
        {
            sum += term;
            term = -(term * square >> Precision) / ((k + 1) * (k + 2));
        }

        if (sum.Sign < 0)
        {
            sum = -sum;
            negative = !negative;
        }

        return Round(sum, 0, negative && sum.Sign != 0);
    }

    // arccos(x)(|x| ≤ 1): x ≥ 0이면 2·asin(√((1 − x)/2)), x < 0이면 π − acos(−x). √는 정수 제곱근,
    // asin은 고정소수점 sin·cos로 Newton 반복한다(인자 ≤ π/4라 급수가 빨리 수렴한다).
    private static double Acos(double x)
    {
        var (m, e) = Exact(Math.Abs(x));
        if (!m.IsZero && -e > Precision - 100)
        {
            throw new InvalidOperationException("Acos reference input is too small");
        }

        BigInteger magnitude = m.IsZero ? 0 : e >= 0 ? (m << e) * One : (m * One) >> -e;
        BigInteger half = (One - magnitude) / 2;
        BigInteger s = IntegerSqrt(half * One);
        BigInteger angle = 2 * AsinFixed(s);
        if (x < 0)
        {
            angle = PiFixed - angle;
        }

        return Round(angle, 0);
    }

    // cos(x): x를 정확한 고정소수점으로 두고 2π로 나눈 나머지를 [−π, π]로 옮긴 뒤 Taylor 급수.
    private static double Cos(double x)
    {
        var (m, e) = Exact(Math.Abs(x));
        if (!m.IsZero && -e > Precision - 100)
        {
            throw new InvalidOperationException("Cos reference input is too small");
        }

        BigInteger magnitude = m.IsZero ? 0 : e >= 0 ? (m << e) * One : (m * One) >> -e;
        BigInteger twoPi = 2 * PiFixed;
        BigInteger r = magnitude % twoPi;
        if (r > PiFixed)
        {
            r -= twoPi;
        }

        var (_, cosine) = SinCosFixed(r);
        bool negative = cosine.Sign < 0;
        return Round(BigInteger.Abs(cosine), 0, negative);
    }

    // atan(x): |x| > 1이면 π/2 − atan(1/|x|). [0, 1]은 반각 공식 atan(t) = 2·atan(t/(1 + √(1 + t²)))을
    // 세 번 쓴 뒤(|t| ≤ tan(π/32)) 급수.
    private static double Atan(double x)
    {
        var (m, e) = Exact(Math.Abs(x));
        if (m.IsZero)
        {
            return x;
        }

        if (-e > Precision - 100)
        {
            throw new InvalidOperationException("Atan reference input is too small");
        }

        BigInteger magnitude = e >= 0 ? (m << e) * One : (m * One) >> -e;
        bool inverted = magnitude > One;
        BigInteger t = inverted ? One * One / magnitude : magnitude;
        for (int halving = 0; halving < 3; ++halving)
        {
            BigInteger root = IntegerSqrt(One * One + t * t);
            t = t * One / (One + root);
        }

        BigInteger square = t * t >> Precision;
        BigInteger sum = 0, power = t;
        for (int k = 0; power != 0; ++k)
        {
            sum += (k % 2 == 0 ? power : -power) / (2 * k + 1);
            power = power * square >> Precision;
        }

        BigInteger angle = 8 * sum;
        if (inverted)
        {
            angle = PiFixed / 2 - angle;
        }

        return Round(angle, 0, x < 0);
    }

    // asin(s)(s 고정소수점 ∈ [0, √½]): sin(φ) = s를 Newton으로 푼다. 시작값은 double asin(53비트)이고
    // 반복마다 맞는 비트가 두 배가 되므로 4회면 400비트를 넘는다(여유로 6회).
    private static BigInteger AsinFixed(BigInteger s)
    {
        if (s.Sign == 0)
        {
            return 0;
        }

        var (m, e) = Exact(Math.Asin((double)s / (double)One));
        BigInteger phi = e >= 0 ? (m << e) * One : (m * One) >> -e;
        for (int iteration = 0; iteration < 6; ++iteration)
        {
            var (sine, cosine) = SinCosFixed(phi);
            phi -= (sine - s) * One / cosine;
        }

        return phi;
    }

    // sin(a)·cos(a)(a 고정소수점, |a| ≤ 1)의 Taylor 급수.
    private static (BigInteger Sine, BigInteger Cosine) SinCosFixed(BigInteger a)
    {
        BigInteger square = a * a >> Precision;
        BigInteger sine = 0, term = a;
        for (int k = 1; term != 0; k += 2)
        {
            sine += term;
            term = -(term * square >> Precision) / ((k + 1) * (k + 2));
        }

        BigInteger cosine = 0;
        term = One;
        for (int k = 0; term != 0; k += 2)
        {
            cosine += term;
            term = -(term * square >> Precision) / ((k + 1) * (k + 2));
        }

        return (sine, cosine);
    }

    // ⌊√n⌋(n ≥ 0) — Newton 정수 반복.
    private static BigInteger IntegerSqrt(BigInteger n)
    {
        if (n.Sign == 0)
        {
            return 0;
        }

        BigInteger x = BigInteger.One << (int)((n.GetBitLength() + 1) / 2);
        while (true)
        {
            BigInteger next = (x + n / x) >> 1;
            if (next >= x)
            {
                return x;
            }

            x = next;
        }
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
