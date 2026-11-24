#include <ShaderFormatMetal/MetalShaderFormat.h>

#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Platform/Process.h>
#include <ShaderCompilerCommon/ShaderSpirvBindings.h>
#include <ShaderCompilerCommon/SpirvRewrite.h>
#include <ShaderToolLoader/MetalToolchain.h>
#include <ShaderToolLoader/ShaderToolCalls.h>
#include <stdio.h>
#include <string.h>

// Metal 형식 처리기(Docs/Design/ShaderSystem 11.1절 SH1e E6)다. 규칙은 공개 헤더 주석을 따른다.
// metal·metallib 실행은 이 기계(Windows, Metal Developer Tools 없음)에서 확인하지 못했다.
namespace rocks::shader {

namespace metal_format_detail {

// 임시 파일 이름이다(job마다 같다 — 결과 byte에 임시 경로가 들어가지 않게).
constexpr char SourceName[] = "job.metal";
constexpr char AirName[] = "job.air";
constexpr char LibraryName[] = "job.metallib";
// 도구 출력(진단)을 모을 버퍼 크기다.
constexpr usize ToolOutputBytes = 64 * 1024;
// 인자 버퍼 크기다.
constexpr usize ArgumentBytes = 64;

struct MinimumVersion {
    ShaderVersion msl;
    uint32 macos;
};

constexpr MinimumVersion MinimumVersions[] = {{{2, 4}, 12}, {{3, 0}, 13}, {{3, 1}, 14}};

ShaderWorkerJobStatus MetalJobStatus(ShaderToolOutcome outcome) noexcept {
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

bool PathOf(StringView directory, const char* name, char (&out)[MaxShaderToolPathBytes]) noexcept {
    const int bytes = snprintf(out, sizeof(out), "%.*s/%s", static_cast<int>(directory.ByteCount()),
                               directory.Data(), name);
    return bytes > 0 && static_cast<usize>(bytes) < sizeof(out);
}

bool WriteText(const char* path, StringView text) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    const platform::FileTransfer written = file.WriteAll(text.Data(), text.ByteCount());
    return static_cast<bool>(file.Close()) && static_cast<bool>(written.status);
}

bool ReadBytes(const char* path, Array<uint8>& out) noexcept {
    platform::File file;
    uint64 size = 0;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting) || !file.Size(size) ||
        !out.TryResize(static_cast<usize>(size))) {
        return false;
    }

    const platform::FileTransfer read = file.ReadExact(out.Data(), out.Size());
    return static_cast<bool>(file.Close()) && static_cast<bool>(read.status);
}

// 도구 하나를 실행한다. 0으로 끝나지 않으면 false와 진단(도구 출력)이다.
bool RunTool(const char* executable, Span<const platform::ProcessArgument> arguments,
             StringView scratch, String& diagnostics) noexcept {
    char output[ToolOutputBytes];
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = strlen(executable);
    info.arguments = arguments.Data();
    info.argumentCount = arguments.Size();
    info.workingDirectory = scratch.Data();
    info.workingDirectoryBytes = scratch.ByteCount();
    info.output = platform::ProcessStreamMode::Pipe;
    info.error = platform::ProcessStreamMode::Pipe;
    info.mergeErrorIntoOutput = true;
    platform::ProcessRunOptions options;
    options.output = output;
    options.outputCapacity = sizeof(output);
    options.timeoutMilliseconds = MetalToolTimeoutMilliseconds;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    const bool ok = run.status && !run.timedOut &&
                    run.exit.kind == platform::ProcessExitKind::Exited && run.exit.code == 0;
    if (!ok) {
        (void)(diagnostics.TryAppend(StringView(executable)) && diagnostics.TryAppend(": ") &&
               diagnostics.TryAppend(StringView(output, run.outputBytes)));
        if (run.timedOut) {
            (void)diagnostics.TryAppend(" (timed out)");
        }
    }

    return ok;
}

platform::ProcessArgument Argument(const char* text) noexcept {
    return platform::ProcessArgument{text, strlen(text)};
}

} // namespace metal_format_detail

uint32 MetalMinimumMacOSVersion(ShaderVersion msl) noexcept {
    using namespace metal_format_detail;
    for (const MinimumVersion& entry : MinimumVersions) {
        if (entry.msl == msl) {
            return entry.macos;
        }
    }

    return 0;
}

void MetalShaderFormatBackend::Compile(const ShaderFormatDefinition& format,
                                       const ShaderWorkerJob& job,
                                       const ShaderBackendContext& context,
                                       ShaderWorkerJobResult& result) noexcept {
    using namespace metal_format_detail;
    if (format.compiler.path != ShaderCompilerPath::DxcSpirvMsl ||
        format.compiler.spirvTargetEnv != SpirvTargetEnv::Vulkan1_1 ||
        format.compiler.mslPlatform != MslPlatform::MacOS) {
        FailShaderJob(result, ShaderWorkerJobStatus::UnsupportedCapability,
                      "the Metal processor does not implement the format's compiler path",
                      format.Id());
        return;
    }

    // MSL만 내고 성공하지 않는다(11.1절 SH1e 행) — 도구가 없으면 먼저 끝낸다.
    if (context.metal == nullptr || !context.metal->found) {
        FailShaderJob(result, ShaderWorkerJobStatus::UnsupportedCapability,
                      "MetalToolchain (metal and metallib) is not available on this host",
                      format.Id());
        return;
    }

    if (context.tools == nullptr || !context.tools->IsOpen()) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "shader tools are not available");
        return;
    }

    const uint32 minimumMacOS = MetalMinimumMacOSVersion(format.compiler.mslVersion);
    char profile[MaxShaderProfileBytes];
    if (minimumMacOS == 0 || context.scratchDirectory.IsEmpty() ||
        !MakeShaderProfile(job.stage, format.compiler.shaderModel, profile) ||
        job.options.optimization >= ShaderOptimizationLevels) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      "invalid Metal format version, scratch directory, stage or options");
        return;
    }

    Allocator& allocator = result.values.GetAllocator();
    SpirvCompileRequest request;
    request.source = job.source.View();
    request.entry = job.entry.View();
    request.profile = StringView(profile);
    request.hlsl2021 = job.options.hlsl2021;
    request.optimization = job.options.optimization;
    request.dxPositionW = format.compiler.dxPositionW;
    request.reflect = true;
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
        FailShaderJob(result, MetalJobStatus(compiled), diagnostics.View());
        return;
    }

    // 고쳐 쓰기(ATTRIBUTEn·TEXCOORDn location, set·binding)·검증·사용 쌍·값 1과 Metal 인덱스(16.1절
    // SSD36·SSD39)다.
    Array<uint32> raw(allocator);
    Array<uint32> rewritten(allocator);
    Array<uint8> rewrittenBytes(allocator);
    if (spirv.Size() % sizeof(uint32) != 0 || !raw.TryResize(spirv.Size() / sizeof(uint32))) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      "DXC produced a malformed SPIR-V module");
        return;
    }

    platform::MemoryCopy(raw.Data(), spirv.Data(), spirv.Size());
    SpirvRewriteOptions rewrite;
    rewrite.stage = job.stage;
    rewrite.stageLocations = true;
    if (!RewriteSpirv(raw.View(), rewrite, rewritten, diagnostics)) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError, "SPIR-V binding rules",
                      diagnostics.View());
        return;
    }

    if (!rewrittenBytes.TryResize(rewritten.Size() * sizeof(uint32))) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    platform::MemoryCopy(rewrittenBytes.Data(), rewritten.Data(), rewrittenBytes.Size());
    if (!ValidateSpirv(context.tools->SpirvTools(), rewrittenBytes.View(), diagnostics)) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      "spirv-val rejected the rewritten SPIR-V", diagnostics.View());
        return;
    }

    Array<SpirvSamplerUse> pairs(allocator);
    const ShaderToolOutcome queried = QuerySpirvSamplerUses(
        context.tools->SpirvCross(), rewrittenBytes.View(), pairs, diagnostics);
    if (queried != ShaderToolOutcome::Success) {
        FailShaderJob(result, MetalJobStatus(queried), "SPIR-V sampler use query",
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

    SpirvBindingRequest bindingRequest;
    bindingRequest.format = &format;
    bindingRequest.stage = job.stage;
    bindingRequest.requiredFeatures = job.requiredFeatures;
    bindingRequest.layouts = layouts.View();
    bindingRequest.raw = raw.View();
    bindingRequest.rewritten = rewritten.View();
    bindingRequest.uses = uses.View();
    bindingRequest.hasUses = true;
    SpirvBindingResult bound(allocator);
    const ShaderWorkerJobStatus normalized =
        BuildSpirvBindingLayout(bindingRequest, bound, diagnostics);
    if (normalized != ShaderWorkerJobStatus::Success) {
        FailShaderJob(result, normalized, "shader binding rules", diagnostics.View());
        return;
    }

    Array<SpirvMslBinding> mslBindings(allocator);
    for (const MetalResourceBinding& entry : bound.metalBindings) {
        if (!mslBindings.TryPushBack(SpirvMslBinding{entry.set, entry.binding, entry.buffer,
                                                     entry.texture, entry.sampler})) {
            FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
            return;
        }
    }

    SpirvCrossRequest cross;
    cross.spirv = rewrittenBytes.View();
    cross.target = SpirvCrossTarget::Msl;
    cross.stage = job.stage;
    cross.mslBindings = mslBindings.View();
    cross.mslBufferSizeIndex = bound.metalBufferSizeIndex;
    cross.mslMajor = format.compiler.mslVersion.major;
    cross.mslMinor = format.compiler.mslVersion.minor;
    String msl(allocator);
    const ShaderToolOutcome crossed =
        CrossCompileSpirv(context.tools->SpirvCross(), cross, msl, diagnostics);
    if (crossed != ShaderToolOutcome::Success) {
        FailShaderJob(result, MetalJobStatus(crossed), diagnostics.View());
        return;
    }

    char source[MaxShaderToolPathBytes];
    char library[MaxShaderToolPathBytes];
    if (!PathOf(context.scratchDirectory, SourceName, source) ||
        !PathOf(context.scratchDirectory, LibraryName, library) || !WriteText(source, msl.View())) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      "cannot write the MSL source to the scratch directory");
        return;
    }

    char standard[ArgumentBytes];
    char minimum[ArgumentBytes];
    snprintf(standard, sizeof(standard), "-std=macos-metal%u.%u",
             static_cast<unsigned>(format.compiler.mslVersion.major),
             static_cast<unsigned>(format.compiler.mslVersion.minor));
    snprintf(minimum, sizeof(minimum), "-mmacosx-version-min=%u", minimumMacOS);
    const platform::ProcessArgument compileArguments[] = {Argument("-c"),    Argument(standard),
                                                          Argument(minimum), Argument("-o"),
                                                          Argument(AirName), Argument(SourceName)};
    String toolDiagnostics(allocator);
    if (!RunTool(context.metal->executables[MetalCompilerIndex], compileArguments,
                 context.scratchDirectory, toolDiagnostics)) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError, "metal failed",
                      toolDiagnostics.View());
        return;
    }

    const platform::ProcessArgument libraryArguments[] = {Argument("-o"), Argument(LibraryName),
                                                          Argument(AirName)};
    Array<uint8> code(allocator);
    if (!RunTool(context.metal->executables[MetalLibrarianIndex], libraryArguments,
                 context.scratchDirectory, toolDiagnostics) ||
        !ReadBytes(library, code)) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "metallib failed",
                      toolDiagnostics.View());
        return;
    }

    SucceedShaderJob(result, code, bound.bindingLayout);
}

} // namespace rocks::shader
