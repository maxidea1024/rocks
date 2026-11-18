#pragma once

#include <Core/Containers/Array.h>
#include <Object/ObjectBatchPublish.h>
#include <Object/PathTarget.h>
#include <Object/PropertyPath.h>
#include <Object/StructRefArray.h>
#include <Object/TypeLease.h>

// StructRefArray 하나의 원소 지우기·비우기·추가 준비물이다(리플렉션 B9i3c4·B9i5 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절 "Registry 컨테이너" 준비 단위). 대입 묶음이 원소를
// 객체를 바꾸지 않고 지우거나 끝에 더해 두고, 게시 때 한 번에 원소 블록 색인을 다시 짠다:
//   1. 준비: PathPrepareStructElements가 경로 끝의 StructRefArray에 묶는다(지금 원소 수와 변경
//      계수를 기록한다 — 객체는 읽기 Borrow로만 본다). 그 뒤 RemoveAt·Clear는 이 준비물의 index
//      표(지금 index → 묶을 때 index)만 바꾼다. 원소 블록은 그대로라 같은 묶음의 다른 단위는 묶을
//      때 index(OriginalIndex)로 원소 안 자리를 찾는다. Append는 끝에 기본 원소 블록을 할당해
//      이 준비물이 소유하고(묶을 때 index는 OriginalCount() 이상인 추가 원소 id), 그 원소 안 대입은
//      AddedTarget(게시 전 블록 — 강한 참조도 블록에 직접 쓴다)으로 한다.
//   2. 게시 직전(구간 밖): 같은 경로로 PathPrepareStructElements를 다시 불러 owner 생존·같은 배열·
//      변경 계수를 확인하고 지울 원소 안의 참조(RemovedReferences — 간선 변경 {참조, null})를
//      trace로 모으고, 남는 추가 원소 안의 참조(AddedReferences — 간선 변경 {null, 참조}와 재확인
//      대상)도 모은다. PreparePublish가 지울 블록을 받을 목록과(원소 수가 용량을 넘으면) 새 색인을
//      할당한다.
//   3. 구간 안: Publish(batch)가 지울 블록을 목록으로 옮기고 남는 블록과 추가 블록을 차례로 놓는다
//      (무할당·무실패 — 지울 원소의 참조마다 관찰자 검사합을 빼고, 추가 원소의 참조마다 재확인·
//      장벽·검사합을 더한다). 같은 원소의 값·참조 단위는 그 전에 게시한다(index가 묶을 때
//      index다). staged 객체는 Publish()가 트랜잭션 안에서 같은 일을 한다.
//   4. 지운 블록과 옛 색인은 ReleaseRemoved·Reset·소멸(구간 밖)에서 역순으로 소멸하고 반환한다.
//      게시하지 않은 추가 블록은 Reset·소멸이 반환한다.
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

// StructRefArray 하나의 원소 지우기·추가 준비물이다. 복사·이동 금지, 소유 스레드 전용이다. 한
// 준비물은 처음 묶은 배열 하나에만 쓴다(다른 배열에 묶으면 InvalidArgument(InvalidValue)). 준비와
// 게시 사이에 그 배열을 바꾸면 다시 묶기가 Busy다(살아 있는 객체는 변경 표지도 BeginBatchPublish를
// Busy로 만든다).
class PreparedStructElements {
public:
    // allocator는 index 표·참조 목록·블록 목록의 allocator다(원소 블록과 색인은 배열의 allocator로
    // 만든다).
    explicit PreparedStructElements(Allocator& allocator) noexcept
        : kept_(allocator), references_(allocator), addedReferences_(allocator),
          removed_(allocator), added_(allocator) {
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

    // 묶을 때 있던 원소를 지웠는지다.
    bool HasRemovals() const noexcept {
        return kept_.Size() - addedKept_ != original_;
    }

    // 게시할 변경(지운 원소나 남은 추가 원소)이 있는지다.
    bool HasChanges() const noexcept {
        return HasRemovals() || addedKept_ != 0;
    }

    // 지금 index(< Count())의 원소가 묶을 때 가졌던 index다(추가 원소는 OriginalCount() 이상의 id).
    uint32 OriginalIndex(usize index) const noexcept {
        return kept_[index];
    }

    // 묶을 때 index(또는 추가 원소 id) original의 원소를 지웠는지다.
    bool IsRemoved(uint32 original) const noexcept;

    // original이 Append로 더한 원소의 id인지다(그 뒤 지웠어도 참이다).
    bool IsAdded(uint32 original) const noexcept {
        return original >= original_ && original - original_ < added_.Size();
    }

    // 남은 추가 원소마다 원소 struct 값 트리의 검증 hook을 안쪽부터
    // 부른다(Docs/Design/CurveData.ko.md 9.1절 — 원소 struct는 참조를 담으므로 자기 hook은 없고
    // 멤버 struct의 hook이다). 추가 원소 안 대입은 블록에 바로 적용되므로 대입 묶음이 게시 전에
    // 부른다. 거부는 InvalidArgument이고 그 원소의 지금 index를 rejected에 둔다. 할당하지 않는다.
    // 게시 전 1회 계약: PreparePublish·staged Publish()가 아직 검증하지 않았으면 부르고, 대입
    // 묶음은 진단 위치를 얻으려고 먼저 부른다(통과하면 Append·AddedTarget·Reset·블록 쓰기 전까지
    // 다시 부르지 않는다). 묶지 않았으면 InvalidArgument다.
    Result<void> ValidateAdded(usize& rejected) noexcept;

    // 끝에 기본 원소 하나를 더한다(리플렉션 B9i5): 블록을 배열의 allocator로 할당해 기본 생성하고
    // 게시나 Reset까지 이 준비물이 소유한다. 새 원소의 지금 index는 Count() - 1이고 돌려주는 값은
    // 그 묶을 때 index(추가 원소 id)다. 묶지 않았으면 InvalidArgument, 원소 수가 uint32 상한에
    // 닿으면 Overflow, 할당 실패는 그 오류이고 그때 준비물은 그대로다.
    Result<uint32> Append() noexcept;

    // 추가 원소 id의 블록 주소다(Reset·게시까지 그대로 — 대입 묶음이 그 원소의 고정 owner로 쓴다).
    // 추가 원소가 아니면 nullptr다.
    const void* AddedBlock(uint32 id) const noexcept {
        return IsAdded(id) ? added_[id - original_] : nullptr;
    }

    // 추가 원소 id(지우지 않은 것)에서 시작하는 경로 연산 위치다: 원소 struct 값(첫 단계는 그
    // 멤버, 새 값은 배열의 allocator)이고 강한 참조 멤버 쓰기는 블록에 대상을 직접 쓴다
    // (PathTarget::WithAddedElement — staged 배열이면 그 트랜잭션 규칙으로 대상을 검증한다).
    // 지운 추가 원소이거나 추가 원소가 아니면 InvalidArgument다. 반환 위치는 이 준비물의 lease를
    // 가리키므로 준비물보다 오래 쓰지 않는다.
    //
    // 사용 예:
    //   auto id = stage.Append();
    //   auto start = stage.AddedTarget(id.Value());
    //   auto written = PathWrite(registry, access, start.Value(), slotNamePath.View(), name);
    Result<PathTarget> AddedTarget(uint32 id) noexcept;

    // 지금 index의 원소를 지운다(이 준비물만 바뀐다 — 할당하지 않는다). 묶지 않았으면
    // InvalidArgument, index ≥ Count()면 InvalidArgument다.
    Result<void> RemoveAt(usize index) noexcept;

    // 남은 원소를 모두 지운다(할당하지 않는다). 묶지 않았으면 InvalidArgument다.
    Result<void> Clear() noexcept;

    // 지울 원소 안의 non-null 강한 참조다(게시 직전의 다시 묶기가 모은다 — 간선 변경 {참조, null}).
    Span<const ObjectHandle> RemovedReferences() const noexcept {
        return references_.View();
    }

    // 남는 추가 원소 안의 non-null 강한 참조다(게시 직전의 다시 묶기가 모은다 — 간선 변경
    // {null, 참조}이고 BeginBatchPublish의 재확인 대상이다).
    Span<const ObjectHandle> AddedReferences() const noexcept {
        return addedReferences_.View();
    }

    // 게시를 준비한다(구간 밖): 지울 블록을 받을 목록과, 게시 뒤 원소 수가 배열 용량을 넘으면 배열
    // allocator의 새 색인을 할당한다. 묶지 않았으면 InvalidArgument, 할당 실패는 그 오류다.
    // 추가 원소의 검증 hook(ValidateAdded)을 아직 부르지 않았으면 먼저 부른다(거부는
    // InvalidArgument).
    Result<void> PreparePublish() noexcept;

    // batch(같은 owner의 열린 묶음 게시 구간) 안에서 지울 블록을 목록으로 옮기고 남은 블록과 추가
    // 블록을 차례로 놓는다(할당·실패가 없다). PreparePublish 뒤에 한 번이고, RemovedReferences()와
    // AddedReferences()의 간선 변경과 AddedReferences()의 재확인을 BeginBatchPublish에 선언했어야
    // 한다. 준비하지 않았거나 게시된 객체 자리가 아니거나 다른 owner의 구간이거나 배열이 묶을 때와
    // 다르거나 추가 원소의 대상이 재확인되지 않았으면 Fatal이다(계약 위반). 변경이 없으면
    // 무동작이다. 게시한 추가 블록은 배열이 소유한다.
    void Publish(ObjectBatchPublish& batch) noexcept;

    // staged 객체의 배열을 같은 방식으로 다시 짠다(트랜잭션 안 — 지운 블록은 이 안에서 처리 표시
    // 아래 소멸한다. 추가 원소의 참조는 Commit이 trace로 검증하고 센다). 묶지 않았거나 staged
    // 자리가 아니면 InvalidArgument, 트랜잭션이 끝났거나 오류 상태면 InvalidArgument, Required 검사
    // 중·Registry 처리 중이면 Busy, 배열이 묶을 때와 다르면 Busy, 할당 실패는 그 오류이고 그때
    // 배열은 그대로다. 변경이 없으면 무동작이다.
    // 추가 원소의 검증 hook(ValidateAdded)을 아직 부르지 않았으면 바꾸기 전에 부른다(거부는
    // InvalidArgument — 배열은 그대로).
    Result<void> Publish() noexcept;

    // 게시로 떼어 낸 원소를 역순으로 소멸하고 블록과 옛 색인을 반환한다(구간 밖 — 원소 소멸자·
    // 해제 콜백이 이 안에서 돈다).
    void ReleaseRemoved() noexcept;

    // 떼어 낸 원소와 게시하지 않은 추가 원소를 반환하고 묶음을 푼다.
    void Reset() noexcept;

private:
    friend class detail::PathCursor;
    friend Result<void> PathPrepareStructElements(ObjectRegistry&, const ObjectAccessContext&,
                                                  const PathTarget&, PropertyPath,
                                                  PreparedStructElements&,
                                                  PropertyPathFailure*) noexcept;

    // 묶음 게시 구간의 검사합에서 지울 원소의 참조 하나를 뺀다(trace 방문 함수).
    static void NoteRemoved(void* registry, ObjectHandle handle) noexcept;

    // 묶음 게시 구간에서 추가 원소의 참조 하나를 재확인하고 장벽·검사합을 더한다(trace 방문 함수 —
    // 재확인되지 않은 대상은 Fatal).
    static void NoteAdded(void* registry, ObjectHandle handle) noexcept;

    // 지울 원소 안의 non-null 참조를 references_에, 남는 추가 원소 안의 것을 addedReferences_에
    // 모은다(세고 잡은 뒤 담는다). 오류: 할당 실패.
    Result<void> CollectReferences() noexcept;

    // 지울 블록(게시하지 않을 추가 블록 포함)을 removed_로 옮기고 남은 블록과 추가 블록을 차례로
    // 놓는다(registry가 있으면 지울 원소의 참조마다 그 구간의 검사합을 빼고 추가 원소의 참조마다
    // 재확인·장벽·검사합을 더한다). removed_의 용량과 새 색인은 PreparePublish가 잡았다.
    void Compact(ObjectRegistry* registry) noexcept;

    // PreparePublish가 잡은 새 색인을 반환한다.
    void ReleaseFresh() noexcept;

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
    // kept_ 안의 추가 원소 id 수다.
    usize addedKept_ = 0;
    // 추가 원소를 ValidateAdded로 검증한 뒤 바꾸지 않았는지다(Append·AddedTarget·Reset과
    // AddedTarget 시작 위치의 변경 연산이 내린다).
    bool addedValidated_ = false;
    // 남은 원소의 묶을 때 index(오름차순 — 추가 원소 id가 뒤에 온다)다.
    Array<uint32> kept_;
    Array<ObjectHandle> references_;
    Array<ObjectHandle> addedReferences_;
    // 게시로 떼어 낸 블록이다(PreparePublish가 용량을 잡는다).
    Array<void*> removed_;
    // Append로 더한 블록이다(id - original_ 순서 — 게시하면 배열이 소유한다).
    Array<void*> added_;
    // PreparePublish가 잡은 새 색인과 그 용량, 그때의 배열 용량이다(게시가 대조한다).
    void** fresh_ = nullptr;
    uint32 freshCapacity_ = 0;
    uint32 preparedCapacity_ = 0;
    // 게시로 바꾼 옛 색인이다(ReleaseRemoved가 반환한다).
    void** retired_ = nullptr;
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
