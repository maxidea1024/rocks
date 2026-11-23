#include <ShaderFormatOpenGL/OpenGLShaderFormat.h>

#include <ShaderToolLoader/ShaderToolCalls.h>
#include <string.h>

// OpenGL ES 형식 처리기(Docs/Design/ShaderSystem 11.1절 SH1e E5)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace opengl_format_detail {

// SPIR-V 모듈 머리 단어 수와 명령 부호(SPIR-V 사양 2.3·3.32)다.
constexpr usize SpirvHeaderWords = 5;
constexpr uint32 SpirvWordCountShift = 16;
constexpr uint32 SpirvOpcodeMask = 0xFFFF;
constexpr uint32 OpTypeImage = 25;
// OpTypeImage의 Sampled·Image Format 단어 위치와 storage image 표지(Sampled = 2)다.
constexpr usize ImageSampledWord = 7;
constexpr usize ImageFormatWord = 8;
constexpr uint32 StorageImageSampled = 2;
// unsupportedImageFormats 비트 수다.
constexpr uint32 ImageFormatMaskBits = 64;

ShaderWorkerJobStatus OpenGLJobStatus(ShaderToolOutcome outcome) noexcept {
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

} // namespace opengl_format_detail

usize FindUnsupportedImageFormat(Span<const uint8> spirv, uint64 unsupported) noexcept {
    using namespace opengl_format_detail;
    if (unsupported == 0 || spirv.Size() % sizeof(uint32) != 0) {
        return IndexNone;
    }

    const usize count = spirv.Size() / sizeof(uint32);
    usize index = SpirvHeaderWords;
    while (index < count) {
        uint32 instruction = 0;
        memcpy(&instruction, spirv.Data() + index * sizeof(uint32), sizeof(uint32));
        const usize length = instruction >> SpirvWordCountShift;
        if (length == 0 || index + length > count) {
            return IndexNone;
        }

        if ((instruction & SpirvOpcodeMask) == OpTypeImage && length > ImageFormatWord) {
            uint32 sampled = 0;
            uint32 format = 0;
            memcpy(&sampled, spirv.Data() + (index + ImageSampledWord) * sizeof(uint32),
                   sizeof(uint32));
            memcpy(&format, spirv.Data() + (index + ImageFormatWord) * sizeof(uint32),
                   sizeof(uint32));
            // SPIR-V image format 값은 ImageFormatName 색인 + 1이다(0은 Unknown).
            if (sampled == StorageImageSampled && format != 0 && format <= ImageFormatMaskBits &&
                (unsupported & (uint64{1} << (format - 1))) != 0) {
                return format - 1;
            }
        }

        index += length;
    }

    return IndexNone;
}

void OpenGLShaderFormatBackend::Compile(const ShaderFormatDefinition& format,
                                        const ShaderWorkerJob& job,
                                        const ShaderBackendContext& context,
                                        ShaderWorkerJobResult& result) noexcept {
    using namespace opengl_format_detail;
    if (format.compiler.path != ShaderCompilerPath::DxcSpirvEssl ||
        format.compiler.spirvTargetEnv != SpirvTargetEnv::Vulkan1_1) {
        FailShaderJob(result, ShaderWorkerJobStatus::UnsupportedCapability,
                      "the OpenGL processor does not implement the format's compiler path",
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

    Allocator& allocator = result.values.GetAllocator();
    SpirvCompileRequest request;
    request.source = job.source.View();
    request.entry = job.entry.View();
    request.profile = StringView(profile);
    request.hlsl2021 = job.options.hlsl2021;
    request.optimization = job.options.optimization;
    request.dxPositionW = format.compiler.dxPositionW;
    Array<uint8> spirv(allocator);
    String diagnostics(allocator);
    const ShaderToolOutcome compiled =
        CompileHlslToSpirv(*context.tools, request, spirv, diagnostics);
    if (compiled != ShaderToolOutcome::Success) {
        FailShaderJob(result, OpenGLJobStatus(compiled), diagnostics.View());
        return;
    }

    const usize unsupported =
        FindUnsupportedImageFormat(spirv.View(), format.compiler.unsupportedImageFormats);
    if (unsupported != IndexNone) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError,
                      "the format does not support this storage image format",
                      StringView(ImageFormatName(unsupported)));
        return;
    }

    SpirvCrossRequest cross;
    cross.spirv = spirv.View();
    cross.target = SpirvCrossTarget::Essl;
    cross.esslVersion = format.compiler.esslVersion;
    cross.fixupClipSpace = format.compiler.fixupClipSpace;
    cross.flipVertexY = format.compiler.flipVertexY;
    String essl(allocator);
    const ShaderToolOutcome crossed =
        CrossCompileSpirv(context.tools->SpirvCross(), cross, essl, diagnostics);
    if (crossed != ShaderToolOutcome::Success) {
        FailShaderJob(result, OpenGLJobStatus(crossed), diagnostics.View());
        return;
    }

    result.status = ShaderWorkerJobStatus::Success;
    result.diagnostics.Clear();
    result.values.Clear();
    auto code = result.values.TryEmplaceBack(allocator);
    if (!code || !code.Value()->bytes.TryAppend(Span<const uint8>(
                     reinterpret_cast<const uint8*>(essl.Data()), essl.ByteCount()))) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    code.Value()->id = 0;
}

} // namespace rocks::shader
