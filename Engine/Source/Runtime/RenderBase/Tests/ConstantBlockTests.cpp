#include "RecordingDevice.h"

#include <Core/Memory/SystemAllocator.h>
#include <RenderBase/ShaderParameters.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <TestSupport/Test.h>

// 상수 블록의 장치 경로 시험이다(Docs/Design/RenderBase 9.11절 7항 GX6): 수명별 commit(PerDraw
// 없음, PerFrame SingleFrame·값이 바뀌면 새 버퍼·옛것 Retire, Persistent MultiFrame·Update), 지도
// 자리마다 묶기(PerDraw는 SetConstantData, 그 밖은 SetConstantBuffer), commit 전 거부, Release와
// 지연 회수.
using namespace rocks;
using namespace rocks::render;
using namespace rocks::shader;
using rocks::render::testing::RecordingDevice;

namespace constant_block_tests_file {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

constexpr ShaderParameterMember MaterialMembers[] = {{"color", ShaderParameterType::Float4, 0, 0}};
constexpr ShaderParameterLayout MaterialLayout = {"Material", false, 16, MaterialMembers};
constexpr ShaderParameterMember TintMembers[] = {{"tint", ShaderParameterType::Float4, 0, 0}};
constexpr ShaderParameterLayout TintLayout = {"Tint", true, 16, TintMembers};

// VS slot 1·PS slot 4에 Material을 둔 단계 layout이다.
struct Stages {
    Stages() noexcept : vertex(Allocator_()), pixel(Allocator_()) {
        vertex.stage = ShaderStageVertex;
        pixel.stage = ShaderStagePixel;
        Add(vertex, 1);
        Add(pixel, 4);
    }

    static void Add(ShaderBindingLayout& layout, uint16 slot) noexcept {
        ShaderBindingMember member{"color", 0, 16};
        (void)layout.members.TryPushBack(member);
        ShaderBindingResource buffer;
        buffer.kind = ShaderResourceKind::ConstantBuffer;
        buffer.slot = slot;
        buffer.name = "Material";
        buffer.constantBufferBytes = 16;
        buffer.firstMember = static_cast<uint32>(layout.members.Size() - 1);
        buffer.memberCount = 1;
        (void)layout.resources.TryPushBack(buffer);
    }

    ShaderBindingLayout vertex;
    ShaderBindingLayout pixel;
};

uint32 WordOf(float value) noexcept {
    uint32 word = 0;
    platform::MemoryCopy(&word, &value, sizeof(word));
    return word;
}

// 기록에서 op의 value 목록 가운데 n번째다(없으면 0).
uint64 NthValue(const RecordingDevice& device, RecordingDevice::Op op, usize n) noexcept {
    usize seen = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        if (call.op == op && seen++ == n) {
            return call.value;
        }
    }

    return 0;
}

usize CountOps(const RecordingDevice& device, RecordingDevice::Op op) noexcept {
    usize count = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        count += call.op == op ? 1u : 0u;
    }

    return count;
}

TEST(ConstantBlock_CommitByLifetimeAndApplyToPlacements) {
    RecordingDevice device(Allocator_());
    Stages stages;
    const ShaderBindingLayout* layouts[] = {&stages.vertex, &stages.pixel};
    auto map = ShaderBindingMap::Build(MaterialLayout, layouts);
    REQUIRE(map);
    REQUIRE(map.Value().Placements().Size() == 2);
    CHECK(ShaderStageOfBit(ShaderStageVertex) == graphics::ShaderStage::Vertex &&
          ShaderStageOfBit(ShaderStagePixel) == graphics::ShaderStage::Pixel &&
          ShaderStageOfBit(ShaderStageCompute) == graphics::ShaderStage::Compute);
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};

    graphics::PassDesc pass;
    auto recording = device.BeginPass(pass);
    REQUIRE(recording);
    graphics::CommandContext& commands = *recording.Value();

    // PerDraw: 장치 버퍼 없이 자리마다 SetConstantData.
    ConstantBlock perDraw(Allocator_(), MaterialLayout, ConstantBlockLifetime::PerDraw);
    CHECK(perDraw.Commit(device).Error() == ErrorCode::InvalidArgument);
    REQUIRE(perDraw.Initialize());
    REQUIRE(perDraw.SetValue("color", red));
    REQUIRE(perDraw.Commit(device));
    CHECK(perDraw.Buffer().IsNull() && perDraw.IsCommitted());
    REQUIRE(ApplyConstantBlock(commands, map.Value(), perDraw));
    CHECK(CountOps(device, RecordingDevice::Op::SetConstantData) == 2);
    CHECK(NthValue(device, RecordingDevice::Op::SetConstantData, 0) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Vertex, 1, WordOf(1.0f)));
    CHECK(NthValue(device, RecordingDevice::Op::SetConstantData, 1) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Pixel, 4, WordOf(1.0f)));

    // PerFrame: commit 전 묶기는 거부, commit은 SingleFrame 하나, 같은 값이면 다시 만들지 않고,
    // 값이 바뀌면 새로 만들고 옛것을 Retire한다.
    ConstantBlock perFrame(Allocator_(), MaterialLayout, ConstantBlockLifetime::PerFrame);
    REQUIRE(perFrame.Initialize());
    REQUIRE(perFrame.SetValue("color", red));
    CHECK(ApplyConstantBlock(commands, map.Value(), perFrame).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(perFrame.Commit(device));
    REQUIRE(perFrame.Commit(device));
    CHECK(CountOps(device, RecordingDevice::Op::CreateConstantBuffer) == 1);
    CHECK(NthValue(device, RecordingDevice::Op::CreateConstantBuffer, 0) ==
          ((static_cast<uint64>(graphics::ConstantBufferLifetime::SingleFrame) << 32) | 16));
    REQUIRE(ApplyConstantBlock(commands, map.Value(), perFrame));
    CHECK(NthValue(device, RecordingDevice::Op::SetConstantBuffer, 1) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Pixel, 4, WordOf(1.0f)));
    const graphics::ConstantBufferHandle first = perFrame.Buffer();
    REQUIRE(perFrame.SetValue("color", green));
    CHECK(!perFrame.IsCommitted());
    REQUIRE(perFrame.Commit(device));
    CHECK(!(perFrame.Buffer() == first));
    CHECK(CountOps(device, RecordingDevice::Op::CreateConstantBuffer) == 2 &&
          CountOps(device, RecordingDevice::Op::RetireConstantBuffer) == 1);
    REQUIRE(ApplyConstantBlock(commands, map.Value(), perFrame));
    CHECK(NthValue(device, RecordingDevice::Op::SetConstantBuffer, 2) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Vertex, 1, 0));

    // Persistent: MultiFrame 하나를 만들고 바뀐 version만 Update한다(버퍼는 그대로).
    ConstantBlock persistent(Allocator_(), MaterialLayout, ConstantBlockLifetime::Persistent);
    REQUIRE(persistent.Initialize());
    REQUIRE(persistent.SetValue("color", red));
    REQUIRE(persistent.Commit(device));
    const graphics::ConstantBufferHandle kept = persistent.Buffer();
    REQUIRE(persistent.SetValue("color", green));
    REQUIRE(persistent.Commit(device));
    REQUIRE(persistent.Commit(device));
    CHECK(persistent.Buffer() == kept);
    CHECK(CountOps(device, RecordingDevice::Op::UpdateConstantBuffer) == 1 &&
          NthValue(device, RecordingDevice::Op::UpdateConstantBuffer, 0) == 0);

    // 표가 다르거나 루트 상수 표면 거부한다.
    ConstantBlock other(Allocator_(), TintLayout, ConstantBlockLifetime::PerDraw);
    REQUIRE(other.Initialize());
    CHECK(ApplyConstantBlock(commands, map.Value(), other).Error() == ErrorCode::InvalidArgument);
    ShaderBindingLayout rooted(Allocator_());
    rooted.stage = ShaderStagePixel;
    rooted.rootConstantBytes = 16;
    const ShaderBindingLayout* rootStages[] = {&rooted};
    auto tint = ShaderBindingMap::Build(TintLayout, rootStages);
    REQUIRE(tint);
    CHECK(ApplyConstantBlock(commands, tint.Value(), other).Error() == ErrorCode::InvalidArgument);
    CHECK(ApplyConstantBuffer(commands, tint.Value(), kept).Error() == ErrorCode::InvalidArgument);
    REQUIRE(device.EndFrame());

    // Release는 Retire이고 회수는 마지막 사용 fence 뒤다.
    const usize retiredBefore = CountOps(device, RecordingDevice::Op::RetireConstantBuffer);
    perFrame.Release(device);
    persistent.Release(device);
    perFrame.Release(device);
    CHECK(perFrame.Buffer().IsNull() && persistent.Buffer().IsNull());
    CHECK(CountOps(device, RecordingDevice::Op::RetireConstantBuffer) == retiredBefore + 2);
    CHECK(device.LiveConstantBuffers() == 0);
    REQUIRE(device.WaitIdle());
    CHECK(device.PendingRetireCount() == 0);
}

TEST(ConstantBlock_RecordingDeviceKeepsTheDeviceRules) {
    RecordingDevice device(Allocator_());
    const uint8 bytes[16] = {7};
    // 크기 규칙·수명·SingleFrame 갱신 거부, stale 갱신, 기록 밖 SetConstantData.
    CHECK(device
              .CreateConstantBuffer(graphics::ConstantBufferLifetime::SingleFrame,
                                    Span<const uint8>(bytes, 8))
              .Error() == ErrorCode::InvalidArgument);
    auto single = device.CreateConstantBuffer(graphics::ConstantBufferLifetime::SingleFrame,
                                              Span<const uint8>(bytes, 16));
    REQUIRE(single);
    CHECK(device.UpdateConstantBuffer(single.Value(), Span<const uint8>(bytes, 16)).Error() ==
          ErrorCode::InvalidArgument);
    auto multi = device.CreateConstantBuffer(graphics::ConstantBufferLifetime::MultiFrame,
                                             Span<const uint8>(bytes, 16));
    REQUIRE(multi);
    CHECK(device.UpdateConstantBuffer(multi.Value(), Span<const uint8>(bytes, 8)).Error() ==
          ErrorCode::InvalidArgument);
    device.Retire(multi.Value());
    CHECK(device.UpdateConstantBuffer(multi.Value(), Span<const uint8>(bytes, 16)).Error() ==
          ErrorCode::StaleHandle);
    graphics::PassDesc pass;
    auto recording = device.BeginPass(pass);
    REQUIRE(recording);
    // stale handle을 묶으면 빈 slot이다.
    recording.Value()->SetConstantBuffer(graphics::ShaderStage::Pixel, 0, multi.Value());
    CHECK(NthValue(device, RecordingDevice::Op::SetConstantBuffer, 0) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Pixel, 0,
                                         RecordingDevice::UnboundConstantWord));
    CHECK(recording.Value()
              ->SetConstantData(graphics::ShaderStage::Pixel, graphics::MaxConstantBufferSlots,
                                Span<const uint8>(bytes, 16))
              .Error() == ErrorCode::InvalidArgument);
    REQUIRE(device.EndFrame());
    // 기록이 끝난 뒤의 SetConstantData는 거부한다.
    CHECK(recording.Value()
              ->SetConstantData(graphics::ShaderStage::Pixel, 0, Span<const uint8>(bytes, 16))
              .Error() == ErrorCode::InvalidArgument);
    device.Retire(single.Value());
    REQUIRE(device.WaitIdle());
}

} // namespace constant_block_tests_file
