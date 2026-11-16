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

} // namespace rocks::shader
