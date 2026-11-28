#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/UniquePtr.h>
#include <Object/ObjectBatchPublish.h>
#include <Object/PathTarget.h>

// Registry 컨테이너(ObjectRefArray·ObjectRefMap) 하나의 준비 사본이다(리플렉션 B9i3c3 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절 "Registry 컨테이너" 준비 단위). 대입 묶음이
// 컨테이너의 새 내용을 객체를 바꾸지 않고 만들고, 게시 때 한 번에 바꾼다:
//   1. 준비: 시작 위치에 사본을 건 경로 연산(PathTarget::WithReferences)을 컨테이너 경로에 부른다.
//      처음 닿는 연산이 컨테이너를 사본에 묶고(지금 내용을 복사), 그 뒤의 원소·항목 읽기·쓰기·넣기·
//      지우기·비우기는 객체 대신 사본을 바꾼다. 검증(ReadOnly·대상 타입·생존·키·원소 수 한도)은
//      보통 경로 연산과 같고, 객체는 읽기 Borrow로만 본다(변경 표지를 올리지 않는다).
//   2. 게시 직전: PreparePublish가 새 버퍼를 만든다(구간 밖 할당). 간선 변경은 Previous()의
//   원소마다
//      {원소, null}, Current()의 원소마다 {null, 원소}이고 새 대상의 재확인 기대 타입은
//      TargetType()이다.
//   3. 구간 안: Publish(batch)가 컨테이너 내용을 무할당으로 맞바꾼다. staged 객체는 Publish()가
//      트랜잭션의 AssignReferences·AssignMapReferences로 바꾼다.
//   4. 옛 버퍼·옛 Map은 사본이 받고 Reset·소멸(구간 밖)에서 반환한다.
//
// 사용 예(살아 있는 객체의 ObjectRefArray 원소 하나를 바꾼다):
//   PreparedReferences stage(allocator);
//   const PathTarget staged = PathTarget(mesh).WithReferences(stage);
//   auto written = PathWrite(registry, access, staged, slotPath.View(), value);   // 사본만 바뀐다
//   ...
//   if (auto prepared = stage.PreparePublish(); !prepared) {
//       return prepared;
//   }
//
//   // targets·edges를 Previous()·Current()·TargetType()으로 채운 뒤
//   auto batch = registry.BeginBatchPublish(access, mesh, stamp, targets, edges);
//   stage.Publish(batch.Value());
//   batch.Value().End();
//   stage.Reset();   // 옛 버퍼 반환
namespace rocks {

class ConstructionContext;
class PreparedReferences;

namespace detail {

class PathCursor;

// 준비 사본의 컨테이너 형식별 연산 표다(생성 경로 thunk가 형식마다 정적 값으로 만든다 —
// Object/GeneratedStagedReferenceAccess.h).
struct ReferenceStageOps {
    // 컨테이너의 C++ 대상 타입(NativeTypeKey<T>)이다.
    const void* nativeType = nullptr;
    // Map 사본(OrderedMap<K, ObjectHandle>)을 소멸하고 반환한다. 배열은 nullptr이다(사본이 handle
    // 목록이다).
    void (*destroyClone)(void* clone) noexcept = nullptr;
    // Map 사본의 값을 항목 순서로 out 끝에 붙인다(배열은 nullptr). 오류: 할당 실패.
    Result<void> (*collect)(const void* clone, Array<ObjectHandle>& out) noexcept = nullptr;
    // 구간 안에서 컨테이너 내용을 사본과 맞바꾼다(무할당·무실패).
    void (*publish)(ObjectBatchPublish& batch, void* container,
                    PreparedReferences& stage) noexcept = nullptr;
    // staged 객체의 컨테이너를 사본 내용으로 통째로 바꾼다(트랜잭션 Assign*).
    Result<void> (*publishStaged)(ConstructionContext& transaction, ObjectHandle owner,
                                  void* container, PreparedReferences& stage) noexcept = nullptr;
};

// 형식별 thunk가 준비 사본의 내용에 닿는 통로다(사용자 코드는 쓰지 않는다).
struct PreparedReferencesAccess;

} // namespace detail

// Registry 컨테이너 하나의 준비 사본이다. 복사·이동 금지(경로 연산의 시작 위치가 주소를 쥔다),
// 소유 스레드 전용이다. 한 사본은 처음 묶은 컨테이너 하나에만 쓴다 — 다른 컨테이너에 닿는 연산은
// InvalidArgument(InvalidValue)다. 준비와 게시 사이에 그 객체의 컨테이너를 바꾸면 계약 위반이다
// (리플렉션 쓰기·Registry 참조 변경은 변경 표지가 잡아 BeginBatchPublish가 Busy다).
//
// 변경 통지: 구간 안의 게시는 Notify 필드를 알리지 않는다(그 구간의 통지는 Busy다 —
// Docs/Design/ChangeNotification.ko.md CN-D10·CN-D11). 게시한 호출자가 End 뒤에 게시한 최상위
// 필드를 ObjectRegistry::NotifyIfTarget으로 알린다(여러 자리면 NotifyGroup으로 한 번씩 — 경로
// 진입점과 ReflectionText 대입 묶음이 그렇게 한다). 알리지 않으면 구독자는 그 변경을 놓친다.
class ROCKS_OBJECTRUNTIME_API PreparedReferences {
public:
    // allocator는 handle 목록(배열 사본·Previous·Current)의 allocator다. Map 사본과 새 배열 버퍼는
    // 컨테이너의 allocator로 만든다(게시가 같은 allocator를 요구한다).
    explicit PreparedReferences(Allocator& allocator) noexcept
        : allocator_(&allocator), elements_(allocator), previous_(allocator), current_(allocator) {
    }

    ~PreparedReferences() {
        Reset();
    }

    PreparedReferences(const PreparedReferences&) = delete;
    PreparedReferences& operator=(const PreparedReferences&) = delete;
    PreparedReferences(PreparedReferences&&) = delete;
    PreparedReferences& operator=(PreparedReferences&&) = delete;

    // 컨테이너에 묶였는지다.
    bool IsReady() const noexcept {
        return container_ != nullptr;
    }

    // 묶은 컨테이너의 C++ 대상 타입(NativeTypeKey<T>)이다 — BatchTargetCheck의 기대 타입(묶기
    // 전에는 nullptr).
    const void* TargetType() const noexcept {
        return ops_ ? ops_->nativeType : nullptr;
    }

    // 묶을 때 컨테이너가 담던 handle이다(배열 순서·Map 항목 순서, null 포함) — 간선 변경의 removed.
    Span<const ObjectHandle> Previous() const noexcept {
        return previous_.View();
    }

    // 게시할 새 내용의 handle이다(PreparePublish 뒤 — 간선 변경의 added와 재확인 대상).
    Span<const ObjectHandle> Current() const noexcept {
        return current_.View();
    }

    // 게시를 준비한다(구간 밖): 새 handle 목록과 배열의 새 버퍼(컨테이너 allocator)를 만든다. 묶지
    // 않았으면 InvalidArgument, 할당 실패는 그 오류이고 그때 사본은 그대로다. 다시 부르면 다시
    // 만든다.
    Result<void> PreparePublish() noexcept;

    // batch(같은 owner의 열린 묶음 게시 구간) 안에서 컨테이너 내용을 사본과 맞바꾼다(할당·실패가
    // 없다). PreparePublish 뒤에 한 번이고, 간선 변경과 새 대상 재확인을 BeginBatchPublish에
    // 선언했어야 한다. 준비하지 않았거나 게시된 객체 자리가 아니거나 다른 owner의 구간이면
    // Fatal이다(계약 위반). 옛 내용은 사본이 받는다.
    void Publish(ObjectBatchPublish& batch) noexcept;

    // staged 객체의 컨테이너를 사본 내용으로 바꾼다(트랜잭션의 AssignReferences·AssignMapReferences
    // — 대상 규칙과 오류는 그 함수와 같고, 실패하면 컨테이너는 그대로다). 묶지 않았거나 staged
    // 자리가 아니면 InvalidArgument다.
    Result<void> Publish() noexcept;

    // 사본·버퍼·목록을 반환하고 묶음을 푼다(해제 콜백이 이 안에서 돈다 — 게시 구간 밖에서 부른다).
    void Reset() noexcept;

private:
    friend class detail::PathCursor;
    friend struct detail::PreparedReferencesAccess;

    Allocator* allocator_;
    const detail::ReferenceStageOps* ops_ = nullptr;
    void* container_ = nullptr;
    // Map 사본(OrderedMap<K, ObjectHandle> — 컨테이너 allocator)이다(배열은 nullptr).
    void* clone_ = nullptr;
    // 배열 사본과 그 게시 버퍼(컨테이너 allocator)다.
    Array<ObjectHandle> elements_;
    UniquePtr<PreparedReferenceArray> buffer_;
    Allocator* containerAllocator_ = nullptr;
    Array<ObjectHandle> previous_;
    Array<ObjectHandle> current_;
    ObjectHandle owner_{};
    PathTarget::Kind kind_ = PathTarget::Kind::Object;
    ConstructionContext* transaction_ = nullptr;
};

namespace detail {

struct PreparedReferencesAccess {
    static Array<ObjectHandle>& Elements(PreparedReferences& stage) noexcept {
        return stage.elements_;
    }

    static void* Clone(PreparedReferences& stage) noexcept {
        return stage.clone_;
    }

    static PreparedReferenceArray& Buffer(PreparedReferences& stage) noexcept {
        return *stage.buffer_;
    }
};

} // namespace detail

} // namespace rocks
