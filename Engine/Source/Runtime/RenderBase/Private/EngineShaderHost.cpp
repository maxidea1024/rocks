#include <RenderBase/EngineShaderHost.h>

#include <Core/Assert.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <ModuleLoader/HostServices.h>
#include <ShaderCore/ShaderVirtualPath.h>
#include <stdio.h>

// 엔진 호스트의 셰이더 연결(Docs/Design/RenderBase 9.12절 RR11)이다. 규칙은 공개 헤더 주석을
// 따른다.
namespace rocks::render {

namespace {

constexpr LogCategory LogEngineShaderHost{"RenderBase"};

} // namespace

EngineShaderHost::EngineShaderHost(Allocator& allocator) noexcept
    : allocator_(allocator), registry_(allocator), storage_(allocator), directories_(allocator) {
}

EngineShaderHost::~EngineShaderHost() noexcept {
    ROCKS_ASSERT(!attached_);
}

Result<void> EngineShaderHost::Attach(HostServices& services) noexcept {
    if (attached_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (auto registered = services.Register(registry_); !registered) {
        return registered;
    }

    if (auto registered = services.Register(*this); !registered) {
        (void)services.Unregister(registry_);
        return registered;
    }

    attached_ = true;
    return Result<void>::Success();
}

void EngineShaderHost::Detach(HostServices& services) noexcept {
    if (!attached_) {
        return;
    }

    (void)services.Unregister(*this);
    (void)services.Unregister(registry_);
    attached_ = false;
}

Result<void> EngineShaderHost::AddSourceDirectory(StringView virtualRoot,
                                                  StringView directory) noexcept {
    if (registry_.IsFrozen()) {
        return Result<void>::Failure(ErrorCode::ScopeClosed);
    }

    if (!shader::CheckShaderVirtualDirectory(virtualRoot) || directory.IsEmpty() ||
        EqualsIgnoringAsciiCase(virtualRoot, StringView(shader::EngineShaderVirtualRoot))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const ShaderSourceDirectory& existing : directories_) {
        if (EqualsIgnoringAsciiCase(existing.virtualRoot, virtualRoot)) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    if (directories_.Size() >= MaxShaderSourceDirectories) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    auto created = UniquePtr<DirectoryStorage>::TryMake(allocator_, allocator_);
    if (!created) {
        return Result<void>::Failure(created.Error());
    }

    DirectoryStorage& entry = *created.Value();
    if (auto assigned = entry.virtualRoot.TryAssign(virtualRoot); !assigned) {
        return assigned;
    }

    if (auto assigned = entry.directory.TryAssign(directory); !assigned) {
        return assigned;
    }

    if (auto reserved = storage_.TryReserve(storage_.Size() + 1); !reserved) {
        return reserved;
    }

    if (auto reserved = directories_.TryReserve(directories_.Size() + 1); !reserved) {
        return reserved;
    }

    const ShaderSourceDirectory view{entry.virtualRoot.View(), entry.directory.View()};
    (void)storage_.TryPushBack(Move(created.Value()));
    (void)directories_.TryPushBack(view);
    return Result<void>::Success();
}

Result<void> EngineShaderHost::Freeze() noexcept {
    if (registry_.IsFrozen()) {
        return Result<void>::Success();
    }

    if (auto frozen = registry_.Freeze(); !frozen) {
        const shader::ShaderTypeRegistrationFailure& failure = registry_.LastFailure();
        Log::Error(LogEngineShaderHost,
                   "Engine shader types could not be frozen ({Error}): {Type}: {Reason}",
                   ErrorCodeName(frozen.Error()), StringView(failure.typeName),
                   StringView(failure.reason));
        return frozen;
    }

    Log::Info(LogEngineShaderHost,
              "Engine shader types are frozen: {Types} shader types, {VertexInputs} vertex input "
              "types",
              static_cast<uint64>(registry_.Types().Size()),
              static_cast<uint64>(registry_.VertexInputTypes().Size()));
    return Result<void>::Success();
}

Result<void> EngineShaderHost::AttachProvider(ShaderProvider& provider) noexcept {
    platform::LockGuard lock(gate_);
    if (provider_ != nullptr && provider_ != &provider) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    provider_ = &provider;
    return Result<void>::Success();
}

void EngineShaderHost::DetachProvider(ShaderProvider& provider) noexcept {
    platform::LockGuard lock(gate_);
    if (provider_ == &provider) {
        provider_ = nullptr;
    }
}

ShaderProvider* EngineShaderHost::Provider() const noexcept {
    platform::LockGuard lock(gate_);
    return provider_;
}

Result<void> EngineShaderHost::AttachCookedLibraries(CookedShaderLibrarySource& source) noexcept {
    platform::LockGuard lock(gate_);
    if (cooked_ != nullptr && cooked_ != &source) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    cooked_ = &source;
    return Result<void>::Success();
}

void EngineShaderHost::DetachCookedLibraries(CookedShaderLibrarySource& source) noexcept {
    platform::LockGuard lock(gate_);
    if (cooked_ == &source) {
        cooked_ = nullptr;
    }
}

CookedShaderLibrarySource* EngineShaderHost::CookedLibraries() const noexcept {
    platform::LockGuard lock(gate_);
    return cooked_;
}

StringView EngineShaderFormat(graphics::Backend backend) noexcept {
    switch (backend) {
    case graphics::Backend::D3D12:
        return "D3D12_SM6";
    case graphics::Backend::D3D11:
        return "D3D11_SM5";
    case graphics::Backend::Vulkan:
        return "VULKAN_SM5";
    case graphics::Backend::OpenGL:
        // Windows 미리보기 형식이다. Android GLES31_ANDROID는 쿠킹 원천의 형식 선택과 함께다(9.12절
        // 한계 — Android에는 공급자가 없다).
        return "GLES31_PC";
    case graphics::Backend::Metal:
        return "METAL_SM5";
    }

    return StringView();
}

EngineRenderResources::EngineRenderResources(Allocator& allocator) noexcept
    : allocator_(allocator) {
}

EngineRenderResources::~EngineRenderResources() noexcept = default;

Result<void> EngineRenderResources::Fail(ErrorCode error, StringView format, const char* stage,
                                         StringView detail) noexcept {
    snprintf(problem_, sizeof(problem_), "%s: %.*s", stage, static_cast<int>(detail.ByteCount()),
             detail.Data());
    Log::Error(LogEngineShaderHost, "Engine render resources ({Format}) could not open: {Problem}",
               format, StringView(problem_));
    return Result<void>::Failure(error);
}

Result<void> EngineRenderResources::Open(graphics::Device& device,
                                         const EngineShaderHost& host) noexcept {
    if (ready_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 앞서 실패했거나 Retire한 집합은 여기서 부순다(lease는 주인이 이미 비웠다).
    set_.Reset();
    problem_[0] = '\0';
    cooked_ = false;
    const StringView format = EngineShaderFormat(device.Info().backend);
    if (!host.IsFrozen()) {
        return Fail(ErrorCode::NotReady, format, "shader type registry",
                    "the registry is not frozen (freeze it after the modules start and before "
                    "the render lane starts)");
    }

    if (format.IsEmpty()) {
        return Fail(ErrorCode::Unsupported, format, "shader format",
                    "the device backend has no engine shader format");
    }

    ShaderSetDesc desc;
    desc.allocator = &allocator_;
    desc.device = &device;
    desc.registry = &host.Registry();
    desc.library = shader::ShaderLibraryKind::Engine;
    desc.formatId = format;
    auto created = UniquePtr<ShaderSet>::TryMake(allocator_, desc);
    if (!created) {
        return Fail(created.Error(), format, "engine shader set", "out of memory");
    }

    set_ = Move(created.Value());
    ShaderSet& set = *set_;
    bool opened = false;
    if (CookedShaderLibrarySource* source = host.CookedLibraries()) {
        Array<uint8> bytes(allocator_);
        auto read = source->ReadLibrary(format, shader::ShaderLibraryKind::Engine, bytes);
        if (read) {
            (void)set.OpenCooked(Move(bytes));
            opened = true;
            cooked_ = true;
        } else if (read.Error() != ErrorCode::NotFound) {
            set.Retire();
            return Fail(read.Error(), format, "cooked shader library",
                        StringView(ErrorCodeName(read.Error())));
        }
    }

    if (!opened) {
        (void)set.OpenFromProvider(host.Provider());
    }

    const ShaderSetState state = set.WaitUntilSettled();
    if (state != ShaderSetState::Ready) {
        char detail[384];
        const StringView problemDetail = set.ProblemDetail();
        snprintf(detail, sizeof(detail), "%s (%.*s)", ShaderSetProblemName(set.Problem()),
                 static_cast<int>(problemDetail.ByteCount()), problemDetail.Data());
        const ErrorCode error = set.Problem() == ShaderSetProblem::NoProvider
                                    ? ErrorCode::UnsupportedCapability
                                    : ErrorCode::DependencyFailed;
        set.Retire();
        return Fail(error, format, "engine shader set", StringView(detail));
    }

    if (auto built = builtins_.Create(device, set); !built) {
        set.Retire();
        return Fail(built.Error(), format, "builtin resources",
                    StringView(builtins_.LastFailure()));
    }

    ready_ = true;
    return Result<void>::Success();
}

void EngineRenderResources::Retire(graphics::Device& device) noexcept {
    if (!set_) {
        return;
    }

    builtins_.Retire(device);
    set_->Retire();
    ready_ = false;
}

} // namespace rocks::render

ROCKS_DEFINE_HOST_SERVICE(rocks::render::EngineShaderHost)
