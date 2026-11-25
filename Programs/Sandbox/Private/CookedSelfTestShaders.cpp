#include "CookedSelfTestShaders.h"

#include <AssetRuntime/CookedManifest.h>
#include <Core/FileRead.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <EngineAssets/EngineAssetsSubsystem.h>
#include <HostConfig/HostSettings.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <RenderBase/EngineShaderTypes.h>
#include <stdio.h>

// 쿠킹 엔진 셰이더 라이브러리 self-test의 셰이더 호스트(Docs/Design/ShaderSystem 16.1절 SSD48)다.
namespace sandbox {

using namespace rocks;

namespace {

// 경로 버퍼 byte 수다.
constexpr usize CookedPathBytes = 4096;

SystemAllocator& CookedAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

} // namespace

CookedSelfTestShaders& CookedSelfTestShaders::Get() noexcept {
    static CookedSelfTestShaders shaders(CookedAllocator());
    static bool prepared = false;
    if (!prepared) {
        prepared = true;
        shaders.Prepare();
    }

    return shaders;
}

CookedSelfTestShaders::CookedSelfTestShaders(Allocator& allocator) noexcept
    : allocator_(allocator), host_(allocator) {
}

bool CookedSelfTestShaders::Cooks(graphics::Backend backend) const noexcept {
    if (!ready_) {
        return false;
    }

    for (const graphics::Backend cooked : source_->Backends()) {
        if (cooked == backend) {
            return true;
        }
    }

    return false;
}

void CookedSelfTestShaders::Prepare() noexcept {
    char engine[CookedPathBytes] = {};
    auto found = FindEngineDirectory(Span<char>(engine, sizeof(engine)));
    if (!found) {
        snprintf(reason_, sizeof(reason_), "the engine folder was not found");
        return;
    }

    char executable[CookedPathBytes] = {};
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        snprintf(reason_, sizeof(reason_), "the executable path is not available");
        return;
    }

    const StringView profile = ResolveRuntimeProfile(StringView());
    String root(allocator_);
    if (!ResolveCookedRoot(StringView(), StringView(engine, found.Value()), profile,
                           ExecutableDirectoryOf(StringView(executable, executableBytes)), root)) {
        snprintf(reason_, sizeof(reason_), "the engine cooked root could not be resolved");
        return;
    }

    String path(allocator_);
    if (!path.TryAssign(root.View()) || !path.TryAppend("/") ||
        !path.TryAppend(CookedManifestFileName)) {
        snprintf(reason_, sizeof(reason_), "out of memory");
        return;
    }

    Array<uint8> bytes(allocator_);
    bool existed = false;
    if (!ReadWholeFile(platform::PhysicalFileSystem(), path.View(), CookedManifestMaxFileBytes,
                       bytes, existed) ||
        !existed) {
        snprintf(reason_, sizeof(reason_),
                 "no engine cook at %.*s (run asset-cook --engine --profile=%.*s)",
                 static_cast<int>(root.View().ByteCount()), root.View().Data(),
                 static_cast<int>(profile.ByteCount()), profile.Data());
        return;
    }

    AssetDiagnostic failure;
    auto manifest = ReadCookedManifest(allocator_, bytes.View(), &failure);
    if (!manifest) {
        snprintf(reason_, sizeof(reason_), "the engine cooked manifest is invalid (%s)",
                 AssetDiagnosticName(failure.code));
        return;
    }

    // 쿠커와 같은 엔진 모듈 타입이다(타입 집합 SHA가 쿠킹 라이브러리와 같아야 한다).
    if (!host_.Registry().Register(render::EngineShaderTypes()) ||
        !host_.Registry().RegisterVertexInputTypes(render::EngineVertexInputTypes()) ||
        !host_.Freeze()) {
        snprintf(reason_, sizeof(reason_), "the engine shader types could not be registered");
        return;
    }

    auto source = render::CookedManifestShaderLibraries::TryCreate(
        allocator_, platform::PhysicalFileSystem(), manifest.Value(), root.View());
    if (!source || !host_.AttachCookedLibraries(*source.Value())) {
        snprintf(reason_, sizeof(reason_), "the cooked shader library source could not be made");
        return;
    }

    source_ = Move(source.Value());
    ready_ = true;
}

} // namespace sandbox
