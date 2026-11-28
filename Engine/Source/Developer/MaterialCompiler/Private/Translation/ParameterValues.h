#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialCurveAtlasSnapshot.h>
#include <MaterialCompiler/MaterialIrParameter.h>
#include <MaterialCompiler/MaterialIrTexture.h>
#include <MaterialCompiler/MaterialIrType.h>
#include <MaterialCompiler/MaterialIrValue.h>
#include <MaterialSchema/MaterialParameterMetadata.h>

// 파라미터 노드가 함께 쓰는 값이다(Docs/Design/MaterialSystem 6.10절 MS2c2·MS2c5 —
// LowerParameters가 정의하고 CurveAtlasRowParameter(LowerTextures)도 쓴다).
namespace rocks::material::detail {

class LoweringContext;

// 스칼라 custom primitive data 문맥 값 이름이다(UE CustomPrimitiveData(…, MCT_Float)).
inline constexpr StringView CustomPrimitiveDataScalar = "CustomPrimitiveData1";

// 같은 정체가 다른 종류(수·텍스처 표 사이 포함)로 이미 쓰였을 때의 문구다(V8).
inline constexpr StringView ParameterTypeConflictMessage =
    "Parameter is used with another parameter type";

// 같은 정체가 다른 이름·기본값으로 이미 쓰였는지 보고 그렇다면 ParameterConflict를 진단한다(12.8절
// MS3d1 — UE r.Material.PedanticErrorChecksEnabled "has duplicates with different default values"를
// 늘 켠다). numeric은 수 파라미터(아니면 파라미터 텍스처 — defaultAsset을 비교), kind·defaults는 수
// 파라미터 값이다. 충돌이면 true(진단함)다. 오류: 할당 실패.
Result<bool> ParameterConflicts(LoweringContext& context, Guid identity,
                                const MaterialParameterMetadata& metadata, bool numeric,
                                IrParameterKind kind, const double (&defaults)[MaxIrComponents],
                                IrTextureKind textureKind, Guid defaultAsset) noexcept;

// 노드 속성 parameterId의 수 파라미터 읽기 값이다(6.10절). 기본값은 노드 값 hook과
// ResolveParameterDefaults로 정한다(12.8절 MS3d1 — 쿠킹 처리기와 같은 길). atlas는 커브 atlas 행
// 파라미터의 snapshot이다. 같은 정체가 다른 종류나 다른 기본값으로 이미 쓰였으면
// ParameterConflict를 진단하고 NoIrValue다(ParameterConflicts — 이름이 다른 것도). 출처를 함께
// 기록한다.
Result<IrValueId> NumericParameter(LoweringContext& context, IrParameterKind kind,
                                   const MaterialCurveAtlasSnapshot* atlas = nullptr) noexcept;

// custom primitive data 읽기다(UE CustomPrimitiveData — 노드 속성 useCustomPrimitiveData·
// primitiveDataIndex, UI 영역은 오류, 번호는 0–35). name은 문맥 값
// 이름(CustomPrimitiveData1·4)이다. 쓰지 않으면 used가 거짓이다(속성을 읽지 못해 진단했으면 used가
// 참이고 NoIrValue다).
Result<IrValueId> CustomPrimitiveData(LoweringContext& context, StringView name,
                                      bool& used) noexcept;

} // namespace rocks::material::detail
