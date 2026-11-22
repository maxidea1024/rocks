#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Graphics/ShaderSourceCompiler.h>

namespace rocks::shader {

// 개발용 Vulkan 셰이더 컴파일 훅이다(Docs/Design/VulkanGraphics.ko.md 13절). HLSL 원문을 DXC로
// Vulkan 1.1 SPIR-V로 바꾸고(-spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-position-w -Zpr,
// ShaderSystem 형식 VULKAN_SM5의 옵션) RewriteSpirv로 Rocks 규약에 맞춘 뒤 spirv-val로 검증하고
// SPIRV-Reflect로 리플렉션을 만든다. DXC·SPIRV-Tools는 사용자 저장소에서 ShaderToolLoader로 연다.
//
// 개발 프로그램 전용이다. Release Game 타깃은 이 모듈에 닿으면 안 된다(Release 배제 정책). SH2에서
// 쿠킹된 셰이더를 받는 경로가 생기면 이 경로의 규칙은 셰이더 워커의 VULKAN_SM5 형식으로 옮겨진다.
//
// 사용 예:
//   String diagnostics(allocator);
//   auto compiler = shader::VulkanShaderCompiler::Create(allocator, diagnostics);
//   if (!compiler) { printf("%s\n", diagnostics.CStr()); return; }   // NotBootstrapped 안내 등
//   graphics::DeviceDesc desc;
//   desc.shaderCompiler = compiler.Value().Get();
//
// 스레드: 한 번에 한 호출만 받는다(장치가 내부 잠금으로 직렬화한다).
class ROCKS_SHADERFORMATVULKAN_API VulkanShaderCompiler final
    : public graphics::ShaderSourceCompiler {
public:
    // 저장소의 DXC·SPIRV-Tools를 연다. 실패: 이 호스트에 도구가 없거나(Windows 밖) 저장소가
    // 비었거나 다르면 Unsupported·NotFound·DependencyFailed이고 diagnostics에 안내(bootstrap 명령
    // 포함)를 쓴다.
    static Result<UniquePtr<VulkanShaderCompiler>> Create(Allocator& allocator,
                                                          String& diagnostics) noexcept;

    ~VulkanShaderCompiler() noexcept;

    VulkanShaderCompiler(const VulkanShaderCompiler&) = delete;
    VulkanShaderCompiler& operator=(const VulkanShaderCompiler&) = delete;

    Result<void> CompileSpirv(graphics::ShaderStage stage, StringView hlsl,
                              graphics::CompiledShader& out, String& diagnostics) noexcept override;

    // 구현 상태다(도구 함수 표·lease). 공개 API가 아니다.
    struct State;

    explicit VulkanShaderCompiler(UniquePtr<State> state) noexcept;

private:
    UniquePtr<State> state_;
};

} // namespace rocks::shader
