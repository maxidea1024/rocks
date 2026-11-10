#pragma once

#include <Object/ObjectHandle.h>

namespace rocks {

class ObjectRegistry;
class ConstructionContext;
class TraceVisitor;

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

protected:
    Object() noexcept = default;
    ~Object() = default;

private:
    friend class ObjectRegistry;
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
