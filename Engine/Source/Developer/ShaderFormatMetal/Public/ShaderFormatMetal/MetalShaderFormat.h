#pragma once

#include <ShaderCompilerCommon/ShaderFormatBackend.h>

// Metal 형식 처리기다(Docs/Design/ShaderSystem 2절, 11.1절 SC1·SH1e E6 — 워커가 링크한다).
// METAL_SM5는 DXC → SPIR-V(vulkan1.1, 형식 dxPositionW) → spirv-val → SPIRV-Cross MSL(형식
// mslVersion, macOS, typed buffer는 texture_buffer) → `metal -c -std=macos-metal<버전>
// -mmacosx-version-min=<최소 OS> -o job.air job.metal` → `metallib -o job.metallib job.air`이다.
// 결과 값 0(Code)은 metallib byte다. 임시 파일은 문맥의 scratchDirectory에 job마다 같은 이름으로
// 쓴다.
//
// MetalToolchain(system 도구, ShaderToolLoader LocateMetalToolchain)이 없으면 MSL을 만들기 전에
// UnsupportedCapability다(MSL만 내고 성공하지 않는다). 메인 프로세스 키 계산이 system 도구 지문을
// 요구하므로 보통은 그 전에 같은 상태가 된다.
//
// 사용 예(워커):
//   MetalShaderFormatBackend metal;
//   ShaderBackendContext context;
//   context.tools = &tools;
//   context.metal = &toolchain;
//   context.scratchDirectory = scratch;
//   metal.Compile(format, job, context, result);
namespace rocks::shader {

// 처리기 버전이다(형식 정의 processor.version).
inline constexpr uint32 MetalShaderProcessorVersion = 1;

class ROCKS_SHADERFORMATMETAL_API MetalShaderFormatBackend final : public ShaderFormatBackend {
public:
    ShaderFormatModule Module() const noexcept override {
        return ShaderFormatModule::Metal;
    }

    uint32 ProcessorVersion() const noexcept override {
        return MetalShaderProcessorVersion;
    }

    void Compile(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                 const ShaderBackendContext& context,
                 ShaderWorkerJobResult& result) noexcept override;
};

// MSL 버전의 macOS 최소 버전(major)이다(UE 5.8 MetalShaderCompiler.cpp 대응: 2.4 → 12, 3.0 → 13,
// 3.1 → 14). 모르는 버전이면 0이다.
ROCKS_SHADERFORMATMETAL_API uint32 MetalMinimumMacOSVersion(ShaderVersion msl) noexcept;

} // namespace rocks::shader
