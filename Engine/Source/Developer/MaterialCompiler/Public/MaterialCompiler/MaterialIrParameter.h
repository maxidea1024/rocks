#pragma once

#include <Base/Types.h>
#include <Base/Guid.h>
#include <MaterialCompiler/MaterialIrType.h>

// 머티리얼 IR의 수 파라미터 하나다(Docs/Design/MaterialSystem 6.10절 MS2c2, 9.2–9.4절). IR 값
// Parameter가 가리키고, 모듈은 진입점에서 닿는 파라미터만 ParameterId 순으로 담는다(상수 식
// 프로그램과 인스턴스 값 배열의 slot 순서).
//
// 사용 예:
//   for (const IrParameter& parameter : module.Parameters()) {
//       Use(parameter.id, parameter.defaults);
//   }
namespace rocks::material {

// 수 파라미터 종류다(UE EMaterialParameterType의 수 종류).
enum class IrParameterKind : uint8 {
    Scalar = 0,
    Vector = 1,
    DoubleVector = 2
};

struct IrParameter {
    // 파라미터 노드의 ParameterId다(9.2절 — 함수 scope는 MS2e가 더한다).
    Guid id;
    IrParameterKind kind = IrParameterKind::Scalar;
    // 값 타입이다(Scalar Float1, Vector Float4, DoubleVector Double4).
    IrType type = IrFloat(1);
    // 머티리얼 기본값이다(성분 수만큼 — float도 double로 둔다).
    double defaults[MaxIrComponents] = {0.0, 0.0, 0.0, 0.0};
};

} // namespace rocks::material
