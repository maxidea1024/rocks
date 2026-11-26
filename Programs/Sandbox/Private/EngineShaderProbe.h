#pragma once

#include "ProbeFunctionIds.h"

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// 엔진 셰이더 probe 시험 틀이다(Docs/Design/EngineShaders 7.6절·8장, 결정 SLD19·SLD20). PS probe
// 하나를 장치에서 돌린 결과를 CPU double 기준 구현(EngineShaderReference*.cpp)과 대조하고, 결과를
// 장치 사이 대조용으로 기록한다. 입력 생성기는 probe 셰이더 Common/ProbeCommon.rsh와 같은
// 식이다(정수 섞기와 2의 거듭제곱 배율 — 입력이 bit 단위로 같다).
//
// 사용 예(영역 시험 함수 안):
//   ProbeRequest request = MakeProbeRequest(ProbeScalarPowers, 0, 64, 64);
//   CheckFloatProbe(context, "ScalarMath.Powers", request, UniformTolerance(0, Arithmetic(3)),
//                   [](uint32 c, const float*, double* reference) { ... return 0xFu; });
namespace sandbox::engine_shaders {

using rocks::Allocator;
using rocks::Array;
using rocks::Span;
using rocks::uint32;
using rocks::uint64;
using rocks::usize;
using namespace sandbox::engine_shader_probe;

// 특수 실수 bit 패턴이다(ProbeCommon.rsh ProbeSpecialBits와 같은 순서).
inline constexpr uint32 ProbeSpecialCount = 16;
inline constexpr uint32 ProbeSpecialBits[ProbeSpecialCount] = {
    0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x3f000000u, 0x00800000u,
    0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x0d000000u, 0x477fe000u,
    0x477ff000u, 0x38800000u, 0x33800000u, 0x33000000u};

// float 기계 엡실론의 반(2^-24, 반올림 한 번의 상대 오차)과 ulp(2^-23)다.
inline constexpr double FloatUlp = 1.1920928955078125e-7;

// 산술만 n번 쓰는 함수의 상대 오차 상한(n·0.5 + 2.5)·2^-23이다(8.3절).
constexpr double Arithmetic(uint32 operations) noexcept {
    return (operations * 0.5 + 2.5) * FloatUlp;
}

// 사양 상한(8.3절)이다: sin·cos 절대 2^-11, exp2 (3 + 2|x|) ulp, log2 [0.5, 2] 안 절대 2^-21.
inline constexpr double SpecTrigAbsolute = 4.8828125e-4;
inline constexpr double SpecLog2Absolute = 4.76837158203125e-7;

// pow(base, exponent)의 상대 오차 상한(exp2(y·log2 x) 전파 — 8.3절)이다.
inline double PowRelative(double exponent, double base) noexcept {
    if (base <= 0.0) {
        return 0.0;
    }

    const double t = fabs(exponent * log2(base));
    return (3.0 + 2.0 * t) * FloatUlp +
           fabs(exponent) * 0.6931471805599453 * SpecLog2Absolute * 4.0;
}

// χ² 임계값(Wilson–Hilferty 근사, 유의 수준 0.001 — 표준 정규 분위수 3.0902)이다.
inline double ChiSquareCritical(double degrees) noexcept {
    constexpr double Quantile = 3.0902323061678132;
    const double a = 2.0 / (9.0 * degrees);
    const double b = 1.0 - a + Quantile * sqrt(a);
    return degrees * b * b * b;
}

// probe 요청이다(루트 상수와 배치).
struct ProbeRequest {
    uint32 function = 0;
    uint32 selector = 0;
    uint32 width = 0;
    uint32 height = 0;
    uint32 seed = 0;
    float parameters[4] = {};
    float values[16] = {};
};

inline ProbeRequest MakeProbeRequest(uint32 function, uint32 selector, uint32 width, uint32 height,
                                     uint32 seed = 0) noexcept {
    ProbeRequest request;
    request.function = function;
    request.selector = selector;
    request.width = width;
    request.height = height;
    request.seed = seed;
    return request;
}

inline uint32 CaseCount(const ProbeRequest& request) noexcept {
    return request.width * request.height;
}

// 장치에서 probe를 돌린다(EngineShaderSelfTest.cpp가 구현한다). out은 사례마다 float 넷 또는 uint
// 하나다.
class ProbeDevice {
public:
    virtual bool RunFloat(const ProbeRequest& request, Array<float>& out) noexcept = 0;
    virtual bool RunUint(const ProbeRequest& request, Array<uint32>& out) noexcept = 0;

protected:
    ~ProbeDevice() = default;
};

// 장치 사이 대조 기록이다(EngineShaderSelfTest.cpp가 구현한다).
class ProbeRecorder {
public:
    virtual void RecordFloat(const char* label, Span<const float> values) noexcept = 0;
    virtual void RecordUint(const char* label, Span<const uint32> values) noexcept = 0;

protected:
    ~ProbeRecorder() = default;
};

// 영역 시험 함수가 받는 문맥이다.
struct ProbeContext {
    ProbeDevice& device;
    ProbeRecorder& recorder;
    Allocator& allocator;
    const char* deviceName;
    uint32 failures = 0;
    uint32 probes = 0;
};

// 검사 결과 한 줄을 찍는다(통과는 측정값 요약, 실패는 [FAIL]).
inline void ReportProbe(ProbeContext& context, const char* label, bool passed,
                        const char* detail) noexcept {
    ++context.probes;
    if (!passed) {
        ++context.failures;
    }

    printf("    [%s] %s %s: %s\n", passed ? "PASS" : "FAIL", context.deviceName, label, detail);
}

// --- 입력 생성기(ProbeCommon.rsh와 같은 식) ---

inline constexpr uint32 ProbeCaseMultiplier = 0x9e3779b1u;
inline constexpr uint32 ProbeSeedMultiplier = 0x632be5abu;

inline uint32 ProbeMix(uint32 x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

inline uint32 ProbeBits(uint32 caseIndex, uint32 stream, uint32 seed) noexcept {
    return ProbeMix(caseIndex * ProbeCaseMultiplier ^
                    ProbeMix(stream + seed * ProbeSeedMultiplier));
}

inline float ProbePowerOfTwo(int exponent) noexcept {
    return ldexpf(1.0f, exponent);
}

inline float ProbeUnit(uint32 caseIndex, uint32 stream, uint32 seed) noexcept {
    return static_cast<float>(ProbeBits(caseIndex, stream, seed) >> 8) * ProbePowerOfTwo(-24);
}

inline float ProbeSigned(uint32 caseIndex, uint32 stream, int exponent, uint32 seed) noexcept {
    const float scaled = ProbeUnit(caseIndex, stream, seed) * ProbePowerOfTwo(exponent + 1);
    const float shifted = scaled - ProbePowerOfTwo(exponent);
    return shifted;
}

struct Double3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline Double3 ProbeVector(uint32 caseIndex, uint32 stream, uint32 seed) noexcept {
    return {ProbeSigned(caseIndex, stream, 0, seed), ProbeSigned(caseIndex, stream + 1, 0, seed),
            ProbeSigned(caseIndex, stream + 2, 0, seed)};
}

inline double Dot(const Double3& a, const Double3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Double3 Scale(const Double3& a, double s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}

inline Double3 Add(const Double3& a, const Double3& b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Double3 Subtract(const Double3& a, const Double3& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Double3 Cross(const Double3& a, const Double3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// GPU 결과 float 셋(사례 값의 앞 셋)이다.
inline Double3 ToDouble3(const float* values) noexcept {
    return {values[0], values[1], values[2]};
}

inline Double3 Normalize(const Double3& a) noexcept {
    return Scale(a, 1.0 / sqrt(Dot(a, a)));
}

// ProbeDirection과 같은 규칙(길이 제곱 1/64 미만은 +Z)이다.
inline Double3 ProbeDirection(uint32 caseIndex, uint32 stream, uint32 seed) noexcept {
    const Double3 v = ProbeVector(caseIndex, stream, seed);
    return Dot(v, v) < 1.0 / 64.0 ? Double3{0.0, 0.0, 1.0} : Normalize(v);
}

inline float ProbeWidePositive(uint32 caseIndex, uint32 stream, uint32 seed) noexcept {
    const uint32 bits = ProbeBits(caseIndex, stream, seed);
    const uint32 exponent = 107u + (bits >> 27) + ((bits >> 22) & 0x1fu) % 9u;
    const uint32 pattern = (exponent << 23) | (bits & 0x7fffffu);
    float value;
    memcpy(&value, &pattern, sizeof(value));
    return value;
}

inline uint32 ProbeFloatBits(uint32 caseIndex, uint32 stream, uint32 seed) noexcept {
    if (caseIndex < ProbeSpecialCount) {
        return ProbeSpecialBits[caseIndex];
    }

    uint32 bits = ProbeBits(caseIndex, stream, seed);
    if ((bits & 0x7f800000u) == 0u && (bits & 0x7fffffu) != 0u) {
        bits |= 0x00800000u;
    }

    return bits;
}

inline float BitsToFloat(uint32 bits) noexcept {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

inline uint32 FloatToBits(float value) noexcept {
    uint32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// --- 대조 ---

// 채널별 허용 오차 |gpu − ref| ≤ absolute + relative·|ref|다.
struct Tolerance {
    double absolute[4] = {};
    double relative[4] = {};
};

inline Tolerance UniformTolerance(double absolute, double relative) noexcept {
    Tolerance tolerance;
    for (usize channel = 0; channel < 4; ++channel) {
        tolerance.absolute[channel] = absolute;
        tolerance.relative[channel] = relative;
    }

    return tolerance;
}

// 사례 하나의 기대값이다. mask bit가 선 채널만 대조한다(0이면 그 사례를 건너뛴다). 기대값이 NaN이면
// GPU 값도 NaN이어야 하고, ±Inf면 같은 Inf여야 한다.
struct ProbeExpectation {
    double value[4] = {};
    uint32 mask = 0xFu;
    // 음수가 아니면 이 사례·채널의 절대 허용 오차다(입력에 따라 달라지는 사양 상한 — Tolerance
    // 대신).
    double absolute[4] = {-1.0, -1.0, -1.0, -1.0};
};

// 대조 결과 통계다.
struct FloatProbeStats {
    double maxError = 0.0;
    double maxRatio = 0.0;
    uint32 worstCase = 0;
    uint32 worstChannel = 0;
    uint32 compared = 0;
    uint32 failed = 0;
    double worstGpu = 0.0;
    double worstReference = 0.0;
};

// 장치에서 돌리고 기록한다. 실패하면 [FAIL]을 남기고 false다.
inline bool RunFloatProbe(ProbeContext& context, const char* label, const ProbeRequest& request,
                          Array<float>& gpu) noexcept {
    if (!context.device.RunFloat(request, gpu) ||
        gpu.Size() != static_cast<usize>(CaseCount(request)) * 4u) {
        ReportProbe(context, label, false, "probe draw or readback failed");
        return false;
    }

    context.recorder.RecordFloat(label, gpu.View());
    return true;
}

inline bool RunUintProbe(ProbeContext& context, const char* label, const ProbeRequest& request,
                         Array<uint32>& gpu) noexcept {
    if (!context.device.RunUint(request, gpu) ||
        gpu.Size() != static_cast<usize>(CaseCount(request))) {
        ReportProbe(context, label, false, "probe draw or readback failed");
        return false;
    }

    context.recorder.RecordUint(label, gpu.View());
    return true;
}

// 사례마다 expected(사례, gpu 값 넷, 기대값)로 대조한다. 결과 통계를 돌려준다(stats가 null이
// 아니면).
template <typename Expected>
bool CheckFloatProbe(ProbeContext& context, const char* label, const ProbeRequest& request,
                     const Tolerance& tolerance, Expected&& expected,
                     FloatProbeStats* statsOut = nullptr) noexcept {
    Array<float> gpu(context.allocator);
    if (!RunFloatProbe(context, label, request, gpu)) {
        return false;
    }

    FloatProbeStats stats;
    const uint32 cases = CaseCount(request);
    for (uint32 c = 0; c < cases; ++c) {
        const float* values = gpu.Data() + static_cast<usize>(c) * 4u;
        ProbeExpectation expectation;
        expected(c, values, expectation);
        for (uint32 channel = 0; channel < 4; ++channel) {
            if ((expectation.mask & (1u << channel)) == 0) {
                continue;
            }

            ++stats.compared;
            const double reference = expectation.value[channel];
            const double actual = values[channel];
            bool ok;
            double error = 0.0;
            double ratio = 0.0;
            if (isnan(reference)) {
                ok = isnan(actual);
            } else if (isinf(reference)) {
                ok = actual == reference;
            } else {
                error = fabs(actual - reference);
                const double bound = expectation.absolute[channel] >= 0.0
                                         ? expectation.absolute[channel]
                                         : tolerance.absolute[channel] +
                                               tolerance.relative[channel] * fabs(reference);
                ratio = bound > 0.0 ? error / bound : (error > 0.0 ? 1.0e30 : 0.0);
                ok = error <= bound;
            }

            if (!ok) {
                ++stats.failed;
                if (ratio < 1.0) {
                    // NaN·Inf 불일치는 배율이 없으므로 가장 나쁜 값으로 센다.
                    ratio = 1.0e30;
                }
            }

            if (ratio >= stats.maxRatio) {
                stats.maxRatio = ratio;
                stats.worstCase = c;
                stats.worstChannel = channel;
                stats.worstGpu = actual;
                stats.worstReference = reference;
            }

            if (error > stats.maxError) {
                stats.maxError = error;
            }
        }
    }

    char detail[256];
    snprintf(detail, sizeof(detail),
             "%u values, max error %.3g (%.2f of tolerance)%s, worst case %u.%u gpu %.9g ref %.9g",
             stats.compared, stats.maxError, stats.maxRatio,
             stats.failed != 0 ? ", OUT OF TOLERANCE" : "", stats.worstCase, stats.worstChannel,
             stats.worstGpu, stats.worstReference);
    const bool passed = stats.failed == 0 && stats.compared != 0;
    ReportProbe(context, label, passed, detail);
    if (statsOut != nullptr) {
        *statsOut = stats;
    }

    return passed;
}

// 64 bit FNV-1a다(golden 지문).
inline uint64 Fingerprint(Span<const uint32> values) noexcept {
    uint64 hash = 0xcbf29ce484222325ull;
    for (const uint32 value : values) {
        for (uint32 byte = 0; byte < 4; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xffu;
            hash *= 0x100000001b3ull;
        }
    }

    return hash;
}

// 정수 probe를 bit 단위로 대조한다. CPU 기준 결과의 지문이 golden과 같아야 한다(CPU 기준 구현을
// 고친 커밋에서만 golden을 바꾼다 — 8.6절).
template <typename Expected>
bool CheckUintProbe(ProbeContext& context, const char* label, const ProbeRequest& request,
                    uint64 golden, Expected&& expected) noexcept {
    Array<uint32> gpu(context.allocator);
    if (!RunUintProbe(context, label, request, gpu)) {
        return false;
    }

    Array<uint32> reference(context.allocator);
    if (!reference.TryResize(gpu.Size())) {
        ReportProbe(context, label, false, "out of memory");
        return false;
    }

    uint32 mismatches = 0;
    uint32 first = 0;
    for (uint32 c = 0; c < CaseCount(request); ++c) {
        reference[c] = expected(c);
        if (reference[c] != gpu[c]) {
            if (mismatches == 0) {
                first = c;
            }

            ++mismatches;
        }
    }

    const uint64 fingerprint = Fingerprint(reference.View());
    char detail[256];
    snprintf(detail, sizeof(detail),
             "%u values, %u mismatches%s (first case %u gpu 0x%08x ref 0x%08x), golden 0x%016llx%s",
             CaseCount(request), mismatches, mismatches != 0 ? " NOT BIT EXACT" : "", first,
             gpu.IsEmpty() ? 0u : gpu[first], reference.IsEmpty() ? 0u : reference[first],
             static_cast<unsigned long long>(fingerprint),
             fingerprint != golden ? " DIFFERS FROM THE STORED GOLDEN" : "");
    const bool passed = mismatches == 0 && fingerprint == golden;
    ReportProbe(context, label, passed, detail);
    return passed;
}

// 영역 시험 함수다(EngineShaderReference*.cpp).
void RunScalarVectorProbes(ProbeContext& context) noexcept;
void RunMatrixQuaternionProbes(ProbeContext& context) noexcept;
void RunPackingProbes(ProbeContext& context) noexcept;
void RunRandomSamplingProbes(ProbeContext& context) noexcept;
void RunColorProbes(ProbeContext& context) noexcept;
void RunShadingProbes(ProbeContext& context) noexcept;

} // namespace sandbox::engine_shaders
