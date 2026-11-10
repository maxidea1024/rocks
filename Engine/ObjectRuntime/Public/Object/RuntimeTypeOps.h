#pragma once

#include <Core/Lifetime/ModuleLease.h>
#include <Core/Move.h>
#include <Object/Object.h>
#include <new>

namespace rocks {

// Trace 호출 동안만 유효한 방문자다. 보관하거나 다른 스레드에 전달하지 않는다.
// 객체가 가진 모든 강한 ObjectRef를 빠짐없이 Visit해야 한다. WeakRef는 방문하지 않는다.
class TraceVisitor {
public:
    using VisitFn = void (*)(void*, ObjectHandle) noexcept;

    TraceVisitor(void* state, VisitFn visit) noexcept : state_(state), visit_(visit) {
    }

    void Visit(ObjectHandle handle) noexcept {
        visit_(state_, handle);
    }

    template <class Ref>
    void Visit(const Ref& reference) noexcept {
        Visit(reference.Handle());
    }

private:
    void* state_;
    VisitFn visit_;
};

// 생성·추적·소멸 콜백은 noexcept이며 Registry 잠금 밖에서 실행된다.
// Trace는 읽기만 수행한다. 콜백 중 생성·간선 변경·수집 재진입은 Busy로 거부한다.
// lease 반환과 통계 조회는 허용한다. 활성 ConstructionContext의 롤백/소멸 재진입은 금지한다.
// 동적 모듈의 함수 포인터를 제공한다면 module로 코드 수명도 유지해야 한다.
struct RuntimeTypeOps {
    usize size = 0;
    usize alignment = 0;
    const void* nativeType = nullptr;
    Object* (*construct)(void*) noexcept = nullptr;
    void (*destroy)(Object*) noexcept = nullptr;
    void (*trace)(const Object&, TraceVisitor&) noexcept = nullptr;
    ModuleLease module;
};

// 실패 가능한 초기화는 기본 생성자 밖의 ConstructionContext 단계에서 처리한다.
// Trace가 없는 타입은 강한 간선이 없는 것으로 취급한다. 메타데이터 자동 추적은 없다.
template <class T>
RuntimeTypeOps MakeRuntimeTypeOps(ModuleLease module = {}) noexcept {
    static_assert(__is_base_of(Object, T));
    static_assert(__is_nothrow_constructible(T));
    static_assert(__is_nothrow_destructible(T));

    RuntimeTypeOps ops;
    ops.size = sizeof(T);
    ops.alignment = alignof(T);
    ops.nativeType = NativeTypeKey<T>();
    ops.construct = [](void* memory) noexcept -> Object* { return ::new (memory) T(); };
    ops.destroy = [](Object* object) noexcept { static_cast<T*>(object)->~T(); };
    ops.trace = [](const Object& object, TraceVisitor& visitor) noexcept {
        if constexpr (requires(const T& value) { value.Trace(visitor); }) {
            static_assert(noexcept(static_cast<const T&>(object).Trace(visitor)));
            static_cast<const T&>(object).Trace(visitor);
        }
    };
    ops.module = Move(module);
    return ops;
}

} // namespace rocks
