#include <ShaderFormatD3D/D3DShaderFormat.h>

#include "D3DReflection.h"

#include <ShaderToolLoader/ShaderToolCalls.h>
#include <ShaderToolLoader/ShaderToolLoader.h>

// D3D 형식 처리기(Docs/Design/ShaderSystem 11.1절 SC1·SH1e E3·E4)다. 규칙은 공개 헤더 주석을
// 따른다.
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
// 대체 경로의 DXC SPIR-V 셰이더 모델(DXC SPIR-V는 6.0 아래 프로필을 받지 않는다 — E4)이다.
constexpr ShaderVersion FallbackSpirvModel{6, 0};
// SPIRV-Cross HLSL 출력의 엔트리 이름이다.
constexpr StringView CrossEntry("main");

constexpr StringView OptimizationFlags[ShaderOptimizationLevels] = {"-O0", "-O1", "-O2", "-O3"};

ShaderWorkerJobStatus D3DJobStatus(ShaderToolOutcome outcome) noexcept {
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

// 리플렉션을 공용 규칙으로 정규화해 값 0(Code)·값 1(BindingLayout)을 둔다(16.1절 SSD33).
void Succeed(D3DCodeKind kind, const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
             const ShaderToolSet& tools, ShaderWorkerJobResult& result,
             Array<uint8>& code) noexcept {
    ShaderParameterLayoutSet layouts(result.values.GetAllocator());
    if (!DecodeShaderJobLayouts(job, layouts, result)) {
        return;
    }

    Array<uint8> binding(result.values.GetAllocator());
    String diagnostics(result.values.GetAllocator());
    const ShaderToolOutcome normalized = BuildD3DBindingLayout(
        kind, tools, format, job.stage, layouts.View(), code.View(), binding, diagnostics);
    if (normalized != ShaderToolOutcome::Success) {
        FailShaderJob(result, D3DJobStatus(normalized), "shader binding rules", diagnostics.View());
        return;
    }

    SucceedShaderJob(result, code, binding);
}

void CompileDxil(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                 const ShaderToolSet& tools, ShaderWorkerJobResult& result) noexcept {
    char profile[MaxShaderProfileBytes];
    if (!MakeShaderProfile(job.stage, format.compiler.shaderModel, profile) ||
        job.options.optimization >= ShaderOptimizationLevels) {
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
    const ShaderToolLoadStatus ran = CompileWithDxc(tools.Dxc(), request, compiled);
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

    Succeed(D3DCodeKind::Dxil, format, job, tools, result, compiled.object);
}

// 대체 경로: DXC → SPIR-V → SPIRV-Cross HLSL 5.0 → FXC(E4). 실패하면 outcome과 diagnostics다.
ShaderToolOutcome CompileFallback(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                                  const ShaderToolSet& tools, StringView fxcProfile,
                                  Array<uint8>& object, String& diagnostics) noexcept {
    char spirvProfile[MaxShaderProfileBytes];
    if (!MakeShaderProfile(job.stage, FallbackSpirvModel, spirvProfile)) {
        (void)diagnostics.TryAppend("invalid shader stage");
        return ShaderToolOutcome::CompileError;
    }

    SpirvCompileRequest spirvRequest;
    spirvRequest.source = job.source.View();
    spirvRequest.entry = job.entry.View();
    spirvRequest.profile = StringView(spirvProfile);
    spirvRequest.hlsl2021 = job.options.hlsl2021;
    spirvRequest.optimization = job.options.optimization;
    spirvRequest.dxPositionW = format.compiler.dxPositionW;
    Array<uint8> spirv(object.GetAllocator());
    const ShaderToolOutcome compiled = CompileHlslToSpirv(tools, spirvRequest, spirv, diagnostics);
    if (compiled != ShaderToolOutcome::Success) {
        return compiled;
    }

    SpirvCrossRequest crossRequest;
    crossRequest.spirv = spirv.View();
    crossRequest.target = SpirvCrossTarget::Hlsl50;
    String hlsl(object.GetAllocator());
    const ShaderToolOutcome crossed =
        CrossCompileSpirv(tools.SpirvCross(), crossRequest, hlsl, diagnostics);
    if (crossed != ShaderToolOutcome::Success) {
        return crossed;
    }

    FxcCompileRequest fxcRequest;
    fxcRequest.source = hlsl.View();
    fxcRequest.entry = CrossEntry;
    fxcRequest.profile = fxcProfile;
    fxcRequest.optimization = job.options.optimization;
    return CompileWithFxc(tools.Fxc(), fxcRequest, object, diagnostics);
}

// D3D11_SM5: FXC, 원문 탓 실패나 HLSL 2021이면 대체 경로(SC10, E3·E4)다.
void CompileDxbc(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                 const ShaderToolSet& tools, ShaderWorkerJobResult& result) noexcept {
    char profile[MaxShaderProfileBytes];
    if (!MakeShaderProfile(job.stage, format.compiler.shaderModel, profile) ||
        job.options.optimization >= ShaderOptimizationLevels) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError,
                      "invalid shader stage or options");
        return;
    }

    if (!tools.HasFxc()) {
        FailShaderJob(result, ShaderWorkerJobStatus::UnsupportedCapability,
                      "FXC is not available on this host", format.Id());
        return;
    }

    Allocator& allocator = result.values.GetAllocator();
    Array<uint8> object(allocator);
    String fxcDiagnostics(allocator);
    if (!job.options.hlsl2021) {
        FxcCompileRequest request;
        request.source = job.source.View();
        request.entry = job.entry.View();
        request.profile = StringView(profile);
        request.optimization = job.options.optimization;
        const ShaderToolOutcome compiled =
            CompileWithFxc(tools.Fxc(), request, object, fxcDiagnostics);
        if (compiled == ShaderToolOutcome::Success) {
            Succeed(D3DCodeKind::Dxbc, format, job, tools, result, object);
            return;
        }

        // 컴파일 오류만 대체 경로를 탄다. 그 밖의 FXC 실패는 도구 오류다(같은 키에서 다른 출력이
        // 나오지 않게 — SC10).
        if (compiled != ShaderToolOutcome::CompileError) {
            FailShaderJob(result, D3DJobStatus(compiled), "FXC call failed", fxcDiagnostics.View());
            return;
        }
    }

    String fallbackDiagnostics(allocator);
    const ShaderToolOutcome fallback =
        CompileFallback(format, job, tools, StringView(profile), object, fallbackDiagnostics);
    if (fallback == ShaderToolOutcome::Success) {
        Succeed(D3DCodeKind::Dxbc, format, job, tools, result, object);
        return;
    }

    String diagnostics(allocator);
    if (!fxcDiagnostics.IsEmpty()) {
        (void)(diagnostics.TryAppend("FXC: ") && diagnostics.TryAppend(fxcDiagnostics.View()) &&
               diagnostics.TryAppend("\n"));
    }

    (void)(diagnostics.TryAppend("fallback (DXC, SPIR-V, HLSL 5.0, FXC): ") &&
           diagnostics.TryAppend(fallbackDiagnostics.View()));
    FailShaderJob(result, D3DJobStatus(fallback), "D3D11 compile failed", diagnostics.View());
}

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
                                     const ShaderWorkerJob& job,
                                     const ShaderBackendContext& context,
                                     ShaderWorkerJobResult& result) noexcept {
    using namespace d3d_format_detail;
    if (format.compiler.path != ShaderCompilerPath::DxcDxil &&
        format.compiler.path != ShaderCompilerPath::Fxc) {
        FailShaderJob(result, ShaderWorkerJobStatus::UnsupportedCapability,
                      "the D3D processor does not implement the format's compiler path",
                      format.Id());
        return;
    }

    if (context.tools == nullptr || !context.tools->IsOpen()) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "shader tools are not available");
        return;
    }

    if (format.compiler.path == ShaderCompilerPath::DxcDxil) {
        CompileDxil(format, job, *context.tools, result);
    } else {
        CompileDxbc(format, job, *context.tools, result);
    }
}

} // namespace rocks::shader
