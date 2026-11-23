#include "../MetalToolchainInternal.h"

#include <Platform/Process.h>
#include <string.h>

// macOS Metal 도구 위치다(Docs/Design/ShaderSystem 11.1절 SH1e E7, UE 5.8
// MetalShaderFormat.cpp:1008의 xcrun --sdk macosx --find). 이 기계(Windows)에서 실행 확인하지
// 못했다.
namespace rocks::shader::metal_toolchain_detail {

namespace metal_mac_detail {

constexpr char Xcrun[] = "/usr/bin/xcrun";

// xcrun --sdk macosx --find <tool>의 첫 줄(절대 경로)을 out에 둔다.
bool FindTool(const char* tool, char (&out)[MaxShaderToolPathBytes]) noexcept {
    const platform::ProcessArgument arguments[] = {
        {"--sdk", 5}, {"macosx", 6}, {"--find", 6}, {tool, strlen(tool)}};
    platform::ProcessStartInfo info;
    info.executable = Xcrun;
    info.executableBytes = sizeof(Xcrun) - 1;
    info.arguments = arguments;
    info.argumentCount = sizeof(arguments) / sizeof(arguments[0]);
    info.output = platform::ProcessStreamMode::Pipe;
    info.error = platform::ProcessStreamMode::Null;
    platform::ProcessRunOptions options;
    options.output = out;
    options.outputCapacity = sizeof(out) - 1;
    options.timeoutMilliseconds = MetalToolTimeoutMilliseconds;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    if (!run.status || run.timedOut || run.outputTruncated ||
        run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0) {
        out[0] = '\0';
        return false;
    }

    usize bytes = run.outputBytes;
    while (bytes > 0 && (out[bytes - 1] == '\n' || out[bytes - 1] == '\r')) {
        --bytes;
    }

    out[bytes] = '\0';
    return bytes > 0 && out[0] == '/' && memchr(out, '\n', bytes) == nullptr;
}

} // namespace metal_mac_detail

bool FindMetalExecutables(char (&metal)[MaxShaderToolPathBytes],
                          char (&metallib)[MaxShaderToolPathBytes]) noexcept {
    using namespace metal_mac_detail;
    return FindTool("metal", metal) && FindTool("metallib", metallib);
}

} // namespace rocks::shader::metal_toolchain_detail
