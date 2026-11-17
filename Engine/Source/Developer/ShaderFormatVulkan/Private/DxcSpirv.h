#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Graphics/ShaderSourceCompiler.h>
#include <ShaderToolLoader/ShaderToolLoader.h>

namespace rocks::shader {

// DXC로 HLSL 진입점(VSMain·PSMain)을 Vulkan 1.1 SPIR-V로 컴파일한다(VULKAN_SM5 옵션
// -spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-position-w -Zpr, 리플렉션용 -fspv-reflect).
// 실패하면 diagnostics에 컴파일러 메시지를 덧붙인다. 컴파일 오류는 DependencyFailed, 이 플랫폼에
// DXC 호출 구현이 없으면 Unsupported다. 구현은 플랫폼 폴더(Win64·Unsupported)에 있다.
Result<void> CompileHlslToSpirv(const DxcFunctions& dxc, graphics::ShaderStage stage,
                                StringView hlsl, Array<uint32>& words,
                                String& diagnostics) noexcept;

} // namespace rocks::shader
