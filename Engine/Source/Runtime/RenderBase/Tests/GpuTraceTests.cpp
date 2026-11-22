#include "RecordingDevice.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/ModuleLoader.h>
#include <RenderBase/GpuTrace.h>
#include <RenderBase/RenderLane.h>
#include <TestSupport/Test.h>
#include <TraceAnalysis/TraceStreamDecoder.h>
#include <TraceCapture/Tests/TraceCaptureTestSupport.h>

// GPU 계측 시험이다(Docs/Design/RenderBase.ko.md 13.3절 RB7). GPU 완료는 기록 장치의 fence gate
// (HoldCompletions·CompleteAll)로만 진행한다. sleep이나 시간 문턱이 없다.
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;
using rocks::tracetest::MemoryOutput;

namespace {

using Format = TraceStandardFormat;

TraceScopeName FrameCpuName{"Test.RenderFrame"};
TraceScopeName OuterName{"Test.GpuOuter"};
TraceScopeName InnerName{"Test.GpuInner"};

// GPU 구간 프레임 packet이다(값 없음).
struct GpuPacket {
    uint32 nested = 1;
};

} // namespace

ROCKS_RENDER_TRANSFERABLE(GpuPacket);

namespace {

// lane 쪽 처리기다. offscreen 대상과 GpuTrace를 가진다(GpuTrace는 lane 쪽 객체다).
class GpuProcessor final : public RenderLaneProcessor {
public:
    explicit GpuProcessor(GpuTrace& trace) noexcept : gpu(trace) {
    }

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

    GpuTrace& gpu;
    graphics::TextureHandle target;
};

// CPU 구간 안에서 프레임을 기록한다: GPU 구간 Outer{Inner}를 nested번 둔다.
void DrawGpu(RenderFrameContext& context, GpuPacket& packet) noexcept {
    CpuScope cpu(FrameCpuName);
    auto* processor = static_cast<GpuProcessor*>(context.Processor());
    graphics::Device& device = context.Device();
    auto frame = device.BeginOffscreen(processor->target, {});
    if (!frame) {
        context.ReportFailure(frame.Error());
        return;
    }

    graphics::CommandContext& commands = *frame.Value();
    processor->gpu.BeginFrame(device, commands, context.FrameNumber());
    for (uint32 index = 0; index < packet.nested; ++index) {
        GpuScope outer(processor->gpu, commands, OuterName);
        GpuScope inner(processor->gpu, commands, InnerName);
    }

    processor->gpu.EndFrame(commands);
    auto submitted = device.EndFrame();
    (void)submitted;
}

RenderLaneConfig MakeConfig(GpuProcessor& processor) noexcept {
    RenderLaneConfig config;
    config.mode = RenderLaneMode::Thread;
    config.processor = &processor;
    config.hangTimeoutMs = 0;
    return config;
}

// 렌더 lane 하나로 프레임 하나를 그리고, GPU 완료 전후로 완료 통지를 GpuTrace에 넘긴다.
struct GpuRun {
    GpuTraceStats beforeCompletion;
    GpuTraceStats afterCompletion;
};

bool RunFrame(Allocator& allocator, RecordingDevice& device, GpuTrace& trace,
              GpuRun& run) noexcept {
    device.HoldCompletions(true);
    GpuProcessor processor(trace);
    RenderLane lane(allocator, device);
    if (!lane.Start(MakeConfig(processor))) {
        return false;
    }

    auto ticket = lane.BeginFrame();
    bool ok = ticket && lane.SubmitFrame(Move(ticket.Value()), GpuPacket{1}, &DrawGpu) &&
              lane.Drain(RenderDrainReason::Test);
    Array<RenderNotice> notices(allocator);
    ok = ok && lane.Notices().TakeAll(notices);
    trace.Consume(notices.View());
    run.beforeCompletion = trace.Stats();

    // GPU 완료(fence gate)를 연 뒤에야 timestamp 통지가 온다.
    device.CompleteAll();
    ok = ok && lane.Drain(RenderDrainReason::Test);
    notices.Clear();
    ok = ok && lane.Notices().TakeAll(notices);
    trace.Consume(notices.View());
    run.afterCompletion = trace.Stats();
    return lane.Shutdown() && ok;
}

TraceSessionSettings ManualSettings(Allocator& allocator) noexcept {
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    return settings;
}

uint16 RecordType(const TraceStreamDecoder& decoder, const char* name) noexcept {
    const DecodedSchema* schema = decoder.FindSchema(StringView(name));
    return schema != nullptr ? schema->typeId : TraceFormat::InvalidTypeId;
}

StringView TextOf(Span<const uint8> bytes) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
}

// 기록된 모든 기록을 한 배열로 모은다(정의 스트림과 스레드 스트림).
void CollectAll(const TraceStreamDecoder& decoder, Array<DecodedRecord>& out) noexcept {
    for (const DecodedStream& stream : decoder.Streams()) {
        if (stream.id != TraceFormat::SchemaStreamId) {
            CHECK(decoder.DecodeRecords(stream.id, out));
        }
    }
}

uint32 NameId(const TraceStreamDecoder& decoder, const Array<DecodedRecord>& records,
              StringView name) noexcept {
    const uint16 type = RecordType(decoder, Format::ScopeNameType);
    for (const DecodedRecord& record : records) {
        if (record.typeId == type && TextOf(record.variable[0]) == name) {
            return ReadFixed<uint32>(record, Format::ScopeNameIdOffset);
        }
    }

    return 0;
}

} // namespace

// timestamp는 GPU 완료 gate 뒤에만 값이 되고, calibration 쌍으로 옮긴 GPU 구간이 CPU 구간과 같은
// 시간축에 놓인다. 구간은 중첩(깊이)과 marker push·pop을 함께 남긴다.
TEST(GpuTrace_ScopesResolveAfterCompletionOnCpuTimeline) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(RegisterGpuTraceTypes(session));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        REQUIRE(session.SetChannelEnabled(GpuTraceChannel, true));
        REQUIRE(session.SetChannelEnabled(TraceCpuChannel, true));

        RecordingDevice device(allocator);
        GpuTraceSettings settings;
        settings.markers = true;
        settings.timestamps = true;
        GpuTrace trace(allocator, settings);
        GpuRun run;
        REQUIRE(RunFrame(allocator, device, trace, run));
        session.Stop();

        // GPU 완료 전에는 아무 구간도 나가지 않고 모두 pending이다(프레임 1 + 구간 2).
        CHECK(run.beforeCompletion.timing);
        CHECK(run.beforeCompletion.scopesRecorded == 0);
        CHECK(run.beforeCompletion.framesRecorded == 0);
        CHECK(run.beforeCompletion.pending == 3);
        CHECK(run.afterCompletion.scopesRecorded == 2);
        CHECK(run.afterCompletion.framesRecorded == 1);
        CHECK(run.afterCompletion.pending == 0);
        CHECK(run.afterCompletion.invalid == 0);
        CHECK(run.afterCompletion.calibrations == 1);

        // marker는 Outer{Inner} 순으로 열고 닫힌다.
        uint64 markers[4] = {};
        usize markerCount = 0;
        RecordingDevice::Op ops[4] = {};
        for (const RecordingDevice::Call& call : device.Calls()) {
            if ((call.op == RecordingDevice::Op::PushMarker ||
                 call.op == RecordingDevice::Op::PopMarker) &&
                markerCount < 4) {
                ops[markerCount] = call.op;
                markers[markerCount++] = call.value;
            }
        }

        REQUIRE(markerCount == 4);
        CHECK(ops[0] == RecordingDevice::Op::PushMarker &&
              markers[0] == StringView("Test.GpuOuter").ByteCount());
        CHECK(ops[1] == RecordingDevice::Op::PushMarker &&
              markers[1] == StringView("Test.GpuInner").ByteCount());
        CHECK(ops[2] == RecordingDevice::Op::PopMarker && markers[2] == 1);
        CHECK(ops[3] == RecordingDevice::Op::PopMarker && markers[3] == 0);

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> records(allocator);
        CollectAll(decoder, records);

        // calibration 정의(오차 포함)와 queue 정의(GPU 시간 있음)다.
        const DecodedRecord* calibration = nullptr;
        const DecodedRecord* queue = nullptr;
        const uint16 calibrationType = RecordType(decoder, Format::GpuCalibrationType);
        const uint16 queueType = RecordType(decoder, Format::GpuQueueType);
        for (const DecodedRecord& record : records) {
            calibration = record.typeId == calibrationType ? &record : calibration;
            queue = record.typeId == queueType ? &record : queue;
        }

        REQUIRE(calibration != nullptr && queue != nullptr);
        CHECK(ReadFixedByte(*queue, Format::GpuQueueTimingOffset) == 1);
        CHECK(TextOf(queue->variable[0]) == StringView("Graphics"));
        graphics::ClockCalibration pair;
        pair.gpuTicks = ReadFixed<uint64>(*calibration, Format::GpuCalibrationGpuTicksOffset);
        pair.cpuTicks = ReadFixed<uint64>(*calibration, Format::GpuCalibrationCpuTickOffset);
        pair.gpuFrequency = ReadFixed<uint64>(*calibration, Format::GpuCalibrationFrequencyOffset);
        CHECK(pair.gpuFrequency == RecordingDevice::TimestampFrequency);
        CHECK(ReadFixed<uint64>(*calibration, Format::GpuCalibrationUncertaintyOffset) ==
              RecordingDevice::CalibrationUncertaintyNanoseconds);

        // 기록 장치는 쓸 때마다 GPU tick을 100씩 늘린다: 프레임 시작, Outer 시작, Inner 시작, Inner
        // 끝, Outer 끝, 프레임 끝 순이다. calibration은 첫 쓰기 전 GPU 시계를 쌍으로 잡았다.
        const uint64 frequency = platform::MonotonicTickFrequency();
        auto expected = [&](uint64 writes) noexcept {
            return GpuTicksToTraceTicks(
                pair, pair.gpuTicks + writes * RecordingDevice::TimestampTickStep, frequency);
        };

        const uint32 outerName = NameId(decoder, records, StringView("Test.GpuOuter"));
        const uint32 innerName = NameId(decoder, records, StringView("Test.GpuInner"));
        const uint32 cpuName = NameId(decoder, records, StringView("Test.RenderFrame"));
        REQUIRE(outerName != 0 && innerName != 0 && cpuName != 0);
        const uint16 scopeType = RecordType(decoder, Format::GpuScopeType);
        const uint16 frameType = RecordType(decoder, Format::GpuFrameType);
        usize scopes = 0;
        uint64 frameBegin = 0;
        for (const DecodedRecord& record : records) {
            if (record.typeId == frameType) {
                frameBegin = ReadFixed<uint64>(record, Format::GpuFrameBeginOffset);
                CHECK(frameBegin == expected(1));
                CHECK(ReadFixed<uint64>(record, Format::GpuFrameEndOffset) == expected(6));
                CHECK(ReadFixed<uint64>(record, Format::GpuFrameNumberOffset) == 1);
            } else if (record.typeId == scopeType) {
                ++scopes;
                const uint32 name = ReadFixed<uint32>(record, Format::GpuScopeNameOffset);
                const uint32 depth = ReadFixed<uint32>(record, Format::GpuScopeDepthOffset);
                const uint64 begin = ReadFixed<uint64>(record, Format::GpuScopeBeginOffset);
                const uint64 end = ReadFixed<uint64>(record, Format::GpuScopeEndOffset);
                if (name == outerName) {
                    CHECK(depth == 0 && begin == expected(2) && end == expected(5));
                } else {
                    CHECK(name == innerName);
                    CHECK(depth == 1 && begin == expected(3) && end == expected(4));
                }
            }
        }

        CHECK(scopes == 2);
        // 같은 시간축: CPU 구간 "Test.RenderFrame"이 calibration을 감싸고 시작했으므로, GPU 프레임
        // 시작은 CPU 구간 시작 뒤이고 calibration CPU tick 뒤다.
        uint64 cpuBegin = 0;
        const uint16 batchType = RecordType(decoder, Format::CpuScopeBatchType);
        for (const DecodedRecord& record : records) {
            if (record.typeId != batchType) {
                continue;
            }

            Array<DecodedScopeEvent> events(allocator);
            REQUIRE(DecodeScopeBatch(record.variable[0], events));
            for (const DecodedScopeEvent& event : events) {
                if (event.begin && event.nameId == cpuName) {
                    cpuBegin = event.tick;
                }
            }
        }

        REQUIRE(cpuBegin != 0);
        CHECK(cpuBegin <= pair.cpuTicks);
        CHECK(pair.cpuTicks <= frameBegin);
    }
}

// timestamp·calibration이 없는 장치(GLES 확장 없음과 같다)는 queue 정의에 "GPU 시간 없음"을
// 공개하고 GPU 구간을 기록하지 않는다(추정값 없음). marker는 그대로 남는다.
TEST(GpuTrace_NoTimingIsPublishedWithoutEstimates) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(RegisterGpuTraceTypes(session));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        REQUIRE(session.SetChannelEnabled(GpuTraceChannel, true));

        RecordingDevice device(allocator);
        device.SetTimestampsSupported(false);
        GpuTrace trace(allocator);
        GpuRun run;
        REQUIRE(RunFrame(allocator, device, trace, run));
        session.Stop();

        CHECK(!run.afterCompletion.timing);
        CHECK(run.afterCompletion.scopesRecorded == 0);
        CHECK(run.afterCompletion.framesRecorded == 0);
        CHECK(run.afterCompletion.pending == 0);
        CHECK(run.afterCompletion.calibrations == 0);
        usize pushes = 0;
        usize timestamps = 0;
        for (const RecordingDevice::Call& call : device.Calls()) {
            pushes += call.op == RecordingDevice::Op::PushMarker ? 1 : 0;
            timestamps += call.op == RecordingDevice::Op::WriteTimestamp ? 1 : 0;
        }

        CHECK(pushes == (IsReleaseBuild ? 0u : 2u));
        CHECK(timestamps == 0);

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> records(allocator);
        CollectAll(decoder, records);
        const uint16 queueType = RecordType(decoder, Format::GpuQueueType);
        const uint16 scopeType = RecordType(decoder, Format::GpuScopeType);
        const uint16 calibrationType = RecordType(decoder, Format::GpuCalibrationType);
        usize queues = 0;
        for (const DecodedRecord& record : records) {
            CHECK(record.typeId != scopeType && record.typeId != calibrationType);
            if (record.typeId == queueType) {
                ++queues;
                CHECK(ReadFixedByte(record, Format::GpuQueueTimingOffset) == 0);
            }
        }

        CHECK(queues == 1);
    }
}

// Release 기본(marker 끔·timestamp 끔)을 설정으로 고르면 marker도 timestamp도 쓰지 않는다. 구간이
// 끊긴 timestamp(disjoint)를 받은 구간은 버린다.
TEST(GpuTrace_SettingsAndDisjointSamples) {
    SystemAllocator allocator;
    {
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(RegisterGpuTraceTypes(session));
        REQUIRE(session.SetChannelEnabled(GpuTraceChannel, true));

        RecordingDevice off(allocator);
        GpuTraceSettings releaseLike;
        releaseLike.markers = false;
        releaseLike.timestamps = false;
        GpuTrace quiet(allocator, releaseLike);
        GpuRun run;
        REQUIRE(RunFrame(allocator, off, quiet, run));
        for (const RecordingDevice::Call& call : off.Calls()) {
            CHECK(call.op != RecordingDevice::Op::PushMarker &&
                  call.op != RecordingDevice::Op::WriteTimestamp);
        }

        CHECK(!run.afterCompletion.timing);

        // 장치를 직접 쓰는 흐름: 구간 하나의 끝 timestamp가 disjoint로 오면 그 구간을 버린다.
        RecordingDevice device(allocator);
        graphics::TextureDesc desc;
        desc.width = 4;
        desc.height = 4;
        desc.usage = graphics::TextureUsage::RenderTarget;
        auto target = device.CreateTexture(desc, {});
        REQUIRE(target);
        GpuTrace trace(allocator);
        auto frame = device.BeginOffscreen(target.Value(), {});
        REQUIRE(frame);
        trace.BeginFrame(device, *frame.Value(), 1);
        {
            GpuScope scope(trace, *frame.Value(), OuterName);
        }

        trace.EndFrame(*frame.Value());
        REQUIRE(device.EndFrame());
        // 쓴 순서의 번호: 프레임 시작 1, 구간 시작 2, 구간 끝 3, 프레임 끝 4.
        RenderNotice notices[4];
        for (uint64 query = 1; query <= 4; ++query) {
            notices[query - 1].kind = RenderNoticeKind::GpuTimestamp;
            notices[query - 1].frame = query;
            notices[query - 1].value = query * 100;
            notices[query - 1].error = query == 3 ? ErrorCode::Cancelled : ErrorCode::None;
        }

        trace.Consume(Span<const RenderNotice>(notices, 2));
        CHECK(trace.Stats().pending == 2);
        trace.Consume(Span<const RenderNotice>(notices + 2, 2));
        const GpuTraceStats stats = trace.Stats();
        CHECK(stats.invalid == 1);
        CHECK(stats.scopesRecorded == 0);
        CHECK(stats.framesRecorded == 1);
        CHECK(stats.pending == 0);
        device.Retire(target.Value());
        REQUIRE(device.WaitIdle());
        session.Stop();
    }
}

// 정적 모듈 표로 새 로더를 만드는 시험이라 Monolithic에서만 돈다(Modular에는 정적 표가 없다 —
// Docs/Design/ModuleLoader.ko.md 3.14절, BuildTargets.ko.md 3.8절 BS8e).
#if ROCKS_MONOLITHIC
// RenderBase 모듈(Docs/Design/ModuleLoader.ko.md 3.12절): 호스트가 실행 중인 trace 세션을 서비스로
// 등록하면 모듈 Startup이 GPU 채널·형을 등록하고 모듈 lease를 잡으며, 묶음 종료가 등록을 풀고
// lease를 놓는다. 같은 세션에서 엔진 묶음을 다시 시작하면 다시 등록된다(세션이 같은 이름 채널·같은
// 스키마 형의 id를 다시 쓴다 — TraceSystem 개정 9). 서비스가 없으면 아무것도 등록하지 않는다. 이미
// 등록된 세션에 다시 등록하면 실패하되 앞 등록은 그대로 둔다.
TEST(GpuTrace_RenderBaseModuleRegistersThroughTheTraceService) {
    SystemAllocator allocator;
    {
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        {
            auto created = ModuleLoader::TryCreate(allocator, GeneratedModuleTable());
            REQUIRE(created);
            UniquePtr<ModuleLoader> modules = Move(created.Value());
            REQUIRE(modules->Load("RenderBase").IsOk());
            CHECK(session.SetChannelEnabled(GpuTraceChannel, true).Error() == ErrorCode::NotFound);
            CHECK(modules->LeaseCount("RenderBase") == 0);
            modules->ShutdownGroup(ModuleGroup::Process);
        }

        {
            auto created = ModuleLoader::TryCreate(allocator, GeneratedModuleTable());
            REQUIRE(created);
            UniquePtr<ModuleLoader> modules = Move(created.Value());
            REQUIRE(modules->Services().Register(session));
            for (uint32 round = 0; round < 2; ++round) {
                REQUIRE(modules->StartEngineGroup().IsOk());
                CHECK(modules->IsStarted("RenderBase"));
                CHECK(session.SetChannelEnabled(GpuTraceChannel, true));
                CHECK(modules->LeaseCount("RenderBase") == 1);
                CHECK(RegisterGpuTraceTypes(session).Error() == ErrorCode::AlreadyExists);
                CHECK(session.SetChannelEnabled(GpuTraceChannel, false));
                modules->ShutdownEngineGroup();
                CHECK(session.SetChannelEnabled(GpuTraceChannel, true).Error() ==
                      ErrorCode::NotFound);
                CHECK(modules->LeaseCount("RenderBase") == 0);
            }

            modules->ShutdownGroup(ModuleGroup::Process);
            CHECK(modules->Services().Unregister(session));
        }

        session.Stop();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
#endif // ROCKS_MONOLITHIC

// 형 등록이 중간에 실패하면 이 호출이 등록한 형과 채널만 되돌린다. 다른 채널에 같은 이름·다른
// 스키마의 Trace.GpuScope가 먼저 있으면 TypeMismatch이고 GPU 채널은 등록되지 않은 채로 남는다.
TEST(GpuTrace_RegisterRollsBackOnlyItsOwnRegistrations) {
    SystemAllocator allocator;
    {
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        TraceChannel other{"test.other", ""};
        TraceEventType conflicting{Format::GpuScopeType, other};
        REQUIRE(session.RegisterChannel(other, false));
        REQUIRE(session.RegisterEventType(conflicting));
        CHECK(RegisterGpuTraceTypes(session).Error() == ErrorCode::TypeMismatch);
        CHECK(session.SetChannelEnabled(GpuTraceChannel, true).Error() == ErrorCode::NotFound);
        CHECK(conflicting.Id() != TraceEventType::NoId);
        CHECK(UnregisterGpuTraceTypes(session));
        session.Stop();
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
