#pragma once

#include <Meta/GeneratedType.h>
#include <Object/TypeRegistry.h>

// 메타컴파일러를 거치지 않는 네이티브 클래스의 타입 등록이다. 리플렉션 클래스(ROCKS_CLASS)는 소유
// 모듈의 Metadata 헤더에 선언하고 모든 필드가 리플렉션 형식(또는 ROCKS_NATIVE)이어야 한다. .cpp
// 안의 시험 타입이나 영속 상태가 없는 런타임 전용 파생은 Metadata 입력이 될 수 없거나 될 필요가
// 없으므로 ROCKS_NATIVE_TYPE으로 선언하고 RegisterNativeClass로 등록해 Object 생성·수명·상속
// 판정(Resolve<기반 타입>)과 GC Trace를 쓴다. native 클래스가 더한 필드는 리플렉션·객체 그래프
// 직렬화 대상이 아니다.
//
// 기반은 rocks::Object, 다른 native 클래스, 또는 리플렉션 클래스(그 생성 헤더를 include해야
// 한다)다. native 사슬 위에 리플렉션 조상 R이 있으면(Docs/Design/ReflectionFoundations.ko.md
// 9.1절 B8a):
// - 스키마는 R의 필드(R이 상속한 필드 포함)를 같은 ID로 담고 함수표는 R의 생성 thunk를
//   빌린다. 그래서 base 필드의 읽기·쓰기·경로 접근(Object/PropertyPath.h)이 파생 객체에 그대로
//   된다.
// - WorldOnly만 R에서 물려받는다(WorldOnly base의 파생에 플래그가 없으면 TypeRegistry가
//   거부한다). Abstract 비트는 T 자신의 C++ 추상 여부로만 정한다.
// - GC Trace는 native 사슬의 손 Trace를 부르다가 R에서 생성 Trace(GeneratedType<R>::Trace —
//   상속 필드 포함)를 부르고 멈춘다.
// - completeFieldLayout은 false다: native 파생의 C++ 상태를 모르므로 그 객체가 들어간 객체
//   그래프의 저장과 그 타입 노드의 로드는 Unsupported다(GC는 Trace만 쓴다). parallelTraceSafe도
//   false다(손 Trace는 소유 스레드에서만 부른다).
// - 빌린 thunk와 생성 Trace는 RegisterNativeClass<T>를 인스턴스화한 번역 단위에서 인스턴스화되므로
//   그 코드 수명은 T의 module lease가 지키고, base 스키마와 enum·struct 의존 레코드는 base 레코드의
//   dependents가 지킨다(파생이 등록되어 있으면 base의 Unregister는 Busy).
//
// 사용 예:
//   class SpinningActor : public rocks::Actor {
//       ROCKS_NATIVE_TYPE(SpinningActor, rocks::Actor, 0x5b1c7d2e8a4f4c10ull,
//                         0x9e02b6a1d3c84f57ull)
//   public:
//       explicit SpinningActor(rocks::Allocator& allocator) noexcept;
//   };
//
//   // 기반 타입을 먼저 등록한다(TypeRegistry 규칙 — 리플렉션 기반은 RegisterModuleTypes가
//   // 등록한다). 실패하면 AlreadyExists·NotFound 등을 반환한다.
//   auto token = rocks::RegisterNativeClass<SpinningActor>(types);
//   auto actor = objects.NewObject<SpinningActor>(access, rocks::ObjectTypeIdOf<SpinningActor>());
//
// - TypeId는 저장소 전체에서 유일한 128비트 값이다. 새 클래스마다 무작위 값을 발급하고 재사용하지
//   않는다.
// - 매크로는 `public:` 접근 지정을 남긴다. 매크로 뒤의 멤버 접근 지정은 직접 다시 쓴다.
// - 단일 public 비가상 상속만 지원한다.
// - 파생 클래스가 매크로를 빠뜨리면 기반의 ID를 물려받는다. NativeClassType concept이 이를
//   거부하므로 NativeTypeIdOf·RegisterNativeClass(그리고 이를 쓰는 World Spawn·AddComponent)가
//   컴파일되지 않고, ObjectTypeIdOf는 안내 메시지가 있는 static_assert로 멈춘다.
// - 객체 안에 다른 객체로의 강한 간선(ObjectRef·ObjectRefArray)을 두면
//   `void Trace(TraceVisitor&) const noexcept`를 정의해 자기 간선을 모두 방문한다. 등록이 기반부터
//   각 클래스가 직접 선언한 Trace를 차례로 부르므로 Super::Trace를 부르지 않는다(리플렉션 기반에는
//   Trace 멤버가 없다). 접근 지정은 자유다(private·protected도 부른다 — 매크로가
//   detail::NativeClassAccess를 friend로 선언한다). Trace라는 이름을 직접 선언했으면 정확히 이
//   시그니처여야 한다(const·noexcept 누락, 다른 매개변수, static, 오버로드·템플릿은 컴파일 오류).
// - native 클래스 안에는 ROCKS_PROPERTY·ROCKS_NATIVE를 쓰지 않는다. MetaCompiler가 보지 않아
//   attribute로만 남고 추적·저장되지 않는 필드가 되므로 policy(reflection-annotation-native-type)가
//   거부한다.
#define ROCKS_NATIVE_TYPE(Type, Base, IdHigh, IdLow)                                               \
    friend struct ::rocks::detail::NativeClassAccess;                                              \
                                                                                                   \
public:                                                                                            \
    using ThisClass = Type;                                                                        \
    using Super = Base;                                                                            \
    static constexpr ::rocks::TypeId StaticTypeId{IdHigh, IdLow};                                  \
    static constexpr char StaticTypeName[] = #Type;

namespace rocks {

// ROCKS_NATIVE_TYPE를 선언한 Object 파생 클래스다. T 자신의 매크로 멤버(ThisClass가 T)로 판정하므로
// 생성 헤더 include 여부와 무관하게 번역 단위마다 결과가 같다.
template <class T>
concept NativeClassType = __is_base_of(Object, T) && requires {
    typename T::ThisClass;
    typename T::Super;
    T::StaticTypeId;
    T::StaticTypeName;
} && __is_same(typename T::ThisClass, T);

namespace detail {

// native 클래스의 Super 사슬에서 처음 만나는 리플렉션 조상을 찾는다(Type). U가 native면 Super로
// 올라가고, native가 아닌 첫 조상이 Object면 리플렉션 조상이 없다(void). 그 밖의 조상은 리플렉션
// 클래스여야 한다. ROCKS_NATIVE_TYPE 매크로 멤버로만 판정한다 — 생성 헤더 유무로 갈리는 탐지 형질을
// 쓰지 않으므로, 생성 헤더가 보이지 않으면 조용히 다른 길로 가지 않고 static_assert가 include를
// 안내한다.
template <class U, bool Native = NativeClassType<U>>
struct ReflectedAncestorOf;

template <class U>
struct ReflectedAncestorOf<U, true> {
    using Type = typename ReflectedAncestorOf<typename U::Super>::Type;
};

template <class U>
struct ReflectedAncestorOf<U, false> {
    static_assert(__is_base_of(Object, U), "native class base must derive from rocks::Object");
    static_assert(
        requires { GeneratedType<U>::Schema(); },
        "native class base is neither a ROCKS_NATIVE_TYPE class (did it forget the macro?) "
        "nor a reflected ROCKS_CLASS whose generated header is included; include "
        "<Module/Type.generated.h> or the module aggregate header before RegisterNativeClass");
    using Type = U;
};

template <>
struct ReflectedAncestorOf<Object, false> {
    using Type = void;
};

// native 클래스 T의 Super 사슬에서 처음 만나는 리플렉션 조상 R이다(사슬이 Object에서 끝나면 void).
template <NativeClassType T>
using ReflectedAncestor = typename ReflectedAncestorOf<typename T::Super>::Type;

// 멤버 포인터 타입의 클래스다(멤버 포인터가 아니면 void).
template <class Pointer>
struct MemberPointerClass {
    using Type = void;
};

template <class Member, class Class>
struct MemberPointerClass<Member Class::*> {
    using Type = Class;
};

// native 클래스의 손 Trace에 닿는 접근자다. ROCKS_NATIVE_TYPE이 이 타입을 friend로 선언하므로
// private·protected Trace도 찾고 부른다(공개 이름만 보면 접근할 수 없는 Trace를 조용히 건너뛰어
// GC 구멍이 된다). 직접 쓰지 않는다.
struct NativeClassAccess {
    // native 클래스 U가 직접 선언한 Trace의 상태다. None은 Trace라는 이름이 없거나 기반에서
    // 물려받은 것(그 기반 단계가 부른다), Own은 정확히 `void Trace(TraceVisitor&) const noexcept`,
    // Invalid는 그 밖(다른 시그니처·static 함수·데이터 멤버, 또는 오버로드·템플릿이라 주소가 하나로
    // 정해지지 않는데 호출할 수 있는 이름)이다.
    enum class TraceDeclaration : uint8 {
        None,
        Own,
        Invalid
    };

    template <class U>
    static constexpr TraceDeclaration TraceOf() noexcept {
        if constexpr (requires { &U::Trace; }) {
            using Pointer = decltype(&U::Trace);
            using Owner = typename MemberPointerClass<Pointer>::Type;
            if constexpr (__is_same(Owner, void)) {
                return TraceDeclaration::Invalid;
            } else if constexpr (!__is_same(Owner, U)) {
                return TraceDeclaration::None;
            } else if constexpr (__is_same(Pointer, void (U::*)(TraceVisitor&) const noexcept)) {
                return TraceDeclaration::Own;
            } else {
                return TraceDeclaration::Invalid;
            }
        } else if constexpr (requires(const U& value, TraceVisitor& visitor) {
                                 value.Trace(visitor);
                             }) {
            return TraceDeclaration::Invalid;
        } else {
            return TraceDeclaration::None;
        }
    }

    // U와 Super 사슬의 native 클래스가 모두 Trace를 올바르게 선언했는지(또는 선언하지 않았는지)다.
    template <class U>
    static constexpr bool ValidTraceChain() noexcept {
        if constexpr (NativeClassType<U>) {
            return TraceOf<U>() != TraceDeclaration::Invalid &&
                   ValidTraceChain<typename U::Super>();
        } else {
            return true;
        }
    }

    template <class U>
    static void Trace(const U& object, TraceVisitor& visitor) noexcept {
        object.U::Trace(visitor);
    }
};

// 기반부터 파생 순서로, 각 native 클래스가 직접 선언한 Trace(자기 필드만 방문)를 부른다. 파생
// 클래스가 Super::Trace를 부르지 않아도 기반의 간선이 빠지지 않는다(불러도 같은 간선을 두 번 방문할
// 뿐이다). 사슬이 리플렉션 조상 R에 닿으면 R의 생성 Trace(상속 필드 포함)를 부르고 멈춘다 —
// ROCKS_CLASS는 손 Trace와 강한 참조 NATIVE 멤버를 금지하므로 R 위로는 생성 Trace가 방문하지 않은
// 간선이 없다.
template <class U>
void TraceNativeChain(const U& object, TraceVisitor& visitor) noexcept {
    if constexpr (NativeClassType<U>) {
        TraceNativeChain<typename U::Super>(object, visitor);
        if constexpr (NativeClassAccess::TraceOf<U>() == NativeClassAccess::TraceDeclaration::Own) {
            NativeClassAccess::Trace<U>(object, visitor);
        }
    } else if constexpr (!__is_same(U, Object)) {
        GeneratedType<U>::Trace(object, visitor);
    }
}

} // namespace detail

// native 클래스 T의 정적 TypeId다. ROCKS_NATIVE_TYPE 선언이 필요하다. 등록 여부와 무관한 값이다.
// native·리플렉션 타입을 함께 받는 코드는 ObjectTypeIdOf를 쓴다.
template <NativeClassType T>
constexpr TypeId NativeTypeIdOf() noexcept {
    return T::StaticTypeId;
}

// Object 파생 타입 T의 정적 TypeId다(등록 여부와 무관한 값). T가 ROCKS_NATIVE_TYPE을 선언했으면(T
// 자신의 매크로 멤버로 판정 — 번역 단위마다 같다) T::StaticTypeId, 아니면 리플렉션 타입의 생성
// ID(TypeIdOf<T>() — 생성 헤더를 include해야 한다)다. 둘 다 아니면 안내 메시지가 있는
// static_assert로 컴파일을 멈춘다(native 파생이 ROCKS_NATIVE_TYPE을 빠뜨린 경우 "incomplete type"
// 대신). native 파생과 리플렉션 타입을 함께 받는 코드(World의 Spawn·AddComponent 등)가 쓴다.
//
// 사용 예:
//   auto actor = objects.NewObject<Actor>(access, rocks::ObjectTypeIdOf<SpinningActor>());
template <class T>
constexpr TypeId ObjectTypeIdOf() noexcept {
    static_assert(__is_base_of(Object, T), "ObjectTypeIdOf requires an Object-derived type");
    if constexpr (NativeClassType<T>) {
        return T::StaticTypeId;
    } else if constexpr (requires { GeneratedType<T>::Id(); }) {
        return GeneratedType<T>::Id();
    } else {
        static_assert(
            requires { GeneratedType<T>::Id(); },
            "ObjectTypeIdOf<T> needs ROCKS_NATIVE_TYPE declared in T itself, or a reflected "
            "ROCKS_CLASS T whose generated header is included");
        return {};
    }
}

// T를 등록한다. 성공하면 등록 토큰을 반환하며, 이 토큰으로 Unregister할 수 있다(파생이 남아
// 있으면 Busy).
// - 기반 사슬에 리플렉션 조상이 없으면 필드 없는 스키마다. 리플렉션 조상 R이 있으면 R의 스키마
//   필드와 생성 함수표를 빌리고(R의 thunk는 Resolve<R>·construction.Get<R>의 IsNativeA 판정이라
//   파생 객체에 맞다) R의 WorldOnly를 물려받으며 completeFieldLayout·parallelTraceSafe는
//   false다(이 타입 객체의 그래프 저장은 Unsupported).
// - GC Trace는 상속 사슬의 각 native 클래스가 직접 선언한
//   `void Trace(TraceVisitor&) const noexcept`(접근 지정 무관)를 기반부터 차례로 부르고, 리플렉션
//   조상이 있으면 그 생성 Trace에서 멈춘다. 사슬의 어느 native 클래스든 Trace라는 이름을 다른
//   모양으로 선언했으면(const·noexcept 누락, 다른 매개변수, static, 오버로드·템플릿) 컴파일 시
//   거부한다.
// - 기반 타입이 아직 등록되지 않았으면 NotFound, 같은 ID가 이미 있으면 AlreadyExists, 종료 중이면
//   ShuttingDown이다. 리플렉션 조상의 enum·struct 필드가 가리키는 타입도 먼저 등록되어 있어야
//   한다(RegisterModuleTypes가 등록한다 — 없으면 NotFound). 실패해도 기존 등록 상태는 바뀌지
//   않는다.
// - 등록 스레드 규약은 TypeRegistry를 따른다. module은 동적 모듈의 코드 수명을 유지할 때
//   넘긴다(정적 링크면 생략). 빌린 base thunk와 생성 Trace는 GeneratedType<R>의 inline 함수라 이
//   번역 단위에서 인스턴스화되므로 그 코드 수명도 module(T의 module lease)이 지킨다. base 스키마와
//   enum·struct 의존 레코드는 base 레코드가 지킨다 — 이 등록이 base의 dependent이므로 base가 먼저
//   해제되지 않는다.
template <NativeClassType T>
Result<TypeRegistrationToken> RegisterNativeClass(TypeRegistry& types,
                                                  ModuleLease module = {}) noexcept {
    static_assert(__is_same(typename T::ThisClass, T),
                  "ROCKS_NATIVE_TYPE is missing in this class; it inherited its base's identity");
    using Base = typename T::Super;
    static_assert(__is_base_of(Base, T) && !__is_same(Base, T));
    // native 사슬 위의 첫 리플렉션 조상이다(없으면 void). 기반이 native도 리플렉션 클래스도 아니면
    // ReflectedAncestorOf의 static_assert가 안내한다.
    using Reflected = detail::ReflectedAncestor<T>;
    static_assert(
        detail::NativeClassAccess::ValidTraceChain<T>(),
        "a native class in this Super chain declares Trace in another form; declare exactly "
        "`void Trace(TraceVisitor&) const noexcept` (any access) or no member named Trace "
        "(overloads, templates, static and other signatures are not traced)");

    TypeSchema schema;
    schema.id = T::StaticTypeId;
    schema.name = Span<const char>(T::StaticTypeName, sizeof(T::StaticTypeName) - 1);
    schema.flags = __is_abstract(T) ? static_cast<uint32>(TypeFlags::Abstract) : 0u;
    RuntimeTypeOps ops = MakeRuntimeTypeOps<T>(Move(module));
    ops.trace = [](const Object& object, TraceVisitor& visitor) noexcept {
        detail::TraceNativeChain<T>(static_cast<const T&>(object), visitor);
    };
    if constexpr (!__is_same(Reflected, void)) {
        // 리플렉션 조상의 필드를 같은 ID·표현으로 담고(TypeRegistry의 상속 필드 대조) 그 생성
        // thunk를 빌린다. 스키마와 함수표는 TypeRegistry가 깊게 복사한다. WorldOnly는 상속되는
        // 플래그다(생성 스키마의 flags는 상속한 WorldOnly를 포함한다).
        const TypeSchema& reflected = GeneratedType<Reflected>::Schema();
        schema.fields = reflected.fields;
        schema.flags |= reflected.flags & static_cast<uint32>(TypeFlags::WorldOnly);
        ops.fields = GeneratedType<Reflected>::Ops().fields;
        ops.completeFieldLayout = false;
        ops.parallelTraceSafe = false;
    }

    if constexpr (!__is_same(Base, Object)) {
        schema.baseType = ObjectTypeIdOf<Base>();
        ops.nativeBaseType = NativeTypeKey<Base>();
    }

    return types.Register(schema, Move(ops));
}

} // namespace rocks
