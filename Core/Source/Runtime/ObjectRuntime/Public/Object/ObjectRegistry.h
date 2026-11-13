#pragma once

#include <Core/Containers/Array.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <Object/ObjectDiagnostics.h>
#include <Object/StructRefArray.h>
#include <Object/TypeRegistry.h>

namespace rocks {

class WorkerScheduler;

enum class GcPhase : uint8 {
    Idle,
    Marking,
    Sweeping,
    Reclaiming
};

// 시간 예산은 soft deadline이다. Trace/소멸자 한 번을 중간에 중단하지 않는다.
// Begin의 root scan·잠금 획득·회수 후보 탐색도 hard deadline으로 제한하지 않는다.
// maxObjects는 trace·slot 분류·실제 회수의 합이며 0은 잘못된 설정이다.
// maxNanoseconds=0은 시간 한도를 끄되 객체 작업량 한도는 유지한다.
struct GcBudget {
    uint32 maxObjects = 64;
    uint64 maxNanoseconds = 1000000;
};

// 한도는 생성 시 고정한다. maxGeneration을 낮추면 슬롯 고갈 회귀 시험을 할 수 있다.
struct RegistryOptions {
    uint32 maxSlots = 1048576;
    uint32 maxGeneration = 0xffffffffu;
    // 기본값은 scope 없는 객체 자체 호출에 Registry의 문맥을 사용한다. World/PIE dispatcher가
    // 실행 문맥을 설정하는 구성은 true로 하여 무인자 생명주기·타입 조회 API의 scope 누락을
    // 거부한다. 명시적 ObjectAccessContext를 받는 Registry API의 검증에는 영향을 주지 않는다.
    bool requireLifecycleScope = false;
};

struct ObjectStats {
    uint32 liveObjects = 0;
    uint32 reservedObjects = 0;
    uint32 destroyRequested = 0;
    uint32 retiredObjects = 0;
    uint32 roots = 0;
    uint32 borrows = 0;
    uint32 backendHolds = 0;
    // 쥐고 있는 encode 고정(EncodePin)의 합이다. 0이 아니면 Shutdown이 Busy다.
    uint32 encodePins = 0;
    uint32 chunks = 0;
};

// 성공한 한 cycle의 결과다. 시간은 실제 collector slice의 합(ns)이며 mutator 시간은 제외한다.
struct GcReport {
    uint32 examined = 0;
    uint32 marked = 0;
    uint32 retired = 0;
    uint32 reclaimed = 0;
    uint64 durationNanoseconds = 0;
};

struct GcProgress {
    GcPhase phase = GcPhase::Idle;
    uint32 processed = 0;
    GcReport report;

    bool IsComplete() const noexcept {
        return phase == GcPhase::Idle;
    }
};

// 비이동 객체 저장소와 GC를 소유한다. Allocator/TypeRegistry가 먼저 생성되고 나중에 파괴되어야
// 한다. 객체 접근·변경·GC는 생성 스레드 전용이다. gate는 통계와 참조 계수를 보호할 뿐, 사용자 객체
// 필드를 스레드 안전하게 만들지 않는다. 별도 래퍼의 반환/복제 예외는 ObjectRefs 참고. 종료 시 모든
// 루트·Borrow·BackendLease·EncodePin을 반환한다. 남은 소유권을 무시한 파괴는 즉시 종료한다.
class ObjectRegistry {
public:
    ObjectRegistry(Allocator&, TypeRegistry&, ContextId, RegistryOptions = {}) noexcept;
    ~ObjectRegistry();
    ObjectRegistry(const ObjectRegistry&) = delete;
    ObjectRegistry& operator=(const ObjectRegistry&) = delete;

    ObjectAccessContext OwnerAccess() const noexcept {
        return {context_};
    }

    // 로더가 검사한 schema/code lease와 실제 생성에 쓰는 registry가 같은지 확인한다.
    // 타입 ID가 우연히 같다는 이유로 서로 다른 등록 저장소의 callback을 혼합하지 않는다.
    bool UsesTypeRegistry(const TypeRegistry& registry) const noexcept {
        return &types_ == &registry;
    }

    // 기본 생성과 공개를 한 트랜잭션으로 실행한다. 실패 가능한 별도 초기화에는 ConstructionContext
    // 사용.
    template <class T>
    Result<RootRef<T>> NewObject(const ObjectAccessContext& access, TypeId type) noexcept {
        auto root = NewRoot(access, type, NativeTypeKey<T>());
        if (!root) {
            return Result<RootRef<T>>::Failure(root.Error());
        }

        RootRef<T> typed(this, root.Value().handle_);
        root.Value().registry_ = nullptr;
        root.Value().handle_ = {};
        return Result<RootRef<T>>::Success(Move(typed));
    }

    template <class T = Object>
    Result<Borrow<T>> Resolve(const ObjectAccessContext& access, ObjectHandle handle) noexcept {
        auto result = AddBorrow(access, handle, NativeTypeKey<T>());
        if (!result) {
            return Result<Borrow<T>>::Failure(result.Error());
        }

        return Result<Borrow<T>>::Success(Borrow<T>(this, handle));
    }

    // 필드 값을 바꾸려고 쥐는 Borrow다. Resolve와 같고, owner가 encode 고정(PinForEncode) 중이면
    // Busy다 — encode가 순회하는 owner 밖 버퍼(String·Soft 경로)를 쓰기가 해제하지 못하게 한다.
    // 생성 필드 쓰기(detail::WriteGeneratedField)가 쓴다.
    //
    // 사용 예:
    //   auto writable = registry.ResolveForWrite<Node>(access, node);
    //   if (!writable) { return Result<void>::Failure(writable.Error()); }
    template <class T = Object>
    Result<Borrow<T>> ResolveForWrite(const ObjectAccessContext& access,
                                      ObjectHandle handle) noexcept {
        auto result = AddBorrow(access, handle, NativeTypeKey<T>(), true);
        if (!result) {
            return Result<Borrow<T>>::Failure(result.Error());
        }

        return Result<Borrow<T>>::Success(Borrow<T>(this, handle));
    }

    template <class T = Object>
    Result<RootRef<T>> TryPin(const ObjectAccessContext& access, ObjectHandle handle) noexcept {
        auto result = AddRoot(access, handle, NativeTypeKey<T>());
        if (!result) {
            return Result<RootRef<T>>::Failure(result.Error());
        }

        return Result<RootRef<T>>::Success(RootRef<T>(this, handle));
    }

    // owner 저장소 안에 직접 포함된 field만 허용한다. 대상 타입/문맥/생존 검증 실패 시 field는
    // 불변이다. Marking 동안 대상 shade와 field 게시를 같은 gate 안에서 수행한다. owner가 encode
    // 고정(PinForEncode) 중이면 Busy다.
    template <class T>
    Result<void> SetReference(const ObjectAccessContext& access, ObjectHandle owner,
                              ObjectRef<T>& field, ObjectHandle target) noexcept {
        return SetEdge(access, owner, field.handle_, target, 0, NativeTypeKey<T>());
    }

    // ObjectRefArray의 live 변경 API다(ObjectRefArray.h). 공통 규칙:
    // - owner는 게시된 생존 객체(Alive — 제거·비우기는 DestroyRequested도)이고 호출자가 owner의
    //   Borrow를 쥐고 있어야 한다(없으면 InvalidArgument). 배열은 owner 저장소 안의 멤버여야
    //   한다(아니면 InvalidArgument). owner가 encode 고정 중이면 Busy다.
    // - target이 null(IsNull — registry 0 또는 generation 0)이면 검증·장벽 없이 ObjectHandle{}로
    //   정규화해 넣는다. non-null target은 같은 Registry의 생존 T(또는 파생) 객체여야 하고(아니면
    //   StaleHandle — GC cutoff 뒤의 white 객체 포함, 타입이 다르면 TypeMismatch), Marking 중이면
    //   shade한 뒤 게시한다(삽입 장벽).
    // - 원소 버퍼가 가득 차면 Registry 잠금 밖에서 배열의 allocator로 새 버퍼를 할당만 하고(실패 시
    //   OutOfMemory), 다시 잠근 뒤 owner·대상을 재검증하며 그때의 size·index로 다시 판단한다. 그
    //   사이 allocator 콜백이 같은 배열을 바꿔 새 버퍼가 모자라거나 index가 size를 넘으면 Busy,
    //   용량이 충분해졌으면 기존 버퍼에 넣는다. 레지스트리 전체 처리 표시(processing_)를 세우지
    //   않으므로 다른 스레드의 RootRef 복제 등을 막지 않는다.
    // - 실패하면 배열 내용은 바뀌지 않는다. GC·callback 처리 중에는 Busy, 종료 시작 후에는
    //   ShuttingDown이다. MaxElements(리플렉션 필드 상한)는 여기서 걸지 않는다(저장할 때 검사한다).

    // target을 끝에 추가한다.
    //
    // 사용 예:
    //   registry.AddReference(access, actor, actorPointer->components_, component);
    template <class T>
    Result<void> AddReference(const ObjectAccessContext& access, ObjectHandle owner,
                              ObjectRefArray<T>& array, ObjectHandle target) noexcept {
        return InsertArrayEdge(access, owner, array.storage_, 0, true, target, NativeTypeKey<T>());
    }

    // target을 index 위치에 넣고 뒤 원소를 민다(index == Size()면 끝). index > Size()는
    // InvalidArgument다. 도구 편집용이다.
    template <class T>
    Result<void> InsertReferenceAt(const ObjectAccessContext& access, ObjectHandle owner,
                                   ObjectRefArray<T>& array, usize index,
                                   ObjectHandle target) noexcept {
        return InsertArrayEdge(access, owner, array.storage_, index, false, target,
                               NativeTypeKey<T>());
    }

    // index 원소를 target으로 바꾼다(null이면 슬롯을 비운다). 할당하지 않는다. index >= Size()는
    // InvalidArgument다.
    template <class T>
    Result<void> SetReferenceAt(const ObjectAccessContext& access, ObjectHandle owner,
                                ObjectRefArray<T>& array, usize index,
                                ObjectHandle target) noexcept {
        return ReplaceArrayEdge(access, owner, array.storage_, index, target, NativeTypeKey<T>());
    }

    // index 원소를 지우고 뒤 원소를 당긴다(순서 유지). 범위 밖은 InvalidArgument다. 할당하지
    // 않는다. owner는 Alive 또는 DestroyRequested(정리 중)이고 Borrow를 쥐고 있어야 한다.
    template <class T>
    Result<void> RemoveReferenceAt(const ObjectAccessContext& access, ObjectHandle owner,
                                   ObjectRefArray<T>& array, usize index) noexcept {
        return RemoveArrayEdge(access, owner, array.storage_, index, ObjectHandle{}, false);
    }

    // target과 같은 첫 원소를 지운다(null이면 첫 null 원소). 없으면 NotFound다.
    template <class T>
    Result<void> RemoveReference(const ObjectAccessContext& access, ObjectHandle owner,
                                 ObjectRefArray<T>& array, ObjectHandle target) noexcept {
        return RemoveArrayEdge(access, owner, array.storage_, 0, target, true);
    }

    // 모든 원소를 지운다(용량 유지). 할당하지 않는다.
    template <class T>
    Result<void> ClearReferences(const ObjectAccessContext& access, ObjectHandle owner,
                                 ObjectRefArray<T>& array) noexcept {
        return ClearArrayEdges(access, owner, array.storage_);
    }

    // StructRefArray의 live 변경 API다(StructRefArray.h). owner·Borrow·컨테이너 위치·encode 고정
    // 규칙은 ObjectRefArray와 같다. 원소 블록 할당·기본 생성(S(Allocator&), 참조는 null이라 장벽이
    // 필요 없다)·새 색인 할당은 Registry 잠금 밖에서 하고, 다시 잠근 뒤 owner를 재검증하며 그때의
    // size·index로 다시 판단한다(그 사이 생성자·allocator 콜백이 같은 배열을 바꿔 새 색인이
    // 모자라거나 index가 size를 넘으면 원소를 소멸·반환하고 Busy). 실패하면 원소를 소멸·반환하고
    // 배열은 바뀌지 않는다. 원소 주소는 고정이다(색인만 옮긴다).

    // 기본 원소 하나를 끝에 추가하고 그 index를 돌려준다. size가 uint32 상한이면 Overflow다.
    //
    // 사용 예:
    //   auto index = registry.AddElement(access, mesh, meshPointer->materials);
    template <class S>
    Result<usize> AddElement(const ObjectAccessContext& access, ObjectHandle owner,
                             StructRefArray<S>& array) noexcept {
        return InsertStructElement(access, owner, array.storage_, 0, true);
    }

    // 기본 원소 하나를 index 위치에 넣는다(index == Size()면 끝). index > Size()는
    // InvalidArgument다.
    template <class S>
    Result<void> InsertElementAt(const ObjectAccessContext& access, ObjectHandle owner,
                                 StructRefArray<S>& array, usize index) noexcept {
        auto inserted = InsertStructElement(access, owner, array.storage_, index, false);
        return inserted ? Result<void>::Success() : Result<void>::Failure(inserted.Error());
    }

    // index 원소를 색인에서 떼어 잠금 밖에서 소멸·반환한다(뒤 원소의 주소는 그대로다). 범위 밖은
    // InvalidArgument다. owner는 Alive 또는 DestroyRequested(정리 중)이고 Borrow를 쥐고 있어야
    // 한다.
    template <class S>
    Result<void> RemoveElementAt(const ObjectAccessContext& access, ObjectHandle owner,
                                 StructRefArray<S>& array, usize index) noexcept {
        return RemoveStructElement(access, owner, array.storage_, index);
    }

    // 모든 원소를 떼어 잠금 밖에서 역순으로 소멸·반환한다(색인도 반환한다). owner 규칙은
    // RemoveElementAt과 같다.
    template <class S>
    Result<void> ClearElements(const ObjectAccessContext& access, ObjectHandle owner,
                               StructRefArray<S>& array) noexcept {
        return ClearStructElements(access, owner, array.storage_);
    }

    // index 원소 안의 참조 멤버 field를 target으로 바꾼다(SetReference의 원소판 — null 허용).
    // owner는 Alive이고 Borrow를 쥐고 있어야 하며(없으면 InvalidArgument), 컨테이너가 owner 저장소
    // 안이고 index < Size()이며 field가 그 원소 블록 안이어야 한다(아니면 InvalidArgument). 대상
    // 규칙·shade·encode 고정은 SetReference와 같다.
    //
    // 사용 예:
    //   registry.SetElementReference(access, mesh, meshPointer->materials, 2,
    //                                meshPointer->materials[2].material, material);
    template <class S, class T>
    Result<void> SetElementReference(const ObjectAccessContext& access, ObjectHandle owner,
                                     StructRefArray<S>& array, usize index, ObjectRef<T>& field,
                                     ObjectHandle target) noexcept {
        const detail::StructElementLocation element{&array.storage_, index};
        return SetEdge(access, owner, field.handle_, target, 0, NativeTypeKey<T>(), &element);
    }

    // encode thunk를 부르는 쪽이 owner의 필드를 encode하는 동안 쥐는 고정을 얻는다(EncodePin —
    // ObjectRefs.h). 고정된 owner의 컨테이너·참조 변경 API와 ResolveForWrite(등록된 필드 쓰기)는
    // Busy다. owner는 Alive여야 한다(아니면 CheckLive 규칙 — StaleHandle·Destroyed). 계수가 넘치면
    // Overflow, GC·callback 처리 중에는 Busy, 종료 시작 후에는 ShuttingDown이다. 고정은 수명을
    // 연장하지 않지만(루트가 아니다) 고정 중에는 저장소를 회수하지 않으며, 남은 고정은
    // ObjectStats::encodePins에 보이고 Shutdown을 Busy로 남긴다.
    //
    // 사용 예:
    //   auto pin = registry.PinForEncode(access, node);
    //   if (!pin) {
    //       return Result<void>::Failure(pin.Error());
    //   }
    Result<EncodePin> PinForEncode(const ObjectAccessContext& access, ObjectHandle owner) noexcept;

    // 논리적 접근을 즉시 닫는다. 기존 루트/간선은 GC에서 추적되어 보관된 자식도 보존한다.
    // 저장소 파괴는 도달성 및 모든 보유 계수 해소 뒤 Collect/DrainRetired에서 수행한다.
    Result<void> RequestDestroy(const ObjectAccessContext&, ObjectHandle) noexcept;
    // 조회는 수명을 연장하지 않으며 반환 후에도 상태가 바뀔 수 있다. 미공개 생성 슬롯,
    // 무효 handle·다른 registry·회수 후 generation은 StaleHandle이다. GC cutoff white는
    // Unreachable이다. 소유 스레드에서 콜백/GC 재진입은 Busy, 종료 시작 후에는 ShuttingDown이다.
    // 다른 스레드의 호출은 콜백 중에도 먼저 WrongThread로 거부한다.
    Result<ObjectLifecycle> InspectLifecycle(const ObjectAccessContext&,
                                             ObjectHandle) const noexcept;
    Result<BackendLease> AcquireBackendLease(const ObjectAccessContext&, ObjectHandle) noexcept;
    // 읽기용 불변 metadata/code lease를 복제한다. 객체 수명이나 접근권은 따로 보존해야 한다.
    // cutoff·논리 파괴 뒤에는 새 조회를 거부하며 호출자의 context/owner thread를 검사한다.
    Result<TypeLease> AcquireType(const ObjectAccessContext&, ObjectHandle) noexcept;
    // 참조 필드의 선언 타입을 이 저장소의 등록 타입에서 조회한다. 객체를 생성하거나 pin하지 않는다.
    // owner/context/종료/재진입 규약은 AcquireType과 같고 미등록 ID는 NotFound다.
    Result<TypeLease> AcquireRegisteredType(const ObjectAccessContext&, TypeId) noexcept;
    // 소유 스레드 full STW 수집. Borrow/생성 트랜잭션/재진입 중에는 Busy다.
    // 작업 큐 할당이나 Trace 간선 검증 실패 시 부분 표식으로 sweep하지 않는다.
    Result<GcReport> Collect(const ObjectAccessContext&) noexcept;
    // Begin/Step은 Borrow=0·활성 생성=0인 소유 스레드 safepoint에서만 진행한다.
    // slice 사이에는 mutator를 재개하며 root/간선/신규 게시가 삽입 장벽을 거친다.
    Result<void> BeginCollection(const ObjectAccessContext&) noexcept;
    // scheduler를 지정하면 parallelTraceSafe 타입만 worker에서 추적한다. 미선언 타입은 owner에서
    // 실행한다. allocator는 worker의 capture 정리를 지원해야 한다. 각 slice는 worker를 모두 drain한
    // 뒤 반환하므로 mutator와 field scan을 동시에 실행하지 않는다.
    Result<GcProgress> StepCollection(const ObjectAccessContext&, GcBudget = {},
                                      WorkerScheduler* scheduler = nullptr) noexcept;
    // 아직 cutoff를 게시하지 않은 Marking만 취소한다. cutoff 이후 중단으로 죽은 간선을 부활시키지
    // 않도록 Sweeping/Reclaiming 취소는 Busy다. 마지막 성공 보고서는 바꾸지 않는다.
    Result<void> CancelCollection(const ObjectAccessContext&) noexcept;
    GcPhase CollectionPhase() const noexcept;
    Result<uint32> DrainRetired(const ObjectAccessContext&) noexcept;
    // 신규 객체/루트/접근권을 닫은 후 회수를 시도한다. Busy면 보유권을 반환하고 재시도한다.
    // 접근 검증/재진입 거부는 닫기 전에 반환한다. 닫은 뒤의 회수 실패는 닫힌 상태를 유지한다.
    // 보유권이 남은 동안 Registry를 먼저 파괴해서는 안 된다.
    Result<void> Shutdown(const ObjectAccessContext&) noexcept;
    ObjectStats Stats() const noexcept;
    // 잠금 안에서 호출자 버퍼에 스냅샷을 복사한다. 할당하지 않으며 용량 부족 시 부분 기록하지
    // 않는다.
    Result<uint32> CaptureDiagnostics(Span<ObjectDiagnosticRecord>) const noexcept;
    // 최초 성공 전에는 0 값이다. 실패한 GC는 마지막 성공 결과를 덮어쓰지 않는다.
    GcReport LastGcReport() const noexcept;

private:
    friend class ConstructionContext;
    friend class Object;
    friend Result<void> detail::RetainRoot(ObjectRegistry*, ObjectHandle) noexcept;
    friend void detail::ReleaseRoot(ObjectRegistry*, ObjectHandle) noexcept;
    friend Result<void*> detail::AccessBorrow(ObjectRegistry*, ObjectHandle, const void*) noexcept;
    friend Result<ObjectLifecycle> detail::InspectBorrow(ObjectRegistry*, ObjectHandle) noexcept;
    friend void detail::ReleaseBorrow(ObjectRegistry*, ObjectHandle) noexcept;
    friend void detail::ReleaseBackend(ObjectRegistry*, ObjectHandle) noexcept;
    friend void detail::ReleaseEncodePin(ObjectRegistry*, ObjectHandle) noexcept;

    struct Slot;
    struct Chunk;
    struct MarkState;
    static void MarkEdge(void*, ObjectHandle) noexcept;
    static void ValidateStagedEdge(void*, ObjectHandle) noexcept;
    Slot* FindSlot(ObjectHandle) const noexcept;
    Slot* SlotAt(uint32) const noexcept;
    Result<void> CheckAccess(const ObjectAccessContext&) const noexcept;
    Result<void> CheckLive(Slot*, const void*) const noexcept;
    Result<ObjectAccessContext> ScopedAccess() const noexcept;
    Result<ObjectLifecycle> InspectChecked(const ObjectAccessContext&, ObjectHandle, const Object*,
                                           bool requireBorrow) const noexcept;
    Result<void> RequestDestroyChecked(const ObjectAccessContext&, ObjectHandle, const Object*,
                                       bool requireBorrow) noexcept;
    Result<ObjectLifecycle> InspectSelf(ObjectHandle, const Object*) const noexcept;
    Result<void> DestroySelf(ObjectHandle, const Object*) noexcept;
    Result<TypeLease> AcquireTypeSelf(ObjectHandle, const Object*) noexcept;
    Result<void> AddRoot(const ObjectAccessContext&, ObjectHandle, const void*) noexcept;
    // rejectEncodePinned면 encode 고정 중인 객체를 Busy로 거부한다(ResolveForWrite).
    Result<void> AddBorrow(const ObjectAccessContext&, ObjectHandle, const void*,
                           bool rejectEncodePinned = false) noexcept;
    // 참조 하나의 쓰기 단일 지점이다. transaction이 0이면 live(owner Alive), 아니면 그 생성
    // 트랜잭션의 staged 쓰기(owner Reserved)다. element가 없으면 field는 owner 저장소 안이어야
    // 하고, 있으면 그 컨테이너가 owner 저장소 안·index < size·field가 그 원소 블록 안이어야
    // 한다(원소 모드는 CheckContainerOwnerLocked — live는 Borrow 필수).
    Result<void> SetEdge(const ObjectAccessContext&, ObjectHandle owner, ObjectHandle& field,
                         ObjectHandle target, uint64 transaction, const void* nativeType,
                         const detail::StructElementLocation* element = nullptr) noexcept;
    // append면 index를 무시하고 그때(재잠금 시점 포함)의 끝에 넣는다.
    Result<void> InsertArrayEdge(const ObjectAccessContext&, ObjectHandle,
                                 detail::ObjectRefArrayStorage&, usize index, bool append,
                                 ObjectHandle, const void*) noexcept;
    Result<void> ReplaceArrayEdge(const ObjectAccessContext&, ObjectHandle,
                                  detail::ObjectRefArrayStorage&, usize index, ObjectHandle,
                                  const void*) noexcept;
    Result<void> RemoveArrayEdge(const ObjectAccessContext&, ObjectHandle,
                                 detail::ObjectRefArrayStorage&, usize, ObjectHandle,
                                 bool byHandle) noexcept;
    Result<void> ClearArrayEdges(const ObjectAccessContext&, ObjectHandle,
                                 detail::ObjectRefArrayStorage&) noexcept;
    Result<void> AssignArrayEdges(const ObjectAccessContext&, ObjectHandle,
                                  detail::ObjectRefArrayStorage&, Span<const ObjectHandle>,
                                  uint64 transaction, const void*) noexcept;
    Result<usize> InsertStructElement(const ObjectAccessContext&, ObjectHandle,
                                      detail::StructRefArrayStorage&, usize index,
                                      bool append) noexcept;
    Result<void> RemoveStructElement(const ObjectAccessContext&, ObjectHandle,
                                     detail::StructRefArrayStorage&, usize index) noexcept;
    Result<void> ClearStructElements(const ObjectAccessContext&, ObjectHandle,
                                     detail::StructRefArrayStorage&) noexcept;
    Result<void> AssignStructElements(const ObjectAccessContext&, ObjectHandle,
                                      detail::StructRefArrayStorage&, usize count,
                                      uint64 transaction) noexcept;
    // owner·컨테이너 위치 검증(잠금 보유 상태에서 호출 — processing_·closed_는 호출자가 먼저 본다).
    // 순서는 상태 → (live면 Borrow) → 위치 → (live면 encode 고정)이다. live: owner Alive
    // (allowDestroyRequested면 DestroyRequested도)·cutoff 아님(StaleHandle), Borrow 있음
    // (InvalidArgument). staged: owner가 그 트랜잭션의 Reserved(StaleHandle). 둘 다
    // [storage, storage + storageBytes)가 owner 저장소 안(InvalidArgument). 마지막으로 live owner가
    // encode 고정 중이면 Busy다.
    Result<void> CheckContainerOwnerLocked(ObjectHandle owner, const void* storage,
                                           usize storageBytes, uint64 transaction,
                                           bool allowDestroyRequested) noexcept;
    // non-null 대상 검증(잠금 보유): 생존(transaction이 있으면 그 트랜잭션의 Reserved도)·cutoff
    // 아님 (StaleHandle), nativeType의 파생(TypeMismatch).
    Result<void> CheckTargetLocked(ObjectHandle target, uint64 transaction,
                                   const void* nativeType) const noexcept;

    // ConstructionContext의 staged 컨테이너 API가 거치는 지점이다(컨테이너의 friend는 이 클래스
    // 하나다).
    template <class T>
    Result<void> AssignReferencesStaged(const ObjectAccessContext& access, ObjectHandle owner,
                                        ObjectRefArray<T>& array, Span<const ObjectHandle> targets,
                                        uint64 transaction) noexcept {
        return AssignArrayEdges(access, owner, array.storage_, targets, transaction,
                                NativeTypeKey<T>());
    }

    template <class S>
    Result<void> AssignElementsStaged(const ObjectAccessContext& access, ObjectHandle owner,
                                      StructRefArray<S>& array, usize count,
                                      uint64 transaction) noexcept {
        return AssignStructElements(access, owner, array.storage_, count, transaction);
    }

    template <class S, class T>
    Result<void> SetElementReferenceStaged(const ObjectAccessContext& access, ObjectHandle owner,
                                           StructRefArray<S>& array, usize index,
                                           ObjectRef<T>& field, ObjectHandle target,
                                           uint64 transaction) noexcept {
        const detail::StructElementLocation element{&array.storage_, index};
        return SetEdge(access, owner, field.handle_, target, transaction, NativeTypeKey<T>(),
                       &element);
    }

    Result<RootRef<Object>> NewRoot(const ObjectAccessContext&, TypeId, const void*) noexcept;
    Result<uint64> BeginConstruction(const ObjectAccessContext&) noexcept;
    Result<ObjectHandle> CreatePending(const ObjectAccessContext&, TypeId, uint64,
                                       const void*) noexcept;
    Result<void*> GetPending(const ObjectAccessContext&, ObjectHandle, uint64,
                             const void*) noexcept;
    Result<RootRef<Object>> Commit(const ObjectAccessContext&, uint64,
                                   Span<const ObjectHandle>) noexcept;
    void Rollback(uint64, Span<const ObjectHandle>) noexcept;
    ErrorCode ShadeLocked(ObjectHandle) noexcept;
    bool IsCutoff(const Slot*) const noexcept;
    bool HasReclaimableLocked() const noexcept;
    uint32 ReclaimEligible(uint32 maximum = 0xffffffffu, uint64 deadline = 0) noexcept;

    Allocator& allocator_;
    TypeRegistry& types_;
    ContextId context_;
    RegistryOptions options_;
    uint64 domain_ = 0;
    uint64 ownerThread_ = 0;
    mutable platform::Mutex gate_;
    Chunk* chunks_ = nullptr;
    uint32 chunkCount_ = 0;
    uint32 totalBorrows_ = 0;
    uint32 constructions_ = 0;
    uint64 nextTransaction_ = 1;
    uint64 markEpoch_ = 0;
    GcReport lastGc_;
    Array<ObjectHandle> gcWork_;
    usize gcCursor_ = 0;
    GcPhase gcPhase_ = GcPhase::Idle;
    GcReport cycleGc_;
    Chunk* sweepChunk_ = nullptr;
    uint32 sweepOffset_ = 0;
    // 잠금을 풀고 사용자 코드를 호출하는 구간에도 변경 재진입을 차단하는 상태다.
    // worker trace 실행 구간도 포함한다. phase 자체와 달리 slice 사이에는 false다.
    bool processing_ = false;
    bool closed_ = false;
};

template <class T>
Result<RootRef<T>> WeakRef<T>::TryPin(ObjectRegistry& registry,
                                      const ObjectAccessContext& access) const noexcept {
    return registry.TryPin<T>(access, handle_);
}

} // namespace rocks
