#include "DxcSpirv.h"

namespace rocks::shader {

// DXC 호출(COM 형식의 dxcapi.h)은 Windows 구현만 있다. 다른 호스트는 동작하는 척하지 않는다.
Result<void> CompileHlslToSpirv(const DxcFunctions&, graphics::ShaderStage, StringView,
                                Array<uint32>&, String& diagnostics) noexcept {
    auto appended = diagnostics.TryAppend(StringView("DXC is only wired on Windows hosts"));
    (void)appended;
    return Result<void>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::shader
