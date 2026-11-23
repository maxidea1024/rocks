// 셰이더 도구용 외부 라이브러리(Engine/Source/ThirdParty)를 엔진 경고 규칙으로 쓰는지
// 확인한다(Docs/Design/ShaderSystem 8절). SPIRV-Reflect는 Rocks 경고 규칙으로 빌드한 C 번역
// 단위이고, 두 헤더(adapter)는 이 C++ 번역 단위가 경고 0으로 include한다.
#include <TestSupport/Test.h>
#include <spirv_reflect.h>
#include <string.h>
#include <vulkan/vulkan.h>

namespace {

// 최소 compute 셰이더(SPIR-V 1.0): OpEntryPoint GLCompute "main", OpExecutionMode LocalSize 4 2 1,
// 빈 본문. id: 1 main, 2 void, 3 함수 타입, 4 label. 마지막 id 다음 값(bound)이 5다.
constexpr uint32_t ComputeModule[] = {
    0x07230203u, 0x00010000u, 0x00000000u,
    5u,          0u,              // magic, version 1.0, generator, bound, schema
    0x00020011u, 1u,              // OpCapability Shader
    0x0003000Eu, 0u,          1u, // OpMemoryModel Logical GLSL450
    0x0005000Fu, 5u,          1u,
    0x6E69616Du, 0x00000000u, // OpEntryPoint GLCompute %1 "main"
    0x00060010u, 1u,          17u,
    4u,          2u,          1u, // OpExecutionMode %1 LocalSize 4 2 1
    0x00020013u, 2u,              // %2 = OpTypeVoid
    0x00030021u, 3u,          2u, // %3 = OpTypeFunction %2
    0x00050036u, 2u,          1u,
    0u,          3u, // %1 = OpFunction %2 None %3
    0x000200F8u, 4u, // %4 = OpLabel
    0x000100FDu,     // OpReturn
    0x00010038u      // OpFunctionEnd
};

} // namespace

TEST(ThirdParty_SpirvReflectReadsComputeEntryPoint) {
    SpvReflectShaderModule module;
    const SpvReflectResult result =
        spvReflectCreateShaderModule(sizeof(ComputeModule), ComputeModule, &module);
    CHECK(result == SPV_REFLECT_RESULT_SUCCESS);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        return;
    }

    CHECK(module.entry_point_count == 1u);
    CHECK(strcmp(module.entry_point_name, "main") == 0);
    CHECK(module.shader_stage == SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT);
    CHECK(module.entry_points[0].local_size.x == 4u && module.entry_points[0].local_size.y == 2u &&
          module.entry_points[0].local_size.z == 1u);
    CHECK(module.descriptor_binding_count == 0u && module.push_constant_block_count == 0u);
    spvReflectDestroyShaderModule(&module);
}

TEST(ThirdParty_SpirvReflectRejectsMalformedModule) {
    // word 단위가 아닌 크기와 잘못된 magic은 오류로 끝나야 한다(할당한 상태는 함수가 정리한다).
    SpvReflectShaderModule module;
    CHECK(spvReflectCreateShaderModule(sizeof(ComputeModule) - 1u, ComputeModule, &module) !=
          SPV_REFLECT_RESULT_SUCCESS);
    uint32_t corrupted[sizeof(ComputeModule) / sizeof(uint32_t)];
    memcpy(corrupted, ComputeModule, sizeof(ComputeModule));
    corrupted[0] = 0xDEADBEEFu;
    CHECK(spvReflectCreateShaderModule(sizeof(corrupted), corrupted, &module) !=
          SPV_REFLECT_RESULT_SUCCESS);
}

TEST(ThirdParty_VulkanHeadersMatchPinnedSdkVersion) {
    // Vulkan-Headers·SPIRV-Reflect·DXC submodule이 같은 Vulkan SDK 1.4.363.0에 고정되어 있다.
    CHECK(VK_HEADER_VERSION == 363);
    CHECK(VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE) == 1u &&
          VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE) == 4u);
    // SPIRV-Reflect의 단계 비트는 Vulkan 정의와 같은 값이다(리플렉션 결과를 Vulkan 파이프라인
    // 정보로 그대로 옮긴다).
    CHECK(static_cast<uint32_t>(SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT) ==
          static_cast<uint32_t>(VK_SHADER_STAGE_COMPUTE_BIT));
    CHECK(static_cast<uint32_t>(SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT) ==
          static_cast<uint32_t>(VK_SHADER_STAGE_FRAGMENT_BIT));
}
