#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Module/HostServiceKey.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <Platform/Sync.h>
#include <RenderBase/BuiltinResources.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderLibrary.h>
#include <ShaderCore/ShaderTypeRegistry.h>

// 엔진 호스트의 셰이더 연결이다(Docs/Design/RenderBase 9.12절 RR11).
//
// - EngineShaderHost: 엔진 호스트가 하나 만드는 호스트 서비스다. 셰이더 타입 등록부(각 모듈이
//   Startup에서 등록 — ShaderSystem SSD3), 원문 디렉터리 표(UE AddShaderSourceDirectoryMapping
//   대응 — 공급자만 읽는다), 공급자 자리(SX14), 쿠킹 라이브러리 원천 자리를 가진다.
// - EngineRenderResources: 렌더 lane 시작 단계에서 엔진 셰이더 집합과 내장 자원 팩을 연다.
// - CookedShaderLibrarySource: 쿠킹 라이브러리(매니페스트 v2·.rshl) 원천 interface다. 구현은
//   CookedManifestShaderLibraries(RenderBase/CookedManifestShaderLibraries.h)다.
//
// 사용 예(엔진 호스트):
//   render::EngineShaderHost shaders(allocator);
//   (void)shaders.Registry().Register(MyExecutableShaderTypes);     // 실행 파일 자신의 타입
//   (void)shaders.AddSourceDirectory("/Plugin/Sandbox", directory);
//   (void)shaders.Attach(loader.Services());          // StartPhase(PostConfigInit) 전
//   ... 모듈 시작(Engine::Initialize — 각 모듈이 등록, 개발 구성은 공급자 모듈이 붙는다) ...
//   if (!shaders.Freeze()) { ... }                     // 렌더 lane 시작 전
//   ... 렌더 lane(처리기 OnLaneStart가 EngineRenderResources::Open) ... 엔진 종료 ...
//   shaders.Detach(loader.Services());                 // 모든 묶음 종료 뒤
//
// 사용 예(렌더 lane 처리기):
//   Result<void> OnLaneStart(render::RenderLaneContext& context) noexcept override {
//       return resources_.Open(context.Device(), shaders_);
//   }
//   void OnLaneStop(render::RenderLaneContext& context) noexcept override {
//       resources_.Retire(context.Device());
//   }
namespace rocks {

class HostServices;

} // namespace rocks

namespace rocks::render {

// 원문 디렉터리 표의 최대 항목 수다.
inline constexpr usize MaxShaderSourceDirectories = 16;

// 원문 디렉터리 하나다(가상 루트 → 디스크 폴더).
struct ShaderSourceDirectory {
    StringView virtualRoot;
    StringView directory;
};

// 쿠킹 라이브러리 원천이다(9.12절 3항, ShaderSystem 16.1절 SSD47). 호스트가 AttachCookedLibraries로
// 붙인다. 구현은 매니페스트 v2를 읽는 CookedManifestShaderLibraries다(시험은 가짜 원천).
// EngineRenderResources::Open이 SelectCookedShaderFormat으로 장치 형식을 고를 때 쓴다. 모든 호출은
// 렌더 lane 스레드(또는 장치 소유 스레드) 하나에서 차례로 온다.
class CookedShaderLibrarySource {
public:
    // 쿠킹한 backend 순서(매니페스트 머리 graphicsBackends)다. 빈 목록이면 셰이더 없는 쿠킹이다.
    // 원천이 사는 동안 유효하다.
    virtual Span<const graphics::Backend> Backends() const noexcept = 0;

    // backend의 선호 순서 형식 id(매니페스트 머리 shaderFormats)다. 쿠킹에 그 backend가 없으면 빈
    // 목록이다. 원천이 사는 동안 유효하다.
    virtual Span<const StringView> Formats(graphics::Backend backend) const noexcept = 0;

    // formatId·library의 라이브러리 byte를 out에 담는다. 그 항목이 없으면 NotFound(형식 선택이 다음
    // 형식으로 넘어간다), 크기·SHA-256 불일치·파일 없음은 손상(InvalidArgument — Problem에
    // CorruptPackage 설명), 읽기 실패는 그 오류다.
    virtual Result<void> ReadLibrary(StringView formatId, shader::ShaderLibraryKind library,
                                     Array<uint8>& out) noexcept = 0;

    // 마지막 ReadLibrary 실패의 영어 설명이다(없으면 빈 값). 다음 ReadLibrary까지 유효하다.
    virtual StringView Problem() const noexcept = 0;

protected:
    ~CookedShaderLibrarySource() = default;
};

class ROCKS_RENDERBASE_API EngineShaderHost final {
public:
    // allocator는 등록부·표 저장소용이며 이 객체보다 오래 살아야 한다.
    explicit EngineShaderHost(Allocator& allocator) noexcept;
    // 서비스에서 떼어진 뒤 파괴한다(Detach). 붙은 채 파괴하면 assert다.
    ~EngineShaderHost() noexcept;
    EngineShaderHost(const EngineShaderHost&) = delete;
    EngineShaderHost& operator=(const EngineShaderHost&) = delete;

    // 등록부와 이 객체를 호스트 서비스로 올린다. 오류: 이미 붙음 Busy, 서비스 표의 오류(그 형식이
    // 이미 등록됨 AlreadyRegistered 등 — 그때는 아무것도 올리지 않는다).
    Result<void> Attach(HostServices& services) noexcept;

    // 서비스에서 뗀다. 붙어 있지 않으면 아무 일도 하지 않는다.
    void Detach(HostServices& services) noexcept;

    bool IsAttached() const noexcept {
        return attached_;
    }

    // 셰이더 타입 등록부다. Freeze 전에는 실행 파일도 직접 등록할 수 있다.
    shader::ShaderTypeRegistry& Registry() noexcept {
        return registry_;
    }

    const shader::ShaderTypeRegistry& Registry() const noexcept {
        return registry_;
    }

    // 원문 디렉터리를 더한다(문자열은 복사한다). 오류: Freeze 뒤 ScopeClosed, 가상 루트가
    // 디렉터리 규칙(CheckShaderVirtualDirectory)을 어기거나 /Engine이거나(공급자가 엔진 폴더로
    // 정한다) 폴더가 비면 InvalidArgument, 같은 루트가 이미 있으면 AlreadyExists, 표가 차면
    // CapacityExceeded, 할당 실패.
    Result<void> AddSourceDirectory(StringView virtualRoot, StringView directory) noexcept;

    // 원문 디렉터리 표다(추가 순서). Freeze 뒤에만 다른 스레드에서 읽는다.
    Span<const ShaderSourceDirectory> SourceDirectories() const noexcept {
        return directories_.View();
    }

    // 등록부를 닫는다(모듈 시작 뒤·렌더 lane 시작 전, 로더 스레드). 성공은 Info 한 줄(타입 수),
    // 실패는 Error(등록부 LastFailure)와 그 오류다. 두 번째 호출은 아무 일도 하지 않는다.
    Result<void> Freeze() noexcept;

    bool IsFrozen() const noexcept {
        return registry_.IsFrozen();
    }

    // 공급자를 붙인다(공급자 모듈 Startup 또는 호스트). 오류: 이미 다른 공급자가 붙음 Busy.
    // provider는 떼기 전까지 산다.
    Result<void> AttachProvider(ShaderProvider& provider) noexcept;
    // provider가 붙어 있으면 뗀다.
    void DetachProvider(ShaderProvider& provider) noexcept;
    // 붙은 공급자다(없으면 null). 어느 스레드에서나 부를 수 있다.
    ShaderProvider* Provider() const noexcept;

    // 쿠킹 라이브러리 원천을 붙인다. 오류: 이미 다른 원천이 붙음 Busy.
    Result<void> AttachCookedLibraries(CookedShaderLibrarySource& source) noexcept;
    void DetachCookedLibraries(CookedShaderLibrarySource& source) noexcept;
    CookedShaderLibrarySource* CookedLibraries() const noexcept;

private:
    struct DirectoryStorage {
        explicit DirectoryStorage(Allocator& allocator) noexcept
            : virtualRoot(allocator), directory(allocator) {
        }

        String virtualRoot;
        String directory;
    };

    Allocator& allocator_;
    shader::ShaderTypeRegistry registry_;
    // 문자열 저장소는 항목마다 따로 할당해 표가 커져도 directories_의 view가 그대로다.
    Array<UniquePtr<DirectoryStorage>> storage_;
    Array<ShaderSourceDirectory> directories_;
    mutable platform::Mutex gate_;
    ShaderProvider* provider_ = nullptr;
    CookedShaderLibrarySource* cooked_ = nullptr;
    bool attached_ = false;
};

// backend의 엔진 셰이더 형식 id다(9.12절 2항 고정 표 — D3D12_SM6·D3D11_SM5·VULKAN_SM5·
// GLES31_PC·METAL_SM5). Android GLES31_ANDROID는 쿠킹 원천의 형식 선택과 함께 정한다.
ROCKS_RENDERBASE_API StringView EngineShaderFormat(graphics::Backend backend) noexcept;

// 엔진 셰이더 집합과 내장 자원 팩이다(9.12절 2항). 렌더 lane 처리기가 멤버로 둔다.
//
// 스레드: Open·Retire는 장치 소유 스레드(렌더 lane 시작·종료 단계 또는 직접 그리는 소유
// 스레드)에서 부른다. 연 뒤의 조회는 ShaderSet·BuiltinResources 규칙을 따른다.
class ROCKS_RENDERBASE_API EngineRenderResources final {
public:
    explicit EngineRenderResources(Allocator& allocator) noexcept;
    // 집합을 파괴한다(Retire하지 않는다 — 주인이 종료 단계에서 Retire한다).
    ~EngineRenderResources() noexcept;
    EngineRenderResources(const EngineRenderResources&) = delete;
    EngineRenderResources& operator=(const EngineRenderResources&) = delete;

    // 엔진 셰이더 집합을 열고 검사한 뒤 내장 자원 팩을 만든다. host와 device는 Retire까지 산다.
    // 쿠킹 원천이 붙어 있으면 SelectCookedShaderFormat(매니페스트 backend·형식 순서, 상한·장치
    // 요구)으로 고른 형식의 라이브러리를 열고, 장치 backend가 쿠킹에 없으면(NotFound) 고정 표
    // 형식(EngineShaderFormat)으로 공급자에 간다(ShaderSystem SSD16). 실패: 등록부가 Freeze되지
    // 않음 NotReady, 형식 없음·셰이더 없는 쿠킹·맞는 쿠킹 형식 없음 Unsupported, 이미 엶 Busy,
    // 공급자 없음 UnsupportedCapability, 집합 검사·공급 실패 DependencyFailed, 쿠킹 원천의 손상
    // (InvalidArgument)·읽기 오류, 팩 생성 오류(빠진 fallback 셰이더 NotFound). 실패는 Error 로그를
    // 남기고 Problem에 영어 설명을 둔다. 실패하면 만든 것을 Retire한다.
    Result<void> Open(graphics::Device& device, const EngineShaderHost& host) noexcept;

    // 팩 → 집합 순으로 Retire한다(5.5절 종료 4단계). lease가 남지 않았으면 집합 객체도
    // 놓는다(공급자 저장소를 공급자 모듈 종료 전에 돌려준다 — 9.12절 2항). 열지 않았으면 아무 일도
    // 하지 않는다.
    void Retire(graphics::Device& device) noexcept;

    bool IsReady() const noexcept {
        return ready_;
    }

    // 연 집합이다(Ready일 때). 없으면 null이다.
    ShaderSet* Shaders() noexcept {
        return ready_ ? set_.Get() : nullptr;
    }

    BuiltinResources& Builtins() noexcept {
        return builtins_;
    }

    const BuiltinResources& Builtins() const noexcept {
        return builtins_;
    }

    // 집합을 쿠킹 원천으로 열었는가(아니면 공급자).
    bool OpenedFromCookedLibrary() const noexcept {
        return cooked_;
    }

    // 연 집합의 셰이더 형식 id다(마지막 Open이 고른 형식 — 없으면 빈 값).
    StringView FormatId() const noexcept {
        return StringView(format_);
    }

    // 마지막 실패의 영어 설명이다(없으면 빈 문자열).
    StringView Problem() const noexcept {
        return StringView(problem_);
    }

private:
    Result<void> Fail(ErrorCode error, StringView format, const char* stage,
                      StringView detail) noexcept;

    Allocator& allocator_;
    UniquePtr<ShaderSet> set_;
    BuiltinResources builtins_;
    char problem_[512] = {};
    // 집합 desc가 가리키는 형식 id 저장소다(집합보다 오래 산다).
    char format_[shader::MaxShaderFormatIdBytes] = {};
    bool ready_ = false;
    bool cooked_ = false;
};

} // namespace rocks::render

ROCKS_DECLARE_HOST_SERVICE(rocks::render::EngineShaderHost, ROCKS_RENDERBASE_API)
