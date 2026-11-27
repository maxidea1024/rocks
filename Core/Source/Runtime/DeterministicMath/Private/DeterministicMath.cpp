#include "SrgbTables.generated.h"

#include <DeterministicMath/DeterministicMath.h>
#include <Platform/Memory.h>

namespace rocks::deterministic {
namespace {

using platform::BitCast;

constexpr uint64 SignBit = 0x8000000000000000ull;
constexpr uint64 CanonicalNaNBits = 0x7FF8000000000000ull;
constexpr double PositiveInfinity = BitCast<double>(uint64{0x7FF0000000000000});
constexpr double TwoTo52 = 4503599627370496.0;
constexpr double Pi = BitCast<double>(uint64{0x400921FB54442D18});    // 3.141592653589793
constexpr double Sqrt2 = BitCast<double>(uint64{0x3FF6A09E667F3BCD}); // 1.4142135623730951

// 오차 없는 변환으로 다루는 두 double의 합 hi + lo(|lo| ≤ ulp(hi)/2). 상수는 고정밀 값을 hi(가장
// 가까운 double)와 lo(나머지의 가장 가까운 double)로 나눈 비트 패턴이다.
struct DoubleDouble {
    double hi;
    double lo;
};

constexpr DoubleDouble Ln2{BitCast<double>(uint64{0x3FE62E42FEFA39EF}),
                           BitCast<double>(uint64{0x3C7ABC9E3B39803F})};
constexpr DoubleDouble InvLn2{BitCast<double>(uint64{0x3FF71547652B82FE}),
                              BitCast<double>(uint64{0x3C7777D0FFDA0D24})};
constexpr DoubleDouble Srgb055{BitCast<double>(uint64{0x3FAC28F5C28F5C29}),
                               BitCast<double>(uint64{0xBC147AE147AE147B})};
constexpr DoubleDouble Srgb1055{BitCast<double>(uint64{0x3FF0E147AE147AE1}),
                                BitCast<double>(uint64{0x3C91EB851EB851EC})};

// 1/n!(n = 0..23). 컴파일 시점 IEEE 나눗셈 연쇄라 모든 대상에서 같은 상수다.
struct InverseFactorials {
    double value[24];
};

constexpr InverseFactorials MakeInverseFactorials() noexcept {
    InverseFactorials table{};
    table.value[0] = 1.0;
    for (int n = 1; n < 24; ++n) {
        table.value[n] = table.value[n - 1] / static_cast<double>(n);
    }

    return table;
}

constexpr InverseFactorials Factorial = MakeInverseFactorials();

double NaN() noexcept {
    return BitCast<double>(CanonicalNaNBits);
}

bool IsNaN(double x) noexcept {
    return x != x;
}

bool IsInfinite(double x) noexcept {
    return (BitCast<uint64>(x) & ~SignBit) == 0x7FF0000000000000ull;
}

double Canonical(double x) noexcept {
    return IsNaN(x) ? NaN() : x;
}

double Magnitude(double x) noexcept {
    return BitCast<double>(BitCast<uint64>(x) & ~SignBit);
}

// ---- 오차 없는 변환(Dekker·Knuth). 기본 반올림 모드와 -ffp-contract=off를 전제한다. ----

// |a| ≥ |b|(또는 a = 0)일 때 a + b = s + e를 정확히.
DoubleDouble FastTwoSum(double a, double b) noexcept {
    const double sum = a + b;
    return {sum, b - (sum - a)};
}

// 크기 제한 없이 a + b = s + e를 정확히.
DoubleDouble TwoSum(double a, double b) noexcept {
    const double sum = a + b;
    const double virtualB = sum - a;
    return {sum, (a - (sum - virtualB)) + (b - virtualB)};
}

// Veltkamp 분할: a = high + low, 두 부분이 26비트 이하. |a| < 2^995에서 넘치지 않는다.
void Split(double a, double& high, double& low) noexcept {
    const double scaled = 134217729.0 * a; // 2^27 + 1
    high = scaled - (scaled - a);
    low = a - high;
}

// a·b = p + e를 정확히(FMA 없이). 결과가 넘치거나 비정규 범위가 아니어야 한다.
DoubleDouble TwoProduct(double a, double b) noexcept {
    const double product = a * b;
    double aHigh = 0.0;
    double aLow = 0.0;
    double bHigh = 0.0;
    double bLow = 0.0;
    Split(a, aHigh, aLow);
    Split(b, bHigh, bLow);
    const double error = ((aHigh * bHigh - product) + aHigh * bLow + aLow * bHigh) + aLow * bLow;
    return {product, error};
}

DoubleDouble Add(DoubleDouble a, DoubleDouble b) noexcept {
    DoubleDouble sum = TwoSum(a.hi, b.hi);
    sum.lo += a.lo + b.lo;
    return FastTwoSum(sum.hi, sum.lo);
}

DoubleDouble Multiply(DoubleDouble a, DoubleDouble b) noexcept {
    DoubleDouble product = TwoProduct(a.hi, b.hi);
    product.lo += a.hi * b.lo + a.lo * b.hi;
    return FastTwoSum(product.hi, product.lo);
}

DoubleDouble Divide(DoubleDouble a, DoubleDouble b) noexcept {
    const double quotient = a.hi / b.hi;
    const DoubleDouble back = TwoProduct(quotient, b.hi);
    // a − quotient·b(앞 두 항은 크기가 거의 같아 뺄셈이 정확하다).
    const double remainder = (((a.hi - back.hi) - back.lo) + a.lo) - quotient * b.lo;
    return FastTwoSum(quotient, remainder / b.hi);
}

// 2^n(n ∈ [−1022, 1023])는 지수 필드만으로 정확히 만든다.
double PowerOfTwo(int n) noexcept {
    return BitCast<double>(static_cast<uint64>(n + 1023) << 52);
}

// value · 2^n. 결과가 정규 수면 정확하고, 비정규 수면 마지막 곱셈 한 번만 반올림한다(value ≥
// 2^−2이고 n ≥ −1076이면 첫 곱셈은 정규 수다).
double Scale(double value, int n) noexcept {
    if (n > 1023) {
        return (value * PowerOfTwo(1023)) * PowerOfTwo(n - 1023);
    }

    if (n >= -1022) {
        return value * PowerOfTwo(n);
    }

    return (value * PowerOfTwo(n + 54)) * PowerOfTwo(-54);
}

// log2(x)를 double-double로(x는 0이 아닌 양의 유한 값). 상대 오차가 약 2^−66 이하다.
DoubleDouble Log2Extended(double x) noexcept {
    uint64 bits = BitCast<uint64>(x);
    int exponent = static_cast<int>(bits >> 52) - 1023;
    if (exponent == -1023) {
        // 비정규 수: 2^54를 곱해 정규화한다(정확).
        bits = BitCast<uint64>(x * PowerOfTwo(54));
        exponent = static_cast<int>(bits >> 52) - 1023 - 54;
    }

    double mantissa = BitCast<double>((bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull);
    if (mantissa > Sqrt2) {
        mantissa *= 0.5; // (√½, √2]로 옮긴다(정확)
        ++exponent;
    }

    // ln(m) = 2·atanh(s), s = f/(2 + f), f = m − 1(Sterbenz: 정확). s는 double-double로 나눈다.
    const double f = mantissa - 1.0;
    DoubleDouble logMantissa{0.0, 0.0};
    if (f != 0.0) {
        const DoubleDouble s = Divide({f, 0.0}, TwoSum(2.0, f));
        // 2·atanh(s) = 2s + (2/3)s³ + 2s·z²·(1/5 + z/7 + …), z = s²(|s| ≤ 0.1716, z ≤ 0.0295). 둘째
        // 항까지는 double-double로, 나머지(2s의 6e-5배 이하)는 double로 계산한다. 그래서 상대
        // 오차가 약 2^−66이고, Pow의 지수 y·log2 x가 1100까지 커져도 결과 오차가 0.1 ulp 아래다.
        const DoubleDouble z = Multiply(s, s);
        const DoubleDouble cube = Multiply(s, z);
        const DoubleDouble twoThirds = Divide({2.0, 0.0}, {3.0, 0.0});
        double tail = 1.0 / 29.0;
        for (int denominator = 27; denominator >= 5; denominator -= 2) {
            tail = tail * z.hi + 1.0 / static_cast<double>(denominator);
        }

        const double rest = 2.0 * s.hi * z.hi * z.hi * tail;
        logMantissa = Add({2.0 * s.hi, 2.0 * s.lo}, Multiply(cube, twoThirds));
        logMantissa = Add(logMantissa, {rest, 0.0});
    }

    // log2(x) = exponent + ln(m)/ln2.
    const DoubleDouble scaled = Multiply(logMantissa, InvLn2);
    return Add({static_cast<double>(exponent), 0.0}, scaled);
}

// 2^t(t = hi + lo, |t.hi| ≤ 1100)를 정규 범위 hi + lo와 지수 n으로: 2^t = (result.hi +
// result.lo)·2^n.
struct ScaledResult {
    DoubleDouble value;
    int exponent;
};

ScaledResult Exp2Extended(DoubleDouble t) noexcept {
    const double n = RoundHalfEven(t.hi);
    // r = t − n(앞 뺄셈은 Sterbenz로 정확), |r| ≤ ½ + ε. 2^r = e^a, a = r·ln2(double-double).
    const DoubleDouble r = FastTwoSum(t.hi - n, t.lo);
    DoubleDouble a = TwoProduct(r.hi, Ln2.hi);
    a.lo += r.hi * Ln2.lo + r.lo * Ln2.hi;
    a = FastTwoSum(a.hi, a.lo);

    // e^a = 1 + a.hi + a.hi²·Q(a.hi) + a.lo·(1 + a.hi), Q = Σ_{n≥2} a^(n−2)/n!(18차까지, |a| ≤
    // 0.35에서 나머지 < 1e-22). 큰 항 1 + a.hi는 정확히 더하고 작은 항을 모아 한 번 반올림한다.
    double q = Factorial.value[18];
    for (int index = 17; index >= 2; --index) {
        q = q * a.hi + Factorial.value[index];
    }

    const DoubleDouble leading = FastTwoSum(1.0, a.hi);
    const double small = a.hi * a.hi * q + a.lo + a.lo * a.hi;
    return {FastTwoSum(leading.hi, leading.lo + small), static_cast<int>(n)};
}

double FinishExp2(const ScaledResult& result) noexcept {
    // value < √2·(1 + ε)이므로 지수가 −1076보다 작으면 최소 비정규 수의 절반(2^−1075)보다 작다.
    if (result.exponent < -1076) {
        return 0.0;
    }

    return Canonical(Scale(result.value.hi + result.value.lo, result.exponent));
}

// 양의 유한 값 base(1이 아닌)의 base^exponent를 double-double 지수 t = exponent·log2(base)로
// 계산한다. |t|가 1100을 넘으면 무한대·0이 확실하다.
double PowFromLog(DoubleDouble logBase, DoubleDouble exponent) noexcept {
    const double estimate = exponent.hi * logBase.hi;
    if (IsNaN(estimate)) {
        return NaN();
    }

    if (estimate > 1100.0) {
        return PositiveInfinity;
    }

    if (estimate < -1100.0) {
        return 0.0;
    }

    return FinishExp2(Exp2Extended(Multiply(logBase, exponent)));
}

// sin(t)·cos(t)(|t| ≤ π/4): 21차·22차 Taylor(나머지 < 1e-24).
double SinKernel(double t) noexcept {
    const double t2 = t * t;
    double sum = Factorial.value[21];
    for (int n = 19; n >= 1; n -= 2) {
        sum = -sum * t2 + Factorial.value[n];
    }

    return sum * t;
}

double CosKernel(double t) noexcept {
    const double t2 = t * t;
    double sum = Factorial.value[22];
    for (int n = 20; n >= 0; n -= 2) {
        sum = -sum * t2 + Factorial.value[n];
    }

    return sum;
}

// asin Taylor 계수 c_k = (2k)!/(4^k·(k!)²·(2k+1))(k = 1..AsinTerms)다. 앞 인수 b_k = b_{k−1}·(2k −
// 1)/(2k)를 컴파일 시점 IEEE 연산으로 이어 만들므로 모든 대상에서 같은 상수다.
constexpr int AsinTerms = 30;

struct AsinCoefficients {
    double value[AsinTerms + 1];
};

constexpr AsinCoefficients MakeAsinCoefficients() noexcept {
    AsinCoefficients table{};
    double binomial = 1.0;
    for (int k = 1; k <= AsinTerms; ++k) {
        binomial = binomial * static_cast<double>(2 * k - 1) / static_cast<double>(2 * k);
        table.value[k] = binomial / static_cast<double>(2 * k + 1);
    }

    return table;
}

constexpr AsinCoefficients AsinSeries = MakeAsinCoefficients();

// π의 double-double(hi = 가장 가까운 double, lo = 나머지의 가장 가까운 double)이다.
constexpr DoubleDouble PiExtended{Pi, BitCast<double>(uint64{0x3CA1A62633145C07})};

// asin(s) − s = s·y·P(y), y = s²(|s| ≤ ½). 30항이면 s = ½에서 나머지가 s의 2^−60배 아래다.
double AsinTail(double s) noexcept {
    const double y = s * s;
    double sum = AsinSeries.value[AsinTerms];
    for (int k = AsinTerms - 1; k >= 1; --k) {
        sum = sum * y + AsinSeries.value[k];
    }

    return s * y * sum;
}

// 1 미만 양의 정규 수 z의 제곱근 근사다(상대 오차 1 ulp 남짓). z = m·2^(2h), m ∈ [1, 4)로 나눠
// m에서 Newton 반복을 고정 횟수만 한다(시작 오차 25% 이하 → 6회면 수렴). 기본 연산만 쓰므로
// 결정적이다. 호출자(Acos)는 남은 오차를 z − s²로 따로 보정한다.
constexpr int SqrtNewtonIterations = 6;

double SqrtApproximation(double z) noexcept {
    const int exponent = static_cast<int>(BitCast<uint64>(z) >> 52) - 1023;
    // z < 1이라 지수가 음수다: h = ⌊지수/2⌋.
    const int half = -((1 - exponent) / 2);
    const double m = z * PowerOfTwo(-2 * half);
    double y = 0.5 * (1.0 + m);
    for (int iteration = 0; iteration < SqrtNewtonIterations; ++iteration) {
        y = 0.5 * (y + m / y);
    }

    return y * PowerOfTwo(half);
}

// π/2의 3단 Cody-Waite 조각이다(fdlibm __ieee754_rem_pio2 — 앞 세 조각은 33비트라 n < 2^20이면
// n·조각이 오차 없다. 네 조각의 합과 π/2의 차는 1e−48 아래).
constexpr double InversePiOverTwo = BitCast<double>(uint64{0x3FE45F306DC9C883});
constexpr double PiOverTwoPart1 = BitCast<double>(uint64{0x3FF921FB54400000});
constexpr double PiOverTwoPart2 = BitCast<double>(uint64{0x3DD0B4611A600000});
constexpr double PiOverTwoPart3 = BitCast<double>(uint64{0x3BA3198A2E000000});
constexpr double PiOverTwoPart3Tail = BitCast<double>(uint64{0x397B839A252049C1});

// 축소가 정확한 4분면 번호의 상한이다(n < 2^20 — n·조각이 오차 없다). 넘으면 Cos는 CosPi(|x|/π)로
// 결정적인 [−1, 1] 값을 낸다(정확도는 보장하지 않는다).
constexpr double CosExactQuadrantLimit = 1048576.0; // 2^20

// atan(k/8)(k = 0..8)의 double-double이다(hi = 가장 가까운 double, lo = 나머지의 가장 가까운 double
// — 고정밀 계산의 비트 패턴, 기준값 표 시험이 k/8 입력으로 확인한다). atan(1) = π/4.
constexpr DoubleDouble AtanEighths[9] = {
    {0.0, 0.0},
    {BitCast<double>(uint64{0x3FBFD5BA9AAC2F6E}), BitCast<double>(uint64{0xBC4CD37686760C17})},
    {BitCast<double>(uint64{0x3FCF5B75F92C80DD}), BitCast<double>(uint64{0x3C68AB6E3CF7AFBD})},
    {BitCast<double>(uint64{0x3FD6F61941E4DEF1}), BitCast<double>(uint64{0xBC7C63AAE6F6E918})},
    {BitCast<double>(uint64{0x3FDDAC670561BB4F}), BitCast<double>(uint64{0x3C7A2B7F222F65E2})},
    {BitCast<double>(uint64{0x3FE1E00BABDEFEB4}), BitCast<double>(uint64{0xBC5928DF287A668F})},
    {BitCast<double>(uint64{0x3FE4978FA3269EE1}), BitCast<double>(uint64{0x3C72419A87F2A458})},
    {BitCast<double>(uint64{0x3FE700A7C5784634}), BitCast<double>(uint64{0xBC78C34D25AADEF6})},
    {BitCast<double>(uint64{0x3FE921FB54442D18}), BitCast<double>(uint64{0x3C81A62633145C07})}};

// atan(r) − r의 홀수 Taylor 꼬리 −r³/3 + r⁵/5 − …(r^19까지)다. |r| ≤ 1/16 남짓이면 나머지가 r의
// 2^−80배 아래다.
constexpr int AtanTailTerms = 9;

double AtanTail(double r) noexcept {
    const double y = r * r;
    double sum = 0.0;
    for (int k = AtanTailTerms; k >= 1; --k) {
        const double coefficient = 1.0 / static_cast<double>(2 * k + 1);
        sum = sum * y + ((k & 1) != 0 ? -coefficient : coefficient);
    }

    return r * y * sum;
}

// 너무 작아 atan(x)가 x로 반올림되는 크기의 경계다(|x| < 2^−27이면 x³/3 < ulp(x)/2).
constexpr double AtanIdentityLimit = BitCast<double>(uint64{0x3E40000000000000}); // 2^−27

// sin(π(n/2 + r))를 4분면으로 나눠 계산한다(|r| ≤ ¼, quadrant = n mod 4).
double SinPiQuadrant(double r, int64 quadrant) noexcept {
    const double t = r * Pi;
    switch (quadrant & 3) {
    case 0:
        return SinKernel(t);
    case 1:
        return CosKernel(t);
    case 2:
        return -SinKernel(t);
    default:
        return -CosKernel(t);
    }
}

} // namespace

double Trunc(double x) noexcept {
    const uint64 bits = BitCast<uint64>(x);
    const int exponent = static_cast<int>((bits >> 52) & 0x7FF) - 1023;
    if (exponent == 1024) {
        return Canonical(x); // ±Inf는 그대로, NaN은 고정 NaN
    }

    if (exponent < 0) {
        return BitCast<double>(bits & SignBit);
    }

    if (exponent >= 52) {
        return x;
    }

    const uint64 fraction = (uint64{1} << (52 - exponent)) - 1;
    return BitCast<double>(bits & ~fraction);
}

double Floor(double x) noexcept {
    const double truncated = Trunc(x);
    if (IsNaN(truncated)) {
        return truncated;
    }

    return x < truncated ? truncated - 1.0 : truncated;
}

double RoundHalfEven(double x) noexcept {
    if (IsNaN(x)) {
        return NaN();
    }

    const uint64 bits = BitCast<uint64>(x);
    const double magnitude = BitCast<double>(bits & ~SignBit);
    if (!(magnitude < TwoTo52)) {
        return x; // 이미 정수(또는 Inf)
    }

    // 2^52를 더했다 빼면 nearest-even 반올림이 정확히 일어난다(기본 반올림 모드 전제).
    const double rounded = (magnitude + TwoTo52) - TwoTo52;
    return BitCast<double>(BitCast<uint64>(rounded) | (bits & SignBit));
}

double Exp2(double x) noexcept {
    if (IsNaN(x)) {
        return NaN();
    }

    if (x >= 1024.0) {
        return PositiveInfinity;
    }

    if (x < -1076.0) {
        return 0.0;
    }

    return FinishExp2(Exp2Extended({x, 0.0}));
}

double Log2(double x) noexcept {
    if (IsNaN(x) || x < 0.0) {
        return NaN();
    }

    if (x == 0.0) {
        return -PositiveInfinity;
    }

    if (IsInfinite(x)) {
        return PositiveInfinity;
    }

    const DoubleDouble result = Log2Extended(x);
    return result.hi + result.lo;
}

double Pow(double base, double exponent) noexcept {
    // C·IEEE pow처럼 지수 0과 밑 1은 다른 입력이 NaN이어도 1이다.
    if (exponent == 0.0 || base == 1.0) {
        return 1.0;
    }

    if (IsNaN(base) || IsNaN(exponent) || base < 0.0) {
        return NaN(); // 음수 밑(−Inf 포함)은 다루지 않는다
    }

    if (base == 0.0) {
        return exponent > 0.0 ? 0.0 : PositiveInfinity; // ±0 밑의 부호는 보지 않는다
    }

    if (IsInfinite(base)) {
        return exponent > 0.0 ? PositiveInfinity : 0.0;
    }

    if (IsInfinite(exponent)) {
        return (base > 1.0) == (exponent > 0.0) ? PositiveInfinity : 0.0;
    }

    return PowFromLog(Log2Extended(base), {exponent, 0.0});
}

double SinPi(double x) noexcept {
    if (IsNaN(x) || IsInfinite(x)) {
        return NaN();
    }

    if (!(Magnitude(x) < TwoTo52)) {
        return BitCast<double>(BitCast<uint64>(x) & SignBit); // 정수: sin(±πn) = ±0
    }

    const double n = RoundHalfEven(2.0 * x);
    const double r = x - 0.5 * n; // 오차 없음, |r| ≤ ¼
    if (r == 0.0 && (static_cast<int64>(n) & 1) == 0) {
        return BitCast<double>(BitCast<uint64>(x) & SignBit); // IEEE 754-2019: sinPi(±n) = ±0
    }

    return SinPiQuadrant(r, static_cast<int64>(n));
}

double CosPi(double x) noexcept {
    if (IsNaN(x) || IsInfinite(x)) {
        return NaN();
    }

    const double magnitude = Magnitude(x);
    if (!(magnitude < TwoTo52)) {
        // 정수 n: cos(πn) = (−1)^n. 2^53 이상은 모두 짝수다.
        return magnitude < 2.0 * TwoTo52 && (BitCast<uint64>(magnitude) & 1) != 0 ? -1.0 : 1.0;
    }

    // cos(πx) = sin(π(|x| + ½)). |x| + ½를 직접 더하지 않고 4분면 번호를 하나 옮긴다.
    const double n = RoundHalfEven(2.0 * magnitude);
    const double r = magnitude - 0.5 * n;
    if (r == 0.0 && (static_cast<int64>(n) & 1) != 0) {
        return 0.0; // IEEE 754-2019: cosPi(n + ½) = +0
    }

    return SinPiQuadrant(r, static_cast<int64>(n) + 1);
}

double Sinc(double x) noexcept {
    if (x == 0.0) {
        return 1.0;
    }

    if (IsNaN(x) || IsInfinite(x)) {
        return NaN();
    }

    return SinPi(x) / (Pi * x);
}

double Lanczos(double x, int lobes) noexcept {
    if (IsNaN(x) || lobes < 1) {
        return NaN();
    }

    const double magnitude = Magnitude(x);
    const double support = static_cast<double>(lobes);
    if (magnitude >= support) {
        return 0.0;
    }

    if (magnitude == 0.0) {
        return 1.0;
    }

    // sinc(|x|/L)의 분자 sin(π|x|/L)는 지지 경계 근처에서 0에 가까워 |x|/L의 반올림 오차가 크게
    // 증폭된다. |x| ≥ L/2이면 sin(π|x|/L) = sin(π(L − |x|)/L)를 쓴다(L − |x|는 Sterbenz로 정확).
    const double ratio = magnitude / support;
    const double windowNumerator =
        magnitude >= 0.5 * support ? SinPi((support - magnitude) / support) : SinPi(ratio);
    const double window = windowNumerator / (Pi * ratio);
    return Sinc(magnitude) * window;
}

double Acos(double x) noexcept {
    if (IsNaN(x) || Magnitude(x) > 1.0) {
        return NaN();
    }

    const double magnitude = Magnitude(x);
    if (magnitude <= 0.5) {
        // acos(x) = π/2 − x − (asin(x) − x). 앞 두 항을 오차 없이 더하고 꼬리를 모아 한 번
        // 반올림한다.
        const DoubleDouble head = TwoSum(0.5 * PiExtended.hi, -x);
        return Canonical(head.hi + (head.lo + (0.5 * PiExtended.lo - AsinTail(x))));
    }

    if (magnitude == 1.0) {
        return x > 0.0 ? 0.0 : PiExtended.hi;
    }

    // |x| > ½: acos(|x|) = 2·asin(√z), z = (1 − |x|)/2(Sterbenz로 정확, 2^−54 이상의 정규 수).
    // s = √z 근사의 오차는 c = (z − s²)/(2s)로 보정한다(z − s²는 오차 없는 곱으로 정확). asin(s +
    // c) ≈ asin(s) + c·(1 + s²/2)이고 c가 ulp 수준이라 그 이후 항은 무시된다.
    const double z = 0.5 * (1.0 - magnitude);
    const double s = SqrtApproximation(z);
    const DoubleDouble square = TwoProduct(s, s);
    const double correction = ((z - square.hi) - square.lo) / (2.0 * s) * (1.0 + 0.5 * z);
    const double tail = AsinTail(s) + correction;
    if (x > 0.0) {
        const DoubleDouble sum = FastTwoSum(2.0 * s, 2.0 * tail);
        return Canonical(sum.hi + sum.lo);
    }

    // acos(x) = π − 2·asin(√z)(x < −½).
    const DoubleDouble head = TwoSum(PiExtended.hi, -2.0 * s);
    return Canonical(head.hi + (head.lo + (PiExtended.lo - 2.0 * tail)));
}

double Cos(double x) noexcept {
    if (IsNaN(x) || IsInfinite(x)) {
        return NaN();
    }

    // cos는 짝함수다. n = round(|x|·2/π), r = |x| − n·π/2를 double-double로 줄인다(n·조각 셋은
    // 오차 없고, 첫 뺄셈은 Sterbenz로 정확하다 — n ≥ 1이면 |x|와 n·조각1이 두 배 안이다).
    const double magnitude = Magnitude(x);
    const double n = RoundHalfEven(magnitude * InversePiOverTwo);
    if (n >= CosExactQuadrantLimit) {
        return CosPi(magnitude / Pi);
    }

    const double head = magnitude - n * PiOverTwoPart1;
    DoubleDouble r = TwoSum(head, -n * PiOverTwoPart2);
    r = Add(r, {-n * PiOverTwoPart3, 0.0});
    r = Add(r, {-n * PiOverTwoPart3Tail, 0.0});
    // n mod 4를 정확히(n은 정수 double — 곱·내림·뺄셈이 모두 정확하다).
    const int quadrant = static_cast<int>(n - 4.0 * Floor(0.25 * n));
    // cos(r + e) ≈ cos r − e·sin r, sin(r + e) ≈ sin r + e·cos r(e = r.lo는 ulp 수준).
    double result;
    switch (quadrant) {
    case 0:
        result = CosKernel(r.hi) - r.lo * SinKernel(r.hi);
        break;
    case 1:
        result = -(SinKernel(r.hi) + r.lo * CosKernel(r.hi));
        break;
    case 2:
        result = -(CosKernel(r.hi) - r.lo * SinKernel(r.hi));
        break;
    default:
        result = SinKernel(r.hi) + r.lo * CosKernel(r.hi);
        break;
    }

    return Canonical(result);
}

double Atan(double x) noexcept {
    if (IsNaN(x)) {
        return NaN();
    }

    const bool negative = (BitCast<uint64>(x) & SignBit) != 0;
    const double magnitude = Magnitude(x);
    const DoubleDouble halfPi{0.5 * PiExtended.hi, 0.5 * PiExtended.lo};
    if (IsInfinite(x)) {
        return negative ? -halfPi.hi : halfPi.hi;
    }

    if (magnitude < AtanIdentityLimit) {
        return x; // ±0·비정규 수 포함 — atan(x)는 x로 반올림된다
    }

    // |x| > 1이면 atan(|x|) = π/2 − atan(1/|x|)(1/|x|는 double-double).
    const bool inverted = magnitude > 1.0;
    const DoubleDouble t =
        inverted ? Divide({1.0, 0.0}, {magnitude, 0.0}) : DoubleDouble{magnitude, 0.0};
    // t ∈ [0, 1]을 가장 가까운 k/8로 나눈다: atan(t) = atan(k/8) + atan(r), r = (t − c)/(1 + t·c).
    const double k = RoundHalfEven(8.0 * t.hi);
    const double c = 0.125 * k;
    const DoubleDouble numerator = Add(t, {-c, 0.0});
    const DoubleDouble denominator = Add({1.0, 0.0}, Multiply(t, {c, 0.0}));
    const DoubleDouble r = Divide(numerator, denominator);
    const DoubleDouble atanR = FastTwoSum(r.hi, r.lo + AtanTail(r.hi));
    DoubleDouble sum = Add(AtanEighths[static_cast<int>(k)], atanR);
    if (inverted) {
        sum = Add(halfPi, {-sum.hi, -sum.lo});
    }

    const double result = sum.hi + sum.lo;
    return Canonical(negative ? -result : result);
}

double SrgbToLinear(double encoded) noexcept {
    if (IsNaN(encoded)) {
        return NaN();
    }

    if (encoded <= 0.04045) {
        return encoded / 12.92;
    }

    if (IsInfinite(encoded)) {
        return PositiveInfinity;
    }

    // ((encoded + 0.055)/1.055)^(12/5). 밑과 지수를 double-double로 계산해 상수 반올림이 지수로
    // 커지지 않게 한다.
    const DoubleDouble base = Divide(Add({encoded, 0.0}, Srgb055), Srgb1055);
    DoubleDouble logBase = Log2Extended(base.hi);
    logBase.lo += base.lo / base.hi * InvLn2.hi; // log2(hi + lo) ≈ log2(hi) + lo/(hi·ln2)
    logBase = FastTwoSum(logBase.hi, logBase.lo);
    const DoubleDouble exponent = Divide({12.0, 0.0}, {5.0, 0.0});
    return PowFromLog(logBase, exponent);
}

double LinearToSrgb(double linear) noexcept {
    if (IsNaN(linear)) {
        return NaN();
    }

    if (linear <= 0.0031308) {
        return linear * 12.92;
    }

    if (IsInfinite(linear)) {
        return PositiveInfinity;
    }

    // 1.055·linear^(5/12) − 0.055. 거듭제곱을 double-double로 남겨 뺄셈의 상쇄가 오차를 키우지 않게
    // 한다.
    const DoubleDouble t = Multiply(Log2Extended(linear), Divide({5.0, 0.0}, {12.0, 0.0}));
    const ScaledResult power = Exp2Extended(t);
    const double unit = PowerOfTwo(power.exponent); // linear ≤ 2^1024이므로 지수가 정규 범위다
    const DoubleDouble value{power.value.hi * unit, power.value.lo * unit};
    const DoubleDouble result = Add(Multiply(value, Srgb1055), {-Srgb055.hi, -Srgb055.lo});
    return Canonical(result.hi + result.lo);
}

double Gamma22ToLinear(double encoded) noexcept {
    if (IsNaN(encoded)) {
        return NaN();
    }

    if (encoded <= 0.0) {
        return 0.0;
    }

    if (encoded == 1.0) {
        return 1.0;
    }

    if (IsInfinite(encoded)) {
        return PositiveInfinity;
    }

    // 지수는 정확한 11/5다.
    return PowFromLog(Log2Extended(encoded), Divide({11.0, 0.0}, {5.0, 0.0}));
}

float SrgbByteToLinear(uint8 value) noexcept {
    return BitCast<float>(detail::SrgbDecodeBits[value]);
}

uint8 LinearToSrgbByte(float linear) noexcept {
    if (!(linear > 0.0f)) {
        return 0; // NaN·0 이하
    }

    // 양의 float는 비트 순서가 값 순서와 같다. 경계 t_k ≤ linear인 개수가 결과다.
    const uint32 bits = BitCast<uint32>(linear);
    usize low = 0;
    usize high = 255;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (detail::SrgbEncodeThresholdBits[middle] <= bits) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return static_cast<uint8>(low);
}

} // namespace rocks::deterministic
