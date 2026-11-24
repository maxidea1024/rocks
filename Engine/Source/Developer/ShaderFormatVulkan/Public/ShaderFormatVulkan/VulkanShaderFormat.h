#pragma once

#include <ShaderCompilerCommon/ShaderFormatBackend.h>

// Vulkan 형식 처리기다(Docs/Design/ShaderSystem 2절, 11.1절 SC1 — 워커가 링크한다).
// VULKAN_SM5는 DXC → SPIR-V(target env는 형식 정의 compiler.spirvTargetEnv)이고 spirv-val로
// 검증한다. SH2의 재배치·리플렉션 전 산출물이다(processor.version을 SH2가 올린다). 개발용 컴파일 훅
// (VulkanShaderCompiler)은 SH2까지 같은 모듈에 남는다.
//
// DXC 인자: -E 엔트리, -T <단계>_<셰이더 모델>, -HV 2021|2018, -O0–-O3, -Zpr, -WX, -spirv,
// -fspv-target-env=vulkan1.1, (형식 정의 dxPositionW면) -fvk-use-dx-position-w. 결과 값 0(Code)은
// SPIR-V word byte다.
namespace rocks::shader {

// 처리기 버전이다(형식 정의 processor.version).
inline constexpr uint32 VulkanShaderProcessorVersion = 3;

class ROCKS_SHADERFORMATVULKAN_API VulkanShaderFormatBackend final : public ShaderFormatBackend {
public:
    ShaderFormatModule Module() const noexcept override {
        return ShaderFormatModule::Vulkan;
    }

    uint32 ProcessorVersion() const noexcept override {
        return VulkanShaderProcessorVersion;
    }

    void Compile(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                 const ShaderBackendContext& context,
                 ShaderWorkerJobResult& result) noexcept override;
};

} // namespace rocks::shader
