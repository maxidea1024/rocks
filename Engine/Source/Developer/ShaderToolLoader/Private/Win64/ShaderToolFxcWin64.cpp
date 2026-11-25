#include <Platform/Win64/ComRef.h>
#include <ShaderToolLoader/ShaderToolCalls.h>
#include <stdio.h>
#include <string.h>

// FXC 호출(Docs/Design/ShaderSystem 11.1절 SH1e E3)이다. d3dcompiler.h는 Windows SDK 헤더라
// Windows 헤더 묶음 안에서 include한다(함수는 표에서 얻은 주소로 부르고 링크하지 않는다).

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>
#include <Unknwn.h>
#include <d3dcompiler.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::shader {

namespace fxc_call_detail {

// 최적화 단계(O0–O3)의 D3DCOMPILE 플래그다.
constexpr UINT OptimizationFlags[ShaderOptimizationLevels] = {
    D3DCOMPILE_OPTIMIZATION_LEVEL0, D3DCOMPILE_OPTIMIZATION_LEVEL1, D3DCOMPILE_OPTIMIZATION_LEVEL2,
    D3DCOMPILE_OPTIMIZATION_LEVEL3};
// HRESULT 진단 버퍼 크기다.
constexpr usize HresultTextBytes = 64;

void AppendBlob(String& diagnostics, ID3DBlob* blob) noexcept {
    if (blob == nullptr) {
        return;
    }

    // FXC 문구 blob은 끝 NUL을 크기에 넣는다.
    const char* text = static_cast<const char*>(blob->GetBufferPointer());
    usize bytes = blob->GetBufferSize();
    while (bytes > 0 && text[bytes - 1] == '\0') {
        --bytes;
    }

    (void)diagnostics.TryAppend(StringView(text, bytes));
}

} // namespace fxc_call_detail

ShaderToolOutcome CompileWithFxc(const FxcFunctions& fxc, const FxcCompileRequest& request,
                                 Array<uint8>& object, String& diagnostics) noexcept {
    using namespace fxc_call_detail;
    object.Clear();
    diagnostics.Clear();
    const auto compile = reinterpret_cast<pD3DCompile>(fxc.address[FxcFunctions::Compile]);
    if (compile == nullptr) {
        (void)diagnostics.TryAppend("FXC is not open");
        return ShaderToolOutcome::ToolError;
    }

    if (request.optimization >= ShaderOptimizationLevels) {
        (void)diagnostics.TryAppend("invalid optimization level");
        return ShaderToolOutcome::CompileError;
    }

    // 엔트리·프로필은 NUL 종료 문자열로 넘긴다.
    char entry[MaxShaderToolPathBytes];
    char profile[MaxShaderToolPathBytes];
    const int entryBytes =
        snprintf(entry, sizeof(entry), "%.*s", static_cast<int>(request.entry.ByteCount()),
                 request.entry.Data());
    const int profileBytes =
        snprintf(profile, sizeof(profile), "%.*s", static_cast<int>(request.profile.ByteCount()),
                 request.profile.Data());
    if (entryBytes <= 0 || static_cast<usize>(entryBytes) >= sizeof(entry) || profileBytes <= 0 ||
        static_cast<usize>(profileBytes) >= sizeof(profile)) {
        (void)diagnostics.TryAppend("invalid FXC entry or profile");
        return ShaderToolOutcome::CompileError;
    }

    const UINT flags = OptimizationFlags[request.optimization] | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR |
                       D3DCOMPILE_WARNINGS_ARE_ERRORS;
    platform::windows::ComRef<ID3DBlob> code;
    platform::windows::ComRef<ID3DBlob> errors;
    const HRESULT compiled =
        compile(request.source.Data(), request.source.ByteCount(), nullptr, nullptr, nullptr, entry,
                profile, flags, 0, code.Out(), errors.Out());
    if (SUCCEEDED(compiled) && code) {
        AppendBlob(diagnostics, errors.Get());
        if (!object.TryResize(code->GetBufferSize())) {
            (void)diagnostics.TryAppend("out of memory");
            return ShaderToolOutcome::ToolError;
        }

        memcpy(object.Data(), code->GetBufferPointer(), object.Size());
        return ShaderToolOutcome::Success;
    }

    // E_FAIL과 오류 blob만 원문 탓의 컴파일 오류다(SC10 — 그 밖은 도구 오류).
    if (compiled == E_FAIL && errors) {
        AppendBlob(diagnostics, errors.Get());
        return ShaderToolOutcome::CompileError;
    }

    char text[HresultTextBytes];
    snprintf(text, sizeof(text), "FXC failed with HRESULT 0x%08lx",
             static_cast<unsigned long>(compiled));
    (void)diagnostics.TryAppend(StringView(text));
    AppendBlob(diagnostics, errors.Get());
    return ShaderToolOutcome::ToolError;
}

} // namespace rocks::shader
