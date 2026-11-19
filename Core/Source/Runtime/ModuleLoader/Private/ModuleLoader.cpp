#include <ModuleLoader/ModuleLoader.h>
#include <ModuleLoader/ModuleStartContext.h>

#include <Base/NumericLimits.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks {

namespace {

using platform::AtomicFetchAdd;
using platform::AtomicFetchSub;
using platform::AtomicLoad;
using platform::AtomicStore;
using platform::MemoryOrder;

// 진입점 기호 이름을 만들 수 있는 C 식별자인지다(영문자·숫자·'_', 숫자로 시작하지 않음).
bool IsIdentifier(StringView name) noexcept {
    if (name.IsEmpty()) {
        return false;
    }

    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char value = name.Data()[index];
        const bool letter = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        const bool digit = value >= '0' && value <= '9';
        if (!letter && value != '_' && !(digit && index != 0)) {
            return false;
        }
    }

    return true;
}

// lease 계수 콜백이다. 문맥은 Record::leases의 주소다. 해제는 어느 스레드에서든 일어난다.
void RetainModuleLease(void* context) noexcept {
    AtomicFetchAdd<MemoryOrder::Relaxed>(static_cast<usize*>(context), usize{1});
}

void ReleaseModuleLease(void* context) noexcept {
    // 계수를 넘어 놓으면(0에서 빼기) lease 계약이 깨진 것이다.
    if (AtomicFetchSub<MemoryOrder::AcquireRelease>(static_cast<usize*>(context), usize{1}) == 0) {
        platform::Fatal("ModuleLease released more times than it was retained");
    }
}

// report 문자열 칸 하나에 쓴다. 넘치면 잘리고 report.truncated를 켠다.
template <usize Capacity, typename... Args>
void WriteReportText(ModuleLoadReport& report, char (&field)[Capacity], StringView format,
                     const Args&... args) noexcept {
    FixedStringBuilder text{Span<char>(field)};
    text.Clear();
    (void)text.TryAppendFormat(format, args...);
    if (text.Truncated()) {
        report.truncated = true;
    }
}

void WriteReportName(ModuleLoadReport& report, char (&field)[ModuleLoadReport::ModuleNameBytes],
                     StringView name) noexcept {
    FixedStringBuilder text{Span<char>(field)};
    text.Clear();
    (void)text.TryAppend(name);
    if (text.Truncated()) {
        report.truncated = true;
    }
}

} // namespace

const char* ModuleStateName(ModuleState state) noexcept {
    switch (state) {
    case ModuleState::NotLoaded:
        return "NotLoaded";
    case ModuleState::Starting:
        return "Starting";
    case ModuleState::Started:
        return "Started";
    case ModuleState::ShuttingDown:
        return "ShuttingDown";
    case ModuleState::Stopped:
        return "Stopped";
    case ModuleState::Failed:
        return "Failed";
    }

    return "Unknown";
}

const char* ModuleGroupName(ModuleGroup group) noexcept {
    switch (group) {
    case ModuleGroup::Process:
        return "Process";
    case ModuleGroup::Host:
        return "Host";
    case ModuleGroup::Engine:
        return "Engine";
    }

    return "Unknown";
}

Result<UniquePtr<ModuleLoader>> ModuleLoader::TryCreate(Allocator& allocator,
                                                        ModuleTable table) noexcept {
    using Created = Result<UniquePtr<ModuleLoader>>;
    auto created = UniquePtr<ModuleLoader>::TryMake(allocator, CreateTag{}, allocator, table);
    if (!created) {
        return Created::Failure(created.Error());
    }

    UniquePtr<ModuleLoader> loader = Move(created.Value());
    auto initialized = loader->Initialize(table);
    if (!initialized) {
        return Created::Failure(initialized.Error());
    }

    return Created::Success(Move(loader));
}

ModuleLoader::ModuleLoader(CreateTag, Allocator& allocator, ModuleTable table) noexcept
    : Changed(allocator), allocator_(&allocator), services_(allocator), records_(allocator),
      dependencies_(allocator), loaderThread_(platform::CurrentThreadId()) {
    (void)table;
}

Result<void> ModuleLoader::Initialize(ModuleTable table) noexcept {
    if (table.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 기록 배열은 여기서 정확한 크기로 한 번만 잡는다. 이후 자라지 않으므로 기록 주소(lease 문맥)가
    // 고정된다.
    usize dependencyTotal = 0;
    for (const ModuleTableRow& row : table) {
        dependencyTotal += row.dependencyCount;
    }

    auto reservedRecords = records_.TryReserve(table.Size());
    if (!reservedRecords) {
        return reservedRecords;
    }

    auto reservedDependencies = dependencies_.TryReserve(dependencyTotal);
    if (!reservedDependencies) {
        return reservedDependencies;
    }

    for (usize index = 0; index < table.Size(); ++index) {
        const ModuleTableRow& row = table[index];
        if (row.name == nullptr || row.entry == nullptr || !IsIdentifier(row.name) ||
            (row.dependencyCount != 0 && row.dependencies == nullptr)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 이름 중복과 의존 해석은 앞 행만 본다. 표는 의존 순서이므로 의존은 반드시 앞 행에 있다.
        if (IndexOf(row.name) != IndexNone) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const ModuleEntry& entry = row.entry();
        if (entry.entryVersion != ModuleEntryVersion || entry.name == nullptr ||
            entry.create == nullptr || StringView(entry.name) != StringView(row.name)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        Record record;
        record.row = &row;
        record.entry = &entry;
        record.dependencyStart = dependencies_.Size();
        record.dependencyCount = row.dependencyCount;
        for (usize dependency = 0; dependency < row.dependencyCount; ++dependency) {
            const char* name = row.dependencies[dependency];
            const usize resolved = name == nullptr ? IndexNone : IndexOf(name);
            if (resolved == IndexNone) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            if (!dependencies_.TryPushBack(resolved)) {
                platform::Fatal("ModuleLoader reserved dependency push failed");
            }
        }

        if (!records_.TryPushBack(Move(record))) {
            platform::Fatal("ModuleLoader reserved record push failed");
        }
    }

    return Result<void>::Success();
}

ModuleLoader::~ModuleLoader() noexcept {
    if (records_.Size() != 0 && !OnLoaderThread()) {
        platform::Fatal("ModuleLoader destroyed off the loader thread");
    }

    // 남은 모듈은 바깥 묶음까지 한 번의 종료 절차로 내린다(안쪽 묶음 모듈이 나중에 시작했으므로
    // 시작 역순이 곧 안쪽부터다). 자동 종료를 거부한 모듈은 Shutdown 없이 객체만 파괴한다(UE와
    // 같다).
    ShutdownRecords(ModuleGroup::Process);

    // 남은 객체(자동 종료를 거부한 모듈)를 시작 역순으로 파괴한다. 나중에 시작한 모듈이 먼저 시작한
    // 모듈을 쓸 수 있으므로 소멸자도 그 순서를 따른다.
    uint64 ceiling = NumericLimits<uint64>::Max();
    while (true) {
        usize next = IndexNone;
        for (usize index = 0; index < records_.Size(); ++index) {
            const Record& record = records_[index];
            if (record.owned && record.startOrder < ceiling &&
                (next == IndexNone || record.startOrder > records_[next].startOrder)) {
                next = index;
            }
        }

        if (next == IndexNone) {
            break;
        }

        Record& record = records_[next];
        ceiling = record.startOrder;
        AtomicStore<MemoryOrder::Release>(&record.object,
                                          static_cast<ModuleImplementation*>(nullptr));
        record.owned.Reset();
    }

    for (const Record& record : records_) {
        if (AtomicLoad<MemoryOrder::Acquire>(&record.leases) != 0) {
            platform::Fatal("ModuleLoader destroyed while module leases are outstanding");
        }
    }
}

bool ModuleLoader::OnLoaderThread() const noexcept {
    return platform::CurrentThreadId() == loaderThread_;
}

usize ModuleLoader::IndexOf(StringView name) const noexcept {
    // 모듈 수가 수십 개라 선형 비교로 충분하다. 기록 배열은 생성 뒤 바뀌지 않는다.
    for (usize index = 0; index < records_.Size(); ++index) {
        if (StringView(records_[index].row->name) == name) {
            return index;
        }
    }

    return IndexNone;
}

ModuleLoadReport ModuleLoader::Load(StringView name) noexcept {
    ModuleLoadReport report;
    WriteReportName(report, report.requested, name);
    if (!OnLoaderThread()) {
        report.code = ModuleLoadCode::WrongThread;
        WriteReportName(report, report.failed, name);
        WriteReportText(report, report.stage, "Load");
        WriteReportText(report, report.message,
                        "Module {} must be loaded on the thread that created the module loader",
                        name);
        return report;
    }

    if (shutdownInProgress_ || processShutdown_) {
        report.code = ModuleLoadCode::ShuttingDown;
        WriteReportName(report, report.failed, name);
        WriteReportText(report, report.stage, "Load");
        WriteReportText(report, report.message,
                        processShutdown_
                            ? "Module {} cannot be loaded after process shutdown started"
                            : "Module {} cannot be loaded while a module group is shutting down",
                        name);
        return report;
    }

    const usize index = IndexOf(name);
    if (index == IndexNone) {
        report.code = ModuleLoadCode::NotFound;
        WriteReportName(report, report.failed, name);
        WriteReportText(report, report.stage, "Lookup");
        WriteReportText(report, report.message, "Module {} is not in the module table", name);
        return report;
    }

    // Load 깊이: Startup·Changed 구독자가 부른 ShutdownGroup을 막는다(시작 중인 의존 사슬 아래에서
    // 모듈을 내리지 않게).
    ++loadDepth_;
    (void)StartRecord(index, report);
    --loadDepth_;
    return report;
}

bool ModuleLoader::StartRecord(usize index, ModuleLoadReport& report) noexcept {
    Record& record = records_[index];
    const StringView name = record.row->name;
    const ModuleState previous = record.state;
    if (previous == ModuleState::Started) {
        return true;
    }

    // 시작 중인 모듈(자기 자신이나 시작 중인 의존 사슬)을 다시 적재했다. UE는 초기화가 덜 된 객체를
    // 돌려주지만 Rocks는 거부한다(7절).
    if (previous == ModuleState::Starting) {
        report.code = ModuleLoadCode::Cycle;
        WriteReportName(report, report.failed, name);
        WriteReportText(report, report.stage, "Load");
        WriteReportText(
            report, report.message,
            "Module {} is already starting (reentrant load of a module that is still in "
            "its dependency chain or Startup)",
            name);
        return false;
    }

    // ShuttingDown은 묶음 종료 안에서만 보이며 그동안의 Load는 이미 거부했다.
    AtomicStore<MemoryOrder::Release>(&record.state, ModuleState::Starting);
    for (usize offset = 0; offset < record.dependencyCount; ++offset) {
        const usize dependency = dependencies_[record.dependencyStart + offset];
        if (!StartRecord(dependency, report)) {
            // 이 모듈은 시작하지 않았다. 상태를 되돌린다(Failed였으면 NotLoaded로 — 이번 실패는
            // 의존의 것이다). 의존의 실패는 요청 모듈 입장에서 DependencyFailed다(Cycle은 그대로
            // 둔다).
            AtomicStore<MemoryOrder::Release>(
                &record.state, previous == ModuleState::Failed ? ModuleState::NotLoaded : previous);
            if (report.code == ModuleLoadCode::StartupFailed) {
                report.code = ModuleLoadCode::DependencyFailed;
            }

            return false;
        }
    }

    auto created = record.entry->create(*allocator_);
    if (!created) {
        AtomicStore<MemoryOrder::Release>(&record.state, ModuleState::Failed);
        report.code = ModuleLoadCode::StartupFailed;
        report.error = created.Error();
        WriteReportName(report, report.failed, name);
        WriteReportText(report, report.stage, "Create");
        WriteReportText(report, report.message,
                        "Module {} lifetime object could not be created: {}", name,
                        ErrorCodeName(created.Error()));
        return false;
    }

    record.owned = Move(created.Value());
    auto lease = ModuleLease::Acquire(&record.leases, &RetainModuleLease, &ReleaseModuleLease);
    if (!lease) {
        platform::Fatal("ModuleLoader could not issue a module lease");
    }

    // Startup 중에는 로더 스레드의 Find만 이 객체를 본다(state가 Starting이다).
    AtomicStore<MemoryOrder::Release>(&record.object, record.owned.Get());
    Result<void> started = Result<void>::Success();
    {
        ModuleStartContext context(*this, record.row->phase, Move(lease.Value()), services_);
        started = record.owned->Startup(context);
    }

    if (!started) {
        // 실패한 Startup은 스스로 부분 등록을 되돌렸다(계약). Shutdown은 부르지 않는다.
        AtomicStore<MemoryOrder::Release>(&record.object,
                                          static_cast<ModuleImplementation*>(nullptr));
        record.owned.Reset();
        AtomicStore<MemoryOrder::Release>(&record.state, ModuleState::Failed);
        report.code = ModuleLoadCode::StartupFailed;
        report.error = started.Error();
        WriteReportName(report, report.failed, name);
        WriteReportText(report, report.stage, "Startup");
        WriteReportText(report, report.message, "Module {} Startup failed: {}", name,
                        ErrorCodeName(started.Error()));
        return false;
    }

    // 시작 순번은 Startup이 성공한 뒤에 매긴다(UE LoadOrder). Startup 안에서 적재한 모듈이 더 작은
    // 순번을 받으므로 역순 종료에서 이 모듈이 먼저 내려간다.
    record.group = currentGroup_;
    record.startOrder = nextStartOrder_++;
    AtomicStore<MemoryOrder::Release>(&record.state, ModuleState::Started);
    Changed.Broadcast(name, ModuleChange::Started);
    return true;
}

void ModuleLoader::ShutdownGroup(ModuleGroup group) noexcept {
    if (!OnLoaderThread()) {
        platform::Fatal("ModuleLoader::ShutdownGroup called off the loader thread");
    }

    if (loadDepth_ != 0) {
        platform::Fatal("ModuleLoader::ShutdownGroup called while a module load is in progress");
    }

    if (shutdownInProgress_) {
        platform::Fatal("ModuleLoader::ShutdownGroup called from a module shutdown callback");
    }

    if (group == ModuleGroup::Process) {
        processShutdown_ = true;
    }

    ShutdownRecords(group);
}

void ModuleLoader::ShutdownRecords(ModuleGroup group) noexcept {
    // 대상: group과 그 안쪽 묶음의 Started 모듈 중 자동 종료를 허용하는 것, 시작 순번 역순. 고르는
    // 동안 할당하지 않도록 순번이 큰 것부터 하나씩 찾는다(모듈 수가 작다).
    shutdownInProgress_ = true;
    const auto selected = [group](const Record& record) noexcept {
        return record.state == ModuleState::Started && record.group >= group &&
               record.owned->SupportsAutomaticShutdown();
    };

    // 1단계: 전체 PreUnload를 시작 역순으로 먼저 부른다. 2단계 대상은 pendingShutdown으로 표시하고
    // 상태는 Started로 둔다(UE는 UnloadModule 직전에만 bIsReady를 끈다). 그래서 PreUnload 동안 대상
    // 모듈이 서로 Find로 보인다. 이 동안의 새 Load는 shutdownInProgress_가 막는다.
    uint64 ceiling = NumericLimits<uint64>::Max();
    while (true) {
        usize next = IndexNone;
        for (usize index = 0; index < records_.Size(); ++index) {
            const Record& record = records_[index];
            if (selected(record) && record.startOrder < ceiling &&
                (next == IndexNone || record.startOrder > records_[next].startOrder)) {
                next = index;
            }
        }

        if (next == IndexNone) {
            break;
        }

        Record& record = records_[next];
        ceiling = record.startOrder;
        record.pendingShutdown = true;
        record.owned->PreUnload();
    }

    // 2단계: 각 Shutdown을 같은 역순으로 부르고 객체를 파괴한다. 모듈은 자기 Shutdown 직전에만
    // ShuttingDown이 되므로 아직 내리지 않은 의존 모듈은 그 Shutdown 동안 보인다. 공유 라이브러리는
    // 내리지 않는다.
    ceiling = NumericLimits<uint64>::Max();
    while (true) {
        usize next = IndexNone;
        for (usize index = 0; index < records_.Size(); ++index) {
            const Record& record = records_[index];
            if (record.pendingShutdown && record.startOrder < ceiling &&
                (next == IndexNone || record.startOrder > records_[next].startOrder)) {
                next = index;
            }
        }

        if (next == IndexNone) {
            break;
        }

        Record& record = records_[next];
        ceiling = record.startOrder;
        record.pendingShutdown = false;
        AtomicStore<MemoryOrder::Release>(&record.state, ModuleState::ShuttingDown);
        record.owned->Shutdown();
        AtomicStore<MemoryOrder::Release>(&record.object,
                                          static_cast<ModuleImplementation*>(nullptr));
        record.owned.Reset();
        record.startOrder = 0;
        AtomicStore<MemoryOrder::Release>(&record.state, ModuleState::Stopped);
    }

    shutdownInProgress_ = false;
}

bool ModuleLoader::IsStarted(StringView name) const noexcept {
    const usize index = IndexOf(name);
    return index != IndexNone &&
           AtomicLoad<MemoryOrder::Acquire>(&records_[index].state) == ModuleState::Started;
}

Optional<ModuleState> ModuleLoader::State(StringView name) const noexcept {
    const usize index = IndexOf(name);
    if (index == IndexNone) {
        return {};
    }

    return AtomicLoad<MemoryOrder::Acquire>(&records_[index].state);
}

ModuleImplementation* ModuleLoader::Find(StringView name) const noexcept {
    const usize index = IndexOf(name);
    if (index == IndexNone) {
        return nullptr;
    }

    const Record& record = records_[index];
    const ModuleState state = AtomicLoad<MemoryOrder::Acquire>(&record.state);
    const bool visible =
        state == ModuleState::Started || (state == ModuleState::Starting && OnLoaderThread());
    return visible ? AtomicLoad<MemoryOrder::Acquire>(&record.object) : nullptr;
}

usize ModuleLoader::LeaseCount(StringView name) const noexcept {
    const usize index = IndexOf(name);
    return index == IndexNone ? 0 : AtomicLoad<MemoryOrder::Acquire>(&records_[index].leases);
}

usize ModuleLoader::ModuleCount() const noexcept {
    return records_.Size();
}

} // namespace rocks
