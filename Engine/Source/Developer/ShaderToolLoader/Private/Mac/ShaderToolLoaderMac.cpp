#include <Platform/Path.h>
#include <ShaderToolLoader/ShaderToolCalls.h>
#include <ShaderToolLoader/ShaderToolLoader.h>
#include <string.h>

namespace rocks::shader {

namespace {

// 저장소 루트의 하위 경로다(Rocks.Tooling.Common ThirdPartyStore.DefaultRoot와 같다).
constexpr char StoreSuffix[] = "/Rocks/ThirdParty";

} // namespace

// macOS 저장소는 ~/Library/Application Support(PlatformBase UserData) 아래다. OS가 지울 수 있는
// Library/Caches(UserCache)는 쓰지 않는다.
platform::FileStatus GetShaderToolStoreRoot(char* buffer, usize capacity, usize& bytes) noexcept {
    bytes = 0;
    usize base = 0;
    if (const platform::FileStatus found = platform::GetStandardDirectory(
            platform::StandardDirectory::UserData, buffer, capacity, base);
        !found) {
        if (found.code == platform::FileCode::Overflow) {
            bytes = base + sizeof(StoreSuffix) - 1;
        }

        return found;
    }

    const usize total = base + sizeof(StoreSuffix) - 1;
    if (total + 1 > capacity) {
        bytes = total;
        return {platform::FileCode::Overflow, 0};
    }

    memcpy(buffer + base, StoreSuffix, sizeof(StoreSuffix));
    bytes = total;
    return {};
}

// macOS DXC 산출물(dylib, WinAdapter.h COM 대응)은 아직 없다(P3 macOS 산출물 후속).
ShaderToolLoadStatus QueryDxcVersion(const DxcFunctions&, uint32& major, uint32& minor) noexcept {
    major = 0;
    minor = 0;
    ShaderToolLoadStatus status;
    status.code = ShaderToolLoadCode::UnsupportedHost;
    status.tool = DxcToolName;
    return status;
}

// DXC 컴파일은 Windows 구현만 있다(dxcapi.h를 Windows 헤더와 함께 include하는 Private/Win64).
ShaderToolLoadStatus CompileWithDxc(const DxcFunctions&, const DxcCompileRequest&,
                                    DxcCompileResult& result) noexcept {
    result.object.Clear();
    result.diagnostics.Clear();
    result.compiled = false;
    ShaderToolLoadStatus status;
    status.code = ShaderToolLoadCode::UnsupportedHost;
    status.tool = DxcToolName;
    return status;
}

// FXC는 Windows 도구다(Windows 밖에는 구현이 없다 — 11.1절 SH1e E3).
ShaderToolOutcome CompileWithFxc(const FxcFunctions&, const FxcCompileRequest&,
                                 Array<uint8>& object, String& diagnostics, FxcSymbols*) noexcept {
    object.Clear();
    diagnostics.Clear();
    (void)diagnostics.TryAppend("FXC runs only on Windows");
    return ShaderToolOutcome::UnsupportedHost;
}

} // namespace rocks::shader
