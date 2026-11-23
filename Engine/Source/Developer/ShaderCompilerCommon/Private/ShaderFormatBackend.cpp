#include <ShaderCompilerCommon/ShaderFormatBackend.h>

#include <stdio.h>

// 형식 구현이 함께 쓰는 도구다(Docs/Design/ShaderSystem 11.1절 SC1).
namespace rocks::shader {

bool MakeShaderProfile(uint32 stage, ShaderVersion model,
                       char (&out)[MaxShaderProfileBytes]) noexcept {
    const char* prefix = nullptr;
    switch (stage) {
    case ShaderStageVertex:
        prefix = "vs";
        break;
    case ShaderStagePixel:
        prefix = "ps";
        break;
    case ShaderStageCompute:
        prefix = "cs";
        break;
    default:
        out[0] = '\0';
        return false;
    }

    const int written =
        snprintf(out, sizeof(out), "%s_%u_%u", prefix, static_cast<unsigned>(model.major),
                 static_cast<unsigned>(model.minor));
    return written > 0 && static_cast<usize>(written) < sizeof(out);
}

void FailShaderJob(ShaderWorkerJobResult& result, ShaderWorkerJobStatus status, StringView message,
                   StringView detail) noexcept {
    result.status = status;
    result.values.Clear();
    result.diagnostics.Clear();
    // 진단 쓰기 실패(메모리)는 상태만 남긴다.
    if (result.diagnostics.TryAppend(message) && !detail.IsEmpty()) {
        (void)(result.diagnostics.TryAppend(": ") && result.diagnostics.TryAppend(detail));
    }
}

} // namespace rocks::shader
