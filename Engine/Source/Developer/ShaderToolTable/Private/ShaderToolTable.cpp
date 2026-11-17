#include <ShaderToolTable/ShaderToolTable.h>

namespace rocks::shader {

const ShaderTool* FindShaderTool(StringView name) noexcept {
    for (const ShaderTool& tool : ShaderTools()) {
        if (StringView(tool.name) == name) {
            return &tool;
        }
    }

    return nullptr;
}

const ShaderToolFile* FindShaderToolFile(const ShaderTool& tool, ShaderToolFileRole role) noexcept {
    for (usize index = 0; index < tool.fileCount; ++index) {
        if (tool.files[index].role == role) {
            return &tool.files[index];
        }
    }

    return nullptr;
}

} // namespace rocks::shader
