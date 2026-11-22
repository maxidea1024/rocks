#include <ShaderToolLoader/ShaderToolLoader.h>

// 셰이더 도구는 개발 호스트(Windows·macOS)에서만 돈다. 그 밖의 플랫폼(Android)은 저장소가 없다.
namespace rocks::shader {

platform::FileStatus GetShaderToolStoreRoot(char*, usize, usize& bytes) noexcept {
    bytes = 0;
    return {platform::FileCode::Unsupported, 0};
}

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

} // namespace rocks::shader
