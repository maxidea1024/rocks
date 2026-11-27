#pragma once

#include <Core/Memory/Allocator.h>
#include <ModuleLoader/ModuleLoader.h>
#include <Progress/ProgressScope.h>

namespace rocks {

// 모듈 시작을 진행 구간으로 보고한다(Docs/Design/SplashScreen 8.1절,
// Docs/Design/ModuleLoader 3.16절). 살아 있는 동안 로더에 시작 관찰자를 걸어, StartPhase 하나마다
// 모듈 수 구간을 열고 모듈마다 문구 "Starting module <이름>"으로 EnterStep한다. 로더는 Progress를
// 모르므로(부트 묶음) 이 어댑터가 구간을 연다. 이 스레드에 Primary 바인딩이 없으면 구간은 아무 일도
// 하지 않고 문구도 만들지 않는다 (도구·시험·서버).
//
// 사용 예(로더 스레드):
//   {
//       ModuleStartProgress progress(loader, allocator);
//       const ModuleLoadReport host = loader.StartPhase(LoadingPhase::PostConfigInit);
//   }
//
// 규칙: 로더 스레드의 스택 객체로 쓰고 StartPhase 밖에서 만들고 없앤다. 겹쳐 만들 수 있고(앞
// 관찰자를 되돌려 놓는다) 연 역순으로 없앤다. allocator는 문구 Text를 만들고 그 Text가 스냅샷에서
// 놓일 때까지 살아 있어야 한다.
class ROCKS_ENGINE_API ModuleStartProgress {
public:
    ModuleStartProgress(ModuleLoader& loader, Allocator& allocator) noexcept;
    ~ModuleStartProgress() noexcept;
    ModuleStartProgress(const ModuleStartProgress&) = delete;
    ModuleStartProgress& operator=(const ModuleStartProgress&) = delete;

private:
    static void PhaseBegin(void* context, usize moduleCount) noexcept;
    static void ModuleStarting(void* context, StringView moduleName) noexcept;
    static void PhaseEnd(void* context) noexcept;

    void CloseScope() noexcept;

    ModuleLoader& loader_;
    Allocator& allocator_;
    ModuleStartObserver previous_;
    alignas(ProgressScope) unsigned char scopeStorage_[sizeof(ProgressScope)];
    ProgressScope* scope_ = nullptr;
};

} // namespace rocks
