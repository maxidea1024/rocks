#pragma once

#include <Core/Lifetime/ModuleLease.h>
#include <Core/Module/ModuleEntry.h>

namespace rocks {

class HostServices;
class ModuleLoader;

// ModuleImplementation::Startup이 받는 문맥이다(Docs/Design/ModuleLoader 3.1절). 전역 접근자
// 대신 이것으로 로더·호스트 서비스·이 모듈의 코드 수명 토큰을 얻는다. 로더만 만든다.
//
// 수명: Startup 호출 동안만 유효하다. 참조·포인터를 보관하지 말고, 필요한 서비스 포인터와 lease는
// 복사해 둔다(서비스 수명은 호스트가 정하고 lease는 복사본이 따로 산다).
class ModuleStartContext final {
public:
    ModuleStartContext(const ModuleStartContext&) = delete;
    ModuleStartContext& operator=(const ModuleStartContext&) = delete;

    // 이 모듈을 시작하는 로더다. Startup 안에서 다른 모듈을 Load할 수 있다(3.4절 재진입).
    ModuleLoader& Loader() const noexcept {
        return *loader_;
    }

    // 이 모듈의 로딩 단계(모듈 표 행의 값)다.
    LoadingPhase Phase() const noexcept {
        return phase_;
    }

    // 이 모듈의 코드 수명 토큰 복사본이다(3.6절 MD16). 모듈 코드·데이터를 가리키는 등록(타입·핸들·
    // delegate)이 잡고, 모듈 Shutdown에서 그 등록을 해제해 놓는다. 로더가 파괴되기 전에 모두 놓아야
    // 한다.
    ModuleLease Lease() const noexcept {
        return lease_;
    }

    // 호스트 서비스 표다. 아직 등록되지 않은 서비스의 Find는 null이다.
    HostServices& Services() const noexcept {
        return *services_;
    }

private:
    friend class ModuleLoader;

    ModuleStartContext(ModuleLoader& loader, LoadingPhase phase, ModuleLease lease,
                       HostServices& services) noexcept
        : loader_(&loader), phase_(phase), lease_(Move(lease)), services_(&services) {
    }

    ModuleLoader* loader_;
    LoadingPhase phase_;
    ModuleLease lease_;
    HostServices* services_;
};

} // namespace rocks
