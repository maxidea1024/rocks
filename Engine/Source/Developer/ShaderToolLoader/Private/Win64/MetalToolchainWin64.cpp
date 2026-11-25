#include "../MetalToolchainInternal.h"

#include <Platform/Win64/WideString.h>
#include <stdio.h>

// Windows Metal Developer Tools 위치다(Docs/Design/ShaderSystem 11.1절 SH1e E7, UE 5.8
// MetalShaderFormat.cpp:1055–1072의 기본 설치 위치).

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::shader::metal_toolchain_detail {

namespace metal_windows_detail {

// %ProgramFiles% 값의 UTF-16 단위 상한이다.
constexpr DWORD ProgramFilesUnits = 1024;

} // namespace metal_windows_detail

bool FindMetalExecutables(char (&metal)[MaxShaderToolPathBytes],
                          char (&metallib)[MaxShaderToolPathBytes]) noexcept {
    using namespace metal_windows_detail;
    wchar_t wide[ProgramFilesUnits];
    const DWORD units = GetEnvironmentVariableW(L"ProgramFiles", wide, ProgramFilesUnits);
    if (units == 0 || units >= ProgramFilesUnits) {
        return false;
    }

    char base[MaxShaderToolPathBytes];
    const platform::windows::TextConversion converted = platform::windows::ConvertWideToUtf8(
        wide, units, base, sizeof(base), platform::windows::TextFit::Exact);
    if (!converted) {
        return false;
    }

    const int metalBytes =
        snprintf(metal, sizeof(metal), "%s\\Metal Developer Tools\\metal\\%s\\bin\\metal.exe", base,
                 MetalWindowsToolchainSubversion);
    const int metallibBytes = snprintf(metallib, sizeof(metallib),
                                       "%s\\Metal Developer Tools\\metal\\%s\\bin\\metallib.exe",
                                       base, MetalWindowsToolchainSubversion);
    return metalBytes > 0 && static_cast<usize>(metalBytes) < sizeof(metal) && metallibBytes > 0 &&
           static_cast<usize>(metallibBytes) < sizeof(metallib);
}

} // namespace rocks::shader::metal_toolchain_detail
