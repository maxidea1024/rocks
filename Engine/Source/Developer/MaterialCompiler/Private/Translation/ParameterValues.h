#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialCurveAtlasSnapshot.h>
#include <MaterialCompiler/MaterialIrParameter.h>
#include <MaterialCompiler/MaterialIrType.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 파라미터 노드가 함께 쓰는 값이다(Docs/Design/MaterialSystem 6.10절 MS2c2·MS2c5 —
// LowerParameters가 정의하고 CurveAtlasRowParameter(LowerTextures)도 쓴다).
namespace rocks::material::detail {

class LoweringContext;

// 스칼라 custom primitive data 문맥 값 이름이다(UE CustomPrimitiveData(…, MCT_Float)).
inline constexpr StringView CustomPrimitiveDataScalar = "CustomPrimitiveData1";

// 노드 속성 parameterId의 수 파라미터 읽기 값이다(6.10절). 기본값은 노드 값 hook과
// ResolveParameterDefaults로 정한다(12.8절 MS3d1 — 쿠킹 처리기와 같은 길). atlas는 커브 atlas 행
// 파라미터의 snapshot이다. 같은 정체가 다른 종류나 다른 기본값으로 이미 쓰였으면
// ParameterConflict를 진단하고 NoIrValue다(UE r.Material.PedanticErrorChecksEnabled 검사 —
// Rocks는 늘 켠다). 출처를 함께 기록한다.
Result<IrValueId> NumericParameter(LoweringContext& context, IrParameterKind kind,
                                   const MaterialCurveAtlasSnapshot* atlas = nullptr) noexcept;

// custom primitive data 읽기다(UE CustomPrimitiveData — 노드 속성 useCustomPrimitiveData·
// primitiveDataIndex, UI 영역은 오류, 번호는 0–35). name은 문맥 값
// 이름(CustomPrimitiveData1·4)이다. 쓰지 않으면 used가 거짓이다(속성을 읽지 못해 진단했으면 used가
// 참이고 NoIrValue다).
Result<IrValueId> CustomPrimitiveData(LoweringContext& context, StringView name,
                                      bool& used) noexcept;

} // namespace rocks::material::detail
