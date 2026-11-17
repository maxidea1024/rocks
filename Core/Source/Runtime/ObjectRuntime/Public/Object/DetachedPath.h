#pragma once

#include <Object/PropertyPath.h>

// 분리된 값과 경로 연산 Detach·Attach다(리플렉션 B9i3c0 — Docs/Design/ReflectionExtensions.ko.md
// 10.2절). 대입 묶음(B9i3c)의 값 부분 트리 준비 단위가 쓴다: Prepare에서 지금 값을 상자로 깊게
// 복사하고(PathDetach) 상자 위에서 경로 연산으로 고친 뒤(PathTarget::Value(box)), 게시 때 상자
// 내용을 원래 자리와 무할당으로 맞바꾼다(PathPrepareAttach → PreparedAttach::Publish). 옛 값은
// 상자가 받고 상자를 비우거나 소멸할 때(게시 구간을 나온 뒤) 반환한다. UE 대응은
// FProperty::CopyCompleteValue(UE 5.8 CoreUObject/Public/UObject/UnrealType.h:1027)로 만든 사본을
// 편집 뒤 되돌려 놓는 방식이다.
namespace rocks {

class ObjectBatchPublish;

namespace detail {

class PathCursor;
struct PathRunner;

// 상자 안 값(C++ 멤버 형식)에서 경로를 시작하는 생성 함수다(PathMember<Target, Member> — Detach가
// 멤버 형식으로 채운다).
using DetachedPathFunction = Result<void> (*)(PathCursor& cursor, void* value) noexcept;

} // namespace detail

// 필드·멤버 값 하나를 담는 상자다(PathDetach가 채운다). 값은 생성자의 allocator로 만든 저장소에
// 있고, 그 필드의 스키마(값을 분리한 타입의 lease를 복제해 쥔다)와 바깥에서 누적한 유효 플래그를
// 함께 담는다. 복사·이동 금지(PreparedAttach가 주소를 쥔다). 소유 스레드 전용이다.
//
// 사용 예:
//   DetachedValue box(allocator);
//   if (auto detached = PathDetach(registry, access, mesh, path.View(), box); !detached) {
//       return detached;
//   }
//
//   auto count = PathCount(registry, access, PathTarget::Value(box), path.View());
class DetachedValue {
public:
    explicit DetachedValue(Allocator& allocator) noexcept : allocator_(&allocator) {
    }

    ~DetachedValue() {
        Reset();
    }

    DetachedValue(const DetachedValue&) = delete;
    DetachedValue& operator=(const DetachedValue&) = delete;
    DetachedValue(DetachedValue&&) = delete;
    DetachedValue& operator=(DetachedValue&&) = delete;

    bool IsEmpty() const noexcept {
        return storage_ == nullptr;
    }

    // 담은 필드의 ID다(빈 상자는 0 Guid). 상자에서 시작하는 경로의 첫 단계가 이 ID다.
    Guid FieldId() const noexcept;

    // 담은 필드의 스키마다(빈 상자는 nullptr). 상자가 쥔 lease 수명 동안 유효하다.
    const FieldSchema* Field() const noexcept {
        return field_;
    }

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    // 값을 소멸하고 저장소와 lease를 반환한다(빈 상자는 무동작). 해제 콜백은 이 안에서 돈다 — 묶음
    // 게시 구간 밖에서 부른다.
    void Reset() noexcept;

    // 담은 값을 그 형식의 기본값(T(Allocator&) 또는 T() — 상자의 allocator)으로 바꾼다(리플렉션
    // B9i3c1 — 텍스트 `s={}`). 옛 값은 이 안에서 소멸·반환하므로 게시 구간 밖에서 부른다. 할당하지
    // 않고 실패하지 않는다. 빈 상자는 Fatal이다(계약 위반).
    //
    // 사용 예:
    //   if (!box.IsEmpty()) {
    //       box.ResetToDefault();
    //   }
    void ResetToDefault() noexcept;

    // 이 상자 위의 경로 쓰기가 ReadOnly를 막지 않는지다. staged 객체(또는 그 상자)에서 분리한
    // 상자(staged 객체 대상의 ReadOnly 허용 — Docs/Design/ReflectionExtensions.ko.md 10.2절)와
    // AcceptReadOnlyWrites를 부른 상자가 참이다. 상자에서 다시 분리한 상자는 이 값을 물려받는다.
    bool AcceptsReadOnlyWrites() const noexcept {
        return acceptsReadOnly_;
    }

    // 이 상자 위의 경로 쓰기가 ReadOnly를 막지 않게 한다(Reset까지). 상자는 사본이므로 대상은
    // 바뀌지 않는다 — 게시(PathPrepareAttach)는 플래그를 보지 않으므로, 호출자가 게시 전에 ReadOnly
    // 자리의 내용이 원래 값과 같은지 확인해야 한다(리플렉션 B9i 대입 묶음은 ReadOnly 자리의 대입을
    // 상자에 적용한 뒤 호출 끝에 지금 값과 대조하고, 다르면 진단 ReadOnly로 게시를 막는다 —
    // Docs/Design/ReflectionExtensions.ko.md 10.2절 "리뷰 반영에서 정한 점"). 빈 상자에도 부를 수
    // 있다(다음 PathDetach가 덮어쓴다).
    //
    // 사용 예:
    //   box.AcceptReadOnlyWrites();
    //   auto written = PathWrite(registry, access, PathTarget::Value(box), path.View(), value);
    void AcceptReadOnlyWrites() noexcept {
        acceptsReadOnly_ = true;
    }

private:
    friend class detail::PathCursor;
    friend struct detail::PathRunner;
    friend class PreparedAttach;

    Allocator* allocator_;
    void* storage_ = nullptr;
    const RuntimeValueOps* ops_ = nullptr;
    detail::DetachedPathFunction enter_ = nullptr;
    TypeLease lease_;
    const FieldSchema* field_ = nullptr;
    uint32 effectiveFlags_ = 0;
    bool acceptsReadOnly_ = false;
    const EnumSchema* enumeration_ = nullptr;
    const EnumSchema* keyEnumeration_ = nullptr;
    StructView target_{};
};

// PathPrepareAttach가 찾아 둔 게시 자리 하나다: 대상 값의 주소, 그 형식의 무할당 교환, 상자. 게시는
// 한 번이다. 준비와 게시 사이에 대상 객체의 그 값 자리를 바꾸거나(리플렉션 쓰기·Registry 참조
// 변경은 변경 표지가 잡아 BeginBatchPublish가 Busy다 — C++ 직접 쓰기는 호출자 책임) 상자를 비우면
// 계약 위반이다. 같은 묶음의 게시 자리들은 서로 겹치지 않아야 한다(한 자리가 다른 자리를 담으면
// 먼저 게시한 쪽이 뒤 자리의 주소를 무효로 만들 수 있다).
class PreparedAttach {
public:
    PreparedAttach() noexcept = default;
    PreparedAttach(const PreparedAttach&) = delete;
    PreparedAttach& operator=(const PreparedAttach&) = delete;

    bool IsReady() const noexcept {
        return location_ != nullptr;
    }

    // 게시된 객체의 자리를 batch(같은 owner의 열린 묶음 게시 구간) 안에서 상자와 맞바꾼다. 할당·
    // 반환·콜백이 없고 실패하지 않는다. 준비되지 않았거나 게시된 객체 자리가 아니거나 다른 owner의
    // 구간·닫힌 구간·비운 상자면 Fatal이다(계약 위반). 게시 뒤 옛 값은 상자에 있고 구간을 닫은 뒤
    // 상자를 비워 반환한다. owner의 변경 표지는 구간 End가 올린다.
    //
    // 사용 예:
    //   auto batch = registry.BeginBatchPublish(access, mesh, stamp, {}, {});
    //   if (!batch) {
    //       return Result<void>::Failure(batch.Error());
    //   }
    //
    //   prepared.Publish(batch.Value());
    //   batch.Value().End();
    //   box.Reset();   // 옛 값 반환
    void Publish(ObjectBatchPublish& batch) noexcept;

    // staged 객체·분리된 값의 자리를 상자와 맞바꾼다(묶음 게시 구간이 없다). 준비되지 않았으면
    // InvalidArgument, staged 자리인데 트랜잭션이 끝났거나 오류로 고정됐으면 그 판정
    // (ConstructionContext::CheckOpen)이고, 그때 아무것도 바꾸지 않는다. 게시된 객체 자리는
    // Fatal이다 (묶음 게시 구간이 필요하다).
    Result<void> Publish() noexcept;

private:
    friend class detail::PathCursor;

    void* location_ = nullptr;
    void (*swap_)(void*, void*) noexcept = nullptr;
    usize bytes_ = 0;
    DetachedValue* box_ = nullptr;
    void* boxStorage_ = nullptr;
    ObjectHandle owner_{};
    PathTarget::Kind kind_ = PathTarget::Kind::Object;
    ConstructionContext* transaction_ = nullptr;
    // StructRefArray 원소 안 자리면 그 원소 위치다(게시 구간의 위치 검사).
    detail::StructElementLocation element_{};
    bool inElement_ = false;
};

// PathPrepareReference가 찾아 둔 단일 강한 참조 위치 하나다(리플렉션 B9i3c2 — 대입 묶음의 간선
// 변경 단위): ObjectRef 자리의 주소, 그 C++ 대상 타입, 찾을 때의 지금 대상(옛 대상), StructRefArray
// 원소 안이면 그 원소 위치. 게시는 묶음 게시 구간 안에서 한 번이다. 준비와 게시 사이에 대상 객체의
// 그 자리를 바꾸면 계약 위반이다(리플렉션 쓰기·Registry 참조 변경은 변경 표지가 잡아
// BeginBatchPublish가 Busy다).
//
// 사용 예:
//   PreparedReference prepared;
//   auto located = PathPrepareReference(registry, access, mesh, path.View(), value, prepared);
//   const BatchTargetCheck targets[] = {{value.reference, prepared.TargetType()}};
//   const BatchEdgeChange edges[] = {{prepared.Previous(), value.reference}};
//   auto batch = registry.BeginBatchPublish(access, mesh, stamp, targets, edges);
//   ...
//   prepared.Publish(batch.Value(), value.reference);
class PreparedReference {
public:
    PreparedReference() noexcept = default;
    PreparedReference(const PreparedReference&) = delete;
    PreparedReference& operator=(const PreparedReference&) = delete;

    bool IsReady() const noexcept {
        return location_ != nullptr;
    }

    // 찾을 때 그 자리가 가리키던 대상이다(null 가능 — 간선 변경의 removed).
    ObjectHandle Previous() const noexcept {
        return previous_;
    }

    // 자리의 C++ 대상 타입(NativeTypeKey<T>)이다 — BatchTargetCheck의 기대 타입.
    const void* TargetType() const noexcept {
        return nativeType_;
    }

    // batch(같은 owner의 열린 묶음 게시 구간) 안에서 자리를 target으로 바꾼다(null이면 비운다).
    // target은 BeginBatchPublish에서 TargetType()으로 재확인한 생존 대상이어야 하고, 간선 변경
    // {Previous(), target}을 선언했어야 한다. 할당·실패가 없다. 준비되지 않았거나 다른 owner의
    // 구간·닫힌 구간이면 Fatal이다(계약 위반).
    void Publish(ObjectBatchPublish& batch, ObjectHandle target) noexcept;

private:
    friend class detail::PathCursor;

    using PublishFunction = void (*)(ObjectBatchPublish&, void*, ObjectHandle,
                                     const detail::StructElementLocation*) noexcept;

    // 자리 형식마다의 게시 함수다(정의는 Object/ObjectBatchPublish.h).
    template <class T>
    static void PublishAt(ObjectBatchPublish& batch, void* field, ObjectHandle target,
                          const detail::StructElementLocation* element) noexcept;

    void* location_ = nullptr;
    PublishFunction publish_ = nullptr;
    const void* nativeType_ = nullptr;
    ObjectHandle previous_{};
    ObjectHandle owner_{};
    detail::StructElementLocation element_{};
    bool inElement_ = false;
};

// path의 끝(필드·멤버 하나 — 원소·항목 선택자 없음)의 값을 box에 깊게 복사한다(box의 allocator).
// 읽기 연산이다: 게시된 객체는 읽기 Borrow(변경 표지를 올리지 않는다) + encode 고정 아래 복사하고
// 상자를 채우기 전에 owner를 다시 확인한다(복사의 할당 콜백이 원본을 바꾸려 하면 Busy). 실패하면
// 원본과 box 모두 그대로다.
//
// 오류: box가 비어 있지 않으면 InvalidArgument, 끝 단계에 선택자가 있으면
// InvalidArgument(IndexNotAllowed), 깊은 복사·무할당 교환을 할 수 없는 값(강한 참조·Registry
// 컨테이너·복사 의미가 없는 NATIVE 멤버를 담은 값)은 Unsupported(NotCopyable), 경로 thunk가 없으면
// Unsupported(NoPathAccess), lease 복제·할당 실패는 그 오류다. 그 밖의 순서는 PathRead와 같다.
//
// 사용 예:
//   DetachedValue box(registry.ObjectAllocator());
//   auto detached = PathDetach(registry, access, mesh, slotsPath.View(), box);
Result<void> PathDetach(ObjectRegistry& registry, const ObjectAccessContext& access,
                        const PathTarget& target, PropertyPath path, DetachedValue& box,
                        PropertyPathFailure* failure = nullptr) noexcept;

// path의 끝(필드·멤버 하나)을 box와 맞바꿀 자리를 찾아 out에 둔다(값은 아직 바꾸지 않는다).
// 게시된 객체는 읽기 Borrow로 찾으므로 변경 표지를 올리지 않는다 — 대입 묶음은 표지를 읽은 뒤
// 준비하고 BeginBatchPublish 구간 안에서 out.Publish(batch)로 게시한다. staged 객체·분리된 값은
// out.Publish()로 게시한다.
//
// 오류: box가 비었으면 InvalidArgument, 끝 단계에 선택자가 있으면 InvalidArgument(IndexNotAllowed),
// box의 필드가 끝 필드와 다르면(다른 스키마 레코드·다른 C++ 형식) TypeMismatch(StaticTypeMismatch),
// 유효 플래그가 ReadOnly면 Unsupported(ReadOnly — staged 객체는 막지 않는다), 교환할 수 없는 값은
// Unsupported(NotCopyable), box가 target 자신이면 InvalidArgument. 그 밖의 순서는 PathWrite와
// 같다(값 검증은 없다 — 상자 안의 값은 경로 연산이 검증하며 넣었다).
//
// 사용 예(게시된 객체):
//   PreparedAttach prepared;
//   auto located = PathPrepareAttach(registry, access, mesh, slotsPath.View(), box, prepared);
//   ...
//   prepared.Publish(batch.Value());
Result<void> PathPrepareAttach(ObjectRegistry& registry, const ObjectAccessContext& access,
                               const PathTarget& target, PropertyPath path, DetachedValue& box,
                               PreparedAttach& out,
                               PropertyPathFailure* failure = nullptr) noexcept;

// path의 끝(단일 강한 참조 위치 — ObjectRef 필드, 인라인 struct·StructRefArray 원소의 ObjectRef
// 멤버, 정적 배열의 ObjectRef 원소)을 value(ObjectReference)로 바꿀 자리를 찾아 out에 둔다(리플렉션
// B9i3c2). 값은 바꾸지 않는다: PathWrite와 같은 해석·검증(ReadOnly·기대 타입·대상 생존 — staged
// 객체는 ReadOnly를 막지 않는다)을 하되 읽기 Borrow로 찾으므로 변경 표지를 올리지 않는다. 게시된
// 객체는 BeginBatchPublish 구간 안에서 out.Publish(batch, value.reference)로 게시한다. staged
// 객체에서는 검증과 옛 대상 읽기만 쓴다(게시는 PathWrite — 트랜잭션의 staged 간선).
//
// 오류: 시작 위치가 분리된 값이면 Unsupported(NoPathAccess), 끝이 단일 강한 참조 위치가 아니면
// (ObjectRefArray·ObjectRefMap 원소 포함) InvalidArgument(InvalidValue), value가 ObjectReference가
// 아니면 TypeMismatch. 그 밖의 순서는 PathWrite와 같다.
Result<void> PathPrepareReference(ObjectRegistry& registry, const ObjectAccessContext& access,
                                  const PathTarget& target, PropertyPath path,
                                  const FieldValue& value, PreparedReference& out,
                                  PropertyPathFailure* failure = nullptr) noexcept;

} // namespace rocks
