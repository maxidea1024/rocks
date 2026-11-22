#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Object/ObjectHandle.h>

namespace rocks {

class ObjectRegistry;

// 패키지 사이 강한 간선 다중도가 바뀐 원인이다(Docs/Design/ObjectFoundations.ko.md 3절).
enum class ReferenceChangeCause : uint8 {
    Edit,   // 공개된 객체의 live 참조 쓰기·컨테이너 변경(transaction 0)
    Commit, // 생성 트랜잭션 공개(transaction = 그 트랜잭션)
    Discard // 부분 트리 Discard가 나가는 간선을 반납(transaction 0)
};

// 패키지 사이 강한 간선 다중도의 순 변화 하나다. 패키지는 객체의 outermost다.
struct ReferenceEdgeDelta {
    ObjectHandle ownerPackage{};
    ObjectHandle targetPackage{};
    int64 delta = 0; // 같은 키의 변화를 합산한 순 변화(0인 키는 TakeChanges가 뺀다)
    ReferenceChangeCause cause = ReferenceChangeCause::Edit;
    // Commit이면 ConstructionContext::TransactionId()(공개 전 값), Edit·Discard는 0이다.
    uint64 transaction = 0;
};

// 패키지 정책 ObjectRegistry가 패키지 사이 강한 간선의 다중도 변화를 쌓는 순 다중도 합산 표다(AS
// 6.1·11.4 — AssetManager의 상주 단위 계수용). 가상 콜백이 아니라 Registry가 쓰는 표이고, 소비자는
// Pump에서 TakeChanges로 꺼낸다 — Registry 잠금 안에서 사용자 코드를 부르지 않는다. 같은 (소유
// 패키지, 대상 패키지, 원인, 트랜잭션) 키의 변화는 하나로 합산하므로 TakeChanges 사이 편집이 많아도
// 크기가 키 수로 묶인다(maxKeys).
//
// 기록 규칙(Registry가 지킨다): 소유자가 Alive이고 Outer를 가진(패키지 자신이 아닌) 객체이며
// 소유자와 대상의 outermost가 다른 강한 간선(ObjectRef·ObjectRefArray 원소·StructRefArray 원소의
// 참조 멤버)만 센다. 공개 전 객체의 staged 쓰기는 Commit이 한꺼번에 보고하고, Discard가 부분
// 트리에서 나가는 다중도를 한 번 음수로 반납하므로 DestroyRequested 소유자의 제거·비우기는 기록하지
// 않는다. 패키지 객체가 소유한 간선과 같은 패키지 안 간선도 기록하지 않는다.
//
// 수명·스레드: ObjectRegistry를 만들 때 RegistryOptions::referenceObserver로 붙이며, 패키지 정책
// Registry 하나에만 붙고(아니면 Registry 생성이 Fatal) 그 Registry보다 오래 살아야 한다(붙은 채
// 소멸하면 Fatal). 붙은 동안 TakeChanges·PendingCount는 그 Registry의 소유 스레드 전용이다.
// Registry가 소멸하면 떨어지고, 남은 변화는 계속 꺼낼 수 있다.
//
// 사용 예(A1 AssetManager):
//   ReferenceObserver observer(allocator);
//   RegistryOptions options;
//   options.policy = {NativeTypeKey<Package>(), true};
//   options.referenceObserver = &observer;
//   ObjectRegistry assets(allocator, types, context, options);
class ROCKS_OBJECTRUNTIME_API ReferenceObserver {
public:
    // TakeChanges 사이에 둘 수 있는 키 수의 기본 상한이다.
    static constexpr usize DefaultMaxKeys = usize(1) << 20;
    // maxKeys가 포화하는 값이다(색인 위치가 uint32다).
    static constexpr usize MaxKeysLimit = usize(1) << 30;

    // allocator는 합산 표(항목 배열·색인)를 할당하며 관찰자보다 오래 살아야 한다. maxKeys는
    // TakeChanges 사이에 둘 수 있는 키 수 상한이다(0이 된 키도 TakeChanges 전까지 센다,
    // MaxKeysLimit에서 포화). 새 키가 이것을 넘게 되는 기록 지점은 CapacityExceeded이고 그 변경은
    // 적용되지 않는다. Commit·Discard 하나는 그 작업의 패키지 쌍을 한 번에 기록하므로 maxKeys는
    // 단일 Commit·Discard가 만들 수 있는 최대 쌍 수보다 커야 한다 — 아니면 그 작업은 비운 표에서도
    // 언제나 CapacityExceeded다. 생성은 할당하지 않는다.
    explicit ReferenceObserver(Allocator& allocator, usize maxKeys = DefaultMaxKeys) noexcept;
    // 붙은 Registry가 남아 있으면 Fatal이다.
    ~ReferenceObserver();
    ReferenceObserver(const ReferenceObserver&) = delete;
    ReferenceObserver& operator=(const ReferenceObserver&) = delete;

    // 쌓인 변화를 out과 맞바꾼다. 0이 된 항목은 맞바꾸기 전에 제자리에서 지우고, 순서는 키가 처음
    // 생긴 순서다(같은 작업 순서면 같은 결과 — 결정적). 이 순서는 시간 순서가 아니다: 한 키의 합
    // 안에 앞뒤 변화가 섞여 있으므로 소비자는 묶음 전체를 더한 뒤에만 다중도·Closing을
    // 판정한다(중간 합은 음수일 수 있다). 맞바꾼 뒤 관찰자는 out이 갖고 있던 빈 배열을 쓴다(다음
    // 기록이 공간을 다시 예약한다). 할당하지 않는다.
    //
    // 오류: 붙은 Registry의 소유 스레드가 아니면 WrongThread, out이 비어 있지 않거나 관찰자와 같은
    // allocator(주소 비교)가 아니면 InvalidArgument, Registry가 관찰자 공간을 예약하는 중(그 할당
    // 콜백 재진입)이면 Busy다. 실패하면 관찰자와 out은 바뀌지 않는다.
    //
    // 사용 예(AssetLoader::Pump — Closing 처리 전에):
    //   Array<ReferenceEdgeDelta> changes(allocator);
    //   if (auto taken = observer.TakeChanges(changes); !taken) {
    //       return taken;
    //   }
    //
    //   for (const ReferenceEdgeDelta& change : changes) {
    //       // (소유 패키지, 대상 패키지) 다중도 갱신, Commit이면 transaction으로 로드 작업의 내부
    //       // 보유를 실제 간선 다중도로 바꾼다
    //   }
    Result<void> TakeChanges(Array<ReferenceEdgeDelta>& out) noexcept;

    // 지금 쌓인 키 수다(0이 된 키 포함). 붙은 Registry의 소유 스레드 전용이다.
    usize PendingCount() const noexcept {
        return entries_.Size();
    }

    // 생성 때 정한(MaxKeysLimit에서 포화한) 키 수 상한이다. 생성 뒤 바뀌지 않으므로 어느
    // 스레드에서나 읽을 수 있다.
    usize MaxKeys() const noexcept {
        return maxKeys_;
    }

private:
    friend class ObjectRegistry;

    // 잠금 밖 — 새 키 newKeys개를 받을 공간(항목 배열 + 색인)을 반환 시점에 보장한다. 판정 순서:
    // 지금 키 수 + newKeys가 maxKeys를 넘으면 CapacityExceeded → 이미 공간이 있으면 할당 없이
    // 성공 → 할당 콜백 안에서 다시 부른 것이면(중첩 예약) Busy → 새 배열·색인을 모두 할당한 뒤
    // 옮긴다. 그 할당 콜백 중 Add로 옛 표에 들어간 항목도 옮기고(그 항목이 새 공간을 써 버렸을 수
    // 있으므로 호출자는 다시 잠근 뒤 FreeKeys로 재확인한다), 옮길 수 없을 만큼 늘었으면 Busy다.
    // 할당 실패는 그 오류다. 실패하면 표는 그대로다.
    Result<void> Reserve(usize newKeys) noexcept;
    // 잠금 안 — 무할당. 키가 있으면 더하고, 없으면 예약한 공간에 넣는다(공간이 없으면 Fatal).
    void Add(ObjectHandle owner, ObjectHandle target, int64 delta, ReferenceChangeCause cause,
             uint64 transaction) noexcept;
    // 잠금 안 — 지금 남은 새 키 공간이다(재검증용).
    usize FreeKeys() const noexcept;
    // 잠금 안 — 키가 이미 있는지다(새 키 수 계산용).
    bool Contains(ObjectHandle owner, ObjectHandle target, ReferenceChangeCause cause,
                  uint64 transaction) const noexcept;
    // 키의 항목 위치 + 1이다(없으면 0).
    uint32 Find(uint64 hash, ObjectHandle owner, ObjectHandle target, ReferenceChangeCause cause,
                uint64 transaction) const noexcept;

    Allocator& allocator_;
    usize maxKeys_;
    // 키가 처음 생긴 순서의 항목이다.
    Array<ReferenceEdgeDelta> entries_;
    // 개방 주소 색인(2의 거듭제곱 칸, 항목 위치 + 1 — 0은 빈 칸)이다. 적재율 1/2 이하이고 지우지
    // 않는다(TakeChanges가 통째로 비운다). Core HashMap을 쓰지 않는 이유: 잠금 안 Add는 예약한
    // 공간에 무할당으로 들어가야 하고, 예약(할당) 콜백 중 옛 표에 들어간 항목을 새 표로 옮겨야
    // 하는데 HashMap은 할당 콜백 중 같은 Map 재진입을 금지하고 삽입 때 스스로 재해시·할당한다.
    uint32* index_ = nullptr;
    usize indexCapacity_ = 0;
    // 붙은 Registry와 그 소유 스레드다(Registry 생성자·소멸자가 정한다).
    ObjectRegistry* registry_ = nullptr;
    uint64 ownerThread_ = 0;
    bool reserving_ = false;
};

} // namespace rocks
