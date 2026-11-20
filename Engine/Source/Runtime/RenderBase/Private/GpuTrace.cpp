#include <RenderBase/GpuTrace.h>

#include <Base/Endian.h>
#include <Platform/Clock.h>
#include <TraceCapture/TraceWrite.h>

namespace rocks::render {

TraceChannel GpuTraceChannel{"gpu", "GPU scopes and frames (timestamps converted to trace ticks)"};

namespace {

using Format = TraceStandardFormat;

constexpr TraceField GpuQueueFields[] = {
    {"queue", TraceFieldType::UInt16, Format::GpuQueueIdOffset},
    {"kind", TraceFieldType::UInt8, Format::GpuQueueKindOffset},
    {"timing", TraceFieldType::Bool, Format::GpuQueueTimingOffset},
    {"name", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField GpuCalibrationFields[] = {
    {"queue", TraceFieldType::UInt16, Format::GpuCalibrationQueueOffset},
    {"gpuTicks", TraceFieldType::UInt64, Format::GpuCalibrationGpuTicksOffset},
    {"cpuTick", TraceFieldType::UInt64, Format::GpuCalibrationCpuTickOffset},
    {"gpuFrequency", TraceFieldType::UInt64, Format::GpuCalibrationFrequencyOffset},
    {"uncertaintyNs", TraceFieldType::UInt64, Format::GpuCalibrationUncertaintyOffset},
};

constexpr TraceField GpuScopeFields[] = {
    {"queue", TraceFieldType::UInt16, Format::GpuScopeQueueOffset},
    {"name", TraceFieldType::UInt32, Format::GpuScopeNameOffset},
    {"depth", TraceFieldType::UInt32, Format::GpuScopeDepthOffset},
    {"frame", TraceFieldType::UInt64, Format::GpuScopeFrameOffset},
    {"begin", TraceFieldType::UInt64, Format::GpuScopeBeginOffset},
    {"end", TraceFieldType::UInt64, Format::GpuScopeEndOffset},
};

constexpr TraceField GpuFrameFields[] = {
    {"queue", TraceFieldType::UInt16, Format::GpuFrameQueueOffset},
    {"frame", TraceFieldType::UInt64, Format::GpuFrameNumberOffset},
    {"begin", TraceFieldType::UInt64, Format::GpuFrameBeginOffset},
    {"end", TraceFieldType::UInt64, Format::GpuFrameEndOffset},
};

TraceEventType GpuQueueType{Format::GpuQueueType, GpuTraceChannel, Format::GpuQueueFixedBytes,
                            GpuQueueFields, TraceEventFlags::Definition};
TraceEventType GpuCalibrationType{Format::GpuCalibrationType, GpuTraceChannel,
                                  Format::GpuCalibrationFixedBytes, GpuCalibrationFields,
                                  TraceEventFlags::Definition};
TraceEventType GpuScopeType{Format::GpuScopeType, GpuTraceChannel, Format::GpuScopeFixedBytes,
                            GpuScopeFields};
TraceEventType GpuFrameType{Format::GpuFrameType, GpuTraceChannel, Format::GpuFrameFixedBytes,
                            GpuFrameFields};

// 고정부 byte 배열을 TraceWrite로 넘기는 껍데기다(배치는 TraceStandardFormat의 offset).
template <usize Bytes>
struct FixedRecord {
    uint8 bytes[Bytes] = {};
};

} // namespace

Result<void> RegisterGpuTraceTypes(TraceSession& session) noexcept {
    if (auto channel = session.RegisterChannel(GpuTraceChannel, false); !channel) {
        return channel;
    }

    TraceEventType* const types[] = {&GpuQueueType, &GpuCalibrationType, &GpuScopeType,
                                     &GpuFrameType};
    for (TraceEventType* type : types) {
        if (auto registered = session.RegisterEventType(*type); !registered) {
            return registered;
        }
    }

    return Result<void>::Success();
}

uint64 GpuTicksToTraceTicks(const graphics::ClockCalibration& calibration, uint64 gpuTicks,
                            uint64 traceFrequency) noexcept {
    if (calibration.gpuFrequency == 0) {
        return calibration.cpuTicks;
    }

    // 몫·나머지로 나눠 중간 곱의 overflow를 피한다(1초 미만 부분만 double).
    auto scale = [&](uint64 delta) noexcept {
        const uint64 seconds = delta / calibration.gpuFrequency;
        const uint64 remainder = delta % calibration.gpuFrequency;
        return seconds * traceFrequency +
               static_cast<uint64>(static_cast<double>(remainder) *
                                   static_cast<double>(traceFrequency) /
                                   static_cast<double>(calibration.gpuFrequency));
    };

    if (gpuTicks >= calibration.gpuTicks) {
        return calibration.cpuTicks + scale(gpuTicks - calibration.gpuTicks);
    }

    const uint64 before = scale(calibration.gpuTicks - gpuTicks);
    return before < calibration.cpuTicks ? calibration.cpuTicks - before : 0;
}

GpuTrace::GpuTrace(Allocator& allocator, const GpuTraceSettings& settings) noexcept
    : settings_(settings), pending_(allocator) {
    if (settings_.calibrationIntervalFrames == 0) {
        settings_.calibrationIntervalFrames = 1;
    }
}

void GpuTrace::WriteQueueDefinition(bool timing) noexcept {
    FixedRecord<Format::GpuQueueFixedBytes> fixed;
    StoreLittleEndian<uint16>(fixed.bytes + Format::GpuQueueIdOffset, settings_.queueId);
    // 종류 칸은 장치 queue 번호다(0 graphics, 1 async compute).
    fixed.bytes[Format::GpuQueueKindOffset] = static_cast<uint8>(settings_.queue);
    fixed.bytes[Format::GpuQueueTimingOffset] = timing ? 1 : 0;
    const TraceVariablePart parts[] = {TraceVariablePart::FromString(
        StringView(settings_.queueName != nullptr ? settings_.queueName : ""))};
    const TraceWriteResult written = TraceWrite(GpuQueueType, fixed, parts);
    (void)written;
}

void GpuTrace::WriteCalibration(const graphics::ClockCalibration& calibration) noexcept {
    FixedRecord<Format::GpuCalibrationFixedBytes> fixed;
    StoreLittleEndian<uint16>(fixed.bytes + Format::GpuCalibrationQueueOffset, settings_.queueId);
    StoreLittleEndian<uint64>(fixed.bytes + Format::GpuCalibrationGpuTicksOffset,
                              calibration.gpuTicks);
    StoreLittleEndian<uint64>(fixed.bytes + Format::GpuCalibrationCpuTickOffset,
                              calibration.cpuTicks);
    StoreLittleEndian<uint64>(fixed.bytes + Format::GpuCalibrationFrequencyOffset,
                              calibration.gpuFrequency);
    StoreLittleEndian<uint64>(fixed.bytes + Format::GpuCalibrationUncertaintyOffset,
                              calibration.uncertaintyNanoseconds);
    const TraceWriteResult written = TraceWrite(GpuCalibrationType, fixed);
    (void)written;
}

void GpuTrace::BeginFrame(graphics::Device& device, graphics::CommandContext& commands,
                          uint64 frame) noexcept {
    frame_ = frame;
    frameEntry_ = 0;
    depth_ = 0;
    const graphics::DeviceInfo info = device.Info();
    const bool enabled = GpuTraceChannel.IsEnabled();
    bool timing = enabled && settings_.timestamps && info.timestamps && info.clockCalibration;

    // 세션이 바뀌었으면(새 출력·새 세션) queue 정의와 calibration을 다시 쓴다.
    const uint64 serial = TraceCurrentSessionSerial();
    const bool newSession = enabled && serial != sessionSerial_;
    if (newSession) {
        haveCalibration_ = false;
    }

    if (timing &&
        (!haveCalibration_ || frame >= calibratedFrame_ + settings_.calibrationIntervalFrames)) {
        auto calibrated = device.CalibrateClocks(settings_.queue);
        if (calibrated) {
            calibration_ = calibrated.Value();
            calibratedFrame_ = frame;
            haveCalibration_ = true;
            platform::LockGuard lock(gate_);
            ++stats_.calibrations;
        }
    }

    // calibration을 한 번도 얻지 못했으면 GPU 시간은 없다(추정값을 만들지 않는다).
    timing = timing && haveCalibration_;
    if (newSession) {
        sessionSerial_ = serial;
        WriteQueueDefinition(timing);
        if (timing) {
            WriteCalibration(calibration_);
        }
    } else if (timing && calibratedFrame_ == frame) {
        WriteCalibration(calibration_);
    }

    timing_ = timing;
    {
        platform::LockGuard lock(gate_);
        stats_.timing = timing;
    }

    if (!timing_) {
        return;
    }

    auto begin = commands.WriteTimestamp();
    platform::LockGuard lock(gate_);
    Pending entry;
    entry.id = nextId_++;
    entry.frame = frame;
    entry.isFrame = true;
    entry.calibration = calibration_;
    if (begin) {
        entry.beginQuery = begin.Value();
    } else {
        entry.invalid = true;
    }

    if (pending_.TryPushBack(entry)) {
        frameEntry_ = entry.id;
        ++stats_.pending;
    }
}

void GpuTrace::EndFrame(graphics::CommandContext& commands) noexcept {
    if (frameEntry_ != 0) {
        CloseScope(commands, frameEntry_);
        frameEntry_ = 0;
    }

    timing_ = false;
}

GpuTrace::Pending* GpuTrace::Find(uint64 id) noexcept {
    for (Pending& entry : pending_) {
        if (entry.id == id) {
            return &entry;
        }
    }

    return nullptr;
}

uint64 GpuTrace::OpenScope(graphics::CommandContext& commands, TraceScopeName& name) noexcept {
    if (!timing_) {
        return 0;
    }

    // GPU 구간 이름은 CPU 구간과 같은 세션 이름 표를 쓴다(이름 정의가 먼저 나간다).
    const uint32 nameId = TraceResolveScopeName(name);
    if (nameId == 0) {
        return 0;
    }

    auto begin = commands.WriteTimestamp();
    platform::LockGuard lock(gate_);
    Pending entry;
    entry.id = nextId_++;
    entry.frame = frame_;
    entry.nameId = nameId;
    entry.depth = depth_;
    entry.calibration = calibration_;
    if (begin) {
        entry.beginQuery = begin.Value();
    } else {
        entry.invalid = true;
    }

    if (!pending_.TryPushBack(entry)) {
        ++stats_.invalid;
        return 0;
    }

    ++stats_.pending;
    return entry.id;
}

void GpuTrace::CloseScope(graphics::CommandContext& commands, uint64 id) noexcept {
    auto end = commands.WriteTimestamp();
    platform::LockGuard lock(gate_);
    Pending* entry = Find(id);
    if (entry == nullptr) {
        return;
    }

    entry->closed = true;
    if (end) {
        entry->endQuery = end.Value();
    } else {
        entry->invalid = true;
    }
}

void GpuTrace::Emit(const Pending& entry) noexcept {
    const uint64 frequency = platform::MonotonicTickFrequency();
    const uint64 begin = GpuTicksToTraceTicks(entry.calibration, entry.beginTicks, frequency);
    const uint64 end = GpuTicksToTraceTicks(entry.calibration, entry.endTicks, frequency);
    if (entry.isFrame) {
        FixedRecord<Format::GpuFrameFixedBytes> fixed;
        StoreLittleEndian<uint16>(fixed.bytes + Format::GpuFrameQueueOffset, settings_.queueId);
        StoreLittleEndian<uint64>(fixed.bytes + Format::GpuFrameNumberOffset, entry.frame);
        StoreLittleEndian<uint64>(fixed.bytes + Format::GpuFrameBeginOffset, begin);
        StoreLittleEndian<uint64>(fixed.bytes + Format::GpuFrameEndOffset, end);
        if (TraceWrite(GpuFrameType, fixed) == TraceWriteResult::Recorded) {
            ++stats_.framesRecorded;
        }

        return;
    }

    FixedRecord<Format::GpuScopeFixedBytes> fixed;
    StoreLittleEndian<uint16>(fixed.bytes + Format::GpuScopeQueueOffset, settings_.queueId);
    StoreLittleEndian<uint32>(fixed.bytes + Format::GpuScopeNameOffset, entry.nameId);
    StoreLittleEndian<uint32>(fixed.bytes + Format::GpuScopeDepthOffset, entry.depth);
    StoreLittleEndian<uint64>(fixed.bytes + Format::GpuScopeFrameOffset, entry.frame);
    StoreLittleEndian<uint64>(fixed.bytes + Format::GpuScopeBeginOffset, begin);
    StoreLittleEndian<uint64>(fixed.bytes + Format::GpuScopeEndOffset, end);
    if (TraceWrite(GpuScopeType, fixed) == TraceWriteResult::Recorded) {
        ++stats_.scopesRecorded;
    }
}

void GpuTrace::Consume(Span<const RenderNotice> notices) noexcept {
    platform::LockGuard lock(gate_);
    for (const RenderNotice& notice : notices) {
        if (notice.kind != RenderNoticeKind::GpuTimestamp || notice.queue != settings_.queue) {
            continue;
        }

        for (Pending& entry : pending_) {
            const bool begin = entry.beginQuery != 0 && entry.beginQuery == notice.frame;
            const bool end = entry.endQuery != 0 && entry.endQuery == notice.frame;
            if (!begin && !end) {
                continue;
            }

            // 구간이 끊긴 timestamp(disjoint)가 하나라도 있으면 그 구간은 잴 수 없다.
            entry.invalid = entry.invalid || notice.error != ErrorCode::None;
            if (begin) {
                entry.haveBegin = true;
                entry.beginTicks = notice.value;
            } else {
                entry.haveEnd = true;
                entry.endTicks = notice.value;
            }
        }
    }

    // 두 끝이 모두 정해진 항목을 쓰고 지운다. 쓰지 못한 끝(timestamp 쓰기 실패)이 있는 항목은 닫힌
    // 뒤 남은 끝이 오면 버린다.
    usize index = 0;
    while (index < pending_.Size()) {
        Pending& entry = pending_[index];
        const bool beginDone = entry.beginQuery == 0 || entry.haveBegin;
        const bool endDone = entry.closed && (entry.endQuery == 0 || entry.haveEnd);
        if (!beginDone || !endDone) {
            ++index;
            continue;
        }

        if (entry.invalid || !entry.haveBegin || !entry.haveEnd) {
            ++stats_.invalid;
        } else {
            Emit(entry);
        }

        --stats_.pending;
        auto removed = pending_.TryRemoveAt(index);
        (void)removed;
    }
}

GpuTraceStats GpuTrace::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    return stats_;
}

uint64 GpuTrace::BeginSpan(graphics::CommandContext& commands, TraceScopeName& name) noexcept {
    const uint64 entry = OpenScope(commands, name);
    ++depth_;
    return entry;
}

void GpuTrace::EndSpan(graphics::CommandContext& commands, uint64 span) noexcept {
    --depth_;
    if (span != 0) {
        CloseScope(commands, span);
    }
}

void GpuTrace::DropSpan(uint64 span) noexcept {
    --depth_;
    if (span == 0) {
        return;
    }

    platform::LockGuard lock(gate_);
    Pending* entry = Find(span);
    if (entry != nullptr) {
        entry->closed = true;
        entry->invalid = true;
    }
}

GpuScope::GpuScope(GpuTrace& trace, graphics::CommandContext& commands,
                   TraceScopeName& name) noexcept
    : trace_(trace), commands_(commands) {
    if (trace_.settings_.markers) {
        commands_.PushMarker(StringView(name.Name()));
    }

    entry_ = trace_.OpenScope(commands_, name);
    ++trace_.depth_;
}

GpuScope::~GpuScope() noexcept {
    --trace_.depth_;
    if (entry_ != 0) {
        trace_.CloseScope(commands_, entry_);
    }

    if (trace_.settings_.markers) {
        commands_.PopMarker();
    }
}

} // namespace rocks::render
