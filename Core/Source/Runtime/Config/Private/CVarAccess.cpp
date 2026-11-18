#include "ConfigState.h"

#include <Config/CVarAccess.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ValueText.h>
#include <Platform/Clock.h>

// CVar 읽기·쓰기 경로와 쓰기 큐다(Docs/Design/CVarSystem.ko.md 6.2·6.3·7.1·10.1절 — CVR2): 아무
// 스레드의 쓰기를 받는 큐(통로), Sync가 꺼내 적용 전에 다시 검사하기, snapshot 얻기·묶기, 핸들의
// Set·Clear, 범위 임시 변경 태그.
namespace rocks::config {

namespace detail {

Result<void> Enqueue(WriteChannel& channel, Span<UniquePtr<QueuedChange>> changes) noexcept {
    platform::LockGuard lock(channel.mutex);
    if (!channel.open) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (channel.queue.Size() + changes.Size() > MaxQueuedChanges) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (auto reserved = channel.queue.TryReserve(channel.queue.Size() + changes.Size());
        !reserved) {
        return reserved;
    }

    for (UniquePtr<QueuedChange>& change : changes) {
        static_cast<void>(channel.queue.TryPushBack(Move(change))); // 예약했다
    }

    return Result<void>::Success();
}

bool HasQueuedChanges(WriteChannel& channel) noexcept {
    platform::LockGuard lock(channel.mutex);
    return !channel.queue.IsEmpty();
}

Result<void> DrainQueue(ConfigState& state) noexcept {
    WriteChannel& channel = *state.channel;
    const usize first = state.queue.Size();
    {
        platform::LockGuard lock(channel.mutex);
        if (channel.queue.IsEmpty()) {
            return Result<void>::Success();
        }

        if (auto reserved = state.queue.TryReserve(first + channel.queue.Size()); !reserved) {
            return reserved;
        }

        for (UniquePtr<QueuedChange>& change : channel.queue) {
            static_cast<void>(state.queue.TryPushBack(Move(change))); // 예약했다
        }

        channel.queue.Clear();
    }

    // 넣은 순서대로 입력 순번을 매기고, 다른 스레드에서 넣은 쓰기를 소유 스레드에서 다시 검사한다
    // (진단을 남긴다 — 거부되면 버린다). 검사가 할당 실패로 끝나면 남은 것은 다음 Sync가 이어서
    // 본다.
    usize kept = 0;
    Result<void> status = Result<void>::Success();
    for (usize index = 0; index < state.queue.Size(); ++index) {
        UniquePtr<QueuedChange>& change = state.queue[index];
        bool keep = true;
        if (status && change->entry.Get() != nullptr && change->validateAtApply) {
            auto valid = ValidateEntry(state, *change->entry);
            if (!valid) {
                status = Result<void>::Failure(valid.Error());
            } else {
                keep = valid.Value();
                change->validateAtApply = false;
            }
        }

        if (!keep) {
            continue;
        }

        if (change->entry.Get() != nullptr && change->entry->sequence == 0) {
            change->entry->sequence = state.nextSequence++;
        }

        if (kept != index) {
            state.queue[kept] = Move(change);
        }

        ++kept;
    }

    static_cast<void>(state.queue.TryResize(kept)); // 줄이기는 할당하지 않는다
    return status;
}

namespace {

// 정의의 값(검사를 거친 value)을 콘솔 표기로 out에 쓴다(Sync가 값 표기로 다시 읽는다 — 1.5·14.1절).
Result<void> FormatDefinitionValue(const cvar::detail::VarDefinition& definition,
                                   const cvar::detail::ScalarValue& value,
                                   StringBuilderBase& out) noexcept {
    switch (definition.type) {
    case ConsoleValueType::Bool:
        return out.TryAppend(value.boolean ? StringView("true") : StringView("false"));
    case ConsoleValueType::Int32:
    case ConsoleValueType::Int64:
        return out.TryAppendFormat("{}", value.integer);
    case ConsoleValueType::Float:
        return out.TryAppendFormat("{}", static_cast<float>(value.real));
    case ConsoleValueType::Double:
        return out.TryAppendFormat("{}", value.real);
    case ConsoleValueType::String:
    case ConsoleValueType::Name:
        return out.TryAppend(value.text);
    case ConsoleValueType::Enum: {
        const EnumeratorSchema* enumerator =
            definition.enumSchema != nullptr
                ? rocks::detail::FindEnumeratorByValue(definition.enumSchema(), value.integer)
                : nullptr;
        if (enumerator == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const Span<const char> name = EffectiveToolName(*enumerator);
        return out.TryAppend(StringView(name.Data(), name.Size()));
    }
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 이 스레드에 묶인 문맥의 통로다(없으면 null).
WriteChannel* BoundChannel() noexcept {
    const ConfigSnapshot* snapshot = CurrentBinding.snapshot;
    return snapshot != nullptr ? SnapshotAccess::Channel(*snapshot).Get() : nullptr;
}

// 쓰기 대상 (정의, 층)의 공통 검사다.
Result<void> CheckWriteTarget(const cvar::detail::VarDefinition& definition,
                              const cvar::SetOptions& options, WriteChannel*& channel) noexcept {
    if (definition.name.IsEmpty()) {
        // 출시 빌드의 치트 변수(13.1절 — 등록되지 않는다).
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!IsConfigLayer(options.layer) || options.layer == ConfigLayer::CodeDefault) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    channel = BoundChannel();
    if (channel == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return Result<void>::Success();
}

// 쓰기 하나(entry 없음이면 지우기)를 만든다.
Result<void> MakeChange(Allocator& allocator, const cvar::detail::VarDefinition& definition,
                        const cvar::SetOptions& options, UniquePtr<QueuedChange>& out) noexcept {
    auto change = UniquePtr<QueuedChange>::TryMake(allocator, allocator);
    if (!change) {
        return Result<void>::Failure(change.Error());
    }

    change.Value()->layer = options.layer;
    Result<void> assigned = change.Value()->tag.TryAssign(options.tag);
    if (assigned) {
        assigned = change.Value()->path.TryAssign(definition.name);
    }

    if (!assigned) {
        return assigned;
    }

    out = Move(change.Value());
    return Result<void>::Success();
}

} // namespace

} // namespace detail

using detail::ConfigState;
using detail::WriteChannel;

SharedPtr<const ConfigSnapshot> ConfigContext::Acquire() const noexcept {
    const ConfigState* state = state_.Get();
    if (state == nullptr) {
        return SharedPtr<const ConfigSnapshot>();
    }

    platform::LockGuard lock(state->channel->mutex);
    return state->channel->current;
}

Result<void> ConfigContext::ClearLayer(ConfigLayer layer) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr || !IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto change = UniquePtr<detail::QueuedChange>::TryMake(*allocator_, *allocator_);
    if (!change) {
        return Result<void>::Failure(change.Error());
    }

    change.Value()->layer = layer;
    change.Value()->allTags = true;
    UniquePtr<detail::QueuedChange> one[] = {Move(change.Value())};
    return detail::Enqueue(*state->channel, Span<UniquePtr<detail::QueuedChange>>(one));
}

MulticastDelegate<void(const SyncChanges&)>& ConfigContext::OnSynced() noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        platform::Fatal("ConfigContext::OnSynced: the context is not initialized");
    }

    if (platform::CurrentThreadId() != state->owner) {
        platform::Fatal("ConfigContext used from a thread other than its owner");
    }

    return state->onSynced;
}

} // namespace rocks::config

namespace rocks::cvar {

using config::detail::SnapshotAccess;
using config::detail::ThreadBinding;
using config::detail::WriteChannel;

SnapshotBinding::SnapshotBinding(SharedPtr<const config::ConfigSnapshot> snapshot) noexcept
    : snapshot_(Move(snapshot)), previous_(config::detail::CurrentBinding),
      thread_(platform::CurrentThreadId()) {
    if (snapshot_.Get() == nullptr) {
        platform::Fatal("cvar::SnapshotBinding needs a snapshot (ConfigContext::Acquire)");
    }

    if (previous_.gameLane != nullptr && previous_.snapshot != nullptr) {
        previousGameLane_ = SnapshotAccess::Channel(*previous_.snapshot);
    }

    platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(
        &SnapshotAccess::Channel(*snapshot_)->bindings, uint64{1});
    config::detail::CurrentBinding = ThreadBinding{snapshot_.Get(), nullptr};
}

SnapshotBinding::~SnapshotBinding() {
    if (platform::CurrentThreadId() != thread_ ||
        config::detail::CurrentBinding.snapshot != snapshot_.Get()) {
        platform::Fatal("cvar::SnapshotBinding released on another thread or out of order");
    }

    if (previousGameLane_.Get() != nullptr) {
        // 그 사이 Sync가 새 snapshot을 게시했을 수 있으므로 그 문맥의 최신 snapshot에 다시 묶는다.
        platform::LockGuard lock(previousGameLane_->mutex);
        config::detail::CurrentBinding =
            previousGameLane_->open
                ? ThreadBinding{previousGameLane_->current.Get(), previousGameLane_.Get()}
                : ThreadBinding{};
    } else {
        config::detail::CurrentBinding = previous_;
    }

    platform::AtomicFetchSub<platform::MemoryOrder::AcquireRelease>(
        &SnapshotAccess::Channel(*snapshot_)->bindings, uint64{1});
}

namespace detail {

namespace {

// 범위 임시 변경 태그 번호다(프로세스 전체 — 상수 초기화).
uint64 NextScopedTag = 0;

} // namespace

void FatalUnbound(const VarDefinition& definition) noexcept {
    Log::Fatal(config::detail::LogConfig,
               "CVar '{Name}' read on a thread without a snapshot binding; bind one with "
               "cvar::SnapshotBinding",
               definition.name);
}

void FatalUnregistered(const VarDefinition& definition) noexcept {
    Log::Fatal(config::detail::LogConfig,
               "CVar '{Name}' is not registered; pass it to ConfigContext::Register in its module",
               definition.name);
}

Result<void> SubmitSet(const VarDefinition& definition, ScalarValue value,
                       const SetOptions& options, SourceLocation where) noexcept {
    WriteChannel* channel = nullptr;
    if (auto checked = config::detail::CheckWriteTarget(definition, options, channel); !checked) {
        return checked;
    }

    // 층 권한(콘솔 읽기 전용·사용자 설정·출시 규칙의 치트)과 범위·Validate를 지금 검사한다. 적용 때
    // Sync가 다시 검사한다(6.2절).
    config::ConfigDiagnosticCode code = config::ConfigDiagnosticCode::LayerNotAllowed;
    if (!config::detail::CheckConsoleLayer(channel->releaseRules, options.layer,
                                           config::detail::ConsoleFlagsOfDefinition(definition),
                                           code)
             .IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // `Clamp()` 변수는 보정해 받는다. 넣는 값은 원래 값이다(적용 때 다시 보정하고 Clamped를
    // 남긴다).
    ScalarValue checkedValue = value;
    StringView reason;
    switch (definition.check(definition, checkedValue, reason)) {
    case ValueCheck::Accepted:
    case ValueCheck::Clamped:
        break;
    case ValueCheck::OutOfRange:
    case ValueCheck::ValidationFailed:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = channel->allocator;
    StringBuilder<64> text(allocator);
    if (auto formatted = config::detail::FormatDefinitionValue(definition, value, text);
        !formatted) {
        return formatted;
    }

    StringBuilder<128> source(allocator);
    static_cast<void>(source.TryAppendFormat("{}:{}", StringView(where.file), where.line));
    if (auto status = source.Status(); !status) {
        return status;
    }

    UniquePtr<config::detail::QueuedChange> change;
    if (auto made = config::detail::MakeChange(allocator, definition, options, change); !made) {
        return made;
    }

    auto entry = UniquePtr<config::detail::ConfigEntry>::TryMake(allocator, allocator);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    config::detail::ConfigEntry& added = *entry.Value();
    added.kind = config::detail::EntryKind::Console;
    added.layer = options.layer;
    Result<void> assigned = added.tag.TryAssign(options.tag);
    if (assigned) {
        assigned = added.sourceName.TryAssign(source.View());
    }

    if (assigned) {
        assigned = added.path.TryAssign(definition.name);
    }

    if (assigned) {
        assigned = added.value.TryAssign(text.View());
    }

    if (!assigned) {
        return assigned;
    }

    change->entry = Move(entry.Value());
    change->replace = true;
    change->validateAtApply = true;
    UniquePtr<config::detail::QueuedChange> one[] = {Move(change)};
    return config::detail::Enqueue(*channel, Span<UniquePtr<config::detail::QueuedChange>>(one));
}

Result<void> SubmitClear(const VarDefinition& definition, const SetOptions& options) noexcept {
    WriteChannel* channel = nullptr;
    if (auto checked = config::detail::CheckWriteTarget(definition, options, channel); !checked) {
        return checked;
    }

    UniquePtr<config::detail::QueuedChange> change;
    if (auto made = config::detail::MakeChange(channel->allocator, definition, options, change);
        !made) {
        return made;
    }

    UniquePtr<config::detail::QueuedChange> one[] = {Move(change)};
    return config::detail::Enqueue(*channel, Span<UniquePtr<config::detail::QueuedChange>>(one));
}

StringView MakeScopedTag(char (&tag)[ScopedTagBytes]) noexcept {
    const uint64 number =
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&NextScopedTag, uint64{1});
    char digits[rocks::detail::DecimalTextBytes];
    const Span<const char> decimal =
        rocks::detail::FormatDecimalText(number, false, digits, sizeof(digits));
    constexpr char Prefix[] = "scoped.";
    constexpr usize PrefixBytes = sizeof(Prefix) - 1;
    usize length = 0;
    for (; length < PrefixBytes; ++length) {
        tag[length] = Prefix[length];
    }

    for (const char c : decimal) {
        tag[length++] = c;
    }

    tag[length] = '\0';
    return StringView(tag, length);
}

void EndScopedOverride(const VarDefinition& definition, StringView tag) noexcept {
    SetOptions options;
    options.layer = config::ConfigLayer::Scoped;
    options.tag = tag;
    auto cleared = SubmitClear(definition, options);
    if (!cleared && cleared.Error() != ErrorCode::ShuttingDown) {
        Log::Fatal(config::detail::LogConfig,
                   "cvar::ScopedOverride of '{Name}' could not queue its removal ({Error})",
                   definition.name, ErrorCodeName(cleared.Error()));
    }
}

} // namespace detail

} // namespace rocks::cvar
