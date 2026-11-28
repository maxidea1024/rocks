#include <DevelopmentShaderProvider/DevelopmentShaderProvider.h>

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/InitializerList.h>
#include <Base/PlacementNew.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/FileError.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Engine/CountedPendingWork.h>
#include <HostConfig/HostSettings.h>
#include <Localization/Text.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/DirectoryWatch.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Progress/ProgressCounter.h>
#include <Progress/ProgressHub.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderDiagnostics.h>
#include <ShaderCompiler/ShaderJobKey.h>
#include <ShaderCompiler/ShaderLibraryCook.h>
#include <ShaderCompiler/ShaderLibraryReloader.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCompiler/ShaderSourceTree.h>
#include <ShaderCompiler/ShaderWatchSettle.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCore/ShaderVirtualPath.h>
#include <Task/WorkerScheduler.h>
#include <stdio.h>
#include <string.h>

// Development 셰이더 공급자(Docs/Design/RenderBase 9.12절 4항, ShaderSystem 12.3.1절 SSD16·16.3절
// SH4)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace {

constexpr LogCategory LogDevelopmentShaderProvider{"DevelopmentShaderProvider"};

// 경로 버퍼 byte 수다.
constexpr usize ProviderPathBytes = 1024;

// 진단 한 줄 버퍼 byte 수다.
constexpr usize ProviderNoteBytes = 256;

// 공급자 옵션 리플렉션 레지스트리 문맥 id다("DSPV").
constexpr uint32 ProviderReflectionContextId = 0x44535056;

// 라이브러리 종류 수(Engine·Project)다.
constexpr usize LibraryKindCount = 2;

// 감시 스레드가 바뀐 원문이 없을 때 정지 event를 기다리는 시간(ms)이다(반응 지연 상한 — 판정에
// 쓰지 않는다, ShaderSystem 16.3절 SSD77).
constexpr uint32 WatchIdleMilliseconds = 100;

// 한 번의 재컴파일에 모으는 바뀐 원문 수의 상한이다(넘으면 모두 바뀐 것으로 본다).
constexpr usize MaxPendingSources = 256;
// 공급자 스케줄러 스레드 수의 상한이다(ShaderSystem 16.6절 SSD95 — 너겟 조회·쓰기와 job 준비 병렬).
constexpr uint32 MaxProviderSchedulerWorkers = 16;

// 공급자 스케줄러 스레드 수다: LogicalProcessorCount() - 1(호출 스레드 몫을 뺀다), 최소 1, 상한
// MaxProviderSchedulerWorkers(엔진 GC 스케줄러의 자동 크기와 같은 모양).
uint32 ProviderSchedulerWorkers() noexcept {
    const uint32 processors = platform::LogicalProcessorCount();
    const uint32 workers = processors > 1 ? processors - 1 : 1;
    return workers < MaxProviderSchedulerWorkers ? workers : MaxProviderSchedulerWorkers;
}

// 실패 로그에 남기는 진단 수의 상한이다(나머지는 수만 센다).
constexpr usize MaxLoggedDiagnostics = 32;

// 나노초 → 밀리초다.
constexpr uint64 NanosecondsPerMillisecond = 1'000'000;

// 실행 파일 출력 폴더의 이름 규칙(<도구>-<플랫폼>-<타깃>-<구성>)에서 바꿔 넣는 워커 타깃 이름이다
// (Docs/Design/BuildTargets 3.11절 선행 타깃 출력).
constexpr char WorkerTargetName[] = "shadercompileworker";

// 심볼 파일 디렉터리(<너겟 root> 아래 — UE <ProjectSaved>/ShaderSymbols, 16.3절 SSD74)다.
constexpr char SymbolDirectoryName[] = "Saved/ShaderSymbols";

// 대기 작업 종류 이름과 먼저 끝낼 종류(UE FShaderCompilingManager::GetDependentTypeNames — 텍스처가
// 머티리얼을 다시 갱신하게 할 수 있어 먼저 끝낸다), 진행 문구다(진행 보고 8·12장).
constexpr StringView ShadersPendingWorkName = "Shaders";
constexpr StringView ShadersRunBefore[] = {"Textures"};
constexpr StringView ShadersDisplayName = "Shaders";
constexpr StringView CompilingShadersTitle = "Compiling shaders";
constexpr StringView RecompilingShadersTitle = "Recompiling shaders";

// 진행 중계로 넘어가는 Text(작업 제목 — 중계의 사건 기록이 Clone으로 노드를 붙잡는다)와 대기 작업
// 취소 상태(연결 취소의 부모 — 중계 작업 source가 참조한다)의 저장소다. 중계는 이 모듈보다 오래 살
// 수 있으므로(호스트가 엔진 묶음을 내린 뒤 끈다) 모듈 할당자가 아니라 프로세스 수명이다.
Allocator& ProgressAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 컴파일 한 번의 진행이다: 대기 작업·호스트 준비 진행·진행 작업 계수 sink와 그 작업(hub가 꺼져
// 있으면 작업 없이). 만든 자리에서 움직이지 않는다(sink가 멤버 주소를 쥔다).
class CompileProgress final {
public:
    CompileProgress(CountedPendingWork& pending, const render::EngineShaderHost& host,
                    const Text& title, ProgressCancel cancel) noexcept
        : host_(host) {
        ProgressTaskDesc desc;
        desc.title = &title;
        desc.presentation = ProgressPresentation::Background;
        desc.cancel = cancel;
        // 사용자 취소 가능한 작업은 대기 작업 token에도 묶는다(엔진 종료가 취소한다).
        desc.externalCancel =
            cancel == ProgressCancel::UserCancellable ? pending.Token() : CancelToken();
        if (title.IsValid()) {
            if (auto begun = ProgressHub::Get().Begin(desc)) {
                task_ = Move(begun.Value());
                if (auto counter = ProgressCounter::ForTask(task_, 0)) {
                    counter_ = Move(counter.Value());
                }
            }
        }

        sinks_[0] = ShaderCompileProgressSink{&PendingAdded, &PendingFinished, &pending};
        sinks_[1] = ShaderCompileProgressSink{&HostAdded, &HostFinished, this};
        sinks_[2] = MakeShaderCompileCounterSink(counter_);
        control_.sinks = Span<const ShaderCompileProgressSink>(sinks_);
        // 사용자 취소 가능한 작업이면 작업 token(대기 작업 token에 연결됨), 아니면 대기 작업
        // token이다.
        control_.cancel = task_.IsValid() && cancel == ProgressCancel::UserCancellable
                              ? task_.Token()
                              : pending.Token();
    }

    ~CompileProgress() noexcept {
        Finish(ProgressOutcome::Abandoned);
    }

    CompileProgress(const CompileProgress&) = delete;
    CompileProgress& operator=(const CompileProgress&) = delete;

    const ShaderCompileControl& Control() const noexcept {
        return control_;
    }

    // 작업을 끝낸다(두 번째부터는 아무 일도 하지 않는다).
    void Finish(ProgressOutcome outcome) noexcept {
        if (task_.IsValid() && !finished_) {
            (void)task_.Finish(outcome);
        }

        finished_ = true;
    }

private:
    static void PendingAdded(void* context, uint64 jobs) noexcept {
        static_cast<CountedPendingWork*>(context)->AddWork(jobs);
    }

    static void PendingFinished(void* context, uint64 jobs) noexcept {
        static_cast<CountedPendingWork*>(context)->FinishWork(jobs);
    }

    static void HostAdded(void* context, uint64 jobs) noexcept {
        static_cast<CompileProgress*>(context)->host_.ReportPreparationAdded(jobs);
    }

    static void HostFinished(void* context, uint64 jobs) noexcept {
        static_cast<CompileProgress*>(context)->host_.ReportPreparationFinished(jobs);
    }

    const render::EngineShaderHost& host_;
    ProgressTaskHandle task_;
    ProgressCounter counter_;
    ShaderCompileProgressSink sinks_[3];
    ShaderCompileControl control_;
    bool finished_ = false;
};

bool IsSeparator(char c) noexcept {
    return c == '/' || c == '\\';
}

// 옵션 리플렉션 문맥이다(키 6행 — shader-compile 명령과 같은 준비).
struct Reflection {
    explicit Reflection(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator),
          objects(allocator, types, {ProviderReflectionContextId, 1}),
          access(objects.OwnerAccess()) {
        registered = static_cast<bool>(RegisterShaderCompilerTypes(registration));
    }

    ~Reflection() noexcept {
        (void)objects.Shutdown(objects.OwnerAccess());
        (void)registration.Unregister();
        (void)types.Shutdown();
    }

    TypeRegistry types;
    ModuleRegistration registration;
    ObjectRegistry objects;
    ObjectAccessContext access;
    bool registered = false;
};

void CopyText(char* out, usize capacity, StringView text) noexcept {
    (void)snprintf(out, capacity, "%.*s", static_cast<int>(text.ByteCount()), text.Data());
}

// 감시 루트 하나다(mount 하나 — 같은 스레드에서만 쓴다).
struct WatchRoot {
    explicit WatchRoot(Allocator& allocator) noexcept : virtualRoot(allocator) {
    }

    String virtualRoot;
    platform::DirectoryWatcher watcher;
};

// Poll visitor 문맥이다.
struct WatchVisit {
    const WatchRoot* root = nullptr;
    Array<String>* pending = nullptr;
    bool everything = false;
    bool rootRemoved = false;
    // 이번 Poll에 셰이더 원문 사건(이미 모은 경로 포함)이나 Overflow가 있었다(SSD100 묶기).
    bool changed = false;
};

bool AddPending(WatchVisit& visit, StringView relative) noexcept {
    String path(visit.pending->GetAllocator());
    if (!MakeWatchedShaderPath(visit.root->virtualRoot.View(), relative, path)) {
        return true;
    }

    visit.changed = true;

    for (const String& existing : *visit.pending) {
        if (existing.View() == path.View()) {
            return true;
        }
    }

    if (visit.pending->Size() >= MaxPendingSources || !visit.pending->TryPushBack(Move(path))) {
        visit.everything = true;
    }

    return true;
}

bool VisitChange(const platform::DirectoryChange& change, void* context) noexcept {
    auto& visit = *static_cast<WatchVisit*>(context);
    switch (change.kind) {
    case platform::DirectoryChangeKind::Overflow:
        visit.everything = true;
        visit.changed = true;
        return true;
    case platform::DirectoryChangeKind::RootRemoved:
        visit.rootRemoved = true;
        return true;
    case platform::DirectoryChangeKind::Renamed:
        (void)AddPending(visit, StringView(change.oldPath, change.oldPathBytes));
        return AddPending(visit, StringView(change.path, change.pathBytes));
    case platform::DirectoryChangeKind::Added:
    case platform::DirectoryChangeKind::Removed:
    case platform::DirectoryChangeKind::Modified:
        return AddPending(visit, StringView(change.path, change.pathBytes));
    }

    return true;
}

} // namespace

struct DevelopmentShaderProvider::State {
    State(Allocator& allocator, const render::EngineShaderHost& shaderHost,
          const DevelopmentShaderProviderOptions& providerOptions) noexcept
        : allocator(allocator), host(shaderHost), options(providerOptions), tree(allocator),
          reflection(allocator), cache(allocator, platform::PhysicalFileSystem(), clock),
          scheduler(allocator), root(allocator), workRoot(allocator), symbolRoot(allocator),
          diagnostics(allocator) {
    }

    ~State() noexcept {
        Shutdown();
    }

    void StopWatch() noexcept {
        if (!watchStarted) {
            return;
        }

        stopWatch.Signal();
        (void)watchThread.Join();
        watchStarted = false;
    }

    // 대기 작업 종류와 진행 문구를 만든다(TryCreate). 오류는 그 오류다.
    Result<void> Initialize() noexcept {
        Allocator& progress = ProgressAllocator();
        auto shaders = Text::Invariant(progress, ShadersDisplayName);
        auto compiling = Text::Invariant(progress, CompilingShadersTitle);
        auto recompiling = Text::Invariant(progress, RecompilingShadersTitle);
        if (!shaders || !compiling || !recompiling) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        compilingTitle = Move(compiling.Value());
        recompilingTitle = Move(recompiling.Value());
        if (auto initialized =
                pendingWork.Initialize(progress, ShadersPendingWorkName, shaders.Value(),
                                       Span<const StringView>(ShadersRunBefore));
            !initialized) {
            return initialized;
        }

        // 너겟 캐시의 비동기 I/O 스케줄러는 Start·Shutdown이 생성 스레드 전용이다
        // (Task/WorkerScheduler.h). 생성(이 상태)과 Shutdown(모듈 Shutdown)이 모두 이 스레드이므로
        // 여기서 시작한다. 첫 셰이더 요청은 렌더 lane에서 오므로 Prepare에서 시작하면 WrongThread다
        // (Docs/Design/ShaderSystem 12.3.1절 SSD16 "너겟 캐시를 쓰지 못하던 결함"). 실패해도
        // 공급자는 캐시 없이 동작한다.
        WorkerSchedulerConfig schedulerConfig;
        schedulerConfig.workerCount = ProviderSchedulerWorkers();
        if (auto started = scheduler.Start(schedulerConfig); !started) {
            Log::Warning(LogDevelopmentShaderProvider,
                         "The shader nugget scheduler could not start ({Error}); shaders compile "
                         "without the cache",
                         ErrorCodeName(started.Error()));
        } else {
            schedulerStarted = true;
        }

        return Result<void>::Success();
    }

    void Shutdown() noexcept {
        // 진행 중 컴파일을 취소하고 끝날 때까지 기다린다(대기 작업 Shutdown — 엔진 종료가 먼저
        // 불렀으면 기다리기만 한다). 그 뒤 감시 스레드를 멈추고 관리자를 멈춘다.
        pendingWork.Shutdown();
        StopWatch();
        for (usize kind = 0; kind < LibraryKindCount; ++kind) {
            platform::AtomicStore<platform::MemoryOrder::Release>(
                &published[kind], static_cast<ShaderLibraryReloader*>(nullptr));
            libraries[kind].Reset();
        }

        if (manager != nullptr) {
            manager->Shutdown();
            manager->~ShaderCompileManager();
            (void)allocator.Deallocate(manager);
            manager = nullptr;
        }

        if (cacheOpen) {
            cache.Shutdown(0);
            cacheOpen = false;
        }

        if (schedulerStarted) {
            (void)scheduler.Shutdown();
            schedulerStarted = false;
        }

        shutDown = true;
    }

    void Note(StringView text) noexcept {
        (void)diagnostics.TryAppend(text);
    }

    // 원문·워커·너겟 캐시·관리자·라이브러리를 처음 한 번 준비한다(gate 아래).
    bool Prepare() noexcept {
        if (manager != nullptr) {
            return true;
        }

        if (prepareFailed || shutDown) {
            Note(shutDown ? StringView("the shader provider is shut down")
                          : StringView(prepareProblem));
            return false;
        }

        prepareFailed = true;
        if (!PrepareSteps()) {
            CopyText(prepareProblem, sizeof(prepareProblem), diagnostics.View());
            Log::Error(LogDevelopmentShaderProvider,
                       "The development shader provider could not "
                       "start: {Problem}",
                       StringView(prepareProblem));
            return false;
        }

        prepareFailed = false;
        return true;
    }

    bool PrepareSteps() noexcept {
        char engine[ProviderPathBytes] = {};
        auto found = FindEngineDirectory(Span<char>(engine, sizeof(engine)));
        if (!found) {
            Note("cannot find the engine folder");
            return false;
        }

        char path[ProviderPathBytes];
        snprintf(path, sizeof(path), "%s/Shaders", engine);
        if (!tree.Mount(EngineShaderVirtualRoot, StringView(path))) {
            Note("cannot mount /Engine");
            return false;
        }

        // 호스트 원문 디렉터리(Freeze 뒤라 읽기 전용이다).
        for (const render::ShaderSourceDirectory& directory : host.SourceDirectories()) {
            if (!tree.Mount(directory.virtualRoot, directory.directory)) {
                char note[ProviderNoteBytes];
                snprintf(note, sizeof(note), "cannot mount %.*s at %.*s",
                         static_cast<int>(directory.virtualRoot.ByteCount()),
                         directory.virtualRoot.Data(),
                         static_cast<int>(directory.directory.ByteCount()),
                         directory.directory.Data());
                Note(StringView(note));
                return false;
            }
        }

        snprintf(formats, sizeof(formats), "%s/Config/ShaderFormats", engine);
        if (!FindWorker()) {
            Note("ShaderCompileWorker is neither next to the executable nor in the "
                 "ShaderCompileWorker target output of the same build (build it first)");
            return false;
        }

        if (!reflection.registered || !ResolveShaderNuggetRoot(StringView(), root) ||
            !workRoot.TryAssign(root.View()) || !workRoot.TryAppend("/") ||
            !workRoot.TryAppend(ShaderWorkerDirectoryName) || !symbolRoot.TryAssign(root.View()) ||
            !symbolRoot.TryAppend("/") || !symbolRoot.TryAppend(SymbolDirectoryName)) {
            Note("cannot prepare the shader compile workspace");
            return false;
        }

        ShaderCompileManagerConfig config;
        NuggetSettingsFailure failure;
        // 캐시를 쓰지 못하면 매 시작이 엔진 셰이더를 모두 다시 컴파일하므로 원인을 남긴다
        // (Docs/Design/ShaderSystem 12.3.1절 SSD16). 스케줄러는 Initialize가 이미 시작했다.
        if (schedulerStarted) {
            auto cacheOpened = OpenShaderNuggetCache(
                cache, allocator, platform::PhysicalFileSystem(), root.View(), &failure);
            if (!cacheOpened) {
                Log::Warning(LogDevelopmentShaderProvider,
                             "The shader nugget cache under {Root} could not be opened ({Error}); "
                             "shaders compile without it",
                             root.View(), ErrorCodeName(cacheOpened.Error()));
            } else {
                if (cache.OpenError() != ErrorCode::None) {
                    Log::Warning(LogDevelopmentShaderProvider,
                                 "The shader nugget cache under {Root} is off ({Error}); shaders "
                                 "compile without it",
                                 root.View(), ErrorCodeName(cache.OpenError()));
                }

                cacheOpen = true;
                config.cache = &cache;
                config.scheduler = &scheduler;
            }
        }

        config.allocator = &allocator;
        config.tree = &tree;
        config.reflection = ShaderKeyReflection{&reflection.objects, reflection.access,
                                                &reflection.types, &allocator};
        config.workerExecutable = StringView(worker);
        config.workRoot = workRoot.View();
        auto memory =
            allocator.Allocate(sizeof(ShaderCompileManager), alignof(ShaderCompileManager));
        if (!memory) {
            Note("out of memory");
            return false;
        }

        manager = ::new (memory.Value()) ShaderCompileManager(config);
        if (!manager->Start()) {
            Note("cannot start the shader compile workers");
            manager->~ShaderCompileManager();
            (void)allocator.Deallocate(manager);
            manager = nullptr;
            return false;
        }

        for (usize kind = 0; kind < LibraryKindCount; ++kind) {
            ShaderLibraryReloaderDesc desc;
            desc.allocator = &allocator;
            desc.registry = &host.Registry();
            desc.library = static_cast<ShaderLibraryKind>(kind);
            // 개발 전용 타입도 만든다(진단 버퍼 퍼뮤테이션 — ShaderSystem 7.2절 12항, SSD16).
            desc.includeDevelopment = true;
            desc.manager = manager;
            desc.formatDirectory = StringView(formats);
            desc.debugSymbols = options.debugSymbols;
            desc.symbolDirectory = options.debugSymbols ? symbolRoot.View() : StringView();
            auto created = UniquePtr<ShaderLibraryReloader>::TryMake(allocator, desc);
            if (!created) {
                Note("out of memory");
                return false;
            }

            libraries[kind] = Move(created.Value());
            platform::AtomicStore<platform::MemoryOrder::Release>(&published[kind],
                                                                  libraries[kind].Get());
        }

        return true;
    }

    // 워커는 실행 파일 옆, 없으면 같은 출력 뿌리의 ShaderCompileWorker 타깃 출력이다: 실행 파일
    // 폴더가 <out>/<도구>-<플랫폼>-<타깃>-<구성>/bin이면 <out>/<도구>-<플랫폼>-shadercompileworker-
    // <구성>/bin이다. Game 타깃은 워커를 동반 타깃으로 함께 빌드하지 못해 선행 타깃으로 그 출력에
    // 먼저 빌드한다(Docs/Design/BuildTargets 3.1·3.11절).
    bool FindWorker() noexcept {
        usize bytes = 0;
        char executable[ProviderPathBytes] = {};
        if (!platform::GetExecutablePath(executable, sizeof(executable), bytes)) {
            return false;
        }

        while (bytes > 0 && !IsSeparator(executable[bytes - 1])) {
            --bytes;
        }

        executable[bytes] = '\0';
        if (TryWorker(executable)) {
            return true;
        }

        // executable = "<out>/<폴더>/<bin>/"이다. <폴더>를 찾는다.
        usize leafEnd = bytes;
        if (leafEnd == 0) {
            return false;
        }

        --leafEnd;
        usize leafStart = leafEnd;
        while (leafStart > 0 && !IsSeparator(executable[leafStart - 1])) {
            --leafStart;
        }

        if (leafStart == 0) {
            return false;
        }

        const usize folderEnd = leafStart - 1;
        usize folderStart = folderEnd;
        while (folderStart > 0 && !IsSeparator(executable[folderStart - 1])) {
            --folderStart;
        }

        // <도구>-<플랫폼>-<타깃>-<구성>: 둘째 '-'와 마지막 '-' 사이가 타깃이다.
        usize second = 0;
        usize last = 0;
        uint32 dashes = 0;
        for (usize index = folderStart; index < folderEnd; ++index) {
            if (executable[index] == '-') {
                ++dashes;
                second = dashes == 2 ? index : second;
                last = index;
            }
        }

        if (dashes < 3) {
            return false;
        }

        char sibling[ProviderPathBytes] = {};
        snprintf(sibling, sizeof(sibling), "%.*s-%s%.*s", static_cast<int>(second), executable,
                 WorkerTargetName, static_cast<int>(bytes - last), executable + last);
        return TryWorker(sibling);
    }

    bool TryWorker(const char* directory) noexcept {
        for (const char* name : {"ShaderCompileWorker.exe", "ShaderCompileWorker"}) {
            snprintf(worker, sizeof(worker), "%s%s", directory, name);
            bool exists = false;
            if (platform::PhysicalFileSystem().FileExists(worker, strlen(worker), exists) &&
                exists) {
                return true;
            }
        }

        return false;
    }

    ShaderLibraryReloader* Published(ShaderLibraryKind kind) const noexcept {
        const usize index = static_cast<usize>(kind);
        return index < LibraryKindCount
                   ? platform::AtomicLoad<platform::MemoryOrder::Acquire>(&published[index])
                   : nullptr;
    }

    // 진단을 편집기에서 눌러 이동하는 꼴로 남긴다(SSD72 — 디스크 경로가 없으면 가상 경로).
    void LogDiagnostics(const Array<ShaderDiagnostic>& list) noexcept {
        String disk(allocator);
        String line(allocator);
        usize logged = 0;
        for (const ShaderDiagnostic& diagnostic : list) {
            if (logged == MaxLoggedDiagnostics) {
                Log::Error(LogDevelopmentShaderProvider,
                           "... {Count} more shader diagnostic(s) not shown",
                           static_cast<uint64>(list.Size() - logged));
                return;
            }

            ++logged;
            if (diagnostic.virtualPath.IsEmpty() ||
                !tree.ResolveDiskPath(diagnostic.virtualPath.View(), disk)) {
                disk.Clear();
            }

            if (!FormatShaderDiagnostic(diagnostic, disk.View(), line)) {
                continue;
            }

            if (diagnostic.context.IsEmpty()) {
                Log::Error(LogDevelopmentShaderProvider, "{Diagnostic}", line.View());
            } else {
                Log::Error(LogDevelopmentShaderProvider, "{Diagnostic}\n{Context}", line.View(),
                           diagnostic.context.View());
            }
        }
    }

    // 통지를 낸다(순번을 올리고 기다리는 쪽을 깨운다).
    void Publish(render::ShaderReloadNotice notice) noexcept {
        {
            platform::LockGuard lock(noticeGate);
            notice.sequence = latest.sequence + 1;
            latest = notice;
        }

        noticeChanged.NotifyAll();
    }

    // 첫 라이브러리를 만든 뒤 감시 스레드를 띄운다(gate 아래).
    void StartWatch() noexcept {
        if (!options.watchSources || watchStarted || shutDown) {
            return;
        }

        platform::ThreadOptions threadOptions;
        threadOptions.name = "ShaderSourceWatch";
        stopWatch.Reset();
        if (watchThread.Start(&State::WatchEntry, this, threadOptions) !=
            platform::ThreadStatus::Success) {
            Log::Warning(LogDevelopmentShaderProvider,
                         "The shader source watch thread could not start; shaders are not "
                         "recompiled on save");
            return;
        }

        watchStarted = true;
    }

    static void WatchEntry(void* context) noexcept {
        static_cast<State*>(context)->WatchLoop();
    }

    // 감시 스레드 본체다(SSD77): mount마다 감시기를 열고, 기다리지 않는 Poll로 변경을 모은다. 첫
    // 사건 뒤 조용한 구간까지 모았다가(16.6절 SSD100 — ShaderWatchSettled) 한 번 재컴파일한다.
    // 모으는 동안은 ShaderWatchSettlePollMilliseconds, 변경이 없으면 WatchIdleMilliseconds까지 정지
    // event를 기다린다.
    void WatchLoop() noexcept {
        Array<UniquePtr<WatchRoot>> roots(allocator);
        for (usize index = 0; index < tree.MountCount(); ++index) {
            auto created = UniquePtr<WatchRoot>::TryMake(allocator, allocator);
            if (!created || !created.Value()->virtualRoot.TryAssign(tree.MountedDirectory(index))) {
                continue;
            }

            const StringView directory = tree.MountedRealDirectory(index);
            if (const platform::FileStatus opened =
                    created.Value()->watcher.Open(directory.Data(), directory.ByteCount());
                !opened) {
                Log::Warning(LogDevelopmentShaderProvider,
                             "Shader sources under {Root} ({Directory}) are not watched: {Error}",
                             tree.MountedDirectory(index), directory,
                             ErrorCodeName(FileErrorCode(opened)));
                continue;
            }

            if (!roots.TryPushBack(Move(created.Value()))) {
                continue;
            }

            Log::Info(LogDevelopmentShaderProvider, "Watching shader sources {Root} -> {Directory}",
                      tree.MountedDirectory(index), directory);
        }

        Array<String> pending(allocator);
        Array<StringView> views(allocator);
        bool everything = false;
        bool collecting = false;
        uint64 firstChange = 0;
        uint64 lastChange = 0;
        while (stopWatch.Wait(0) != platform::WaitResult::Signaled) {
            bool changed = false;
            for (UniquePtr<WatchRoot>& root : roots) {
                if (!root->watcher.IsOpen()) {
                    continue;
                }

                WatchVisit visit;
                visit.root = root.Get();
                visit.pending = &pending;
                (void)root->watcher.Poll(&VisitChange, &visit);
                everything = everything || visit.everything;
                changed = changed || visit.changed;
                if (visit.rootRemoved) {
                    Log::Error(LogDevelopmentShaderProvider,
                               "The watched shader source root {Root} was removed; it is no "
                               "longer watched",
                               root->virtualRoot.View());
                    root->watcher.Close();
                }
            }

            const uint64 now = platform::MonotonicNanoseconds();
            if (changed) {
                if (!collecting) {
                    collecting = true;
                    firstChange = now;
                }

                lastChange = now;
            }

            if (!collecting) {
                (void)stopWatch.Wait(WatchIdleMilliseconds);
                continue;
            }

            if (!ShaderWatchSettled(firstChange, lastChange, now, ShaderWatchSettleMilliseconds,
                                    ShaderWatchSettleLimitMilliseconds)) {
                (void)stopWatch.Wait(ShaderWatchSettlePollMilliseconds);
                continue;
            }

            if (!pending.IsEmpty() || everything) {
                views.Clear();
                for (const String& path : pending) {
                    (void)views.TryPushBack(path.View());
                }

                (void)Reload(views.View(), everything);
            }

            pending.Clear();
            everything = false;
            collecting = false;
        }

        for (UniquePtr<WatchRoot>& root : roots) {
            root->watcher.Close();
        }
    }

    // 바뀐 원문으로 만든 라이브러리를 다시 만들고 로그·통지를 낸다(SSD70·SSD71).
    Result<void> Reload(Span<const StringView> changed, bool everything) noexcept {
        platform::LockGuard lock(reloadGate);
        // 재컴파일은 대기 작업의 일 구간이고 사용자 취소 가능한 진행 작업이다(PG5).
        CountedPendingWork::WorkScope work = pendingWork.BeginWork();
        CompileProgress progress(pendingWork, host, recompilingTitle,
                                 ProgressCancel::UserCancellable);
        const uint64 started = platform::MonotonicNanoseconds();
        render::ShaderReloadNotice notice;
        notice.succeeded = true;
        Result<void> outcome = Result<void>::Success();
        bool rebuilt = false;
        uint32 affected = 0;
        uint32 formatsRebuilt = 0;
        ShaderReloadReport report(allocator);
        for (usize kind = 0; kind < LibraryKindCount && outcome; ++kind) {
            ShaderLibraryReloader* libraries = Published(static_cast<ShaderLibraryKind>(kind));
            if (libraries == nullptr || libraries->FormatCount() == 0) {
                continue;
            }

            if (auto reloaded = libraries->Reload(changed, everything, report, progress.Control());
                !reloaded) {
                outcome = Result<void>::Failure(reloaded.Error());
                notice.succeeded = false;
                CopyText(notice.message, sizeof(notice.message),
                         StringView(ErrorCodeName(reloaded.Error())));
                if (reloaded.Error() == ErrorCode::Cancelled) {
                    // 사용자가 고른 일이다 — Error가 아니다(SSD71처럼 이전 셰이더 유지).
                    Log::Info(LogDevelopmentShaderProvider,
                              "Shader recompile was cancelled; the previous shaders stay");
                }

                break;
            }

            affected += report.affectedTypes;
            formatsRebuilt += report.formats;
            notice.changedFormats += report.changedFormats;
            notice.failedJobs += report.failedJobs;
            rebuilt = rebuilt || report.outcome != ShaderReloadOutcome::NoShaderAffected;
            if (report.outcome == ShaderReloadOutcome::Failed) {
                notice.succeeded = false;
                if (!report.diagnostics.IsEmpty() && notice.path[0] == '\0' &&
                    notice.message[0] == '\0') {
                    const ShaderDiagnostic& first = report.diagnostics[0];
                    CopyText(notice.path, sizeof(notice.path), first.virtualPath.View());
                    CopyText(notice.message, sizeof(notice.message), first.message.View());
                    notice.line = first.line;
                    notice.column = first.column;
                }

                LogDiagnostics(report.diagnostics);
                Log::Error(LogDevelopmentShaderProvider,
                           "Shader recompile failed: {Jobs} job(s) failed (first {Type} for "
                           "{Format}); the previous shaders stay",
                           static_cast<uint64>(report.failedJobs), report.failedType.View(),
                           report.failedFormat.View());
            }
        }

        notice.elapsedMilliseconds =
            (platform::MonotonicNanoseconds() - started) / NanosecondsPerMillisecond;
        if (rebuilt && notice.succeeded) {
            Log::Info(LogDevelopmentShaderProvider,
                      "Shader recompile finished in {Milliseconds} ms: {Types} shader type(s) "
                      "affected, {Changed} of {Formats} format librar(ies) changed",
                      notice.elapsedMilliseconds, static_cast<uint64>(affected),
                      static_cast<uint64>(notice.changedFormats),
                      static_cast<uint64>(formatsRebuilt));
        }

        progress.Finish(outcome                                   ? ProgressOutcome::Succeeded
                        : outcome.Error() == ErrorCode::Cancelled ? ProgressOutcome::Cancelled
                                                                  : ProgressOutcome::Failed);
        Publish(notice);
        return outcome;
    }

    Allocator& allocator;
    const render::EngineShaderHost& host;
    DevelopmentShaderProviderOptions options;
    // 준비와 첫 만들기를 차례로 돌린다(이미 만든 형식의 요청은 잡지 않는다).
    platform::Mutex gate;
    // 재컴파일(감시 스레드·NotifySourcesChanged)을 차례로 돌린다.
    platform::Mutex reloadGate;
    ShaderSourceTree tree;
    Reflection reflection;
    NuggetSystemClock clock;
    NuggetCache cache;
    WorkerScheduler scheduler;
    String root;
    String workRoot;
    String symbolRoot;
    String diagnostics;
    ShaderCompileManager* manager = nullptr;
    // 종류(Engine·Project)마다의 라이브러리와 그 게시 포인터(준비가 끝나면 Release로 게시 — 빠른
    // 요청 경로가 gate 없이 읽는다)다.
    UniquePtr<ShaderLibraryReloader> libraries[LibraryKindCount];
    ShaderLibraryReloader* published[LibraryKindCount] = {};
    // 통지다.
    mutable platform::Mutex noticeGate;
    platform::ConditionVariable noticeChanged;
    render::ShaderReloadNotice latest;
    // 감시 스레드다.
    platform::Thread watchThread;
    platform::ManualResetEvent stopWatch;
    bool watchStarted = false;
    // 대기 작업 "Shaders"와 진행 작업 제목이다(PG5).
    CountedPendingWork pendingWork;
    Text compilingTitle;
    Text recompilingTitle;
    char worker[ProviderPathBytes] = {};
    char formats[ProviderPathBytes] = {};
    char prepareProblem[ProviderNoteBytes] = {};
    bool cacheOpen = false;
    // Initialize가 스케줄러를 시작했는지다(Shutdown이 같은 스레드에서 닫는다).
    bool schedulerStarted = false;
    bool prepareFailed = false;
    bool shutDown = false;
};

Result<UniquePtr<DevelopmentShaderProvider>>
DevelopmentShaderProvider::TryCreate(Allocator& allocator, const render::EngineShaderHost& host,
                                     const DevelopmentShaderProviderOptions& options) noexcept {
    auto state = UniquePtr<State>::TryMake(allocator, allocator, host, options);
    if (!state) {
        return Result<UniquePtr<DevelopmentShaderProvider>>::Failure(state.Error());
    }

    if (auto initialized = state.Value()->Initialize(); !initialized) {
        return Result<UniquePtr<DevelopmentShaderProvider>>::Failure(initialized.Error());
    }

    return UniquePtr<DevelopmentShaderProvider>::TryMake(allocator, allocator, Move(state.Value()));
}

DevelopmentShaderProvider::DevelopmentShaderProvider(Allocator& allocator,
                                                     UniquePtr<State> state) noexcept
    : allocator_(allocator), state_(Move(state)) {
}

DevelopmentShaderProvider::~DevelopmentShaderProvider() noexcept = default;

Result<void>
DevelopmentShaderProvider::RequestLibrary(const render::ShaderProvisionRequest& request,
                                          render::ShaderSet& set) noexcept {
    State& state = *state_;
    Array<uint8> copy(allocator_);
    // 이미 만든 형식은 잠금 없이 사본을 넘긴다(다시 만드는 동안에도 짧은 잠금만 — SSD76).
    if (ShaderLibraryReloader* libraries = state.Published(request.library)) {
        auto copied = libraries->CopyLibrary(request.formatId, copy);
        if (copied) {
            set.CompleteProvision(Result<void>::Success(), Move(copy), StringView());
            return Result<void>::Success();
        }

        if (copied.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    platform::LockGuard lock(state.gate);
    // 첫 만들기(준비 포함)는 대기 작업의 일 구간이고 취소 불가 진행 작업이다(PG5 — 취소하면 렌더
    // 집합이 열리지 않는다. 엔진 종료의 대기 작업 token은 본다).
    CountedPendingWork::WorkScope work = state.pendingWork.BeginWork();
    CompileProgress progress(state.pendingWork, state.host, state.compilingTitle,
                             ProgressCancel::NotCancellable);
    state.diagnostics.Clear();
    ShaderLibraryReloader* libraries = nullptr;
    // 시작 시간 진단(준비 = 원문 트리·너겟 캐시·워커 시작, 만들기 =
    // 열거·전처리·키·조회·컴파일)이다.
    const uint64 prepareStart = platform::MonotonicNanoseconds();
    if (state.Prepare()) {
        libraries = state.Published(request.library);
        if (libraries == nullptr) {
            state.Note("unknown shader library kind");
        }
    }

    const uint64 buildStart = platform::MonotonicNanoseconds();
    const ShaderCompileProgress before =
        state.manager != nullptr ? state.manager->Progress() : ShaderCompileProgress{};
    if (libraries != nullptr) {
        ShaderLibraryCookReport report(allocator_);
        auto built = libraries->Build(request.formatId, report, progress.Control());
        if (built && state.manager != nullptr) {
            const ShaderCompileProgress after = state.manager->Progress();
            const uint64 end = platform::MonotonicNanoseconds();
            Log::Info(
                LogDevelopmentShaderProvider,
                "The {Format} shader library was built in {BuildMs} ms after {PrepareMs} ms "
                "of preparation: {Jobs} jobs, {Hits} nugget hits, {Workers} workers started, "
                "{Batches} batches (started at monotonic {StartMs} ms; prepare {PrepMs} ms, "
                "lookup {LookupMs} ms, compile {CompileMs} ms, publish {PublishMs} ms)",
                request.formatId, (end - buildStart) / NanosecondsPerMillisecond,
                (buildStart - prepareStart) / NanosecondsPerMillisecond, after.total - before.total,
                after.cacheHits - before.cacheHits, after.workersStarted - before.workersStarted,
                after.batches - before.batches, buildStart / NanosecondsPerMillisecond,
                (after.prepareNanoseconds - before.prepareNanoseconds) / NanosecondsPerMillisecond,
                (after.lookupNanoseconds - before.lookupNanoseconds) / NanosecondsPerMillisecond,
                (after.compileNanoseconds - before.compileNanoseconds) / NanosecondsPerMillisecond,
                (after.publishNanoseconds - before.publishNanoseconds) / NanosecondsPerMillisecond);
        }

        if (!built) {
            char line[ProviderNoteBytes];
            snprintf(line, sizeof(line), "shader library compile failed (%s)",
                     ErrorCodeName(built.Error()));
            state.Note(StringView(line));
            if (built.Error() == ErrorCode::Cancelled) {
                // 엔진 종료(대기 작업 Shutdown)가 취소했다 — 오류가 아니다.
                Log::Info(LogDevelopmentShaderProvider,
                          "The {Format} shader library build was cancelled", request.formatId);
                progress.Finish(ProgressOutcome::Cancelled);
                set.CompleteProvision(Result<void>::Failure(ErrorCode::Cancelled),
                                      Array<uint8>(allocator_), state.diagnostics.View());
                return Result<void>::Success();
            }

            Array<ShaderDiagnostic> diagnostics(allocator_);
            if (CollectShaderLibraryDiagnostics(report, diagnostics)) {
                state.LogDiagnostics(diagnostics);
                if (!diagnostics.IsEmpty()) {
                    String first(allocator_);
                    if (FormatShaderDiagnostic(diagnostics[0], StringView(), first)) {
                        state.Note("; ");
                        state.Note(first.View());
                    }
                }
            }

            Log::Error(LogDevelopmentShaderProvider,
                       "The {Format} shader library could not be built: {Problem}",
                       request.formatId, state.diagnostics.View());
            libraries = nullptr;
        }
    }

    if (libraries == nullptr) {
        progress.Finish(ProgressOutcome::Failed);
        set.CompleteProvision(Result<void>::Failure(ErrorCode::DependencyFailed),
                              Array<uint8>(allocator_), state.diagnostics.View());
        return Result<void>::Success();
    }

    if (auto copied = libraries->CopyLibrary(request.formatId, copy); !copied) {
        progress.Finish(ProgressOutcome::Failed);
        return Result<void>::Failure(copied.Error());
    }

    progress.Finish(ProgressOutcome::Succeeded);
    state.StartWatch();
    set.CompleteProvision(Result<void>::Success(), Move(copy), StringView());
    return Result<void>::Success();
}

uint64 DevelopmentShaderProvider::LibraryRevision(StringView formatId,
                                                  ShaderLibraryKind library) const noexcept {
    ShaderLibraryReloader* libraries = state_->Published(library);
    return libraries != nullptr ? libraries->Revision(formatId) : 0;
}

bool DevelopmentShaderProvider::WaitForReload(uint64 afterSequence, uint32 timeoutMilliseconds,
                                              render::ShaderReloadNotice& out) noexcept {
    State& state = *state_;
    const uint64 deadline = platform::MonotonicNanoseconds() +
                            static_cast<uint64>(timeoutMilliseconds) * NanosecondsPerMillisecond;
    platform::LockGuard lock(state.noticeGate);
    while (state.latest.sequence <= afterSequence) {
        const uint64 now = platform::MonotonicNanoseconds();
        if (now >= deadline) {
            return false;
        }

        const uint64 remaining =
            (deadline - now + NanosecondsPerMillisecond - 1) / NanosecondsPerMillisecond;
        (void)state.noticeChanged.Wait(state.noticeGate, static_cast<uint32>(remaining));
    }

    out = state.latest;
    return true;
}

Result<void> DevelopmentShaderProvider::NotifySourcesChanged(Span<const StringView> virtualPaths,
                                                             bool everything) noexcept {
    return state_->Reload(virtualPaths, everything);
}

bool DevelopmentShaderProvider::IsWatching() const noexcept {
    platform::LockGuard lock(state_->gate);
    return state_->watchStarted;
}

void DevelopmentShaderProvider::Shutdown() noexcept {
    // 진행 중 컴파일을 먼저 취소하고 기다린다(gate를 쥔 첫 만들기가 빨리 끝나게). 감시 스레드는
    // gate 밖에서 멈춘다(그 재컴파일은 gate를 잡지 않지만 Shutdown은 둘 다 정리한다).
    state_->pendingWork.Shutdown();
    state_->StopWatch();
    platform::LockGuard lock(state_->gate);
    state_->Shutdown();
}

PendingWorkSource& DevelopmentShaderProvider::PendingWork() noexcept {
    return state_->pendingWork;
}

usize DevelopmentShaderProvider::BuiltLibraryCount() const noexcept {
    usize count = 0;
    for (usize kind = 0; kind < LibraryKindCount; ++kind) {
        if (ShaderLibraryReloader* libraries =
                state_->Published(static_cast<ShaderLibraryKind>(kind))) {
            count += libraries->FormatCount();
        }
    }

    return count;
}

} // namespace rocks::shader
