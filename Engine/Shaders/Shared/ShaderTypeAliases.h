// C++·HLSL 공용 헤더가 쓰는 HLSL 타입 이름이다(Docs/Design/EngineShaders 3.3절, UE
// HLSLTypeAliases.h 역할). Shared/*.h는 C++와 HLSL 양쪽에서 컴파일된다.
//
// - HLSL 쪽(정의 조립이 ROCKS_SHADER_MODEL_MAJOR를 넣는 셰이더 컴파일): 아무것도 정의하지 않는다
//   (내장 타입이다).
// - C++ 쪽: rocks::shader::hlsl 이름공간에 float2·float4·uint4·float4x4 같은 이름을 ShaderCore
// 셰이더
//   정렬 타입(ShaderSystem 7.2절 5항)으로 둔다. 그래서 같은 구조체 선언이 양쪽에서 같은 배치를
//   낸다(2성분 8 byte 정렬, 3·4성분·행렬 16 byte 정렬). C++ 쪽은 ShaderCore 공개 헤더가 보이는
//   모듈에서만 include한다.
//
// 사용 예(공용 헤더):
//   #include "/Engine/Shared/ShaderTypeAliases.h"   // HLSL
//   ROCKS_SHADER_SHARED_BEGIN
//   struct LightRecord { float4 position; float4 color; uint4 flags; };
//   ROCKS_SHADER_SHARED_END
#pragma once

#if defined(ROCKS_SHADER_MODEL_MAJOR)

#define ROCKS_SHADER_SHARED_BEGIN
#define ROCKS_SHADER_SHARED_END

#else

#include <Base/Types.h>
#include <ShaderCore/ShaderParameterTypes.h>

namespace rocks::shader::hlsl {

using uint = rocks::uint32;
using float2 = rocks::shader::ShaderFloat2;
using float3 = rocks::shader::ShaderFloat3;
using float4 = rocks::shader::ShaderFloat4;
using int2 = rocks::shader::ShaderInt2;
using int3 = rocks::shader::ShaderInt3;
using int4 = rocks::shader::ShaderInt4;
using uint2 = rocks::shader::ShaderUint2;
using uint3 = rocks::shader::ShaderUint3;
using uint4 = rocks::shader::ShaderUint4;
using float4x4 = rocks::shader::ShaderFloat4x4;

} // namespace rocks::shader::hlsl

#define ROCKS_SHADER_SHARED_BEGIN namespace rocks::shader::hlsl {
#define ROCKS_SHADER_SHARED_END }

#endif
