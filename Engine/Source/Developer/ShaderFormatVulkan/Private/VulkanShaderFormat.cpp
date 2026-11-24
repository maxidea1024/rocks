#include <ShaderFormatVulkan/VulkanShaderFormat.h>

#include "SpirvReflection.h"

#include <Platform/Memory.h>
#include <ShaderFormatVulkan/SpirvRewrite.h>
#include <ShaderToolLoader/ShaderToolCalls.h>

// Vulkan 형식 처리기(Docs/Design/ShaderSystem 11.1절 SC1)다. 규칙은 공개 헤더 주석을 따른다.
// DXC 호출과 spirv-val은 공용 도구 호출(CompileHlslToSpirv, SH1e E2)이다. SH2b1(16.1절 SSD29):
// -fspv-reflect로 컴파일하고, 고쳐 쓰기(ATTRIBUTEn·TEXCOORDn location, set = 단계·binding = 종류
// 순서, 리플렉션 장식 제거) 뒤 spirv-val로 다시 검증하고, 리플렉션을 공용 규칙으로 정규화해 값 1을
// 낸다.
namespace rocks::shader {

namespace vulkan_format_detail {

ShaderWorkerJobStatus VulkanJobStatus(ShaderToolOutcome outcome) noexcept {
    switch (outcome) {
    case ShaderToolOutcome::Success:
        return ShaderWorkerJobStatus::Success;
    case ShaderToolOutcome::CompileError:
        return ShaderWorkerJobStatus::CompileError;
    case ShaderToolOutcome::UnsupportedHost:
        return ShaderWorkerJobStatus::UnsupportedCapability;
    case ShaderToolOutcome::ToolError:
        break;
    }

    return ShaderWorkerJobStatus::ToolError;
}

} // namespace vulkan_format_detail

void VulkanShaderFormatBackend::Compile(const ShaderFormatDefinition& format,
                                        const ShaderWorkerJob& job,
                                        const ShaderBackendContext& context,
                                        ShaderWorkerJobResult& result) noexcept {
    using namespace vulkan_format_detail;
    if (format.compiler.path != ShaderCompilerPath::DxcSpirv ||
        format.compiler.spirvTargetEnv != SpirvTargetEnv::Vulkan1_1) {
        FailShaderJob(
            result, ShaderWorkerJobStatus::UnsupportedCapability,
            "this shader compile worker does not implement the format's compiler path yet",
            format.Id());
        return;
    }

    if (context.tools == nullptr || !context.tools->IsOpen()) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "shader tools are not available");
        return;
    }

    char profile[MaxShaderProfileBytes];
    if (!MakeShaderProfile(job.stage, format.compiler.shaderModel, profile) ||
        job.options.optimization >= ShaderOptimizationLevels) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError,
                      "invalid shader stage or options");
        return;
    }

    SpirvCompileRequest request;
    request.source = job.source.View();
    request.entry = job.entry.View();
    request.profile = StringView(profile);
    request.hlsl2021 = job.options.hlsl2021;
    request.optimization = job.options.optimization;
    request.dxPositionW = format.compiler.dxPositionW;
    request.reflect = true;
    Allocator& allocator = result.values.GetAllocator();
    ShaderParameterLayoutSet layouts(allocator);
    if (!DecodeShaderJobLayouts(job, layouts, result)) {
        return;
    }

    Array<uint8> spirv(allocator);
    String diagnostics(allocator);
    const ShaderToolOutcome compiled =
        CompileHlslToSpirv(*context.tools, request, spirv, diagnostics);
    if (compiled != ShaderToolOutcome::Success) {
        FailShaderJob(result, VulkanJobStatus(compiled), diagnostics.View());
        return;
    }

    // byte → word(SPIR-V는 4 byte 단위다 — DXC 출력은 늘 4의 배수다).
    Array<uint32> raw(allocator);
    if (spirv.Size() % sizeof(uint32) != 0 || !raw.TryResize(spirv.Size() / sizeof(uint32))) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      "DXC produced a malformed SPIR-V module");
        return;
    }

    platform::MemoryCopy(raw.Data(), spirv.Data(), spirv.Size());
    SpirvRewriteOptions options;
    options.stage = job.stage;
    options.stageLocations = true;
    Array<uint32> rewritten(allocator);
    if (auto done = RewriteSpirv(raw.View(), options, rewritten, diagnostics); !done) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError, "SPIR-V binding rules",
                      diagnostics.View());
        return;
    }

    Array<uint8> code(allocator);
    if (!code.TryResize(rewritten.Size() * sizeof(uint32))) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    platform::MemoryCopy(code.Data(), rewritten.Data(), code.Size());
    if (!ValidateSpirv(context.tools->SpirvTools(), code.View(), diagnostics)) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      "spirv-val rejected the rewritten SPIR-V", diagnostics.View());
        return;
    }

    Array<uint8> binding(allocator);
    const ShaderToolOutcome normalized = BuildSpirvBindingLayout(
        format, job.stage, layouts.View(), raw.View(), rewritten.View(), binding, diagnostics);
    if (normalized != ShaderToolOutcome::Success) {
        FailShaderJob(result, VulkanJobStatus(normalized), "shader binding rules",
                      diagnostics.View());
        return;
    }

    SucceedShaderJob(result, code, binding);
}

} // namespace rocks::shader
