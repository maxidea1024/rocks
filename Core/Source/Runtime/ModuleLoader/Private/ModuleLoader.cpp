#include <ModuleLoader/ModuleLoader.h>
#include <ModuleLoader/ModuleStartContext.h>

#include "ModuleLoaderInternal.h"
#include "ModuleManifest.h"

#include <Base/NumericLimits.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Fatal.h>

namespace rocks {

namespace {

using platform::AtomicFetchAdd;
using platform::AtomicFetchSub;
using platform::AtomicLoad;
using platform::AtomicStore;
using platform::MemoryOrder;

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

using detail::WriteReportName;
using detail::WriteReportText;

// manifest 단계 이름이다(report.stage).
constexpr StringView ManifestStage = "Manifest";

// Windows 절대 경로(드라이브 "X:\\"·"X:/", UNC "\\\\server")나 POSIX 절대 경로("/…")인지다
// (DynamicLibrary::Open과 같은 규칙).
bool IsAbsolutePath(StringView path) noexcept {
    const char* text = path.Data();
    const usize bytes = path.ByteCount();
    const bool drive = bytes >= 3 &&
                       ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')) &&
                       text[1] == ':' && (text[2] == '\\' || text[2] == '/');
    const bool unc = bytes >= 2 && text[0] == '\\' && text[1] == '\\';
    const bool posix = bytes >= 1 && text[0] == '/';
    return drive || unc || posix;
}

// 파일 전체를 읽는다. 실패하면 FileStatus 실패이고 text는 비어 있을 수 있다(크기를 읽지 못함·너무
// 큼은 Overflow다). 할당 실패는 outOfMemory를 켠다.
platform::FileStatus ReadWholeFile(StringView path, Array<char>& text, bool& outOfMemory) noexcept {
    outOfMemory = false;
    StringBuilder<> terminated(text.GetAllocator());
    if (!terminated.TryAppend(path)) {
        outOfMemory = true;
        return platform::FileStatus{platform::FileCode::OutOfMemory, 0};
    }

    platform::File file;
    const platform::FileStatus opened =
        file.Open(terminated.CString(), platform::FileOpenMode::ReadExisting);
    if (!opened) {
        return opened;
    }

    uint64 bytes = 0;
    const platform::FileStatus sized = file.Size(bytes);
    if (!sized || bytes > NumericLimits<usize>::Max()) {
        return sized ? platform::FileStatus{platform::FileCode::Overflow, 0} : sized;
    }

    if (!text.TryResize(static_cast<usize>(bytes))) {
        outOfMemory = true;
        return platform::FileStatus{platform::FileCode::OutOfMemory, 0};
    }

    return file.ReadExact(text.Data(), text.Size()).status;
}

// 플러그인 manifest 하나의 본문과 폴더다(3.15절 — 실행 파일 manifest 폴더의 부모 + "Plugins/…").
struct PluginManifestFile {
    explicit PluginManifestFile(Allocator& allocator) noexcept
        : text(allocator), folder(allocator) {
    }

    Array<char> text;
    StringBuilder<> folder;
};

// 단계 값의 순서 번호다(LoadingPhase 값 순서가 단계 순서다).
constexpr uint8 PhaseOrder(LoadingPhase phase) noexcept {
    return static_cast<uint8>(phase);
}

// 묶음의 첫 단계다. 묶음을 내리면 그 단계부터 다시 돈다.
constexpr LoadingPhase FirstPhaseOf(ModuleGroup group) noexcept {
    switch (group) {
    case ModuleGroup::Process:
        return LoadingPhase::EarliestPossible;
    case ModuleGroup::Host:
        return LoadingPhase::PostConfigInit;
    case ModuleGroup::Engine:
        return LoadingPhase::PreDefault;
    }

    return LoadingPhase::EarliestPossible;
}

// 묶음의 바깥 묶음이다. 프로세스 묶음은 그대로다.
constexpr ModuleGroup OuterGroupOf(ModuleGroup group) noexcept {
    return group == ModuleGroup::Engine ? ModuleGroup::Host : ModuleGroup::Process;
}

// 등록기 자리 하나의 비트다.
constexpr uint32 SlotBit(usize slot) noexcept {
    return uint32{1} << slot;
}

static_assert(MaxTypeRegistrars <= sizeof(uint32) * 8,
              "Record::registeredSlots must hold one bit per type registrar slot");

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

Result<UniquePtr<ModuleLoader>>
ModuleLoader::TryCreateFromManifest(Allocator& allocator, StringView manifestPath,
                                    ModuleLoadReport& report) noexcept {
    using Created = Result<UniquePtr<ModuleLoader>>;
    report = ModuleLoadReport{};
    auto failed = [&report]() noexcept {
        ErrorCode code = ErrorCode::InvalidArgument;
        if (report.error == ErrorCode::OutOfMemory) {
            code = ErrorCode::OutOfMemory;
        } else if (report.code == ModuleLoadCode::NotFound ||
                   report.code == ModuleLoadCode::EntryMissing) {
            code = ErrorCode::NotFound;
        } else if (report.code == ModuleLoadCode::OsLoadFailed) {
            code = ErrorCode::DependencyFailed;
        }

        return Created::Failure(code);
    };
    auto reject = [&](ModuleLoadCode code, uint32 nativeError, StringView message) noexcept {
        report.code = code;
        report.nativeError = nativeError;
        WriteReportText(report, report.stage, ManifestStage);
        WriteReportText(report, report.message, message, manifestPath);
        return failed();
    };

    // requested는 manifest 파일 이름이다(경로 전체는 문구에 담는다 — 이름 칸이 짧다). manifest
    // 폴더는 마지막 경로 구분자 앞이다.
    usize separator = IndexNone;
    for (usize index = 0; index < manifestPath.ByteCount(); ++index) {
        if (manifestPath.Data()[index] == '/' || manifestPath.Data()[index] == '\\') {
            separator = index;
        }
    }

    WriteReportName(report, report.requested,
                    separator == IndexNone ? manifestPath
                                           : StringView(manifestPath.Data() + separator + 1,
                                                        manifestPath.ByteCount() - separator - 1));
    if (!IsAbsolutePath(manifestPath) || separator == IndexNone) {
        return reject(ModuleLoadCode::Incompatible, 0,
                      "Module manifest path {} is not an absolute file path");
    }

    // POSIX 루트 바로 아래 파일("/x.modules.json")은 폴더가 "/"다.
    const StringView directory(manifestPath.Data(), separator == 0 ? 1 : separator);
    const StringView fileName(manifestPath.Data() + separator + 1,
                              manifestPath.ByteCount() - separator - 1);
    // 파일 하나를 읽는다. 실패하면 report를 채운다(path는 문구, 없으면 NotFound).
    auto read = [&](StringView path, Array<char>& text) noexcept {
        bool outOfMemory = false;
        const platform::FileStatus status = ReadWholeFile(path, text, outOfMemory);
        if (status) {
            return true;
        }

        report.code = outOfMemory || status.code != platform::FileCode::NotFound
                          ? ModuleLoadCode::Incompatible
                          : ModuleLoadCode::NotFound;
        report.nativeError = status.nativeError;
        report.error = outOfMemory ? ErrorCode::OutOfMemory : report.error;
        WriteReportText(report, report.stage, ManifestStage);
        WriteReportText(report, report.message,
                        outOfMemory
                            ? StringView("Out of memory while reading the module manifest {}")
                            : StringView("Module manifest {} cannot be read"),
                        path);
        return false;
    };

    Array<char> text(allocator);
    if (!read(manifestPath, text)) {
        return failed();
    }

    // 실행 파일 manifest가 가리키는 플러그인 manifest를 읽는다(3.15절 — 목록의 manifest만, 폴더는
    // 실행 파일 manifest 폴더의 부모 + "Plugins/…").
    detail::ModuleManifestPlugins plugins(allocator);
    if (!detail::ListModuleManifestPlugins(StringView(text.Data(), text.Size()), fileName, plugins,
                                           report)) {
        return failed();
    }

    // 부모 폴더는 마지막 구분자(연속 구분자는 첫 것) 앞이다. POSIX 루트 바로 아래("/bin")는 빈
    // 부모("/Plugins/…")이고, 드라이브("C:")나 UNC 서버("\\srv")까지 올라가면 부모가 없다.
    auto isSeparator = [](char value) noexcept { return value == '/' || value == '\\'; };
    usize parent = IndexNone;
    for (usize index = 0; index < directory.ByteCount(); ++index) {
        if (isSeparator(directory.Data()[index])) {
            parent = index;
        }
    }

    while (parent != IndexNone && parent != 0 && isSeparator(directory.Data()[parent - 1])) {
        --parent;
    }

    const bool unc =
        directory.ByteCount() >= 2 && directory.Data()[0] == '\\' && directory.Data()[1] == '\\';
    bool uncServerOnly = unc;
    for (usize index = 2; unc && parent != IndexNone && index < parent; ++index) {
        uncServerOnly = uncServerOnly && !isSeparator(directory.Data()[index]);
    }

    if (plugins.Count() != 0 && (parent == IndexNone || uncServerOnly)) {
        return reject(ModuleLoadCode::Incompatible, 0,
                      "Module manifest {} lists plugin manifests but its folder has no parent");
    }

    const usize count = plugins.Count() + 1;
    Array<UniquePtr<PluginManifestFile>> files(allocator);
    Array<detail::ModuleManifestText> manifests(allocator);
    if (!files.TryReserve(plugins.Count()) || !manifests.TryReserve(count) ||
        !manifests.TryPushBack({StringView(text.Data(), text.Size()), directory, StringView()})) {
        report.error = ErrorCode::OutOfMemory;
        return reject(ModuleLoadCode::Incompatible, 0,
                      "Out of memory while reading the module manifest {}");
    }

    for (usize index = 0; index < plugins.Count(); ++index) {
        // "../Plugins/…/<파일>"의 첫 "../"를 실행 파일 manifest 폴더의 부모로 바꾼다.
        const StringView relative = plugins.Path(index);
        const StringView inside(relative.Data() + 3, relative.ByteCount() - 3);
        usize fileStart = inside.ByteCount();
        while (fileStart != 0 && inside.Data()[fileStart - 1] != '/') {
            --fileStart;
        }

        auto made = UniquePtr<PluginManifestFile>::TryMake(allocator, allocator);
        StringBuilder<> path(allocator);
        if (!made || !files.TryPushBack(Move(made.Value())) ||
            !path.TryAppend(StringView(directory.Data(), parent)) || !path.TryAppend("/") ||
            !path.TryAppend(inside) ||
            !files[index]->folder.TryAppend(StringView(directory.Data(), parent)) ||
            !files[index]->folder.TryAppend("/") ||
            !files[index]->folder.TryAppend(StringView(inside.Data(), fileStart - 1))) {
            report.error = ErrorCode::OutOfMemory;
            return reject(ModuleLoadCode::Incompatible, 0,
                          "Out of memory while reading the module manifest {}");
        }

        if (!read(path.View(), files[index]->text)) {
            WriteReportName(report, report.requested, plugins.Name(index));
            return failed();
        }

        const Array<char>& body = files[index]->text;
        if (!manifests.TryPushBack({StringView(body.Data(), body.Size()),
                                    files[index]->folder.View(), plugins.Name(index)})) {
            platform::Fatal("ModuleLoader reserved manifest push failed");
        }
    }

    auto data = UniquePtr<detail::ModuleManifestData>::TryMake(allocator, allocator);
    if (!data) {
        report.error = ErrorCode::OutOfMemory;
        return reject(ModuleLoadCode::Incompatible, 0,
                      "Out of memory while reading the module manifest {}");
    }

    if (!detail::ParseModuleManifests(
            Span<const detail::ModuleManifestText>(manifests.Data(), manifests.Size()),
            *data.Value(), report) ||
        !detail::PrepareModuleBinaries(*data.Value(), report)) {
        return failed();
    }

    auto created = UniquePtr<ModuleLoader>::TryMake(
        allocator, CreateTag{}, allocator,
        ModuleTable(data.Value()->rows.Data(), data.Value()->rows.Size()));
    if (!created) {
        report.error = created.Error();
        return reject(ModuleLoadCode::Incompatible, 0,
                      "Out of memory while creating the module loader for {}");
    }

    UniquePtr<ModuleLoader> loader = Move(created.Value());
    loader->manifest_ = Move(data.Value());
    const detail::ModuleManifestData& stored = *loader->manifest_;
    auto initialized = loader->Initialize(
        ModuleTable(stored.rows.Data(), stored.rows.Size()),
        Span<const ModuleEntry* const>(stored.entries.Data(), stored.entries.Size()));
    if (!initialized) {
        report.error = initialized.Error();
        (void)reject(ModuleLoadCode::Incompatible, 0,
                     "Module manifest {} does not form a valid module table");
        return Created::Failure(initialized.Error());
    }

    return Created::Success(Move(loader));
}

bool ModuleLoader::FindHostSymbol(const char* symbol, void*& address,
                                  platform::DynamicLibraryLease& lease) const noexcept {
    address = nullptr;
    if (!manifest_) {
        return false;
    }

    for (usize index = 0; index < manifest_->host.Size(); ++index) {
        if (manifest_->host[index]) {
            return static_cast<bool>(manifest_->libraries[index].Find(symbol, address, lease));
        }
    }

    return false;
}

const ModuleEntry& ModuleLoader::EntryAt(usize index) const noexcept {
    if (index >= records_.Size()) {
        platform::Fatal("ModuleLoader::EntryAt index out of range");
    }

    return *records_[index].entry;
}

ModuleLoader::ModuleLoader(CreateTag, Allocator& allocator, ModuleTable table) noexcept
    : Changed(allocator), allocator_(&allocator), services_(allocator), records_(allocator),
      dependencies_(allocator), loaderThread_(platform::CurrentThreadId()) {
    (void)table;
}

Result<void> ModuleLoader::Initialize(ModuleTable table,
                                      Span<const ModuleEntry* const> resolved) noexcept {
    if (!resolved.IsEmpty() && resolved.Size() != table.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (table.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    table_ = table;

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
        const bool hasEntry =
            resolved.IsEmpty() ? row.entry != nullptr : resolved[index] != nullptr;
        if (row.name == nullptr || !hasEntry || !detail::IsModuleIdentifier(row.name) ||
            (row.dependencyCount != 0 && row.dependencies == nullptr)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 이름 중복과 의존 해석은 앞 행만 본다. 표는 의존 순서이므로 의존은 반드시 앞 행에 있다.
        if (IndexOf(row.name) != IndexNone) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const ModuleEntry& entry = resolved.IsEmpty() ? row.entry() : *resolved[index];
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

    // Engine이 로더보다 먼저 끝나야 한다(엔진 묶음 사용자가 남은 채 로더를 파괴하면 Engine이
    // 파괴된 로더를 부른다).
    if (engineUsers_ != 0) {
        platform::Fatal("ModuleLoader destroyed while engine group users remain");
    }

    // 남은 모듈은 바깥 묶음까지 한 번의 종료 절차로 내린다(안쪽 묶음 모듈이 나중에 시작했으므로
    // 시작 역순이 곧 안쪽부터다). 자동 종료를 거부한 모듈은 Shutdown 없이 객체만 파괴한다(UE와
    // 같다).
    ShutdownRecords(ModuleGroup::Process);
    // 자동 종료를 거부해 남은 모듈의 타입 등록도 되돌린다(등록기는 아직 살아 있어야 한다 — 계약).
    for (usize slot = 0; slot < MaxTypeRegistrars; ++slot) {
        if (registrars_[slot] != nullptr) {
            UnregisterSlotTypes(slot);
            registrars_[slot] = nullptr;
        }
    }

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

    // manifest 로더는 모든 수명 객체를 파괴한 뒤에 공유 라이브러리를 적재 역순으로 닫는다(3.7절).
    // 구독이 남아 있으면 바인딩 함수표가 닫을 라이브러리 안에 있을 수 있으므로 먼저 비운다.
    Changed.Clear();
    manifest_.Reset();
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

bool ModuleLoader::RejectStart(ModuleLoadReport& report, StringView requested,
                               bool phase) const noexcept {
    // requested는 모듈 이름(Load)이나 단계 이름(StartPhase·StartEngineGroup)이다.
    if (!OnLoaderThread()) {
        report.code = ModuleLoadCode::WrongThread;
        WriteReportName(report, report.failed, requested);
        WriteReportText(report, report.stage, phase ? "StartPhase" : "Load");
        WriteReportText(report, report.message,
                        phase ? "Loading phase {} must be started on the thread that created the "
                                "module loader"
                              : "Module {} must be loaded on the thread that created the module "
                                "loader",
                        requested);
        return true;
    }

    if (shutdownInProgress_ || processShutdown_) {
        report.code = ModuleLoadCode::ShuttingDown;
        WriteReportName(report, report.failed, requested);
        WriteReportText(report, report.stage, phase ? "StartPhase" : "Load");
        const char* message = nullptr;
        if (phase) {
            message = processShutdown_
                          ? "Loading phase {} cannot be started after process shutdown started"
                          : "Loading phase {} cannot be started while a module group is shutting "
                            "down";
        } else {
            message = processShutdown_
                          ? "Module {} cannot be loaded after process shutdown started"
                          : "Module {} cannot be loaded while a module group is shutting down";
        }

        WriteReportText(report, report.message, message, requested);
        return true;
    }

    return false;
}

ModuleLoadReport ModuleLoader::Load(StringView name) noexcept {
    ModuleLoadReport report;
    WriteReportName(report, report.requested, name);
    if (RejectStart(report, name, false)) {
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

    // 타입 등록기가 붙어 있으면 Startup 직전에 이 모듈의 타입을 모든 등록기에 등록한다(UE도
    // StartupModule 전에 UObject를 등록한다). 실패하면 Startup·Shutdown을 부르지 않는다.
    if (!RegisterRecordTypesAll(index, report)) {
        record.owned.Reset();
        AtomicStore<MemoryOrder::Release>(&record.state, ModuleState::Failed);
        return false;
    }

    // Startup 중에는 로더 스레드의 Find만 이 객체를 본다(state가 Starting이다).
    AtomicStore<MemoryOrder::Release>(&record.object, record.owned.Get());
    Result<void> started = Result<void>::Success();
    {
        ModuleStartContext context(*this, record.row->phase, Move(lease.Value()), services_);
        started = record.owned->Startup(context);
    }

    if (!started) {
        // 실패한 Startup은 스스로 부분 등록을 되돌렸다(계약). Shutdown은 부르지 않는다. 로더가 한
        // 타입 등록은 로더가 되돌린다.
        UnregisterRecordTypesAll(index);
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

void ModuleLoader::CheckShutdownCall(bool engineGroup) const noexcept {
    if (!OnLoaderThread()) {
        platform::Fatal(engineGroup
                            ? "ModuleLoader::ShutdownEngineGroup called off the loader thread"
                            : "ModuleLoader::ShutdownGroup called off the loader thread");
    }

    if (loadDepth_ != 0) {
        platform::Fatal(
            engineGroup
                ? "ModuleLoader::ShutdownEngineGroup called while a module load is in progress"
                : "ModuleLoader::ShutdownGroup called while a module load is in progress");
    }

    if (shutdownInProgress_) {
        platform::Fatal(
            engineGroup ? "ModuleLoader::ShutdownEngineGroup called from a module shutdown callback"
                        : "ModuleLoader::ShutdownGroup called from a module shutdown callback");
    }
}

void ModuleLoader::ShutdownGroup(ModuleGroup group) noexcept {
    CheckShutdownCall(false);
    // 엔진 묶음 사용자(Engine)가 남아 있는데 묶음을 내리면 그 Engine의 모듈이 사라진다.
    if (engineUsers_ != 0) {
        platform::Fatal("ModuleLoader::ShutdownGroup called while engine group users remain");
    }

    if (group == ModuleGroup::Process) {
        processShutdown_ = true;
    }

    ShutdownRecords(group);
}

ModuleLoadReport ModuleLoader::StartPhase(LoadingPhase phase) noexcept {
    ModuleLoadReport report;
    const StringView phaseName = LoadingPhaseName(phase);
    WriteReportName(report, report.requested, phaseName);
    if (RejectStart(report, phaseName, true)) {
        return report;
    }

    if (phase == LoadingPhase::None) {
        platform::Fatal("ModuleLoader::StartPhase called with LoadingPhase::None");
    }

    if (loadDepth_ != 0) {
        platform::Fatal("ModuleLoader::StartPhase called while a module load is in progress");
    }

    // 이미 돈 단계다. 같은 묶음 안이면 할 일이 없고(UE도 같은 단계 재호출을 허용한다), 앞 묶음의
    // 단계면 호스트의 순서 오류다.
    if (PhaseOrder(phase) < PhaseOrder(nextPhase_)) {
        if (LoadingPhaseGroup(phase) < currentGroup_) {
            platform::Fatal("ModuleLoader::StartPhase called for a phase of a group before the "
                            "current one (host start order error)");
        }

        return report;
    }

    ++loadDepth_;
    bool started = true;
    while (started && PhaseOrder(nextPhase_) <= PhaseOrder(phase)) {
        started = RunPhase(nextPhase_, report);
        if (started) {
            nextPhase_ = static_cast<LoadingPhase>(PhaseOrder(nextPhase_) + 1);
        } else {
            // 실패한 단계를 알린다(부른 단계보다 앞 단계일 수 있다).
            WriteReportName(report, report.requested, LoadingPhaseName(nextPhase_));
        }
    }

    --loadDepth_;
    return report;
}

bool ModuleLoader::RunPhase(LoadingPhase phase, ModuleLoadReport& report) noexcept {
    // 이 단계에서 시작하는 모듈은 이 단계의 묶음에 속한다.
    currentGroup_ = LoadingPhaseGroup(phase);
    for (usize index = 0; index < records_.Size(); ++index) {
        const ModuleTableRow& row = *records_[index].row;
        if (row.phase != phase || row.onRequest) {
            continue;
        }

        if (!StartRecord(index, report)) {
            return false;
        }
    }

    return true;
}

bool ModuleLoader::PhaseStarted(LoadingPhase phase) const noexcept {
    return phase != LoadingPhase::None && PhaseOrder(phase) < PhaseOrder(nextPhase_);
}

ModuleLoadReport ModuleLoader::StartEngineGroup() noexcept {
    ModuleLoadReport report;
    WriteReportName(report, report.requested, LoadingPhaseName(LoadingPhase::PostDefault));
    if (RejectStart(report, LoadingPhaseName(LoadingPhase::PostDefault), true)) {
        return report;
    }

    // 첫 사용자가 아니어도 StartPhase와 같은 계약을 지킨다(모듈 Startup 안에서 Engine을 만들지
    // 않는다).
    if (loadDepth_ != 0) {
        platform::Fatal("ModuleLoader::StartEngineGroup called while a module load is in progress");
    }

    if (engineUsers_ != 0) {
        ++engineUsers_;
        return report;
    }

    report = StartPhase(LoadingPhase::PostDefault);
    if (!report.IsOk()) {
        // 처음 사용자의 시작이 실패했다. 이 묶음에서 시작한 모듈을 내려 다음 시도가 처음부터 하게
        // 한다(사용자 수는 0 그대로).
        ShutdownRecords(ModuleGroup::Engine);
        return report;
    }

    engineUsers_ = 1;
    return report;
}

void ModuleLoader::ShutdownEngineGroup() noexcept {
    CheckShutdownCall(true);
    if (engineUsers_ == 0) {
        platform::Fatal("ModuleLoader::ShutdownEngineGroup called without an engine group user");
    }

    --engineUsers_;
    if (engineUsers_ == 0) {
        ShutdownRecords(ModuleGroup::Engine);
    }
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
        // 모듈 코드가 내려간 뒤 그 모듈의 타입을 모든 등록기에서 되돌린다(3.7절 3단계).
        UnregisterRecordTypesAll(next);
        AtomicStore<MemoryOrder::Release>(&record.object,
                                          static_cast<ModuleImplementation*>(nullptr));
        record.owned.Reset();
        record.startOrder = 0;
        AtomicStore<MemoryOrder::Release>(&record.state, ModuleState::Stopped);
    }

    // 내린 묶음과 안쪽 묶음의 단계는 다시 돌 수 있다(다음 Engine). 지금 묶음은 바깥 묶음이다.
    const LoadingPhase first = FirstPhaseOf(group);
    if (PhaseOrder(first) < PhaseOrder(nextPhase_)) {
        nextPhase_ = first;
    }

    if (currentGroup_ >= group) {
        currentGroup_ = OuterGroupOf(group);
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

Result<ModuleLease> ModuleLoader::AcquireLease(StringView name) noexcept {
    const usize index = IndexOf(name);
    if (index == IndexNone) {
        return Result<ModuleLease>::Failure(ErrorCode::NotFound);
    }

    return ModuleLease::Acquire(&records_[index].leases, &RetainModuleLease, &ReleaseModuleLease);
}

usize ModuleLoader::ModulesWithLeases(Span<StringView> names) const noexcept {
    usize count = 0;
    for (const Record& record : records_) {
        if (AtomicLoad<MemoryOrder::Acquire>(&record.leases) == 0) {
            continue;
        }

        if (count < names.Size()) {
            names[count] = record.row->name;
        }

        ++count;
    }

    return count;
}

void ModuleLoader::CheckRegistrarCall() const noexcept {
    // 시작 중인 의존 사슬이나 종료 절차 아래에서 등록기를 바꾸면 모듈마다 등록 여부가 갈린다.
    if (loadDepth_ != 0 || shutdownInProgress_) {
        platform::Fatal("ModuleLoader type registrar changed from a module callback");
    }
}

ModuleLoadReport ModuleLoader::AttachTypeRegistrar(ModuleTypeRegistrar& registrar) noexcept {
    ModuleLoadReport report;
    if (!OnLoaderThread()) {
        report.code = ModuleLoadCode::WrongThread;
        WriteReportText(report, report.stage, "AttachTypeRegistrar");
        WriteReportText(report, report.message,
                        "A type registrar must be attached on the thread that created the module "
                        "loader");
        return report;
    }

    if (SlotOf(registrar) != IndexNone) {
        platform::Fatal("ModuleLoader type registrar is already attached");
    }

    CheckRegistrarCall();
    usize slot = IndexNone;
    for (usize candidate = 0; candidate < MaxTypeRegistrars; ++candidate) {
        if (registrars_[candidate] == nullptr) {
            slot = candidate;
            break;
        }
    }

    if (slot == IndexNone) {
        platform::Fatal("ModuleLoader has no free type registrar slot (MaxTypeRegistrars)");
    }

    registrars_[slot] = &registrar;
    // 이미 시작한 모듈을 시작 순서(순번 오름차순)로 등록한다. 고르는 동안 할당하지 않도록 순번이
    // 작은 것부터 하나씩 찾는다(모듈 수가 작다).
    uint64 floor = 0;
    while (true) {
        usize next = IndexNone;
        for (usize index = 0; index < records_.Size(); ++index) {
            const Record& record = records_[index];
            if (record.state == ModuleState::Started && record.startOrder > floor &&
                (next == IndexNone || record.startOrder < records_[next].startOrder)) {
                next = index;
            }
        }

        if (next == IndexNone) {
            break;
        }

        floor = records_[next].startOrder;
        if (!RegisterRecordTypes(next, slot, report)) {
            // 이번에 등록한 것을 시작 역순으로 되돌리고 붙이지 않는다.
            UnregisterSlotTypes(slot);
            registrars_[slot] = nullptr;
            return report;
        }
    }

    return report;
}

void ModuleLoader::DetachTypeRegistrar(ModuleTypeRegistrar& registrar) noexcept {
    if (!OnLoaderThread()) {
        platform::Fatal("ModuleLoader::DetachTypeRegistrar called off the loader thread");
    }

    const usize slot = SlotOf(registrar);
    if (slot == IndexNone) {
        return;
    }

    CheckRegistrarCall();
    UnregisterSlotTypes(slot);
    registrars_[slot] = nullptr;
}

usize ModuleLoader::TypeRegistrarCount() const noexcept {
    usize count = 0;
    for (const ModuleTypeRegistrar* registrar : registrars_) {
        if (registrar != nullptr) {
            ++count;
        }
    }

    return count;
}

usize ModuleLoader::SlotOf(const ModuleTypeRegistrar& registrar) const noexcept {
    for (usize slot = 0; slot < MaxTypeRegistrars; ++slot) {
        if (registrars_[slot] == &registrar) {
            return slot;
        }
    }

    return IndexNone;
}

bool ModuleLoader::RegisterRecordTypesAll(usize index, ModuleLoadReport& report) noexcept {
    for (usize slot = 0; slot < MaxTypeRegistrars; ++slot) {
        if (registrars_[slot] != nullptr && !RegisterRecordTypes(index, slot, report)) {
            // 이 모듈을 이미 등록한 다른 등록기를 되돌린다(Startup은 부르지 않는다).
            UnregisterRecordTypesAll(index);
            return false;
        }
    }

    return true;
}

bool ModuleLoader::RegisterRecordTypes(usize index, usize slot, ModuleLoadReport& report) noexcept {
    Record& record = records_[index];
    if (record.entry->registerTypes == nullptr) {
        return true;
    }

    const StringView name = record.row->name;
    auto lease = ModuleLease::Acquire(&record.leases, &RetainModuleLease, &ReleaseModuleLease);
    if (!lease) {
        platform::Fatal("ModuleLoader could not issue a module lease");
    }

    auto registered = registrars_[slot]->Register(name, record.entry->registerTypes, lease.Value());
    if (!registered) {
        report.code = ModuleLoadCode::StartupFailed;
        report.error = registered.Error();
        WriteReportName(report, report.failed, name);
        WriteReportText(report, report.stage, "RegisterTypes");
        WriteReportText(report, report.message,
                        "Module {} reflection types could not be registered: {}", name,
                        ErrorCodeName(registered.Error()));
        return false;
    }

    record.registeredSlots |= SlotBit(slot);
    return true;
}

void ModuleLoader::UnregisterRecordTypes(usize index, usize slot) noexcept {
    Record& record = records_[index];
    if ((record.registeredSlots & SlotBit(slot)) == 0) {
        return;
    }

    record.registeredSlots &= ~SlotBit(slot);
    registrars_[slot]->Unregister(record.row->name);
}

void ModuleLoader::UnregisterRecordTypesAll(usize index) noexcept {
    // 붙인 역순(자리 번호 역순)으로 되돌린다.
    for (usize slot = MaxTypeRegistrars; slot-- > 0;) {
        UnregisterRecordTypes(index, slot);
    }
}

void ModuleLoader::UnregisterSlotTypes(usize slot) noexcept {
    // 시작 역순이다. 등록한 모듈은 모두 시작해 있다(Startup 실패는 그 자리에서 되돌렸다).
    uint64 ceiling = NumericLimits<uint64>::Max();
    while (true) {
        usize next = IndexNone;
        for (usize index = 0; index < records_.Size(); ++index) {
            const Record& record = records_[index];
            if ((record.registeredSlots & SlotBit(slot)) != 0 && record.startOrder < ceiling &&
                (next == IndexNone || record.startOrder > records_[next].startOrder)) {
                next = index;
            }
        }

        if (next == IndexNone) {
            break;
        }

        ceiling = records_[next].startOrder;
        UnregisterRecordTypes(next, slot);
    }
}

} // namespace rocks
