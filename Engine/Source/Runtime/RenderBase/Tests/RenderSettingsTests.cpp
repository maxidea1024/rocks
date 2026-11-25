#include "RecordingDevice.h"

#include <Config/Tests/ConfigTestSupport.h>
#include <RenderBase/RenderLane.h>

// 렌더 설정 snapshot 시험이다(Docs/Design/RenderBase 12절, ConfigSystem.ko.md 5.3절 — RB1
// 설정 snapshot). 게임 lane이 sync 지점 뒤 snapshot을 프레임 slot에 담고, 렌더 lane은 그 프레임
// 작업 안에서 담긴 값만 읽는다. GPU 완료는 기록 장치 gate(HoldCompletions·CompleteAll)로만
// 진행한다.
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;
using rocks::test::config::ConfigFixture;

namespace {

// 설정을 읽기만 하는 프레임 packet이다.
struct SettingsPacket {
    uint32 index = 0;
};

} // namespace

ROCKS_RENDER_TRANSFERABLE(SettingsPacket);

namespace {

// lane 쪽 처리기다. 프레임마다 읽은 세대와 값을 남긴다(lane만 쓰고, 게임 lane은 Drain 뒤 읽는다).
class SettingsProcessor final : public RenderLaneProcessor {
public:
    Result<void> OnLaneStart(RenderLaneContext& context) noexcept override {
        graphics::TextureDesc desc;
        desc.width = 4;
        desc.height = 4;
        desc.usage = graphics::TextureUsage::RenderTarget;
        auto created = context.Device().CreateTexture(desc, {});
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        target = created.Value();
        return Result<void>::Success();
    }

    void OnLaneStop(RenderLaneContext& context) noexcept override {
        context.Device().Retire(target);
    }

    graphics::TextureHandle target;
    uint64 generations[4] = {};
    bool immediate[4] = {};
    bool found[4] = {};
};

// 프레임 작업이다. 설정은 프레임 문맥의 snapshot으로만 읽는다.
void ReadSettings(RenderFrameContext& context, SettingsPacket& packet) noexcept {
    auto* processor = static_cast<SettingsProcessor*>(context.Processor());
    const config::ConfigSnapshot* settings = context.Settings().Get<config::ConfigSnapshot>();
    const FieldValue* value =
        settings != nullptr ? settings->Find("render.graph.immediate") : nullptr;
    processor->generations[packet.index] = context.Settings().Generation();
    processor->found[packet.index] = value != nullptr;
    processor->immediate[packet.index] = value != nullptr && value->boolean;
    // 프레임마다 제출 하나가 slot의 GPU 완료 표지다.
    auto frame = context.Device().BeginOffscreen(processor->target, {});
    if (!frame) {
        context.ReportFailure(frame.Error());
        return;
    }

    auto submitted = context.Device().EndFrame();
    (void)submitted;
}

// 다른 타입으로 꺼내면 null이다(타입 표지 확인용).
struct OtherSnapshot {
    uint64 Generation() const noexcept {
        return 0;
    }
};

} // namespace

// sync 지점에서 바뀐 값은 그 뒤에 담은 다음 프레임 packet부터 보이고, 이전 프레임 packet은 옛 값을
// 그대로 읽는다. snapshot은 그 프레임 slot이 다시 확보될 때까지(GPU 완료 뒤) 살아 있다.
TEST(RenderLane_SettingsSnapshotFollowsFramePackets) {
    ConfigFixture fixture;
    config::ConfigContext& settings = fixture.Context();
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    device.HoldCompletions(true);
    SettingsProcessor processor;
    RenderLane lane(allocator, device);
    RenderLaneConfig config;
    config.mode = RenderLaneMode::Thread;
    config.processor = &processor;
    config.frameSlots = 2;
    config.hangTimeoutMs = 0;
    REQUIRE(lane.Start(config));

    // 프레임 1: 세대 1(immediate 꺼짐)을 담는다.
    SharedPtr<const config::ConfigSnapshot> first = settings.Acquire();
    REQUIRE(first.Get() != nullptr);
    const uint64 firstGeneration = first->Generation();
    WeakPtr<const config::ConfigSnapshot> firstWeak(first);
    {
        RenderSettingsRef shared = RenderSettingsRef::Share(first);
        CHECK(shared.Get<config::ConfigSnapshot>() == first.Get());
        CHECK(shared.Get<OtherSnapshot>() == nullptr);
        CHECK(shared.Generation() == firstGeneration);
    }

    auto ticket = lane.BeginFrame();
    REQUIRE(ticket);
    lane.AttachSettings(ticket.Value(), RenderSettingsRef::Share(first));
    REQUIRE(lane.SubmitFrame(Move(ticket.Value()), SettingsPacket{0}, &ReadSettings));

    // sync 지점에서 값을 바꾼다. 게임 lane은 새 snapshot만 갖고, 옛 snapshot은 프레임 1 slot만
    // 쥔다.
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "console",
                                   "render.graph.immediate", "true"));
    REQUIRE(settings.Sync());
    first = nullptr;
    SharedPtr<const config::ConfigSnapshot> second = settings.Acquire();
    REQUIRE(second.Get() != nullptr && second->Generation() == firstGeneration + 1);

    // 프레임 2: 새 세대를 담는다.
    ticket = lane.BeginFrame();
    REQUIRE(ticket);
    lane.AttachSettings(ticket.Value(), RenderSettingsRef::Share(second));
    REQUIRE(lane.SubmitFrame(Move(ticket.Value()), SettingsPacket{1}, &ReadSettings));
    REQUIRE(lane.Drain(RenderDrainReason::Test));

    CHECK(processor.found[0] && processor.found[1]);
    CHECK(processor.generations[0] == firstGeneration && !processor.immediate[0]);
    CHECK(processor.generations[1] == firstGeneration + 1 && processor.immediate[1]);
    // GPU가 프레임 1을 끝내지 않았으므로 slot이 옛 snapshot을 쥐고 있다.
    CHECK(!firstWeak.IsExpired());

    // GPU 완료 gate를 열고 프레임 3이 slot 0을 다시 확보하면 옛 snapshot이 놓인다. 프레임 3은
    // snapshot을 담지 않았으므로 빈 참조를 읽는다.
    device.CompleteAll();
    ticket = lane.BeginFrame();
    REQUIRE(ticket);
    CHECK(ticket.Value().SlotIndex() == 0);
    CHECK(firstWeak.IsExpired());
    REQUIRE(lane.SubmitFrame(Move(ticket.Value()), SettingsPacket{2}, &ReadSettings));
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    CHECK(!processor.found[2] && processor.generations[2] == 0);

    // 값을 다시 바꿔 문맥이 새 snapshot을 갖게 하면, 세대 2는 프레임 2 slot만 쥔다. 정지하면 slot에
    // 남은 snapshot을 모두 놓는다.
    WeakPtr<const config::ConfigSnapshot> secondWeak(second);
    second = nullptr;
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "console",
                                   "render.graph.immediate", "false"));
    REQUIRE(settings.Sync());
    CHECK(settings.Acquire()->Generation() == firstGeneration + 2);
    CHECK(!secondWeak.IsExpired());
    device.CompleteAll();
    REQUIRE(lane.Shutdown());
    CHECK(secondWeak.IsExpired());
}
