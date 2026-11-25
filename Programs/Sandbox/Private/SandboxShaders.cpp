#include "SandboxShaders.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Graphics/GraphicsLog.h>
#include <RenderBase/EngineShaderTypes.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <stdio.h>
#include <string.h>

#if SANDBOX_SHADER_PROVIDER
#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Base/InitializerList.h>
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
#endif

// Sandbox 셰이더 집합과 Development 공급자(Docs/Design/ShaderSystem 16.1절 SSD19, 12.3.1절
// SSD16)다.
namespace sandbox::shaders {

using namespace rocks;

namespace {

#define SANDBOX_VERTEX(Variable, File, Entry) &Variable,
#define SANDBOX_PIXEL(Variable, File, Entry) &Variable,
#define SANDBOX_COMPUTE(Variable, File, Entry) &Variable,
#define SANDBOX_VERTEX_IF(Variable, File, Entry, Filter) &Variable,
#define SANDBOX_PIXEL_IF(Variable, File, Entry, Filter) &Variable,
#define SANDBOX_PIXEL_DEV_IF(Variable, File, Entry, Filter) &Variable,
const ShaderTypeDesc* const Types[] = {
#include "SandboxShaderTypes.inl"
};
#undef SANDBOX_VERTEX
#undef SANDBOX_PIXEL
#undef SANDBOX_COMPUTE
#undef SANDBOX_VERTEX_IF
#undef SANDBOX_PIXEL_IF
#undef SANDBOX_PIXEL_DEV_IF

// 경로 버퍼 byte 수다.
constexpr usize PathBytes = 1024;

SystemAllocator& ProcessAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 등록부다(처음 쓸 때 등록·Freeze한다 — 프로세스 수명).
shader::ShaderTypeRegistry* Registry() noexcept {
    static shader::ShaderTypeRegistry registry(ProcessAllocator());
    // 엔진 셰이더 타입(RenderBase 11.4절 2항)도 함께 등록한다(엔진 호스트에서는 RenderBase 모듈이
    // 호스트 등록부에 넣는다).
    static bool ready =
        registry.Register(Types) && registry.Register(render::EngineShaderTypes()) &&
        registry.RegisterVertexInputTypes(render::EngineVertexInputTypes()) && registry.Freeze();
    if (!ready) {
        Log::Error(graphics::LogGraphics,
                   "Sandbox shader types failed to register: {Type}: {Reason}",
                   registry.LastFailure().typeName, registry.LastFailure().reason);
        return nullptr;
    }

    return &registry;
}

// 장치 backend의 셰이더 형식 id다(호스트 Windows TargetProfile 대신 — SH2d 전 고정 표).
StringView FormatFor(graphics::Backend backend) noexcept {
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

render::ShaderSet* active = nullptr;

#if SANDBOX_SHADER_PROVIDER

// 옵션 리플렉션 문맥이다(키 6행 — shader-compile 명령과 같은 준비).
struct Reflection {
    explicit Reflection(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator),
          objects(allocator, types, {ContextId, 1}), access(objects.OwnerAccess()) {
        registered = static_cast<bool>(shader::RegisterShaderCompilerTypes(registration));
    }

    ~Reflection() noexcept {
        (void)objects.Shutdown(objects.OwnerAccess());
        (void)registration.Unregister();
        (void)types.Shutdown();
    }

    // Sandbox 공급자 레지스트리 문맥 id다("SBSP").
    static constexpr uint32 ContextId = 0x53425350;

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

    char formatId[shader::MaxShaderFormatIdBytes] = {};
    Array<uint8> bytes;
};

// Development 공급자다: 형식마다 한 번 CompileShaderLibrary로 메모리 라이브러리를 만들고(너겟
// 캐시로 다음 실행은 재사용) 요청한 셰이더 집합에 넘긴다. 같은 스레드에서 끝낸다.
class DevelopmentProvider final : public render::ShaderProvider {
public:
    explicit DevelopmentProvider(Allocator& allocator) noexcept
        : allocator_(allocator), tree_(allocator), reflection_(allocator), clock_(),
          cache_(allocator, platform::PhysicalFileSystem(), clock_), scheduler_(allocator),
          root_(allocator), workRoot_(allocator), snapshots_(allocator), libraries_(allocator),
          diagnostics_(allocator) {
    }

    ~DevelopmentProvider() noexcept {
        Shutdown();
    }

    void Shutdown() noexcept {
        if (manager_ != nullptr) {
            manager_->Shutdown();
            manager_->~ShaderCompileManager();
            (void)allocator_.Deallocate(manager_);
            manager_ = nullptr;
        }

        if (cacheOpen_) {
            cache_.Shutdown(0);
            (void)scheduler_.Shutdown();
            cacheOpen_ = false;
        }

        for (shader::ShaderFormatSnapshot* snapshot : snapshots_) {
            snapshot->~ShaderFormatSnapshot();
            (void)allocator_.Deallocate(snapshot);
        }

        snapshots_.Clear();
    }

    Result<void> RequestLibrary(const render::ShaderProvisionRequest& request,
                                render::ShaderSet& set) noexcept override {
        diagnostics_.Clear();
        const BuiltLibrary* built = Build(request);
        if (built == nullptr) {
            set.CompleteProvision(Result<void>::Failure(ErrorCode::DependencyFailed),
                                  Array<uint8>(allocator_), diagnostics_.View());
            return Result<void>::Success();
        }

        Array<uint8> copy(allocator_);
        if (!copy.TryAppend(built->bytes.View())) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        set.CompleteProvision(Result<void>::Success(), Move(copy), StringView());
        return Result<void>::Success();
    }

private:
    void Note(const char* text) noexcept {
        (void)diagnostics_.TryAppend(StringView(text));
    }

    // 엔진·Sandbox 셰이더 원문, 워커, 너겟 캐시, 관리자를 처음 한 번 준비한다.
    bool Prepare() noexcept {
        if (manager_ != nullptr) {
            return true;
        }

        if (prepareFailed_) {
            Note("the shader provider could not start earlier");
            return false;
        }

        prepareFailed_ = true;
        char engine[PathBytes] = {};
        if (!FindEngineDirectory(Span<char>(engine, sizeof(engine)))) {
            Note("cannot find the engine folder");
            return false;
        }

        char path[PathBytes];
        snprintf(path, sizeof(path), "%s/Shaders", engine);
        if (!tree_.Mount("/Engine", StringView(path))) {
            Note("cannot mount /Engine");
            return false;
        }

        // Sandbox 원문은 저장소의 Programs/Sandbox/Shaders다(엔진 폴더의 형제 폴더).
        snprintf(path, sizeof(path), "%s/../Programs/Sandbox/Shaders", engine);
        if (!tree_.Mount("/Plugin/Sandbox", StringView(path))) {
            Note("cannot mount /Plugin/Sandbox");
            return false;
        }

        snprintf(formats_, sizeof(formats_), "%s/Config/ShaderFormats", engine);
        if (!FindWorker()) {
            Note("ShaderCompileWorker is neither next to Sandbox nor in the ShaderCompileWorker "
                 "target output (the Sandbox build builds it first as a prerequisite target)");
            return false;
        }

        if (!reflection_.registered || !shader::ResolveShaderNuggetRoot(StringView(), root_) ||
            !workRoot_.TryAssign(root_.View()) || !workRoot_.TryAppend("/") ||
            !workRoot_.TryAppend(shader::ShaderWorkerDirectoryName)) {
            Note("cannot prepare the shader compile workspace");
            return false;
        }

        shader::ShaderCompileManagerConfig config;
        NuggetSettingsFailure failure;
        if (shader::OpenShaderNuggetCache(cache_, allocator_, platform::PhysicalFileSystem(),
                                          root_.View(), &failure) &&
            scheduler_.Start()) {
            cacheOpen_ = true;
            config.cache = &cache_;
            config.scheduler = &scheduler_;
        }

        config.allocator = &allocator_;
        config.tree = &tree_;
        config.reflection = shader::ShaderKeyReflection{&reflection_.objects, reflection_.access,
                                                        &reflection_.types, &allocator_};
        config.workerExecutable = StringView(worker_);
        config.workRoot = workRoot_.View();
        auto memory = allocator_.Allocate(sizeof(shader::ShaderCompileManager),
                                          alignof(shader::ShaderCompileManager));
        if (!memory) {
            Note("out of memory");
            return false;
        }

        manager_ = ::new (memory.Value()) shader::ShaderCompileManager(config);
        if (!manager_->Start()) {
            Note("cannot start the shader compile workers");
            manager_->~ShaderCompileManager();
            (void)allocator_.Deallocate(manager_);
            manager_ = nullptr;
            return false;
        }

        prepareFailed_ = false;
        return true;
    }

    // 워커는 실행 파일 옆, 없으면 같은 구성으로 빌드한 ShaderCompileWorker 타깃 출력
    // (out/<도구>-<플랫폼>-shadercompileworker-<구성>/bin)이다. Sandbox는 Game 타깃이라 워커를 동반
    // 타깃으로 함께 빌드하지 못하고(동반 타깃은 Program 주 타깃만 — Docs/Design/BuildTargets
    // 3.1절), Sandbox 빌드가 선행 타깃으로 그 출력에 먼저 빌드한다(3.11절).
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

        // <out>/<이름>-sandbox-<구성>/bin/ → <out>/<이름>-shadercompileworker-<구성>/bin/
        constexpr char Marker[] = "-sandbox-";
        const char* marker = strstr(executable, Marker);
        if (marker == nullptr) {
            return false;
        }

        char sibling[PathBytes] = {};
        snprintf(sibling, sizeof(sibling), "%.*s-shadercompileworker-%s",
                 static_cast<int>(marker - executable), executable, marker + sizeof(Marker) - 1);
        return TryWorker(sibling);
    }

    bool TryWorker(const char* directory) noexcept {
        for (const char* name : {"ShaderCompileWorker.exe", "ShaderCompileWorker"}) {
            snprintf(worker_, sizeof(worker_), "%s%s", directory, name);
            bool exists = false;
            if (platform::PhysicalFileSystem().FileExists(worker_, strlen(worker_), exists) &&
                exists) {
                return true;
            }
        }

        return false;
    }

    const BuiltLibrary* Build(const render::ShaderProvisionRequest& request) noexcept {
        for (const BuiltLibrary& library : libraries_) {
            if (StringView(library.formatId) == request.formatId) {
                return &library;
            }
        }

        if (!Prepare()) {
            return nullptr;
        }

        auto memory = allocator_.Allocate(sizeof(shader::ShaderFormatSnapshot),
                                          alignof(shader::ShaderFormatSnapshot));
        if (!memory) {
            Note("out of memory");
            return nullptr;
        }

        auto* snapshot = ::new (memory.Value()) shader::ShaderFormatSnapshot(allocator_);
        if (!snapshots_.TryPushBack(snapshot)) {
            snapshot->~ShaderFormatSnapshot();
            (void)allocator_.Deallocate(snapshot);
            Note("out of memory");
            return nullptr;
        }

        const shader::ShaderFormatStatus loaded =
            shader::LoadShaderFormatSnapshot(formats_, request.formatId, *snapshot);
        if (!loaded) {
            Note("cannot load the shader format definition");
            return nullptr;
        }

        const shader::ShaderFormatSnapshot* formats[] = {snapshot};
        shader::ShaderLibraryCookDesc desc;
        desc.allocator = &allocator_;
        desc.registry = request.registry;
        desc.formats = formats;
        desc.library = request.library;
        // Development 공급자는 개발 전용 타입도 만든다(진단 버퍼 퍼뮤테이션 — ShaderSystem 7.2절
        // 12항, RenderBase 9.11절 6항의 D3D12 root 예산 시험).
        desc.includeDevelopment = true;
        desc.manager = manager_;
        shader::ShaderLibraryCookReport report(allocator_);
        if (!libraries_.TryReserve(libraries_.Size() + 1)) {
            Note("out of memory");
            return nullptr;
        }

        BuiltLibrary built(allocator_);
        if (!shader::CompileShaderLibrary(desc, report, built.bytes)) {
            Note("shader library compile failed");
            for (const shader::ShaderLibraryCookJobFailure& job : report.failures) {
                char line[256];
                snprintf(line, sizeof(line), "; %s permutation %u: ", job.typeName,
                         job.permutationId);
                Note(line);
                (void)diagnostics_.TryAppend(job.diagnostics.View().LeftBytesAtCodePoint(400));
                Log::Error(graphics::LogGraphics,
                           "Sandbox shader {Type} ({Format}) failed: {Diagnostics}", job.typeName,
                           job.formatId, job.diagnostics.CStr());
            }

            return nullptr;
        }

        snprintf(built.formatId, sizeof(built.formatId), "%.*s",
                 static_cast<int>(request.formatId.ByteCount()), request.formatId.Data());
        (void)libraries_.TryPushBack(Move(built));
        return &libraries_[libraries_.Size() - 1];
    }

    Allocator& allocator_;
    shader::ShaderSourceTree tree_;
    Reflection reflection_;
    NuggetSystemClock clock_;
    NuggetCache cache_;
    WorkerScheduler scheduler_;
    String root_;
    String workRoot_;
    Array<shader::ShaderFormatSnapshot*> snapshots_;
    Array<BuiltLibrary> libraries_;
    String diagnostics_;
    shader::ShaderCompileManager* manager_ = nullptr;
    char worker_[PathBytes] = {};
    char formats_[PathBytes] = {};
    bool cacheOpen_ = false;
    bool prepareFailed_ = false;
};

DevelopmentProvider* Provider() noexcept {
    static DevelopmentProvider provider(ProcessAllocator());
    return &provider;
}

#endif

render::ShaderSetDesc SetDesc(Allocator& allocator, graphics::Device& device) noexcept {
    render::ShaderSetDesc desc;
    desc.allocator = &allocator;
    desc.device = &device;
    desc.registry = Registry();
    desc.library = shader::ShaderLibraryKind::Engine;
    desc.formatId = FormatFor(device.Info().backend);
    return desc;
}

} // namespace

Span<const ShaderTypeDesc* const> AllTypes() noexcept {
    return Types;
}

DeviceShaders::DeviceShaders(Allocator& allocator, graphics::Device& device) noexcept
    : set_(SetDesc(allocator, device)) {
#if SANDBOX_SHADER_PROVIDER
    render::ShaderProvider* provider = Provider();
#else
    render::ShaderProvider* provider = nullptr;
#endif
    if (Registry() == nullptr) {
        return;
    }

    if (auto opened = set_.OpenFromProvider(provider); !opened) {
        return;
    }

    if (set_.WaitUntilSettled() == render::ShaderSetState::Ready) {
        active = &set_;
        active_ = true;
    }
}

DeviceShaders::~DeviceShaders() noexcept {
    if (active_) {
        active = nullptr;
    }

    set_.Retire();
}

bool DeviceShaders::IsReady() const noexcept {
    return active_;
}

StringView DeviceShaders::Problem() const noexcept {
    return Registry() == nullptr ? StringView("Sandbox shader types failed to register")
                                 : set_.ProblemDetail();
}

bool Has(const ShaderTypeDesc& type) noexcept {
    return active != nullptr && active->FindEntry(type.nameHash).IsOk();
}

render::ShaderSet* ActiveSet() noexcept {
    return active;
}

const shader::ShaderBindingLayout* Layout(const ShaderTypeDesc& type) noexcept {
    if (active == nullptr) {
        return nullptr;
    }

    auto entry = active->FindEntry(type.nameHash);
    return entry ? active->BindingLayout(entry.Value()) : nullptr;
}

Result<graphics::ShaderHandle> Get(const ShaderTypeDesc& type) noexcept {
    if (active == nullptr) {
        return Result<graphics::ShaderHandle>::Failure(ErrorCode::NotReady);
    }

    auto entry = active->FindEntry(type.nameHash);
    if (!entry) {
        return Result<graphics::ShaderHandle>::Failure(entry.Error());
    }

    return active->DeviceShader(entry.Value());
}

Result<graphics::PipelineHandle> CreateGraphicsPipeline(graphics::Device& device,
                                                        const ShaderTypeDesc& vertex,
                                                        const ShaderTypeDesc& pixel,
                                                        graphics::PipelineDesc desc) noexcept {
    auto vs = Get(vertex);
    if (!vs) {
        return Result<graphics::PipelineHandle>::Failure(vs.Error());
    }

    auto ps = Get(pixel);
    if (!ps) {
        return Result<graphics::PipelineHandle>::Failure(ps.Error());
    }

    desc.vertexShader = vs.Value();
    desc.pixelShader = ps.Value();
    return device.CreatePipeline(desc);
}

Result<graphics::PipelineHandle>
CreateComputePipeline(graphics::Device& device, const ShaderTypeDesc& compute,
                      graphics::ComputePipelineDesc desc) noexcept {
    auto cs = Get(compute);
    if (!cs) {
        return Result<graphics::PipelineHandle>::Failure(cs.Error());
    }

    desc.computeShader = cs.Value();
    return device.CreateComputePipeline(desc);
}

void ShutdownProvider() noexcept {
#if SANDBOX_SHADER_PROVIDER
    Provider()->Shutdown();
#endif
}

} // namespace sandbox::shaders
