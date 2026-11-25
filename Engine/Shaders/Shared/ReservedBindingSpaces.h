// Rocks가 예약한 D3D register space·SPIR-V descriptor set 번호다(Docs/Design/EngineShaders 3.3절,
// ShaderSystem 7.2절 2·12항, UE HLSLReservedSpaces.h 역할). Platform.rsh의 ROCKS_ROOT_CONSTANTS·
// RocksDebugBuffer 전개와 Graphics backend의 root signature·pipeline layout이 같은 번호를 쓴다.
// 셰이더 원문은 register·[[vk::binding]]을 적지 않으므로(7.2절 1항) 이 번호는 플랫폼
// 머리·backend·워커만 쓴다. 값을 바꾸면 Platform.rsh와 backend 상수를 함께 바꾼다.
#pragma once

#include "ShaderStaticAssert.h"

#if defined(ROCKS_SHADER_MODEL_MAJOR)
#define ROCKS_RESERVED_SPACE_CONSTANT static const uint
#else
#include <Base/Types.h>
namespace rocks::shader {
#define ROCKS_RESERVED_SPACE_CONSTANT inline constexpr rocks::uint32
#endif

// D3D12 루트 상수(그리기별 상수)의 register space다(b0, space3).
ROCKS_RESERVED_SPACE_CONSTANT ReservedRootConstantsSpace = 3;
// D3D12 루트 상수의 register 번호다.
ROCKS_RESERVED_SPACE_CONSTANT ReservedRootConstantsRegister = 0;
// D3D11 루트 상수 cbuffer의 register 번호다(b13 — 앞 13개는 일반 cbuffer).
ROCKS_RESERVED_SPACE_CONSTANT ReservedD3D11RootConstantsRegister = 13;
// 개발 전용 PS 진단 버퍼의 D3D12 register space다(u0, space999).
ROCKS_RESERVED_SPACE_CONSTANT ReservedDebugBufferSpace = 999;
// 개발 전용 PS 진단 버퍼의 SPIR-V descriptor set이다(set 3, binding 0).
ROCKS_RESERVED_SPACE_CONSTANT ReservedDebugBufferDescriptorSet = 3;

ROCKS_SHADER_STATIC_ASSERT(ReservedRootConstantsSpace != ReservedDebugBufferSpace,
                           "reserved register spaces must differ");

#undef ROCKS_RESERVED_SPACE_CONSTANT

#if !defined(ROCKS_SHADER_MODEL_MAJOR)
} // namespace rocks::shader
#endif
