#include <ShaderFormatVulkan/VulkanShaderFormat.h>

#include <ShaderToolLoader/ShaderToolLoader.h>
#include <spirv-tools/libspirv.h>

// Vulkan 형식 처리기(Docs/Design/ShaderSystem 11.1절 SC1)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace vulkan_format_detail {

// 고정 DXC 인자 수의 상한이다.
constexpr usize MaxFormatDxcArguments = 16;

constexpr StringView FormatOptimizationFlags[] = {"-O0", "-O1", "-O2", "-O3"};

template <class Function>
Function SpirvFunction(const SpirvToolsFunctions& tools, usize index) noexcept {
    return reinterpret_cast<Function>(tools.address[index]);
}

// spirv-val(Vulkan 1.1 환경)이다. 실패하면 diagnostics에 검증기 문구를 둔다.
bool ValidateSpirvWords(const SpirvToolsFunctions& tools, Span<const uint8> code,
                        String& diagnostics) noexcept {
    using Index = SpirvToolsFunctions;
    const auto contextCreate =
        SpirvFunction<decltype(&spvContextCreate)>(tools, Index::ContextCreate);
    const auto contextDestroy =
        SpirvFunction<decltype(&spvContextDestroy)>(tools, Index::ContextDestroy);
    const auto optionsCreate =
        SpirvFunction<decltype(&spvValidatorOptionsCreate)>(tools, Index::ValidatorOptionsCreate);
    const auto optionsDestroy =
        SpirvFunction<decltype(&spvValidatorOptionsDestroy)>(tools, Index::ValidatorOptionsDestroy);
    const auto validate =
        SpirvFunction<decltype(&spvValidateWithOptions)>(tools, Index::ValidateWithOptions);
    const auto diagnosticDestroy =
        SpirvFunction<decltype(&spvDiagnosticDestroy)>(tools, Index::DiagnosticDestroy);
    if (contextCreate == nullptr || validate == nullptr || code.Size() % sizeof(uint32) != 0) {
        (void)diagnostics.TryAppend("spirv-val is not available or the module is not word aligned");
        return false;
    }

    spv_context context = contextCreate(SPV_ENV_VULKAN_1_1);
    spv_validator_options options = optionsCreate();
    spv_const_binary_t binary{reinterpret_cast<const uint32*>(code.Data()),
                              code.Size() / sizeof(uint32)};
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t validated = validate(context, options, &binary, &diagnostic);
    if (validated != SPV_SUCCESS && diagnostic != nullptr && diagnostic->error != nullptr) {
        (void)diagnostics.TryAppend(StringView(diagnostic->error));
    }

    diagnosticDestroy(diagnostic);
    optionsDestroy(options);
    contextDestroy(context);
    return validated == SPV_SUCCESS;
}

} // namespace vulkan_format_detail

void VulkanShaderFormatBackend::Compile(const ShaderFormatDefinition& format,
                                        const ShaderWorkerJob& job, const ShaderToolSet* tools,
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

    if (tools == nullptr || !tools->IsOpen()) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "shader tools are not available");
        return;
    }

    char profile[MaxShaderProfileBytes];
    if (!MakeShaderProfile(job.stage, format.compiler.shaderModel, profile) ||
        job.options.optimization >=
            sizeof(FormatOptimizationFlags) / sizeof(FormatOptimizationFlags[0])) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError,
                      "invalid shader stage or options");
        return;
    }

    StringView arguments[MaxFormatDxcArguments] = {
        "-E",
        job.entry.View(),
        "-T",
        StringView(profile),
        "-HV",
        job.options.hlsl2021 ? StringView("2021") : StringView("2018"),
        FormatOptimizationFlags[job.options.optimization],
        "-Zpr",
        "-WX",
        "-spirv",
        "-fspv-target-env=vulkan1.1"};
    usize count = 11;
    if (format.compiler.dxPositionW) {
        arguments[count++] = "-fvk-use-dx-position-w";
    }

    DxcCompileRequest request;
    request.source = job.source.View();
    request.arguments = Span<const StringView>(arguments, count);
    DxcCompileResult compiled(result.values.GetAllocator());
    const ShaderToolLoadStatus ran = CompileWithDxc(tools->Dxc(), request, compiled);
    if (!ran) {
        FailShaderJob(result,
                      ran.code == ShaderToolLoadCode::UnsupportedHost
                          ? ShaderWorkerJobStatus::UnsupportedCapability
                          : ShaderWorkerJobStatus::ToolError,
                      "DXC call failed", StringView(ShaderToolLoadCodeName(ran.code)));
        return;
    }

    if (!compiled.compiled) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError, "DXC compile failed",
                      compiled.diagnostics.View());
        return;
    }

    String validation(result.values.GetAllocator());
    if (!ValidateSpirvWords(tools->SpirvTools(), compiled.object.View(), validation)) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "spirv-val rejected the SPIR-V",
                      validation.View());
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
    code.Value()->bytes = Move(compiled.object);
}

} // namespace rocks::shader
