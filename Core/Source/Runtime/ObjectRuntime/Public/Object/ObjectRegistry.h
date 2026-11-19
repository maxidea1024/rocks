#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefMap.h>
#include <Object/ObjectRefs.h>
#include <Object/ObjectDiagnostics.h>
#include <Object/ObjectIdentity.h>
#include <Object/ObjectRegistryOptions.h>
#include <Object/ReferenceObserver.h>
#include <Object/StructRefArray.h>
#include <Object/TypeRegistry.h>

namespace rocks {

class ObjectBatchPublish;
class PreparedReferenceArray;
class PreparedStructElements;
struct BatchTargetCheck;
struct BatchEdgeChange;

namespace detail {
class PathCursor;
// 시험 전용 내부 계수 통로다(정의는 ObjectRuntime/Private/ObjectRegistryAccess.h).
struct ObjectRegistryAccess;

// 경로 접근의 참조 멤버 쓰기 단일 지점이다(정의는 Object/GeneratedPathAccess.h).
template <class T>
Result<void> SetPathMemberReference(PathCursor& cursor, ObjectRef<T>& field,
                                    ObjectHandle target) noexcept;

} // namespace detail

// 비이동 객체 저장소와 GC를 소유한다. Allocator/TypeRegistry가 먼저 생성되고 나중에 파괴되어야
// 한다. 객체 접근·변경·GC는 생성 스레드 전용이다. gate는 통계와 참조 계수를 보호할 뿐, 사용자 객체
// 필드를 스레드 안전하게 만들지 않는다. 별도 래퍼의 반환/복제 예외는 ObjectRefs 참고. 종료 시 모든
// 루트·Borrow·BackendLease·EncodePin을 반환한다. 남은 소유권을 무시한 파괴는 즉시 종료한다.
//
// 객체는 생성 때 정한 불변 Outer와 이름을 가진다(Docs/Design/ObjectFoundations.ko.md 1절). 같은
// Outer 안에서 이름은 대소문자·정규화 형태를 무시하고 유일하며(TryMakeNameComponentKey 키),
// Registry가 이름 색인과 Outer별 자식 고리를 소유한다. inner → outer는 언제나 GC가 추적하는 강한
// 간선이고, 정책 없는 Registry에서 outer는 inner를 살리지 않는다. 패키지 정책 Registry에서는 outer
// → inner도 강한 간선이라(자식 고리가 export 표다) 소속 객체는 Discard로만 죽는다.
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

    // 생성 때 받은 객체 정책이다. 생성 뒤 바뀌지 않으므로 어느 스레드에서나 읽을 수 있다.
    const RegistryPolicy& Policy() const noexcept {
        return options_.policy;
    }

    // RegistryOptions::names(없으면 nullptr)다. 생성 뒤 바뀌지 않아 어느 스레드에서나 읽는다(pool의
    // 스레드 규칙은 NamePool). Name 쓰기 검사·저장 encode·복원 2a intern이 쓴다.
    // 이 Registry가 쓰는 타입 Registry다(생성자 인자 — 텍스트 codec 같은 도구 계층이 대상
    // Registry에서 얻는다).
    TypeRegistry& Types() const noexcept {
        return types_;
    }

    NamePool* Names() const noexcept {
        return options_.names;
    }

    // 이 Registry가 객체 생성자에 주입하는 allocator(생성 때 받은 Allocator)다(리플렉션 B9e —
    // Docs/Design/ReflectionExtensions.ko.md 1.7절). 값이 없던 필드 자리에 새 값을 만들 때 —
    // Optional<String>·Optional<값 struct>를 unset에서 set으로 바꾸는 쓰기·경로 연산·복원 2단계,
    // 뒤 단계의 InstancedStruct·Text — 객체의 다른 값과 같은 allocator로 만들어 그 값의 수명을
    // 객체에 맞춘다(한 Registry의 객체 값이 다른 수명의 allocator 노드를 쥐지 않게).
    //
    // 수명: 반환 참조는 이 Registry 생성 때 받은 allocator 자체이며 Registry 계약상 Registry보다
    // 오래 산다. 그 allocator로 만든 값은 객체 안에 두는 동안 객체(Registry) 수명을 따른다. 스레드:
    // 접근자는 상태를 읽지 않으므로 어느 스레드에서나 부를 수 있다. 그 allocator로 객체 필드의 값을
    // 만들고 바꾸는 일은 다른 Registry API와 같이 소유 스레드에서 한다(allocator 자체의 스레드
    // 규칙은 그 Allocator 구현이다). 오류는 없다.
    //
    // 사용 예(Optional<String> 멤버를 unset에서 set으로 — 생성 코드 detail::WriteOptionalValue):
    //   String replacement(registry.ObjectAllocator());
    //   if (auto copied = replacement.TryAssign(text); !copied) {
    //       return copied;
    //   }
    Allocator& ObjectAllocator() const noexcept {
        return allocator_;
    }

    // 호출 스레드와 access가 이 Registry의 소유 스레드·문맥인지 본다. 다른 API의 1단계 접근 검사와
    // 같다(소유 스레드가 아니면 WrongThread, 문맥이 다르면 WrongContext). Registry 상태를 읽거나
    // 바꾸지 않으므로 종료·재진입과 무관하다. Registry를 부르지 않을 수도 있는 소유 스레드 전용
    // 단계(단계형 그래프 복원 — ObjectGraphStage)가 계약을 먼저 확인할 때 쓴다.
    //
    // 사용 예:
    //   if (auto owned = registry.VerifyAccess(access); !owned) {
    //       return owned;
    //   }
    Result<void> VerifyAccess(const ObjectAccessContext& access) const noexcept {
        return CheckAccess(access);
    }

    // 기본 생성과 공개를 한 트랜잭션으로 실행한다. 실패 가능한 별도 초기화에는 ConstructionContext
    // 사용. Outer·이름 없는 객체다(패키지 정책 Registry에서는 패키지 타입만 된다).
    template <class T>
    Result<RootRef<T>> NewObject(const ObjectAccessContext& access, TypeId type) noexcept {
        return NewObject<T>(access, type, ObjectHandle{}, StringView{});
    }

    // outer 아래 name으로 만들고 공개한다(ConstructionContext::Create(type, outer, name) +
    // Commit()과 같다). outer는 생존(Alive) 객체이거나 null이고, 오류 순서·규칙은
    // ConstructionContext::Create와 같다.
    //
    // 사용 예:
    //   auto socket = registry.NewObject<Socket>(access, SocketType, body, "Socket");
    //   if (!socket) {
    //       return Result<void>::Failure(socket.Error());   // AlreadyExists면 이름이 이미 있다
    //   }
    template <class T>
    Result<RootRef<T>> NewObject(const ObjectAccessContext& access, TypeId type, ObjectHandle outer,
                                 StringView name) noexcept {
        auto root = NewRoot(access, type, NativeTypeKey<T>(), outer, name);
        if (!root) {
            return Result<RootRef<T>>::Failure(root.Error());
        }

        RootRef<T> typed(this, root.Value().handle_);
        root.Value().registry_ = nullptr;
        root.Value().handle_ = {};
        return Result<RootRef<T>>::Success(Move(typed));
    }

    // ---- Outer·이름 조회(Docs/Design/ObjectFoundations.ko.md 1.2절) ----
    // 공통 규칙: 소유 스레드 전용(WrongThread), 접근 문맥이 다르면 WrongContext, 종료 시작 뒤
    // ShuttingDown, GC·callback 처리 중(재진입) Busy. 조회는 수명을 연장하지 않으며 결과는 반환 뒤
    // 바뀔 수 있다. handle 규칙(따로 적지 않으면): 무효·다른 Registry·회수된 generation·공개 전
    // (Reserved)은 StaleHandle, GC cutoff white·Retired는 Destroyed다. Alive와
    // DestroyRequested(논리 정체성은 회수 전까지 남는다)를 받는다.

    // handle의 Outer다. Outer가 없으면 ObjectHandle{}이다.
    //
    // 사용 예:
    //   auto outer = registry.GetOuter(access, socket);
    //   if (outer && outer.Value().IsNull()) { /* outermost */ }
    Result<ObjectHandle> GetOuter(const ObjectAccessContext&, ObjectHandle) const noexcept;

    // handle의 Outer 사슬 끝(소속 패키지)이다. Outer가 없으면 handle 자신이다.
    Result<ObjectHandle> GetOutermost(const ObjectAccessContext&, ObjectHandle) const noexcept;

    // handle의 이름을 out 뒤에 붙인다(Outer 없는 이름 없는 객체와 대표 자식은 빈 문자열). 잠금
    // 안에서 64 byte 스택 버퍼로 복사한 뒤 잠금 밖에서 out의 allocator로 붙이므로, 그 할당 콜백이
    // Registry에 재진입해 그 객체를 파괴·회수해도 해제된 이름을 읽지 않는다. out의 할당 실패는 그
    // 오류이고 out은 그대로다.
    //
    // 사용 예:
    //   String name(allocator);
    //   if (auto got = registry.GetName(access, socket, name); !got) { return got; }
    Result<void> GetName(const ObjectAccessContext&, ObjectHandle, String& out) const noexcept;

    // handle의 객체 경로(outermost 아래 이름을 '.'로 이은 것, 대표 자식의 빈 이름은 건너뛴다 — 예:
    // 대표 에셋 아래 Body 아래 Socket = "Body.Socket")를 out 뒤에 붙인다. outermost와 대표 자식은
    // 빈 문자열이다. GetName과 같은 방식으로 4 KiB 스택 버퍼에 만든 뒤 붙인다(경로 상한이 4 KiB라
    // 넘치지 않는다).
    Result<void> GetObjectPath(const ObjectAccessContext&, ObjectHandle,
                               String& out) const noexcept;

    // 이름 색인 조회다. outer 아래 이름이 name인 공개된 객체(Alive, RequestDestroy만 받은
    // DestroyRequested)를 돌려준다 — 공개 전(Reserved)·cutoff white는 NotFound다. outer가 null이면
    // Outer 없는 이름 있는 객체의 최상위 이름 공간이다. outer가 있고 name이 비면 대표 자식을
    // 찾는다. 비교는 대소문자·정규화 형태를 무시한다(TryMakeNameComponentKey).
    //
    // 순서와 오류: 접근 → 종료·재진입 → outer(handle 규칙) → 이름 형식(ValidateNameComponent —
    // InvalidArgument, outer 없는 빈 이름도 InvalidArgument, 애매한 결합 문자의 NFC 판정 할당
    // 실패는 OutOfMemory) → 비교 키 생성 할당(비ASCII 이름만 — OutOfMemory) → 없으면 NotFound.
    //
    // 사용 예:
    //   auto body = registry.FindChild(access, asset, "Body");
    //   if (!body) { /* NotFound면 없다 */ }
    Result<ObjectHandle> FindChild(const ObjectAccessContext&, ObjectHandle outer,
                                   StringView name) const noexcept;

    // outermost 아래 객체 경로를 푼다. outermost에 공개된 대표 자식이 있으면 경로는 그 아래에서
    // 시작하고, 빈 경로는 대표 자식이다(대표 자식이 없으면 NotFound — outermost 자신을 돌려주지
    // 않는다). 구성 요소마다 FindChild와 같은 규칙으로 찾는다.
    //
    // 순서와 오류: 접근 → 종료·재진입 → outermost(handle 규칙, Outer가 있으면 InvalidArgument) →
    // 경로 형식(4 KiB 초과·빈 구성 요소·이름 형식 위반 InvalidArgument, NFC 판정 할당 실패
    // OutOfMemory — 조회 전에 모든 구성 요소를 검사한다) → 비교 키 할당(OutOfMemory) → 없는 구성
    // 요소 NotFound.
    //
    // 사용 예(A1 Soft 참조 해석):
    //   auto target = registry.FindObject(access, package, softRef.objectPath.View());
    //   if (!target) { /* NotFound면 MissingDependency 진단 */ }
    Result<ObjectHandle> FindObject(const ObjectAccessContext&, ObjectHandle outermost,
                                    StringView objectPath) const noexcept;

    // outer 아래 name이 이미 점유되었는지(그 이름의 생성이 AlreadyExists가 될지)다. 어느 트랜잭션의
    // 공개 전(Reserved) 객체든 점유로 보고, outer도 어느 트랜잭션의 공개 전 객체여도 된다. 빈
    // name은 대표 자식 자리다(outer가 null이면 InvalidArgument). 오류 순서는 FindChild와 같다. 대표
    // 자식· 패키지 같은 구조 규칙은 보지 않는다(DescribeOuter로 확인한다). 도구는 생성 실패가
    // 트랜잭션을 오류 상태로 고정하므로 이것으로 먼저 확인한다.
    Result<bool> IsNameTaken(const ObjectAccessContext&, ObjectHandle outer,
                             StringView name) const noexcept;

    // outer를 Outer로 쓸 때의 요약이다. outer는 생존(Alive, cutoff 아님) 객체여야 한다(null은
    // InvalidArgument, 그 밖은 생성 규칙 5와 같이 StaleHandle — DestroyRequested 포함). 공개 전
    // 객체는 ConstructionContext::DescribeOuter로 본다.
    Result<OuterSummary> DescribeOuter(const ObjectAccessContext&,
                                       ObjectHandle outer) const noexcept;

    // outer의 Inner를 이름 순(같은 Outer 안 NFC UTF-8 byte 순 — 빈 이름이 먼저)으로 out 끝에
    // 붙인다. Alive 객체만 모은다(공개 전·DestroyRequested·cutoff white는 그 부분 트리째 뺀다 —
    // DepthFirst도 Outer가 언제나 Inner보다 앞이다). outer는 handle 규칙을 따른다. out은 자기
    // allocator로 늘린다.
    //
    // 처리: 잠금 안에서 개수를 센 뒤 처리 표시(그동안 Registry 변경 재진입은 Busy)를 세우고 잠금
    // 밖에서 out 용량과 보조 배열(Registry allocator)을 할당한다. 다시 잠가 수준 순서로 모은 뒤
    // 잠금 밖에서 같은 Outer 구간마다 정렬하고 순서를 만든다. 재귀·스택 배열이 없어 깊은 트리(2048
    // 수준)에서도 스택 사용이 일정하다.
    //
    // 오류: 할당 실패는 그 오류(OutOfMemory 등)이고, 실패하면 out은 호출 전 길이로 돌아간다.
    //
    // 사용 예(A1 writer의 export 순서):
    //   Array<ObjectHandle> exports(allocator);
    //   auto collected =
    //       registry.CollectInners(access, package, InnerOrder::DepthFirst, exports);
    //   if (!collected) {
    //       return collected;
    //   }
    Result<void> CollectInners(const ObjectAccessContext&, ObjectHandle outer, InnerOrder,
                               Array<ObjectHandle>& out) noexcept;

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
    // 고정(PinForEncode) 중이면 Busy다. Registry API로 쓰는 모든 참조(이 field와 컨테이너 원소)는
    // owner 타입의 Trace가 방문해야 한다 — GC와 관찰자의 Commit·Discard 계수가 Trace로 센다.
    //
    // 관찰자 Registry(RegistryOptions::referenceObserver)에서는 옛 대상·새 대상의 패키지 사이 간선
    // 변화를 기록한다(옛 대상 키 먼저, 두 패키지가 같으면 기록·예약하지 않는다). 새 키 공간이
    // 모자라면 잠금을 풀고 관찰자 allocator로 예약한 뒤 다시 잠가 검증부터 다시 한다 — 할당
    // 실패는 OutOfMemory, 키 상한 초과는 CapacityExceeded, 그 사이 재진입이 공간을 다시 써 버리면
    // Busy이고 모두 field 불변이다. 관찰자가 없으면 할당하지 않는다.
    template <class T>
    Result<void> SetReference(const ObjectAccessContext& access, ObjectHandle owner,
                              ObjectRef<T>& field, ObjectHandle target) noexcept;

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
    // - 관찰자 Registry(RegistryOptions::referenceObserver)에서는 패키지 사이 간선 변화를 기록한다
    //   (ReferenceObserver.h — Alive이고 Outer가 있는 owner만, DestroyRequested owner의 제거와
    //   비우기는 기록하지 않는다). 단일 간선 변경(AddReference·InsertReferenceAt·SetReferenceAt과
    //   RemoveReferenceAt·RemoveReference)은 새 키 공간이 모자라면 버퍼 성장과 같은 잠금 밖
    //   구간에서 관찰자 allocator로 예약하고 다시 잠가 재검증한다(그 사이 공간이 다시 모자라거나
    //   위치가 바뀌면 Busy). ClearReferences는 떼어 낼 원소의 패키지 쌍을 처리 표시(processing_)
    //   아래 잠금 밖에서 Registry allocator로 합친 뒤 예약하고 한 잠금 안에서 비우고 기록한다
    //   (그동안 Registry 변경 재진입은 Busy, 다른 스레드의 RootRef 복제도 Busy). 예약·집계 할당
    //   실패는 OutOfMemory, 키 상한 초과는 CapacityExceeded다. 아래 "할당하지 않는다"는 관찰자
    //   없는 Registry의 규칙이다.
    // - 실패하면 배열 내용은 바뀌지 않는다. GC·callback 처리 중에는 Busy, 종료 시작 후에는
    //   ShuttingDown이다. MaxElements(리플렉션 필드 상한)는 여기서 걸지 않는다(저장할 때 검사한다).

    // target을 끝에 추가한다.
    //
    // 사용 예:
    //   registry.AddReference(access, actor, actorPointer->components_, component);
    template <class T>
    Result<void> AddReference(const ObjectAccessContext& access, ObjectHandle owner,
                              ObjectRefArray<T>& array, ObjectHandle target) noexcept;

    // target을 index 위치에 넣고 뒤 원소를 민다(index == Size()면 끝). index > Size()는
    // InvalidArgument다. 도구 편집용이다.
    template <class T>
    Result<void> InsertReferenceAt(const ObjectAccessContext& access, ObjectHandle owner,
                                   ObjectRefArray<T>& array, usize index,
                                   ObjectHandle target) noexcept;

    // index 원소를 target으로 바꾼다(null이면 슬롯을 비운다). 할당하지 않는다(관찰자 Registry는
    // 위 공통 규칙의 예약). index >= Size()는 InvalidArgument다.
    template <class T>
    Result<void> SetReferenceAt(const ObjectAccessContext& access, ObjectHandle owner,
                                ObjectRefArray<T>& array, usize index,
                                ObjectHandle target) noexcept;

    // index 원소를 지우고 뒤 원소를 당긴다(순서 유지). 범위 밖은 InvalidArgument다. 할당하지
    // 않는다(관찰자 Registry는 위 공통 규칙의 예약). owner는 Alive 또는 DestroyRequested(정리
    // 중)이고 Borrow를 쥐고 있어야 한다.
    template <class T>
    Result<void> RemoveReferenceAt(const ObjectAccessContext& access, ObjectHandle owner,
                                   ObjectRefArray<T>& array, usize index) noexcept;

    // index 원소를 지우고 끝 원소를 그 자리로 옮긴다(순서를 지키지 않는다 — O(1)). 지운 원소의
    // 규칙(장부·관찰자 기록·오류)은 RemoveReferenceAt과 같다. 옮기는 원소는 같은 배열에 이미 있는
    // 간선이라 대상의 생존·cutoff·세대를 다시 묻지 않는다(대상이 DestroyRequested여도 성공한다 —
    // SetReferenceAt으로 옮기면 대상 검증이 실패한다). 순서가 의미 없는 큰 목록의 제거에 쓴다
    // (Docs/Design/EngineRuntime.ko.md 4.5절).
    //
    // 사용 예(World가 Level의 Actor 배열에서 위치를 기억해 뗀다):
    //   registry.RemoveReferenceAtSwap(access, level, levelPointer->actors_, position);
    template <class T>
    Result<void> RemoveReferenceAtSwap(const ObjectAccessContext& access, ObjectHandle owner,
                                       ObjectRefArray<T>& array, usize index) noexcept;

    // target과 같은 첫 원소를 지운다(null이면 첫 null 원소). 없으면 NotFound다. 관찰자 Registry의
    // 예약 뒤 재검증에서 그 원소가 사라졌으면 Busy다.
    template <class T>
    Result<void> RemoveReference(const ObjectAccessContext& access, ObjectHandle owner,
                                 ObjectRefArray<T>& array, ObjectHandle target) noexcept;

    // 모든 원소를 지운다(용량 유지). 할당하지 않는다(관찰자 Registry는 위 공통 규칙의 집계·예약).
    template <class T>
    Result<void> ClearReferences(const ObjectAccessContext& access, ObjectHandle owner,
                                 ObjectRefArray<T>& array) noexcept;

    // ObjectRefMap의 live 변경 API다(ObjectRefMap.h — 리플렉션 B9f4, Docs/Design/
    // ReflectionExtensions.ko.md 7.4절). owner·Borrow·컨테이너 위치·encode 고정·대상 검증·null
    // 정규화·삽입 장벽·관찰자 기록 규칙은 위 ObjectRefArray 공통 규칙과 같다. 다른 점:
    // - key는 K이거나 String 키의 StringView, Soft 키의 SoftObjectRefView다(조회는 할당하지
    // 않는다).
    // - 컨테이너를 순회(View().ForEach 등) 중이면 Busy다(Core OrderedMap의 Fatal에 닿기 전에 거절).
    // - 새 키는 Registry 잠금 밖에서 컨테이너 allocator로 키 사본과 한 항목 더 담을 저장소를
    // 준비하고
    //   (할당만 — 컨테이너는 그대로, 관찰자 예약도 같은 구간), 다시 잠가 owner·대상·관찰자 공간을
    //   재검증한 뒤 같은 잠금 안에서 무할당·무실패로 적용·게시·기록한다. 그 사이 allocator 콜백이
    //   같은 컨테이너의 구조를 바꿨으면(넣기·지우기·비우기) Busy, owner가 파괴 요청·회수 대상이
    //   됐으면 StaleHandle이고 준비한 것만 반환한다(컨테이너의 내용·용량·항목 주소 불변).
    // - 옛 저장소·지운 키·떼어 낸 옛 항목은 잠금을 푼 뒤 반환한다(그 해제 콜백은 게시를 마친
    //   컨테이너를 보고, 재진입한 변경도 그 상태에서 다시 판단한다). 항목 포인터는 잠금을 넘겨
    //   쥐지 않는다(다시 잠그면 키로 다시 찾는다).
    // - 실패하면 컨테이너는 바뀌지 않는다. MaxElements는 여기서 걸지 않는다(저장할 때 검사한다).

    // key의 값을 target으로 한다: 없으면 끝에 넣고, 있으면 자리를 지키며 바꾼다(null이면 값을
    // 비운다 — 항목은 남는다). 있는 키의 교체는 할당하지 않는다(관찰자 Registry는 위 공통 규칙의
    // 예약). 항목 수가 OrderedMap 상한(2^31)이면 Overflow, 키 사본·저장소 할당 실패는 그 오류다.
    // write(선택 — MapReferenceWrite)를 주면 새 키의 항목 수 상한(넘으면 CapacityExceeded — 첫
    // 판정과 게시 직전 재검증)을 걸고, 성공하면 게시 잠금 안에서 실제로 덮어쓴 옛 값과 키가 이미
    // 있었는지를 돌려준다(실패하면 write의 출력은 그대로다).
    //
    // 사용 예:
    //   registry.SetMapReference(access, inventory, pointer->slots_, StringView("sword"), item);
    template <class K, class T, class Q>
    Result<void> SetMapReference(const ObjectAccessContext& access, ObjectHandle owner,
                                 ObjectRefMap<K, T>& map, const Q& key, ObjectHandle target,
                                 MapReferenceWrite* write = nullptr) noexcept;

    // key 항목을 지운다(남은 순서 유지). 없으면 NotFound(관찰자 예약 뒤 재검증에서 사라졌으면
    // Busy)다. owner는 Alive 또는 DestroyRequested(정리 중)다. 할당하지 않는다(관찰자 Registry는
    // 예약). 지운 키(String·Soft)는 잠금을 푼 뒤 반환한다. removed(선택)를 주면 성공했을 때 게시
    // 잠금 안에서 실제로 지운 값을 담는다(호출 전에 읽은 값은 관찰자 예약 콜백이 바꿀 수 있다).
    //
    // 사용 예:
    //   registry.EraseMapReference(access, inventory, pointer->slots_, StringView("sword"));
    template <class K, class T, class Q>
    Result<void> EraseMapReference(const ObjectAccessContext& access, ObjectHandle owner,
                                   ObjectRefMap<K, T>& map, const Q& key,
                                   ObjectHandle* removed = nullptr) noexcept;

    // 모든 항목과 저장소를 떼어 빈 컨테이너를 게시하고 옛 항목은 잠금을 푼 뒤 반환한다. owner
    // 규칙은 EraseMapReference와 같다. 할당하지 않는다(관찰자 Registry는 ClearReferences와 같은
    // 처리 표시 아래 집계·예약).
    //
    // 사용 예:
    //   registry.ClearMapReferences(access, inventory, pointer->slots_);
    template <class K, class T>
    Result<void> ClearMapReferences(const ObjectAccessContext& access, ObjectHandle owner,
                                    ObjectRefMap<K, T>& map) noexcept;

    // StructRefArray의 live 변경 API다(StructRefArray.h). owner·Borrow·컨테이너 위치·encode 고정
    // 규칙은 ObjectRefArray와 같다. 원소 블록 할당·기본 생성(S(Allocator&), 참조는 null이라 장벽이
    // 필요 없다)·새 색인 할당은 Registry 잠금 밖에서 하고, 다시 잠근 뒤 owner를 재검증하며 그때의
    // size·index로 다시 판단한다(그 사이 생성자·allocator 콜백이 같은 배열을 바꿔 새 색인이
    // 모자라거나 index가 size를 넘으면 원소를 소멸·반환하고 Busy). 실패하면 원소를 소멸·반환하고
    // 배열은 바뀌지 않는다. 원소 주소는 고정이다(색인만 옮긴다). 새 원소의 참조는 null이라 추가·
    // 삽입은 관찰자에 기록하지 않는다. 관찰자 Registry의 제거·비우기는 떼어 낼 원소만 등록 struct
    // trace(TypeRegistry::AcquireStructByNative — 원소 struct가 등록되어 있지 않으면 Unsupported)로
    // 훑어 패키지 쌍을 합친다(ClearReferences와 같은 처리 표시·예약 규칙, Alive이고 Outer가 있는
    // owner만).

    // 기본 원소 하나를 끝에 추가하고 그 index를 돌려준다. size가 uint32 상한이면 Overflow다.
    //
    // 사용 예:
    //   auto index = registry.AddElement(access, mesh, meshPointer->materials);
    template <class S>
    Result<usize> AddElement(const ObjectAccessContext& access, ObjectHandle owner,
                             StructRefArray<S>& array) noexcept;

    // 기본 원소 하나를 index 위치에 넣는다(index == Size()면 끝). index > Size()는
    // InvalidArgument다.
    template <class S>
    Result<void> InsertElementAt(const ObjectAccessContext& access, ObjectHandle owner,
                                 StructRefArray<S>& array, usize index) noexcept;

    // index 원소를 색인에서 떼어 잠금 밖에서 소멸·반환한다(뒤 원소의 주소는 그대로다). 범위 밖은
    // InvalidArgument다. owner는 Alive 또는 DestroyRequested(정리 중)이고 Borrow를 쥐고 있어야
    // 한다.
    template <class S>
    Result<void> RemoveElementAt(const ObjectAccessContext& access, ObjectHandle owner,
                                 StructRefArray<S>& array, usize index) noexcept;

    // 모든 원소를 떼어 잠금 밖에서 역순으로 소멸·반환한다(색인도 반환한다). owner 규칙은
    // RemoveElementAt과 같다.
    template <class S>
    Result<void> ClearElements(const ObjectAccessContext& access, ObjectHandle owner,
                               StructRefArray<S>& array) noexcept;

    // index 원소 안의 참조 멤버 field를 target으로 바꾼다(SetReference의 원소판 — null 허용).
    // owner는 Alive이고 Borrow를 쥐고 있어야 하며(없으면 InvalidArgument), 컨테이너가 owner 저장소
    // 안이고 index < Size()이며 field가 그 원소 블록 안이어야 한다(아니면 InvalidArgument). 대상
    // 규칙·shade·encode 고정·관찰자 기록과 예약은 SetReference와 같다.
    //
    // 사용 예:
    //   registry.SetElementReference(access, mesh, meshPointer->materials, 2,
    //                                meshPointer->materials[2].material, material);
    template <class S, class T>
    Result<void> SetElementReference(const ObjectAccessContext& access, ObjectHandle owner,
                                     StructRefArray<S>& array, usize index, ObjectRef<T>& field,
                                     ObjectHandle target) noexcept;

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

    // ---- 묶음 게시(리플렉션 B9i2 — Docs/Design/ReflectionExtensions.ko.md 10.2절) ----

    // owner의 변경 표지다. 그 객체를 바꿀 수 있는 지점마다 오른다: 쓰기용 Borrow(ResolveForWrite —
    // 리플렉션 필드 쓰기·경로 쓰기), live 참조 쓰기(SetReference·SetElementReference)와 live
    // 컨테이너 변경 시도(ObjectRefArray·ObjectRefMap·StructRefArray — owner 검사를 통과한 시도마다,
    // 뒤 단계가 실패해도 오를 수 있다), 게시한 묶음(ObjectBatchPublish::End). 같은 값이면 그 사이
    // owner가 Registry API로 바뀌지 않았다(값이 다르다고 꼭 바뀐 것은 아니다 — 보수적). 읽기 Borrow
    // (Resolve)·RootRef·조회는 올리지 않는다. 값 필드를 C++로 직접 쓰는 것은 잡지 못한다(호출자
    // 책임).
    //
    // 오류: 접근(WrongThread·WrongContext), 종료 뒤 ShuttingDown, 재진입·묶음 구간 중 Busy, owner가
    // 생존(Alive)이 아니면 CheckLive 규칙(StaleHandle·Destroyed).
    //
    // 사용 예(대입 묶음 Prepare가 읽어 두고 Publish가 BeginBatchPublish에 넘긴다):
    //   auto stamp = registry.MutationStamp(access, owner);
    //   if (!stamp) { return Result<void>::Failure(stamp.Error()); }
    Result<uint64> MutationStamp(const ObjectAccessContext& access,
                                 ObjectHandle owner) const noexcept;

    // owner의 묶음 게시 구간에 들어간다(ObjectBatchPublish.h). 그 뒤의 게시는 할당·실패가 없으므로
    // 실패할 수 있는 검사를 모두 여기서 한다. 순서와 오류(실패하면 아무것도 바뀌지 않는다):
    //   1. 접근(WrongThread·WrongContext) → 종료(ShuttingDown) → 재진입·다른 구간(Busy).
    //   2. owner: 생존(Alive, cutoff 아님)이 아니면 StaleHandle(Discard·파괴 요청 포함), 호출자가
    //      owner의 Borrow를 쥐지 않았으면 InvalidArgument, encode 고정 중이면 Busy, 변경 표지가
    //      expectedStamp와 다르면 Busy.
    //   3. edges의 removed(non-null)가 회수 전 객체(Alive·DestroyRequested)가 아니면
    //   InvalidArgument.
    //   4. targets(null이면 InvalidArgument)와 edges의 non-null added: 같은 Registry의 생존(Alive,
    //      cutoff 아님 — Discard·회수·다른 Registry는 StaleHandle)이고 기대 타입의
    //      파생(TypeMismatch).
    //   5. 처리 표시(processing_)를 세운다. 관찰자 Registry이고 owner가 기록 대상이면(Alive·Outer
    //      있음) edges의 패키지 쌍을 Registry allocator로 합친 뒤(잠금 밖 — OutOfMemory) 모자란 새
    //      키 공간을 관찰자에 예약하고(OutOfMemory·키 상한 CapacityExceeded·예약 콜백 재진입 Busy)
    //      다시 잠가 공간을 확인한다(모자라면 Busy).
    // 구간은 반환값이 닫는다(End·소멸). 열린 동안 이 Registry의 다른 변경·Borrow·RootRef 복제·
    // GC 단계·Discard는 Busy이고 콜백이 없다. owner Borrow는 구간을 닫은 뒤 놓는다.
    Result<ObjectBatchPublish> BeginBatchPublish(const ObjectAccessContext& access,
                                                 ObjectHandle owner, uint64 expectedStamp,
                                                 Span<const BatchTargetCheck> targets,
                                                 Span<const BatchEdgeChange> edges) noexcept;

    // 논리적 접근을 즉시 닫는다. 기존 루트/간선은 GC에서 추적되어 보관된 자식도 보존한다.
    // 저장소 파괴는 도달성 및 모든 보유 계수 해소 뒤 Collect/DrainRetired에서 수행한다. 이름은
    // 회수 전까지 남는다(이름을 바로 비우려면 Discard). 패키지 정책 Registry에서는 접근 검사 다음에
    // Unsupported다(소속 객체는 Discard로만 죽는다 — Object::Destroy도 같다).
    Result<void> RequestDestroy(const ObjectAccessContext&, ObjectHandle) noexcept;

    // 설계: Docs/Design/ObjectFoundations.ko.md 2절. handle과 Outer 사슬이 handle을 지나는 모든
    // 객체(부분 트리 — 자식 고리로 닿는 객체)를 논리 파괴(DestroyRequested)하고 이름 색인과 자식
    // 고리에서 즉시 뺀다. 이후 FindChild·FindObject·CollectInners·IsNameTaken은 이 객체들을 보지
    // 못하고 같은 이름의 새 객체를 바로 만들 수 있다. 패키지 정책이면 고리에서 빠졌으므로 패키지가
    // 더 붙잡지 않는다. 저장소는 남은 RootRef·Borrow·간선이 풀린 뒤 GC가 회수한다(Resolve와
    // WeakObjectRef::TryPin은 Destroyed다. GetName·GetObjectPath·GetOuter는 GC cutoff
    // white·Retired가 되기 전까지 되고 그 뒤는 Destroyed다). 남은 참조가 Discard된 객체를 붙잡으면
    // inner → outer 간선으로 그 Outer 사슬도 남지만 형제는 고리에서 빠져 남지 않는다. 관찰자가
    // 있으면 부분 트리에서 다른 패키지로 나가는 강한 간선 다중도를 Discard 원인의 음수 변화로 한 번
    // 기록한다(Outer가 있는 Alive 소유자만). 이미 Discard된 객체는 성공(무동작)이고,
    // RequestDestroy만 받은 객체도 받는다(이름을 이때 뗀다). 되돌릴 수 없다(handle은 다시 살지
    // 않는다). GC 단계와 무관하게 부를 수 있다.
    //
    // 순서와 오류: 접근(WrongThread·WrongContext) → 종료·재진입(ShuttingDown·Busy) → handle(무효·
    // 다른 Registry·공개 전 StaleHandle, 이미 Discard됨은 성공, GC cutoff white·Retired Destroyed)
    // → 부분 트리에 공개 전(Reserved) 객체가 있으면 Busy → 할당(부분 트리 목록·간선 집계는 Registry
    // allocator, 관찰자 예약은 관찰자 allocator — OutOfMemory, 키 상한 초과 CapacityExceeded) →
    // 적용. 할당은 처리 표시(processing_) 아래 잠금 밖이며 그동안 Registry 변경 재진입은 Busy다.
    // 실패하면 아무것도 바뀌지 않는다.
    //
    // 사용 예(AssetLoader::Pump의 Closing 처리 — AS 11.4):
    //   for (ObjectHandle package : unit.packages) {
    //       // Busy면 RootRef를 쥔 채 다음 Pump에서 다시 한다(Discard한 패키지는 무동작).
    //       if (auto discarded = registry.Discard(access, package); !discarded) {
    //           return discarded;
    //       }
    //   }
    //
    //   unit.roots.Clear(); // 모두 Discard한 뒤에만 RootRef를 놓는다
    Result<void> Discard(const ObjectAccessContext&, ObjectHandle) noexcept;
    // 조회는 수명을 연장하지 않으며 반환 후에도 상태가 바뀔 수 있다. 미공개 생성 슬롯,
    // 무효 handle·다른 registry·회수 후 generation은 StaleHandle이다. GC cutoff white는
    // Unreachable이다. 소유 스레드에서 콜백/GC 재진입은 Busy, 종료 시작 후에는 ShuttingDown이다.
    // 다른 스레드의 호출은 콜백 중에도 먼저 WrongThread로 거부한다.
    Result<ObjectLifecycle> InspectLifecycle(const ObjectAccessContext&,
                                             ObjectHandle) const noexcept;

    // 지난 호출 뒤 DestroyRequested가 된 객체(RequestDestroy·Object::Destroy·Discard)를 out에 꺼내
    // 그 수를 돌려준다(out이 차면 남은 것은 다음 호출이 꺼낸다 — 0이 나올 때까지 부른다). 그 사이
    // 회수된 객체는 빠진다. 소유 계층(World)이 자기 API 밖의 파괴를 객체 수에 비례하는 훑기 없이
    // 찾으려고 쓴다. 순서는 정하지 않는다. 할당하지 않는다. 접근 규칙과 오류는 InspectLifecycle과
    // 같다.
    //
    // 사용 예:
    //   ObjectHandle batch[64];
    //   for (;;) {
    //       auto taken = registry.TakeDestroyRequests(access, batch);
    //       if (!taken || taken.Value() == 0) break;
    //       for (usize at = 0; at < taken.Value(); ++at) { /* batch[at] 처리 */ }
    //   }
    Result<usize> TakeDestroyRequests(const ObjectAccessContext&, Span<ObjectHandle> out) noexcept;

    // 객체가 지금 클러스터 소속인지다(진단·시험용 — 장기 smoke가 경계 비율 안전장치의 해체를 모델에
    // 맞춘다). 클러스터를 켜지 않은 Registry는 언제나 false다. handle·접근 규칙과 오류는
    // InspectLifecycle과 같다(cutoff white는 false). 결과는 반환 후에도 바뀔 수 있다.
    Result<bool> InspectClusterMembership(const ObjectAccessContext&, ObjectHandle) const noexcept;

    // handle이 이 Registry가 발급한 것인지다(domain 비교만 — 생존·세대·접근 문맥은 보지 않는다).
    // 조회 API는 다른 Registry의 handle을 회수된 handle과 같은 StaleHandle로 알리므로, 둘을
    // 구별해야 하는 쪽이 먼저 부른다 — weak 참조(WeakObjectRef PROPERTY)는 죽은 대상이면 null로
    // 쓰지만 다른 Registry를 가리키면 WrongContext로
    // 실패한다(Docs/Design/ReflectionExtensions.ko.md 5.1절). null handle(registry 0)은 거짓이다.
    // 잠금·스레드 검사 없이 어느 스레드에서나 부를 수 있다.
    //
    // 사용 예:
    //   if (!target.IsNull() && !registry.OwnsHandle(target)) {
    //       return Result<void>::Failure(ErrorCode::WrongContext);
    //   }
    bool OwnsHandle(ObjectHandle handle) const noexcept {
        return handle.registry != 0 && handle.registry == domain_;
    }

    Result<BackendLease> AcquireBackendLease(const ObjectAccessContext&, ObjectHandle) noexcept;
    // 읽기용 불변 metadata/code lease를 복제한다. 객체 수명이나 접근권은 따로 보존해야 한다.
    // cutoff·논리 파괴 뒤에는 새 조회를 거부하며 호출자의 context/owner thread를 검사한다.
    Result<TypeLease> AcquireType(const ObjectAccessContext&, ObjectHandle) noexcept;
    // 참조 필드의 선언 타입을 이 저장소의 등록 타입에서 조회한다. 객체를 생성하거나 pin하지 않는다.
    // owner/context/종료/재진입 규약은 AcquireType과 같고 미등록 ID는 NotFound다.
    Result<TypeLease> AcquireRegisteredType(const ObjectAccessContext&, TypeId) noexcept;
    // 소유 스레드 full STW 수집. Borrow/생성 트랜잭션/재진입 중에는 Busy다.
    // 작업 큐 할당이나 Trace 간선 검증 실패 시 부분 표식으로 sweep하지 않는다.
    // scheduler를 주면 표식을 병렬로 한다(StepCollection의 scheduler 규칙과 같다 — 소유 스레드도
    // 표식에 참여하고 parallelTraceSafe가 아닌 타입은 소유 스레드만 trace한다). 반환 전에 worker
    // 작업을 모두 회수한다.
    //
    // 사용 예(레벨 전환 때 STW 병렬 수집):
    //   auto report = registry.Collect(access, &scheduler);
    Result<GcReport> Collect(const ObjectAccessContext&,
                             WorkerScheduler* scheduler = nullptr) noexcept;
    // Begin/Step은 Borrow=0·활성 생성=0인 소유 스레드 safepoint에서만 진행한다.
    // slice 사이에는 mutator를 재개하며 root/간선/신규 게시가 삽입 장벽을 거친다.
    Result<void> BeginCollection(const ObjectAccessContext&) noexcept;
    // scheduler를 지정하면 표식을 병렬로 한다(GC-S4, Docs/Design/GarbageCollectionScale.ko.md):
    // worker 수만큼 task를 한 번 제출하고 소유 스레드도 참여한다. 참여자마다 지역 큐(512칸 블록)를
    // 두고 남는 블록을 공유 풀에 내놓아 나눈다. 표식은 atomic fetch_or로 이긴 쪽만 큐에 넣는다.
    // parallelTraceSafe가 아닌 수동 Trace 타입은 소유 스레드 큐로 모아 이 스레드에서 trace한다.
    // 예산(객체 수·시간)이 다하면 남은 회색 객체를 다음 slice로 넘긴다. allocator는 여러 스레드의
    // 동시 할당·반환을 지원해야 한다(worker의 큐 블록·capture 정리). 각 slice는 worker를 모두
    // 회수한 뒤 반환하므로 mutator와 field scan을 동시에 실행하지 않는다. scheduler worker
    // 스레드에서 부르면 WrongThread다.
    Result<GcProgress> StepCollection(const ObjectAccessContext&, GcBudget = {},
                                      WorkerScheduler* scheduler = nullptr) noexcept;
    // 아직 cutoff를 게시하지 않은 Marking만 취소한다. cutoff 이후 중단으로 죽은 간선을 부활시키지
    // 않도록 Sweeping/Reclaiming 취소는 Busy다. 마지막 성공 보고서는 바꾸지 않는다.
    Result<void> CancelCollection(const ObjectAccessContext&) noexcept;
    GcPhase CollectionPhase() const noexcept;
    Result<uint32> DrainRetired(const ObjectAccessContext&) noexcept;
    // 영구 창(RegistryOptions::permanentRegion)을 닫는다. 이 뒤에 Commit하는 객체는 일반 객체이고
    // 다시 열 수 없다(이미 닫았거나 창 없이 만든 Registry는 성공 — 무동작). GC·callback 처리 중에는
    // Busy(Commit 검증과 공개 사이에 창 상태가 바뀌지 않게), 종료 시작 뒤에는 ShuttingDown이다.
    //
    // 사용 예(Engine이 서브시스템 초기화 뒤):
    //   if (auto closed = objects.ClosePermanentRegion(access); !closed) { return closed; }
    Result<void> ClosePermanentRegion(const ObjectAccessContext&) noexcept;
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
    template <class T>
    friend Result<void> detail::SetPathMemberReference(detail::PathCursor&, ObjectRef<T>&,
                                                       ObjectHandle) noexcept;
    friend struct detail::ObjectRegistryAccess;
    friend class ObjectBatchPublish;
    friend class PreparedStructElements;

    struct Slot;
    struct SlotCold;
    struct Chunk;
    struct MarkState;
    struct NameRecord;
    struct NameEntry;
    struct InnerEntry;
    struct ReclaimWork;
    struct ChunkTable;
    struct EdgeTally;
    struct EdgePlan;
    struct GcHot;
    struct GcBits;
    struct GcPage;
    struct GcType;
    struct GcBlock;
    struct ParallelMark;
    struct MarkParticipant;
    struct ClusterCell;
    struct ClusterChunk;
    struct ClusterRecord;

    // Retired 슬롯이 든 회수 대상 목록이다. 슬롯은 셋 중 하나에만 있다(Retired가 아니면 어디에도
    // 없다).
    enum class RetiredQueue : uint8 {
        None,
        Pending,  // 아직 분류 전(Retire 순서)
        Deferred, // 보유 계수·남은 자식으로 지금은 회수할 수 없다(분류 뒤 다시 걷지 않는다)
        Ready     // 지연이 풀려 회수할 수 있다(풀린 순서)
    };

    // Slot의 nextInList·prevInList로 잇는 이중 연결 목록이다.
    struct RetiredList {
        Slot* head = nullptr;
        Slot* tail = nullptr;
    };

    // 표식(Private/ObjectRegistryGcMark.cpp). MarkBatch는 직렬 marker의 배치 visitor 콜백이다(잠금
    // 없음 — 처리 표시 아래 단일 marker, Docs/Design/GarbageCollectionScale.ko.md 7.1절).
    // MarkHandles가 본문이다. 병렬 marker는 ParallelMark(ObjectRegistryGcParallel.cpp)다.
    static void MarkBatch(void*, const ObjectHandle*, uint32) noexcept;
    void MarkHandles(MarkState&, const ObjectHandle*, uint32) noexcept;
    // 간선 handle이 이 Registry의 슬롯 범위 안인지 본다(Release 검사). Debug는 세대·상태까지 보아
    // "강한 간선은 언제나 살아 있는 객체를 가리킨다"를 확인한다.
    bool IsMarkableEdge(ObjectHandle handle, uint64 slots) const noexcept;
    // 슬롯 index를 표식한다(잠금 없이 또는 gate_ 아래). 살아 있지 않으면 StaleHandle이다. 잎 타입은
    // 큐에 넣지 않고 Outer 사슬을 바로 따라간다(M2). work는 모든 슬롯을 담을 용량을 미리 확보했다.
    ErrorCode MarkIndex(uint32 index, Array<uint32>& work, uint32& marked) noexcept;
    // index 객체 하나를 trace한다(Trace 간선 → Outer → 패키지 정책이면 자식 고리).
    void TraceIndex(uint32 index, MarkState& state, TraceVisitor& visitor) noexcept;
    // 객체의 강한 간선을 visitor에 넘긴다(참조 위치 표가 있으면 표로, 없으면 trace로 — M3). Debug는
    // 표와 trace의 간선 순서를 대조한다.
    void VisitStrongEdges(const GcType& type, const Object& object,
                          TraceVisitor& visitor) const noexcept;
    // 큐에서 몇 칸 앞 객체의 배열 버퍼를 미리 읽는다(참조 위치 표의 배열 칸만).
    void PrefetchArrays(uint32 index) const noexcept;
    // 잠금 밖·처리 표시 아래: 새 객체로 그 타입의 참조 위치 표를 정한다(첫 객체에서 한 번).
    void PrepareReferenceLayout(uint16 type, const Object& object) noexcept;
    // 작업 큐 work를 cursor부터 trace한다(최대 maxObjects개, deadline이 0이 아니면 그때까지 —
    // 시계는 일정 개수마다 본다). 잠금 없이 처리 표시 아래에서 부른다. 반환은 trace한 객체 수이고
    // 오류는 state.error다.
    uint32 MarkSerial(Array<uint32>& work, usize& cursor, MarkState& state, uint32 maxObjects,
                      uint64 deadline) noexcept;
    // 병렬 표식이다(GC-S4). work[cursor..)를 씨앗으로 쓰고 work를 비운 뒤, 예산이 다해 남은 회색
    // 객체를 work에 다시 담는다(cursor는 0). 반환은 trace한 객체 수이고 오류는 state.error다.
    // 끝까지 표식했으면 completed가 참이다.
    // carried가 있으면 그 블록 목록(앞 slice가 남긴 회색 객체)도 씨앗으로 쓰고, 예산이 다해 남은
    // 회색 객체를 work에 펴지 않고 블록째 carried에 남긴다(slice마다 회색 객체 전체를 복사하지
    // 않는다 — GC-S4 보정). times가 있으면 준비·표식·회수·넘기기 시간과 넘긴 회색 객체 수를 적는다.
    uint32 MarkParallel(Array<uint32>& work, usize& cursor, MarkState& state, uint32 maxObjects,
                        uint64 deadline, WorkerScheduler& scheduler, bool& completed,
                        GcBlock** carried = nullptr, GcSliceTimes* times = nullptr) noexcept;
    // 남은 회색 블록(gcGrayBlocks_)을 표식 큐 뒤로 펴거나(직렬 slice) 재사용 목록으로
    // 돌린다(취소·오류· cycle 끝).
    void FlattenGrayBlocks() noexcept;
    void ReleaseGrayBlocks() noexcept;
    // 장벽 큐(gcShade_)를 표식 큐(gcWork_) 뒤로 옮긴다(slice 시작).
    void DrainShadeQueue() noexcept;
    static void ValidateStagedEdge(void*, ObjectHandle) noexcept;
    // 떼어 낼 간선의 패키지 쌍을 집계한다(MarkState의 tally·ownerPackage·delta — 대상 패키지 조회는
    // 간선마다 잠금, 집계 삽입은 잠금 밖).
    static void TallyEdge(void*, ObjectHandle) noexcept;
    // Registry가 직접 방문하는 간선이다: inner → outer, 패키지 정책이면 outer → inner(자식 고리의
    // 모든 자식). gate_를 쥐지 않고 부른다(처리 표시 아래라 Outer·고리가 바뀌지 않는다).
    void VisitIdentityEdges(const Slot&, TraceVisitor&) const noexcept;
    // 정의는 ObjectRuntime/Private/ObjectRegistryInternal.h(inline — 구현 파일만 부른다).
    inline Slot* FindSlot(ObjectHandle) const noexcept;
    inline Slot* SlotAt(uint32) const noexcept;
    // 슬롯의 cold 레코드(이름·고리·회수 목록·생성 트랜잭션·Outer)다.
    inline SlotCold& ColdOf(const Slot&) const noexcept;
    // 생성된 객체(object가 있는 슬롯)의 저장소 시작 주소다. hot의 object와 타입의 기반 위치
    // (TypeLease::ObjectOffset)에서 유도한다 — 소유자 범위 검사가 cold를 읽지 않게 한다.
    static inline uptr StorageStartOf(const Slot&) noexcept;
    // GC 밀집 자료(GC-S3, 정의는 ObjectRegistryInternal.h). 슬롯 index의 페이지·hot 레코드·비트
    // 묶음과 그 묶음 안의 비트다. index는 chunk가 있는 슬롯이어야 한다.
    inline GcPage& PageOf(uint32) const noexcept;
    inline GcHot& HotOf(uint32) const noexcept;
    inline GcBits& BitsOf(uint32) const noexcept;
    // 클러스터 장부 칸(계수·고리)과 장부 상태다(클러스터를 켠 Registry만 — 정의는
    // ObjectRegistryInternal.h).
    inline ClusterCell& CellOf(uint32) const noexcept;
    inline uint32& ClusterStateOf(uint32) const noexcept;
    // 장부 inCount(Slot 안 — 공개 전 가입 지정이면 루트 슬롯)다.
    inline uint32& ClusterInCountOf(uint32) const noexcept;
    // chunk 클러스터 표와, 소속 슬롯의 클러스터 번호다(표에 있으면 표, 섞인 chunk면 장부 상태).
    inline ClusterChunk& ClusterChunkOf(uint32) const noexcept;
    inline uint32 MemberClusterOf(uint32) const noexcept;
    // 잠금 보유: slot의 장부(상태·계수·고리)를 비운다.
    void ClearClusterSlotLocked(uint32 slot) noexcept;

    // ---- 클러스터(GC-S7, Docs/Design/GarbageCollectionScale.ko.md 13절 — Private/
    // ObjectRegistryClusters.cpp) ----
    // ConstructionContext::SetClusterRoot·JoinCluster의 본문이다. root가 null이면 object를 새
    // 클러스터 루트로 지정한다.
    Result<void> DesignateCluster(const ObjectAccessContext&, uint64 transaction,
                                  ObjectHandle object, ObjectHandle root) noexcept;
    // 잠금 보유: id 클러스터가 이번 cycle에 표식됐는지다(영구 영역은 언제나 참).
    bool IsClusterMarkedLocked(uint32 id) const noexcept;
    // 잠금 보유: owner(소속 객체)의 간선 하나를 장부에 더하거나 뺀다(13.4절 간선 분류).
    void LedgerEdgeLocked(uint32 owner, uint32 target, bool add) noexcept;
    // 잠금 보유: 단일 간선 지점의 장부 갱신이다(owner가 소속이 아니면 아무것도 하지 않는다).
    void LedgerEditLocked(const Slot& owner, ObjectHandle removed, ObjectHandle added) noexcept;
    // 간선 제거의 장부 감소를 미뤄 기록한다(GC 설계 13.4절 — 대상 줄을 건드리지 않는다). 기록이
    // 차면 먼저 적용한다.
    void DeferLedgerRemovalLocked(uint32 owner, ObjectHandle target) noexcept;
    // 미룬 장부 감소를 모두 적용한다. 장부 계수로 판단하거나 클러스터 상태를 바꾸기 전(이탈·해체·
    // 경계 비율 판정·Begin·Collect)에 부른다. Debug 장부 검증은 적용하지 않고 기대 계수에 더한다.
    void ApplyLedgerRemovalsLocked() noexcept;
    // 클러스터 밖 대상으로 가던 간선 하나를 소유자 outCount에서 뺀다(경계 고리 출입 포함).
    void LedgerOutsideRemoveLocked(uint32 owner) noexcept;
    // 잠금 보유: 비우기 지점에서 떼어 낼 값 count개를 장부에서 뺀다(owner가 소속이 아니면 무동작).
    void LedgerClearLocked(const Slot& owner, const ObjectHandle* values, usize count) noexcept;
    // owner가 클러스터 소속인지다(잠금 보유, 클러스터를 끈 Registry는 거짓).
    bool IsClusterMemberLocked(const Slot& owner) const noexcept;
    // TraceVisitor 콜백(잠금 보유 상태의 호출자가 쓴다): MarkState::ledgerOwner의 간선을 장부에서
    // 뺀다.
    static void LedgerRemoveVisited(void*, ObjectHandle) noexcept;
    // TraceVisitor 콜백(잠금 밖): 간선마다 잠가 MarkState::ledgerOwner의 몫을 장부에서 뺀다(이탈).
    static void LeaveVisited(void*, ObjectHandle) noexcept;
    // 잠금 밖·처리 표시 아래: 이탈할 소속 객체 slot이 장부에 남긴 몫(Trace 간선과 outer)을 뺀다.
    void LeaveTrace(Slot&) noexcept;
    // 잠금 보유: LeaveTrace 뒤 slot을 클러스터에서 뺀다(inCount가 남으면 detached, 아니면 없음).
    // Marking이고 클러스터가 표식됐으면 slot을 shade하고, Sweeping이면 mark를 켠다.
    void FinishLeaveLocked(Slot&) noexcept;
    // 잠금 보유: 클러스터를 해체한다(13.6절 Marking·Sweeping 규칙 포함).
    void DissolveClusterLocked(uint32 id) noexcept;
    // 잠금 보유: 표식되지 않은 클러스터를 모두 해체한다(cutoff·Collect sweep 직전).
    void DissolveUnmarkedClustersLocked() noexcept;
    // 잠금 보유: 경계 비율이 registry_detail::ClusterFlattenBoundaryPercent를 넘는 패키지
    // 클러스터를 해체하고 그 수를 돌려준다(경계 비율 안전장치 — cutoff, Collect sweep 뒤). 남은
    // 클러스터는 모두 표식된 상태여야 한다.
    uint32 FlattenBoundaryClustersLocked() noexcept;
    // 경계 비율 안전장치의 문턱을 넘는 레코드인지다(패키지 클러스터만).
    bool ExceedsFlattenThreshold(const ClusterRecord& record) const noexcept;
    // 잠금 보유: 안전장치로 클러스터를 해체한 패키지(살아 있는 루트가 소속이 아니다)인지 본다. 그
    // 패키지의 새 객체는 클러스터에 가입하지 않는다.
    bool IsFlattenedPackageLocked(const Slot& package) const noexcept;
    // 잠금 보유: slot이 영구 영역 소속인지다.
    bool IsPermanentLocked(uint32 slot) const noexcept;
    // 잠금 보유: 영구 소속 owner의 새 간선 target이 영구 객체가 아니면 Unsupported다(13.7절).
    Result<void> CheckPermanentEdgeLocked(const Slot& owner, ObjectHandle target) const noexcept;
    // 잠금 보유: 표식 epoch를 하나 올린다(Begin·Collect — 한 바퀴 돌면 레코드 epoch를 비운다).
    void AdvanceGcEpochLocked() noexcept;
    // 잠금 보유: epoch가 한 바퀴 돌았을 때 레코드 epoch를 비운다.
    void WrapGcEpochLocked() noexcept;
    // 잠금 보유: 모든 클러스터를 해체한다(Shutdown).
    void DissolveAllClustersLocked() noexcept;
    // 잠금 보유: Marking 중 해체해 큐 항목이 남았을 수 있던 레코드를 빈 목록으로 돌린다(큐가 빈
    // 뒤).
    void ReleaseDissolvedClustersLocked() noexcept;
    // 잠금 보유: 새 클러스터 레코드를 받아 root를 첫 소속으로 넣는다(빈 레코드가 있어야 한다).
    uint32 CreateClusterLocked(uint32 root, bool designated) noexcept;
    // 잠금 보유: slot을 id 클러스터 소속으로 넣는다(장부 칸은 비어 있어야 한다).
    void JoinClusterLocked(uint32 id, uint32 slot) noexcept;
    // 잠금 보유: slot의 chunk 클러스터 표 칸을 id로 맞춘다(others는 같은 chunk의 다른 소속 비트).
    void SetChunkClusterLocked(uint32 slot, uint32 id, uint64 others) noexcept;
    // 잠금 보유: 표식 비트와 클러스터 표식을 본 소속 비트를 모두 지운다(Begin·Collect).
    void ClearMarksLocked() noexcept;
    // 잠금 밖·처리 표시 아래: 빈 클러스터 레코드가 count개 이상이 되게 표를 키운다(Marking이면 작업
    // 큐도).
    Result<void> ReserveClusters(uint32 count) noexcept;
    // 작업 큐 상한(슬롯 수 slots + 클러스터 레코드 수)이다.
    uint64 GcQueueBound(uint64 slots) const noexcept;
    // 잠금 밖·처리 표시 아래: 작업 큐와 장벽 큐를 needed칸 이상으로 키운다(두 배씩, 상한 limit).
    Result<void> ReserveGcQueues(uint64 needed, uint64 limit) noexcept;
    // 잠금 보유: 장부 칸 slot을 head 목록(boundary면 경계 고리, 아니면 소속·detached 고리) 앞에
    // 넣거나 뺀다.
    void LinkCellLocked(uint32& head, uint32 slot, bool boundary) noexcept;
    void UnlinkCellLocked(uint32& head, uint32 slot, bool boundary) noexcept;
    // 잠금 보유: root 슬롯에 가입할 수 있는지다(transaction에서 루트로 지정한 공개 전 객체, 또는
    // 해체되지 않은 클러스터의 생존 루트).
    bool IsJoinableRootLocked(uint32 root, uint64 transaction) const noexcept;
    // 클러스터 표식(직렬): id 클러스터를 표식하고 처음이면 큐에 넣는다.
    ErrorCode MarkCluster(uint32 id, Array<uint32>& work, uint32& marked) noexcept;
    // 클러스터 처리(직렬): 경계 소속을 큐에 넣고 detached를 표식한 뒤 처리 epoch를 단다.
    void ProcessCluster(uint32 id, MarkState&) noexcept;
    // Debug 장부 검증(RegistryOptions::verifyClusterLedger): 잠금 밖·처리 표시 아래에서 다시 계산해
    // 비교하고 어긋나면 Fatal이다. 할당 실패는 그 오류다(검증을 건너뛰지 않는다).
    Result<void> VerifyClusterLedger() noexcept;
    // 잠금 보유: 슬롯을 루트 목록에 넣고 뺀다(roots 0 ↔ 1 전이에서 부른다).
    void AddRootListLocked(Slot&) noexcept;
    void RemoveRootListLocked(Slot&) noexcept;
    // 잠금 밖·처리 표시 아래: ops의 GC 타입 표 번호를 찾거나 새로 단다(0은 없음). 표가 가득하면
    // CapacityExceeded, 할당 실패는 그 오류다.
    Result<uint16> AcquireGcType(const RuntimeTypeOps& ops) noexcept;
    // 잠금 밖·처리 표시 아래: 슬롯 수 slots를 담을 GC 페이지·루트 목록을 준비한다. 새 페이지·표·
    // 목록은 잠금 안에서 게시해야 하므로 넘겨받은 칸에 담아 돌려준다(GrowGcLocked).
    struct GcGrowth;
    Result<void> PrepareGcGrowth(uint64 slots, GcGrowth& growth) noexcept;
    void GrowGcLocked(GcGrowth& growth) noexcept;
    void ReleaseGcGrowth(GcGrowth& growth) noexcept;
    Result<void> CheckAccess(const ObjectAccessContext&) const noexcept;
    Result<void> CheckLive(Slot*, const void*) const noexcept;
    Result<ObjectAccessContext> ScopedAccess() const noexcept;
    Result<ObjectLifecycle> InspectChecked(const ObjectAccessContext&, ObjectHandle, const Object*,
                                           bool requireBorrow) const noexcept;
    Result<void> RequestDestroyChecked(const ObjectAccessContext&, ObjectHandle, const Object*,
                                       bool requireBorrow) noexcept;
    // 잠금 보유: RequestDestroyChecked의 검사와 상태 전이다. 클러스터 소속(루트 아님)이면 처리
    // 표시를 세우고 leaving에 슬롯을 돌려준다(호출자가 잠금 밖에서 이탈 trace를 한다).
    Result<void> RequestDestroyLocked(ObjectHandle, const Object*, bool requireBorrow,
                                      Slot*& leaving) noexcept;
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
    // swap이면 지운 자리에 끝 원소를 옮긴다(RemoveReferenceAtSwap), 아니면 뒤를 당긴다.
    Result<void> RemoveArrayEdge(const ObjectAccessContext&, ObjectHandle,
                                 detail::ObjectRefArrayStorage&, usize, ObjectHandle, bool byHandle,
                                 bool swap = false) noexcept;
    Result<void> ClearArrayEdges(const ObjectAccessContext&, ObjectHandle,
                                 detail::ObjectRefArrayStorage&) noexcept;
    Result<void> AssignArrayEdges(const ObjectAccessContext&, ObjectHandle,
                                  detail::ObjectRefArrayStorage&, Span<const ObjectHandle>,
                                  uint64 transaction, const void*) noexcept;
    // ObjectRefMap 편집 본문이다(Private/ObjectRegistryMapEdges.cpp — 리플렉션 B9f4). edit가 키
    // 형식별 조회·준비·게시를 맡는다(Object/ObjectRefMap.h).
    Result<void> SetMapEdge(const ObjectAccessContext&, ObjectHandle, detail::ObjectRefMapEdit&,
                            ObjectHandle target, const void* nativeType,
                            MapReferenceWrite* write) noexcept;
    Result<void> EraseMapEdge(const ObjectAccessContext&, ObjectHandle, detail::ObjectRefMapEdit&,
                              ObjectHandle* removed) noexcept;
    Result<void> ClearMapEdges(const ObjectAccessContext&, ObjectHandle,
                               detail::ObjectRefMapEdit&) noexcept;
    // 잠금 보유: 비울 map의 값을 클러스터 장부에서 뺀다(owner가 소속이 아니면 무동작).
    void ClearMapLedgerLocked(const Slot& owner, detail::ObjectRefMapEdit&) noexcept;
    Result<void> AssignMapEdges(const ObjectAccessContext&, ObjectHandle, detail::ObjectRefMapEdit&,
                                Span<const ObjectHandle>, uint64 transaction,
                                const void* nativeType) noexcept;
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
                                        uint64 transaction) noexcept;

    template <class S>
    Result<void> AssignElementsStaged(const ObjectAccessContext& access, ObjectHandle owner,
                                      StructRefArray<S>& array, usize count,
                                      uint64 transaction) noexcept;

    template <class K, class T>
    Result<void> AssignMapReferencesStaged(const ObjectAccessContext& access, ObjectHandle owner,
                                           ObjectRefMap<K, T>& map, Span<const K> keys,
                                           Span<const ObjectHandle> targets,
                                           uint64 transaction) noexcept;

    template <class S, class T>
    Result<void> SetElementReferenceStaged(const ObjectAccessContext& access, ObjectHandle owner,
                                           StructRefArray<S>& array, usize index,
                                           ObjectRef<T>& field, ObjectHandle target,
                                           uint64 transaction) noexcept;

    // StructRefArray 원소 위치(detail::StructRefArrayAccess::Location)로 고르는 live 원소 참조
    // 쓰기다. 경로 접근(B6)의 참조 멤버 쓰기 단일 지점 detail::SetPathMemberReference만 friend로
    // 부른다 — 경로 thunk는 원소를 고른 단계의 배열 타입을 모르고 위치만 들고 내려온다. 위조한
    // 위치를 공개 API로 받지 않으려고 private이다. 검사(owner Alive와 Borrow, 컨테이너가 owner
    // 저장소 안, index < Size(), field가 그 원소 블록 안 — 아니면 InvalidArgument), 대상 규칙,
    // shade, encode 고정 Busy는 공개 (배열, index) 오버로드와 같다.
    template <class T>
    Result<void> SetElementReference(const ObjectAccessContext& access, ObjectHandle owner,
                                     const detail::StructElementLocation& element,
                                     ObjectRef<T>& field, ObjectHandle target) noexcept;

    Result<RootRef<Object>> NewRoot(const ObjectAccessContext&, TypeId, const void*,
                                    ObjectHandle outer, StringView name) noexcept;
    Result<uint64> BeginConstruction(const ObjectAccessContext&) noexcept;
    // 생성 오류 순서 1–12(Docs/Design/ObjectFoundations.ko.md 1.5절)를 따른다. 0단계(트랜잭션
    // 상태·저널 예약)는 ConstructionContext가 먼저 한다.
    Result<ObjectHandle> CreatePending(const ObjectAccessContext&, TypeId, uint64, const void*,
                                       ObjectHandle outer, StringView name) noexcept;
    Result<void*> GetPending(const ObjectAccessContext&, ObjectHandle, uint64,
                             const void*) noexcept;
    // ConstructionContext::AcquireType의 본문이다: 생존(Alive, cutoff 아님) 또는 transaction의 공개
    // 전 객체(생성자 실행 뒤)의 타입 lease, 그 밖은 StaleHandle(staged 참조 대상 규칙과 같다).
    Result<TypeLease> AcquireTypeChecked(const ObjectAccessContext&, ObjectHandle,
                                         uint64 transaction) noexcept;
    // ConstructionContext::Commit(roots, out)의 Registry 몫이다. roots는 이 트랜잭션 객체여야 하고
    // (StaleHandle), out은 roots와 크기가 같고 비어 있다(호출자가 확인한다).
    Result<void> Commit(const ObjectAccessContext&, uint64, Span<const ObjectHandle> staged,
                        Span<const ObjectHandle> roots, Span<RootRef<Object>> out) noexcept;
    void Rollback(uint64, Span<const ObjectHandle>) noexcept;
    // transaction이 0이 아니면 그 트랜잭션의 공개 전 객체도 받는다(ConstructionContext 판).
    Result<OuterSummary> DescribeOuterChecked(const ObjectAccessContext&, ObjectHandle outer,
                                              uint64 transaction) const noexcept;
    // FindObject의 본문이다. transaction이 0이 아니면 outermost와 경로의 객체로 그 트랜잭션의 공개
    // 전 객체도 받는다(ConstructionContext::FindObject — 다른 트랜잭션의 공개 전 객체는 outermost면
    // StaleHandle, 경로 구성 요소면 NotFound).
    Result<ObjectHandle> FindObjectChecked(const ObjectAccessContext&, ObjectHandle outermost,
                                           StringView objectPath,
                                           uint64 transaction) const noexcept;
    ErrorCode ShadeLocked(ObjectHandle) noexcept;
    bool IsCutoff(const Slot*) const noexcept;
    bool IsReclaimable(const Slot&) const noexcept;
    // 지금 회수할 수 있는 슬롯이 있는지다(NextReclaimableLocked와 같은 분류 — 슬롯 전체를 훑지
    // 않는다).
    bool HasReclaimableLocked() noexcept;
    // 잠금 보유: 다음에 회수할 슬롯(목록에 둔 채)이다. 풀린(Ready) 목록 맨 앞, 없으면 분류
    // 전(Pending) 목록을 앞에서부터 분류해 회수할 수 없는 슬롯은 지연(Deferred) 목록으로 옮긴다.
    // 슬롯마다 분류는 한 번이라 회수 한 개당 분할 상환 O(1)이다. 없으면 nullptr.
    Slot* NextReclaimableLocked() noexcept;
    // 회수할 수 있는 슬롯을 차례로 회수한다(최대 maximum개, deadline이 지나면 멈춘다). 호출자는
    // 처리 표시(processing_)를 세운 상태다.
    uint32 ReclaimEligible(uint32 maximum = NumericLimits<uint32>::Max(),
                           uint64 deadline = 0) noexcept;
    // 잠금 보유: Alive·DestroyRequested 슬롯을 Retired로 바꾸고 이름 색인·부모 고리에서 떼어 분류
    // 전 목록 끝에 붙인다(sweep).
    void RetireLocked(Slot&) noexcept;
    // 잠금 보유: chunk의 white 후보(GC 비트 묶음의 live & ~mark)를 낮은 번호부터 Retire하고 그 수를
    // 돌려준다(sweep).
    uint32 RetireWhiteLocked(uint32 chunk) noexcept;
    // 잠금 보유: 어느 목록에도 없는 Retired 슬롯을 queue 목록 끝에 붙인다.
    void LinkRetiredLocked(Slot&, RetiredQueue) noexcept;
    // 잠금 보유: DestroyRequested가 된 slot을 파괴 요청 목록에 넣는다 / 뺀다.
    void LinkDestroyRequestLocked(Slot&) noexcept;
    void UnlinkDestroyRequestLocked(Slot&) noexcept;
    // 잠금 보유: 슬롯을 지금 든 회수 대상 목록에서 뗀다.
    void UnlinkRetiredLocked(Slot&) noexcept;
    // 잠금 보유: 지연 목록의 슬롯이 회수 가능해졌으면(보유 계수 반환·마지막 자식이 떨어짐) 풀린
    // 목록 끝으로 옮긴다. 그 밖의 슬롯에는 아무것도 하지 않는다.
    void WakeDeferredLocked(Slot&) noexcept;
    // 잠금 보유: 회수 가능하고 어느 목록에도 없는 slot을 Reclaiming으로 바꾸고, 잠금 밖에서 소멸·
    // 반환할 것을 넘긴다.
    ReclaimWork TakeForReclaimLocked(Slot&) noexcept;
    // 잠금 밖에서 부른다: work의 객체를 소멸·반환한 뒤 다시 잠가 슬롯을 비우고 빈 슬롯 목록에
    // 넣는다(세대 한도면 Exhausted로 격리한다).
    void FinishReclaim(ReclaimWork&) noexcept;

    // ---- Outer·이름(잠금 보유 상태에서 부른다) ----
    ObjectHandle HandleOf(const Slot&) const noexcept;
    // 공개된 객체(Alive·DestroyRequested, cutoff 아님)의 슬롯이다. 아니면 StaleHandle·Destroyed다.
    Result<Slot*> FindPublishedLocked(ObjectHandle) const noexcept;
    // FindPublishedLocked에 transaction(0이 아니면)의 공개 전 객체(생성자 실행 뒤, cutoff 아님)를
    // 더한다.
    Result<Slot*> FindVisibleLocked(ObjectHandle, uint64 transaction) const noexcept;
    // scanNamedInners가 거짓이면 hasNamedInners를 구하지 않는다(자식 고리를 훑지 않는다 — 이름 있는
    // 객체 생성은 그 값을 쓰지 않는다).
    OuterSummary DescribeSlotLocked(const Slot&, bool scanNamedInners = true) const noexcept;
    // (outer, 비교 키)의 색인 항목 슬롯이다. published면 공개된 객체만, 아니면 점유 기준(Reserved
    // 포함)이다. cutoff white 항목은 건너뛴다. 없으면 nullptr.
    Slot* FindNamedLocked(ObjectHandle outer, uint64 hash, StringView key,
                          bool published) const noexcept;
    // 조회 기준의 색인 항목이다: 공개된 객체와 transaction(0이 아니면)의 공개 전 객체만 돌려주고,
    // 다른 트랜잭션의 공개 전 객체가 그 이름을 점유하면 nullptr다.
    Slot* FindVisibleNamedLocked(ObjectHandle outer, uint64 hash, StringView key,
                                 uint64 transaction) const noexcept;
    // 다음 삽입 한 번에 필요한 새 색인 표 크기다(0이면 지금 표로 충분하다).
    Result<uint32> NameIndexGrowthLocked() const noexcept;
    // 색인을 table(capacity 칸, 0으로 채워짐)로 옮기고 옛 표를 돌려준다(잠금 밖에서 반환한다).
    NameEntry* RehashNamesLocked(NameEntry* table, uint32 capacity) noexcept;
    // slot을 색인에 넣는다. 같은 키의 cutoff white 항목은 그 자리에서 뺀다.
    void InsertNameLocked(Slot&, uint64 hash, StringView key) noexcept;
    void RemoveNameLocked(Slot&) noexcept;
    void LinkChildLocked(Slot& parent, Slot& child) noexcept;
    void UnlinkChildLocked(Slot& child) noexcept;
    // Retire(sweep·rollback)가 같은 잠금 안에서 이름 색인 항목과 자기 고리를 뗀다.
    void DetachIdentityLocked(Slot&) noexcept;

    // ---- 관찰자(Docs/Design/ObjectFoundations.ko.md 3절 — 잠금 보유 상태에서 부른다) ----
    // 관찰 대상 간선의 대상(Reserved·Alive·DestroyRequested — 회수 전이라 outermost 슬롯이
    // 유효하다, GC 색과 무관)의 소속 패키지다(그 밖이면 불변식 위반으로 Fatal).
    ObjectHandle PackageOfLocked(ObjectHandle target) const noexcept;
    // owner의 간선 변화를 기록하는지다(관찰자가 있고 owner가 Alive이며 Outer가 있다).
    bool ObservesOwnerLocked(const Slot& owner) const noexcept;
    // 단일 간선 지점의 기록 계획이다(옛 대상 removed → 새 대상 added, 옛 대상 키 먼저 — 두 패키지가
    // 같으면 없다).
    EdgePlan PlanEditLocked(const Slot& owner, ObjectHandle removed,
                            ObjectHandle added) const noexcept;
    // 관찰자에 아직 없는 키 수다.
    usize MissingKeysLocked(const EdgePlan&) const noexcept;
    usize MissingKeysLocked(const EdgeTally&, ReferenceChangeCause, uint64) const noexcept;
    // 관찰자의 남은 새 키 공간이다(관찰자가 없으면 최대값).
    usize ObserverFreeLocked() const noexcept;
    void ApplyPlanLocked(const EdgePlan&) noexcept;
    void ApplyTallyLocked(const EdgeTally&, ReferenceChangeCause, uint64) noexcept;
    // 잠금 밖·처리 표시 아래: tally의 새 키 공간을 관찰자에 예약한다(모자랄 때만).
    Result<void> ReserveTally(const EdgeTally&, ReferenceChangeCause, uint64) noexcept;
    // 잠금 밖·처리 표시 아래: 떼어 낼 간선 handles의 (ownerPackage, 대상 패키지, -1)을 tally에
    // 합친다.
    Result<void> TallyHandles(const ObjectHandle* handles, usize count, ObjectHandle ownerPackage,
                              EdgeTally&) noexcept;
    // 잠금 밖·처리 표시 아래: StructRefArray 원소 블록들을 등록 struct trace로 훑어 null이 아닌
    // 간선을 out에 모은다(클러스터 장부). 원소 struct 조회 실패는 Unsupported다.
    Result<void> CollectStructEdges(const detail::StructElementOps&, void* const* blocks,
                                    uint32 count, Array<ObjectHandle>& out) noexcept;
    // 처리 표시를 내리고 result를 돌려준다.
    Result<void> EndProcessing(Result<void> result) noexcept;

    // ---- 묶음 게시 구간 안(Private/ObjectRegistryBatch.cpp — 계약 위반은 Fatal) ----
    template <class T>
    void PublishBatchReference(ObjectRef<T>& field, ObjectHandle target,
                               const detail::StructElementLocation* element = nullptr) noexcept;
    template <class S, class T>
    void PublishBatchElementReference(StructRefArray<S>& array, usize index, ObjectRef<T>& field,
                                      ObjectHandle target) noexcept;
    template <class T>
    void PublishBatchReferences(ObjectRefArray<T>& array,
                                PreparedReferenceArray& prepared) noexcept;
    template <class K, class T>
    void PublishBatchMap(ObjectRefMap<K, T>& map, OrderedMap<K, ObjectHandle>& prepared) noexcept;
    // 단일 참조 위치(element가 있으면 StructRefArray 원소 블록 안, 없으면 owner 저장소 안).
    void PublishBatchEdge(ObjectHandle& field, ObjectHandle target, const void* nativeType,
                          const detail::StructElementLocation* element) noexcept;
    void PublishBatchArray(detail::ObjectRefArrayStorage& storage,
                           detail::ObjectRefArrayStorage& prepared,
                           const void* nativeType) noexcept;
    // 값 부분 트리 자리 하나의 위치를 확인하고 게시를 기록한다(리플렉션 B9i3c0 — 교환은
    // PreparedAttach가 한다).
    void PublishBatchValue(void* location, usize bytes,
                           const detail::StructElementLocation* element) noexcept;
    // 열린 구간과 컨테이너 위치(owner 저장소 안)를 확인한다.
    void CheckBatchContainer(const void* storage, usize bytes) const noexcept;
    // 게시로 빠지는 옛 대상을 검사합에 넣는다.
    void NoteBatchRemoved(ObjectHandle removed) noexcept;
    // 새 값 하나를 정규화(null)·재확인·shade하고 검사합에 넣는다. 재확인에 실패해도 retained(state,
    // 값)가 참이면(컨테이너 옛 내용의 대상 — 이미 게시된 간선, 리플렉션 B9i5) 검사합에만 넣는다.
    void PublishBatchTarget(ObjectHandle& value, const void* nativeType,
                            bool (*retained)(const void*, ObjectHandle) noexcept = nullptr,
                            const void* state = nullptr) noexcept;
    // 관찰자 기록·표지 증가 뒤 구간을 닫는다(ObjectBatchPublish::End).
    void EndBatchPublish(ObjectHandle owner) noexcept;

    Allocator& allocator_;
    TypeRegistry& types_;
    ContextId context_;
    RegistryOptions options_;
    uint64 domain_ = 0;
    uint64 ownerThread_ = 0;
    mutable platform::Mutex gate_;
    // chunk 포인터 표(chunk 번호 = 슬롯 번호 / chunk 크기로 색인해 handle → 슬롯이 O(1))의 칸이다.
    // 표는 가득 찰 때 두 배로 잠금 밖에서 받아 잠금 안에서 바꾼다. 옛 표는 chunkTables_ 사슬로
    // Registry 소멸 때까지 보관한다(잠금 없이 표를 읽는 경로가 교체와 겹쳐도 해제된 표를 읽지 않게
    // — 두 배씩이라 합은 최종 표의 두 배 이하). chunk 자체도 소멸 전까지 이동·반환하지 않으므로
    // Slot*는 안정하다.
    Chunk** chunks_ = nullptr;
    uint32 chunkCount_ = 0;
    uint32 chunkCapacity_ = 0;
    ChunkTable* chunkTables_ = nullptr; // 지금 표(옛 표는 그 previous 사슬)
    // GC 밀집 자료(GC-S3): GcPageSlots개 슬롯마다 페이지 하나(hot 레코드와 live·mark·leaf 비트)다.
    // 페이지는 이동하지 않고 표만 두 배로 키운다(생성의 처리 표시 아래 — marker와 겹치지 않는다).
    GcPage** gcPages_ = nullptr;
    uint32 gcPageCount_ = 0;
    uint32 gcPageCapacity_ = 0;
    // GC 타입 표(hot 레코드의 타입 번호 → trace·병렬 안전·잎). 0번은 비어 있다. 생성의 처리 표시
    // 아래에서만 자란다.
    GcType* gcTypes_ = nullptr;
    uint32 gcTypeCount_ = 0;
    uint32 gcTypeCapacity_ = 0;
    // 타입 키(RuntimeTypeOps 주소) → gcTypes_ 번호의 개방 주소 색인이다(0이면 빈 칸, 용량은 2의
    // 거듭제곱이고 gcTypeCapacity_의 두 배). 생성마다 타입 표를 처음부터 훑지 않게 한다.
    uint16* gcTypeIndex_ = nullptr;
    uint32 gcTypeIndexCapacity_ = 0;
    // 루트(roots > 0) 슬롯 번호 목록이다. Begin·Collect가 O(루트 수)로 루트를 표식한다. 용량은
    // 언제나 슬롯 수 이상이라 gate_ 아래 추가가 할당하지 않는다(다른 스레드의 ReleaseRoot도 gate_
    // 아래에서 즉시 뺀다 — 목록은 Begin·Collect의 잠금 구간만 읽는다).
    uint32* rootList_ = nullptr;
    uint32 rootCount_ = 0;
    // 파괴 요청 목록 머리다(Slot::destroyListed — TakeDestroyRequests).
    Slot* destroyRequests_ = nullptr;
    uint32 rootCapacity_ = 0;
    // 빈(Free) 슬롯 목록이다. 가장 최근에 회수한 슬롯부터 재사용한다(새 chunk는 낮은 번호부터).
    Slot* freeSlots_ = nullptr;
    // 회수 대상 목록(RetiredQueue): sweep이 분류 전 목록 끝에 붙이고, 회수가 분류하며 지연 목록으로
    // 옮기고, 보유 계수 반환·마지막 자식 분리가 지연 목록에서 풀린 목록으로 옮긴다.
    RetiredList retiredPending_;
    RetiredList retiredDeferred_;
    RetiredList retiredReady_;
    // 상태별 슬롯 수와 보유 계수 합이다(상태 전이·계수 변경마다 갱신 — Stats가 슬롯을 훑지 않는다).
    // borrows·chunks·namedObjects는 totalBorrows_·chunkCount_·nameLive_에서 채운다.
    ObjectStats tally_;
    // 잠금 안 탐색 계수(성능 회귀 시험용 — detail::ObjectRegistryAccess): 빈 슬롯을 고르며 본 슬롯
    // 수, 회수 대상 목록에서 본 슬롯 수.
    uint64 freeSlotProbes_ = 0;
    uint64 reclaimProbes_ = 0;
    uint32 totalBorrows_ = 0;
    uint32 constructions_ = 0;
    uint64 nextTransaction_ = 1;
    GcReport lastGc_;
    // 표식 작업 큐(슬롯 번호)와 장벽 큐다. 장벽(삽입 장벽·새 루트·Marking 중 출생)은 slice 사이에
    // gcShade_에만 넣고 slice 시작에 gcWork_로 옮긴다(병렬 marker의 지역 큐와 섞이지 않는다). 둘 다
    // 슬롯 수만큼 용량을 미리 확보한다.
    Array<uint32> gcWork_;
    Array<uint32> gcShade_;
    // 병렬 표식의 512칸 큐 블록 재사용 목록이다(Registry 소멸 때 반환한다).
    GcBlock* gcFreeBlocks_ = nullptr;
    // 병렬 slice가 예산을 다해 남긴 회색 객체 블록이다(다음 slice의 씨앗).
    GcBlock* gcGrayBlocks_ = nullptr;
    // 앞 병렬 slice에서 표식이 멈춘 뒤 든 꼬리 시간(worker 회수·회색 넘기기, ns)이다. 다음 병렬
    // slice가 그만큼 표식을 일찍 멈춰 slice 전체가 시간 예산 안에 들게 한다.
    uint64 gcParallelTail_ = 0;
    // 클러스터(GC-S7): 켰는지, 레코드 표(번호로 색인 — 처리 표시 아래에서만 자란다), 빈 레코드
    // 목록과 수, Marking 중 해체해 큐가 빌 때까지 재사용하지 않는 레코드 목록, 표식 epoch다(Begin·
    // Collect마다 하나씩 오른다).
    bool clustersEnabled_ = false;
    // 미룬 장부 감소 기록(DeferLedgerRemovalLocked)이다. 첫 클러스터 예약 때 받는다.
    struct LedgerRemoval {
        uint32 owner;
        uint32 target;
        uint32 targetGeneration;
    };
    LedgerRemoval* ledgerRemovals_ = nullptr;
    uint32 ledgerRemovalCount_ = 0;
    ClusterRecord* clusters_ = nullptr;
    // 클러스터 표식 epoch다(레코드 번호로 색인, 레코드와 같은 용량 — marker가 간선마다 읽으므로
    // 레코드에서 떼어 작고 연속되게 둔다. 영구 영역은 언제나 지금 epoch다).
    uint32* clusterEpochs_ = nullptr;
    uint32 clusterCapacity_ = 0;
    uint32 clusterUsed_ = 0;
    uint32 clusterFree_ = NumericLimits<uint32>::Max();
    uint32 clusterFreeCount_ = 0;
    uint32 clusterDissolved_ = NumericLimits<uint32>::Max();
    uint32 gcEpoch_ = 0;
    // 영구 영역(13.7절): 창이 열렸는지와 영구 클러스터 번호(첫 영구 Commit에서 만든다)다.
    bool permanentOpen_ = false;
    uint32 permanentCluster_ = NumericLimits<uint32>::Max();
    usize gcCursor_ = 0;
    GcPhase gcPhase_ = GcPhase::Idle;
    GcReport cycleGc_;
    // 증분 sweep 위치: 남은 chunk 수다(다음은 sweepChunk_ - 1번 chunk — 새 chunk부터 옛 chunk로,
    // chunk 하나를 한 번에 분류한다). 0이면 끝났다.
    uint32 sweepChunk_ = 0;
    // 이름 색인: 개방 주소 표(2의 거듭제곱 크기, (live + tombstone) ≤ 1/2). 표 할당·반환은 잠금 밖.
    NameEntry* names_ = nullptr;
    uint32 nameCapacity_ = 0;
    uint32 nameLive_ = 0;
    uint32 nameTombstones_ = 0;
    // 잠금을 풀고 사용자 코드를 호출하는 구간에도 변경 재진입을 차단하는 상태다.
    // worker trace 실행 구간도 포함한다. phase 자체와 달리 slice 사이에는 false다.
    bool processing_ = false;
    bool closed_ = false;
    // 열린 묶음 게시 구간(BeginBatchPublish)이다. 열린 동안 processing_도 선다. 검사합은 간선
    // 변경마다 (added 해시 - removed 해시)의 합이다(선언과 게시가 같은지 End가 대조한다).
    ObjectHandle batchOwner_{};
    EdgeTally* batchTally_ = nullptr;
    uint64 batchDeclared_ = 0;
    uint64 batchApplied_ = 0;
    bool batchActive_ = false;
    bool batchPublished_ = false;
};

} // namespace rocks

#include <Object/ObjectRegistryContainers.h>
#include <Object/ObjectBatchPublish.h>
