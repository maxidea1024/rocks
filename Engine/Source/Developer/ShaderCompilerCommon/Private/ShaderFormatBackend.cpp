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

bool DecodeShaderJobLayouts(const ShaderWorkerJob& job, ShaderParameterLayoutSet& out,
                            ShaderWorkerJobResult& result) noexcept {
    const ShaderReadStatus read = DecodeShaderParameterLayouts(job.layout.View(), out);
    if (!read) {
        FailShaderJob(result, ShaderWorkerJobStatus::CompileError,
                      "shader job parameter layout bytes are invalid", StringView(read.reason));
        return false;
    }

    return true;
}

void SucceedShaderJob(ShaderWorkerJobResult& result, Array<uint8>& code,
                      Array<uint8>& bindingLayout) noexcept {
    result.status = ShaderWorkerJobStatus::Success;
    result.diagnostics.Clear();
    result.values.Clear();
    // 값 id 오름차순(0 Code, 1 BindingLayout)이다.
    constexpr uint32 CodeValue = 0;
    constexpr uint32 BindingLayoutValue = 1;
    auto value = result.values.TryEmplaceBack(result.values.GetAllocator());
    if (!value) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    value.Value()->id = CodeValue;
    value.Value()->bytes = Move(code);
    if (bindingLayout.IsEmpty()) {
        return;
    }

    auto layout = result.values.TryEmplaceBack(result.values.GetAllocator());
    if (!layout) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    layout.Value()->id = BindingLayoutValue;
    layout.Value()->bytes = Move(bindingLayout);
}

void SucceedShaderJob(ShaderWorkerJobResult& result, Array<uint8>& code,
                      Array<uint8>& bindingLayout, Array<uint8>& glesHeader) noexcept {
    SucceedShaderJob(result, code, bindingLayout);
    if (result.status != ShaderWorkerJobStatus::Success || glesHeader.IsEmpty()) {
        return;
    }

    constexpr uint32 GlesCopyTableValue = 2;
    auto value = result.values.TryEmplaceBack(result.values.GetAllocator());
    if (!value) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    value.Value()->id = GlesCopyTableValue;
    value.Value()->bytes = Move(glesHeader);
}

void AddShaderJobDebugSymbols(ShaderWorkerJobResult& result, ShaderDebugSymbolKind kind,
                              StringView name, Span<const uint8> contents) noexcept {
    if (result.status != ShaderWorkerJobStatus::Success) {
        return;
    }

    constexpr uint32 DebugSymbolsValue = 3;
    Array<uint8> bytes(result.values.GetAllocator());
    if (auto encoded = EncodeShaderDebugSymbols(kind, name, contents, bytes); !encoded) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      encoded.Error() == ErrorCode::OutOfMemory
                          ? StringView("out of memory")
                          : StringView("the shader debug symbol file name is invalid"),
                      name);
        return;
    }

    auto value = result.values.TryEmplaceBack(result.values.GetAllocator());
    if (!value) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError, "out of memory");
        return;
    }

    value.Value()->id = DebugSymbolsValue;
    value.Value()->bytes = Move(bytes);
}

void AddShaderJobWarnings(ShaderWorkerJobResult& result, StringView stage,
                          StringView warnings) noexcept {
    if (result.status != ShaderWorkerJobStatus::Success || warnings.IsEmpty()) {
        return;
    }

    const bool appended = result.diagnostics.TryAppend(stage) &&
                          result.diagnostics.TryAppend(": ") &&
                          result.diagnostics.TryAppend(warnings) &&
                          (warnings.EndsWith("\n", StringComparison::CaseSensitive) ||
                           result.diagnostics.TryAppend("\n"));
    if (!appended) {
        FailShaderJob(result, ShaderWorkerJobStatus::ToolError,
                      "out of memory while keeping shader warnings");
    }
}

} // namespace rocks::shader
