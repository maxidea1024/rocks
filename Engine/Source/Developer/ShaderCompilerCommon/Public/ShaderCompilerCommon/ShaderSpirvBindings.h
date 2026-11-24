#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCompilerCommon/ShaderGlesBindings.h>
#include <ShaderCompilerCommon/ShaderReflection.h>
#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderParameterLayout.h>

// SPIR-V 경로 형식의 바인딩 정규화다(Docs/Design/ShaderSystem 16.1절
// SSD30·SSD33·SSD34·SSD36·SSD39·SSD40). 셰이더 워커의 VULKAN_SM5·METAL_SM5 처리기가 함께
// 쓴다(도구를 열지 않는다 — SPIRV-Reflect는 소스 라이브러리다).
//
// - BuildSpirvBindingLayout: 고쳐 쓰기 전 DXC 출력(raw — -fspv-reflect의 시맨틱·user type)과 고쳐
// 쓴 모듈
//   (rewritten — set·binding)을 SPIRV-Reflect로 읽어 공용 규칙(NormalizeShaderReflection)을 거친 값
//   1 byte를 만든다. Metal 형식이면 slot을 Metal 인덱스로 바꾸고 SPIRV-Cross MSL에 줄 배정을 함께
//   낸다.
// - AssignMetalBindingIndices: Metal 인덱스 배정(7.2절 4항 Metal, SSD39 — 순수 함수).
// - CheckStorageImageFormats: 전처리 결과에서 storage image 선언마다 [[vk::image_format(…)]] 지정이
// 있는지
//   본다(SSD40).
namespace rocks::shader {

// Metal 자원 하나의 배정이다(SPIRV-Cross spvc_msl_resource_binding — set·binding은 고쳐 쓴 SPIR-V
// 위치).
struct MetalResourceBinding {
    uint32 set = 0;
    uint32 binding = 0;
    uint32 buffer = 0;
    uint32 texture = 0;
    uint32 sampler = 0;
};

// 배정이 없음 표지와 push constant set 표지(SPIRV-Cross SPVC_MSL_PUSH_CONSTANT_DESC_SET과 같은
// 값)다.
inline constexpr uint32 NoMetalIndex = NumericLimits<uint32>::Max();
inline constexpr uint32 MetalPushConstantSet = NumericLimits<uint32>::Max();
// 루트 상수 buffer(0)·PS 진단 buffer(22)·단계별 buffer 인덱스 상한(VS·PS 21, CS 30)이다(7.2절
// 4·12항).
inline constexpr uint32 MetalRootConstantsBuffer = 0;
inline constexpr uint32 MetalDebugBuffer = 22;
inline constexpr uint32 MetalGraphicsMaxBuffer = 21;
inline constexpr uint32 MetalComputeMaxBuffer = 30;
// PS 진단 버퍼의 SPIR-V set(16.1절 SSD34)이다.
inline constexpr uint32 SpirvDebugBufferSet = 3;

// layout(slot = Vulkan binding)의 자원에 Metal 인덱스를 정하고 slot을 그 인덱스로 바꾼다(종류 공간
// — 상수 버퍼·구조화 버퍼는 buffer, SRV 텍스처·storage image는 texture, 샘플러는 sampler). out에는
// 루트 상수(push constant, rootConstantBytes > 0일 때)와 자원마다 배정 하나다. 구조화 버퍼가 있으면
// sizeIndex에 크기 side table buffer 인덱스, 없으면 NoMetalIndex. 단계 buffer 상한을 넘으면 false와
// diagnostics다.
ROCKS_SHADERCOMPILERCOMMON_API bool AssignMetalBindingIndices(ShaderBindingLayout& layout,
                                                              Array<MetalResourceBinding>& out,
                                                              uint32& sizeIndex,
                                                              String& diagnostics) noexcept;

struct SpirvBindingRequest {
    const ShaderFormatDefinition* format = nullptr;
    uint32 stage = 0;
    uint32 requiredFeatures = 0;
    Span<const ShaderParameterLayout> layouts;
    Span<const uint32> raw;
    Span<const uint32> rewritten;
    // (텍스처, 샘플러) 사용 쌍(SPIRV-Cross 결합 sampler — 이름은 호출자 소유)이다. hasUses가
    // 거짓이면 쓰지 않는다.
    Span<const ShaderSamplerUse> uses;
    bool hasUses = false;
};

struct SpirvBindingResult {
    explicit SpirvBindingResult(Allocator& allocator) noexcept
        : bindingLayout(allocator), metalBindings(allocator), gles(allocator) {
    }

    // 값 1 byte다.
    Array<uint8> bindingLayout;
    // Metal 형식의 배정(그 밖은 비어 있다)이다.
    Array<MetalResourceBinding> metalBindings;
    uint32 metalBufferSizeIndex = NoMetalIndex;
    // OpenGL 형식의 GLES 계획(16.1절 SSD42 — 그 밖은 비어 있다)이다.
    GlesPlan gles;
};

// 위 설명대로 값 1을 만든다. 결과: Success, CompileError(규칙 위반·Metal 상한), ToolError(리플렉션
// 실패).
ROCKS_SHADERCOMPILERCOMMON_API ShaderWorkerJobStatus BuildSpirvBindingLayout(
    const SpirvBindingRequest& request, SpirvBindingResult& out, String& diagnostics) noexcept;

// source(전처리 결과)의 storage image 선언(RWTexture1D·RWTexture2D·RWTexture3D·RWTexture1DArray·
// RWTexture2DArray·RWBuffer)마다 바로 앞에 [[vk::image_format(…)]]이 있는지 본다(SSD40 — SPIR-V
// 형식은 ROCKS_IMAGE_FORMAT이 그렇게 전개한다). 없으면 false와 diagnostics(첫 위반의 타입 이름)다.
ROCKS_SHADERCOMPILERCOMMON_API bool CheckStorageImageFormats(StringView source,
                                                             String& diagnostics) noexcept;

} // namespace rocks::shader
