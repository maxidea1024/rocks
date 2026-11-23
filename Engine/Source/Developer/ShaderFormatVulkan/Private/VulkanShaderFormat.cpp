#include <ShaderFormatVulkan/VulkanShaderFormat.h>

#include <ShaderToolLoader/ShaderToolCalls.h>

// Vulkan 형식 처리기(Docs/Design/ShaderSystem 11.1절 SC1)다. 규칙은 공개 헤더 주석을 따른다.
// DXC 호출과 spirv-val은 공용 도구 호출(CompileHlslToSpirv, SH1e E2)이다.
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
    Array<uint8> spirv(result.values.GetAllocator());
    String diagnostics(result.values.GetAllocator());
    const ShaderToolOutcome compiled =
        CompileHlslToSpirv(*context.tools, request, spirv, diagnostics);
    if (compiled != ShaderToolOutcome::Success) {
        FailShaderJob(result, VulkanJobStatus(compiled), diagnostics.View());
        return;
    }

    result.status = ShaderWorkerJobStatus::Success;
    result.diagnostics.Clear();
    result.values.Clear();
    auto code = result.values.TryEmplaceBack(result.values.GetAllocator());
    if (!code) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    code.Value()->id = 0;
    code.Value()->bytes = Move(spirv);
}

} // namespace rocks::shader
