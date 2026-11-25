#include <RenderBase/CookedManifestShaderLibraries.h>

#include <AssetRuntime/CookedManifest.h>
#include <Core/FileRead.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <stdio.h>

// 쿠킹 매니페스트 v2의 셰이더 라이브러리 원천(Docs/Design/ShaderSystem 16.1절 SSD47)이다. 규칙은
// 공개 헤더 주석을 따른다.
namespace rocks::render {

namespace cooked_manifest_detail {

// 경로 builder의 인라인 byte 수다.
constexpr usize PathInlineBytes = 512;

// 매니페스트 backend 이름(고정 어휘 — AssetSystem 10.3절 1항)의 장치 backend다.
bool ParseBackend(StringView name, graphics::Backend& out) noexcept {
    const struct {
        const char* name;
        graphics::Backend backend;
    } names[] = {{"D3D12", graphics::Backend::D3D12},
                 {"D3D11", graphics::Backend::D3D11},
                 {"Vulkan", graphics::Backend::Vulkan},
                 {"Metal", graphics::Backend::Metal},
                 {"OpenGL", graphics::Backend::OpenGL}};
    for (const auto& entry : names) {
        if (name == StringView(entry.name)) {
            out = entry.backend;
            return true;
        }
    }

    return false;
}

const char* ArtifactSectionName(uint32 section) noexcept {
    if (section == static_cast<uint32>(CookedManifestField::ArtifactBytes)) {
        return "ArtifactBytes";
    }

    if (section == static_cast<uint32>(CookedManifestField::ArtifactSha256)) {
        return "ArtifactSha256";
    }

    return "Artifact";
}

// backend 하나의 형식 목록 사본이다(views는 names를 가리킨다 — names를 다 채운 뒤 만든다).
struct BackendFormats {
    explicit BackendFormats(Allocator& allocator) noexcept : names(allocator), views(allocator) {
    }

    BackendFormats(BackendFormats&&) noexcept = default;
    BackendFormats& operator=(BackendFormats&&) noexcept = default;

    Array<String> names;
    Array<StringView> views;
};

} // namespace cooked_manifest_detail

struct CookedManifestShaderLibraries::State {
    explicit State(Allocator& allocator) noexcept
        : allocator(allocator), root(allocator), backends(allocator), formats(allocator),
          artifacts(allocator) {
    }

    Allocator& allocator;
    String root;
    Array<graphics::Backend> backends;
    Array<cooked_manifest_detail::BackendFormats> formats;
    Array<CookedManifestArtifact> artifacts;
};

Result<UniquePtr<CookedManifestShaderLibraries>>
CookedManifestShaderLibraries::TryCreate(Allocator& allocator, platform::FileSystem& files,
                                         const CookedManifest& manifest,
                                         StringView cookedRoot) noexcept {
    using Out = Result<UniquePtr<CookedManifestShaderLibraries>>;
    using namespace cooked_manifest_detail;
    if (cookedRoot.IsEmpty()) {
        return Out::Failure(ErrorCode::InvalidArgument);
    }

    auto created = UniquePtr<State>::TryMake(allocator, allocator);
    if (!created) {
        return Out::Failure(created.Error());
    }

    State& state = *created.Value();
    if (auto assigned = state.root.TryAssign(cookedRoot); !assigned) {
        return Out::Failure(assigned.Error());
    }

    for (const CookedManifestBackend& backend : manifest.Backends()) {
        graphics::Backend parsed = graphics::Backend::D3D12;
        if (!ParseBackend(backend.name.View(), parsed)) {
            return Out::Failure(ErrorCode::InvalidArgument);
        }

        if (!state.backends.TryPushBack(parsed) || !state.formats.TryEmplaceBack(allocator)) {
            return Out::Failure(ErrorCode::OutOfMemory);
        }

        BackendFormats& formats = state.formats[state.formats.Size() - 1];
        for (const String& format : backend.formats) {
            String copy(allocator);
            if (!copy.TryAssign(format.View()) || !formats.names.TryPushBack(Move(copy))) {
                return Out::Failure(ErrorCode::OutOfMemory);
            }
        }

        for (const String& name : formats.names) {
            if (!formats.views.TryPushBack(name.View())) {
                return Out::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    for (const CookedManifestArtifact& artifact : manifest.Artifacts()) {
        if (artifact.kind != CookArtifactKind::ShaderLibrary) {
            continue;
        }

        if (!state.artifacts.TryEmplaceBack(allocator)) {
            return Out::Failure(ErrorCode::OutOfMemory);
        }

        CookedManifestArtifact& copy = state.artifacts[state.artifacts.Size() - 1];
        copy.kind = artifact.kind;
        copy.bytes = artifact.bytes;
        copy.sha256 = artifact.sha256;
        copy.cookKey = artifact.cookKey;
        if (!copy.name.TryAssign(artifact.name.View()) ||
            !copy.formatId.TryAssign(artifact.formatId.View())) {
            return Out::Failure(ErrorCode::OutOfMemory);
        }
    }

    return UniquePtr<CookedManifestShaderLibraries>::TryMake(allocator, files,
                                                             Move(created.Value()));
}

CookedManifestShaderLibraries::CookedManifestShaderLibraries(platform::FileSystem& files,
                                                             UniquePtr<State> state) noexcept
    : files_(files), state_(Move(state)) {
}

CookedManifestShaderLibraries::~CookedManifestShaderLibraries() noexcept = default;

Span<const graphics::Backend> CookedManifestShaderLibraries::Backends() const noexcept {
    return state_->backends.View();
}

Span<const StringView>
CookedManifestShaderLibraries::Formats(graphics::Backend backend) const noexcept {
    for (usize index = 0; index < state_->backends.Size(); ++index) {
        if (state_->backends[index] == backend) {
            return state_->formats[index].views.View();
        }
    }

    return Span<const StringView>();
}

Result<void> CookedManifestShaderLibraries::ReadLibrary(StringView formatId,
                                                        shader::ShaderLibraryKind library,
                                                        Array<uint8>& out) noexcept {
    using namespace cooked_manifest_detail;
    problem_[0] = '\0';
    out.Clear();
    const StringView name = library == shader::ShaderLibraryKind::Engine
                                ? CookedArtifactEngineName
                                : CookedArtifactProjectName;
    const CookedManifestArtifact* artifact = nullptr;
    for (const CookedManifestArtifact& candidate : state_->artifacts) {
        if (candidate.name.View() == name && candidate.formatId.View() == formatId) {
            artifact = &candidate;
            break;
        }
    }

    if (artifact == nullptr) {
        snprintf(problem_, sizeof(problem_), "the cook has no %.*s %.*s shader library",
                 static_cast<int>(name.ByteCount()), name.Data(),
                 static_cast<int>(formatId.ByteCount()), formatId.Data());
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    StringBuilder<PathInlineBytes> path(state_->allocator);
    if (auto made = path.TryAppendFormat("{}/", state_->root.View()); !made) {
        return made;
    }

    if (auto made = AppendCookedArtifactPath(path, *artifact); !made) {
        return made;
    }

    bool existed = false;
    const Result<void> read = ReadWholeFile(files_, path.View(), artifact->bytes, out, existed);
    const auto corrupt = [&](const char* section, const char* what) noexcept {
        snprintf(problem_, sizeof(problem_), "CorruptPackage (%s): %s: %.*s", section, what,
                 static_cast<int>(path.View().ByteCount()), path.View().Data());
        out.Clear();
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    };
    if (!read) {
        // 매니페스트 크기보다 크면 크기 불일치다.
        if (read.Error() == ErrorCode::CapacityExceeded) {
            return corrupt("ArtifactBytes", "the shader library is larger than the manifest says");
        }

        snprintf(problem_, sizeof(problem_), "the shader library could not be read (%s): %.*s",
                 ErrorCodeName(read.Error()), static_cast<int>(path.View().ByteCount()),
                 path.View().Data());
        return Result<void>::Failure(read.Error());
    }

    // 항목은 있는데 파일이 없으면 쿠킹 결과 손상이다(공급자로 넘기지 않는다).
    if (!existed) {
        return corrupt("ArtifactBytes", "the shader library file is missing");
    }

    AssetDiagnostic failure;
    if (auto checked = CheckCookedArtifactFile(*artifact, out.View(), &failure); !checked) {
        return corrupt(ArtifactSectionName(failure.section),
                       "the shader library differs from the cooked manifest");
    }

    return Result<void>::Success();
}

StringView CookedManifestShaderLibraries::Problem() const noexcept {
    return StringView(problem_);
}

usize CookedManifestShaderLibraries::LibraryCount() const noexcept {
    return state_->artifacts.Size();
}

} // namespace rocks::render
