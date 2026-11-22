// Rocks 셰이더 플랫폼 머리다(Docs/Design/ShaderSystem.ko.md 11.1절 SC10).
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
//
// 사용 예:
//   ROCKS_IMAGE_FORMAT(rgba8) RWTexture2D<float4> Output;
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
