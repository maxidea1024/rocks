#pragma once

#include <Core/Lifetime/ModuleLease.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks {

// 모듈의 리플렉션 타입을 레지스트리에 등록·해제하는 쪽이다(UE ProcessLoadedObjectsCallback 대응,
// Docs/Design/ModuleLoader 3.6·3.10절 MD15). 로더는 타입 시스템을 모르므로 레지스트리
// 주인(Engine, 레지스트리를 직접 만드는 도구)이 이 interface를 구현해
// ModuleLoader::AttachTypeRegistrar로 붙인다. 일반 구현은 ObjectRuntime의
// ModuleTypeRegistrations다.
//
// 호출: 모두 로더 스레드에서, 로더가 부른다. registerTypes가 있는(리플렉션 메타데이터가 있는)
// 모듈만 부른다. 같은 모듈에 Register가 성공한 뒤에만 Unregister를 부르며, 그 사이 다시
// Register하지 않는다.
class ModuleTypeRegistrar {
public:
    virtual ~ModuleTypeRegistrar() = default;

    // module 모듈의 자기 타입을 등록한다. registerTypes는 그 모듈 진입 기술의 생성 등록 함수,
    // lease는 로더가 발급한 그 모듈의 코드 수명 토큰이다(등록한 타입의 RuntimeTypeOps.module에
    // 들어간다). 의존 모듈은 이미 등록되어 있다. 실패하면 이 호출이 등록한 것을 스스로 되돌리고
    // 오류를 돌려준다. name은 정적 수명이다(모듈 표).
    virtual Result<void> Register(StringView module,
                                  ModuleEntry::RegisterTypesFunction registerTypes,
                                  const ModuleLease& lease) noexcept = 0;

    // Register가 등록한 것을 놓는다. 실패를 돌려주지 않는다: 남은 객체·native 파생 때문에 지금
    // 해제할 수 없는 타입은 레지스트리 종료가 거두게 두고 이 모듈의 기록만 놓아도 된다(Engine
    // 규칙). 실행 중 해제(ML6)는 CanUnregister로 먼저 거른다.
    virtual void Unregister(StringView module) noexcept = 0;

    // 실행 중 해제(ML6) 전에 그 모듈 타입을 지금 해제할 수 있는지다(살아 있는 인스턴스가 없는지).
    // 기본은 true다.
    virtual bool CanUnregister(StringView module) const noexcept {
        (void)module;
        return true;
    }
};

} // namespace rocks
