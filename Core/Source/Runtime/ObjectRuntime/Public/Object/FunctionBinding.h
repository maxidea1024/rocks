#pragma once

#include <Base/PlacementNew.h>
#include <Core/Memory/Allocator.h>
#include <Object/FunctionCall.h>
#include <Object/ValueCopy.h>

// 리플렉션 함수 바인딩이다(Docs/Design/EventReflection.ko.md ED3·ED5 — 함수 리플렉션 FR4, UE
// TScriptDelegate 대응). 바인딩은 (대상 객체 handle, FunctionId)이고, 바인딩할 때 대상 함수의 호출
// frame 배치가 호출자 인자 Args의 frame 배치와 같은지 검사한 뒤 함수 연산표를 캐시한다. 호출은
// BindingFrame<Args...>로 생성 thunk를 바로 부른다 — 호출마다 할당·TypeRegistry 잠금·함수 찾기가
// 없다. 이벤트(Event — EV1b)와 입력 action 바인딩(InputActions IA7)이 쓴다.
//
// 사용 예:
//   auto binding = FunctionBinding::Create<int32>(registry, access, door, OpenId);
//   if (!binding) { return Result<void>::Failure(binding.Error()); }
//   BindingFrame<int32> frame(allocator);
//   if (auto set = frame.Assign(3); !set) { return set; }
//   return binding.Value().Invoke(registry, access, frame);
//
// enum·struct 인자는 그 타입의 생성 헤더가 보여야 한다(TypeIdOf — 없으면 컴파일 오류).
namespace rocks {

// 호출 frame 배치다(칸 수 ≤ MaxFunctionParameters, frame 크기·정렬, 칸 서술의 digest). 생성 frame
// 규칙(Docs/Design/FunctionReflection.ko.md 7절 — 칸 FrameSlot<T> 선언 순서, 뒤에 ErrorCode error,
// 표준 배치 struct offset)으로 계산한 값이다 — detail::FrameLayoutOf<Args...>().
struct FrameLayout {
    FrameSlotLayout slots[MaxFunctionParameters] = {};
    uint32 count = 0;
    uint32 size = 0;
    uint32 alignment = 0;
    // 칸 서술·크기·정렬의 FNV-1a 64비트 digest다(바인딩이 쥐고 Invoke가 frame과 대조한다).
    uint64 digest = 0;
};

// 함수가 expected 배치의 frame으로 반환 없이 부를 수 있는 서명인지다(EventReflection ED5 —
// FunctionReflection FD11 개정): 반환이 없고 Result 함수가 아니며, 매개변수 수가 같고 칸마다 값
// 형식(value·element·key)·형식 flag·크기·정렬·offset이 같고, frame 크기·정렬이 같다. 이름은 보지
// 않는다.
ROCKS_OBJECTRUNTIME_API bool FunctionSignatureMatches(const FunctionSchema& schema,
                                                      const RuntimeFunctionOps& ops,
                                                      const FrameLayout& expected) noexcept;

namespace detail {

constexpr uint32 AlignFrameOffset(uint32 offset, uint32 alignment) noexcept {
    return (offset + alignment - 1) / alignment * alignment;
}

// 배치 digest다(칸 서술·offset·frame 크기·정렬 — FNV-1a 64비트).
ROCKS_OBJECTRUNTIME_API uint64 FrameLayoutDigest(const FrameLayout& layout) noexcept;

// Args의 frame 모양(offset·error 칸·크기·정렬)이다. 칸은 FrameSlot<T>(T의 크기·정렬 그대로)이고
// 뒤에 ErrorCode error 칸이 온다(생성 frame과 같은 표준 배치 struct 규칙).
template <class... Args>
struct BindingFrameShape {
    static_assert(sizeof...(Args) <= MaxFunctionParameters,
                  "A binding frame takes at most MaxFunctionParameters arguments");

    static constexpr uint32 Count = static_cast<uint32>(sizeof...(Args));

    struct Computed {
        uint32 offsets[Count == 0 ? 1 : Count] = {};
        uint32 errorOffset = 0;
        uint32 size = 0;
        uint32 alignment = 0;
    };

    static constexpr Computed Compute() noexcept {
        Computed computed;
        uint32 offset = 0;
        uint32 alignment = static_cast<uint32>(alignof(ErrorCode));
        uint32 index = 0;
        auto place = [&](uint32 slotSize, uint32 slotAlignment) noexcept {
            offset = AlignFrameOffset(offset, slotAlignment);
            computed.offsets[index++] = offset;
            offset += slotSize;
            alignment = alignment < slotAlignment ? slotAlignment : alignment;
        };
        (place(static_cast<uint32>(sizeof(FrameSlot<BindingSlotType<Args>>)),
               static_cast<uint32>(alignof(FrameSlot<BindingSlotType<Args>>))),
         ...);
        offset = AlignFrameOffset(offset, static_cast<uint32>(alignof(ErrorCode)));
        computed.errorOffset = offset;
        offset += static_cast<uint32>(sizeof(ErrorCode));
        computed.size = AlignFrameOffset(offset, alignment);
        computed.alignment = alignment;
        return computed;
    }

    static constexpr Computed Value = Compute();
};

// Args로 바인딩할 함수의 기대 frame 배치다.
template <class... Args>
FrameLayout FrameLayoutOf() noexcept {
    using Shape = BindingFrameShape<Args...>;
    FrameLayout layout;
    layout.count = Shape::Count;
    layout.size = Shape::Value.size;
    layout.alignment = Shape::Value.alignment;
    uint32 index = 0;
    ((layout.slots[index] = BindingSlotLayoutOf<BindingSlotType<Args>>(),
      layout.slots[index].offset = Shape::Value.offsets[index], ++index),
     ...);
    layout.digest = FrameLayoutDigest(layout);
    return layout;
}

// Args 배치의 digest다(형식마다 한 번 계산해 둔다 — BindingFrame이 생성마다 다시 계산하지 않게.
// 값은 image와 무관하다).
template <class... Args>
uint64 FrameLayoutDigestOf() noexcept {
    static const uint64 digest = FrameLayoutOf<Args...>().digest;
    return digest;
}

} // namespace detail

template <class... Args>
class BindingFrame;

// 리플렉션 함수 바인딩 하나다(값 — 복사할 수 있다). 대상은 handle만 쥔다(대상 수명을 늘리지 않고
// GC trace 대상도 아니다). 캐시한 함수 연산표는 대상 객체가 살아 있는 동안 유효하다(살아 있는 객체
// slot이 자기 타입 lease를 쥔다 — Invoke가 먼저 대상 생존을 본다). 소유 스레드 전용이다.
class ROCKS_OBJECTRUNTIME_API FunctionBinding {
public:
    FunctionBinding() noexcept = default;

    // 대상의 타입(과 기반 체인)에서 functionId 함수를 찾아 Args의 frame으로 부를 수 있는지
    // 검사한다. 실패: 대상 해석 오류(Registry 그대로 —
    // StaleHandle·WrongThread·WrongContext·ShuttingDown· Busy 등), 함수 없음(NotFound — 게임 빌드의
    // EditorOnly 함수 포함), static 함수 (InvalidArgument), 서명·배치 불일치(TypeMismatch).
    template <class... Args>
    static Result<FunctionBinding> Create(ObjectRegistry& registry,
                                          const ObjectAccessContext& access, ObjectHandle target,
                                          Guid functionId) noexcept {
        return CreateWithLayout(registry, access, target, functionId,
                                detail::FrameLayoutOf<Args...>());
    }

    // 배치를 직접 넘기는 판이다(Event처럼 배치를 한 번 계산해 여러 바인딩에 쓰는 호출자).
    static Result<FunctionBinding> CreateWithLayout(ObjectRegistry& registry,
                                                    const ObjectAccessContext& access,
                                                    ObjectHandle target, Guid functionId,
                                                    const FrameLayout& expected) noexcept;

    // frame의 인자로 대상 함수를 부른다. 실패: 빈 바인딩·바인딩 때와 다른 배치의 frame
    // (InvalidArgument), 대상 생존 조회 오류(InspectLifecycle — StaleHandle·WrongThread·
    // WrongContext·Busy·ShuttingDown), 그 밖의 대상 해석 오류(thunk — Destroyed·Unreachable 대상의
    // 해석 실패 등). frame은 바꾸지 않는다(같은 frame으로 여러 바인딩을 부를 수 있다 — FD4).
    template <class... Args>
    Result<void> Invoke(ObjectRegistry& registry, const ObjectAccessContext& access,
                        BindingFrame<Args...>& frame) const noexcept {
        return InvokeWithDigest(registry, access, frame.Data(), frame.Digest());
    }

    bool IsBound() const noexcept {
        return ops_ != nullptr;
    }

    ObjectHandle Target() const noexcept {
        return target_;
    }

    Guid FunctionId() const noexcept {
        return functionId_;
    }

private:
    Result<void> InvokeWithDigest(ObjectRegistry& registry, const ObjectAccessContext& access,
                                  void* frame, uint64 digest) const noexcept;

    ObjectHandle target_{};
    Guid functionId_{};
    uint64 layoutDigest_ = 0;
    const RuntimeFunctionOps* ops_ = nullptr;
};

// 바인딩 호출 frame 하나다(스택 저장소 — Args의 기대 배치 그대로, 칸마다 값 ops로 만든다). 이동·
// 복사 금지. Assign이 인자를 복사해 담고(const T& 인자는 깊은 복사 — 할당 실패면 그 오류, 앞의 칸은
// 이미 새 값이고 실패한 칸과 뒤의 칸은 이전 값 그대로다), 소멸자가 칸을 부순다. 같은 frame으로 여러
// 바인딩을 부를 수 있다. 복사할 수 없는 인자 형식은 컴파일 오류다.
template <class... Args>
class BindingFrame {
    using Shape = detail::BindingFrameShape<Args...>;
    static_assert((detail::ValueCopy<detail::BindingSlotType<Args>>::Supported && ...),
                  "BindingFrame arguments must be copyable reflected value types");

public:
    explicit BindingFrame(Allocator& allocator) noexcept
        : allocator_(&allocator), digest_(detail::FrameLayoutDigestOf<Args...>()) {
        Construct<0, Args...>();
        ::new (static_cast<void*>(storage_ + Shape::Value.errorOffset)) ErrorCode(ErrorCode::None);
    }

    ~BindingFrame() noexcept {
        Destroy<0, Args...>();
    }

    BindingFrame(const BindingFrame&) = delete;
    BindingFrame& operator=(const BindingFrame&) = delete;

    Result<void> Assign(const detail::BindingSlotType<Args>&... args) noexcept {
        return AssignAt<0>(args...);
    }

    void* Data() noexcept {
        return storage_;
    }

    // 이 frame 배치의 digest다(FunctionBinding::Invoke가 바인딩 때의 배치와 대조한다).
    uint64 Digest() const noexcept {
        return digest_;
    }

private:
    template <uint32 Index>
    void Construct() noexcept {
    }

    template <uint32 Index, class First, class... Rest>
    void Construct() noexcept {
        detail::ValueOpsOf<detail::BindingSlotType<First>>().construct(
            storage_ + Shape::Value.offsets[Index], *allocator_);
        Construct<Index + 1, Rest...>();
    }

    template <uint32 Index>
    void Destroy() noexcept {
    }

    template <uint32 Index, class First, class... Rest>
    void Destroy() noexcept {
        detail::ValueOpsOf<detail::BindingSlotType<First>>().destroy(storage_ +
                                                                     Shape::Value.offsets[Index]);
        Destroy<Index + 1, Rest...>();
    }

    template <uint32 Index>
    Result<void> AssignAt() noexcept {
        return Result<void>::Success();
    }

    template <uint32 Index, class First, class... Rest>
    Result<void> AssignAt(const First& first, const Rest&... rest) noexcept {
        if (auto copied = detail::ValueOpsOf<First>().copy(storage_ + Shape::Value.offsets[Index],
                                                           &first, *allocator_);
            !copied) {
            return copied;
        }

        return AssignAt<Index + 1>(rest...);
    }

    Allocator* allocator_;
    uint64 digest_;
    alignas(Shape::Value.alignment) unsigned char storage_[Shape::Value.size];
};

} // namespace rocks
