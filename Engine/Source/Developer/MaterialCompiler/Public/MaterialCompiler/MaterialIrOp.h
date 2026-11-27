#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 머티리얼 IR 연산이다(Docs/Design/MaterialSystem 6.2·6.8·6.9절). 산술·초월 연산은 UE Operator
// 노드의 연산 전부(UE EMaterialExpressionOperatorKind —
// RE/Public/Materials/MaterialExpressionOperator.h:14–100)와 같은 뜻이고, 그 밖에 구조
// 연산(상수·복제·성분 선택·이어 붙이기·변환·선택·분기)·미분·문맥 값을 둔다. 값 번호·hash에
// 들어가므로 순서를 바꾸지 않고 끝에만 더한다.
namespace rocks::material {

enum class IrOp : uint8 {
    // 구조 연산.
    Constant = 0,
    // 스칼라를 성분 수만큼 복제한다(즉시값 없음 — 결과 타입이 성분 수를 준다).
    Splat = 1,
    // 성분 선택이다. 즉시값 [0]에 성분 번호를 2 bit씩(성분 0이 하위) 담는다.
    Swizzle = 2,
    Append = 3,
    // 스칼라 종류 변환이다(결과 타입이 대상 종류).
    Convert = 4,
    // 조건 선택이다(두 값을 모두 계산한다).
    Select = 5,
    // 동적 분기다(UE IfThenElse — 원문은 [branch] if).
    Branch = 6,

    // 단항(UE Operator 단항).
    BitwiseNot = 7,
    Negate = 8,
    Not = 9,
    Abs = 10,
    ACos = 11,
    ACosFast = 12,
    ACosh = 13,
    ASin = 14,
    ASinFast = 15,
    ASinh = 16,
    ATan = 17,
    ATanFast = 18,
    ATanh = 19,
    Ceil = 20,
    Cos = 21,
    Cosh = 22,
    Exponential = 23,
    Exponential2 = 24,
    Floor = 25,
    Frac = 26,
    IsFinite = 27,
    IsInf = 28,
    IsNan = 29,
    Length = 30,
    Logarithm = 31,
    Logarithm10 = 32,
    Logarithm2 = 33,
    WorldPositionTile = 34,
    Reciprocal = 35,
    Round = 36,
    Rsqrt = 37,
    Saturate = 38,
    Sign = 39,
    Sin = 40,
    Sinh = 41,
    Sqrt = 42,
    Tan = 43,
    Tanh = 44,
    Transpose = 45,
    Truncate = 46,
    Normalize = 47,
    Ddx = 48,
    Ddy = 49,

    // 이항(UE Operator 이항).
    Equals = 50,
    GreaterThan = 51,
    GreaterThanOrEquals = 52,
    LessThan = 53,
    LessThanOrEquals = 54,
    NotEquals = 55,
    And = 56,
    Or = 57,
    Add = 58,
    Subtract = 59,
    Multiply = 60,
    MatrixMultiply = 61,
    Divide = 62,
    Modulo = 63,
    BitwiseAnd = 64,
    BitwiseOr = 65,
    BitwiseXor = 66,
    BitShiftLeft = 67,
    BitShiftRight = 68,
    ATan2 = 69,
    ATan2Fast = 70,
    Cross = 71,
    Distance = 72,
    Dot = 73,
    Fmod = 74,
    Max = 75,
    Min = 76,
    Pow = 77,
    Step = 78,

    // 삼항(UE Operator 삼항).
    Clamp = 79,
    Lerp = 80,
    Smoothstep = 81,

    // 문맥 값이다(6.9절). 즉시값 [0]은 문맥 값 표 번호, [1]은 IrExternVariant이고, 피연산자는
    // 함수형 값의 인자(0–3개)다.
    Extern = 82,

    // 정점 → 픽셀 보간 값이다(6.9절 MS2b4a — VertexInterpolator). 피연산자는 정점 문맥 값, 즉시값
    // [0]은 모듈이 정한 첫 성분 번호(성분 단위 오프셋)다.
    Interpolate = 83,

    // 늦은 진단의 자리 값이다(6.10절 MS2c1 — TODO 노드·모르는 노드). 즉시값 [0]은 종류
    // (IrPendingKind), [1]은 선행 ID 번호, [2]·[3]은 노드 NodeId(high·low)다.
    Pending = 84,

    // 수 파라미터 읽기다(6.10절 MS2c2). 즉시값 [0]·[1]은 ParameterId(high·low), [2]는
    // IrParameterKind, [3]은 파라미터 표 번호(빌더 표 → 모듈이 ParameterId 순으로 다시 매긴다).
    Parameter = 85,
    // 파라미터 컬렉션 벡터 하나다(6.10절 MS2c2). 즉시값 [0]·[1]은 컬렉션 asset id, [2]는 벡터 번호,
    // [3]은 그 컬렉션의 벡터 수다.
    CollectionValue = 86,

    // 텍스처 하나다(6.10절 MS2c3). 즉시값 [0]·[1]은 asset id 또는 ParameterId, [2]는 출처
    // (IrTextureSource) | 샘플 종류 << 8, [3]은 텍스처 표 번호(모듈이 (출처, id) 순으로 다시
    // 매긴다).
    Texture = 87,
    // 텍스처 샘플이다(6.10절 MS2c3). 피연산자 [텍스처, uv, mip 값 또는 ddx, ddy]. 즉시값 [0]은 샘플
    // 종류, [1]은 IrTextureMipMode, [2]는 IrSamplerSource, [3]은 gather 채널 | decal << 4. 결과
    // Float4.
    TextureSample = 88,
    // Double uv를 주소 모드대로 Float로 낮춘다(6.10절 MS2c3 — UE WSApplyAddressMode). 즉시값 [0]은
    // 성분마다 2 bit 주소 모드(0 Clamp, 1 Wrap, 2 Mirror).
    AddressDemote = 89,
    // 텍스처 slot의 논리 크기다(6.10절 MS2c3b — UE TextureProperty). 피연산자 텍스처, 즉시값 [0]은
    // 0 크기·1 texel 크기. 빈도 Uniform(상수 식 프로그램 명령).
    TextureSize = 90,
    // 표면 값이다(6.11절 MS2d1). 즉시값 [0]은 번역기 표면 기록 번호다. 모듈에는 남지 않는다(출력
    // 노드가 접어 내보낸다).
    SurfaceRef = 91,
    // 속성 집합 값이다(6.12절 MS2e1). 즉시값 [0]은 번역기 속성 기록 번호다. 모듈에는 남지 않는다
    // (쓰는 곳이 칸을 읽는다).
    AttributesRef = 92,
    // 라이브러리 함수 호출이다(6.13절 MS2f3). 즉시값 [0]은 IrLibraryFunction, [1]–[3]은 그 함수의
    // 정적 인자 묶음(MaterialIrLibrary.h), 피연산자는 실행 값이다. 접지 않는다.
    Library = 93
};

// 연산 수다(마지막 값 + 1).
inline constexpr uint32 IrOpCount = 94;

// Pending 자리 값의 종류다.
enum class IrPendingKind : uint8 {
    // 카탈로그에 없는 노드(V12).
    UnknownNode = 0,
    // 선행이 없는 TODO 노드(PrerequisiteMissing).
    PrerequisiteMissing = 1
};

// 연산이 받는 피연산자 수다(Constant 0, 단항 1, 이항 2, 삼항·선택·분기 3, Append 2). 인자 수가
// 값마다 다른 Extern은 0이다(값의 operandCount를 본다).
ROCKS_MATERIALCOMPILER_API uint8 IrOpArity(IrOp op) noexcept;

// 진단·dump용 영어 이름이다. 범위 밖이면 "Invalid"다.
ROCKS_MATERIALCOMPILER_API StringView IrOpName(IrOp op) noexcept;

} // namespace rocks::material
