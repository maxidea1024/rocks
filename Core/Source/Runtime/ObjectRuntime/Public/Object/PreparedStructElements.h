#pragma once

#include <Core/Containers/Array.h>
#include <Object/ObjectBatchPublish.h>
#include <Object/PathTarget.h>
#include <Object/PropertyPath.h>
#include <Object/StructRefArray.h>
#include <Object/TypeLease.h>

// StructRefArray 하나의 원소 지우기·비우기 준비물이다(리플렉션 B9i3c4 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절 "Registry 컨테이너" 준비 단위). 대입 묶음이 원소를
// 객체를 바꾸지 않고 지워 두고, 게시 때 한 번에 원소 블록 색인을 압축한다:
//   1. 준비: PathPrepareStructElements가 경로 끝의 StructRefArray에 묶는다(지금 원소 수와 변경
//      계수를 기록한다 — 객체는 읽기 Borrow로만 본다). 그 뒤 RemoveAt·Clear는 이 준비물의 index
//      표(지금 index → 묶을 때 index)만 바꾼다. 원소 블록은 그대로라 같은 묶음의 다른 단위는 묶을
//      때 index(OriginalIndex)로 원소 안 자리를 찾는다.
//   2. 게시 직전(구간 밖): 같은 경로로 PathPrepareStructElements를 다시 불러 owner 생존·같은 배열·
//      변경 계수를 확인하고 지울 원소 안의 참조(RemovedReferences — 간선 변경 {참조, null})를
//      trace로 모은다. PreparePublish가 지울 블록을 받을 목록을 할당한다.
//   3. 구간 안: Publish(batch)가 지울 블록을 목록으로 옮기고 남는 블록을 앞으로 당긴다(무할당·
//      무실패 — 지울 원소의 참조마다 관찰자 검사합을 뺀다). 같은 원소의 값·참조 단위는 그 전에
//      게시한다(index가 묶을 때 index다). staged 객체는 Publish()가 트랜잭션 안에서 압축한다.
//   4. 지운 블록은 ReleaseRemoved·Reset·소멸(구간 밖)에서 역순으로 소멸하고 반환한다.
//
// 사용 예(살아 있는 객체의 materials[1]을 지운다):
//   PreparedStructElements stage(allocator);
//   if (auto bound = PathPrepareStructElements(registry, access, mesh, path.View(), stage); !bound)
//   {
//       return bound;
//   }
//
//   (void)stage.RemoveAt(1);
//   ...   // 게시 직전: 다시 묶기(참조 수집) → PreparePublish
//   // edges에 RemovedReferences()마다 {참조, null}을 넣은 뒤
//   auto batch = registry.BeginBatchPublish(access, mesh, stamp, targets, edges);
//   stage.Publish(batch.Value());
//   batch.Value().End();
//   stage.ReleaseRemoved();   // 지운 원소 소멸
namespace rocks {

class ConstructionContext;

namespace detail {

class PathCursor;

} // namespace detail

// StructRefArray 하나의 원소 지우기 준비물이다. 복사·이동 금지, 소유 스레드 전용이다. 한 준비물은
// 처음 묶은 배열 하나에만 쓴다(다른 배열에 묶으면 InvalidArgument(InvalidValue)). 준비와 게시
// 사이에 그 배열을 바꾸면 다시 묶기가 Busy다(살아 있는 객체는 변경 표지도 BeginBatchPublish를
// Busy로 만든다).
class PreparedStructElements {
public:
    // allocator는 index 표·참조 목록·지울 블록 목록의 allocator다(원소 블록은 배열의 allocator가
    // 소유한다).
    explicit PreparedStructElements(Allocator& allocator) noexcept
        : kept_(allocator), references_(allocator), removed_(allocator) {
    }

    ~PreparedStructElements() {
        Reset();
    }

    PreparedStructElements(const PreparedStructElements&) = delete;
    PreparedStructElements& operator=(const PreparedStructElements&) = delete;
    PreparedStructElements(PreparedStructElements&&) = delete;
    PreparedStructElements& operator=(PreparedStructElements&&) = delete;

    // 배열에 묶였는지다.
    bool IsReady() const noexcept {
        return storage_ != nullptr;
    }

    // 묶을 때의 원소 수다.
    usize OriginalCount() const noexcept {
        return original_;
    }

    // 지우고 남은 원소 수다(묶음 안의 지금 원소 수).
    usize Count() const noexcept {
        return kept_.Size();
    }

    // 지운 원소가 있는지다.
    bool HasRemovals() const noexcept {
        return kept_.Size() != original_;
    }

    // 지금 index(< Count())의 원소가 묶을 때 가졌던 index다.
    uint32 OriginalIndex(usize index) const noexcept {
        return kept_[index];
    }

    // 묶을 때 index original의 원소를 지웠는지다(original < OriginalCount()).
    bool IsRemoved(uint32 original) const noexcept;

    // 지금 index의 원소를 지운다(이 준비물만 바뀐다 — 할당하지 않는다). 묶지 않았으면
    // InvalidArgument, index ≥ Count()면 InvalidArgument다.
    Result<void> RemoveAt(usize index) noexcept;

    // 남은 원소를 모두 지운다(할당하지 않는다). 묶지 않았으면 InvalidArgument다.
    Result<void> Clear() noexcept;

    // 지울 원소 안의 non-null 강한 참조다(게시 직전의 다시 묶기가 모은다 — 간선 변경 {참조, null}).
    Span<const ObjectHandle> RemovedReferences() const noexcept {
        return references_.View();
    }

    // 게시를 준비한다(구간 밖): 지울 블록을 받을 목록을 할당한다. 묶지 않았으면 InvalidArgument,
    // 할당 실패는 그 오류다.
    Result<void> PreparePublish() noexcept;

    // batch(같은 owner의 열린 묶음 게시 구간) 안에서 지울 블록을 목록으로 옮기고 남은 블록을 앞으로
    // 당긴다(할당·실패가 없다). PreparePublish 뒤에 한 번이고, RemovedReferences()의 간선 변경을
    // BeginBatchPublish에 선언했어야 한다. 준비하지 않았거나 게시된 객체 자리가 아니거나 다른
    // owner의 구간이거나 배열이 묶을 때와 다르면 Fatal이다(계약 위반). 지운 원소가 없으면
    // 무동작이다.
    void Publish(ObjectBatchPublish& batch) noexcept;

    // staged 객체의 배열을 같은 방식으로 압축한다(트랜잭션 안 — 지운 블록은 이 안에서 처리 표시
    // 아래 소멸한다). 묶지 않았거나 staged 자리가 아니면 InvalidArgument, 트랜잭션이 끝났거나 오류
    // 상태면 InvalidArgument, Required 검사 중·Registry 처리 중이면 Busy, 배열이 묶을 때와 다르면
    // Busy이고 그때 배열은 그대로다. 지운 원소가 없으면 무동작이다.
    Result<void> Publish() noexcept;

    // 게시로 떼어 낸 원소를 역순으로 소멸하고 블록을 반환한다(구간 밖 — 원소 소멸자·해제 콜백이
    // 이 안에서 돈다).
    void ReleaseRemoved() noexcept;

    // 떼어 낸 원소를 반환하고 묶음을 푼다.
    void Reset() noexcept;

private:
    friend class detail::PathCursor;
    friend Result<void> PathPrepareStructElements(ObjectRegistry&, const ObjectAccessContext&,
                                                  const PathTarget&, PropertyPath,
                                                  PreparedStructElements&,
                                                  PropertyPathFailure*) noexcept;

    // 묶음 게시 구간의 검사합에서 지울 원소의 참조 하나를 뺀다(trace 방문 함수).
    static void NoteRemoved(void* registry, ObjectHandle handle) noexcept;

    // 지울 원소 안의 non-null 참조를 references_에 모은다(세고 잡은 뒤 담는다). 오류: 할당 실패.
    Result<void> CollectReferences() noexcept;

    // 지울 블록을 removed_로 옮기고 남은 블록을 앞으로 당긴다(registry가 있으면 지울 원소의
    // 참조마다 그 구간의 검사합을 뺀다). removed_의 용량은 PreparePublish가 잡았다.
    void Compact(ObjectRegistry* registry) noexcept;

    detail::StructRefArrayStorage* storage_ = nullptr;
    // 블록 소멸에 쓰는 배열의 allocator·원소 표다(owner가 사라져도 쓸 수 있게 묶을 때 복사한다).
    Allocator* elementAllocator_ = nullptr;
    const detail::StructElementOps* element_ = nullptr;
    // 원소 struct의 등록 lease(trace — 지울 원소의 참조 수집과 게시의 검사합)다.
    TypeLease elementType_;
    ObjectRegistry* registry_ = nullptr;
    ObjectHandle owner_{};
    PathTarget::Kind kind_ = PathTarget::Kind::Object;
    ConstructionContext* transaction_ = nullptr;
    usize original_ = 0;
    uint32 revision_ = 0;
    // 남은 원소의 묶을 때 index(오름차순)다.
    Array<uint32> kept_;
    Array<ObjectHandle> references_;
    // 게시로 떼어 낸 블록이다(PreparePublish가 용량을 잡는다).
    Array<void*> removed_;
};

// path 끝의 StructRefArray 필드·멤버(선택자 없음)에 out을 묶는다(리플렉션 B9i3c4). target은 게시된
// 객체나 staged 객체이고(분리된 값·사본을 건 시작 위치는 InvalidArgument), 끝이 StructRefArray가
// 아니면 InvalidArgument(InvalidValue)다. 객체는 읽기 Borrow로만 본다(변경 표지를 올리지 않는다).
// 이미 묶였으면 같은 owner·배열인지(아니면 InvalidArgument(InvalidValue))와 배열의 변경 계수가 묶을
// 때와 같은지(아니면 Busy)를 확인하고, 게시된 객체면 지울 원소 안의 참조를 다시 모은다(고정 아래 —
// 할당 실패는 그 오류). 경로 해석·접근 오류는 PathCount와 같다.
//
// 사용 예:
//   PreparedStructElements stage(allocator);
//   auto bound = PathPrepareStructElements(registry, access, mesh, materialsPath.View(), stage);
Result<void> PathPrepareStructElements(ObjectRegistry& registry, const ObjectAccessContext& access,
                                       const PathTarget& target, PropertyPath path,
                                       PreparedStructElements& out,
                                       PropertyPathFailure* failure = nullptr) noexcept;

} // namespace rocks
