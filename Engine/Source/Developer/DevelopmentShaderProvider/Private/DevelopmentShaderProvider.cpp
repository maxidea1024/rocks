#include <DevelopmentShaderProvider/DevelopmentShaderProvider.h>

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/InitializerList.h>
#include <Base/PlacementNew.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <HostConfig/HostSettings.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <Platform/Sync.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderJobKey.h>
#include <ShaderCompiler/ShaderLibraryCook.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCompiler/ShaderSourceTree.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCore/ShaderVirtualPath.h>
#include <Task/WorkerScheduler.h>
#include <stdio.h>
#include <string.h>

// Development 셰이더 공급자(Docs/Design/RenderBase 9.12절 4항, ShaderSystem 12.3.1절 SSD16)다.
// 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace {

constexpr LogCategory LogDevelopmentShaderProvider{"DevelopmentShaderProvider"};

// 경로 버퍼 byte 수다.
constexpr usize ProviderPathBytes = 1024;

// 진단 한 줄 버퍼와 job 진단을 잘라 넣는 최대 byte 수다.
constexpr usize ProviderNoteBytes = 256;
constexpr usize JobDiagnosticBytes = 400;

// 공급자 옵션 리플렉션 레지스트리 문맥 id다("DSPV").
constexpr uint32 ProviderReflectionContextId = 0x44535056;

// 실행 파일 출력 폴더의 이름 규칙(<도구>-<플랫폼>-<타깃>-<구성>)에서 바꿔 넣는 워커 타깃 이름이다
// (Docs/Design/BuildTargets 3.11절 선행 타깃 출력).
constexpr char WorkerTargetName[] = "shadercompileworker";

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

// 형식 하나의 만든 라이브러리다.
struct BuiltLibrary {
    explicit BuiltLibrary(Allocator& allocator) noexcept : bytes(allocator) {
    }

    char formatId[MaxShaderFormatIdBytes] = {};
    Array<uint8> bytes;
};

} // namespace

struct DevelopmentShaderProvider::State {
    State(Allocator& allocator, const render::EngineShaderHost& shaderHost) noexcept
        : allocator(allocator), host(shaderHost), tree(allocator), reflection(allocator),
          cache(allocator, platform::PhysicalFileSystem(), clock), scheduler(allocator),
          root(allocator), workRoot(allocator), snapshots(allocator), libraries(allocator),
          diagnostics(allocator) {
    }

    ~State() noexcept {
        Shutdown();
    }

    void Shutdown() noexcept {
        if (manager != nullptr) {
            manager->Shutdown();
            manager->~ShaderCompileManager();
            (void)allocator.Deallocate(manager);
            manager = nullptr;
        }

        if (cacheOpen) {
            cache.Shutdown(0);
            (void)scheduler.Shutdown();
            cacheOpen = false;
        }

        for (ShaderFormatSnapshot* snapshot : snapshots) {
            snapshot->~ShaderFormatSnapshot();
            (void)allocator.Deallocate(snapshot);
        }

        snapshots.Clear();
        shutDown = true;
    }

    void Note(StringView text) noexcept {
        (void)diagnostics.TryAppend(text);
    }

    // 원문·워커·너겟 캐시·관리자를 처음 한 번 준비한다.
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
            snprintf(prepareProblem, sizeof(prepareProblem), "%.*s",
                     static_cast<int>(diagnostics.View().ByteCount()), diagnostics.View().Data());
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
            !workRoot.TryAppend(ShaderWorkerDirectoryName)) {
            Note("cannot prepare the shader compile workspace");
            return false;
        }

        ShaderCompileManagerConfig config;
        NuggetSettingsFailure failure;
        if (OpenShaderNuggetCache(cache, allocator, platform::PhysicalFileSystem(), root.View(),
                                  &failure) &&
            scheduler.Start()) {
            cacheOpen = true;
            config.cache = &cache;
            config.scheduler = &scheduler;
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

    const BuiltLibrary* Build(const render::ShaderProvisionRequest& request) noexcept {
        for (const BuiltLibrary& library : libraries) {
            if (StringView(library.formatId) == request.formatId) {
                return &library;
            }
        }

        if (!Prepare()) {
            return nullptr;
        }

        auto memory =
            allocator.Allocate(sizeof(ShaderFormatSnapshot), alignof(ShaderFormatSnapshot));
        if (!memory) {
            Note("out of memory");
            return nullptr;
        }

        auto* snapshot = ::new (memory.Value()) ShaderFormatSnapshot(allocator);
        if (!snapshots.TryPushBack(snapshot)) {
            snapshot->~ShaderFormatSnapshot();
            (void)allocator.Deallocate(snapshot);
            Note("out of memory");
            return nullptr;
        }

        if (!LoadShaderFormatSnapshot(formats, request.formatId, *snapshot)) {
            Note("cannot load the shader format definition");
            return nullptr;
        }

        const ShaderFormatSnapshot* formatSnapshots[] = {snapshot};
        ShaderLibraryCookDesc desc;
        desc.allocator = &allocator;
        desc.registry = request.registry;
        desc.formats = formatSnapshots;
        desc.library = request.library;
        // 개발 전용 타입도 만든다(진단 버퍼 퍼뮤테이션 — ShaderSystem 7.2절 12항, SSD16).
        desc.includeDevelopment = true;
        desc.manager = manager;
        ShaderLibraryCookReport report(allocator);
        if (!libraries.TryReserve(libraries.Size() + 1)) {
            Note("out of memory");
            return nullptr;
        }

        BuiltLibrary built(allocator);
        if (!CompileShaderLibrary(desc, report, built.bytes)) {
            Note("shader library compile failed");
            for (const ShaderLibraryCookJobFailure& job : report.failures) {
                char line[ProviderNoteBytes];
                snprintf(line, sizeof(line), "; %s permutation %u: ", job.typeName,
                         job.permutationId);
                Note(StringView(line));
                Note(job.diagnostics.View().LeftBytesAtCodePoint(JobDiagnosticBytes));
                Log::Error(LogDevelopmentShaderProvider,
                           "Shader {Type} ({Format}) failed: {Diagnostics}", job.typeName,
                           job.formatId, job.diagnostics.CStr());
            }

            return nullptr;
        }

        snprintf(built.formatId, sizeof(built.formatId), "%.*s",
                 static_cast<int>(request.formatId.ByteCount()), request.formatId.Data());
        (void)libraries.TryPushBack(Move(built));
        return &libraries[libraries.Size() - 1];
    }

    Allocator& allocator;
    const render::EngineShaderHost& host;
    platform::Mutex gate;
    ShaderSourceTree tree;
    Reflection reflection;
    NuggetSystemClock clock;
    NuggetCache cache;
    WorkerScheduler scheduler;
    String root;
    String workRoot;
    Array<ShaderFormatSnapshot*> snapshots;
    Array<BuiltLibrary> libraries;
    String diagnostics;
    ShaderCompileManager* manager = nullptr;
    char worker[ProviderPathBytes] = {};
    char formats[ProviderPathBytes] = {};
    char prepareProblem[ProviderNoteBytes] = {};
    bool cacheOpen = false;
    bool prepareFailed = false;
    bool shutDown = false;
};

Result<UniquePtr<DevelopmentShaderProvider>>
DevelopmentShaderProvider::TryCreate(Allocator& allocator,
                                     const render::EngineShaderHost& host) noexcept {
    auto state = UniquePtr<State>::TryMake(allocator, allocator, host);
    if (!state) {
        return Result<UniquePtr<DevelopmentShaderProvider>>::Failure(state.Error());
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
    platform::LockGuard lock(state_->gate);
    state_->diagnostics.Clear();
    const BuiltLibrary* built = state_->Build(request);
    if (built == nullptr) {
        set.CompleteProvision(Result<void>::Failure(ErrorCode::DependencyFailed),
                              Array<uint8>(allocator_), state_->diagnostics.View());
        return Result<void>::Success();
    }

    Array<uint8> copy(allocator_);
    if (!copy.TryAppend(built->bytes.View())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    set.CompleteProvision(Result<void>::Success(), Move(copy), StringView());
    return Result<void>::Success();
}

void DevelopmentShaderProvider::Shutdown() noexcept {
    platform::LockGuard lock(state_->gate);
    state_->Shutdown();
}

usize DevelopmentShaderProvider::BuiltLibraryCount() const noexcept {
    platform::LockGuard lock(state_->gate);
    return state_->libraries.Size();
}

} // namespace rocks::shader
