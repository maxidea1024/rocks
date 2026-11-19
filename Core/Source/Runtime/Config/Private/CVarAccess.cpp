#include "ConfigState.h"

#include <Config/CVar.h>
#include <Config/CVarAccess.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ValueText.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>

// CVar 읽기·쓰기 경로와 쓰기 큐다(Docs/Design/CVarSystem.ko.md 6.2·6.3·7.1·10.1절 — CVR2): 아무
// 스레드의 쓰기를 받는 큐(통로), Sync가 꺼내 적용 전에 다시 검사하기, snapshot 얻기·묶기, 핸들의
// Set·Clear, 범위 임시 변경.
namespace rocks::config {

namespace detail {

Result<void> Enqueue(WriteChannel& channel, Span<SharedPtr<QueuedChange>> changes,
                     bool limited) noexcept {
    platform::LockGuard lock(channel.mutex);
    if (!channel.open) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (limited && channel.limitedChanges + changes.Size() > MaxQueuedChanges) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 미리 만든 범위 임시 변경 지우기가 들어올 자리도 늘 남겨 둔다(SubmitRemoval은 할당하지
    // 않는다).
    if (auto reserved = channel.queue.TryEnsureCapacity(channel.queue.Size() + changes.Size() +
                                                        channel.reservedRemovals);
        !reserved) {
        return reserved;
    }

    for (SharedPtr<QueuedChange>& change : changes) {
        static_cast<void>(channel.queue.TryPushBack(Move(change))); // 예약했다
    }

    channel.limitedChanges += limited ? changes.Size() : 0;
    return Result<void>::Success();
}

Result<void> ReserveRemoval(WriteChannel& channel) noexcept {
    platform::LockGuard lock(channel.mutex);
    if (!channel.open) {
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (auto reserved =
            channel.queue.TryEnsureCapacity(channel.queue.Size() + channel.reservedRemovals + 1);
        !reserved) {
        return reserved;
    }

    ++channel.reservedRemovals;
    return Result<void>::Success();
}

void SubmitRemoval(WriteChannel& channel, SharedPtr<QueuedChange> change) noexcept {
    platform::LockGuard lock(channel.mutex);
    --channel.reservedRemovals;
    if (!channel.open || change.Get() == nullptr) {
        return; // 문맥이 없어졌거나(지울 값도 없다) 자리만 돌려준다.
    }

    if (!channel.queue.TryPushBack(Move(change))) {
        platform::Fatal("cvar::ScopedOverride lost its reserved queue slot");
    }
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

        if (auto reserved = state.queue.TryEnsureCapacity(first + channel.queue.Size());
            !reserved) {
            return reserved;
        }

        for (SharedPtr<QueuedChange>& change : channel.queue) {
            static_cast<void>(state.queue.TryPushBack(Move(change))); // 예약했다
        }

        // 용량은 남는다(지우기 자리 예약이 그대로 유효하다).
        channel.queue.Clear();
        channel.limitedChanges = 0;
    }

    // 넣은 순서대로 입력 순번을 매기고, 다른 스레드에서 넣은 쓰기를 소유 스레드에서 다시 검사한다
    // (진단을 남긴다 — 거부되면 버린다). 검사가 할당 실패로 끝나면 남은 것은 다음 Sync가 이어서
    // 본다.
    // 치트 허용 상태는 큐 순서로 따라간다(CVar 13.1절 — 앞에서 치트를 끈 변경 뒤의 치트 쓰기는
    // 거부한다). 적용 입력 목록을 큐 순서로 흉내 내며 `cheat.enabled`를 바꿀 수 있는 항목 뒤에 다시
    // 계산한다.
    Array<ConfigEntry*> plan(state.allocator);
    for (UniquePtr<ConfigEntry>& entry : state.entries) {
        if (auto pushed = plan.TryPushBack(entry.Get()); !pushed) {
            return pushed;
        }
    }

    bool cheats = CheatsAllowed(state);
    usize kept = 0;
    Result<void> status = Result<void>::Success();
    for (usize index = 0; index < state.queue.Size(); ++index) {
        SharedPtr<QueuedChange>& change = state.queue[index];
        bool keep = true;
        // 다른 스레드의 콘솔 입력 한 줄은 쓰기가 아니라 게임 lane이 처리할 일이다(CVar 15절).
        if (status && !change->line.IsEmpty()) {
            status = QueueConsoleLine(state, *change);
            keep = !status;
        } else if (status && change->entry.Get() != nullptr && change->validateAtApply) {
            const usize before = state.diagnostics.Count();
            auto valid = ValidateEntry(state, *change->entry);
            if (!valid) {
                status = Result<void>::Failure(valid.Error());
            } else {
                keep = valid.Value();
                change->validateAtApply = false;
                // 거부한 다른 스레드 콘솔 입력은 그 입력의 sink에 알린다.
                for (usize at = before; !keep && at < state.diagnostics.Count(); ++at) {
                    const ConfigDiagnostic item = state.diagnostics.Get(at);
                    StringBuilder<192> line(state.allocator);
                    static_cast<void>(
                        line.TryAppendFormat("{} {}\n", change->path.View(), item.message));
                    if (change->sink.Get() != nullptr && line.Status()) {
                        static_cast<void>(change->sink->Write(line.View()));
                    }
                }
            }
        }

        // 범위 쓰기는 층 입력 목록에 들지 않는다. 그 자리의 Global 치트 상태만 적어 두고 치트
        // 판정은 Sync가 범위 값에 큐 순서로 적용할 때 한다(13.1절 — 같은 큐의 앞 `cheat.enabled`
        // 범위 쓰기를 따라간다).
        if (change->scope != 0) {
            change->globalCheats = cheats;
        }

        // 콘솔 층 쓰기의 권한(치트·복제)은 적용 때 큐 순서의 치트 상태로 다시 본다(6.2·13.1절).
        if (status && keep && change->scope == 0 && change->entry.Get() != nullptr &&
            change->entry->layer == ConfigLayer::Console) {
            auto allowed = RecheckConsolePermission(state, *change, cheats);
            if (!allowed) {
                status = Result<void>::Failure(allowed.Error());
            } else {
                keep = allowed.Value();
            }
        }

        if (!keep) {
            continue;
        }

        if (change->entry.Get() != nullptr && change->entry->sequence == 0) {
            change->entry->sequence = state.nextSequence++;
            // 봉인 중에는 큐가 비어 있으므로(CompleteStartup은 남은 쓰기가 있으면 Busy) 여기서 본
            // 봉인 상태가 넣은 때의 상태다.
            change->entry->sealedDirectWrite =
                state.startupComplete && IsDirectWriteLayer(change->entry->layer);
        }

        if (status && change->line.IsEmpty() && change->scope == 0) {
            status = ApplyQueuedChange(plan, *change);
            if (status && ChangeTouchesCheats(*change)) {
                auto recomputed = CheatsOfPlan(state, plan.View());
                if (recomputed) {
                    cheats = recomputed.Value();
                } else {
                    status = Result<void>::Failure(recomputed.Error());
                }
            }
        }

        if (kept != index) {
            state.queue[kept] = Move(change);
        }

        ++kept;
    }

    static_cast<void>(state.queue.TryResize(kept)); // 줄이기는 할당하지 않는다
    return status;
}

void RebindGameLane(ConfigState& state) noexcept {
    ThreadBinding& binding = CurrentBinding;
    if (binding.depth == 0) {
        binding.snapshot = state.snapshot.Get();
        binding.gameLane = state.channel.Get();
    }
}

namespace {

// 정의의 값(검사를 거친 value)을 콘솔 표기로 out에 쓴다(Sync가 값 표기로 다시 읽는다 — 1.5·14.1절).
// enum 값이 이름 표에 없으면 false다.
Result<bool> FormatDefinitionValue(const cvar::detail::VarDefinition& definition,
                                   const cvar::detail::ScalarValue& value,
                                   StringBuilderBase& out) noexcept {
    Result<void> written = Result<void>::Success();
    switch (definition.type) {
    case ConsoleValueType::Bool:
        written = out.TryAppend(value.boolean ? StringView("true") : StringView("false"));
        break;
    case ConsoleValueType::Int32:
    case ConsoleValueType::Int64:
        written = out.TryAppendFormat("{}", value.integer);
        break;
    case ConsoleValueType::Float:
        written = out.TryAppendFormat("{}", static_cast<float>(value.real));
        break;
    case ConsoleValueType::Double:
        written = out.TryAppendFormat("{}", value.real);
        break;
    case ConsoleValueType::String:
    case ConsoleValueType::Name:
        written = out.TryAppend(value.text);
        break;
    case ConsoleValueType::Enum: {
        const EnumeratorSchema* enumerator =
            definition.enumSchema != nullptr
                ? rocks::detail::FindEnumeratorByValue(definition.enumSchema(), value.integer)
                : nullptr;
        if (enumerator == nullptr) {
            return Result<bool>::Success(false);
        }

        const Span<const char> name = EffectiveToolName(*enumerator);
        written = out.TryAppend(StringView(name.Data(), name.Size()));
        break;
    }
    }

    if (!written) {
        return Result<bool>::Failure(written.Error());
    }

    return Result<bool>::Success(true);
}

// 프로세스 기본 문맥(CVar 6.2절 — ConfigContext::MakeProcessDefault)의 통로다. 기본 문맥 상태의
// 통로 SharedPtr를 가리키고, 그 문맥이 없어지기 전에 풀린다(ReleaseProcessDefault). 정하기·풀기와
// 묶음 없는 쓰기만 잡는 짧은 잠금(원자 깃발 — 상수 초기화, 전역 생성자 없음)이 지킨다.
uint32 DefaultChannelLock = 0;
const SharedPtr<WriteChannel>* DefaultChannel = nullptr;

// DefaultChannel 잠금이다(잡는 동안은 포인터 비교·참조 계수 증가뿐이다).
class DefaultChannelGuard {
public:
    DefaultChannelGuard() noexcept {
        for (;;) {
            uint32 expected = 0;
            if (platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Acquire,
                                                      platform::MemoryOrder::Relaxed>(
                    &DefaultChannelLock, expected, 1u)) {
                return;
            }

            platform::YieldThread();
        }
    }

    ~DefaultChannelGuard() noexcept {
        platform::AtomicStore<platform::MemoryOrder::Release>(&DefaultChannelLock, 0u);
    }

    DefaultChannelGuard(const DefaultChannelGuard&) = delete;
    DefaultChannelGuard& operator=(const DefaultChannelGuard&) = delete;
};

} // namespace

WriteRoute::~WriteRoute() noexcept {
    if (!viaDefault) {
        return;
    }

    // 통로 참조를 먼저 놓고(소유 문맥이 아직 쥐고 있으므로 마지막 해제가 아니다) 계수를 내린다.
    // 계수가 0이 되기 전에는 문맥 파괴가 기다리므로 raw는 살아 있다.
    WriteChannel* raw = channel.Get();
    channel = SharedPtr<WriteChannel>();
    platform::LockGuard lock(raw->mutex);
    if (--raw->defaultWriters == 0) {
        raw->defaultIdle.NotifyAll();
    }
}

void WriteTarget(WriteRoute& route) noexcept {
    const ConfigSnapshot* snapshot = CurrentBinding.snapshot;
    if (snapshot != nullptr) {
        route.channel = SnapshotAccess::Channel(*snapshot);
        return;
    }

    {
        DefaultChannelGuard guard;
        if (DefaultChannel == nullptr) {
            return;
        }

        route.channel = *DefaultChannel;
        route.viaDefault = true;
        platform::LockGuard lock(route.channel->mutex);
        ++route.channel->defaultWriters;
    }

    if (DefaultWriteProbe* probe = ActiveDefaultWriteProbe;
        probe != nullptr && probe->acquired != nullptr) {
        probe->acquired(probe->context);
    }
}

namespace {

// 거부 이유를 알린다(SetOptions::reason — 6.2절).
Result<void> Reject(const cvar::SetOptions& options, ConfigDiagnosticCode code) noexcept {
    if (options.reason != nullptr) {
        *options.reason = code;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// 쓰기 대상 (정의, 층)의 공통 검사다.
Result<void> CheckWriteTarget(const cvar::detail::VarDefinition& definition,
                              const cvar::SetOptions& options) noexcept {
    if (definition.name.IsEmpty()) {
        // 출시 빌드의 치트 변수(13.1절 — 등록되지 않는다).
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 품질 묶음 층은 묶음 펼침만 쓴다(CVar 11절).
    if (!IsConfigLayer(options.layer) || options.layer == ConfigLayer::CodeDefault ||
        options.layer == ConfigLayer::Quality) {
        return Reject(options, ConfigDiagnosticCode::LayerNotAllowed);
    }

    // 범위 값을 받지 않는 변수다(8절 — 복제 변수는 서버 값만, 묶음 변수는 Global 펼침만).
    // 플레이어별 치트 허용(`cheat.enabled`)은 예외다(13.1절).
    if (!options.scope.IsGlobal()) {
        const bool replicated =
            cvar::HasFlag(definition.flags, cvar::VarFlags::Replicated) &&
            &definition != &cvar::detail::HandleAccess::Definition(cvar::CheatsEnabled);
        if (replicated || definition.kind == cvar::HandleKind::Group) {
            return Reject(options, ConfigDiagnosticCode::ScopeNotAllowed);
        }
    }

    return Result<void>::Success();
}

// 쓰기 하나(entry 없음이면 지우기)를 만든다.
Result<void> MakeChange(Allocator& allocator, const cvar::detail::VarDefinition& definition,
                        const cvar::SetOptions& options, SharedPtr<QueuedChange>& out) noexcept {
    auto change = SharedPtr<QueuedChange>::TryMake(allocator, allocator);
    if (!change) {
        return Result<void>::Failure(change.Error());
    }

    change.Value()->layer = options.layer;
    change.Value()->scope = options.scope.value;
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

// 값 쓰기 하나를 검사하고 만든다(넣지 않는다). 층 권한(콘솔 읽기 전용·사용자 설정·출시 규칙의
// 치트)과 범위·Validate를 지금 검사한다. 적용 때 Sync가 다시 검사한다(6.2절).
Result<void> MakeSet(WriteChannel& channel, const cvar::detail::VarDefinition& definition,
                     const cvar::detail::ScalarValue& value, const cvar::SetOptions& options,
                     SourceLocation where, SharedPtr<QueuedChange>& out) noexcept {
    ConfigDiagnosticCode code = ConfigDiagnosticCode::LayerNotAllowed;
    // `Set`은 CVar<T>에만 있다(BootCVar는 Set이 없다 — 봉인 검사 대상이 아니다).
    if (!CheckConsoleLayer(channel.releaseRules, options.layer,
                           ConsoleFlagsOfDefinition(definition), code, false)
             .IsEmpty()) {
        return Reject(options, code);
    }

    // 콘솔 층 쓰기의 권한(치트 허용 상태·네트워크 역할)은 이 스레드가 보는 snapshot으로 지금 한 번
    // 보고, 적용 때 다시 본다(CVar 13절).
    if (options.layer == ConfigLayer::Console) {
        SharedPtr<const ConfigSnapshot> current;
        {
            platform::LockGuard lock(channel.mutex);
            current = channel.current;
        }

        const auto role = static_cast<NetworkRole>(
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&channel.networkRole));
        // 범위 쓰기는 그 범위의 치트 허용 상태로 본다(13.1절).
        if (!CheckConsolePermission(definition.flags,
                                    SnapshotCheatsAllowed(current.Get(), options.scope.value), role,
                                    code)
                 .IsEmpty()) {
            return Reject(options, code);
        }
    }

    // `Clamp()` 변수는 보정해 받는다. 넣는 값은 원래 값이다(적용 때 다시 보정하고 Clamped를
    // 남긴다).
    cvar::detail::ScalarValue checkedValue = value;
    StringView reason;
    switch (definition.check(definition, checkedValue, reason)) {
    case cvar::detail::ValueCheck::Accepted:
    case cvar::detail::ValueCheck::Clamped:
        break;
    case cvar::detail::ValueCheck::OutOfRange:
        return Reject(options, ConfigDiagnosticCode::ValueOutOfRange);
    case cvar::detail::ValueCheck::ValidationFailed:
        return Reject(options, ConfigDiagnosticCode::ValidationFailed);
    case cvar::detail::ValueCheck::UndefinedLevel:
        return Reject(options, ConfigDiagnosticCode::UndefinedLevel);
    }

    Allocator& allocator = channel.allocator;
    StringBuilder<64> text(allocator);
    auto formatted = FormatDefinitionValue(definition, value, text);
    if (!formatted) {
        return Result<void>::Failure(formatted.Error());
    }

    if (!formatted.Value()) {
        return Reject(options, ConfigDiagnosticCode::InvalidEnum);
    }

    StringBuilder<128> source(allocator);
    static_cast<void>(source.TryAppendFormat("{}:{}", StringView(where.file), where.line));
    if (auto status = source.Status(); !status) {
        return status;
    }

    SharedPtr<QueuedChange> change;
    if (auto made = MakeChange(allocator, definition, options, change); !made) {
        return made;
    }

    auto entry = UniquePtr<ConfigEntry>::TryMake(allocator, allocator);
    if (!entry) {
        return Result<void>::Failure(entry.Error());
    }

    ConfigEntry& added = *entry.Value();
    added.kind = EntryKind::Console;
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
    out = Move(change);
    return Result<void>::Success();
}

// 다른 스레드의 쓰기인지다(큐 한도는 이것에만 건다).
bool IsLimited(const WriteChannel& channel) noexcept {
    return platform::CurrentThreadId() != channel.owner;
}

} // namespace

void ReleaseProcessDefault(WriteChannel& channel) noexcept {
    {
        DefaultChannelGuard guard;
        if (DefaultChannel == nullptr || DefaultChannel->Get() != &channel) {
            return;
        }

        DefaultChannel = nullptr;
    }

    if (DefaultWriteProbe* probe = ActiveDefaultWriteProbe;
        probe != nullptr && probe->released != nullptr) {
        probe->released(probe->context);
    }

    // 기본 문맥을 거쳐 통로를 얻은 쓰기가 참조를 놓을 때까지 기다린다(sleep 없음 — 조건 변수).
    platform::LockGuard lock(channel.mutex);
    while (channel.defaultWriters != 0) {
        static_cast<void>(channel.defaultIdle.Wait(channel.mutex));
    }
}

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
    // 품질 묶음 층은 묶음 펼침만 쓰고 지운다(CVar 11절).
    if (state == nullptr || !IsConfigLayer(layer) || layer == ConfigLayer::CodeDefault ||
        layer == ConfigLayer::Quality) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto change = SharedPtr<detail::QueuedChange>::TryMake(*allocator_, *allocator_);
    if (!change) {
        return Result<void>::Failure(change.Error());
    }

    change.Value()->layer = layer;
    change.Value()->allTags = true;
    SharedPtr<detail::QueuedChange> one[] = {Move(change.Value())};
    return detail::Enqueue(*state->channel, Span<SharedPtr<detail::QueuedChange>>(one),
                           detail::IsLimited(*state->channel));
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

Result<void> ConfigContext::MakeProcessDefault() noexcept {
    detail::ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    detail::DefaultChannelGuard guard;
    if (detail::DefaultChannel != nullptr) {
        return detail::DefaultChannel == &state->channel
                   ? Result<void>::Success()
                   : Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    detail::DefaultChannel = &state->channel;
    return Result<void>::Success();
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

    depth_ = previous_.depth + 1;
    platform::AtomicFetchAdd<platform::MemoryOrder::AcquireRelease>(
        &SnapshotAccess::Channel(*snapshot_)->bindings, uint64{1});
    config::detail::CurrentBinding = ThreadBinding{snapshot_.Get(), nullptr, depth_};
}

SnapshotBinding::~SnapshotBinding() {
    // 깊이가 같아야 이 묶음이 가장 안쪽이다(같은 snapshot을 두 번 묶고 바깥 것부터 풀어도 잡는다).
    if (platform::CurrentThreadId() != thread_ || config::detail::CurrentBinding.depth != depth_ ||
        config::detail::CurrentBinding.snapshot != snapshot_.Get()) {
        platform::Fatal("cvar::SnapshotBinding released on another thread or out of order");
    }

    if (previousGameLane_.Get() != nullptr) {
        // 그 사이 Sync가 새 snapshot을 게시했을 수 있으므로 그 문맥의 최신 snapshot에 다시 묶는다.
        platform::LockGuard lock(previousGameLane_->mutex);
        config::detail::CurrentBinding =
            previousGameLane_->open
                ? ThreadBinding{previousGameLane_->current.Get(), previousGameLane_.Get(), 0}
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

constexpr char ScopedTagPrefix[] = "scoped.";

// 태그 버퍼에 접두와 64-bit 십진수, NUL이 들어간다.
static_assert(sizeof(ScopedTagPrefix) - 1 + rocks::detail::DecimalTextBytes + 1 <= ScopedTagBytes,
              "ScopedTagBytes must hold the scoped tag prefix, a 64-bit decimal and NUL");

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

void FatalUnsealed(const VarDefinition& definition) noexcept {
    Log::Fatal(
        config::detail::LogConfig,
        "BootCVar '{Name}' read before ConfigContext::CompleteStartup sealed the boot values",
        definition.name);
}

Result<void> SubmitSet(const VarDefinition& definition, ScalarValue value,
                       const SetOptions& options, SourceLocation where) noexcept {
    if (auto checked = config::detail::CheckWriteTarget(definition, options); !checked) {
        return checked;
    }

    config::detail::WriteRoute route;
    config::detail::WriteTarget(route);
    if (route.channel.Get() == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    WriteChannel& channel = *route.channel;
    SharedPtr<config::detail::QueuedChange> change;
    if (auto made = config::detail::MakeSet(channel, definition, value, options, where, change);
        !made) {
        return made;
    }

    SharedPtr<config::detail::QueuedChange> one[] = {Move(change)};
    return config::detail::Enqueue(channel, Span<SharedPtr<config::detail::QueuedChange>>(one),
                                   config::detail::IsLimited(channel));
}

Result<void> SubmitClear(const VarDefinition& definition, const SetOptions& options) noexcept {
    if (auto checked = config::detail::CheckWriteTarget(definition, options); !checked) {
        return checked;
    }

    config::detail::WriteRoute route;
    config::detail::WriteTarget(route);
    if (route.channel.Get() == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    WriteChannel& channel = *route.channel;
    SharedPtr<config::detail::QueuedChange> change;
    if (auto made = config::detail::MakeChange(channel.allocator, definition, options, change);
        !made) {
        return made;
    }

    SharedPtr<config::detail::QueuedChange> one[] = {Move(change)};
    return config::detail::Enqueue(channel, Span<SharedPtr<config::detail::QueuedChange>>(one),
                                   config::detail::IsLimited(channel));
}

StringView MakeScopedTag(char (&tag)[ScopedTagBytes]) noexcept {
    const uint64 number =
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&NextScopedTag, uint64{1});
    char digits[rocks::detail::DecimalTextBytes];
    const Span<const char> decimal =
        rocks::detail::FormatDecimalText(number, false, digits, sizeof(digits));
    constexpr usize PrefixBytes = sizeof(ScopedTagPrefix) - 1;
    usize length = 0;
    for (; length < PrefixBytes; ++length) {
        tag[length] = ScopedTagPrefix[length];
    }

    for (const char c : decimal) {
        tag[length++] = c;
    }

    tag[length] = '\0';
    return StringView(tag, length);
}

Result<void> BeginScopedOverride(const VarDefinition& definition, const ScalarValue& value,
                                 StringView tag, SourceLocation where,
                                 ScopedRemoval& removal) noexcept {
    SetOptions options;
    options.layer = config::ConfigLayer::Scoped;
    options.tag = tag;
    if (auto checked = config::detail::CheckWriteTarget(definition, options); !checked) {
        return checked;
    }

    // 끝낼 때 넣을 지우기를 지금 만들고 그 자리를 큐에 잡아 둔다(소멸에 실패 경로가 없다). 지우기는
    // 묶음과 무관하게 이 통로(쓴 문맥)에 간다.
    config::detail::WriteRoute route;
    config::detail::WriteTarget(route);
    if (route.channel.Get() == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 범위가 끝날 때 지우기를 넣으려고 통로 참조를 범위 동안 쥔다(6.2절 — 문맥 allocator보다 먼저
    // 끝나야 한다). route의 계수는 이 함수가 끝날 때 내린다.
    SharedPtr<WriteChannel> channel = route.channel;

    SharedPtr<config::detail::QueuedChange> set;
    SharedPtr<config::detail::QueuedChange> clear;
    if (auto made = config::detail::MakeSet(*channel, definition, value, options, where, set);
        !made) {
        return made;
    }

    if (auto made = config::detail::MakeChange(channel->allocator, definition, options, clear);
        !made) {
        return made;
    }

    if (auto reserved = config::detail::ReserveRemoval(*channel); !reserved) {
        return reserved;
    }

    SharedPtr<config::detail::QueuedChange> one[] = {Move(set)};
    if (auto queued =
            config::detail::Enqueue(*channel, Span<SharedPtr<config::detail::QueuedChange>>(one),
                                    config::detail::IsLimited(*channel));
        !queued) {
        // 잡은 자리를 돌려준다(통로가 닫혔으면 무동작이다).
        config::detail::SubmitRemoval(*channel, SharedPtr<config::detail::QueuedChange>());
        return queued;
    }

    removal.channel = Move(channel);
    removal.change = Move(clear);
    return Result<void>::Success();
}

void EndScopedOverride(ScopedRemoval& removal) noexcept {
    if (removal.channel.Get() == nullptr) {
        return;
    }

    config::detail::SubmitRemoval(*removal.channel, Move(removal.change));
    removal.channel = SharedPtr<WriteChannel>();
}

} // namespace detail

} // namespace rocks::cvar
