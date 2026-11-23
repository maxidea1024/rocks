#include <ShaderFormatD3D/D3DShaderFormat.h>

#include <ShaderToolLoader/ShaderToolLoader.h>

// D3D 형식 처리기(Docs/Design/ShaderSystem 11.1절 SC1)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace d3d_format_detail {

// DXIL container 머리: magic 4 byte + hash 16 byte(업스트림 DxilContainer.h DxilContainerHeader).
constexpr usize ContainerMagicBytes = 4;
constexpr usize ContainerHashBytes = 16;
constexpr uint8 ContainerMagic[ContainerMagicBytes] = {'D', 'X', 'B', 'C'};
// preview validator 우회 hash 값(0x02 × 16, 업스트림 PreviewByPassHash)이다.
constexpr uint8 PreviewBypassByte = 0x02;
// 고정 DXC 인자 수의 상한이다.
constexpr usize MaxDxcArguments = 16;

constexpr StringView OptimizationFlags[] = {"-O0", "-O1", "-O2", "-O3"};

} // namespace d3d_format_detail

bool IsSignedDxilContainer(Span<const uint8> container) noexcept {
    using namespace d3d_format_detail;
    if (container.Size() < ContainerMagicBytes + ContainerHashBytes) {
        return false;
    }

    for (usize index = 0; index < ContainerMagicBytes; ++index) {
        if (container[index] != ContainerMagic[index]) {
            return false;
        }
    }

    bool zero = true;
    bool bypass = true;
    for (usize index = 0; index < ContainerHashBytes; ++index) {
        const uint8 value = container[ContainerMagicBytes + index];
        zero = zero && value == 0;
        bypass = bypass && value == PreviewBypassByte;
    }

    return !zero && !bypass;
}

void D3DShaderFormatBackend::Compile(const ShaderFormatDefinition& format,
                                     const ShaderWorkerJob& job, const ShaderToolSet* tools,
                                     ShaderWorkerJobResult& result) noexcept {
    using namespace d3d_format_detail;
    if (format.compiler.path != ShaderCompilerPath::DxcDxil) {
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
        job.options.optimization >= sizeof(OptimizationFlags) / sizeof(OptimizationFlags[0])) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError,
                      "invalid shader stage or options");
        return;
    }

    const StringView arguments[MaxDxcArguments] = {"-E",
                                                   job.entry.View(),
                                                   "-T",
                                                   StringView(profile),
                                                   "-HV",
                                                   job.options.hlsl2021 ? StringView("2021")
                                                                        : StringView("2018"),
                                                   OptimizationFlags[job.options.optimization],
                                                   "-Zpr",
                                                   "-WX"};
    constexpr usize ArgumentCount = 9;
    DxcCompileRequest request;
    request.source = job.source.View();
    request.arguments = Span<const StringView>(arguments, ArgumentCount);
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

    if (!IsSignedDxilContainer(compiled.object.View())) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      "DXC produced an unsigned DXIL container");
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
