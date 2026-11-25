#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <ShaderCompiler/ShaderCookRegistrySource.h>

// 제품 쿠커의 셰이더 타입 등록부 원천이다(Docs/Design/BuildTargets 3.10절 4항, ShaderSystem 16.1절
// SSD45 3번). Acquire가 등록부를 만들어 호스트 서비스로 올리고 엔진 묶음을 출시 타깃 실행과 같은
// 단계(StartEngineGroup → StartPhase(PostEngineInit))까지 시작한다. 각 모듈 수명 객체가
// Startup에서 셰이더 타입을 등록하면 Freeze해 돌려준다(UE가 모든 모듈을 적재한 뒤 전역 셰이더
// 맵을 만드는 자리). Release가 엔진 묶음을 내리고 서비스를 뗀다. 호스트가 이미 등록부 서비스를
// 올렸으면 그것을 쓴다(Freeze되어 있어야 한다 — 엔진 묶음을 건드리지 않는다).
//
// 사용 예(ShaderCompiler 모듈 수명 객체):
//   ModuleShaderCookRegistrySource source(allocator, context.Loader());
//   ShaderLibraryCookStage stage(config /* registry = &source */);
//
// 스레드·수명: Acquire·Release는 로더 스레드에서 부른다(쿠킹 단계 Run은 명령 스레드 = 로더
// 스레드). loader와 allocator는 이 객체보다 오래 산다.
namespace rocks {

class ModuleLoader;

} // namespace rocks

namespace rocks::shader {

class ROCKS_SHADERCOMPILER_API ModuleShaderCookRegistrySource final
    : public ShaderCookRegistrySource {
public:
    ModuleShaderCookRegistrySource(Allocator& allocator, ModuleLoader& loader) noexcept;
    // 준비한 채 파괴하면 Release한다.
    ~ModuleShaderCookRegistrySource() noexcept override;
    ModuleShaderCookRegistrySource(const ModuleShaderCookRegistrySource&) = delete;
    ModuleShaderCookRegistrySource& operator=(const ModuleShaderCookRegistrySource&) = delete;

    // 오류: 이미 준비함 Busy, 호스트 등록부가 Freeze되지 않음 NotReady, 서비스 등록 오류, 엔진
    // 묶음·PostEngineInit 시작 실패 DependencyFailed(Problem에 모듈·단계·문구), 등록부 Freeze
    // 실패(그 오류 — Problem에 타입·이유), 할당 실패.
    Result<const ShaderTypeRegistry*> Acquire() noexcept override;
    void Release() noexcept override;
    StringView Problem() const noexcept override;

private:
    // 설명 버퍼 byte 수다.
    static constexpr usize ProblemBytes = 512;

    Allocator& allocator_;
    ModuleLoader& loader_;
    UniquePtr<ShaderTypeRegistry> registry_;
    const ShaderTypeRegistry* acquired_ = nullptr;
    bool engineStarted_ = false;
    char problem_[ProblemBytes] = {};
};

} // namespace rocks::shader
