#pragma once

#include <Base/TypeTraits.h>
#include <Core/Text/Name.h>
#include <Localization/Text.h>
#include <Object/BoundCoreValues.h>
#include <Object/ClassRef.h>
#include <Object/InstancedStruct.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>
#include <Object/ValueCopy.h>
#include <Platform/Memory.h>

// 리플렉션 값의 타입별 같음이다(Docs/Design/ChangeNotification.ko.md CN-D15 — RuntimeClassMetadata
// A5b의 인코딩 byte 같음과 같은 판정). 생성 setter·SetAndNotify(CN-D9·CN-D13)가 "같은 값이면 대입·
// 통지하지 않음"에 쓴다. ValueCopy(Object/ValueCopy.h)와 같은 형식 집합이다.
//
// 비교 규칙(ValueSame<T>):
//   - bool·정수·enum은 값 같음, 실수는 비트 같음(0.0과 -0.0은 다르고 NaN은 payload까지 본다).
//   - Name은 토큰과 표시 번호(대소문자가 다른 표시는 다르다).
//   - 그 밖의 단일 값 복사 형식(Guid·AssetId·Digest·ClassRef 등)은 그 형식의 ==다(==가 없으면
//     비교할 수 없다).
//   - WeakObjectRef는 대상 handle이고, null인 두 handle은 같다.
//   - String은 UTF-8 byte, SoftObjectRef는 자산 ID와 경로 byte다.
//   - Array·정적 배열은 길이와 원소별, OrderedMap·OrderedSet은 개수와 저장 순서대로 원소별(순서가
//     인코딩 순서다 — 같은 내용의 다른 순서는 다르다), Optional은 유무와 값이다.
//   - 생성 struct는 MetaCompiler가 내는 특수화가 GeneratedType<S>::SameValue(ROCKS_PROPERTY
//   멤버별)로
//     잇는다. ROCKS_NATIVE 멤버가 있는 struct는 늘 다르다(저장하지 않는 멤버만 다른 새 값을 같다고
//     보면 setter가 대입을 건너뛴다).
//   - Text는 이력 구조(Text::IsIdentical)다.
//   - InstancedStruct와 그 밖의 형식은 비교할 수 없다(Supported 거짓 — Same은 늘 거짓, "다르다").
// 비교는 할당하지 않는다.
namespace rocks::detail {

// T 값의 같음 방식이다(위 규칙). Supported가 거짓이면 Same은 늘 거짓이다.
template <class T>
struct ValueSame {
    // 생성 struct는 생성 헤더의 특수화가 받는다. 리플렉션 struct가 생성 헤더 없이 이 일반형으로
    // 오면 관문이 컴파일 오류를 낸다(Meta/GeneratedType.h).
    static_assert((GeneratedHeaderVisible<T>(), true));
    // Binds 미러의 실제 Core 값 타입(Vector3 등)도 생성 특수화(실수 비트 비교)가 보여야 한다 —
    // 일반형의
    // ==는 실수 ==라 번역 단위마다 같음이 갈린다.
    static_assert(
        !BoundCoreValueV<RemoveCvT<T>> || requires { sizeof(GeneratedType<T>); },
        "ValueSame of a Binds target of CoreTypeBindings needs its generated header: "
        "include <CoreTypeBindings/CoreValueTypes.generated.h> in this translation unit");

    static constexpr bool Supported = IsFloatingPointV<T> || IsIntegralV<T> || IsEnumV<T> ||
                                      (IsPlainCopyV<T> && requires(const T& left, const T& right) {
                                          static_cast<bool>(left == right);
                                      });

    static bool Same(const T& left, const T& right) noexcept {
        if constexpr (IsSameV<T, float>) {
            return platform::BitCast<uint32>(left) == platform::BitCast<uint32>(right);
        } else if constexpr (IsSameV<T, double>) {
            return platform::BitCast<uint64>(left) == platform::BitCast<uint64>(right);
        } else if constexpr (Supported) {
            return left == right;
        } else {
            (void)left;
            (void)right;
            return false;
        }
    }
};

// left와 right가 같은 값인지 본다(위 규칙 — 비교할 수 없는 형식은 거짓).
//
// 사용 예:
//   if (detail::SameValue(health, next)) {
//       return Result<bool>::Success(false);
//   }
template <class T>
bool SameValue(const T& left, const T& right) noexcept {
    return ValueSame<T>::Same(left, right);
}

template <>
struct ValueSame<String> {
    static constexpr bool Supported = true;

    static bool Same(const String& left, const String& right) noexcept {
        return left.View() == right.View();
    }
};

// Name은 토큰 같음에 표시 번호(displayId — 대소문자가 다른 표시)까지 본다: Name의 ==는 ASCII
// 대소문자를 무시하지만 저장하는 표시 문자열은 다르다(A5b 인코딩 같음 — CN1b SameFieldValue와
// 같다).
template <>
struct ValueSame<Name> {
    static constexpr bool Supported = true;

    static bool Same(Name left, Name right) noexcept {
        return left == right && left.displayId == right.displayId;
    }
};

template <>
struct ValueSame<Text> {
    static constexpr bool Supported = true;

    static bool Same(const Text& left, const Text& right) noexcept {
        return left.IsIdentical(right);
    }
};

template <class T>
struct ValueSame<WeakObjectRef<T>> {
    static constexpr bool Supported = true;

    static bool Same(const WeakObjectRef<T>& left, const WeakObjectRef<T>& right) noexcept {
        const ObjectHandle first = left.Handle();
        const ObjectHandle second = right.Handle();
        return first.IsNull() ? second.IsNull() : first == second;
    }
};

template <class T>
struct ValueSame<SoftObjectRef<T>> {
    static constexpr bool Supported = true;

    static bool Same(const SoftObjectRef<T>& left, const SoftObjectRef<T>& right) noexcept {
        return left.asset == right.asset && left.objectPath.View() == right.objectPath.View();
    }
};

template <class E>
struct ValueSame<Array<E>> {
    static constexpr bool Supported = ValueSame<E>::Supported;

    static bool Same(const Array<E>& left, const Array<E>& right) noexcept {
        if (left.Size() != right.Size()) {
            return false;
        }

        for (usize index = 0; index < left.Size(); ++index) {
            if (!ValueSame<E>::Same(left[index], right[index])) {
                return false;
            }
        }

        return true;
    }
};

template <class K, class V, class Hasher, class KeyEqual>
struct ValueSame<OrderedMap<K, V, Hasher, KeyEqual>> {
    static constexpr bool Supported = ValueSame<K>::Supported && ValueSame<V>::Supported;

    static bool Same(const OrderedMap<K, V, Hasher, KeyEqual>& left,
                     const OrderedMap<K, V, Hasher, KeyEqual>& right) noexcept {
        if (left.Count() != right.Count()) {
            return false;
        }

        auto other = right.begin();
        for (const auto& entry : left) {
            const auto& match = *other;
            if (!ValueSame<K>::Same(entry.key, match.key) ||
                !ValueSame<V>::Same(entry.value, match.value)) {
                return false;
            }

            ++other;
        }

        return true;
    }
};

template <class K, class Hasher, class KeyEqual>
struct ValueSame<OrderedSet<K, Hasher, KeyEqual>> {
    static constexpr bool Supported = ValueSame<K>::Supported;

    static bool Same(const OrderedSet<K, Hasher, KeyEqual>& left,
                     const OrderedSet<K, Hasher, KeyEqual>& right) noexcept {
        if (left.Count() != right.Count()) {
            return false;
        }

        auto other = right.begin();
        for (const K& key : left) {
            if (!ValueSame<K>::Same(key, *other)) {
                return false;
            }

            ++other;
        }

        return true;
    }
};

template <class T>
struct ValueSame<Optional<T>> {
    static constexpr bool Supported = ValueSame<T>::Supported;

    static bool Same(const Optional<T>& left, const Optional<T>& right) noexcept {
        if (left.HasValue() != right.HasValue()) {
            return false;
        }

        return !left.HasValue() || ValueSame<T>::Same(left.Value(), right.Value());
    }
};

template <class E, usize N>
struct ValueSame<E[N]> {
    static constexpr bool Supported = ValueSame<E>::Supported;

    static bool Same(const E (&left)[N], const E (&right)[N]) noexcept {
        for (usize index = 0; index < N; ++index) {
            if (!ValueSame<E>::Same(left[index], right[index])) {
                return false;
            }
        }

        return true;
    }
};

// InstancedStruct(리플렉션 B9j)는 담긴 타입이 정해지지 않아 비교하지 않는다(늘 다르다 — CN-D15).
template <class Base>
struct ValueSame<InstancedStruct<Base>> {
    static constexpr bool Supported = false;

    static bool Same(const InstancedStruct<Base>&, const InstancedStruct<Base>&) noexcept {
        return false;
    }
};

} // namespace rocks::detail
