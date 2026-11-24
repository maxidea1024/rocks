// Rocks 셰이더 플랫폼 머리다(Docs/Design/ShaderSystem 11.1절 SC10).
//
// 전처리기가 모든 엔트리 원문 앞에 강제로 include한다(작성자는 쓰지 않는다). 형식·컴파일러 값은
// 메인 프로세스의 정의 조립(ShaderPreprocessDefines)이 넣은 정의에서만 얻는다. 컴파일러 내장 이름
// (__SHADER_TARGET_MAJOR·__spirv__ 등)은 전처리 조건에 쓸 수 없다(SC3).
//
// 정의 조립이 넣는 값:
// - ROCKS_FORMAT_<형식 id> = 1
// - ROCKS_BACKEND_<D3D12|D3D11|VULKAN|METAL|GLES> 하나 = 1
// - ROCKS_COMPILER_DXC 또는 ROCKS_COMPILER_FXC = 1
// - ROCKS_SHADER_MODEL_MAJOR·ROCKS_SHADER_MODEL_MINOR
// - ROCKS_HLSL2021 = 0·1
// - ROCKS_VERTEX_SHADER·ROCKS_PIXEL_SHADER·ROCKS_COMPUTE_SHADER 하나 = 1
//
// 이 파일이 만드는 값:
// - ROCKS_SPIRV: DXC → SPIR-V를 거치는 형식(Vulkan·Metal·GLES)이면 1, D3D 형식이면 0. 형식 정의의
//   compiler.imageFormat과 같은 규칙이다(SPIR-V 경로는 VkImageFormat, D3D 경로는 Empty — 정의
//   읽기가 강제한다).
// - ROCKS_IMAGE_FORMAT(Format): storage image의 형식 지정(7.2절 1항). Format은 DXC
//   [[vk::image_format]] 철자(rgba8·rgba8snorm·rgba16f·r11g11b10f·r32f·r32ui·unknown 등)다. SPIR-V
//   형식은 [[vk::image_format("Format")]]으로, D3D 형식은 빈 것으로 전개한다(DXIL은 이 attribute를
//   무시하며 경고를 내므로 남기지 않는다 — SH0 확인).
// - ROCKS_ROOT_CONSTANTS(Type, Name): 그리기별 상수(루트 상수, 7.2절 2항·16.1절 SSD32). 셰이더 하나에
//   하나다. D3D12는 register(b0, space3)의 ConstantBuffer<Type>, D3D11은 register(b13)의 cbuffer,
//   SPIR-V 형식은 push constant다. Type 크기는 128 byte 이하·16 byte 배수다(워커가 검사한다).
// - RocksDebugBuffer: 개발 전용 PS job에서 정의 조립이 ROCKS_DEBUG_BUFFER = 1을 넣으면 예약 위치의
//   RWByteAddressBuffer로 선언된다(7.2절 12항, 16.1절 SSD34 — D3D12 register(u0, space999), SPIR-V
//   형식 set 3·binding 0). 진단 버퍼 기능이 없는 형식(D3D11·GLES)에서는 오류다.
//
// 사용 예:
//   ROCKS_IMAGE_FORMAT(rgba8) RWTexture2D<float4> Output;
//   struct DrawConstants { float4x4 transform; float4 color; };
//   ROCKS_ROOT_CONSTANTS(DrawConstants, Draw);   // Draw.color
//   #if ROCKS_SHADER_MODEL_MAJOR >= 6
//   ...
//   #endif
#pragma once

#if !defined(ROCKS_SHADER_MODEL_MAJOR) || !defined(ROCKS_SHADER_MODEL_MINOR) || !defined(ROCKS_HLSL2021)
#error "ROCKS_SHADER_MODEL_MAJOR, ROCKS_SHADER_MODEL_MINOR and ROCKS_HLSL2021 must be defined by the shader compiler"
#endif

#if defined(ROCKS_BACKEND_D3D12) + defined(ROCKS_BACKEND_D3D11) + defined(ROCKS_BACKEND_VULKAN) + defined(ROCKS_BACKEND_METAL) + defined(ROCKS_BACKEND_GLES) != 1
#error "exactly one ROCKS_BACKEND_* must be defined by the shader compiler"
#endif

#if defined(ROCKS_COMPILER_DXC) + defined(ROCKS_COMPILER_FXC) != 1
#error "exactly one ROCKS_COMPILER_* must be defined by the shader compiler"
#endif

#if defined(ROCKS_VERTEX_SHADER) + defined(ROCKS_PIXEL_SHADER) + defined(ROCKS_COMPUTE_SHADER) != 1
#error "exactly one shader stage define must be set by the shader compiler"
#endif

#if defined(ROCKS_BACKEND_VULKAN) || defined(ROCKS_BACKEND_METAL) || defined(ROCKS_BACKEND_GLES)
#define ROCKS_SPIRV 1
#else
#define ROCKS_SPIRV 0
#endif

#if ROCKS_SPIRV
#define ROCKS_IMAGE_FORMAT(Format) [[vk::image_format(#Format)]]
#else
#define ROCKS_IMAGE_FORMAT(Format)
#endif

#if ROCKS_SPIRV
#define ROCKS_ROOT_CONSTANTS(Type, Name) [[vk::push_constant]] Type Name
#elif defined(ROCKS_BACKEND_D3D12)
#define ROCKS_ROOT_CONSTANTS(Type, Name) ConstantBuffer<Type> Name : register(b0, space3)
#else
#define ROCKS_ROOT_CONSTANTS(Type, Name) cbuffer Name##RootConstants : register(b13) { Type Name; }
#endif

#if defined(ROCKS_DEBUG_BUFFER)
#if defined(ROCKS_BACKEND_D3D11) || defined(ROCKS_BACKEND_GLES)
#error "ROCKS_DEBUG_BUFFER needs the debugPixelUav format feature (not on D3D11 and GLES)"
#elif ROCKS_SPIRV
[[vk::binding(0, 3)]] RWByteAddressBuffer RocksDebugBuffer;
#else
RWByteAddressBuffer RocksDebugBuffer : register(u0, space999);
#endif
#endif
