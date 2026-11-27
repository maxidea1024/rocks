#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/ShaderSet.h>

// Development 셰이더 공급자다(Docs/Design/RenderBase 9.12절 4항, ShaderSystem 12.3.1절 SSD16 —
// UE의 셰이더 컴파일 관리자처럼 셰이더 컴파일을 허용하는 개발 실행에만 있다). 형식마다 한 번
// 메모리 라이브러리(개발 전용 타입 포함, 너겟 캐시)를 만들어 요청한 셰이더 집합에 넘기고, 같은
// 형식의 다음 요청에는 그 byte를 복사한다.
//
// 감시 재컴파일(ShaderSystem 16.3절 SH4, UE r.ShaderCompiler.RecompileShadersOnSave 대응): 첫
// 라이브러리를 만든 뒤 감시 스레드가 원문 디렉터리(/Engine과 호스트 원문 디렉터리)를 보고, 원문이
// 바뀌면 NotifySourcesChanged로 의존 셰이더가 있는 형식 라이브러리를 모두 다시 만든다
// (ShaderLibraryReloader — 너겟 캐시로 바뀐 job만 컴파일). 성공해 byte가 바뀐 형식은
// LibraryRevision이 오르고, 렌더 쪽(EngineRenderResources::ApplyShaderUpdates)이 프레임 경계에서 새
// 집합으로 바꾼다. 실패하면 이전 라이브러리를 유지하고 진단을 편집기에서 눌러 이동하는 꼴(`<디스크
// 경로>(<줄>,<열>): error: …`)로 Error 로그에 남긴다(SSD72). 재컴파일이 끝날 때마다
// 통지(WaitForReload)를 낸다.
//
// 보통은 직접 만들지 않는다: 실행 파일이 Release 밖에서 이 모듈을 링크하면 모듈 수명 객체가
// Startup에서 EngineShaderHost 서비스에 공급자를 붙이고 Shutdown에서 떼며 감시·워커를
// 멈춘다(서비스가 없으면 아무 일도 하지 않고, 이미 다른 공급자가 붙어 있으면 붙이지 않는다). 설정은
// CVar `shader.recompile_on_save`(기본 true)·`shader.symbols`(기본 false — 디버그 심볼과 심볼 파일
// <너겟 root>/Saved/ShaderSymbols/<형식>/, SSD73–SSD75)다.
//
// 첫 요청에서 준비한다: 원문 /Engine = <engine>/Shaders와 호스트 원문 디렉터리 mount, 형식 정의
// <engine>/Config/ShaderFormats, ShaderCompileWorker(실행 파일 옆, 없으면 같은 출력 뿌리의
// <도구>-<플랫폼>-shadercompileworker-<구성>/bin), 너겟 캐시, 컴파일 관리자. 준비가 실패하면 그
// 뒤 요청은 모두 같은 설명으로 실패한다.
//
// 사용 예(호스트가 직접 붙이는 경우):
//   DevelopmentShaderProviderOptions options;
//   options.watchSources = false;                 // 시험: 변경은 NotifySourcesChanged로 넣는다
//   auto provider = shader::DevelopmentShaderProvider::TryCreate(allocator, shaderHost, options);
//   (void)shaderHost.AttachProvider(*provider.Value());
//   ... 렌더 lane ...
//   shaderHost.DetachProvider(*provider.Value());
//   provider.Value()->Shutdown();
//
// 스레드: RequestLibrary·LibraryRevision·WaitForReload·NotifySourcesChanged는 어느 스레드에서나
// 부른다. 이미 만든 형식의 RequestLibrary와 LibraryRevision은 다시 만드는 동안에도 짧은 잠금만
// 잡는다 (렌더 lane을 막지 않는다). 처음 만드는 형식의 요청은 그 컴파일을 기다린다. Shutdown은
// 마지막 요청이 끝난 뒤 부른다.
namespace rocks::shader {

struct DevelopmentShaderProviderOptions {
    // 첫 라이브러리를 만든 뒤 원문 디렉터리 감시 스레드를 띄운다(CVar shader.recompile_on_save).
    bool watchSources = true;
    // 모든 job을 debugSymbols=true로 만들고 심볼 파일을 쓴다(CVar shader.symbols).
    bool debugSymbols = false;
};

class ROCKS_DEVELOPMENTSHADERPROVIDER_API DevelopmentShaderProvider final
    : public render::ShaderProvider {
public:
    // allocator는 여러 스레드의 동시 할당을 받고 이 객체보다 오래 산다. host는 원문 디렉터리를
    // 읽을 엔진 셰이더 호스트이며 이 객체보다 오래 산다.
    static Result<UniquePtr<DevelopmentShaderProvider>>
    TryCreate(Allocator& allocator, const render::EngineShaderHost& host,
              const DevelopmentShaderProviderOptions& options = {}) noexcept;

    ~DevelopmentShaderProvider() noexcept;
    DevelopmentShaderProvider(const DevelopmentShaderProvider&) = delete;
    DevelopmentShaderProvider& operator=(const DevelopmentShaderProvider&) = delete;

    // 요청을 받아 이 호출 안에서 set.CompleteProvision을 부른다. 컴파일·준비 실패는 영어 진단과
    // 함께 공급 실패로 알리고 Success를 돌려준다. 결과 복사의 할당 실패만 OutOfMemory다.
    Result<void> RequestLibrary(const render::ShaderProvisionRequest& request,
                                render::ShaderSet& set) noexcept override;

    // 그 형식·종류 라이브러리의 revision(1부터, 다시 만들어 byte가 바뀔 때마다 오른다)이다. 만들지
    // 않은 형식은 0이다.
    uint64 LibraryRevision(StringView formatId, ShaderLibraryKind library) const noexcept override;

    // 재컴파일 통지를 기다린다(ShaderProvider 계약 — 순번이 afterSequence보다 큰 가장 최근 통지).
    bool WaitForReload(uint64 afterSequence, uint32 timeoutMilliseconds,
                       render::ShaderReloadNotice& out) noexcept override;

    // 원문이 바뀌었다(가상 경로 — 감시 스레드와 시험이 부른다). everything이면(감시 이벤트 손실)
    // 모든 셰이더가 영향받는다. 이 호출 안에서 다시 만들고 로그·통지를 낸다(재컴파일은 차례로
    // 돈다). 아직 만든 라이브러리가 없으면 다시 만들 것 없이 성공 통지다. 실행 실패(할당·관리자)만
    // Result 실패이고 그때도 실패 통지를 낸다.
    Result<void> NotifySourcesChanged(Span<const StringView> virtualPaths,
                                      bool everything) noexcept;

    // 감시 스레드가 돌고 있는가(시험·진단).
    bool IsWatching() const noexcept;

    // 감시를 멈추고 컴파일 관리자·워커·너겟 캐시를 멈춘다. 두 번째 호출은 아무 일도 하지 않는다.
    void Shutdown() noexcept;

    // 만든 라이브러리 수(형식 × 종류 — 시험·진단)다.
    usize BuiltLibraryCount() const noexcept;

    // 내부 상태(원문 트리·워커·캐시·라이브러리·감시)다. 정의는 cpp에 있다.
    struct State;

    // TryCreate가 쓴다(UniquePtr::TryMake가 부를 수 있게 공개). 직접 부르지 않는다.
    DevelopmentShaderProvider(Allocator& allocator, UniquePtr<State> state) noexcept;

private:
    Allocator& allocator_;
    UniquePtr<State> state_;
};

} // namespace rocks::shader
