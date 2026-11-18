#include "RecordingDevice.h"

#include <Core/Memory/SystemAllocator.h>
#include <Engine/Engine.h>
#include <Platform/Fatal.h>
#include <RenderBase/RenderLane.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;

namespace {

// 프레임 packet이 그릴 항목 하나다(프레임 arena에 둔다).
struct DrawItem {
    float red = 0.0f;
    uint32 vertices = 0;
};

// 시험 장면 프레임 packet이다.
struct ScenePacket {
    Span<const DrawItem> items;
    RenderHandle texture;
};

// 텍스처 생성 작업 값이다.
struct TextureUpload {
    RenderHandle handle;
    uint32 size = 0;
    RenderBlob pixels;
};

// 스왑체인 크기 변경 표면 작업 값이다.
struct SurfaceResize {
    uint32 width = 0;
    uint32 height = 0;
};

// lane 작업 순서 기록용 값이다.
struct OrderMark {
    uint32 id = 0;
};

// timestamp 프레임 packet이다. 그리기를 marker 구간과 timestamp 한 쌍으로 감싼다.
struct TimedPacket {
    uint32 draws = 1;
};

// 등록하지 않은 값이다(전달 거부 시험).
struct Unregistered {
    uint32 value = 0;
};

} // namespace

ROCKS_RENDER_TRANSFERABLE(ScenePacket);
ROCKS_RENDER_TRANSFERABLE(TextureUpload);
ROCKS_RENDER_TRANSFERABLE(SurfaceResize);
ROCKS_RENDER_TRANSFERABLE(OrderMark);
ROCKS_RENDER_TRANSFERABLE(TimedPacket);
// 리플렉션 struct: ArrayTag는 참조가 없고, WeakLink는 WeakObjectRef 필드를, ArraySlot은 ObjectRef
// 필드를 가진다.
ROCKS_RENDER_TRANSFERABLE(rocks::reflection_fixture::ArrayTag);
ROCKS_RENDER_TRANSFERABLE(rocks::reflection_fixture::WeakLink);
ROCKS_RENDER_TRANSFERABLE(rocks::reflection_fixture::ArraySlot);

namespace {

// T 값을 받는 자원 작업을 게시할 수 있는지다(등록하지 않은 타입은 overload가 없다).
template <class T>
concept CanPostResource = requires(RenderLane& lane, T value, RenderLaneFunction<T> run) {
    lane.PostResource(Move(value), run);
};

} // namespace

namespace {

// lane 작업 순서 기록이다. lane 실행 흐름 하나만 쓰고, 읽기는 Drain·Shutdown 뒤에만 한다.
struct OrderLog {
    uint32 values[64] = {};
    uint32 count = 0;

    void Add(uint32 value) noexcept {
        if (count == 64) {
            platform::Fatal("RenderLane test order log overflow");
        }

        values[count++] = value;
    }
};

OrderLog orderLog;

// lane 쪽 시험 처리기다. 시작 때 파이프라인·버퍼·offscreen 대상·스왑체인을 만들고 종료 때
// Retire한다.
class TestProcessor final : public RenderLaneProcessor {
public:
    Result<void> OnLaneStart(RenderLaneContext& context) noexcept override {
        graphics::Device& device = context.Device();
        const graphics::VertexAttribute attributes[] = {{0, graphics::VertexFormat::Float3, 0, 0},
                                                        {1, graphics::VertexFormat::Float4, 0, 12}};
        const graphics::VertexStream streams[] = {{36, graphics::VertexStepRate::Vertex}};
        graphics::PipelineDesc pipelineDesc;
        pipelineDesc.layout = {attributes, streams};
        auto createdPipeline = device.CreatePipeline(pipelineDesc);
        const uint8 bytes[36] = {};
        graphics::BufferDesc bufferDesc;
        bufferDesc.size = sizeof(bytes);
        auto createdBuffer =
            device.CreateBuffer(bufferDesc, Span<const uint8>(bytes, sizeof(bytes)));
        graphics::TextureDesc targetDesc;
        targetDesc.width = 4;
        targetDesc.height = 4;
        targetDesc.usage = graphics::TextureUsage::RenderTarget;
        auto createdTarget = device.CreateTexture(targetDesc, {});
        graphics::SwapChainDesc swapDesc;
        swapDesc.width = 32;
        swapDesc.height = 16;
        auto createdSwapChain = device.CreateSwapChain(swapDesc);
        if (!createdPipeline || !createdBuffer || !createdTarget || !createdSwapChain) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        pipeline = createdPipeline.Value();
        buffer = createdBuffer.Value();
        target = createdTarget.Value();
        swapChain = createdSwapChain.Value();
        return Result<void>::Success();
    }

    void OnLaneStop(RenderLaneContext& context) noexcept override {
        graphics::Device& device = context.Device();
        device.Retire(pipeline);
        device.Retire(buffer);
        device.Retire(target);
        device.Retire(swapChain);
    }

    graphics::PipelineHandle pipeline;
    graphics::BufferHandle buffer;
    graphics::TextureHandle target;
    graphics::SwapChainHandle swapChain;
    // DrawTimed의 timestamp 쓰기 결과다(lane에서 쓰고 Drain 뒤 시험이 읽는다).
    ErrorCode timestampError = ErrorCode::None;
};

void DrawScene(RenderFrameContext& context, ScenePacket& packet) noexcept {
    auto* processor = static_cast<TestProcessor*>(context.Processor());
    graphics::Device& device = context.Device();
    auto frame = device.BeginOffscreen(processor->target, {});
    if (!frame) {
        context.ReportFailure(frame.Error());
        return;
    }

    graphics::CommandContext& commands = *frame.Value();
    commands.SetPipeline(processor->pipeline);
    const graphics::VertexBufferBinding binding{processor->buffer, 0};
    commands.SetVertexBuffers(0, Span<const graphics::VertexBufferBinding>(&binding, 1));
    auto texture = context.Handles().Texture(packet.texture);
    commands.SetTexture(texture ? texture.Value() : graphics::TextureHandle{});
    for (const DrawItem& item : packet.items) {
        graphics::DrawConstants constants;
        constants.color[0] = item.red;
        commands.SetConstants(constants);
        commands.Draw(item.vertices);
    }

    auto submitted = device.EndFrame();
    (void)submitted;
}

// timestamp 쓰기 오류는 processor에 남긴다(프레임은 그대로 제출한다).
void DrawTimed(RenderFrameContext& context, TimedPacket& packet) noexcept {
    auto* processor = static_cast<TestProcessor*>(context.Processor());
    graphics::Device& device = context.Device();
    auto frame = device.BeginOffscreen(processor->target, {});
    if (!frame) {
        context.ReportFailure(frame.Error());
        return;
    }

    graphics::CommandContext& commands = *frame.Value();
    commands.PushMarker(StringView("scene"));
    auto begin = commands.WriteTimestamp();
    commands.SetPipeline(processor->pipeline);
    const graphics::VertexBufferBinding binding{processor->buffer, 0};
    commands.SetVertexBuffers(0, Span<const graphics::VertexBufferBinding>(&binding, 1));
    for (uint32 index = 0; index < packet.draws; ++index) {
        commands.Draw(3);
    }

    auto end = commands.WriteTimestamp();
    commands.PopMarker();
    auto submitted = device.EndFrame();
    (void)submitted;
    processor->timestampError = begin ? (end ? ErrorCode::None : end.Error()) : begin.Error();
}

void CreateTextureOnLane(RenderLaneContext& context, TextureUpload& upload) noexcept {
    graphics::TextureDesc desc;
    desc.width = upload.size;
    desc.height = upload.size;
    auto created = context.Device().CreateTexture(desc, upload.pixels.View());
    RenderNotice notice;
    notice.handle = upload.handle;
    if (!created) {
        auto failed = context.Handles().MarkFailed(upload.handle, created.Error());
        (void)failed;
        notice.kind = RenderNoticeKind::ResourceFailed;
        notice.error = created.Error();
    } else if (!context.Handles().BindTexture(upload.handle, created.Value())) {
        context.Device().Retire(created.Value());
        return;
    } else {
        notice.kind = RenderNoticeKind::ResourceReady;
    }

    context.Notify(notice);
}

void ResizeOnLane(RenderLaneContext& context, SurfaceResize& resize) noexcept {
    auto* processor = static_cast<TestProcessor*>(context.Processor());
    auto resized =
        context.Device().ResizeSwapChain(processor->swapChain, resize.width, resize.height);
    if (!resized) {
        context.ReportFailure(resized.Error());
    }
}

void MarkOrder(RenderLaneContext& context, OrderMark& mark) noexcept {
    (void)context;
    orderLog.Add(mark.id);
}

// lane 작업 안에서 다시 게시한다. 그 작업은 큐 맨 뒤(이미 게시된 작업 뒤)에 붙어야 한다.
void MarkAndRepost(RenderLaneContext& context, OrderMark& mark) noexcept {
    orderLog.Add(mark.id);
    auto posted = context.Lane().PostResource(OrderMark{mark.id + 100}, &MarkOrder);
    (void)posted;
    orderLog.Add(mark.id + 1);
}

// 바깥 lane 작업이 재진입 게시를 하는 작업(A)과 보통 작업(B)을 차례로 게시한다. 두 모드 모두 바깥 →
// A(앞·뒤) → B → A가 넣은 후속 순서여야 한다(CP1 재진입 순서와 같은 시나리오).
void PostReentry(RenderLaneContext& context, OrderMark& mark) noexcept {
    orderLog.Add(mark.id);
    auto first = context.Lane().PostResource(OrderMark{2}, &MarkAndRepost);
    auto second = context.Lane().PostResource(OrderMark{10}, &MarkOrder);
    (void)first;
    (void)second;
}

void NoOp(RenderLaneContext& context, rocks::reflection_fixture::ArrayTag& tag) noexcept {
    (void)context;
    (void)tag;
}

void NoOpLink(RenderLaneContext& context, rocks::reflection_fixture::WeakLink& link) noexcept {
    (void)context;
    (void)link;
}

RenderLaneConfig MakeConfig(RenderLaneMode mode, TestProcessor& processor) noexcept {
    RenderLaneConfig config;
    config.mode = mode;
    config.processor = &processor;
    // 시험은 대기 시간 상한에 기대지 않는다.
    config.hangTimeoutMs = 0;
    return config;
}

// 같은 작업 열을 한 모드로 실행한다. 장치 호출 열과 작업 순서를 비교하려고 쓴다.
bool RunScenario(RenderLaneMode mode, RecordingDevice& device, Allocator& allocator) noexcept {
    TestProcessor processor;
    RenderLane lane(allocator, device);
    if (!lane.Start(MakeConfig(mode, processor))) {
        return false;
    }

    bool ok = true;
    auto texture = lane.ReserveHandle(RenderResourceKind::Texture);
    const uint8 pixels[16] = {};
    auto blob = RenderBlob::TryCopy(allocator, Span<const uint8>(pixels, sizeof(pixels)));
    ok = texture && blob &&
         lane.PostResource(TextureUpload{texture.Value(), 2, Move(blob.Value())},
                           &CreateTextureOnLane);
    ok = ok && lane.PostResource(OrderMark{1}, &PostReentry);
    for (uint32 frame = 0; ok && frame < 4; ++frame) {
        auto ticket = lane.BeginFrame();
        if (!ticket) {
            return false;
        }

        auto items = lane.Arena(ticket.Value()).TryNewArray<DrawItem>(frame + 1);
        if (!items) {
            return false;
        }

        for (uint32 index = 0; index < items.Value().Size(); ++index) {
            items.Value()[index].red = static_cast<float>(index + 1) / 8.0f;
            items.Value()[index].vertices = 3 * (index + 1);
        }

        ok = lane.SubmitFrame(Move(ticket.Value()), ScenePacket{items.Value(), texture.Value()},
                              &DrawScene)
                 .IsOk();
    }

    ok = ok && lane.PostSurface(SurfaceResize{64, 32}, &ResizeOnLane);
    ok = ok && lane.Retire(texture.Value());
    ok = lane.Shutdown().IsOk() && ok;
    const RenderLaneStats stats = lane.Stats();
    return ok && stats.framesCompleted == 4 && stats.retiresCompleted == 1 &&
           stats.tasksPosted == stats.tasksCompleted && lane.Handles().Count() == 0 &&
           device.PendingRetireCount() == 0;
}

// slot 대기 중 펌프를 세고, 무장된 뒤 첫 프레임 제출이 보이면 그 GPU 완료를 연다.
struct GatePump {
    RecordingDevice* device = nullptr;
    uint32 armedCalls = 0;
    bool armed = false;
    bool opened = false;

    static void Pump(void* raw) noexcept {
        auto* self = static_cast<GatePump*>(raw);
        if (!self->armed) {
            return;
        }

        ++self->armedCalls;
        if (!self->opened && self->device->Submitted() >= 1) {
            self->device->Complete(1);
            self->opened = true;
        }
    }
};

// 대기 중 펌프마다 지금까지의 제출을 모두 끝낸다(종료 순서 시험의 GPU).
struct CompletingPump {
    RecordingDevice* device = nullptr;
    uint32 calls = 0;

    static void Pump(void* raw) noexcept {
        auto* self = static_cast<CompletingPump*>(raw);
        ++self->calls;
        self->device->CompleteAll();
    }
};

void SlotWaitScenario(RenderLaneMode mode) noexcept {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    device.HoldCompletions(true);
    TestProcessor processor;
    GatePump pump;
    pump.device = &device;
    RenderLaneConfig config = MakeConfig(mode, processor);
    config.frameSlots = 2;
    config.pump = {&GatePump::Pump, &pump};
    RenderLane lane(allocator, device);
    REQUIRE(lane.Start(config));
    for (uint32 frame = 0; frame < 2; ++frame) {
        auto ticket = lane.BeginFrame();
        REQUIRE(ticket);
        REQUIRE(lane.SubmitFrame(Move(ticket.Value()), ScenePacket{}, &DrawScene));
    }

    CHECK(lane.Stats().slotWaits == 0);
    // 두 slot 모두 GPU 완료 전이다. 세 번째 확보는 가장 오래된 slot을 기다리며 펌프한다.
    pump.armed = true;
    auto third = lane.BeginFrame();
    REQUIRE(third);
    CHECK(pump.opened);
    CHECK(pump.armedCalls >= 1);
    const RenderLaneStats stats = lane.Stats();
    CHECK(stats.slotWaits == 1);
    CHECK(stats.pumpCalls >= pump.armedCalls);
    CHECK(third.Value().SlotIndex() == 0);
    lane.AbandonFrame(Move(third.Value()));
    // lane이 두 번째 프레임까지 제출한 뒤(표지) 남은 GPU 작업을 모두 끝낸다.
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    device.CompleteAll();
    CHECK(lane.Shutdown());
    CHECK(device.PendingRetireCount() == 0);
}

// 엔진 종료 hook으로 렌더 lane 종료 순서(5.5절)를 잇는다(CP6).
class LaneHooks final : public EngineLoopHooks {
public:
    explicit LaneHooks(RenderLane& lane) noexcept : lane_(lane) {
    }

    void EndFrame(Engine& engine) noexcept override {
        (void)engine;
        auto ticket = lane_.BeginFrame();
        if (!ticket) {
            frameErrors_++;
            return;
        }

        auto items = lane_.Arena(ticket.Value()).TryNewArray<DrawItem>(2);
        if (!items || !lane_.SubmitFrame(Move(ticket.Value()),
                                         ScenePacket{items.Value(), RenderHandle{}}, &DrawScene)) {
            frameErrors_++;
        }
    }

    void BeginShutdown(Engine& engine) noexcept override {
        (void)engine;
        steps[stepCount++] = 1;
        lane_.BlockFrames();
    }

    Result<void> CompleteInFlightWork(Engine& engine) noexcept override {
        (void)engine;
        steps[stepCount++] = 2;
        return lane_.WaitFramesIdle();
    }

    Result<uint64> DrainShutdownCleanup(Engine& engine) noexcept override {
        (void)engine;
        steps[stepCount++] = 3;
        // 첫 drain은 "최종 GC가 파괴한 에셋"이 낸 Retire를 흉내 낸다.
        if (drains_++ == 0) {
            for (RenderHandle handle : gcHandles) {
                auto retired = lane_.Retire(handle);
                (void)retired;
            }
        }

        return lane_.DrainRetires();
    }

    Result<void> VerifyShutdownClosure(Engine& engine) noexcept override {
        (void)engine;
        steps[stepCount++] = 4;
        return lane_.CloseResources();
    }

    Result<void> StopExecutors(Engine& engine) noexcept override {
        (void)engine;
        steps[stepCount++] = 5;
        return lane_.Stop();
    }

    RenderHandle gcHandles[2];
    uint32 steps[16] = {};
    uint32 stepCount = 0;
    uint32 frameErrors_ = 0;

private:
    RenderLane& lane_;
    uint32 drains_ = 0;
};

void EngineShutdownScenario(RenderLaneMode mode) noexcept {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    device.HoldCompletions(true);
    TestProcessor processor;
    CompletingPump pump;
    pump.device = &device;
    RenderLaneConfig config = MakeConfig(mode, processor);
    config.pump = {&CompletingPump::Pump, &pump};
    RenderLane lane(allocator, device);
    REQUIRE(lane.Start(config));
    LaneHooks hooks(lane);
    // GC가 회수할 에셋의 GPU 자원 둘이다.
    for (RenderHandle& handle : hooks.gcHandles) {
        auto reserved = lane.ReserveHandle(RenderResourceKind::Texture);
        REQUIRE(reserved);
        handle = reserved.Value();
        const uint8 pixels[16] = {};
        auto blob = RenderBlob::TryCopy(allocator, Span<const uint8>(pixels, sizeof(pixels)));
        REQUIRE(blob);
        REQUIRE(
            lane.PostResource(TextureUpload{handle, 2, Move(blob.Value())}, &CreateTextureOnLane));
    }

    {
        EngineConfig engineConfig;
        engineConfig.hooks = &hooks;
        engineConfig.fixedFrameDeltaSeconds = 1.0 / 60.0;
        engineConfig.maxFrames = 3;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(engineConfig));
        REQUIRE(engine.Run());
        // 마지막 프레임은 GPU에서 아직 끝나지 않았다(펌프만 완료를 연다).
        CHECK(lane.Stats().framesCompleted < 3);
        CHECK(engine.Shutdown());
        CHECK(engine.State() == EngineState::Stopped);
    }

    CHECK(hooks.frameErrors_ == 0);
    // BeginShutdown → CompleteInFlightWork → drain(Retire 2건) → drain(0건, 닫힘) → closure →
    // 실행기 종료.
    const uint32 expected[] = {1, 2, 3, 3, 4, 5};
    REQUIRE(hooks.stepCount == 6);
    for (uint32 index = 0; index < 6; ++index) {
        CHECK(hooks.steps[index] == expected[index]);
    }

    const RenderLaneStats stats = lane.Stats();
    CHECK(!stats.running);
    CHECK(stats.framesCompleted == 3);
    CHECK(stats.retiresCompleted == 2);
    CHECK(stats.retiresPending == 0);
    CHECK(stats.tasksPosted == stats.tasksCompleted);
    CHECK(stats.drains[static_cast<uint32>(RenderDrainReason::Shutdown)] == 1);
    CHECK(lane.Handles().Count() == 0);
    CHECK(device.PendingRetireCount() == 0);
    CHECK(device.IsOwnerThread());
    if (mode == RenderLaneMode::Thread) {
        CHECK(stats.ownershipReturned);
    }

    // 스왑체인은 lane 종료(창 파괴 전)에 Retire되었다.
    bool swapChainRetired = false;
    for (const RecordingDevice::Call& call : device.Calls()) {
        swapChainRetired = swapChainRetired || call.op == RecordingDevice::Op::RetireSwapChain;
    }

    CHECK(swapChainRetired);
}

} // namespace

TEST(RenderLane_InlineAndThreadModesProduceTheSameDeviceCalls) {
    SystemAllocator allocator;
    RecordingDevice inlineDevice(allocator);
    orderLog = {};
    REQUIRE(RunScenario(RenderLaneMode::Inline, inlineDevice, allocator));
    const OrderLog inlineOrder = orderLog;
    RecordingDevice threadDevice(allocator);
    orderLog = {};
    REQUIRE(RunScenario(RenderLaneMode::Thread, threadDevice, allocator));
    const OrderLog threadOrder = orderLog;

    // 재진입 게시(102)는 이미 게시된 작업(10) 뒤에 실행된다.
    const uint32 expectedOrder[] = {1, 2, 3, 10, 102};
    REQUIRE(inlineOrder.count == 5 && threadOrder.count == 5);
    for (uint32 index = 0; index < 5; ++index) {
        CHECK(inlineOrder.values[index] == expectedOrder[index]);
        CHECK(threadOrder.values[index] == expectedOrder[index]);
    }

    const Span<const RecordingDevice::Call> inlineCalls = inlineDevice.Calls();
    const Span<const RecordingDevice::Call> threadCalls = threadDevice.Calls();
    REQUIRE(inlineCalls.Size() == threadCalls.Size());
    bool same = true;
    for (usize index = 0; index < inlineCalls.Size(); ++index) {
        same = same && inlineCalls[index] == threadCalls[index];
    }

    CHECK(same);
    CHECK(inlineCalls.Size() > 30);
    CHECK(inlineDevice.OwnershipTransfers() == 0);
    // Thread 모드는 시작 때 lane이, 종료 때 게임 lane이 소유권을 잡는다.
    CHECK(threadDevice.OwnershipTransfers() == 2);
    CHECK(inlineDevice.RejectedCalls() == 0 && threadDevice.RejectedCalls() == 0);
}

TEST(RenderLane_ThirdSlotWaitsForGpuCompletionWhilePumping) {
    SlotWaitScenario(RenderLaneMode::Inline);
    SlotWaitScenario(RenderLaneMode::Thread);
}

TEST(RenderLane_OnlyRegisteredTypesCrossTheLane) {
    static_assert(!RenderTransferable<Unregistered>);
    static_assert(!RenderTransferable<ScenePacket&>);
    static_assert(RenderTransferable<ScenePacket>);
    static_assert(RenderTransferable<RenderHandle>);
    static_assert(RenderTransferable<RenderBlob>);
    // 등록하지 않은 값을 받는 lane 작업은 만들 수 없다(컴파일 오류).
    static_assert(!CanPostResource<Unregistered>);
    static_assert(CanPostResource<OrderMark>);
    // ObjectRef는 이동할 수 없으므로 그 필드를 가진 struct는 등록해도 C++ 타입 검사가 거부한다.
    static_assert(!RenderTransferable<rocks::reflection_fixture::ArraySlot>);

    SystemAllocator allocator;
    RecordingDevice device(allocator);
    TestProcessor processor;
    RenderLane lane(allocator, device);
    REQUIRE(lane.Start(MakeConfig(RenderLaneMode::Inline, processor)));
    rocks::reflection_fixture::ArrayTag tag(allocator);
    CHECK(lane.PostResource(Move(tag), &NoOp));
    // 이동할 수 있는 Object 참조(WeakObjectRef) 필드가 있는 등록 타입은 Development 검사가 거부한다
    // (Release는 검사 생략).
    rocks::reflection_fixture::WeakLink link;
    auto posted = lane.PostResource(Move(link), &NoOpLink);
    if constexpr (IsReleaseBuild) {
        CHECK(posted);
    } else {
        CHECK(posted.Error() == ErrorCode::InvalidArgument);
    }

    CHECK(lane.Shutdown());
}

TEST(RenderLane_EngineShutdownOrderClosesInFlightFramesAndRetires) {
    EngineShutdownScenario(RenderLaneMode::Inline);
    EngineShutdownScenario(RenderLaneMode::Thread);
}

TEST(RenderLane_ResizeDuringOwnershipTransferRunsAsSurfaceTask) {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    TestProcessor processor;
    RenderLane lane(allocator, device);
    // lane이 소유권을 잡는 순간을 막는다. 그 사이 게시한 resize는 소유권을 얻은 뒤 실행되어야 한다.
    device.GateAcquire();
    REQUIRE(lane.Start(MakeConfig(RenderLaneMode::Thread, processor)));
    CHECK(!device.IsOwnerThread());
    REQUIRE(lane.PostSurface(SurfaceResize{128, 64}, &ResizeOnLane));
    device.OpenAcquire();
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    CHECK(lane.IsReady());
    CHECK(device.FailedResizes() == 0);
    CHECK(device.RejectedCalls() == 0);
    bool resized = false;
    for (const RecordingDevice::Call& call : device.Calls()) {
        resized = resized || (call.op == RecordingDevice::Op::ResizeSwapChain &&
                              call.value == ((uint64{128} << 32) | 64u));
    }

    CHECK(resized);
    CHECK(lane.Stats().drains[static_cast<uint32>(RenderDrainReason::Test)] == 1);
    CHECK(lane.Shutdown());
    CHECK(device.IsOwnerThread());
}

namespace {

struct LostWaiter {
    RecordingDevice* device = nullptr;
    graphics::GpuFencePoint point;
    platform::ManualResetEvent started;
    ErrorCode error = ErrorCode::None;

    static void Run(void* raw) noexcept {
        auto* self = static_cast<LostWaiter*>(raw);
        self->started.Signal();
        auto waited = self->device->WaitFence(self->point, platform::InfiniteTimeout);
        self->error = waited ? ErrorCode::None : waited.Error();
    }
};

} // namespace

TEST(RenderLane_DeviceLossFailsTheNextSlotAndWakesFenceWaiters) {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    device.HoldCompletions(true);
    TestProcessor processor;
    RenderLane lane(allocator, device);
    REQUIRE(lane.Start(MakeConfig(RenderLaneMode::Thread, processor)));
    auto ticket = lane.BeginFrame();
    REQUIRE(ticket);
    REQUIRE(lane.SubmitFrame(Move(ticket.Value()), ScenePacket{}, &DrawScene));
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    const graphics::GpuFencePoint point = device.LastSubmittedFence();
    REQUIRE(!point.IsNull());

    LostWaiter waiter;
    waiter.device = &device;
    waiter.point = point;
    platform::Thread thread;
    REQUIRE(thread.Start(&LostWaiter::Run, &waiter) == platform::ThreadStatus::Success);
    REQUIRE(waiter.started.Wait() == platform::WaitResult::Signaled);
    device.InjectLost();
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(waiter.error == ErrorCode::Destroyed);

    auto next = lane.BeginFrame();
    CHECK(next.Error() == ErrorCode::Destroyed);
    CHECK(lane.Failure() == ErrorCode::Destroyed);
    Array<RenderNotice> notices(allocator);
    REQUIRE(lane.Notices().TakeAll(notices));
    bool lostNotice = false;
    for (const RenderNotice& notice : notices) {
        lostNotice = lostNotice || notice.kind == RenderNoticeKind::DeviceLost;
    }

    CHECK(lostNotice);
    // 손실 뒤에도 종료는 끝까지 진행하고 오류를 보고한다.
    CHECK(lane.Shutdown().Error() == ErrorCode::Destroyed);
    CHECK(!lane.Stats().running);
}

TEST(RenderHandleTable_ReserveBindRetireRemove) {
    SystemAllocator allocator;
    RenderHandleTable table(allocator);
    auto handle = table.Reserve(RenderResourceKind::Texture);
    REQUIRE(handle);
    CHECK(table.State(handle.Value()).Value() == RenderResourceState::Pending);
    CHECK(table.Texture(handle.Value()).Error() == ErrorCode::NotFound);
    CHECK(table.Buffer(handle.Value()).Error() == ErrorCode::TypeMismatch);
    graphics::TextureHandle device{SlotHandle{3, 1}};
    REQUIRE(table.BindTexture(handle.Value(), device));
    CHECK(table.Texture(handle.Value()).Value() == device);
    CHECK(table.BindTexture(handle.Value(), device).Error() == ErrorCode::InvalidArgument);
    auto retired = table.BeginRetire(handle.Value());
    REQUIRE(retired);
    CHECK(retired.Value().device == device.slot);
    CHECK(table.BeginRetire(handle.Value()).Error() == ErrorCode::InvalidArgument);
    CHECK(table.Texture(handle.Value()).Error() == ErrorCode::NotFound);
    REQUIRE(table.Remove(handle.Value()));
    CHECK(table.State(handle.Value()).Error() == ErrorCode::StaleHandle);
    CHECK(table.Count() == 0);

    auto failed = table.Reserve(RenderResourceKind::Buffer);
    REQUIRE(failed);
    REQUIRE(table.MarkFailed(failed.Value(), ErrorCode::OutOfMemory));
    CHECK(table.State(failed.Value()).Value() == RenderResourceState::Failed);
    CHECK(table.Remove(failed.Value()).Error() == ErrorCode::InvalidArgument);
}

TEST(RenderLane_ReadbackNoticeArrivesOnlyAfterGpuCompletion) {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    device.HoldCompletions(true);
    device.SetReadbackFill(0x5a);
    TestProcessor processor;
    RenderLane lane(allocator, device);
    REQUIRE(lane.Start(MakeConfig(RenderLaneMode::Thread, processor)));
    auto texture = lane.ReserveHandle(RenderResourceKind::Texture);
    REQUIRE(texture);
    const uint8 pixels[16] = {};
    auto blob = RenderBlob::TryCopy(allocator, Span<const uint8>(pixels, sizeof(pixels)));
    REQUIRE(blob);
    REQUIRE(lane.PostResource(TextureUpload{texture.Value(), 2, Move(blob.Value())},
                              &CreateTextureOnLane));
    // 갱신 작업은 lane FIFO 순서로 장치 갱신이 된다.
    auto update = RenderBlob::TryCopy(allocator, Span<const uint8>(pixels, sizeof(pixels)));
    REQUIRE(update);
    REQUIRE(lane.UpdateTexture(texture.Value(), 0, Move(update.Value())));
    auto readback = lane.RequestReadback(texture.Value());
    REQUIRE(readback);
    REQUIRE(lane.Drain(RenderDrainReason::Test));

    // GPU 완료(fence gate) 전에는 결과도 통지도 없다.
    CHECK(lane.TakeReadback(readback.Value()).Error() == ErrorCode::NotFound);
    Array<RenderNotice> notices(allocator);
    REQUIRE(lane.Notices().TakeAll(notices));
    bool early = false;
    for (const RenderNotice& notice : notices) {
        early = early || notice.kind == RenderNoticeKind::ReadbackReady;
    }

    CHECK(!early);
    device.CompleteAll();
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    notices.Clear();
    REQUIRE(lane.Notices().TakeAll(notices));
    bool ready = false;
    for (const RenderNotice& notice : notices) {
        ready = ready || (notice.kind == RenderNoticeKind::ReadbackReady &&
                          notice.frame == readback.Value() && notice.error == ErrorCode::None &&
                          notice.handle == texture.Value());
    }

    CHECK(ready);
    auto result = lane.TakeReadback(readback.Value());
    REQUIRE(result);
    REQUIRE(result.Value().Size() == 16);
    CHECK(result.Value().View()[0] == 0x5a && result.Value().View()[15] == 0x5a);
    CHECK(lane.TakeReadback(readback.Value()).Error() == ErrorCode::NotFound);
    bool updated = false;
    for (const RecordingDevice::Call& call : device.Calls()) {
        updated = updated || call.op == RecordingDevice::Op::UpdateTexture;
    }

    CHECK(updated);
    REQUIRE(lane.Retire(texture.Value()));
    CHECK(lane.Shutdown());
}

namespace {

// notices에서 kind 통지를 모은다.
void Gather(const Array<RenderNotice>& notices, RenderNoticeKind kind,
            Array<RenderNotice>& out) noexcept {
    for (const RenderNotice& notice : notices) {
        if (notice.kind == kind) {
            REQUIRE(out.TryPushBack(notice));
        }
    }
}

} // namespace

TEST(RenderLane_TimestampsArriveOnlyAfterGpuCompletion) {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    device.HoldCompletions(true);
    TestProcessor processor;
    RenderLane lane(allocator, device);
    REQUIRE(lane.Start(MakeConfig(RenderLaneMode::Thread, processor)));
    auto ticket = lane.BeginFrame();
    REQUIRE(ticket);
    REQUIRE(lane.SubmitFrame(Move(ticket.Value()), TimedPacket{2}, &DrawTimed));
    REQUIRE(lane.Drain(RenderDrainReason::Test));

    // GPU 완료(fence gate) 전에는 timestamp 통지가 없다(미완료).
    Array<RenderNotice> notices(allocator);
    Array<RenderNotice> stamps(allocator);
    REQUIRE(lane.Notices().TakeAll(notices));
    Gather(notices, RenderNoticeKind::GpuTimestamp, stamps);
    CHECK(stamps.IsEmpty());

    device.CompleteAll();
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    notices.Clear();
    REQUIRE(lane.Notices().TakeAll(notices));
    Gather(notices, RenderNoticeKind::GpuTimestamp, stamps);
    REQUIRE(stamps.Size() == 2);
    CHECK(stamps[0].frame == 1 && stamps[0].value == RecordingDevice::TimestampTickStep);
    CHECK(stamps[1].frame == 2 && stamps[1].value == 2 * RecordingDevice::TimestampTickStep);
    CHECK(stamps[0].error == ErrorCode::None && stamps[1].error == ErrorCode::None);
    CHECK(processor.timestampError == ErrorCode::None);
    Array<RenderNotice> frames(allocator);
    Gather(notices, RenderNoticeKind::FrameCompleted, frames);
    REQUIRE(frames.Size() == 1);
    CHECK(frames[0].error == ErrorCode::None);
    // timestamp 통지는 같은 프레임의 FrameCompleted보다 먼저다.
    usize stampAt = 0;
    usize frameAt = 0;
    for (usize index = 0; index < notices.Size(); ++index) {
        stampAt = notices[index].kind == RenderNoticeKind::GpuTimestamp ? index : stampAt;
        frameAt = notices[index].kind == RenderNoticeKind::FrameCompleted ? index : frameAt;
    }

    CHECK(stampAt < frameAt);
    // marker는 timestamp 쌍을 감싸고, 열고 닫은 수가 같다.
    uint32 pushes = 0;
    uint32 pops = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        pushes += call.op == RecordingDevice::Op::PushMarker ? 1u : 0u;
        pops += call.op == RecordingDevice::Op::PopMarker ? 1u : 0u;
    }

    CHECK(pushes == 1 && pops == 1);
    CHECK(lane.Shutdown());
}

TEST(RenderLane_UnsupportedTimestampsArePublishedAsNone) {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    device.SetTimestampsSupported(false);
    CHECK(!device.Info().timestamps && device.Info().timestampFrequency == 0);
    CHECK(!device.Info().clockCalibration);
    CHECK(device.CalibrateClocks().Error() == ErrorCode::Unsupported);
    TestProcessor processor;
    RenderLane lane(allocator, device);
    REQUIRE(lane.Start(MakeConfig(RenderLaneMode::Thread, processor)));
    auto ticket = lane.BeginFrame();
    REQUIRE(ticket);
    REQUIRE(lane.SubmitFrame(Move(ticket.Value()), TimedPacket{1}, &DrawTimed));
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    Array<RenderNotice> notices(allocator);
    REQUIRE(lane.Notices().TakeAll(notices));
    Array<RenderNotice> stamps(allocator);
    Gather(notices, RenderNoticeKind::GpuTimestamp, stamps);
    Array<RenderNotice> frames(allocator);
    Gather(notices, RenderNoticeKind::FrameCompleted, frames);
    // 추정값을 만들지 않는다: 통지가 없고 쓰기는 Unsupported로 실패하며 프레임은 그대로 끝난다.
    CHECK(stamps.IsEmpty());
    CHECK(processor.timestampError == ErrorCode::Unsupported);
    REQUIRE(frames.Size() == 1);
    CHECK(frames[0].error == ErrorCode::None);
    CHECK(lane.Shutdown());
}
