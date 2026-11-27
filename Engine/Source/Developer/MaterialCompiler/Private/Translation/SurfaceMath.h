#pragma once

#include <Core/Move.h>
#include <Core/Result.h>
#include <MaterialCompiler/MaterialIrOp.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 표면 접기·내보내기 식을 IR로 펼치는 도우미다(Docs/Design/MaterialSystem 6.11절 MS2d2). 연산마다
// Result를 확인하지 않도록 첫 시스템 실패를 기억하고(Status), 실패 뒤의 연산은 NoIrValue를
// 돌려준다. 피연산자에 NoIrValue가 있으면 결과도 NoIrValue다(LoweringContext 규칙과 같다). 실수
// 연산은 Float 규칙(NumberRule::FloatOnly), UInt 연산은 정수를 그대로 둔다.
//
// 사용 예:
//   SurfaceMath m(context);
//   const IrValueId metal = m.Saturate(m.Div(m.Sub(m.Max3(f0), m.Scalar(0.08f)), m.Scalar(0.32f)));
//   if (auto status = m.Status(); !status) { return status; }
namespace rocks::material::detail {

class LoweringContext;

class SurfaceMath {
public:
    explicit SurfaceMath(LoweringContext& context) noexcept : context_(context) {
    }

    SurfaceMath(const SurfaceMath&) = delete;
    SurfaceMath& operator=(const SurfaceMath&) = delete;

    // 첫 시스템 실패다(없으면 성공).
    Result<void> Status() const noexcept {
        return failed_ ? Result<void>::Failure(error_) : Result<void>::Success();
    }

    LoweringContext& Context() noexcept {
        return context_;
    }

    // 상수다.
    IrValueId Scalar(float value) noexcept;
    // 같은 값의 float3다.
    IrValueId Vector3(float value) noexcept;
    IrValueId Vector3(float x, float y, float z) noexcept;
    IrValueId UInt(uint32 value) noexcept;
    IrValueId Bool(bool value) noexcept;

    // 실수 연산이다(폭: 같거나 한쪽 스칼라).
    IrValueId Add(IrValueId left, IrValueId right) noexcept;
    IrValueId Sub(IrValueId left, IrValueId right) noexcept;
    IrValueId Mul(IrValueId left, IrValueId right) noexcept;
    IrValueId Div(IrValueId left, IrValueId right) noexcept;
    IrValueId Min(IrValueId left, IrValueId right) noexcept;
    IrValueId Max(IrValueId left, IrValueId right) noexcept;
    IrValueId Pow(IrValueId base, IrValueId exponent) noexcept;
    IrValueId Lerp(IrValueId from, IrValueId to, IrValueId alpha) noexcept;
    IrValueId Clamp(IrValueId value, IrValueId low, IrValueId high) noexcept;
    IrValueId Saturate(IrValueId value) noexcept;
    IrValueId Sqrt(IrValueId value) noexcept;
    IrValueId Exp(IrValueId value) noexcept;
    IrValueId Log(IrValueId value) noexcept;
    IrValueId Abs(IrValueId value) noexcept;
    IrValueId Cos(IrValueId value) noexcept;
    IrValueId Dot(IrValueId left, IrValueId right) noexcept;
    IrValueId Normalize(IrValueId value) noexcept;
    IrValueId Smoothstep(IrValueId low, IrValueId high, IrValueId value) noexcept;
    // 1 − value다.
    IrValueId OneMinus(IrValueId value) noexcept;
    // 성분 하나다.
    IrValueId Component(IrValueId value, uint8 index) noexcept;
    // 세 성분의 최대·최소다(UE max3·min3).
    IrValueId Max3(IrValueId value) noexcept;
    IrValueId Min3(IrValueId value) noexcept;
    // 세 성분 평균이다(UE dot(v, 0.33) — 무게는 UE 값 그대로).
    IrValueId Average3(IrValueId value) noexcept;
    // x⁵다(UE Pow5).
    IrValueId Pow5(IrValueId value) noexcept;
    // 스칼라를 float3로 복제한다(이미 float3면 그대로).
    IrValueId Splat3(IrValueId value) noexcept;

    // 비교다(Bool 스칼라 — 피연산자는 스칼라).
    IrValueId Greater(IrValueId left, IrValueId right) noexcept;
    IrValueId Less(IrValueId left, IrValueId right) noexcept;
    IrValueId NotEqual(IrValueId left, IrValueId right) noexcept;
    // UInt 같음이다.
    IrValueId Equal(IrValueId left, IrValueId right) noexcept;
    IrValueId Or(IrValueId left, IrValueId right) noexcept;
    IrValueId And(IrValueId left, IrValueId right) noexcept;
    IrValueId Not(IrValueId value) noexcept;
    // UInt 최대·최소다(SSS 종류 병합).
    IrValueId MaxUInt(IrValueId left, IrValueId right) noexcept;
    IrValueId MinUInt(IrValueId left, IrValueId right) noexcept;

    // condition ? whenTrue : whenFalse다(condition은 Bool 스칼라). condition이 상수면 고른 쪽을
    // 그대로 돌려준다(표지가 상수인 흔한 경우에 식이 남지 않는다).
    IrValueId Select(IrValueId condition, IrValueId whenTrue, IrValueId whenFalse) noexcept;

    // 값이 Bool 상수면 그 값이다.
    bool ConstantBool(IrValueId value, bool& out) const noexcept;

    // 바깥에서 만든 값을 받는다(실패면 첫 실패로 기억하고 NoIrValue).
    IrValueId Track(Result<IrValueId> value) noexcept {
        return Check(Move(value));
    }

private:
    IrValueId Check(Result<IrValueId> value) noexcept;
    IrValueId Unary(IrOp op, IrValueId value) noexcept;
    IrValueId Binary(IrOp op, IrValueId left, IrValueId right) noexcept;

    LoweringContext& context_;
    ErrorCode error_ = ErrorCode::None;
    bool failed_ = false;
};

} // namespace rocks::material::detail
