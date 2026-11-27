#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 좌표 기본값 도우미다(Docs/Design/MaterialSystem 6.9절 TexCoord 규칙).
namespace rocks::material::detail {

class LoweringContext;

// TexCoord(index) 문맥 값이다(UInt 상수 인자).
Result<IrValueId> TexCoord(LoweringContext& context, uint32 index) noexcept;

// Coordinate 입력이 연결되면 그 값, 아니면 TexCoord(constCoordinate 속성)다(UE Panner·Rotator·
// BumpOffset).
Result<IrValueId> CoordinateOrTexCoord(LoweringContext& context) noexcept;

} // namespace rocks::material::detail
