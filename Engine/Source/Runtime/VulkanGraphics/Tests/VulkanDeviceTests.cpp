#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <VulkanGraphics/VulkanDevice.h>
#include <stdio.h>

using namespace rocks;
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
    // 컴파일 훅이 없으면 파이프라인은 SH2 전까지 Unsupported로 공개한다.
    PipelineDesc pipeline;
    constexpr char Source[] = "float4 VSMain() : SV_Position { return 0; }";
    pipeline.vertex.hlsl = StringView(Source, sizeof(Source) - 1);
    pipeline.pixel.hlsl = pipeline.vertex.hlsl;
    CHECK(device.CreatePipeline(pipeline).Error() == ErrorCode::Unsupported);
    // 빈 제출의 완료 지점과 회수(제출이 없어도 WaitIdle은 fence 지점을 남긴다).
    REQUIRE(device.WaitIdle());
    CHECK(!device.LastSubmittedFence().IsNull());
    CHECK(device.PendingRetireCount() == 0);
    CHECK(device.ValidationErrorCount() == 0);
}
