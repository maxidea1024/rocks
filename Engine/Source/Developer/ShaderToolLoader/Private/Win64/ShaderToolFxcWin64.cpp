#include <Core/Move.h>
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

// D3DGetBlobPart·D3DStripShader 함수 형이다(SDK 헤더에 함수 포인터 typedef가 없다).
using GetBlobPartFunction = HRESULT(WINAPI*)(LPCVOID data, SIZE_T bytes, D3D_BLOB_PART part,
                                             UINT flags, ID3DBlob** out);
using StripShaderFunction = HRESULT(WINAPI*)(LPCVOID data, SIZE_T bytes, UINT flags,
                                             ID3DBlob** out);

// D3D_BLOB_DEBUG_NAME part 머리(uint16 flags, uint16 이름 길이 — NUL 제외)다(d3dcompiler.h의
// D3D_BLOB_DEBUG_NAME 설명).
constexpr usize DebugNameHeaderBytes = 4;

// code에서 PDB·디버그 이름을 symbols에 꺼내고 code를 디버그 정보를 뗀 DXBC로 바꾼다(16.3절 SSD73).
// 실패하면 진단을 남기고 false다.
bool ExtractSymbols(const FxcFunctions& fxc, platform::windows::ComRef<ID3DBlob>& code,
                    FxcSymbols& symbols, String& diagnostics) noexcept {
    const auto getPart =
        reinterpret_cast<GetBlobPartFunction>(fxc.address[FxcFunctions::GetBlobPart]);
    const auto strip =
        reinterpret_cast<StripShaderFunction>(fxc.address[FxcFunctions::StripShader]);
    if (getPart == nullptr || strip == nullptr) {
        (void)diagnostics.TryAppend("FXC debug symbol functions are not open");
        return false;
    }

    platform::windows::ComRef<ID3DBlob> pdb;
    if (FAILED(
            getPart(code->GetBufferPointer(), code->GetBufferSize(), D3D_BLOB_PDB, 0, pdb.Out())) ||
        !pdb || !symbols.pdb.TryResize(pdb->GetBufferSize())) {
        (void)diagnostics.TryAppend("FXC produced no PDB");
        return false;
    }

    memcpy(symbols.pdb.Data(), pdb->GetBufferPointer(), symbols.pdb.Size());
    platform::windows::ComRef<ID3DBlob> name;
    symbols.name.Clear();
    if (SUCCEEDED(getPart(code->GetBufferPointer(), code->GetBufferSize(), D3D_BLOB_DEBUG_NAME, 0,
                          name.Out())) &&
        name && name->GetBufferSize() > DebugNameHeaderBytes) {
        const auto* bytes = static_cast<const uint8*>(name->GetBufferPointer());
        const usize length = static_cast<usize>(bytes[2]) | (static_cast<usize>(bytes[3]) << 8);
        if (length <= name->GetBufferSize() - DebugNameHeaderBytes) {
            (void)symbols.name.TryAssign(
                StringView(reinterpret_cast<const char*>(bytes + DebugNameHeaderBytes), length));
        }
    }

    platform::windows::ComRef<ID3DBlob> stripped;
    if (FAILED(strip(code->GetBufferPointer(), code->GetBufferSize(), D3DCOMPILER_STRIP_DEBUG_INFO,
                     stripped.Out())) ||
        !stripped) {
        (void)diagnostics.TryAppend("FXC could not strip the debug information");
        return false;
    }

    code = Move(stripped);
    return true;
}

} // namespace fxc_call_detail

ShaderToolOutcome CompileWithFxc(const FxcFunctions& fxc, const FxcCompileRequest& request,
                                 Array<uint8>& object, String& diagnostics,
                                 FxcSymbols* symbols) noexcept {
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

    const bool debug = request.debugSymbols && symbols != nullptr;
    const UINT flags = OptimizationFlags[request.optimization] | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR |
                       D3DCOMPILE_WARNINGS_ARE_ERRORS |
                       (debug ? D3DCOMPILE_DEBUG | D3DCOMPILE_DEBUG_NAME_FOR_BINARY : 0u);
    platform::windows::ComRef<ID3DBlob> code;
    platform::windows::ComRef<ID3DBlob> errors;
    const HRESULT compiled =
        compile(request.source.Data(), request.source.ByteCount(), nullptr, nullptr, nullptr, entry,
                profile, flags, 0, code.Out(), errors.Out());
    if (SUCCEEDED(compiled) && code) {
        AppendBlob(diagnostics, errors.Get());
        if (debug && !ExtractSymbols(fxc, code, *symbols, diagnostics)) {
            return ShaderToolOutcome::ToolError;
        }

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
