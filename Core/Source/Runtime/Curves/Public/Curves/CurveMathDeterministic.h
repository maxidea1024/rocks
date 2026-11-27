#pragma once

#include <Base/Types.h>

// 커브 쿠킹 평가의 결정적 수학 정책이다(Docs/Design/CurveData 5.9절 D22). math::EvalCurveSegment·
// WeightedCurveSegment·SolveCurveCubic의 MathPolicy 인자로 넘기면 가중 경로의 C 런타임 수학(atanf·
// acos·cos·pow)을 DeterministicMath로 바꿔 호스트와 무관하게 같은 비트를 낸다. 기본 정책
// math::CurveMathPlatform(UE와 같은 비트)과 몇 ulp 다를 수 있다.
//
// 사용 예:
//   const float value = math::EvalCurveSegment<CurveMathDeterministic>(first, second, time);
//
// 스레드: 상태가 없어 어느 스레드에서나 부를 수 있다. 결정성은 기본 부동소수점 환경을 전제한다
// (CoreDataFoundations 2.1절) — 쿠킹 호출자가 platform::RunWithDeterministicFloatingPoint 안에서
// 부른다(atlas 굽기·커브 표 쿠킹이 그렇게 한다).
namespace rocks {

struct ROCKS_CURVES_API CurveMathDeterministic {
    // arctan(x)를 double로 계산해 float로 반올림한다(UE atanf 자리).
    static float Atan(float x) noexcept;

    // arccos·cos(라디안)다.
    static double Acos(double x) noexcept;
    static double Cos(double x) noexcept;

    // 세제곱근이다(0은 0, 음수는 −CubeRoot(−x) — 기본 정책과 같은 꼴, 지수는 UE와 같은 1.0 / 3.0).
    static double CubeRoot(double x) noexcept;

    // NaN이면 고정 quiet NaN(0x7FC00000), 아니면 value 그대로다(CPU마다 다른 NaN 비트를 쿠킹 byte에
    // 넣지 않는다).
    static float Canonical(float value) noexcept;
};

} // namespace rocks
