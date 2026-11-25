#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Result.h>
#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/ShaderSet.h>

// Development 셰이더 공급자다(Docs/Design/RenderBase 9.12절 4항, ShaderSystem 12.3.1절 SSD16 —
// UE의 셰이더 컴파일 관리자처럼 셰이더 컴파일을 허용하는 개발 실행에만 있다). 형식마다 한 번
// CompileShaderLibrary(개발 전용 타입 포함, 너겟 캐시)로 메모리 라이브러리를 만들어 요청한 셰이더
// 집합에 넘기고, 같은 형식의 다음 요청에는 그 byte를 복사한다.
//
// 보통은 직접 만들지 않는다: 실행 파일이 Release 밖에서 이 모듈을 링크하면 모듈 수명 객체가
// Startup에서 EngineShaderHost 서비스에 공급자를 붙이고 Shutdown에서 떼며 워커를 멈춘다(서비스가
// 없으면 아무 일도 하지 않고, 이미 다른 공급자가 붙어 있으면 붙이지 않는다).
//
// 첫 요청에서 준비한다: 원문 /Engine = <engine>/Shaders와 호스트 원문 디렉터리 mount, 형식 정의
// <engine>/Config/ShaderFormats, ShaderCompileWorker(실행 파일 옆, 없으면 같은 출력 뿌리의
// <도구>-<플랫폼>-shadercompileworker-<구성>/bin), 너겟 캐시, 컴파일 관리자. 준비가 실패하면 그
// 뒤 요청은 모두 같은 설명으로 실패한다.
//
// 사용 예(호스트가 직접 붙이는 경우):
//   auto provider = shader::DevelopmentShaderProvider::TryCreate(allocator, shaderHost);
//   (void)shaderHost.AttachProvider(*provider.Value());
//   ... 렌더 lane ...
//   shaderHost.DetachProvider(*provider.Value());
//   provider.Value()->Shutdown();
//
// 스레드: RequestLibrary는 어느 스레드에서나 불러도 되고 잠금으로 직렬화한다(같은 호출 안에서
// CompleteProvision까지 끝낸다). Shutdown은 마지막 요청이 끝난 뒤 부른다.
namespace rocks::shader {

class ROCKS_DEVELOPMENTSHADERPROVIDER_API DevelopmentShaderProvider final
    : public render::ShaderProvider {
public:
    // allocator는 여러 스레드의 동시 할당을 받고 이 객체보다 오래 산다. host는 원문 디렉터리를
    // 읽을 엔진 셰이더 호스트이며 이 객체보다 오래 산다.
    static Result<UniquePtr<DevelopmentShaderProvider>>
    TryCreate(Allocator& allocator, const render::EngineShaderHost& host) noexcept;

    ~DevelopmentShaderProvider() noexcept;
    DevelopmentShaderProvider(const DevelopmentShaderProvider&) = delete;
    DevelopmentShaderProvider& operator=(const DevelopmentShaderProvider&) = delete;

    // 요청을 받아 이 호출 안에서 set.CompleteProvision을 부른다. 컴파일·준비 실패는 영어 진단과
    // 함께 공급 실패로 알리고 Success를 돌려준다. 결과 복사의 할당 실패만 OutOfMemory다.
    Result<void> RequestLibrary(const render::ShaderProvisionRequest& request,
                                render::ShaderSet& set) noexcept override;

    // 컴파일 관리자·워커·너겟 캐시를 멈춘다. 두 번째 호출은 아무 일도 하지 않는다.
    void Shutdown() noexcept;

    // 만든 라이브러리 수(형식 수 — 시험·진단)다.
    usize BuiltLibraryCount() const noexcept;

    // 내부 상태(원문 트리·워커·캐시·만든 라이브러리)다. 정의는 cpp에 있다.
    struct State;

    // TryCreate가 쓴다(UniquePtr::TryMake가 부를 수 있게 공개). 직접 부르지 않는다.
    DevelopmentShaderProvider(Allocator& allocator, UniquePtr<State> state) noexcept;

private:
    Allocator& allocator_;
    UniquePtr<State> state_;
};

} // namespace rocks::shader
