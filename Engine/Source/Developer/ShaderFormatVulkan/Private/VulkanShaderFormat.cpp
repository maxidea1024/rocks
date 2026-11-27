#include <ShaderFormatVulkan/VulkanShaderFormat.h>

#include <Platform/Memory.h>
#include <ShaderCompilerCommon/ShaderSpirvBindings.h>
#include <ShaderCompilerCommon/SpirvRewrite.h>
#include <ShaderToolLoader/ShaderToolCalls.h>

// Vulkan 형식 처리기(Docs/Design/ShaderSystem 11.1절 SC1)다. 규칙은 공개 헤더 주석을 따른다.
// DXC 호출과 spirv-val은 공용 도구 호출(CompileHlslToSpirv, SH1e E2)이다. SH2b1(16.1절 SSD29):
// -fspv-reflect로 컴파일하고, 고쳐 쓰기(ATTRIBUTEn·TEXCOORDn location, set = 단계·binding = 종류
// 순서, 리플렉션 장식 제거) 뒤 spirv-val로 다시 검증하고, 리플렉션을 공용 규칙으로 정규화해 값 1을
// 낸다. SH2b2(SSD34–SSD36·SSD40): storage image 형식 지정 원문 검사, SPIRV-Cross 사용 쌍, job 요구
// 기능, PS 진단 버퍼(set 3은 다시 매기지 않는다).
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
    request.warningsAsErrors = job.options.warningsAsErrors;
    Allocator& allocator = result.values.GetAllocator();
    ShaderParameterLayoutSet layouts(allocator);
    if (!DecodeShaderJobLayouts(job, layouts, result)) {
        return;
    }

    Array<uint8> spirv(allocator);
    String diagnostics(allocator);
    if (!CheckStorageImageFormats(job.source.View(), diagnostics)) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError, "shader binding rules",
                      diagnostics.View());
        return;
    }

    const ShaderToolOutcome compiled =
        CompileHlslToSpirv(*context.tools, request, spirv, diagnostics);
    if (compiled != ShaderToolOutcome::Success) {
        FailShaderJob(result, VulkanJobStatus(compiled), diagnostics.View());
        return;
    }

    // DXC 경고는 결과 값 뒤에 진단으로 둔다(16.4절 SSD79).
    String warnings(allocator);
    if (!warnings.TryAppend(diagnostics.View())) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    diagnostics.Clear();

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

    Array<SpirvSamplerUse> pairs(allocator);
    const ShaderToolOutcome queried =
        QuerySpirvSamplerUses(context.tools->SpirvCross(), code.View(), pairs, diagnostics);
    if (queried != ShaderToolOutcome::Success) {
        FailShaderJob(result, VulkanJobStatus(queried), "SPIR-V sampler use query",
                      diagnostics.View());
        return;
    }

    Array<ShaderSamplerUse> uses(allocator);
    for (const SpirvSamplerUse& pair : pairs) {
        if (!uses.TryPushBack(ShaderSamplerUse{pair.texture.View(), pair.sampler.View()})) {
            FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
            return;
        }
    }

    SpirvBindingRequest binding;
    binding.format = &format;
    binding.stage = job.stage;
    binding.requiredFeatures = job.requiredFeatures;
    binding.layouts = layouts.View();
    binding.raw = raw.View();
    binding.rewritten = rewritten.View();
    binding.uses = uses.View();
    binding.hasUses = true;
    SpirvBindingResult bound(allocator);
    const ShaderWorkerJobStatus normalized = BuildSpirvBindingLayout(binding, bound, diagnostics);
    if (normalized != ShaderWorkerJobStatus::Success) {
        FailShaderJob(result, normalized, "shader binding rules", diagnostics.View());
        return;
    }

    // 디버그 심볼(16.3절 SSD73): 같은 원문을 -Zi로 한 번 더 컴파일한 SPIR-V(OpSource·OpLine)가 값
    // 3이다. 값 0은 심볼을 끈 job과 같은 byte로 남는다(따로 컴파일하므로).
    Array<uint8> debugModule(allocator);
    if (job.options.debugSymbols) {
        request.debugInfo = true;
        const ShaderToolOutcome debug =
            CompileHlslToSpirv(*context.tools, request, debugModule, diagnostics);
        if (debug != ShaderToolOutcome::Success) {
            FailShaderJob(result, VulkanJobStatus(debug), "SPIR-V debug module",
                          diagnostics.View());
            return;
        }
    }

    SucceedShaderJob(result, code, bound.bindingLayout);
    if (job.options.debugSymbols) {
        AddShaderJobDebugSymbols(result, ShaderDebugSymbolKind::SpirvModule, StringView(),
                                 debugModule.View());
    }

    AddShaderJobWarnings(result, "DXC", warnings.View());
}

} // namespace rocks::shader
