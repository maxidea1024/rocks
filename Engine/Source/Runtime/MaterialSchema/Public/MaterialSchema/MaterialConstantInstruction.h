#pragma once

#include <Base/Types.h>
#include <MaterialSchema/MaterialConstantOp.h>
#include <MaterialSchema/MaterialConstantScalar.h>
#include <Meta/Annotations.h>

// 상수 식 프로그램 명령 하나다(Docs/Design/MaterialSystem 9.4절, 12.8절 MS3d2). 피연산자는 앞 명령
// 번호다. 즉시값 부호화는 번역 IR과 같다(형식 버전 MaterialConstantProgramVersion): Constant는 성분
// 비트, Swizzle·Append·Convert는 IR 규칙 그대로, Parameter의 [3]은 artifact 파라미터 번호,
// Texture의 [3]은 artifact 텍스처 slot 번호, TextureSize의 [0]은 0 크기·1 texel 크기다.
//
// 사용 예:
//   for (const MaterialConstantInstruction& instruction : artifact.program) {
//       Evaluate(instruction);
//   }
namespace rocks::material {

// 명령 하나의 피연산자·즉시값 칸 수다(IR과 같다).
inline constexpr usize MaterialConstantOperands = 4;
inline constexpr usize MaterialConstantImmediates = 4;

ROCKS_STRUCT()
struct MaterialConstantInstruction {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() MaterialConstantOp op = MaterialConstantOp::Constant;
    ROCKS_PROPERTY() MaterialConstantScalar scalar = MaterialConstantScalar::Float;
    // 값의 성분 수다(1–4, Texture는 0).
    ROCKS_PROPERTY() uint8 components = 1;
    ROCKS_PROPERTY() uint8 operandCount = 0;
    ROCKS_PROPERTY() uint32 operands[MaterialConstantOperands] = { 0, 0, 0, 0 };
    ROCKS_PROPERTY() uint64 immediates[MaterialConstantImmediates] = { 0, 0, 0, 0 };
};

} // namespace rocks::material
