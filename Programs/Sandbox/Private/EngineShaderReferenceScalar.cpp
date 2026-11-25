#include "EngineShaderProbe.h"

// A02 스칼라·벡터·빠른 근사·확장 정밀도의 CPU double 기준 구현과
// 대조다(Docs/Design/EngineShaders 7.3절, 8.2·8.3절). 기준은 문헌 식을 그대로 옮긴 double 계산이고
// 셰이더 원문을 옮기지 않는다(결정 SLD10). 허용 오차는 8.3절 사양 상한에서 유도한다(각 검사 옆
// 주석).
namespace sandbox::engine_shaders {

namespace {

constexpr uint32 SmallWidth = 64;
constexpr uint32 SmallHeight = 64;
// 정규 직교 시험 크기(2^16 — 256 × 256, 7.3절)다.
constexpr uint32 BasisWidth = 256;
constexpr uint32 BasisHeight = 256;
constexpr double OrthonormalLimit = 1.0e-6;
constexpr double Pi = 3.14159265358979323846;
constexpr double FloatMinNormal = 1.1754943508222875e-38;
constexpr double SmallestSafeDivisor = static_cast<float>(1.0e-30);
constexpr double SmallestSafeSquaredLength = static_cast<float>(1.0e-30);
// 빠른 근사의 근사 오차 상한(FastApproximations.rsh 주석)과 float 계산 여유다.
constexpr double FastAcosBound = 6.8e-5;
constexpr double FastAtanBound = 1.2e-5;
constexpr double FastRsqrtBound = 1.76e-3;
constexpr double FastExp2Bound = 6.5e-6;
constexpr double FastLog2Bound = 2.8e-5;
constexpr double FastEvaluationMargin = 2.0e-6;
// 카메라 상대 위치 상한(cm, 7.3절)이다.
constexpr double TranslatedWorldLimit = 0.01;
// 확장 정밀도 덧셈·곱의 상대 오차 상한(≈ 2^-44)이다.
constexpr double ExtendedRelativeLimit = 1.0e-13;

double SafeRcpReference(float x) noexcept {
    const double sign = x >= 0.0f ? 1.0 : -1.0;
    const double magnitude = fabs(static_cast<double>(x)) < SmallestSafeDivisor
                                 ? SmallestSafeDivisor
                                 : fabs(static_cast<double>(x));
    return 1.0 / (sign * magnitude);
}

double Clamp(double x, double low, double high) noexcept {
    return x < low ? low : (x > high ? high : x);
}

double LinearStepReference(float edge0, float edge1, float x) noexcept {
    const double range = static_cast<double>(edge1) - edge0;
    if (edge1 == edge0) {
        return x >= edge1 ? 1.0 : 0.0;
    }

    return Clamp((static_cast<double>(x) - edge0) / range, 0.0, 1.0);
}

// pow(x, y) 사양 상한(exp2(y·log2 x) 전파 — 8.3절): 상대 (3 + 2|t|)·ulp + |y|·ln2·log2 절대 오차.
double PowTolerance(double result, double x, double y) noexcept {
    const double t = fabs(y * log2(x));
    return fabs(result) * ((3.0 + 2.0 * t) * FloatUlp +
                           fabs(y) * 0.6931471805599453 * SpecLog2Absolute * 4.0 + Arithmetic(2));
}

// Duff 2017 Listing 3(부호는 n.z의 부호 bit)다.
void DuffBasis(const Double3& n, bool negative, Double3& t, Double3& b) noexcept {
    const double sign = negative ? -1.0 : 1.0;
    const double a = -1.0 / (sign + n.z);
    const double c = n.x * n.y * a;
    t = {1.0 + sign * n.x * n.x * a, sign * c, -sign * n.x};
    b = {c, sign + n.y * n.y * a, -n.y};
}

void RunScalarProbes(ProbeContext& context) noexcept {
    // Square·Pow3·Pow4·Pow5(곱 1·2·2·3번 — 8.3절 산술 상한).
    ProbeRequest powers = MakeProbeRequest(ProbeScalarPowers, 0, SmallWidth, SmallHeight);
    Tolerance powerTolerance;
    const double powerRelative[4] = {Arithmetic(1), Arithmetic(2), Arithmetic(2), Arithmetic(3)};
    for (uint32 channel = 0; channel < 4; ++channel) {
        powerTolerance.relative[channel] = powerRelative[channel];
    }

    CheckFloatProbe(context, "ScalarMath.Powers", powers, powerTolerance,
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double x = ProbeSigned(c, 0, 2, 0);
                        e.value[0] = x * x;
                        e.value[1] = x * x * x;
                        e.value[2] = x * x * x * x;
                        e.value[3] = x * x * x * x * x;
                    });
    powers.selector = 1;
    CheckFloatProbe(context, "ScalarMath.Pow5Float4", powers, UniformTolerance(0.0, Arithmetic(3)),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        for (uint32 k = 0; k < 4; ++k) {
                            const double v = ProbeSigned(c, k, 2, 0);
                            e.value[k] = v * v * v * v * v;
                        }
                    });
    powers.selector = 2;
    CheckFloatProbe(context, "ScalarMath.PowOverloads", powers,
                    UniformTolerance(0.0, Arithmetic(3)),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double x = ProbeSigned(c, 0, 2, 0);
                        const double y = ProbeSigned(c, 1, 2, 0);
                        const double z = ProbeSigned(c, 2, 2, 0);
                        const double w = ProbeSigned(c, 3, 2, 0);
                        e.value[0] = x * x * x;
                        e.value[1] = y * y * y;
                        e.value[2] = z * z * z;
                        e.value[3] = w * w + y * y;
                    });

    // Max3·Min3·SignNotZero·SaturateSigned(정확).
    ProbeRequest minMax = MakeProbeRequest(ProbeScalarMinMax, 0, SmallWidth, SmallHeight);
    CheckFloatProbe(context, "ScalarMath.MinMaxSign", minMax, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const float a =
                            c < 4 ? BitsToFloat(ProbeSpecialBits[c]) : ProbeSigned(c, 0, 2, 0);
                        const double y = ProbeSigned(c, 1, 2, 0);
                        const double z = ProbeSigned(c, 2, 2, 0);
                        e.value[0] = fmax(a, fmax(y, z));
                        e.value[1] = fmin(a, fmin(y, z));
                        e.value[2] = a >= 0.0f ? 1.0 : -1.0;
                        e.value[3] = Clamp(a, -1.0, 1.0);
                    });
    minMax.selector = 1;
    CheckFloatProbe(context, "ScalarMath.MinMaxOverloads", minMax, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double a =
                            c < 4 ? BitsToFloat(ProbeSpecialBits[c]) : ProbeSigned(c, 0, 2, 0);
                        const double x = ProbeSigned(c, 0, 2, 0);
                        const double y = ProbeSigned(c, 1, 2, 0);
                        const double z = ProbeSigned(c, 2, 2, 0);
                        e.value[0] = fmax(a, fmax(y, z));
                        e.value[1] = fmin(y, fmin(z, a));
                        e.value[2] = a >= 0.0 ? 1.0 : -1.0;
                        e.value[3] = Clamp(a, -1.0, 1.0);
                        (void)x;
                    });

    // SafeRcp(2.5 ulp + 비정규 결과는 0으로 내려갈 수 있다)·SafeSqrt·LinearStep·SmootherStep.
    ProbeRequest safe = MakeProbeRequest(ProbeScalarSafe, 0, SmallWidth, SmallHeight);
    Tolerance safeTolerance;
    safeTolerance.absolute[0] = FloatMinNormal;
    safeTolerance.relative[0] = Arithmetic(1);
    safeTolerance.relative[1] = Arithmetic(2);
    safeTolerance.absolute[2] = Arithmetic(4);
    safeTolerance.absolute[3] = 3.0e-6;
    CheckFloatProbe(context, "ScalarMath.Safe", safe, safeTolerance,
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const float v = c < ProbeSpecialCount ? BitsToFloat(ProbeSpecialBits[c])
                                                              : ProbeSigned(c, 0, 4, 0);
                        const float finite = isfinite(v) ? v : 0.0f;
                        const float x = ProbeSigned(c, 0, 2, 0);
                        const float edge0 = ProbeSigned(c, 4, 1, 0);
                        const float step = ProbeUnit(c, 5, 0) * 2.0f;
                        const float edge1 = (c % 7u) == 0 ? edge0 : edge0 + step;
                        e.value[0] = SafeRcpReference(finite);
                        e.value[1] = sqrt(fmax(static_cast<double>(finite), 0.0));
                        const double t = LinearStepReference(edge0, edge1, x);
                        e.value[2] = t;
                        e.value[3] = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
                    });
    safe.selector = 1;
    CheckFloatProbe(context, "ScalarMath.SafeOverloads", safe,
                    UniformTolerance(FloatMinNormal, Arithmetic(2)),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        e.value[0] = SafeRcpReference(ProbeSigned(c, 0, 2, 0));
                        e.value[1] = SafeRcpReference(ProbeSigned(c, 1, 2, 0));
                        e.value[2] = sqrt(fmax(static_cast<double>(ProbeSigned(c, 1, 2, 0)), 0.0));
                        e.value[3] = sqrt(fmax(-static_cast<double>(ProbeSigned(c, 2, 2, 0)), 0.0));
                    });

    // SafeNormalize: 길이 제곱이 하한 근처(상대 1e-5 안)인 사례는 GPU float 반올림에 따라 갈리므로
    // 뺀다.
    ProbeRequest normalize = MakeProbeRequest(ProbeSafeNormalize, 0, SmallWidth, SmallHeight);
    CheckFloatProbe(context, "ScalarMath.SafeNormalize", normalize,
                    UniformTolerance(Arithmetic(4), 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double scale = ldexp(1.0, -static_cast<int>(c % 80u));
                        const Double3 v = Scale(ProbeVector(c, 0, 0), scale);
                        const double lengthSquared = Dot(v, v);
                        const double planar = v.x * v.x + v.y * v.y;
                        if (fabs(lengthSquared / SmallestSafeSquaredLength - 1.0) < 1.0e-5 ||
                            fabs(planar / SmallestSafeSquaredLength - 1.0) < 1.0e-5 ||
                            lengthSquared < 1.0e-37) {
                            e.mask = 0;
                            return;
                        }

                        const Double3 n = lengthSquared >= SmallestSafeSquaredLength
                                              ? Scale(v, 1.0 / sqrt(lengthSquared))
                                              : Double3{0.0, 0.0, 1.0};
                        e.value[0] = n.x;
                        e.value[1] = n.y;
                        e.value[2] = n.z;
                        e.value[3] = planar >= SmallestSafeSquaredLength ? v.x / sqrt(planar) : 1.0;
                    });

    // 수학 상수 = CPU double을 float로 반올림한 값(7.3절). 마지막 묶음은 곱 하나.
    ProbeRequest constants = MakeProbeRequest(ProbeMathConstants, 0, 4, 1);
    CheckFloatProbe(
        context, "MathConstants", constants, UniformTolerance(0.0, 0.0),
        [](uint32 c, const float*, ProbeExpectation& e) {
            const double table[3][4] = {{Pi, 2.0 * Pi, 0.5 * Pi, 1.0 / Pi},
                                        {0.5 / Pi, Pi / 180.0, 180.0 / Pi, 3.402823466e+38},
                                        {1.175494351e-38, 1.192092896e-07, 1.0e-30, 1.0e-30}};
            if (c < 3) {
                for (uint32 k = 0; k < 4; ++k) {
                    e.value[k] = static_cast<float>(table[c][k]);
                }

                return;
            }

            e.value[0] = static_cast<float>(180.0f * static_cast<float>(Pi / 180.0));
            e.value[1] =
                static_cast<float>(static_cast<float>(Pi) * static_cast<float>(180.0 / Pi));
            e.value[2] = 0.0;
            e.value[3] = 0.0;
            e.absolute[0] = Arithmetic(1) * 4.0;
            e.absolute[1] = Arithmetic(1) * 180.0;
        });

    // PowPositive(x ≤ 0 → 0, 사양 pow 상한).
    ProbeRequest powRequest = MakeProbeRequest(ProbePowPositive, 0, SmallWidth, SmallHeight);
    CheckFloatProbe(context, "ScalarMath.PowPositive", powRequest, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double base = c < 4 ? 0.0 : ProbeUnit(c, 0, 0) * 4.0f;
                        const double y = ProbeSigned(c, 1, 2, 0);
                        const double exponents[3] = {y, 0.5, static_cast<float>(2.4)};
                        for (uint32 k = 0; k < 3; ++k) {
                            e.value[k] = base > 0.0 ? pow(base, exponents[k]) : 0.0;
                            e.absolute[k] =
                                base > 0.0 ? PowTolerance(e.value[k], base, exponents[k]) : 0.0;
                        }

                        e.value[3] = 0.0;
                        e.absolute[3] = 0.0;
                    });
}

void RunVectorProbes(ProbeContext& context) noexcept {
    // 정규 직교 기저(Duff 2017): GPU가 쓴 n(선택자 2)에서 같은 분기로 double 기준을 만든다. 전 방향
    // 무작위 2^16개와 축·n.z = −1 근처 경계(앞 8개).
    ProbeRequest basis = MakeProbeRequest(ProbeOrthonormalBasis, 0, BasisWidth, BasisHeight);
    Array<float> tangent(context.allocator);
    Array<float> bitangent(context.allocator);
    Array<float> normal(context.allocator);
    bool ran = RunFloatProbe(context, "VectorMath.BasisTangent", basis, tangent);
    basis.selector = 1;
    ran = ran && RunFloatProbe(context, "VectorMath.BasisBitangent", basis, bitangent);
    basis.selector = 2;
    ran = ran && RunFloatProbe(context, "VectorMath.BasisNormal", basis, normal);
    if (ran) {
        double worstOrthonormal = 0.0;
        double worstReference = 0.0;
        uint32 handedness = 0;
        for (uint32 c = 0; c < CaseCount(basis); ++c) {
            const Double3 t = ToDouble3(tangent.Data() + c * 4u);
            const Double3 b = ToDouble3(bitangent.Data() + c * 4u);
            const Double3 n = ToDouble3(normal.Data() + c * 4u);
            const double errors[6] = {fabs(Dot(t, t) - 1.0), fabs(Dot(b, b) - 1.0),
                                      fabs(Dot(n, n) - 1.0), fabs(Dot(t, b)),
                                      fabs(Dot(t, n)),       fabs(Dot(b, n))};
            for (const double error : errors) {
                worstOrthonormal = fmax(worstOrthonormal, error);
            }

            if (Dot(Cross(t, b), n) < 0.0) {
                ++handedness;
            }

            Double3 rt;
            Double3 rb;
            DuffBasis(n, signbit(normal[c * 4u + 2]), rt, rb);
            const double differences[6] = {fabs(rt.x - t.x), fabs(rt.y - t.y), fabs(rt.z - t.z),
                                           fabs(rb.x - b.x), fabs(rb.y - b.y), fabs(rb.z - b.z)};
            for (const double difference : differences) {
                worstReference = fmax(worstReference, difference);
            }
        }

        char detail[192];
        snprintf(detail, sizeof(detail),
                 "%u bases, max |B^T B - I| %.3g (limit %.0e), max diff to double Duff %.3g, "
                 "left-handed %u",
                 CaseCount(basis), worstOrthonormal, OrthonormalLimit, worstReference, handedness);
        ReportProbe(context, "VectorMath.BuildOrthonormalBasis",
                    worstOrthonormal <= OrthonormalLimit && worstReference <= OrthonormalLimit &&
                        handedness == 0,
                    detail);
    }

    // Gram–Schmidt: 평행(t = 3n, 다섯에 하나)은 Duff tangent, 나머지는 n에 직교 정규화. 남은 길이가
    // 작으면 오차가 |t| / |t⊥| 배로 커진다(사례별 허용 오차).
    ProbeRequest gram = MakeProbeRequest(ProbeGramSchmidt, 0, BasisWidth, BasisHeight);
    if (ran) {
        CheckFloatProbe(
            context, "VectorMath.GramSchmidtTangent", gram, UniformTolerance(0.0, 0.0),
            [&normal](uint32 c, const float*, ProbeExpectation& e) {
                const Double3 n = ToDouble3(normal.Data() + c * 4u);
                const Double3 t = (c % 5u) == 0 ? Scale(n, 3.0) : ProbeVector(c, 3, 0);
                const Double3 projected = Subtract(t, Scale(n, Dot(n, t)));
                const double projectedSquared = Dot(projected, projected);
                Double3 result;
                if ((c % 5u) == 0) {
                    Double3 unused;
                    DuffBasis(n, signbit(normal[c * 4u + 2]), result, unused);
                } else {
                    result = Scale(projected, 1.0 / sqrt(projectedSquared));
                }

                const double bound =
                    1.0e-6 + 8.0 * FloatUlp * sqrt(Dot(t, t) / fmax(projectedSquared, 1e-30));
                e.value[0] = result.x;
                e.value[1] = result.y;
                e.value[2] = result.z;
                e.mask = 0x7u;
                e.absolute[0] = e.absolute[1] = e.absolute[2] = (c % 5u) == 0 ? 1.0e-6 : bound;
            });
    }

    // 반사·굴절(Snell, GLSL refract 식). 입사가 법선 쪽이면 뒤집는다(경계 |n·i| < 1e-5는 뺀다).
    ProbeRequest reflect = MakeProbeRequest(ProbeReflectRefract, 0, BasisWidth, BasisHeight);
    auto incidentFor = [&normal](uint32 c, Double3& n, Double3& incident) {
        n = ToDouble3(normal.Data() + c * 4u);
        incident = ProbeDirection(c, 3, 0);
        const double d = Dot(incident, n);
        if (d > 0.0) {
            incident = Scale(incident, -1.0);
        }

        return fabs(d) >= 1.0e-5;
    };
    if (ran) {
        CheckFloatProbe(context, "VectorMath.ReflectDirection", reflect,
                        UniformTolerance(2.0e-6, 0.0),
                        [&incidentFor](uint32 c, const float*, ProbeExpectation& e) {
                            Double3 n;
                            Double3 i;
                            if (!incidentFor(c, n, i)) {
                                e.mask = 0;
                                return;
                            }

                            const Double3 r = Subtract(i, Scale(n, 2.0 * Dot(n, i)));
                            e.value[0] = r.x;
                            e.value[1] = r.y;
                            e.value[2] = r.z;
                            e.mask = 0x7u;
                        });
        reflect.selector = 1;
        CheckFloatProbe(context, "VectorMath.RefractDirection", reflect,
                        UniformTolerance(2.0e-6, 0.0),
                        [&incidentFor](uint32 c, const float*, ProbeExpectation& e) {
                            Double3 n;
                            Double3 i;
                            if (!incidentFor(c, n, i)) {
                                e.mask = 0;
                                return;
                            }

                            const float scaled = ProbeUnit(c, 6, 0) * 1.5f;
                            const double eta = 0.5f + scaled;
                            const double cosI = Dot(n, i);
                            const double k = 1.0 - eta * eta * (1.0 - cosI * cosI);
                            if (fabs(k) < 1.0e-5) {
                                e.mask = 0;
                                return;
                            }

                            Double3 r{0.0, 0.0, 0.0};
                            if (k >= 0.0) {
                                r = Subtract(Scale(i, eta), Scale(n, eta * cosI + sqrt(k)));
                            }

                            const double bound = 2.0e-6 + 2.0e-7 / sqrt(fabs(k));
                            e.value[0] = r.x;
                            e.value[1] = r.y;
                            e.value[2] = r.z;
                            e.value[3] = k < 0.0 ? 1.0 : 0.0;
                            e.absolute[0] = e.absolute[1] = e.absolute[2] = bound;
                            e.absolute[3] = 0.0;
                        });
    }

    // 두 단위 벡터 사이 각(atan 사양 4096 ulp — 상대 2^-11).
    ProbeRequest angle = MakeProbeRequest(ProbeAngleBetween, 0, BasisWidth, BasisHeight);
    if (ran) {
        CheckFloatProbe(context, "VectorMath.AngleBetweenUnitVectors", angle,
                        UniformTolerance(2.0e-6, 4096.0 * FloatUlp * 2.0),
                        [&normal](uint32 c, const float*, ProbeExpectation& e) {
                            const Double3 n = ToDouble3(normal.Data() + c * 4u);
                            Double3 other = ProbeDirection(c, 3, 0);
                            if (c % 11u == 1u) {
                                other = n;
                            } else if (c % 11u == 2u) {
                                other = Scale(n, -1.0);
                            }

                            const Double3 d = Subtract(n, other);
                            const Double3 s = Add(n, other);
                            e.value[0] = 2.0 * atan2(sqrt(Dot(d, d)), sqrt(Dot(s, s)));
                            e.mask = 0x1u;
                        });
    }
}

void RunApproximationProbes(ProbeContext& context) noexcept {
    // 빠른 삼각 근사(정의역 전체 — 7.3절): 근사 상한 + float 계산 여유.
    ProbeRequest trig = MakeProbeRequest(ProbeFastTrig, 0, 256, 256);
    Tolerance trigTolerance;
    trigTolerance.absolute[0] = FastAcosBound + FastEvaluationMargin;
    trigTolerance.absolute[1] = FastAcosBound + FastEvaluationMargin;
    trigTolerance.absolute[2] = FastAtanBound + FastEvaluationMargin;
    trigTolerance.absolute[3] = FastAtanBound + 2.0 * FastEvaluationMargin;
    CheckFloatProbe(context, "FastApproximations.Trig", trig, trigTolerance,
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        double x = ProbeSigned(c, 0, 0, 0);
                        if (c < 3) {
                            x = c == 0 ? 1.0 : (c == 1 ? -1.0 : 0.0);
                        }

                        const double y = ProbeSigned(c, 1, 6, 0);
                        const double z =
                            c == 3 ? 0.0 : static_cast<double>(ProbeSigned(c, 2, 6, 0));
                        e.value[0] = acos(x);
                        e.value[1] = asin(x);
                        e.value[2] = atan(y);
                        e.value[3] = (y == 0.0 && z == 0.0) ? 0.0 : atan2(y, z);
                    });

    ProbeRequest expLog = MakeProbeRequest(ProbeFastExpLog, 0, 256, 256);
    Tolerance expLogTolerance;
    expLogTolerance.relative[0] = FastRsqrtBound + Arithmetic(4);
    expLogTolerance.relative[1] = FastExp2Bound + Arithmetic(8);
    expLogTolerance.absolute[2] = FastLog2Bound + FastEvaluationMargin;
    CheckFloatProbe(context, "FastApproximations.ExpLog", expLog, expLogTolerance,
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double positive = ProbeWidePositive(c, 0, 0);
                        const double w = ProbeSigned(c, 1, 7, 0);
                        const double clamped = Clamp(w, -126.0, static_cast<float>(127.99999));
                        e.value[0] = 1.0 / sqrt(positive);
                        e.value[1] = exp2(clamped);
                        e.value[2] = log2(positive);
                        e.value[3] = positive;
                        e.absolute[3] = 0.0;
                    });
}

void RunExtendedProbes(ProbeContext& context) noexcept {
    // TwoSum·TwoProduct가 오차 없는 변환인가(sum + error = a + b, product + error = a·b —
    // double에서 정확). 컴파일러 재결합·FMA 축약이 있으면 깨진다(precise 확인 — 7.3절).
    ProbeRequest sums = MakeProbeRequest(ProbeExtendedSums, 0, 256, 256);
    Array<float> gpu(context.allocator);
    if (RunFloatProbe(context, "ExtendedPrecision.TwoSumTwoProduct", sums, gpu)) {
        uint32 sumFailures = 0;
        uint32 productFailures = 0;
        for (uint32 c = 0; c < CaseCount(sums); ++c) {
            const float a =
                ProbeWidePositive(c, 0, 0) * ((ProbeBits(c, 2, 0) & 1u) != 0 ? -1.0f : 1.0f);
            const float b =
                ProbeWidePositive(c, 1, 0) * ((ProbeBits(c, 3, 0) & 1u) != 0 ? -1.0f : 1.0f);
            const float* v = gpu.Data() + c * 4u;
            const float roundedSum = a + b;
            if (static_cast<double>(v[0]) + v[1] != static_cast<double>(a) + b ||
                v[0] != roundedSum) {
                ++sumFailures;
            }

            if (static_cast<double>(v[2]) + v[3] != static_cast<double>(a) * b) {
                ++productFailures;
            }
        }

        char detail[160];
        snprintf(detail, sizeof(detail), "%u pairs, inexact sums %u, inexact products %u",
                 CaseCount(sums), sumFailures, productFailures);
        ReportProbe(context, "ExtendedPrecision.ErrorFreeTransforms",
                    sumFailures == 0 && productFailures == 0, detail);
    }

    // 1e7 cm(±2^24) world의 카메라 상대 위치 ≤ 0.01 cm(7.3절), 확장 정밀도 덧셈·곱.
    struct WorldInputs {
        double world[3];
        double view[3];
        float worldHigh[3];
        float scale;
    };
    auto inputs = [](uint32 c) {
        WorldInputs in;
        for (uint32 k = 0; k < 3; ++k) {
            const float high = ProbeSigned(c, k, 24, 0);
            const float low = ProbeSigned(c, 3 + k, 0, 0) * 0.5f;
            const float offset = ProbeSigned(c, 6 + k, 0, 0) * 1024.0f;
            const float viewHigh = high + offset;
            const float viewLow = ProbeSigned(c, 9 + k, 0, 0) * 0.5f;
            in.world[k] = static_cast<double>(high) + low;
            in.view[k] = static_cast<double>(viewHigh) + viewLow;
            in.worldHigh[k] = high + low;
        }

        in.scale = ProbeUnit(c, 12, 0) + 0.5f;
        return in;
    };
    ProbeRequest world = MakeProbeRequest(ProbeTranslatedWorld, 0, 256, 256);
    CheckFloatProbe(context, "ExtendedPrecision.ToTranslatedWorld", world,
                    UniformTolerance(TranslatedWorldLimit, 0.0),
                    [&inputs](uint32 c, const float*, ProbeExpectation& e) {
                        const WorldInputs in = inputs(c);
                        for (uint32 k = 0; k < 3; ++k) {
                            e.value[k] = in.world[k] - in.view[k];
                        }

                        e.mask = 0x7u;
                    });

    Array<float> high(context.allocator);
    Array<float> low(context.allocator);
    for (uint32 operation = 0; operation < 2; ++operation) {
        world.selector = operation == 0 ? 1 : 3;
        const bool ok = RunFloatProbe(
            context, operation == 0 ? "ExtendedPrecision.AddHigh" : "ExtendedPrecision.ScaleHigh",
            world, high);
        world.selector = operation == 0 ? 2 : 4;
        if (!ok || !RunFloatProbe(context,
                                  operation == 0 ? "ExtendedPrecision.AddLow"
                                                 : "ExtendedPrecision.ScaleLow",
                                  world, low)) {
            continue;
        }

        double worst = 0.0;
        for (uint32 c = 0; c < CaseCount(world); ++c) {
            const WorldInputs in = inputs(c);
            for (uint32 k = 0; k < 3; ++k) {
                // 곱: (high·s + low·s)는 각 항이 double에서 정확하고 합의 반올림은 2^-53이다.
                const float worldLow = ProbeSigned(c, 3 + k, 0, 0) * 0.5f;
                const float worldHighPart = ProbeSigned(c, k, 24, 0);
                const double exact = operation == 0
                                         ? in.world[k] + in.view[k]
                                         : static_cast<double>(worldHighPart) * in.scale +
                                               static_cast<double>(worldLow) * in.scale;
                const double value = static_cast<double>(high[c * 4u + k]) + low[c * 4u + k];
                worst = fmax(worst, fabs(value - exact) / fmax(fabs(exact), 1.0));
            }
        }

        char detail[128];
        snprintf(detail, sizeof(detail), "max relative error of high + low %.3g (limit %.0e)",
                 worst, ExtendedRelativeLimit);
        ReportProbe(context,
                    operation == 0 ? "ExtendedPrecision.ExtendedAdd"
                                   : "ExtendedPrecision.ExtendedScale",
                    worst <= ExtendedRelativeLimit, detail);
    }

    world.selector = 5;
    CheckFloatProbe(context, "ExtendedPrecision.NormalizeExtended", world,
                    UniformTolerance(0.0, 0.0),
                    [&inputs](uint32 c, const float*, ProbeExpectation& e) {
                        const WorldInputs in = inputs(c);
                        for (uint32 k = 0; k < 3; ++k) {
                            e.value[k] = in.worldHigh[k];
                        }

                        e.mask = 0x7u;
                    });
}

} // namespace

void RunScalarVectorProbes(ProbeContext& context) noexcept {
    RunScalarProbes(context);
    RunVectorProbes(context);
    RunApproximationProbes(context);
    RunExtendedProbes(context);
}

} // namespace sandbox::engine_shaders
