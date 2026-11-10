#pragma once

#include <Core/Result.h>
#include <Object/ObjectHandle.h>

namespace rocks {

class ObjectRegistry;
class ConstructionContext;
class TraceVisitor;

// 레지스트리에서 관찰한 한 시점의 논리 상태다. 값은 객체 수명이나 접근권을 보존하지 않는다.
// Unreachable은 증분 GC cutoff에서 아직 Retired로 분류되지 않은 white 객체다.
// Retired는 명시 파괴와 GC 회수 중 어느 경로인지 구별하지 않는다.
enum class ObjectLifecycle : uint8 {
    Alive,
    DestroyRequested,
    Unreachable,
    Retired
};

// Registry가 식별자와 저장소를 소유하는 기반 타입. 직접 delete하지 않는다.
// 복사·이동은 객체 정체성과 ObjectRef 소유자 검사를 우회하므로 금지한다.
// 파생 타입의 정확한 소멸자는 RuntimeTypeOps를 통해 호출한다.
class Object {
public:
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&&) = delete;
    Object& operator=(Object&&) = delete;

    ObjectHandle Handle() const noexcept {
        return handle_;
    }

    // 소유 스레드와 이 객체의 활성 Borrow가 필요하다. 실행 scope가 있으면 해당 문맥을 검사하고,
    // 없으면 기본 설정에서는 이 객체의 Registry 문맥을 사용한다. Registry가 엄격 모드라면 scope
    // 누락은 실패한다. scope가 없을 때 호출자의 World는 검증할 수 없다. 생성자 안과 미공개
    // 상태는 실패한다. 같은 Borrow 안에서 이미 요청된 파괴의 재요청은 성공한다.
    Result<void> Destroy() noexcept;

    // snapshot은 수명이나 접근권을 보존하지 않는다. 논리 파괴 이후에도 같은 Borrow가 살아 있는
    // 동안 이 함수와 IsDestroyRequested/중복 Destroy만 사용할 수 있다. 다른 객체 접근은 금지다.
    Result<ObjectLifecycle> InspectLifecycle() const noexcept;
    // cutoff/retired에서는 명시 요청 여부를 보존하지 않으므로 false로 추측하지 않고 실패한다.
    // Result 자체의 bool 변환은 조회 성공 여부다. 실제 파괴 요청 여부는 성공 확인 후 Value()로
    // 읽는다.
    Result<bool> IsDestroyRequested() const noexcept;

protected:
    Object() noexcept = default;
    ~Object() = default;

private:
    friend class ObjectRegistry;
    // 생성자 반환 후 설정하며 물리 소멸까지 변경하지 않는다. Registry가 객체보다 오래 산다.
    ObjectRegistry* registry_ = nullptr;
    ObjectHandle handle_;
};

template <class T>
inline const uint8 NativeTypeIdentity = 0;

// 현재 실행 이미지 안의 정확한 C++ 타입 비교용 키다. 직렬화하거나 ABI에 노출하지 않는다.
// Object의 nullptr 키는 기반 타입 조회를 뜻한다. 일반 IsA/상속 탐색은 아직 없다.
// 동적 모듈 사이의 키 통합은 보장하지 않으며 별도 ABI 계약이 필요하다.
template <class T>
const void* NativeTypeKey() noexcept {
    if constexpr (__is_same(T, Object)) {
        return nullptr;
    } else {
        return &NativeTypeIdentity<T>;
    }
}

} // namespace rocks
