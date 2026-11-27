#include <ShaderFormatD3D/D3DShaderFormat.h>

#include "D3DReflection.h"

#include <Platform/Memory.h>
#include <ShaderCompilerCommon/SpirvRewrite.h>

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
// 대체 경로 중간 HLSL 앞에 붙이는 줄이다(ShaderSystem 16.4절 SSD81, UE D3DShaderCompiler.cpp
// PatchHlslForPrecompilation — 중간 원문이 만든 pow 음수 밑 경고 X3571을 끄고 원래 원문의 경고만
// 본다).
constexpr StringView IntermediateWarningPragma("#pragma warning(disable : 3571)\n");
// 대체 경로가 성공했을 때 1차 FXC 진단 뒤에 붙이는 문구다(SSD80).
constexpr StringView
    FallbackSucceededNote("compiled through the fallback path (DXC, SPIR-V, HLSL 5.0, FXC)\n");

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
             const ShaderToolSet& tools, ShaderWorkerJobResult& result, Array<uint8>& code,
             const Array<SpirvSamplerUse>* pairs = nullptr) noexcept {
    ShaderParameterLayoutSet layouts(result.values.GetAllocator());
    if (!DecodeShaderJobLayouts(job, layouts, result)) {
        return;
    }

    Array<ShaderSamplerUse> uses(result.values.GetAllocator());
    if (pairs != nullptr) {
        for (const SpirvSamplerUse& pair : *pairs) {
            if (!uses.TryPushBack(ShaderSamplerUse{pair.texture.View(), pair.sampler.View()})) {
                FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
                return;
            }
        }
    }

    Array<uint8> binding(result.values.GetAllocator());
    String diagnostics(result.values.GetAllocator());
    const ShaderToolOutcome normalized =
        BuildD3DBindingLayout(kind, tools, format, job.stage, job.requiredFeatures, layouts.View(),
                              uses.View(), pairs != nullptr, code.View(), binding, diagnostics);
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

    StringView arguments[MaxDxcArguments] = {"-E",
                                             job.entry.View(),
                                             "-T",
                                             StringView(profile),
                                             "-HV",
                                             job.options.hlsl2021 ? StringView("2021")
                                                                  : StringView("2018"),
                                             OptimizationFlags[job.options.optimization],
                                             "-Zpr"};
    usize count = 8;
    // 경고는 오류(16.4절 SSD79 — job 옵션)다.
    if (job.options.warningsAsErrors) {
        arguments[count++] = "-WX";
    }

    // 디버그 심볼(16.3절 SSD73, UE D3DShaderCompilerDXC.cpp와 같다): PDB를 만들고(-Zi) DXIL에서
    // 떼며(-Qstrip_debug) PDB 이름을 출력 binary hash로 정한다(-Zsb).
    if (job.options.debugSymbols) {
        arguments[count++] = "-Zi";
        arguments[count++] = "-Qstrip_debug";
        arguments[count++] = "-Zsb";
    }

    DxcCompileRequest request;
    request.source = job.source.View();
    request.arguments = Span<const StringView>(arguments, count);
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

    if (job.options.debugSymbols && compiled.pdb.IsEmpty()) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "DXC produced no PDB");
        return;
    }

    Succeed(D3DCodeKind::Dxil, format, job, tools, result, compiled.object);
    if (job.options.debugSymbols) {
        AddShaderJobDebugSymbols(result, ShaderDebugSymbolKind::Pdb, compiled.pdbName.View(),
                                 compiled.pdb.View());
    }

    AddShaderJobWarnings(result, "DXC", compiled.diagnostics.View());
}

// 한 단계 경고를 warnings에 `<stage>: <text>` 줄로 모은다(16.4절 SSD79). 할당 실패면 false다.
bool KeepWarnings(String& warnings, StringView stage, StringView text) noexcept {
    if (text.IsEmpty()) {
        return true;
    }

    return warnings.TryAppend(stage) && warnings.TryAppend(": ") && warnings.TryAppend(text) &&
           (text.EndsWith("\n", StringComparison::CaseSensitive) || warnings.TryAppend("\n"));
}

// 대체 경로: DXC(-fspv-reflect) → SPIR-V 고쳐 쓰기(ATTRIBUTEn·TEXCOORDn location) → SPIRV-Cross
// HLSL 5.0(register 배정·ATTRIBUTEn, 16.1절 SSD38) → X3571 끄기(16.4절 SSD81) → FXC(E4). 사용
// 쌍은 pairs에 둔다(SSD36). 성공하면 단계 경고(`fallback DXC: `·`fallback FXC: `)를 warnings에
// 더한다. 실패하면 outcome과 diagnostics다.
ShaderToolOutcome CompileFallback(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                                  const ShaderToolSet& tools, StringView fxcProfile,
                                  Array<uint8>& object, Array<SpirvSamplerUse>& pairs,
                                  String& diagnostics, String& warnings,
                                  FxcSymbols& symbols) noexcept {
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
    spirvRequest.reflect = true;
    spirvRequest.warningsAsErrors = job.options.warningsAsErrors;
    Allocator& allocator = object.GetAllocator();
    Array<uint8> raw(allocator);
    const ShaderToolOutcome compiled = CompileHlslToSpirv(tools, spirvRequest, raw, diagnostics);
    if (compiled != ShaderToolOutcome::Success) {
        return compiled;
    }

    if (!KeepWarnings(warnings, "fallback DXC", diagnostics.View())) {
        (void)diagnostics.TryAssign("out of memory");
        return ShaderToolOutcome::ToolError;
    }

    diagnostics.Clear();

    Array<uint32> words(allocator);
    Array<uint32> rewritten(allocator);
    Array<uint8> spirv(allocator);
    if (raw.Size() % sizeof(uint32) != 0 || !words.TryResize(raw.Size() / sizeof(uint32))) {
        (void)diagnostics.TryAppend("DXC produced a malformed SPIR-V module");
        return ShaderToolOutcome::ToolError;
    }

    platform::MemoryCopy(words.Data(), raw.Data(), raw.Size());
    SpirvRewriteOptions options;
    options.stage = job.stage;
    options.stageLocations = true;
    if (!RewriteSpirv(words.View(), options, rewritten, diagnostics)) {
        return ShaderToolOutcome::CompileError;
    }

    if (!spirv.TryResize(rewritten.Size() * sizeof(uint32))) {
        (void)diagnostics.TryAppend("out of memory");
        return ShaderToolOutcome::ToolError;
    }

    platform::MemoryCopy(spirv.Data(), rewritten.Data(), spirv.Size());
    const ShaderToolOutcome queried =
        QuerySpirvSamplerUses(tools.SpirvCross(), spirv.View(), pairs, diagnostics);
    if (queried != ShaderToolOutcome::Success) {
        return queried;
    }

    SpirvCrossRequest crossRequest;
    crossRequest.spirv = spirv.View();
    crossRequest.target = SpirvCrossTarget::Hlsl50;
    crossRequest.stage = job.stage;
    crossRequest.hlslAssignRegisters = true;
    String hlsl(object.GetAllocator());
    const ShaderToolOutcome crossed =
        CrossCompileSpirv(tools.SpirvCross(), crossRequest, hlsl, diagnostics);
    if (crossed != ShaderToolOutcome::Success) {
        return crossed;
    }

    // 중간 원문 경고 X3571을 끈다(16.4절 SSD81 — 원문 FXC 경로는 그대로다).
    String patched(allocator);
    if (!patched.TryAppend(IntermediateWarningPragma) || !patched.TryAppend(hlsl.View())) {
        (void)diagnostics.TryAssign("out of memory");
        return ShaderToolOutcome::ToolError;
    }

    FxcCompileRequest fxcRequest;
    fxcRequest.source = patched.View();
    fxcRequest.entry = CrossEntry;
    fxcRequest.profile = fxcProfile;
    fxcRequest.optimization = job.options.optimization;
    fxcRequest.debugSymbols = job.options.debugSymbols;
    fxcRequest.warningsAsErrors = job.options.warningsAsErrors;
    const ShaderToolOutcome fxc =
        CompileWithFxc(tools.Fxc(), fxcRequest, object, diagnostics, &symbols);
    if (fxc == ShaderToolOutcome::Success &&
        !KeepWarnings(warnings, "fallback FXC", diagnostics.View())) {
        (void)diagnostics.TryAssign("out of memory");
        return ShaderToolOutcome::ToolError;
    }

    return fxc;
}

// D3D11_SM5: FXC, 원문 탓 실패(16.4절 SSD80 분류)나 HLSL 2021이면 대체 경로(SC10, E3·E4)다.
// 대체 경로가 성공하면 1차 FXC 진단을 결과 진단 앞에 남긴다(SSD80 — UE와 같다).
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
    FxcSymbols symbols(allocator);
    // 디버그 심볼(16.3절 SSD73): 값 0은 디버그 정보를 뗀 DXBC, 값 3은 FXC PDB다. 경고는 값 뒤에
    // 진단으로 둔다(SSD79).
    const auto succeed = [&](const Array<SpirvSamplerUse>* pairs, StringView warnings) noexcept {
        Succeed(D3DCodeKind::Dxbc, format, job, tools, result, object, pairs);
        if (job.options.debugSymbols) {
            AddShaderJobDebugSymbols(result, ShaderDebugSymbolKind::Pdb, symbols.name.View(),
                                     symbols.pdb.View());
        }

        if (result.status == ShaderWorkerJobStatus::Success && !warnings.IsEmpty() &&
            !result.diagnostics.TryAppend(warnings)) {
            FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                          "out of memory while keeping shader warnings");
        }
    };
    if (!job.options.hlsl2021) {
        FxcCompileRequest request;
        request.source = job.source.View();
        request.entry = job.entry.View();
        request.profile = StringView(profile);
        request.optimization = job.options.optimization;
        request.debugSymbols = job.options.debugSymbols;
        request.warningsAsErrors = job.options.warningsAsErrors;
        const ShaderToolOutcome compiled =
            CompileWithFxc(tools.Fxc(), request, object, fxcDiagnostics, &symbols);
        if (compiled == ShaderToolOutcome::Success) {
            String warnings(allocator);
            if (!KeepWarnings(warnings, "FXC", fxcDiagnostics.View())) {
                FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
                return;
            }

            succeed(nullptr, warnings.View());
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
    // 성공 진단: 1차 FXC가 실패했으면 그 진단과 대체 경로 표시, 그 뒤 대체 경로 단계 경고다.
    String warnings(allocator);
    if (!fxcDiagnostics.IsEmpty() && !(KeepWarnings(warnings, "FXC", fxcDiagnostics.View()) &&
                                       warnings.TryAppend(FallbackSucceededNote))) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    Array<SpirvSamplerUse> pairs(allocator);
    const ShaderToolOutcome fallback =
        CompileFallback(format, job, tools, StringView(profile), object, pairs, fallbackDiagnostics,
                        warnings, symbols);
    if (fallback == ShaderToolOutcome::Success) {
        succeed(&pairs, warnings.View());
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
