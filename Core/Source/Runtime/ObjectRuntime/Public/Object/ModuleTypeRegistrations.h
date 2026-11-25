#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <ModuleLoader/ModuleTypeRegistrar.h>
#include <Object/ModuleRegistration.h>
#include <Object/TypeRegistry.h>

namespace rocks {

// 모듈 로더의 타입 등록기(ModuleTypeRegistrar) 일반 구현이다(Docs/Design/ModuleLoader 3.10절
// MD15). TypeRegistry 하나에 모듈마다 ModuleRegistration을 따로 두어 모듈 하나의 타입만 골라
// 되돌린다. Engine이 엔진 레지스트리에, 엔진 없이 World 레지스트리를 만드는 도구(WorldBench)가 자기
// 레지스트리에 쓴다.
//
// 사용 예:
//   TypeRegistry types(allocator);
//   ModuleTypeRegistrations registrations(types, allocator);
//   ModuleLoadReport attached = loader.AttachTypeRegistrar(registrations);   // 시작한 모듈 등록
//   ...
//   loader.DetachTypeRegistrar(registrations);                                // 레지스트리 종료 전
//   (void)types.Shutdown();
//
// 해제 규칙: Unregister는 그 모듈 기록의 Unregister를 시도하고, 남은 객체·native 파생 때문에 Busy면
// 기록 객체만 놓는다(붙잡은 lease 반환). 남은 레코드는 TypeRegistry::Shutdown이 dependent 순서로
// 반납한다(Engine 종료 규칙과 같다). 그래서 레지스트리를 닫기 전에 등록기를 떼어야 한다.
//
// 수명: registry와 allocator는 이 객체보다 오래 산다. 로더에서 뗀 뒤 파괴한다(파괴는 남은 기록을
// 해제하지 않고 놓기만 한다). 등록·해제는 TypeRegistry와 같은 스레드(로더 스레드)에서 한다.
class ROCKS_OBJECTRUNTIME_API ModuleTypeRegistrations final : public ModuleTypeRegistrar {
public:
    ModuleTypeRegistrations(TypeRegistry& registry, Allocator& allocator) noexcept;
    ~ModuleTypeRegistrations() noexcept override = default;

    ModuleTypeRegistrations(const ModuleTypeRegistrations&) = delete;
    ModuleTypeRegistrations& operator=(const ModuleTypeRegistrations&) = delete;

    // 모듈마다 새 ModuleRegistration에 registerTypes를 부른다. 실패하면 그 기록 전체를 되돌리고 그
    // 오류를 돌려준다. 같은 모듈이 이미 등록되어 있으면 AlreadyRegistered, 할당 실패는 그 오류다.
    Result<void> Register(StringView module, ModuleEntry::RegisterTypesFunction registerTypes,
                          const ModuleLease& lease) noexcept override;

    // 해제 규칙은 클래스 주석과 같다. 등록되지 않은 모듈은 할 일이 없다.
    void Unregister(StringView module) noexcept override;

    // 등록된 모듈 수다.
    usize Count() const noexcept {
        return entries_.Size();
    }

    // module의 타입이 등록되어 있는지다.
    bool Contains(StringView module) const noexcept;

private:
    struct Entry {
        // 모듈 표의 정적 이름이다.
        StringView module;
        UniquePtr<ModuleRegistration> registration;
    };

    usize IndexOf(StringView module) const noexcept;

    TypeRegistry* registry_;
    Allocator* allocator_;
    Array<Entry> entries_;
};

} // namespace rocks
