#pragma once

#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

namespace rocks {

// 소유 스레드에 묶인 비공개 생성 트랜잭션. 생성된 객체는 Commit 전 Resolve/TryPin할 수 없다.
// Commit하지 않고 범위를 벗어나면 생성 역순으로 롤백한다. 객체 포인터를 외부에 유출하지 않는다.
// Registry보다 먼저 파괴해야 한다. 현재 활성 트랜잭션이 있으면 Collect는 Busy다.
//
// 트랜잭션 상태 규칙(Create·Commit·DescribeOuter 공통): 고정된 오류가 있으면 그 오류를 돌려준다 —
// 시작 실패(생성자의 접근 WrongThread·WrongContext, 종료 ShuttingDown, 재진입 Busy 등)와 실패한
// Create의 오류이며, Rollback 뒤에도 남는다. 고정된 오류가 없고 끝난(Commit·Rollback) 트랜잭션이면
// InvalidArgument다.
//
// Required 검사 중 표시(Docs/Design/ObjectFoundations.ko.md 4절): 그래프 로드가 적용 뒤·Commit 전에
// staged 객체의 실효 값을 encode 경로로 검사하는 동안(FieldEncodeContext::BeginRequiredCheck부터 그
// 문맥이 끝날 때까지) Create·SetReference·AssignReferences·AssignElements·SetElementReference·
// GetForWrite(등록된 생성 staged 쓰기 writeStaged와 그래프 적용의 쓰기가 이것으로 owner를 얻는다)·
// Commit은 Busy다(encode 고정의 staged판 — hook·allocator 콜백이 검사 중인 값을 바꾸지 못한다). 이
// Busy는 트랜잭션 상태를 바꾸지 않는다(오류로 고정하지 않는다). Get·DescribeOuter 같은 읽기는 된다
// (Get이 준 포인터로 값을 바꾸는 것은 막지 못한다 — 콜백 계약이 1차 방어다). 표시 중의
// Rollback·소멸은 검사가 읽는 객체를 없애므로 Fatal이다(계약 위반).
class ConstructionContext {
public:
    ConstructionContext(ObjectRegistry&, const ObjectAccessContext&) noexcept;
    ~ConstructionContext();
    ConstructionContext(const ConstructionContext&) = delete;
    ConstructionContext& operator=(const ConstructionContext&) = delete;

    // type 객체를 outer 아래 name으로 만든다(Docs/Design/ObjectFoundations.ko.md 1절). 공개 전
    // 객체도 생성 때부터 이름을 점유하므로 다른 트랜잭션이 같은 이름을 만들 수 없다.
    //
    // 인자:
    // - outer: Outer(없으면 ObjectHandle{}). 생존(Alive) 객체이거나 이 트랜잭션의 공개 전 객체여야
    //   한다. Outer와 이름은 객체 수명 동안 바뀌지 않는다.
    // - name: 이름. 빈 문자열은 Outer가 없으면 이름 없는 객체(이름 색인에 넣지 않는다), Outer가
    //   있으면 대표 자식이다(Outer가 outermost이고 이름 있는 Inner가 없을 때만). 이름 규칙은
    //   ValidateNameComponent(Core/Text/NameComponent.h)이고 Registry는 정규화하지 않는다.
    //
    // 오류 순서: 0. 트랜잭션 상태(클래스 주석의 규칙), Required 검사 중 표시(Busy — 상태를 고정하지
    // 않는다), 저널 예약 OutOfMemory → 1. 접근(WrongThread·WrongContext) → 2. 종료·재진입
    // (ShuttingDown·Busy) → 3. 타입(NotFound·TypeMismatch) → 4. Abstract·생성 함수 없음·패키지
    // 정책 Registry의 완전한 필드 배치 아닌 타입(Unsupported) → 5. Outer 상태(StaleHandle — 생존·이
    // 트랜잭션의 공개 전 객체가 아님, 다른 Registry의 handle, GC cutoff white, DestroyRequested)
    // → 6. WorldOnly 정책(Unsupported) → 7. 이름 형식(InvalidArgument, NFC 판정 OutOfMemory) → 8.
    // 패키지 정책·빈 이름·대표 자식 규칙(InvalidArgument) → 9. 경로 길이(CapacityExceeded) → 10.
    // 비교 키·이름 기록·색인 성장 할당(OutOfMemory, 색인이 더 자랄 수 없으면 CapacityExceeded)
    // → 11. 이름 충돌(AlreadyExists) → 12. 객체 할당(OutOfMemory)·슬롯 상한(CapacityExceeded).
    // 실패는 트랜잭션을 오류 상태로 고정한다(도구는 ObjectRegistry::IsNameTaken으로 먼저 확인한다).
    //
    // 사용 예:
    //   auto asset = transaction.Create<Mesh>(MeshType, package);   // 대표 자식
    //   if (!asset) {
    //       return Result<void>::Failure(asset.Error());
    //   }
    //
    //   auto body = transaction.Create<Part>(PartType, asset.Value(), "Body");
    template <class T>
    Result<ObjectHandle> Create(TypeId type, ObjectHandle outer = {},
                                StringView name = {}) noexcept {
        return CreateImpl(type, NativeTypeKey<T>(), outer, name);
    }

    // outer를 Outer로 쓸 때의 요약이다(ObjectRegistry::DescribeOuter와 같고 이 트랜잭션의 공개 전
    // 객체도 받는다). 트랜잭션 상태(클래스 주석의 규칙) → outer가 null이면 InvalidArgument →
    // 생존·이 트랜잭션의 공개 전 객체가 아니면 StaleHandle이다.
    Result<OuterSummary> DescribeOuter(ObjectHandle outer) noexcept;

    ObjectRegistry& Registry() const noexcept {
        return registry_;
    }

    const ObjectAccessContext& Access() const noexcept {
        return access_;
    }

    // 이 트랜잭션의 ID다(관찰자 Commit 기록 ReferenceEdgeDelta::transaction과 같다 — Commit 뒤에는
    // 0이 되므로 소비자는 Commit 전에 읽어 둔다). 끝났거나 시작하지 못한 트랜잭션은 0이다.
    uint64 TransactionId() const noexcept {
        return finished_ ? 0 : transaction_;
    }

    // 같은 트랜잭션의 객체를 초기화할 때만 사용한다. 반환 포인터는 완료 이후 보관하지 않는다.
    // 실패 가능한 초기화 결과를 호출자가 확인하고 실패 시 Commit을 생략한다.
    template <class T>
    Result<T*> Get(ObjectHandle handle) noexcept {
        if (finished_) {
            return Result<T*>::Failure(ErrorCode::InvalidArgument);
        }

        auto result = registry_.GetPending(access_, handle, transaction_, NativeTypeKey<T>());
        if (!result) {
            return Result<T*>::Failure(result.Error());
        }

        // Registry는 Object 하위 객체 주소를 준다. 파생 객체의 기반 T 접근도 static_cast가
        // 보정한다.
        return Result<T*>::Success(static_cast<T*>(static_cast<Object*>(result.Value())));
    }

    // staged 객체에 값을 쓰려고 얻는 Get이다(등록된 생성 staged 쓰기
    // detail::WriteGeneratedStagedField와 그래프 적용 codec의 struct·배열 쓰기가 쓴다). 끝난
    // 트랜잭션은 InvalidArgument, Required 검사 중 표시 동안은 Busy(상태를 고정하지 않는다)이고 그
    // 밖은 Get과 같다. 반환 포인터 규칙도 Get과 같다.
    //
    // 사용 예(수동 writeStaged):
    //   auto owner = transaction.GetForWrite<Mesh>(object);
    //   if (!owner) {
    //       return Result<void>::Failure(owner.Error());
    //   }
    //
    //   owner.Value()->lodCount = static_cast<uint32>(value.unsignedInteger);
    template <class T>
    Result<T*> GetForWrite(ObjectHandle handle) noexcept {
        if (!finished_ && requiredCheck_) {
            return Result<T*>::Failure(ErrorCode::Busy);
        }

        return Get<T>(handle);
    }

    // staged owner의 ObjectRef field에 target을 쓴다. 트랜잭션이 끝났거나 오류 상태면
    // InvalidArgument, Required 검사 중 표시 동안 Busy다(아래 컨테이너 쓰기도 같다).
    template <class T>
    Result<void> SetReference(ObjectHandle owner, ObjectRef<T>& field,
                              ObjectHandle target) noexcept {
        if (auto writable = CheckStagedWrite(); !writable) {
            return writable;
        }

        return registry_.SetEdge(access_, owner, field.handle_, target, transaction_,
                                 NativeTypeKey<T>());
    }

    // 이 트랜잭션의 staged 객체 owner 안 ObjectRefArray 내용을 targets로 통째로 바꾼다(그래프
    // 로드가 원소마다 부르지 않고 한 번에 채운다). owner는 이 트랜잭션의 Reserved여야 하고(Borrow
    // 불요 — Alive·다른 트랜잭션이면 StaleHandle), 배열은 owner 저장소 안이어야 한다(아니면
    // InvalidArgument). 원소는 null(ObjectHandle{}로 정규화), 생존 객체, 또는 이 트랜잭션의
    // Reserved 객체다(그 밖 StaleHandle — GC cutoff 포함, T의 파생이 아니면 TypeMismatch). 용량이
    // 모자라면 레지스트리 처리 표시(processing_) 아래 잠금 밖에서 새 버퍼를 할당한다(그동안의
    // Registry 재진입은 Busy). 생존 대상만 shade하고 Reserved 대상은 Commit이 shade한다. Commit은
    // owner Trace로 원소를 다시 검증한다. 실패하면 배열 내용은 바뀌지 않는다. 트랜잭션이 끝났거나
    // 오류 상태면 InvalidArgument다.
    //
    // 사용 예(owner·actor는 같은 트랜잭션의 Create·Get 결과):
    //   const ObjectHandle components[] = {first, {}, second};
    //   if (auto assigned = transaction.AssignReferences(owner, actor->components_, components);
    //       !assigned) {
    //       return assigned;
    //   }
    template <class T>
    Result<void> AssignReferences(ObjectHandle owner, ObjectRefArray<T>& array,
                                  Span<const ObjectHandle> targets) noexcept {
        if (auto writable = CheckStagedWrite(); !writable) {
            return writable;
        }

        return registry_.AssignReferencesStaged(access_, owner, array, targets, transaction_);
    }

    // staged owner 안 StructRefArray를 count개의 기본 원소로 통째로 바꾼다. owner 규칙은
    // AssignReferences와 같다. 블록 할당·원소 생성·옛 원소 소멸은 처리 표시 아래 잠금 밖에서 하며,
    // 하나라도 실패하면 만든 원소를 모두 되돌리고 배열은 바뀌지 않는다(옛 원소는 게시 뒤 소멸한다).
    // count가 uint32 상한을 넘으면 Overflow다.
    //
    // 사용 예:
    //   if (auto assigned = transaction.AssignElements(owner, mesh->materials, count); !assigned) {
    //       return assigned;
    //   }
    //
    //   return transaction.SetElementReference(owner, mesh->materials, 0,
    //                                          mesh->materials[0].material, material);
    template <class S>
    Result<void> AssignElements(ObjectHandle owner, StructRefArray<S>& array,
                                usize count) noexcept {
        if (auto writable = CheckStagedWrite(); !writable) {
            return writable;
        }

        return registry_.AssignElementsStaged(access_, owner, array, count, transaction_);
    }

    // staged owner 안 StructRefArray 원소(element 위치)의 참조 멤버 field에 target을 쓴다
    // (SetReference의 원소판). 컨테이너가 owner 저장소 안·index < size·field가 그 원소 블록
    // 안이어야 한다(아니면 InvalidArgument). 대상 규칙은 SetReference와 같다(null·생존·이
    // 트랜잭션의 Reserved). element는 detail::StructRefArrayAccess::Location으로 만든다(codec용).
    template <class T>
    Result<void> SetElementReference(ObjectHandle owner,
                                     const detail::StructElementLocation& element,
                                     ObjectRef<T>& field, ObjectHandle target) noexcept {
        if (auto writable = CheckStagedWrite(); !writable) {
            return writable;
        }

        return registry_.SetEdge(access_, owner, field.handle_, target, transaction_,
                                 NativeTypeKey<T>(), &element);
    }

    // 위와 같고 (배열, index)로 원소를 고른다.
    template <class S, class T>
    Result<void> SetElementReference(ObjectHandle owner, StructRefArray<S>& array, usize index,
                                     ObjectRef<T>& field, ObjectHandle target) noexcept {
        if (auto writable = CheckStagedWrite(); !writable) {
            return writable;
        }

        return registry_.SetElementReferenceStaged(access_, owner, array, index, field, target,
                                                   transaction_);
    }

    // 모든 staged 간선과 Outer를 검증한 뒤 이 트랜잭션의 객체를 한 번에 공개하고, roots의 객체마다
    // RootRef를 out의 같은 위치에 하나씩 돌려준다. 같은 handle을 두 번 주면 RootRef도 둘이다.
    // roots가 비어도 된다 — 공개한 객체는 다른 생존 객체가 붙잡지 않으면 다음 GC에서 회수된다(inner
    // → outer 간선이 있으므로 살아 있는 Inner는 Outer를 붙잡는다).
    //
    // 순서와 오류: 트랜잭션 상태(클래스 주석의 규칙)·out 검사
    // (out.Size() != roots.Size()이거나 비어 있지 않은 원소가 있으면 InvalidArgument) → Required
    // 검사 중 표시(Busy — 상태를 고정하지 않는다) → 접근
    // (WrongThread·WrongContext) → 종료·재진입(ShuttingDown·Busy) → roots(이 트랜잭션의 객체가
    // 아니면 StaleHandle, roots가 uint32 개수를 넘으면 Overflow) → staged 간선·Outer 검증과 관찰자
    // 쌍 집계(잠금 밖, 처리 표시 아래 — 대상·Outer가 생존 또는 이 트랜잭션 객체가 아니거나 cutoff
    // white면 StaleHandle, 집계 할당 실패 OutOfMemory) → 관찰자 예약(관찰자 allocator —
    // OutOfMemory, 키 상한 초과 CapacityExceeded) → 한 잠금 안에서 공개·root 증가·관찰자 기록.
    // 실패하면 비공개 상태와 out을 그대로 두며 호출자가 Rollback하거나 소멸자가 한다.
    //
    // 관찰자 Registry(RegistryOptions::referenceObserver)에서는 공개하는 객체 중 Outer가 있는 것의
    // 다른 패키지로 가는 강한 간선을 (소유 패키지, 대상 패키지) 쌍별로 합쳐 Commit 원인·transaction
    // = TransactionId()로 한 번 기록한다(staged 쓰기는 따로 기록하지 않는다 — 키는 staged 저널 순 ×
    // trace 순으로 처음 생긴다).
    //
    // 사용 예(패키지 둘을 한 트랜잭션으로 공개 — 하드 순환 SCC):
    //   const ObjectHandle roots[] = {packageA, packageB};
    //   RootRef<Object> held[2];
    //   if (auto committed = transaction.Commit(roots, held); !committed) {
    //       return committed;
    //   }
    Result<void> Commit(Span<const ObjectHandle> roots, Span<RootRef<Object>> out) noexcept;

    // 첫 Create 객체 하나를 root로 공개한다(Commit({첫 객체}, out)과 같다). 만든 객체가 없으면
    // InvalidArgument다. 나머지는 이 루트에서 도달하거나 별도로 Pin해야 다음 GC에서 생존한다. 실패
    // 시 비공개 상태를 유지한다. Create 실패는 트랜잭션을 오류 상태로 고정한다.
    Result<RootRef<Object>> Commit() noexcept;
    // 끝나지 않은 트랜잭션의 객체를 생성 역순으로 되돌린다(끝났으면 무동작). Required 검사 중
    // 표시 동안 부르면 Fatal이다(클래스 주석).
    void Rollback() noexcept;

private:
    friend class ObjectRegistry;
    // 검사 중 표시를 세우고 내린다(BeginRequiredCheck·문맥 소멸).
    friend class FieldEncodeContext;
    // 클래스 주석의 트랜잭션 상태 규칙이다.
    Result<void> CheckOpen() const noexcept;

    // staged 쓰기(SetReference·컨테이너 쓰기)의 공통 판정이다: 끝났거나 오류 상태면
    // InvalidArgument, Required 검사 중 표시 동안 Busy다.
    Result<void> CheckStagedWrite() const noexcept {
        if (finished_ || error_ != ErrorCode::None) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (requiredCheck_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return Result<void>::Success();
    }

    Result<ObjectHandle> CreateImpl(TypeId type, const void* nativeType, ObjectHandle outer,
                                    StringView name) noexcept;

    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    Array<ObjectHandle> staged_;
    uint64 transaction_ = 0;
    ErrorCode error_ = ErrorCode::None;
    bool finished_ = false;
    // Required 검사 중 표시다(클래스 주석).
    bool requiredCheck_ = false;
};

} // namespace rocks
