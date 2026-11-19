#pragma once

#include <Core/Result.h>

namespace rocks {

// 수명 객체가 Startup에서 받는 문맥이다. 정의는 ModuleLoader
// 모듈(ModuleLoader/ModuleStartContext.h)에 있다. Startup에서 문맥을 쓰는 모듈은 그 헤더를
// include하고 ModuleLoader에 의존한다.
class ModuleStartContext;

// 모듈 하나의 수명 객체 기반 클래스다(UE IModuleInterface 대응,
// Docs/Design/ModuleLoader.ko.md 3.1절). 모든 C++ 모듈은 이 클래스(또는
// DefaultModuleImplementation)를 진입점 매크로 ROCKS_IMPLEMENT_MODULE(Core/Module/ModuleEntry.h)로
// 등록한다.
//
// 사용 예:
//   class CurvesModule final : public ModuleImplementation {
//   public:
//       Result<void> Startup(ModuleStartContext& context) noexcept override { ... }
//       void Shutdown() noexcept override { ... }
//   };
//   ROCKS_IMPLEMENT_MODULE(CurvesModule, Curves)
//
// 수명: 로더가 모듈을 시작할 때 진입 기술의 create로 만들고(로더의 allocator), 모듈을 내린
// 뒤(Shutdown 뒤)나 Startup이 실패한 뒤 파괴한다. 다시 적재하면 새 객체를 만든다. 생성자·소멸자는
// noexcept여야 하며 생성자에서 실패할 일을 하지 않는다(실패는 Startup에서 돌려준다).
//
// 호출 순서(UE와 같다): 시작은 Startup만, 묶음 종료는 그 묶음 모듈 전체의 PreUnload를 먼저 부른 뒤
// 각 Shutdown을 시작 역순으로 부른다. 모든 호출은 로더 스레드에서 일어난다.
class ModuleImplementation {
public:
    ModuleImplementation() noexcept = default;
    virtual ~ModuleImplementation() = default;

    ModuleImplementation(const ModuleImplementation&) = delete;
    ModuleImplementation& operator=(const ModuleImplementation&) = delete;

    // 모듈이 시작할 때 한 번 부른다. 이 모듈의 의존 모듈은 모두 이미 시작했다. context는 이 호출
    // 동안만 유효하다(필요한 서비스·lease는 복사해 둔다).
    // 실패를 돌려주기 전에 이 함수가 한 부분 등록을 스스로 되돌려야 한다. 실패하면 Shutdown은
    // 불리지 않고 객체는 파괴되며, 적재 요청은 StartupFailed(돌려준 ErrorCode를 담는다)로 끝난다.
    // 재진입: 이 안에서 다른 모듈을 Load할 수 있다. 시작 중인 자기 자신·의존 사슬은 Cycle이다.
    virtual Result<void> Startup(ModuleStartContext& context) noexcept {
        (void)context;
        return Result<void>::Success();
    }

    // 모듈 묶음을 내릴 때 그 묶음 전체의 Shutdown보다 먼저, 시작 역순으로 부른다. 다른 모듈이 아직
    // 살아 있으므로 그 모듈들과의 연결을 끊는 데 쓴다.
    virtual void PreUnload() noexcept {
    }

    // 시작한 모듈을 내릴 때 부른다. 시작 역순이다. Startup에서 한 등록을 모두 해제한다(lease 반환).
    virtual void Shutdown() noexcept {
    }

    // false면 묶음 종료 절차에서 내리지 않는다(UE SupportsAutomaticShutdown). 그 객체는 로더가
    // 파괴될 때 Shutdown 없이 파괴된다.
    virtual bool SupportsAutomaticShutdown() const noexcept {
        return true;
    }

    // false면 실행 중 명시 해제를 거부한다(UE SupportsDynamicReloading, 해제 API는 ML6).
    virtual bool SupportsUnload() const noexcept {
        return true;
    }
};

// 시작·종료 때 할 일이 없는 모듈의 빈 구현이다(UE FDefaultModuleImpl 대응).
// 사용 예: ROCKS_IMPLEMENT_MODULE(DefaultModuleImplementation, Serialization)
class DefaultModuleImplementation : public ModuleImplementation {};

} // namespace rocks
