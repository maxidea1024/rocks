#include "SpikeTools.h"

// DXC(COM)·FXC·D3D12·WGL 구현이 없는 플랫폼이다. macOS 스파이크(S1 macOS DXC 서명, S3 호스트 사이
// 비교)는 Private/Mac 구현으로 후속이며, 그때까지 이 파일이 미지원을 보고한다.
namespace rocks::spike {

DxcOutput::DxcOutput() noexcept
    : code(SpikeAllocator()), reflection(SpikeAllocator()), diagnostics(SpikeAllocator()) {
}

bool CompileWithDxc(const shader::DxcFunctions&, const ShaderInput&, DxcTarget, const char*,
                    DxcOutput& output) noexcept {
    output.diagnostics.Append(StringView("DXC is not implemented on this platform"));
    return false;
}

bool DumpDxilReflection(const shader::DxcFunctions&, Span<const uint8>, Report&) noexcept {
    return false;
}

bool CompileWithFxc(const shader::FxcFunctions&, StringView, const char*, const char*, const char*,
                    ByteArray&, StringBuilderBase& diagnostics) noexcept {
    diagnostics.Append(StringView("FXC is Windows only"));
    return false;
}

void ProbeD3D12ShaderModel(Report& report) noexcept {
    report.BeginObject("d3d12");
    report.String("error", StringView("unsupported platform"));
    report.EndObject();
}

void ProbeOpenGl(StringView, StringView, Report& report) noexcept {
    report.BeginObject("opengl");
    report.String("error", StringView("unsupported platform"));
    report.EndObject();
}

} // namespace rocks::spike
