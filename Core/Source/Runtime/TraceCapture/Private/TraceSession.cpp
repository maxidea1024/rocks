#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Core/Guid.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <new>

namespace rocks {

using detail::TraceChannelAccess;
using detail::TraceEventTypeAccess;
using detail::TraceRecordShape;
using detail::TraceSessionState;
using detail::TraceThreadState;
using detail::TraceTypeEntry;
using platform::AtomicLoad;
using platform::AtomicStore;
using platform::MemoryOrder;

namespace detail {

TraceSessionState::TraceSessionState(Allocator& owner) noexcept
    : allocator(&owner), types(owner), schemaBytes(owner), definitionStore(owner), staging(owner),
      outputBuffer(owner), definitionBatch(owner), compressed(owner), recent(owner) {
}

namespace {

// 세션 내부 형의 스키마다(TraceFormat.h의 고정부 배치와 같다).
constexpr TraceField ChannelInfoFields[] = {
    {"channel", TraceFieldType::UInt16, TraceFormat::ChannelInfoIdOffset},
    {"readOnly", TraceFieldType::Bool, TraceFormat::ChannelInfoReadOnlyOffset},
    {"enabled", TraceFieldType::Bool, TraceFormat::ChannelInfoEnabledOffset},
    {"name", TraceFieldType::String, TraceField::Variable},
    {"description", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField ChannelStateFields[] = {
    {"tick", TraceFieldType::UInt64, TraceFormat::ChannelStateTickOffset},
    {"channel", TraceFieldType::UInt16, TraceFormat::ChannelStateIdOffset},
    {"enabled", TraceFieldType::Bool, TraceFormat::ChannelStateEnabledOffset},
};

constexpr TraceField ThreadInfoFields[] = {
    {"thread", TraceFieldType::UInt32, TraceFormat::ThreadInfoIdOffset},
    {"osThread", TraceFieldType::UInt64, TraceFormat::ThreadInfoOsIdOffset},
    {"sortOrder", TraceFieldType::Int32, TraceFormat::ThreadInfoSortOrderOffset},
    {"name", TraceFieldType::String, TraceField::Variable},
    {"group", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField LossFields[] = {
    {"startTick", TraceFieldType::UInt64, TraceFormat::LossStartTickOffset},
    {"endTick", TraceFieldType::UInt64, TraceFormat::LossEndTickOffset},
    {"channels", TraceFieldType::UInt16Array, TraceField::Variable},
    {"counts", TraceFieldType::UInt64Array, TraceField::Variable},
};

constexpr TraceField DefinitionLossFields[] = {
    {"tick", TraceFieldType::UInt64, TraceFormat::DefinitionLossTickOffset},
    {"dropped", TraceFieldType::UInt64, TraceFormat::DefinitionLossCountOffset},
};

constexpr TraceField SnapshotSyncFields[] = {
    {"tick", TraceFieldType::UInt64, TraceFormat::SnapshotSyncTickOffset},
    {"order", TraceFieldType::UInt64, TraceFormat::SnapshotSyncOrderOffset},
    {"droppedChunks", TraceFieldType::UInt64, TraceFormat::SnapshotSyncDroppedChunksOffset},
    {"droppedBytes", TraceFieldType::UInt64, TraceFormat::SnapshotSyncDroppedBytesOffset},
    {"skippedChunks", TraceFieldType::UInt64, TraceFormat::SnapshotSyncSkippedChunksOffset},
    {"complete", TraceFieldType::Bool, TraceFormat::SnapshotSyncCompleteOffset},
};

struct BuiltinType {
    uint16 id;
    const char* name;
    uint8 flags;
    usize fixedBytes;
    const TraceField* fields;
    usize fieldCount;
};

constexpr BuiltinType BuiltinTypes[] = {
    {TraceFormat::ChannelInfoTypeId, "Trace.ChannelInfo", TraceFormat::TypeFlagDefinition,
     TraceFormat::ChannelInfoFixedBytes, ChannelInfoFields,
     sizeof(ChannelInfoFields) / sizeof(TraceField)},
    {TraceFormat::ChannelStateTypeId, "Trace.ChannelState", TraceFormat::TypeFlagDefinition,
     TraceFormat::ChannelStateFixedBytes, ChannelStateFields,
     sizeof(ChannelStateFields) / sizeof(TraceField)},
    {TraceFormat::ThreadInfoTypeId, "Trace.ThreadInfo", TraceFormat::TypeFlagDefinition,
     TraceFormat::ThreadInfoFixedBytes, ThreadInfoFields,
     sizeof(ThreadInfoFields) / sizeof(TraceField)},
    {TraceFormat::LossTypeId, "Trace.Loss", 0, TraceFormat::LossFixedBytes, LossFields,
     sizeof(LossFields) / sizeof(TraceField)},
    {TraceFormat::DefinitionLossTypeId, "Trace.DefinitionLoss", TraceFormat::TypeFlagDefinition,
     TraceFormat::DefinitionLossFixedBytes, DefinitionLossFields,
     sizeof(DefinitionLossFields) / sizeof(TraceField)},
    {TraceFormat::SnapshotSyncTypeId, "Trace.SnapshotSync", TraceFormat::TypeFlagDefinition,
     TraceFormat::SnapshotSyncFixedBytes, SnapshotSyncFields,
     sizeof(SnapshotSyncFields) / sizeof(TraceField)},
};

} // namespace

Result<void> AppendTraceSchema(Array<uint8>& out, uint16 typeId, const char* name, uint16 channelId,
                               uint8 flags, usize fixedBytes, const TraceField* fields,
                               usize fieldCount) noexcept {
    const usize nameBytes = platform::CStringLength(name);
    // 기록 byte 수·형 id·채널 id·플래그·필드 수·고정부 byte 수·이름 길이·이름(TraceFormat.h).
    usize bytes = 5 * sizeof(uint16) + 2 * sizeof(uint8) + nameBytes;
    for (usize index = 0; index < fieldCount; ++index) {
        bytes += sizeof(uint8) + 2 * sizeof(uint16) + platform::CStringLength(fields[index].name);
    }

    if (bytes > NumericLimits<uint16>::Max()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto appended = out.TryAppendUninitialized(bytes);
    if (!appended) {
        return Result<void>::Failure(appended.Error());
    }

    uint8* cursor = appended.Value().Data();
    auto put16 = [&](uint16 value) {
        StoreLittleEndian<uint16>(cursor, value);
        cursor += sizeof(uint16);
    };
    auto put8 = [&](uint8 value) { *cursor++ = value; };
    auto putText = [&](const char* text, usize length) {
        put16(static_cast<uint16>(length));
        platform::MemoryCopy(cursor, text, length);
        cursor += length;
    };

    put16(static_cast<uint16>(bytes));
    put16(typeId);
    put16(channelId);
    put8(flags);
    put8(static_cast<uint8>(fieldCount));
    put16(static_cast<uint16>(fixedBytes));
    putText(name, nameBytes);
    for (usize index = 0; index < fieldCount; ++index) {
        put8(static_cast<uint8>(fields[index].type));
        put16(fields[index].offset);
        putText(fields[index].name, platform::CStringLength(fields[index].name));
    }

    return Result<void>::Success();
}

} // namespace detail

namespace {

constexpr usize NameLengthLimit = TraceFormat::MaxNameBytes;
// 채널 설명의 byte 상한이다. 채널 정보 정의 기록이 정의 기록 상한 안에 들어가게 한다.
constexpr usize DescriptionLengthLimit = 4096;

bool IsLower(char c) noexcept {
    return c >= 'a' && c <= 'z';
}

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool IsLetter(char c) noexcept {
    return IsLower(c) || (c >= 'A' && c <= 'Z');
}

// NUL 앞 길이가 1 … limit인지 본다(limit + 1 byte만 읽는다).
bool HasLength(const char* text, usize limit, usize& length) noexcept {
    if (text == nullptr) {
        return false;
    }

    length = 0;
    while (text[length] != '\0') {
        if (++length > limit) {
            return false;
        }
    }

    return length != 0;
}

// 소문자 점 경로: 조각마다 [a-z][a-z0-9_]*.
bool IsValidChannelName(const char* name) noexcept {
    usize length = 0;
    if (!HasLength(name, NameLengthLimit, length)) {
        return false;
    }

    bool segmentStart = true;
    for (usize index = 0; index < length; ++index) {
        const char c = name[index];
        if (c == '.') {
            if (segmentStart) {
                return false;
            }

            segmentStart = true;
            continue;
        }

        if (segmentStart ? !IsLower(c) : !(IsLower(c) || IsDigit(c) || c == '_')) {
            return false;
        }

        segmentStart = false;
    }

    return !segmentStart;
}

// 형 이름: 글자로 시작하고 글자·숫자·'_'·'.'만 쓴다(`Frame.Begin`). 필드 이름은 '.'을 쓰지 않는다.
bool IsValidIdentifier(const char* name, bool allowDot) noexcept {
    usize length = 0;
    if (!HasLength(name, NameLengthLimit, length) || !IsLetter(name[0])) {
        return false;
    }

    for (usize index = 1; index < length; ++index) {
        const char c = name[index];
        if (!(IsLetter(c) || IsDigit(c) || c == '_' || (allowDot && c == '.'))) {
            return false;
        }
    }

    return true;
}

// 필드 표를 검사하고 가변 필드 수를 센다.
Result<usize> CheckFields(const TraceEventType& type) noexcept {
    if (type.FieldCount() > TraceFormat::MaxFields ||
        type.FixedBytes() > NumericLimits<uint16>::Max() ||
        (type.FieldCount() != 0 && type.Fields() == nullptr)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    usize variableCount = 0;
    for (usize index = 0; index < type.FieldCount(); ++index) {
        const TraceField& field = type.Fields()[index];
        const usize elementBytes = TraceFieldElementBytes(field.type);
        if (elementBytes == 0 || !IsValidIdentifier(field.name, false)) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        if (IsVariableTraceField(field.type)) {
            if (field.offset != TraceField::Variable) {
                return Result<usize>::Failure(ErrorCode::InvalidArgument);
            }

            ++variableCount;
        } else if (field.offset == TraceField::Variable ||
                   field.offset + elementBytes > type.FixedBytes()) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize other = 0; other < index; ++other) {
            if (platform::CStringEquals(type.Fields()[other].name, field.name)) {
                return Result<usize>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<usize>::Success(variableCount);
}

TraceRecordShape ChannelStateShape(uint8 (&fixed)[TraceFormat::ChannelStateFixedBytes], uint16 id,
                                   bool enabled) noexcept {
    StoreLittleEndian<uint64>(fixed + TraceFormat::ChannelStateTickOffset,
                              platform::MonotonicTicks());
    StoreLittleEndian<uint16>(fixed + TraceFormat::ChannelStateIdOffset, id);
    fixed[TraceFormat::ChannelStateEnabledOffset] = enabled ? 1 : 0;
    TraceRecordShape shape;
    shape.typeId = TraceFormat::ChannelStateTypeId;
    shape.fixed = fixed;
    shape.fixedBytes = TraceFormat::ChannelStateFixedBytes;
    return shape;
}

void WriteChannelState(TraceSessionState& state, uint16 id, bool enabled) noexcept {
    uint8 fixed[TraceFormat::ChannelStateFixedBytes];
    if (!state.WriteDefinition(ChannelStateShape(fixed, id, enabled))) {
        state.definitions.NoteDropped();
    }
}

bool IsRegistered(const TraceSessionState& state, const TraceChannel& channel) noexcept {
    const uint16 id = channel.Id();
    return id < TraceFormat::MaxChannels && state.channels[id] == &channel;
}

// 세션 메모리를 모두 해제한다. writer 스레드는 join됐고 출력은 닫혔어야 한다.
void DestroyState(TraceSessionState* state) noexcept {
    Allocator& allocator = *state->allocator;
    TraceThreadState* thread = AtomicLoad<MemoryOrder::Acquire>(&state->threads);
    while (thread != nullptr) {
        TraceThreadState* next = thread->next;
        thread->~TraceThreadState();
        auto freed = allocator.Deallocate(thread);
        (void)freed;
        thread = next;
    }

    state->pool.Release();
    state->definitions.Release();
    state->names.Release();
    state->recent.Release();
    state->~TraceSessionState();
    auto freed = allocator.Deallocate(state);
    (void)freed;
}

Result<void> InitializeState(TraceSessionState& state) noexcept {
    state.serial = platform::AtomicFetchAdd(&detail::sessionSerialCounter, 1) + 1;
    state.baseTick = platform::MonotonicTicks();
    state.tickFrequency = platform::MonotonicTickFrequency();
    state.startUtcTicks = platform::UtcTicks();
    state.buildId = build::ToolBuildId();
    auto guid = TryNewGuid();
    if (!guid) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    state.guid = guid.Value();
    if (auto pool = state.pool.Initialize(*state.allocator, state.settings.poolMaxBytes); !pool) {
        return pool;
    }

    if (auto buffer =
            state.definitions.Initialize(*state.allocator, state.settings.definitionBufferBytes);
        !buffer) {
        return buffer;
    }

    if (auto names = state.names.Initialize(*state.allocator, state.settings.nameTableCapacity);
        !names) {
        return names;
    }

    if (auto writer = state.InitializeWriter(); !writer) {
        return writer;
    }

    if (auto recent = state.recent.Initialize(*state.allocator, state.settings.recentBytes);
        !recent) {
        return recent;
    }

    // 세션 내부 형(id 1 … FirstUserTypeId - 1)의 자리를 만들고 스키마를 넣는다.
    if (!state.types.TryResize(TraceFormat::FirstUserTypeId - 1)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (const auto& builtin : detail::BuiltinTypes) {
        TraceTypeEntry& entry = state.types[builtin.id - 1];
        entry.schemaOffset = state.schemaBytes.Size();
        if (auto appended = detail::AppendTraceSchema(
                state.schemaBytes, builtin.id, builtin.name, TraceFormat::NoChannelId,
                builtin.flags, builtin.fixedBytes, builtin.fields, builtin.fieldCount);
            !appended) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        entry.schemaBytes = state.schemaBytes.Size() - entry.schemaOffset;
        entry.present = true;
    }

    return Result<void>::Success();
}

} // namespace

TraceSession::~TraceSession() noexcept {
    Stop();
}

Result<void> TraceSession::Start(const TraceSessionSettings& settings) noexcept {
    if (state_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    constexpr usize MinimumNameTableCapacity = 1024;
    if (settings.allocator == nullptr || settings.poolMaxBytes < TraceBlockLayout::PageBytes ||
        settings.nameTableCapacity < MinimumNameTableCapacity ||
        (settings.nameTableCapacity & (settings.nameTableCapacity - 1)) != 0 ||
        (settings.writerMode == TraceWriterMode::Thread && settings.writerPeriodMs == 0) ||
        (settings.recentBytes != 0 &&
         settings.recentBytes < TraceSessionSettings::MinRecentBytes) ||
        settings.poolWarningPercent > 100 || settings.statsIntervalPumps == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = *settings.allocator;
    auto memory = allocator.Allocate(sizeof(TraceSessionState), alignof(TraceSessionState));
    if (!memory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    auto* state = ::new (memory.Value()) TraceSessionState(allocator);
    state->settings = settings;
    if (auto initialized = InitializeState(*state); !initialized) {
        DestroyState(state);
        return initialized;
    }

    // 현재 세션은 하나뿐이다. 공개한 뒤부터 다른 스레드의 TraceWrite가 이 세션을 본다.
    TraceSessionState* expected = nullptr;
    if (!platform::AtomicCompareExchangeStrong(&detail::currentSession, expected, state)) {
        DestroyState(state);
        return Result<void>::Failure(ErrorCode::Busy);
    }

    AtomicStore(&detail::currentSessionSerial, state->serial);

    if (settings.writerMode == TraceWriterMode::Thread) {
        if (state->writerThread.Start(&detail::RunTraceWriter, state) !=
            platform::ThreadStatus::Success) {
            AtomicStore(&detail::currentSessionSerial, uint64{0});
            AtomicStore(&detail::currentSession, static_cast<TraceSessionState*>(nullptr));
            detail::WaitForTraceProducers();
            DestroyState(state);
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        state->writerThreadStarted = true;
    }

    state_ = state;
    // 표준 채널(cpu·counter·mark·span·frame·context, 꺼진 상태)과 표준 형을 등록한다.
    detail::InstallTraceContextObserver();
    if (auto standard = detail::RegisterStandardTraceTypes(*this); !standard) {
        Stop();
        return standard;
    }

    return Result<void>::Success();
}

void TraceSession::Stop() noexcept {
    if (state_ == nullptr) {
        return;
    }

    TraceSessionState& state = *state_;
    // 1) 새 기록 입장 차단: 공개를 거두고 채널을 끈다.
    AtomicStore(&detail::currentSessionSerial, uint64{0});
    AtomicStore(&detail::currentSession, static_cast<TraceSessionState*>(nullptr));
    {
        platform::LockGuard lock(state.registryMutex);
        for (TraceChannel* channel : state.channels) {
            if (channel != nullptr) {
                TraceChannelAccess::SetEnabled(*channel, false);
            }
        }
    }

    // 2) 생산자 종료 확인: 이 세션을 본 기록 호출이 모두 끝나기를 기다린다.
    detail::WaitForTraceProducers();

    // 3) 마지막 비우기 4) 출력 닫기 5) writer join.
    if (state.writerThreadStarted) {
        {
            platform::LockGuard lock(state.wakeMutex);
            state.stopRequested = true;
        }

        state.wake.NotifyAll();
        if (state.writerThread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Trace writer thread failed to join");
        }
    } else {
        platform::LockGuard lock(state.writerMutex);
        state.FinalFlush();
        state.CloseOutputs();
    }

    // 6) 등록 해제와 메모리 해제. 채널·형 객체는 다음 세션에 다시 등록할 수 있다.
    for (TraceChannel* channel : state.channels) {
        if (channel != nullptr) {
            TraceChannelAccess::SetId(*channel, TraceChannel::NoId);
        }
    }

    for (const TraceTypeEntry& entry : state.types) {
        if (entry.object != nullptr) {
            TraceEventTypeAccess::SetId(*entry.object, TraceEventType::NoId);
        }
    }

    DestroyState(state_);
    state_ = nullptr;
}

Result<void> TraceSession::Pump() noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    platform::LockGuard lock(state_->writerMutex);
    state_->Pump();
    return Result<void>::Success();
}

Result<void> TraceSession::RegisterChannel(TraceChannel& channel, bool enabled) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    usize descriptionBytes = 0;
    if (!IsValidChannelName(channel.Name()) || channel.Description() == nullptr ||
        (channel.Description()[0] != '\0' &&
         !HasLength(channel.Description(), DescriptionLengthLimit, descriptionBytes))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    TraceSessionState& state = *state_;
    platform::LockGuard lock(state.registryMutex);
    if (channel.Id() != TraceChannel::NoId) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    for (const TraceChannel* registered : state.channels) {
        if (registered != nullptr && platform::CStringEquals(registered->Name(), channel.Name())) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    if (state.nextChannelId >= TraceFormat::MaxChannels) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 채널 정보를 먼저 정의 버퍼에 넣고 켠다. 그래서 이 채널의 기록은 항상 채널 정보 뒤에 나간다.
    const uint16 id = state.nextChannelId;
    uint8 fixed[TraceFormat::ChannelInfoFixedBytes];
    StoreLittleEndian<uint16>(fixed + TraceFormat::ChannelInfoIdOffset, id);
    fixed[TraceFormat::ChannelInfoReadOnlyOffset] = channel.IsReadOnly() ? 1 : 0;
    fixed[TraceFormat::ChannelInfoEnabledOffset] = enabled ? 1 : 0;
    const TraceVariablePart parts[] = {
        TraceVariablePart::FromString(StringView(channel.Name())),
        TraceVariablePart::FromString(StringView(channel.Description(), descriptionBytes))};
    TraceRecordShape shape;
    shape.typeId = TraceFormat::ChannelInfoTypeId;
    shape.fixed = fixed;
    shape.fixedBytes = sizeof(fixed);
    shape.variable = parts;
    if (!state.WriteDefinition(shape)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    state.channels[id] = &channel;
    ++state.nextChannelId;
    TraceChannelAccess::SetId(channel, id);
    TraceChannelAccess::SetEnabled(channel, enabled);
    return Result<void>::Success();
}

Result<void> TraceSession::UnregisterChannel(TraceChannel& channel) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    TraceSessionState& state = *state_;
    platform::LockGuard lock(state.registryMutex);
    if (!IsRegistered(state, channel)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    for (const TraceTypeEntry& entry : state.types) {
        if (entry.object != nullptr && &entry.object->Channel() == &channel) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
    }

    const uint16 id = channel.Id();
    TraceChannelAccess::SetEnabled(channel, false);
    WriteChannelState(state, id, false);
    state.channels[id] = nullptr;
    TraceChannelAccess::SetId(channel, TraceChannel::NoId);
    return Result<void>::Success();
}

namespace {

Result<void> ChangeChannel(TraceSessionState& state, TraceChannel& channel, bool enabled) noexcept {
    if (channel.IsReadOnly()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (channel.IsEnabled() != enabled) {
        // 켤 때는 상태 기록을 먼저 넣어 새 기록이 상태 변경 뒤에 나가게 한다.
        if (enabled) {
            WriteChannelState(state, channel.Id(), true);
            TraceChannelAccess::SetEnabled(channel, true);
        } else {
            TraceChannelAccess::SetEnabled(channel, false);
            WriteChannelState(state, channel.Id(), false);
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> TraceSession::SetChannelEnabled(TraceChannel& channel, bool enabled) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    platform::LockGuard lock(state_->registryMutex);
    if (!IsRegistered(*state_, channel)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return ChangeChannel(*state_, channel, enabled);
}

Result<void> TraceSession::SetChannelEnabled(StringView name, bool enabled) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    platform::LockGuard lock(state_->registryMutex);
    for (TraceChannel* channel : state_->channels) {
        if (channel != nullptr && StringView(channel->Name()) == name) {
            return ChangeChannel(*state_, *channel, enabled);
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> TraceSession::RegisterEventType(TraceEventType& type) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    if (!IsValidIdentifier(type.Name(), true)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto variableCount = CheckFields(type);
    if (!variableCount) {
        return Result<void>::Failure(variableCount.Error());
    }

    // 고정부(머리·순서 번호·길이 칸 포함)는 블록 하나에 들어가야 한다. 정의 형은 정의 기록 상한
    // 안이다.
    const bool definition = HasTraceEventFlag(type.Flags(), TraceEventFlags::Definition);
    const usize headBytes = sizeof(uint16) + sizeof(uint64) + type.FixedBytes() +
                            variableCount.Value() * sizeof(uint32);
    if (headBytes > TraceBlockLayout::PayloadBytes ||
        (definition && headBytes > TraceFormat::MaxDefinitionRecordBytes)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    TraceSessionState& state = *state_;
    platform::LockGuard lock(state.registryMutex);
    if (type.Id() != TraceEventType::NoId) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (!IsRegistered(state, type.Channel())) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (state.types.Size() >= TraceFormat::MaxTypeId) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 새 스키마를 표 끝에 써 본 뒤 같은 이름의 기존 스키마와 비교한다. 형 id 칸(앞 4 byte 중 뒤
    // 2 byte)을 뺀 나머지가 같으면 같은 스키마다.
    const uint16 id = static_cast<uint16>(state.types.Size() + 1);
    const usize offset = state.schemaBytes.Size();
    if (auto appended = detail::AppendTraceSchema(
            state.schemaBytes, id, type.Name(), type.Channel().Id(),
            static_cast<uint8>(type.Flags()), type.FixedBytes(), type.Fields(), type.FieldCount());
        !appended) {
        return appended;
    }

    const usize bytes = state.schemaBytes.Size() - offset;
    const usize nameBytes = platform::CStringLength(type.Name());
    constexpr usize NameOffset = 5 * sizeof(uint16) + 2 * sizeof(uint8);
    constexpr usize IdEnd = 2 * sizeof(uint16);
    const uint8* candidate = state.schemaBytes.Data() + offset;
    for (const TraceTypeEntry& entry : state.types) {
        if (!entry.present) {
            continue;
        }

        const uint8* existing = state.schemaBytes.Data() + entry.schemaOffset;
        const usize existingName = LoadLittleEndian<uint16>(existing + NameOffset - sizeof(uint16));
        if (existingName != nameBytes ||
            !platform::MemoryEquals(existing + NameOffset, type.Name(), nameBytes)) {
            continue;
        }

        const bool same =
            entry.schemaBytes == bytes &&
            platform::MemoryEquals(existing + IdEnd, candidate + IdEnd, bytes - IdEnd);
        auto shrunk = state.schemaBytes.TryResize(offset);
        (void)shrunk;
        return Result<void>::Failure(same ? ErrorCode::AlreadyExists : ErrorCode::TypeMismatch);
    }

    TraceTypeEntry entry;
    entry.object = &type;
    entry.schemaOffset = offset;
    entry.schemaBytes = bytes;
    entry.present = true;
    if (!state.types.TryPushBack(entry)) {
        auto shrunk = state.schemaBytes.TryResize(offset);
        (void)shrunk;
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    // 가변 필드 배치를 먼저 채우고 id를 release로 공개한다(기록 경로는 id를 acquire로 읽는다).
    TraceEventTypeAccess::SetLayout(type);
    TraceEventTypeAccess::SetId(type, id);
    return Result<void>::Success();
}

Result<void> TraceSession::UnregisterEventType(TraceEventType& type) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    platform::LockGuard lock(state_->registryMutex);
    const uint16 id = type.Id();
    if (id == TraceEventType::NoId || id > state_->types.Size() ||
        state_->types[id - 1].object != &type) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    state_->types[id - 1].object = nullptr;
    TraceEventTypeAccess::SetId(type, TraceEventType::NoId);
    return Result<void>::Success();
}

Result<TraceOutputId> TraceSession::AttachFileOutput(const char* path) noexcept {
    if (state_ == nullptr) {
        return Result<TraceOutputId>::Failure(ErrorCode::WrongContext);
    }

    if (path == nullptr || path[0] == '\0') {
        return Result<TraceOutputId>::Failure(ErrorCode::InvalidArgument);
    }

    auto memory = state_->allocator->Allocate(sizeof(platform::File), alignof(platform::File));
    if (!memory) {
        return Result<TraceOutputId>::Failure(ErrorCode::OutOfMemory);
    }

    auto* file = ::new (memory.Value()) platform::File();
    if (const platform::FileStatus opened = file->Open(path, platform::FileOpenMode::CreateAlways);
        !opened) {
        {
            platform::LockGuard lock(state_->writerMutex);
            state_->lastOutputFileStatus = opened;
            ++state_->outputFailures;
        }

        file->~File();
        auto freed = state_->allocator->Deallocate(file);
        (void)freed;
        return Result<TraceOutputId>::Failure(ErrorCode::DependencyFailed);
    }

    detail::TraceOutput output;
    output.file = file;
    platform::LockGuard lock(state_->writerMutex);
    return state_->AttachOutput(output);
}

Result<TraceOutputId> TraceSession::AttachCallbackOutput(TraceOutputWrite write,
                                                         void* context) noexcept {
    if (state_ == nullptr) {
        return Result<TraceOutputId>::Failure(ErrorCode::WrongContext);
    }

    if (write == nullptr) {
        return Result<TraceOutputId>::Failure(ErrorCode::InvalidArgument);
    }

    detail::TraceOutput output;
    output.write = write;
    output.context = context;
    platform::LockGuard lock(state_->writerMutex);
    return state_->AttachOutput(output);
}

Result<void> TraceSession::DetachOutput(TraceOutputId output) noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    platform::LockGuard lock(state_->writerMutex);
    return state_->DetachOutput(output);
}

TraceSessionStats TraceSession::Stats() const noexcept {
    TraceSessionStats stats;
    if (state_ == nullptr) {
        return stats;
    }

    TraceSessionState& state = *state_;
    stats.poolBytesInUse = state.pool.BlocksInUse() * TraceBlockLayout::BlockBytes;
    stats.poolBytesPeak = state.pool.PeakBlocksInUse() * TraceBlockLayout::BlockBytes;
    stats.poolBytesReserved = state.pool.ReservedPages() * TraceBlockLayout::PageBytes;
    stats.poolBytesLimit = static_cast<uint64>(state.pool.MaxPages()) * TraceBlockLayout::PageBytes;
    stats.lostRecords = AtomicLoad<MemoryOrder::Relaxed>(&state.unattributedLost);
    for (const detail::TraceThreadState* thread = AtomicLoad<MemoryOrder::Acquire>(&state.threads);
         thread != nullptr; thread = thread->next) {
        stats.lostRecords += AtomicLoad<MemoryOrder::Relaxed>(&thread->lostTotal);
    }

    stats.definitionRecordsDropped = state.definitions.Dropped();
    stats.scopeNames = state.names.Count();
    stats.scopeNamesRejected = state.names.Rejected();
    {
        platform::LockGuard registry(state.registryMutex);
        stats.threadCount = state.nextThreadId - 1;
        stats.threadSlots = state.threadSlots;
    }

    platform::LockGuard lock(state.writerMutex);
    stats.definitionStoreBytes = state.definitionStore.Size();
    stats.definitionRecordsNotStored = state.definitionRecordsNotStored;
    stats.outputBytes = state.outputBytes;
    stats.outputFailures = state.outputFailures;
    stats.writerDroppedChunks = state.writerDroppedChunks;
    stats.lastOutputFileStatus = state.lastOutputFileStatus;
    stats.lastOutputCallbackError = state.lastOutputCallbackError;
    stats.recentCapacity = state.recent.Capacity();
    stats.recentBytes = state.recent.UsedBytes();
    stats.recentChunks = state.recent.Chunks();
    stats.recentDroppedChunks = state.recent.DroppedChunks();
    stats.recentDroppedBytes = state.recent.DroppedBytes();
    stats.poolWarnings = state.poolWarnings;
    stats.statsRecords = state.statsRecords;
    return stats;
}

uint64 TraceSession::BaseTick() const noexcept {
    return state_ != nullptr ? state_->baseTick : 0;
}

uint64 TraceSession::TickFrequency() const noexcept {
    return state_ != nullptr ? state_->tickFrequency : 0;
}

Guid TraceSession::SessionGuid() const noexcept {
    return state_ != nullptr ? state_->guid : Guid{};
}

} // namespace rocks
