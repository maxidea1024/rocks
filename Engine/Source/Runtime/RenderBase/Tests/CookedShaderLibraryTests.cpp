#include "RecordingDevice.h"
#include "RegistryShaderSet.h"

#include <AssetRuntime/CookedManifest.h>
#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <Platform/Clock.h>
#include <RenderBase/CookedManifestShaderLibraries.h>
#include <RenderBase/CookedShaderFormat.h>
#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/EngineShaderTypes.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 쿠킹 라이브러리 런타임 열기 시험이다(Docs/Design/ShaderSystem 12.3.1절 SSD14·SSD15, 16.1절
// SSD47): 매니페스트 원천(backend·형식 순서 사본, 정상 읽기, 항목 없음 NotFound, 크기·SHA-256
// 불일치와 파일 없음 CorruptPackage), 형식 선택(장치 요구 미달·상한 미달 → 다음 형식, 모두 미달
// Unsupported, backend 없음 NotFound, 셰이더 없는 쿠킹 Unsupported, 장치 요구 비트), 엔진 렌더
// 자원의 쿠킹 열기와 손상·셰이더 없는 쿠킹 진단.
using namespace rocks;
using namespace rocks::render;
using rocks::render::testing::RecordingDevice;
using rocks::render::testing::RegistryShaderSet;

namespace cooked_shader_library_tests_file {

namespace {

SystemAllocator& Allocator_() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// 경로 버퍼 byte 수다.
constexpr usize TestPathBytes = 1024;
// 파일 읽기·경로 builder 인라인 byte 수다.
constexpr usize TestInlineBytes = 256;

// 임시 쿠킹 루트다(파괴하면 지운다).
class CookedRoot final {
public:
    CookedRoot() noexcept = default;
    CookedRoot(const CookedRoot&) = delete;
    CookedRoot& operator=(const CookedRoot&) = delete;

    ~CookedRoot() noexcept {
        if (length_ != 0) {
            (void)platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), path_, length_);
        }
    }

    bool Make(const char* tag) noexcept {
        char base[TestPathBytes];
        usize bytes = 0;
        if (!platform::GetStandardDirectory(platform::StandardDirectory::Temporary, base,
                                            sizeof(base), bytes)) {
            return false;
        }

        static uint32 serial = 0;
        const int count =
            snprintf(path_, sizeof(path_), "%s/rocks-cooked-%s-%llu-%u", base, tag,
                     static_cast<unsigned long long>(platform::MonotonicNanoseconds()), ++serial);
        if (count <= 0 || static_cast<usize>(count) >= sizeof(path_)) {
            return false;
        }

        length_ = static_cast<usize>(count);
        return static_cast<bool>(
            platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path_, length_));
    }

    StringView View() const noexcept {
        return StringView(path_, length_);
    }

    // artifact 파일 경로를 out에 둔다.
    bool ArtifactPath(const CookedManifestArtifact& artifact,
                      char (&out)[TestPathBytes]) const noexcept {
        StringBuilder<TestInlineBytes> path(Allocator_());
        if (!path.TryAppendFormat("{}/", View()) || !AppendCookedArtifactPath(path, artifact) ||
            path.View().ByteCount() + 1 > sizeof(out)) {
            return false;
        }

        memcpy(out, path.View().Data(), path.View().ByteCount());
        out[path.View().ByteCount()] = '\0';
        return true;
    }

    // artifact 파일을 bytes로 쓴다(Shaders 디렉터리를 만든다).
    bool Write(const CookedManifestArtifact& artifact, Span<const uint8> bytes) const noexcept {
        char path[TestPathBytes];
        char shaders[TestPathBytes];
        snprintf(shaders, sizeof(shaders), "%s/Shaders", path_);
        if (!ArtifactPath(artifact, path) ||
            !platform::MakeDirectoryTree(platform::PhysicalFileSystem(), shaders,
                                         strlen(shaders))) {
            return false;
        }

        platform::File file;
        if (!file.Open(path, platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
        return static_cast<bool>(file.Close()) && static_cast<bool>(written.status);
    }

private:
    char path_[TestPathBytes] = {};
    usize length_ = 0;
};

bool AddBackend(CookedManifest& manifest, StringView name,
                std::initializer_list<StringView> formats) noexcept {
    if (!manifest.backends.TryEmplaceBack(Allocator_())) {
        return false;
    }

    CookedManifestBackend& backend = manifest.backends[manifest.backends.Size() - 1];
    if (!backend.name.TryAssign(name)) {
        return false;
    }

    for (const StringView format : formats) {
        String copy(Allocator_());
        if (!copy.TryAssign(format) || !backend.formats.TryPushBack(Move(copy))) {
            return false;
        }
    }

    return true;
}

// Engine 라이브러리 항목을 더하고(정렬 순서로 넣는다) 파일을 쓴다.
bool AddLibrary(CookedManifest& manifest, const CookedRoot& root, StringView formatId,
                Span<const uint8> bytes) noexcept {
    if (!manifest.artifacts.TryEmplaceBack(Allocator_())) {
        return false;
    }

    CookedManifestArtifact& artifact = manifest.artifacts[manifest.artifacts.Size() - 1];
    artifact.kind = CookArtifactKind::ShaderLibrary;
    artifact.bytes = bytes.Size();
    artifact.sha256 = Sha256::Hash(bytes);
    return artifact.name.TryAssign(CookedArtifactEngineName) &&
           artifact.formatId.TryAssign(formatId) && root.Write(artifact, bytes);
}

// facts만 다른 빈 라이브러리 byte다(형식 선택은 머리만 본다).
bool BuildFactsLibrary(StringView formatId, uint32 renderTargets, uint32 requirements,
                       Array<uint8>& out) noexcept {
    shader::ShaderLibraryBuildDesc build;
    build.kind = shader::ShaderLibraryKind::Engine;
    if (!shader::SetShaderFormatFactsId(build.facts, formatId)) {
        return false;
    }

    build.facts.backend = shader::ShaderBackend::D3D12;
    build.facts.limits.renderTargets = renderTargets;
    build.facts.deviceRequirements = requirements;
    return static_cast<bool>(shader::BuildShaderLibrary(build, Allocator_(), out));
}

bool RegisterEngine(shader::ShaderTypeRegistry& registry) noexcept {
    return registry.Register(EngineShaderTypes()) &&
           registry.RegisterVertexInputTypes(EngineVertexInputTypes());
}

// 바이트 하나를 뒤집어 파일을 다시 쓴다(손상 흉내).
bool Corrupt(const CookedRoot& root, const CookedManifestArtifact& artifact,
             Array<uint8> bytes) noexcept {
    bytes[bytes.Size() / 2] ^= 0x5A;
    CookedManifestArtifact same(Allocator_());
    same.kind = artifact.kind;
    same.bytes = artifact.bytes;
    same.sha256 = artifact.sha256;
    return same.name.TryAssign(artifact.name.View()) &&
           same.formatId.TryAssign(artifact.formatId.View()) && root.Write(same, bytes.View());
}

} // namespace

TEST(RenderBase_CookedManifestShaderLibrariesReadsAndChecks) {
    CookedRoot root;
    REQUIRE(root.Make("source"));
    Array<uint8> d3d12(Allocator_());
    Array<uint8> vulkan(Allocator_());
    REQUIRE(BuildFactsLibrary("D3D12_SM6", 0, 0, d3d12));
    REQUIRE(BuildFactsLibrary("VULKAN_SM5", 0, 0, vulkan));
    CookedManifest manifest(Allocator_());
    REQUIRE(AddBackend(manifest, "D3D12", {"D3D12_SM6"}));
    REQUIRE(AddBackend(manifest, "Vulkan", {"VULKAN_SM5"}));
    REQUIRE(AddLibrary(manifest, root, "D3D12_SM6", d3d12.View()));
    REQUIRE(AddLibrary(manifest, root, "VULKAN_SM5", vulkan.View()));
    // 셰이더 라이브러리가 아닌 종류는 없다(지금 어휘는 ShaderLibrary 하나 — 사본은 그것만 담는다).
    auto created = CookedManifestShaderLibraries::TryCreate(
        Allocator_(), platform::PhysicalFileSystem(), manifest, root.View());
    REQUIRE(created);
    CookedManifestShaderLibraries& source = *created.Value();
    CHECK(CookedManifestShaderLibraries::TryCreate(Allocator_(), platform::PhysicalFileSystem(),
                                                   manifest, StringView())
              .Error() == ErrorCode::InvalidArgument);
    // 매니페스트가 사라져도 사본으로 답한다.
    manifest.backends.Clear();
    REQUIRE(source.Backends().Size() == 2);
    CHECK(source.Backends()[0] == graphics::Backend::D3D12 &&
          source.Backends()[1] == graphics::Backend::Vulkan);
    REQUIRE(source.Formats(graphics::Backend::Vulkan).Size() == 1);
    CHECK(source.Formats(graphics::Backend::Vulkan)[0] == StringView("VULKAN_SM5"));
    CHECK(source.Formats(graphics::Backend::Metal).IsEmpty() && source.LibraryCount() == 2);

    Array<uint8> read(Allocator_());
    REQUIRE(source.ReadLibrary("D3D12_SM6", shader::ShaderLibraryKind::Engine, read));
    CHECK(read.Size() == d3d12.Size() && memcmp(read.Data(), d3d12.Data(), read.Size()) == 0);
    // 항목이 없으면 NotFound다(Project 라이브러리·없는 형식).
    CHECK(source.ReadLibrary("D3D12_SM6", shader::ShaderLibraryKind::Project, read).Error() ==
          ErrorCode::NotFound);
    CHECK(source.ReadLibrary("D3D11_SM5", shader::ShaderLibraryKind::Engine, read).Error() ==
          ErrorCode::NotFound);

    // SHA-256 불일치(같은 크기) → CorruptPackage(ArtifactSha256).
    const CookedManifestArtifact& d3d12Artifact = manifest.artifacts[0];
    REQUIRE(Corrupt(root, d3d12Artifact, Move(d3d12)));
    const Result<void> corrupt =
        source.ReadLibrary("D3D12_SM6", shader::ShaderLibraryKind::Engine, read);
    CHECK(!corrupt && corrupt.Error() == ErrorCode::CorruptData);
    CHECK(source.Problem().Contains(StringView("CorruptPackage (ArtifactSha256)")) &&
          read.IsEmpty());

    // 크기 불일치(매니페스트보다 큼) → CorruptPackage(ArtifactBytes).
    Array<uint8> longer(Allocator_());
    REQUIRE(longer.TryAppend(vulkan.View()));
    REQUIRE(longer.TryPushBack(0));
    REQUIRE(root.Write(manifest.artifacts[1], longer.View()));
    CHECK(source.ReadLibrary("VULKAN_SM5", shader::ShaderLibraryKind::Engine, read).Error() ==
          ErrorCode::CorruptData);
    CHECK(source.Problem().Contains(StringView("CorruptPackage (ArtifactBytes)")));

    // 크기 불일치(작음)도 ArtifactBytes다.
    REQUIRE(root.Write(manifest.artifacts[1], Span<const uint8>(vulkan.Data(), vulkan.Size() - 1)));
    CHECK(source.ReadLibrary("VULKAN_SM5", shader::ShaderLibraryKind::Engine, read).Error() ==
          ErrorCode::CorruptData);
    CHECK(source.Problem().Contains(StringView("CorruptPackage (ArtifactBytes)")));

    // 항목은 있는데 파일이 없으면 손상이다(공급자로 넘기지 않는다 — NotFound가 아니다).
    char path[TestPathBytes];
    REQUIRE(root.ArtifactPath(manifest.artifacts[1], path));
    REQUIRE(platform::PhysicalFileSystem().Delete(path, strlen(path)));
    CHECK(source.ReadLibrary("VULKAN_SM5", shader::ShaderLibraryKind::Engine, read).Error() ==
          ErrorCode::CorruptData);
    CHECK(source.Problem().Contains(StringView("missing")));
}

TEST(RenderBase_SelectCookedShaderFormatFollowsManifestOrder) {
    CookedRoot root;
    REQUIRE(root.Make("select"));
    // D3D12 선호 순서: A(장치 요구 SM 6.6), B(render target 8), C(맞음).
    Array<uint8> a(Allocator_());
    Array<uint8> b(Allocator_());
    Array<uint8> c(Allocator_());
    REQUIRE(BuildFactsLibrary("FORMAT_A", 0, shader::DeviceRequiresD3D12ShaderModel6_6, a));
    REQUIRE(BuildFactsLibrary("FORMAT_B", 8, 0, b));
    REQUIRE(BuildFactsLibrary("FORMAT_C", 0, 0, c));
    CookedManifest manifest(Allocator_());
    REQUIRE(AddBackend(manifest, "D3D12", {"FORMAT_A", "FORMAT_B", "FORMAT_C"}));
    REQUIRE(AddLibrary(manifest, root, "FORMAT_A", a.View()));
    REQUIRE(AddLibrary(manifest, root, "FORMAT_B", b.View()));
    REQUIRE(AddLibrary(manifest, root, "FORMAT_C", c.View()));
    auto created = CookedManifestShaderLibraries::TryCreate(
        Allocator_(), platform::PhysicalFileSystem(), manifest, root.View());
    REQUIRE(created);
    CookedManifestShaderLibraries& source = *created.Value();
    RecordingDevice device(Allocator_());
    CookedShaderFormatChoice choice(Allocator_());

    // 장치 요구도 render target도 모자라면 C다(앞 둘의 이유를 남기지 않는다 — 성공).
    REQUIRE(SelectCookedShaderFormat(source, device, shader::ShaderLibraryKind::Engine, choice));
    CHECK(choice.FormatId() == StringView("FORMAT_C") && choice.Problem().IsEmpty());
    CHECK(choice.bytes.Size() == c.Size());

    // render target 8을 받으면 B, SM 6.6까지 채우면 A다(앞에서부터 첫 형식).
    device.MutableCaps().limits.colorAttachments = 8;
    REQUIRE(SelectCookedShaderFormat(source, device, shader::ShaderLibraryKind::Engine, choice));
    CHECK(choice.FormatId() == StringView("FORMAT_B"));
    device.MutableCaps().features.shaderDeviceRequirements =
        shader::DeviceRequiresD3D12ShaderModel6_6;
    REQUIRE(SelectCookedShaderFormat(source, device, shader::ShaderLibraryKind::Engine, choice));
    CHECK(choice.FormatId() == StringView("FORMAT_A"));

    // 한도 대조 함수: 형식 상한이 장치 단계 한도를 넘으면 그 이유다.
    shader::ShaderFormatFacts facts;
    facts.limits.sampledTextures = 16;
    CHECK(CheckShaderFormatFitsDevice(facts, graphics::Backend::D3D12, device.Caps()) != nullptr);
    device.MutableCaps().limits.vertex.sampledTextures = 16;
    device.MutableCaps().limits.pixel.sampledTextures = 16;
    device.MutableCaps().limits.compute.sampledTextures = 16;
    CHECK(CheckShaderFormatFitsDevice(facts, graphics::Backend::D3D12, device.Caps()) == nullptr);
    facts.limits.storageImages = 8;
    facts.limits.uavStages = shader::ShaderStageCompute;
    CHECK(CheckShaderFormatFitsDevice(facts, graphics::Backend::D3D12, device.Caps()) != nullptr);
    device.MutableCaps().limits.compute.storageImages = 8;
    CHECK(CheckShaderFormatFitsDevice(facts, graphics::Backend::D3D12, device.Caps()) == nullptr);
    facts.limits.uavTotal = 8;
    device.MutableCaps().limits.pipelineUavs = 4;
    CHECK(CheckShaderFormatFitsDevice(facts, graphics::Backend::D3D12, device.Caps()) != nullptr);

    // 맞는 형식이 없으면 Unsupported와 형식마다 이유다.
    device.MutableCaps() = graphics::DeviceCaps{};
    CookedManifest strict(Allocator_());
    REQUIRE(AddBackend(strict, "D3D12", {"FORMAT_A", "FORMAT_B"}));
    REQUIRE(AddLibrary(strict, root, "FORMAT_A", a.View()));
    REQUIRE(AddLibrary(strict, root, "FORMAT_B", b.View()));
    auto strictSource = CookedManifestShaderLibraries::TryCreate(
        Allocator_(), platform::PhysicalFileSystem(), strict, root.View());
    REQUIRE(strictSource);
    CHECK(SelectCookedShaderFormat(*strictSource.Value(), device, shader::ShaderLibraryKind::Engine,
                                   choice)
              .Error() == ErrorCode::Unsupported);
    CHECK(choice.Problem().Contains(StringView("FORMAT_A: the device does not meet")) &&
          choice.Problem().Contains(StringView("FORMAT_B: render targets")) &&
          choice.bytes.IsEmpty());

    // 장치 backend가 쿠킹에 없으면 NotFound(공급자 경로)다.
    CookedManifest vulkanOnly(Allocator_());
    REQUIRE(AddBackend(vulkanOnly, "Vulkan", {"VULKAN_SM5"}));
    auto vulkanSource = CookedManifestShaderLibraries::TryCreate(
        Allocator_(), platform::PhysicalFileSystem(), vulkanOnly, root.View());
    REQUIRE(vulkanSource);
    CHECK(SelectCookedShaderFormat(*vulkanSource.Value(), device, shader::ShaderLibraryKind::Engine,
                                   choice)
              .Error() == ErrorCode::NotFound);

    // 셰이더 없는 쿠킹(backend 0)은 Unsupported "this cook has no shader formats"다.
    CookedManifest shaderless(Allocator_());
    auto emptySource = CookedManifestShaderLibraries::TryCreate(
        Allocator_(), platform::PhysicalFileSystem(), shaderless, root.View());
    REQUIRE(emptySource);
    CHECK(emptySource.Value()->Backends().IsEmpty());
    CHECK(SelectCookedShaderFormat(*emptySource.Value(), device, shader::ShaderLibraryKind::Engine,
                                   choice)
              .Error() == ErrorCode::Unsupported);
    CHECK(choice.Problem() == StringView("this cook has no shader formats"));
}

TEST(RenderBase_EngineRenderResourcesOpenCookedManifest) {
    CookedRoot root;
    REQUIRE(root.Make("resources"));
    RecordingDevice device(Allocator_());
    EngineShaderHost host(Allocator_());
    REQUIRE(RegisterEngine(host.Registry()));
    REQUIRE(host.Freeze());
    Array<uint8> library(Allocator_());
    REQUIRE(RegistryShaderSet::BuildLibrary(Allocator_(), host.Registry(), library));
    CookedManifest manifest(Allocator_());
    REQUIRE(AddBackend(manifest, "D3D12", {"D3D12_SM6"}));
    REQUIRE(AddLibrary(manifest, root, "D3D12_SM6", library.View()));
    auto created = CookedManifestShaderLibraries::TryCreate(
        Allocator_(), platform::PhysicalFileSystem(), manifest, root.View());
    REQUIRE(created);
    REQUIRE(host.AttachCookedLibraries(*created.Value()));
    {
        EngineRenderResources resources(Allocator_());
        REQUIRE(resources.Open(device, host));
        CHECK(resources.OpenedFromCookedLibrary() && resources.FormatId() == "D3D12_SM6");
        CHECK(resources.Builtins().IsReady());
        resources.Retire(device);
    }

    // 손상(SHA-256 불일치)은 공급자로 넘기지 않는 실패이고 진단이 CorruptPackage를 말한다.
    REQUIRE(Corrupt(root, manifest.artifacts[0], Move(library)));
    {
        EngineRenderResources resources(Allocator_());
        CHECK(resources.Open(device, host).Error() == ErrorCode::CorruptData);
        CHECK(resources.Problem().Contains(StringView("CorruptPackage (ArtifactSha256)")));
        CHECK(!resources.IsReady());
    }

    host.DetachCookedLibraries(*created.Value());

    // 셰이더 없는 쿠킹이면 장치 셰이더 집합을 만들지 않고 Unsupported다.
    CookedManifest shaderless(Allocator_());
    auto empty = CookedManifestShaderLibraries::TryCreate(
        Allocator_(), platform::PhysicalFileSystem(), shaderless, root.View());
    REQUIRE(empty);
    REQUIRE(host.AttachCookedLibraries(*empty.Value()));
    {
        EngineRenderResources resources(Allocator_());
        CHECK(resources.Open(device, host).Error() == ErrorCode::Unsupported);
        CHECK(resources.Problem().Contains(StringView("this cook has no shader formats")));
    }

    host.DetachCookedLibraries(*empty.Value());
    CHECK(device.WaitIdle());
    CHECK(device.PendingRetireCount() == 0);
}

} // namespace cooked_shader_library_tests_file
