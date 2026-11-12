#pragma once

#include <Core/Containers/Array.h>
#include <Object/TypeRegistry.h>

namespace rocks {

// 모듈 단위 타입 등록의 기록이다. MetaCompiler가 모듈마다 내는 집계 헤더
// (<Module>/<Module>.module.generated.h)의 RegisterModuleTypes가 이 객체에 타입을 등록한다.
// 의존 모듈의 타입을 먼저 등록하고, 같은 ID·같은 native 타입으로 이미 등록된 타입은 새로 등록하지
// 않고 그 타입의 lease를 붙잡는다. 그래서 여러 모듈이 같은 의존 모듈을 공유해도 한 번만 등록되고,
// 공유 타입을 먼저 등록한 쪽이 해제를 시도해도 그 타입을 쓰는 다른 기록이 남아 있으면 Busy로
// 멈춘다(쓰는 쪽이 타입을 잃지 않는다).
//
// 사용 예:
//   #include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
//   ModuleRegistration registration(types, allocator);
//   auto registered = generated::ReflectionTestFixtures::RegisterModuleTypes(registration);
//   if (!registered) {
//       ...   // 실패하면 이 호출의 기록(등록·lease)은 이미 역순으로 되돌려졌다
//   }
//   ...
//   if (auto removed = registration.Unregister(); !removed) { ... }   // 남은 사용이 있으면 Busy
//
// 소멸자는 등록한 타입을 해제하지 않는다(Registry가 먼저 Shutdown될 수 있고, 해제가 Busy일 수 있다)
// — 호출자가 Unregister를 부른다. 붙잡은 lease는 소멸자에서도 놓는다. lease가 남아 있으면
// TypeRegistry::Shutdown은 Busy이므로 모든 기록을 Unregister한 뒤 Shutdown한다. TypeRegistry와
// allocator는 이 객체보다 오래 살아야 한다. 등록·해제는 TypeRegistry와 같은 생성 스레드에서만 한다.
// 복사·이동하지 않는다.
class ModuleRegistration {
public:
    ModuleRegistration(TypeRegistry& registry, Allocator& allocator) noexcept;
    ModuleRegistration(const ModuleRegistration&) = delete;
    ModuleRegistration& operator=(const ModuleRegistration&) = delete;

    // 기록 수다. 이 객체가 등록한 타입, 이미 등록되어 있어 lease로 붙잡은 타입, 들어온 모듈을 모두
    // 센다. UnregisterFrom의 mark로 쓴다.
    usize Size() const noexcept;

    // 모듈 집계 함수(RegisterModuleTypes)가 처음에 부른다. key는 모듈마다 하나인 정적 객체의
    // 주소다. 이 객체에 같은 모듈이 이미 들어와 있으면 false(이미 등록됨 — 의존 그래프의 여러
    // 경로로 다시 불려도 모듈마다 한 번만 등록한다), 아니면 모듈 기록을 남기고 true다. 모듈 기록은
    // UnregisterFrom이 되돌린다. 실패는 OutOfMemory·InvalidArgument(key가 null)이며 기록은 호출
    // 전과 같다.
    Result<bool> EnterModule(const void* key) noexcept;

    // schema.id가 등록되어 있지 않으면 등록하고 true다. 같은 ID가 같은 native
    // 타입(ops.nativeType)으로 이미 등록되어 있으면 등록하지 않고 그 타입의 lease를 붙잡아 기록하고
    // false다. 같은 ID가 다른 native 타입이면 AlreadyExists. 그 밖의 실패는 할당(OutOfMemory)과
    // TypeRegistry::Register·Acquire의 오류(InvalidArgument 등)이며, 실패하면 기록과 Registry
    // 상태는 호출 전과 같다.
    Result<bool> RegisterIfAbsent(const TypeSchema& schema, RuntimeTypeOps ops) noexcept;

    // mark(Size()로 얻은 값) 이후의 기록을 역순으로 되돌린다: 붙잡은 lease는 놓고, 등록한 타입은
    // 해제한다. 해제가 실패하면(그 타입의 객체나 다른 기록의 lease가 남아 Busy 등) 거기서 멈추고
    // 아직 되돌리지 못한 기록을 보존한 채 오류를 돌려준다(다시 호출할 수 있다). mark가 Size()보다
    // 크면 InvalidArgument.
    Result<void> UnregisterFrom(usize mark) noexcept;

    Result<void> Unregister() noexcept {
        return UnregisterFrom(0);
    }

private:
    // module이 null이 아니면 들어온 모듈의 기록이다. 아니면 retained가 비어 있을 때 이 객체가
    // token으로 등록한 타입이고, retained가 있으면 이미 등록된 타입을 붙잡은 lease다.
    struct Record {
        const void* module = nullptr;
        TypeRegistrationToken token{};
        TypeLease retained;
    };

    Result<void> ReserveRecord() noexcept;

    TypeRegistry* registry_;
    Array<Record> records_;
};

} // namespace rocks
