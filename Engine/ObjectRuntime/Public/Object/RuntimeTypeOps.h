#pragma once

#include <Core/Lifetime/ModuleLease.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Meta/TypeSchema.h>
#include <Object/ObjectAccessContext.h>
#include <Object/Object.h>
#include <new>

namespace rocks {

class ConstructionContext;

// 필드 조회 결과를 소유하는 이동 전용 값이다. 문자열도 복제하므로 임시 Borrow의 view를 유출하지
// 않는다. kind와 대응하는 payload만 읽는다. 같은 래퍼의 변경/allocator callback 재진입은 금지다.
// allocator는 값과 진행 중 호출보다 오래 살아야 한다. primitive의 폭 축소는 setter에서 검사한다.
struct FieldValue {
    explicit FieldValue(Allocator& allocator) noexcept : string(allocator), allocator_(&allocator) {
    }

    FieldValue(FieldValue&&) noexcept = default;
    FieldValue& operator=(FieldValue&&) noexcept = default;
    FieldValue(const FieldValue&) = delete;
    FieldValue& operator=(const FieldValue&) = delete;

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    ValueKind kind = ValueKind::Opaque;
    bool boolean = false;
    uint64 unsignedInteger = 0;
    int64 signedInteger = 0;
    float float32 = 0;
    double float64 = 0;
    Guid guid{};
    ObjectHandle reference{};
    String string;

private:
    Allocator* allocator_;
};

// 함수는 owner context/handle을 검사하는 generated thunk다. offset이나 raw field 주소를 공개하지
// 않는다. read 실패는 out 불변, write 실패는 해당 필드에 새 값을 게시하지 않는다.
// String commit 뒤 이전 buffer 반환 callback의 논리 파괴는 이미 완료된 성공을 되돌리지 않는다.
struct RuntimeFieldOps {
    Guid fieldId{};
    Result<void> (*read)(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                         FieldValue&) noexcept = nullptr;
    Result<void> (*write)(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                          const FieldValue&) noexcept = nullptr;
    Result<void> (*writeStaged)(ConstructionContext&, ObjectHandle,
                                const FieldValue&) noexcept = nullptr;
};

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
    Object* (*construct)(void*, Allocator&) noexcept = nullptr;
    void (*destroy)(Object*) noexcept = nullptr;
    void (*trace)(const Object&, TraceVisitor&) noexcept = nullptr;
    ModuleLease module;
    // 등록 시 배열 자체도 깊게 복사한다. callback 코드 수명은 위 module이 보존한다.
    Span<const RuntimeFieldOps> fields;
    const void* nativeBaseType = nullptr;
    // 기본 수동 Trace는 owner 전용이다. side effect 없는 generated Trace만 명시 opt-in한다.
    bool parallelTraceSafe = false;
    // 모든 비정적 필드와 강한 간선이 typed metadata에 포함된 경우만 true다.
    // graph serializer는 이 선언 없는 수동/부분 metadata를 완전한 객체로 저장하지 않는다.
    bool completeFieldLayout = false;
};

// noexcept T(Allocator&)가 있으면 registry allocator를 주입하고, 없으면 noexcept T()를 사용한다.
// 실패 가능한 초기화는 생성자 밖의 ConstructionContext 단계에서 처리한다.
// Trace가 없는 타입은 강한 간선이 없는 것으로 취급한다. 메타데이터 자동 추적은 없다.
template <class T>
RuntimeTypeOps MakeRuntimeTypeOps(ModuleLease module = {}) noexcept {
    static_assert(__is_base_of(Object, T));
    static_assert(__is_nothrow_constructible(T, Allocator&) || __is_nothrow_constructible(T));
    static_assert(__is_nothrow_destructible(T));

    RuntimeTypeOps ops;
    ops.size = sizeof(T);
    ops.alignment = alignof(T);
    ops.nativeType = NativeTypeKey<T>();
    ops.construct = [](void* memory, Allocator& allocator) noexcept -> Object* {
        if constexpr (__is_constructible(T, Allocator&)) {
            static_assert(__is_nothrow_constructible(T, Allocator&));
            return ::new (memory) T(allocator);
        } else {
            return ::new (memory) T();
        }
    };
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
