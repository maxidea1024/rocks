#pragma once

#include <Base/PlacementNew.h>
#include <Base/TypeTraits.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/RuntimeTypeOps.h>

// 리플렉션 값의 타입 무관 깊은 복사·교환·생성·소멸이다(리플렉션 B9i3c0 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절 "구현(B9i3c0)에서 정한 점"). UE의
// FProperty::CopyCompleteValue·InitializeValue·DestroyValue와 UScriptStruct::ICppStructOps
// 대응이다. 대입 묶음이 값 부분 트리의 사본을 만들고(TryCopyValue — 실패하면 아무것도 바뀌지
// 않는다) 게시 때 무할당으로 옮긴다(SwapValues).
//
// 복사 규칙(ValueCopy<T>):
//   - trivially copyable(단일 값·Guid·AssetId·Digest·Name 토큰·WeakObjectRef·ClassRef·enum)은
//   대입이다.
//   - String·SoftObjectRef는 TryClone이다.
//   - Array·OrderedMap·OrderedSet·Optional·정적 배열(E[N])은 원소·키·값마다 복사한다.
//   - InstancedStruct는 TryClone이다(리플렉션 B9j).
//   - Text는 DeepClone이다(리플렉션 B9l — 노드를 allocator로 새로 만든다).
//   - 생성 struct는 MetaCompiler가 내는 특수화가 GeneratedType<S>::CopyValue(PROPERTY·NATIVE 멤버
//     모두)로 잇는다.
//   - 그 밖의 C++ 타입은 noexcept 복사 생성·대입이 있으면 C++ 복사, 없으면 지원하지 않는다
//     (Supported 거짓 — TryCopyValue가 Unsupported). ObjectRef·ObjectRefArray·StructRefArray·
//     ObjectRefMap은 복사할 수 없다(복사·이동이 막혀 있다 — 강한 참조는 대입 묶음의 간선 변경
//     단위다).
namespace rocks::detail {

// 대입 한 번이 깊은 복사인 타입이다. trivially copyable만으로는 모자라다: 복사·이동을 모두 지운
// 타입(ObjectRef)도 컴파일러 형질이 trivially copyable로 답할 수 있으므로 복사 생성·대입 가능을
// 함께 본다.
template <class T>
inline constexpr bool IsPlainCopyV =
    IsTriviallyCopyableV<T> && IsCopyConstructibleV<T> && IsCopyAssignableV<T>;

// T 값의 깊은 복사 방식이다(위 규칙). Supported가 거짓이면 Copy를 부르지 않는다.
template <class T>
struct ValueCopy {
    // 생성 struct는 생성 헤더의 특수화가 받는다. 리플렉션 struct가 생성 헤더 없이 이 일반형으로
    // 오면 관문이 컴파일 오류를 낸다(Meta/GeneratedType.h).
    static_assert((GeneratedHeaderVisible<T>(), true));

    static constexpr bool Supported =
        IsPlainCopyV<T> || (IsNothrowCopyConstructibleV<T> && IsNothrowCopyAssignableV<T>);

    static Result<void> Copy(T& destination, const T& source, Allocator&) noexcept {
        destination = source;
        return Result<void>::Success();
    }
};

// destination(살아 있는 값)을 source의 깊은 복사로 바꾼다. 새 값은 allocator로 따로 다 만든 뒤
// 무할당 이동으로 넣으므로, 실패하면(OutOfMemory·Overflow 등) destination·source 모두 그대로다.
// 복사할 수 없는 타입(ValueCopy<T>::Supported 거짓)은 Unsupported다.
//
// 사용 예:
//   Array<String> copy(allocator);
//   if (auto copied = detail::TryCopyValue(copy, source, allocator); !copied) {
//       return copied;   // copy는 빈 배열 그대로
//   }
template <class T>
Result<void> TryCopyValue(T& destination, const T& source, Allocator& allocator) noexcept {
    if constexpr (ValueCopy<T>::Supported) {
        return ValueCopy<T>::Copy(destination, source, allocator);
    } else {
        (void)destination;
        (void)source;
        (void)allocator;
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
}

template <>
struct ValueCopy<String> {
    static constexpr bool Supported = true;

    static Result<void> Copy(String& destination, const String& source,
                             Allocator& allocator) noexcept {
        auto cloned = source.TryClone(allocator);
        if (!cloned) {
            return Result<void>::Failure(cloned.Error());
        }

        destination = Move(cloned.Value());
        return Result<void>::Success();
    }
};

template <class T>
struct ValueCopy<SoftObjectRef<T>> {
    static constexpr bool Supported = true;

    static Result<void> Copy(SoftObjectRef<T>& destination, const SoftObjectRef<T>& source,
                             Allocator& allocator) noexcept {
        auto cloned = source.TryClone(allocator);
        if (!cloned) {
            return Result<void>::Failure(cloned.Error());
        }

        destination = Move(cloned.Value());
        return Result<void>::Success();
    }
};

template <class E>
struct ValueCopy<Array<E>> {
    static constexpr bool Supported = ValueCopy<E>::Supported;

    static Result<void> Copy(Array<E>& destination, const Array<E>& source,
                             Allocator& allocator) noexcept {
        Array<E> copy(allocator);
        if constexpr (IsPlainCopyV<E>) {
            if (auto appended = copy.TryAppend(source.View()); !appended) {
                return appended;
            }
        } else {
            if (auto reserved = copy.TryReserve(source.Size()); !reserved) {
                return reserved;
            }

            for (usize index = 0; index < source.Size(); ++index) {
                E element = MakeAllocatedValue<E>(allocator);
                if (auto copied = ValueCopy<E>::Copy(element, source[index], allocator); !copied) {
                    return copied;
                }

                if (auto pushed = copy.TryPushBack(Move(element)); !pushed) {
                    return pushed;
                }
            }
        }

        destination = Move(copy);
        return Result<void>::Success();
    }
};

template <class K, class V, class Hasher, class KeyEqual>
struct ValueCopy<OrderedMap<K, V, Hasher, KeyEqual>> {
    static constexpr bool Supported = ValueCopy<K>::Supported && ValueCopy<V>::Supported;

    static Result<void> Copy(OrderedMap<K, V, Hasher, KeyEqual>& destination,
                             const OrderedMap<K, V, Hasher, KeyEqual>& source,
                             Allocator& allocator) noexcept {
        OrderedMap<K, V, Hasher, KeyEqual> copy(allocator);
        if (auto reserved = copy.Reserve(source.Count()); !reserved) {
            return reserved;
        }

        // ForEach는 멈출 수 없으므로 첫 실패 뒤의 항목은 건너뛴다.
        ErrorCode failure = ErrorCode::None;
        source.ForEach([&](const K& key, const V& value) noexcept {
            if (failure != ErrorCode::None) {
                return;
            }

            K keyCopy = MakeAllocatedValue<K>(allocator);
            V valueCopy = MakeAllocatedValue<V>(allocator);
            if (auto copied = ValueCopy<K>::Copy(keyCopy, key, allocator); !copied) {
                failure = copied.Error();
                return;
            }

            if (auto copied = ValueCopy<V>::Copy(valueCopy, value, allocator); !copied) {
                failure = copied.Error();
                return;
            }

            if (auto inserted = copy.Insert(Move(keyCopy), Move(valueCopy)); !inserted) {
                failure = inserted.Error();
            }
        });
        if (failure != ErrorCode::None) {
            return Result<void>::Failure(failure);
        }

        destination = Move(copy);
        return Result<void>::Success();
    }
};

template <class K, class Hasher, class KeyEqual>
struct ValueCopy<OrderedSet<K, Hasher, KeyEqual>> {
    static constexpr bool Supported = ValueCopy<K>::Supported;

    static Result<void> Copy(OrderedSet<K, Hasher, KeyEqual>& destination,
                             const OrderedSet<K, Hasher, KeyEqual>& source,
                             Allocator& allocator) noexcept {
        OrderedSet<K, Hasher, KeyEqual> copy(allocator);
        if (auto reserved = copy.Reserve(source.Count()); !reserved) {
            return reserved;
        }

        ErrorCode failure = ErrorCode::None;
        source.ForEach([&](const K& key) noexcept {
            if (failure != ErrorCode::None) {
                return;
            }

            K keyCopy = MakeAllocatedValue<K>(allocator);
            if (auto copied = ValueCopy<K>::Copy(keyCopy, key, allocator); !copied) {
                failure = copied.Error();
                return;
            }

            if (auto added = copy.Add(Move(keyCopy)); !added) {
                failure = added.Error();
            }
        });
        if (failure != ErrorCode::None) {
            return Result<void>::Failure(failure);
        }

        destination = Move(copy);
        return Result<void>::Success();
    }
};

template <class T>
struct ValueCopy<Optional<T>> {
    static constexpr bool Supported = ValueCopy<T>::Supported;

    static Result<void> Copy(Optional<T>& destination, const Optional<T>& source,
                             Allocator& allocator) noexcept {
        if (!source.HasValue()) {
            destination.Reset();
            return Result<void>::Success();
        }

        T copy = MakeAllocatedValue<T>(allocator);
        if (auto copied = ValueCopy<T>::Copy(copy, source.Value(), allocator); !copied) {
            return copied;
        }

        destination.Emplace(Move(copy));
        return Result<void>::Success();
    }
};

// InstancedStruct<Base>(리플렉션 B9j)는 TryClone(새 lease·새 값 블록·담긴 struct의 깊은 복사 —
// RuntimeStructOps::copy가 없는 struct는 Unsupported)이다.
template <class Base>
struct ValueCopy<InstancedStruct<Base>> {
    static constexpr bool Supported = true;

    static Result<void> Copy(InstancedStruct<Base>& destination,
                             const InstancedStruct<Base>& source, Allocator& allocator) noexcept {
        auto cloned = source.TryClone(allocator);
        if (!cloned) {
            return Result<void>::Failure(cloned.Error());
        }

        destination = Move(cloned.Value());
        return Result<void>::Success();
    }
};

// Text(리플렉션 B9l)는 DeepClone(allocator로 노드를 새로 만든다 — 원래 노드를 공유하지 않는다)이다.
template <>
struct ValueCopy<Text> {
    static constexpr bool Supported = true;

    static Result<void> Copy(Text& destination, const Text& source, Allocator& allocator) noexcept {
        auto cloned = source.DeepClone(allocator);
        if (!cloned) {
            return Result<void>::Failure(cloned.Error());
        }

        destination = Move(cloned.Value());
        return Result<void>::Success();
    }
};

template <class E, usize N>
struct ValueCopy<E[N]> {
    static constexpr bool Supported =
        ValueCopy<E>::Supported && (IsPlainCopyV<E> || IsNothrowMoveAssignableV<E>);

    static Result<void> Copy(E (&destination)[N], const E (&source)[N],
                             Allocator& allocator) noexcept {
        if constexpr (IsPlainCopyV<E>) {
            for (usize index = 0; index < N; ++index) {
                destination[index] = source[index];
            }
        } else {
            // 원소를 모두 따로 만든 뒤 옮긴다(중간 실패에 destination이 반만 바뀌지 않게).
            Array<E> copies(allocator);
            if (auto reserved = copies.TryReserve(N); !reserved) {
                return reserved;
            }

            for (usize index = 0; index < N; ++index) {
                E element = MakeAllocatedValue<E>(allocator);
                if (auto copied = ValueCopy<E>::Copy(element, source[index], allocator); !copied) {
                    return copied;
                }

                if (auto pushed = copies.TryPushBack(Move(element)); !pushed) {
                    return pushed;
                }
            }

            for (usize index = 0; index < N; ++index) {
                destination[index] = Move(copies[index]);
            }
        }

        return Result<void>::Success();
    }
};

// T 값의 무할당 교환 방식이다: nothrow 이동 생성·대입이 있으면 세 번 이동, 정적 배열은 원소마다.
template <class T>
struct ValueSwap {
    static constexpr bool Supported = IsNothrowMoveConstructibleV<T> && IsNothrowMoveAssignableV<T>;

    static void Swap(T& first, T& second) noexcept {
        T held(Move(first));
        first = Move(second);
        second = Move(held);
    }
};

template <class E, usize N>
struct ValueSwap<E[N]> {
    static constexpr bool Supported = ValueSwap<E>::Supported;

    static void Swap(E (&first)[N], E (&second)[N]) noexcept {
        for (usize index = 0; index < N; ++index) {
            ValueSwap<E>::Swap(first[index], second[index]);
        }
    }
};

// struct 검증 hook(RuntimeStructOps::validate — Docs/Design/CurveData/README.ko.md 9.1절)의 결과를
// 계약 값으로 맞춘다: 성공은 그대로, 그 밖의 모든 오류는 InvalidArgument다.
inline Result<void> StructValidationResult(const Result<void>& result) noexcept {
    return result ? Result<void>::Success() : Result<void>::Failure(ErrorCode::InvalidArgument);
}

// T 값 트리 안의 검증 hook 방식이다(Docs/Design/CurveData/README.ko.md 9.1절). HasHook은 트리
// 어딘가에 hook 있는 struct가 있을 수 있는지(InstancedStruct는 담긴 타입이 정해지지 않으므로 늘
// 참)이고, 거짓이면 Run을 부르지 않는다. Run은 안쪽부터 검증하고 첫 거부(InvalidArgument)에서
// 멈춘다. 생성 struct는 MetaCompiler가 내는 특수화가 GeneratedType<S>::ValidateTree로 잇는다. 그
// 밖의 타입은 hook이 없다.
template <class T>
struct ValueValidate {
    // 생성 struct는 생성 헤더의 특수화가 받는다. 리플렉션 struct가 생성 헤더 없이 이 일반형으로
    // 오면 관문이 컴파일 오류를 낸다(Meta/GeneratedType.h).
    static_assert((GeneratedHeaderVisible<T>(), true));

    static constexpr bool HasHook = false;

    static Result<void> Run(const T&) noexcept {
        return Result<void>::Success();
    }
};

template <class E>
struct ValueValidate<Array<E>> {
    static constexpr bool HasHook = ValueValidate<E>::HasHook;

    static Result<void> Run(const Array<E>& value) noexcept {
        for (usize index = 0; index < value.Size(); ++index) {
            if (auto checked = ValueValidate<E>::Run(value[index]); !checked) {
                return checked;
            }
        }

        return Result<void>::Success();
    }
};

template <class K, class V, class Hasher, class KeyEqual>
struct ValueValidate<OrderedMap<K, V, Hasher, KeyEqual>> {
    static constexpr bool HasHook = ValueValidate<K>::HasHook || ValueValidate<V>::HasHook;

    static Result<void> Run(const OrderedMap<K, V, Hasher, KeyEqual>& value) noexcept {
        // ForEach는 멈출 수 없으므로 첫 거부 뒤의 항목은 건너뛴다.
        bool rejected = false;
        value.ForEach([&](const K& key, const V& mapped) noexcept {
            if (rejected) {
                return;
            }

            rejected = !ValueValidate<K>::Run(key) || !ValueValidate<V>::Run(mapped);
        });
        return rejected ? Result<void>::Failure(ErrorCode::InvalidArgument)
                        : Result<void>::Success();
    }
};

template <class K, class Hasher, class KeyEqual>
struct ValueValidate<OrderedSet<K, Hasher, KeyEqual>> {
    static constexpr bool HasHook = ValueValidate<K>::HasHook;

    static Result<void> Run(const OrderedSet<K, Hasher, KeyEqual>& value) noexcept {
        bool rejected = false;
        value.ForEach([&](const K& key) noexcept {
            if (!rejected) {
                rejected = !ValueValidate<K>::Run(key);
            }
        });
        return rejected ? Result<void>::Failure(ErrorCode::InvalidArgument)
                        : Result<void>::Success();
    }
};

template <class T>
struct ValueValidate<Optional<T>> {
    static constexpr bool HasHook = ValueValidate<T>::HasHook;

    static Result<void> Run(const Optional<T>& value) noexcept {
        return value.HasValue() ? ValueValidate<T>::Run(value.Value()) : Result<void>::Success();
    }
};

template <class E, usize N>
struct ValueValidate<E[N]> {
    static constexpr bool HasHook = ValueValidate<E>::HasHook;

    static Result<void> Run(const E (&value)[N]) noexcept {
        for (usize index = 0; index < N; ++index) {
            if (auto checked = ValueValidate<E>::Run(value[index]); !checked) {
                return checked;
            }
        }

        return Result<void>::Success();
    }
};

// InstancedStruct(리플렉션 B9j)는 담긴 struct의 등록 validateTree다(비어 있거나 트리에 hook이
// 없으면 통과).
template <class Base>
struct ValueValidate<InstancedStruct<Base>> {
    static constexpr bool HasHook = true;

    static Result<void> Run(const InstancedStruct<Base>& value) noexcept {
        if (value.IsEmpty()) {
            return Result<void>::Success();
        }

        const RuntimeStructOps& ops = value.Type().StructOps();
        if (!ops.validateTree) {
            return Result<void>::Success();
        }

        return StructValidationResult(ops.validateTree(InstancedStructAccess::Value(value)));
    }
};

// value(T 값) 트리 안의 hook 있는 struct를 안쪽부터 검증한다(hook이 없으면 아무것도 하지 않는다).
template <class T>
Result<void> ValidateValueTree(const T& value) noexcept {
    if constexpr (ValueValidate<T>::HasHook) {
        return ValueValidate<T>::Run(value);
    } else {
        (void)value;
        return Result<void>::Success();
    }
}

// 두 값을 무할당·무실패로 맞바꾼다(버퍼는 각자의 allocator와 함께 옮겨 간다). 교환할 수 없는
// 타입은 컴파일 오류다(ValueSwap<T>::Supported).
template <class T>
void SwapValues(T& first, T& second) noexcept {
    static_assert(ValueSwap<T>::Supported,
                  "SwapValues needs nothrow move construction and assignment");
    ValueSwap<T>::Swap(first, second);
}

// storage에 T의 기본 값을 만든다(MakeAllocatedValue와 같은 규칙 — 정적 배열은 원소마다).
template <class T>
void ConstructValueAt(void* storage, Allocator& allocator) noexcept {
    if constexpr (IsBoundedArrayV<T>) {
        using E = RemoveExtentT<T>;
        E* elements = static_cast<E*>(storage);
        for (usize index = 0; index < sizeof(T) / sizeof(E); ++index) {
            ConstructValueAt<E>(elements + index, allocator);
        }
    } else if constexpr (IsConstructibleV<T, Allocator&>) {
        static_assert(IsNothrowConstructibleV<T, Allocator&>);
        ::new (storage) T(allocator);
    } else {
        static_assert(IsNothrowConstructibleV<T>);
        ::new (storage) T();
    }
}

// value(T 값)를 소멸한다(정적 배열은 뒤 원소부터).
template <class T>
void DestroyValueAt(void* value) noexcept {
    if constexpr (IsBoundedArrayV<T>) {
        using E = RemoveExtentT<T>;
        E* elements = static_cast<E*>(value);
        for (usize index = sizeof(T) / sizeof(E); index > 0; --index) {
            DestroyValueAt<E>(elements + index - 1);
        }
    } else {
        static_cast<T*>(value)->~T();
    }
}

// C++ 타입 T의 값 연산 표다(RuntimeValueOps — 정적 수명). 복사·교환할 수 없는 타입은 그 칸이
// null이다.
//
// 사용 예:
//   const RuntimeValueOps& ops = detail::ValueOpsOf<OrderedMap<String, int32>>();
//   auto copied = ops.copy(&destination, &source, allocator);
template <class T>
const RuntimeValueOps& ValueOpsOf() noexcept {
    static_assert(IsNothrowDestructibleV<RemoveAllExtentsT<T>>,
                  "Reflected values need a noexcept destructor");
    static const RuntimeValueOps ops = [] {
        RuntimeValueOps made;
        made.size = sizeof(T);
        made.alignment = alignof(T);
        made.nativeType = NativeTypeKey<T>();
        made.construct = [](void* storage, Allocator& allocator) noexcept {
            ConstructValueAt<T>(storage, allocator);
        };
        made.destroy = [](void* value) noexcept { DestroyValueAt<T>(value); };
        if constexpr (ValueCopy<T>::Supported) {
            made.copy = [](void* destination, const void* source,
                           Allocator& allocator) noexcept -> Result<void> {
                return ValueCopy<T>::Copy(*static_cast<T*>(destination),
                                          *static_cast<const T*>(source), allocator);
            };
        }

        if constexpr (ValueSwap<T>::Supported) {
            made.swap = [](void* first, void* second) noexcept {
                ValueSwap<T>::Swap(*static_cast<T*>(first), *static_cast<T*>(second));
            };
        }

        if constexpr (ValueValidate<T>::HasHook) {
            made.validateTree = [](const void* value) noexcept -> Result<void> {
                return ValueValidate<T>::Run(*static_cast<const T*>(value));
            };
        }

        return made;
    }();
    return ops;
}

} // namespace rocks::detail
