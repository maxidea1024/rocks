#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <VulkanGraphics/VulkanDevice.h>
#include <stdio.h>

using namespace rocks;
namespace vulkan_device_tests_file {

using namespace rocks::graphics;

// Vulkan 로더 동적 로드와 검증 층 켜기(ShaderSystem P9 완료 기준)를 확인한다. 로더나 Vulkan 1.1
// 장치가 없는 기계(GPU 없는 CI)에서는 SKIP이며 실패가 아니다. 그리기 계약 전체는 Sandbox
// self-test가 확인한다.
TEST(VulkanDevice_LoadsTheSystemLoaderAndCreatesADevice) {
    SystemAllocator allocator;
    DeviceDesc desc;
    desc.enableValidation = true;
    DeviceCreateReport report;
    auto created = CreateVulkanDevice(allocator, desc, &report);
    if (!created) {
        // 실패는 단계·원본 VkResult와 함께 공개되어야 한다(가짜 장치 없음).
        CHECK(report.code == created.Error() && report.stage[0] != '\0');
        printf("[SKIP] VulkanDevice_LoadsTheSystemLoaderAndCreatesADevice: %s: %s\n", report.stage,
               report.detail);
        return;
    }

    Device& device = *created.Value();
    const DeviceInfo info = device.Info();
    CHECK(info.backend == Backend::Vulkan);
    CHECK(info.adapterName.ByteCount() != 0);
    CHECK(info.deviceEpoch != 0);
    CHECK(info.concurrentResourceCreation);
    CHECK(report.code == ErrorCode::None);
    // 장치는 SPIR-V만 받고(다른 종류는 InvalidArgument), 셰이더 없는 파이프라인은
    // InvalidArgument다.
    constexpr uint8 Code[] = {0x03, 0x02, 0x23, 0x07};
    ShaderCodeDesc shader;
    shader.kind = ShaderCodeKind::Dxil;
    shader.code = Span<const uint8>(Code, sizeof(Code));
    CHECK(device.CreateShader(shader).Error() == ErrorCode::InvalidArgument);
    PipelineDesc pipeline;
    CHECK(device.CreatePipeline(pipeline).Error() == ErrorCode::InvalidArgument);
    // 빈 제출의 완료 지점과 회수(제출이 없어도 WaitIdle은 fence 지점을 남긴다).
    REQUIRE(device.WaitIdle());
    CHECK(!device.LastSubmittedFence().IsNull());
    CHECK(device.PendingRetireCount() == 0);
    CHECK(device.ValidationErrorCount() == 0);
}
} // namespace vulkan_device_tests_file
