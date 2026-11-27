#include <Curves/CurveMathDeterministic.h>

#include <Core/Math/MathUtility.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/Memory.h>

// 커브 쿠킹 평가의 결정적 수학 정책이다(Docs/Design/CurveData 5.9절 D22).
namespace rocks {

namespace {

// 고정 quiet NaN(float)이다(DeterministicMath의 double 고정 NaN과 같은 꼴).
constexpr uint32 CanonicalFloatNaNBits = 0x7FC00000u;

} // namespace

float CurveMathDeterministic::Atan(float x) noexcept {
    return static_cast<float>(deterministic::Atan(static_cast<double>(x)));
}

double CurveMathDeterministic::Acos(double x) noexcept {
    return deterministic::Acos(x);
}

double CurveMathDeterministic::Cos(double x) noexcept {
    return deterministic::Cos(x);
}

double CurveMathDeterministic::CubeRoot(double x) noexcept {
    return x > 0.0 ? deterministic::Pow(x, 1.0 / 3.0)
                   : (x < 0.0 ? -deterministic::Pow(-x, 1.0 / 3.0) : 0.0);
}

float CurveMathDeterministic::Canonical(float value) noexcept {
    return math::IsNaN(value) ? platform::BitCast<float>(CanonicalFloatNaNBits) : value;
}

} // namespace rocks
