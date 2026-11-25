#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Platform/File.h>
#include <RenderBase/EngineShaderHost.h>

// 쿠킹 매니페스트 v2의 셰이더 라이브러리 원천이다(Docs/Design/ShaderSystem 12.3.1절 SSD14, 16.1절
// SSD47, AssetSystem 10.3절 8항). 만들 때 매니페스트의 backend 표(graphicsBackends·shaderFormats
// 순서)와 ShaderLibrary artifact 행을 복사하므로 매니페스트(EngineAssets Subsystem)보다 오래 살아도
// 된다. ReadLibrary는 `<쿠킹 루트>/` + AppendCookedArtifactPath의 파일을 읽어
// CheckCookedArtifactFile로 크기·SHA-256을 대조한다(다르면 CorruptPackage).
//
// 사용 예(엔진 전용 호스트 — 엔진 시작 뒤):
//   const EngineAssetsSubsystem& assets = ...;
//   auto source = render::CookedManifestShaderLibraries::TryCreate(
//       allocator, platform::PhysicalFileSystem(), assets.Manifest(), assets.CookedRoot());
//   (void)shaderHost.AttachCookedLibraries(*source.Value());
//   ... 렌더 lane ... shaderHost.DetachCookedLibraries(*source.Value());
//
// 스레드: 만든 뒤 Backends·Formats는 어느 스레드에서나 읽는다. ReadLibrary·Problem은 한 스레드에서
// 차례로 부른다(렌더 lane 시작 — EngineRenderResources::Open).
namespace rocks {

struct CookedManifest;

} // namespace rocks

namespace rocks::render {

class ROCKS_RENDERBASE_API CookedManifestShaderLibraries final : public CookedShaderLibrarySource {
public:
    // manifest의 셰이더 칸을 복사해 만든다. files와 allocator는 이 객체보다 오래 산다. cookedRoot는
    // 복사한다. 오류: cookedRoot가 비었음 InvalidArgument, 모르는 backend 이름(매니페스트 어휘 밖 —
    // 검증을 통과한 매니페스트에는 없다) InvalidArgument, 할당 실패.
    static Result<UniquePtr<CookedManifestShaderLibraries>>
    TryCreate(Allocator& allocator, platform::FileSystem& files, const CookedManifest& manifest,
              StringView cookedRoot) noexcept;

    ~CookedManifestShaderLibraries() noexcept;
    CookedManifestShaderLibraries(const CookedManifestShaderLibraries&) = delete;
    CookedManifestShaderLibraries& operator=(const CookedManifestShaderLibraries&) = delete;

    Span<const graphics::Backend> Backends() const noexcept override;
    Span<const StringView> Formats(graphics::Backend backend) const noexcept override;

    // 오류: 항목 없음 NotFound, 파일 없음·크기·SHA-256 불일치 InvalidArgument(Problem에
    // "CorruptPackage" 과 칸 ArtifactBytes·ArtifactSha256), 경로·읽기 오류는 그 오류, 할당 실패.
    Result<void> ReadLibrary(StringView formatId, shader::ShaderLibraryKind library,
                             Array<uint8>& out) noexcept override;
    StringView Problem() const noexcept override;

    // 복사한 셰이더 라이브러리 항목 수다(시험·진단).
    usize LibraryCount() const noexcept;

    // 내부 상태(backend 표·artifact 행 사본)다. 정의는 cpp에 있다.
    struct State;

    // TryCreate가 쓴다(UniquePtr::TryMake가 부를 수 있게 공개). 직접 부르지 않는다.
    CookedManifestShaderLibraries(platform::FileSystem& files, UniquePtr<State> state) noexcept;

private:
    // 설명 버퍼 byte 수다.
    static constexpr usize ProblemBytes = 512;

    platform::FileSystem& files_;
    UniquePtr<State> state_;
    char problem_[ProblemBytes] = {};
};

} // namespace rocks::render
