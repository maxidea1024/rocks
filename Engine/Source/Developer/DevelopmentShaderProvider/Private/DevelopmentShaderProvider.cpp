// Development 셰이더 공급자다(Docs/Design/ShaderSystem 16절 SSD19 — Sandbox 공급자를 그대로 옮기고
// mount·호스트 타깃 이름만 인자로 받는다, 2026-11-25 공용화 메모).
#include <DevelopmentShaderProvider/DevelopmentShaderProvider.h>

#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/InitializerList.h>
#include <Base/PlacementNew.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Text/String.h>
#include <HostConfig/HostSettings.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderJobKey.h>
#include <ShaderCompiler/ShaderLibraryCook.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCompiler/ShaderSourceTree.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <Task/WorkerScheduler.h>
#include <stdio.h>
#include <string.h>

ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, DevelopmentShaderProvider)

namespace rocks::shader {

namespace {

inline constexpr LogCategory LogDevelopmentShaderProvider{"DevelopmentShaderProvider"};

// 경로 버퍼 byte 수다.
constexpr usize PathBytes = 1024;

// 진단 한 줄 버퍼와 작업 진단을 진단 문자열에 남기는 최대 byte 수다.
constexpr usize NoteLineBytes = 256;
constexpr usize JobDiagnosticBytes = 400;

// 공급자 레지스트리 문맥 id다("DVSP").
constexpr uint32 ReflectionContextId = 0x44565350;

// 옵션 리플렉션 문맥이다(키 6행 — shader-compile 명령과 같은 준비).
struct Reflection {
    explicit Reflection(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator),
          objects(allocator, types, {ReflectionContextId, 1}), access(objects.OwnerAccess()) {
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
    explicit State(const DevelopmentShaderProviderDesc& desc) noexcept
        : allocator(*desc.allocator), desc(desc), tree(allocator), reflection(allocator), clock(),
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
        libraries.Clear();
    }

    void Note(StringView text) noexcept {
        (void)diagnostics.TryAppend(text);
    }

    // 엔진·호스트 원문, 워커, 너겟 캐시, 관리자를 처음 한 번 준비한다.
    bool Prepare() noexcept {
        if (manager != nullptr) {
            return true;
        }

        if (prepareFailed) {
            Note("the shader provider could not start earlier");
            return false;
        }

        prepareFailed = true;
        char engine[PathBytes] = {};
        if (!FindEngineDirectory(Span<char>(engine, sizeof(engine)))) {
            Note("cannot find the engine folder");
            return false;
        }

        char path[PathBytes];
        snprintf(path, sizeof(path), "%s/Shaders", engine);
        if (!tree.Mount("/Engine", StringView(path))) {
            Note("cannot mount /Engine");
            return false;
        }

        // 호스트 원문은 저장소 루트(엔진 폴더의 부모) 기준이다.
        for (const ShaderSourceMountDesc& mount : desc.mounts) {
            snprintf(path, sizeof(path), "%s/../%.*s", engine,
                     static_cast<int>(mount.repositoryPath.ByteCount()),
                     mount.repositoryPath.Data());
            if (!tree.Mount(mount.virtualRoot, StringView(path))) {
                Note("cannot mount a host shader source root");
                return false;
            }
        }

        snprintf(formats, sizeof(formats), "%s/Config/ShaderFormats", engine);
        if (!FindWorker()) {
            Note("ShaderCompileWorker is neither next to the executable nor in the "
                 "ShaderCompileWorker target output (build it first as a prerequisite target)");
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

        prepareFailed = false;
        return true;
    }

    // 워커는 실행 파일 옆, 없으면 같은 구성으로 빌드한 ShaderCompileWorker 타깃 출력
    // (out/<도구>-<플랫폼>-shadercompileworker-<구성>/bin)이다. Game 타깃 호스트는 워커를 동반
    // 타깃으로 함께 빌드하지 못하므로(Docs/Design/BuildTargets 3.1절) 선행 타깃으로 먼저 빌드한다
    // (3.11절).
    bool FindWorker() noexcept {
        usize bytes = 0;
        char executable[PathBytes] = {};
        if (!platform::GetExecutablePath(executable, sizeof(executable), bytes)) {
            return false;
        }

        while (bytes > 0 && executable[bytes - 1] != '/' && executable[bytes - 1] != '\\') {
            --bytes;
        }

        executable[bytes] = '\0';
        if (TryWorker(executable)) {
            return true;
        }

        if (desc.hostTarget.ByteCount() == 0) {
            return false;
        }

        // <out>/<이름>-<호스트>-<구성>/bin/ → <out>/<이름>-shadercompileworker-<구성>/bin/
        char marker[PathBytes] = {};
        snprintf(marker, sizeof(marker), "-%.*s-", static_cast<int>(desc.hostTarget.ByteCount()),
                 desc.hostTarget.Data());
        const char* found = strstr(executable, marker);
        if (found == nullptr) {
            return false;
        }

        char sibling[PathBytes] = {};
        snprintf(sibling, sizeof(sibling), "%.*s-shadercompileworker-%s",
                 static_cast<int>(found - executable), executable, found + strlen(marker));
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

        worker[0] = '\0';
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

        const ShaderFormatStatus loaded =
            LoadShaderFormatSnapshot(formats, request.formatId, *snapshot);
        if (!loaded) {
            Note("cannot load the shader format definition");
            return nullptr;
        }

        const ShaderFormatSnapshot* formatList[] = {snapshot};
        ShaderLibraryCookDesc cook;
        cook.allocator = &allocator;
        cook.registry = request.registry;
        cook.formats = formatList;
        cook.library = request.library;
        // Development 공급자는 개발 전용 타입도 만든다(진단 버퍼 퍼뮤테이션 — ShaderSystem 7.2절
        // 12항, RenderBase 9.11절 6항의 D3D12 root 예산 시험).
        cook.includeDevelopment = true;
        cook.manager = manager;
        ShaderLibraryCookReport report(allocator);
        if (!libraries.TryReserve(libraries.Size() + 1)) {
            Note("out of memory");
            return nullptr;
        }

        BuiltLibrary built(allocator);
        if (!CompileShaderLibrary(cook, report, built.bytes)) {
            Note("shader library compile failed");
            for (const ShaderLibraryCookJobFailure& job : report.failures) {
                char line[NoteLineBytes];
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
    DevelopmentShaderProviderDesc desc;
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
    char worker[PathBytes] = {};
    char formats[PathBytes] = {};
    bool cacheOpen = false;
    bool prepareFailed = false;
};

DevelopmentShaderProvider::DevelopmentShaderProvider(
    const DevelopmentShaderProviderDesc& desc) noexcept
    : desc_(desc) {
}

DevelopmentShaderProvider::~DevelopmentShaderProvider() noexcept = default;

Result<void>
DevelopmentShaderProvider::RequestLibrary(const render::ShaderProvisionRequest& request,
                                          render::ShaderSet& set) noexcept {
    if (desc_.allocator == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (state_.Get() == nullptr) {
        auto made = UniquePtr<State>::TryMake(*desc_.allocator, desc_);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        state_ = Move(made.Value());
    }

    State& state = *state_;
    state.diagnostics.Clear();
    const BuiltLibrary* built = state.Build(request);
    if (built == nullptr) {
        set.CompleteProvision(Result<void>::Failure(ErrorCode::DependencyFailed),
                              Array<uint8>(state.allocator), state.diagnostics.View());
        return Result<void>::Success();
    }

    Array<uint8> copy(state.allocator);
    if (!copy.TryAppend(built->bytes.View())) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    set.CompleteProvision(Result<void>::Success(), Move(copy), StringView());
    return Result<void>::Success();
}

void DevelopmentShaderProvider::Shutdown() noexcept {
    state_ = UniquePtr<State>();
}

StringView DevelopmentShaderFormat(graphics::Backend backend) noexcept {
    switch (backend) {
    case graphics::Backend::D3D12:
        return "D3D12_SM6";
    case graphics::Backend::D3D11:
        return "D3D11_SM5";
    case graphics::Backend::Vulkan:
        return "VULKAN_SM5";
    case graphics::Backend::OpenGL:
        return "GLES31_PC";
    case graphics::Backend::Metal:
        return "METAL_SM5";
    }

    return StringView();
}

} // namespace rocks::shader
