#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefs.h>
#include <Object/ObjectDiagnostics.h>
#include <Object/StructRefArray.h>
#include <Object/TypeRegistry.h>

namespace rocks {

class WorkerScheduler;

// 객체 경로(Outer 사슬 이름을 '.'로 이은 것 — Docs/Design/ObjectFoundations.ko.md 1.1절)의 byte
// 상한이다(AS 5.1 문자열 상한). 경로가 이것을 넘는 객체는 만들 수 없다(CapacityExceeded). 이름이 1
// byte 이상이므로 Outer 깊이도 이것으로 묶인다(최대 2048).
inline constexpr usize MaxObjectPathBytes = 4096;

// Registry 생성 때 고정하는 객체 정책이다(Docs/Design/ObjectFoundations.ko.md 1.3절). 기본값은 정책
// 없음이다(World registry·시험 Registry). 에셋 객체 Registry(A1 AssetManager)는
// {packageNativeType = NativeTypeKey<Package>(), rejectWorldOnly = true}로 만든다.
//
// 판정은 생성(ConstructionContext::Create·NewObject)의 오류 순서 6·8단계다: WorldOnly는
// Unsupported, 패키지 규칙은 InvalidArgument다.
struct RegistryPolicy {
    // null이 아니면 패키지 정책이다: 이 native 타입(과 파생 — TypeLease::IsNativeA)의 객체는
    // Outer와 이름이 없어야 하고, 그 밖의 객체는 Outer가 있어야 하며 outermost가 이 타입이어야
    // 한다(AS 6.1 "패키지 없는 객체 생성은 거부한다"). TypeId가 아니라 native 키로 정하므로 TypeId
    // 별명도 같은 판정이다. 패키지 정책 Registry의 outer → inner GC 간선은 A0-Ob다.
    const void* packageNativeType = nullptr;
    // 참이면 WorldOnly 타입(TypeFlags::WorldOnly — 상속됨) 생성을 Unsupported로 거부한다(에셋 객체
    // Registry — RF 2.2·결정 21, AS 11.4의 AssetLease 누수 차단).
    bool rejectWorldOnly = false;

    bool operator==(const RegistryPolicy&) const noexcept = default;
};

// Outer로 쓸 객체의 요약이다(Docs/Design/ObjectFoundations.ko.md 1.2절). 생성 규칙(이름·대표 자식·
// 패키지·경로 길이)을 판정하는 입력이며, 그래프 1단계가 아직 없는 Outer를 기술할 때도 쓴다(A0-Od2).
// 조회 시점의 값이며 수명이나 접근권을 보존하지 않는다.
struct OuterSummary {
    ObjectHandle outermost{};        // Outer 사슬 끝(자기 자신이 outermost면 자기)
    bool isOutermost = false;        // 이 객체에 Outer가 없는지
    bool outermostIsPackage = false; // outermost가 정책의 패키지 타입인지(정책이 없으면 false)
    // Inner 유무는 이름 점유와 같은 기준이다(다른 트랜잭션의 Reserved는 세고, GC cutoff white는
    // 세지 않는다).
    bool hasRepresentative = false; // 대표 자식(빈 이름 Inner)이 있는지
    bool hasNamedInners = false;    // 이름 있는 Inner가 있는지
    uint32 pathBytes = 0;           // 이 객체의 객체 경로 byte 수

    // 아직 만들지 않은 outermost(예: 로더가 곧 만들 Package)의 요약이다. isPackage는 그 객체가
    // 정책의 패키지 타입인지다.
    static constexpr OuterSummary ForNewOutermost(bool isPackage) noexcept {
        OuterSummary summary;
        summary.isOutermost = true;
        summary.outermostIsPackage = isPackage;
        return summary;
    }
};

// CollectInners가 Inner를 모으는 순서다.
enum class InnerOrder : uint8 {
    Direct,    // 직계 Inner만(이름 순, 빈 이름이 먼저)
    DepthFirst // 전위 깊이 우선(각 Inner 바로 뒤에 그 부분 트리) — AS 5.1 export 순서
};

namespace detail {
class PathCursor;
// 시험 전용 내부 계수 통로다(정의는 ObjectRuntime/Private/ObjectRegistryAccess.h).
struct ObjectRegistryAccess;

// 경로 접근의 참조 멤버 쓰기 단일 지점이다(정의는 Object/GeneratedPathAccess.h).
template <class T>
Result<void> SetPathMemberReference(PathCursor& cursor, ObjectRef<T>& field,
                                    ObjectHandle target) noexcept;

// 새 객체 정체성 규칙 6–9(Docs/Design/ObjectFoundations.ko.md 1.5절)를 판정하는 순수 함수다.
// Registry 생성과 그래프 1단계·CreateObjects 사전 확인(A0-Od2)이 같이 써서 규칙이 갈라지지 않게
// 한다. 규칙 5(Outer 상태)는 호출자가 outer 요약을 만들 때 판정한다.
//
// 인자: policy는 Registry 정책(또는 그 사본), type은 만들 타입, outer는 Outer 요약(Outer가 없으면
// null), name은 이름(빈 문자열은 이름 없음 또는 대표 자식), scratch는 NFC 판정 임시 저장소다.
//
// 순서와 오류: 6. policy.rejectWorldOnly이고 WorldOnly 타입이면 Unsupported → 7. 이름 형식
// (ValidateNameComponent — InvalidArgument, scratch 할당 실패는 OutOfMemory) → 8. 패키지
// 정책(패키지 타입에 Outer·이름, 비패키지에 Outer 없음·outermost가 패키지 아님), 빈 이름(Outer가
// 있으면 대표 자식뿐 — Outer가 outermost이고 이름 있는 Inner가 없어야 한다), 대표 자식이 있는
// Outer의 이름 있는 자식은 InvalidArgument → 9. 새 객체 경로가 MaxObjectPathBytes를 넘으면
// CapacityExceeded. 같은 대표 자식 둘은 여기서 거부하지 않는다(이름 충돌 — 생성이 AlreadyExists로
// 낸다).
Result<void> CheckNewObjectIdentity(const RegistryPolicy& policy, const TypeLease& type,
                                    const OuterSummary* outer, StringView name,
                                    Allocator& scratch) noexcept;
} // namespace detail

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
    // 생성 뒤 바꾸지 않는 객체 정책이다(Policy()로 어느 스레드에서나 읽는다).
    RegistryPolicy policy;
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
    // 이름 색인 항목 수다(이름 있는 객체와 대표 자식 — Reserved 포함). Retire되거나, cutoff white
    // 객체의 항목을 같은 이름의 새 객체가 대신할 때 빠진다.
    uint32 namedObjects = 0;
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
//
// 객체는 생성 때 정한 불변 Outer와 이름을 가진다(Docs/Design/ObjectFoundations.ko.md 1절). 같은
// Outer 안에서 이름은 대소문자·정규화 형태를 무시하고 유일하며(TryMakeNameComponentKey 키),
// Registry가 이름 색인과 Outer별 자식 고리를 소유한다. inner → outer는 언제나 GC가 추적하는 강한
// 간선이고, 정책 없는 Registry에서 outer는 inner를 살리지 않는다.
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
    template <class T>
    friend Result<void> detail::SetPathMemberReference(detail::PathCursor&, ObjectRef<T>&,
                                                       ObjectHandle) noexcept;
    friend struct detail::ObjectRegistryAccess;

    struct Slot;
    struct Chunk;
    struct MarkState;
    struct NameRecord;
    struct NameEntry;
    struct InnerEntry;
    struct ReclaimWork;
    struct ChunkTable;

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

    // StructRefArray 원소 위치(detail::StructRefArrayAccess::Location)로 고르는 live 원소 참조
    // 쓰기다. 경로 접근(B6)의 참조 멤버 쓰기 단일 지점 detail::SetPathMemberReference만 friend로
    // 부른다 — 경로 thunk는 원소를 고른 단계의 배열 타입을 모르고 위치만 들고 내려온다. 위조한
    // 위치를 공개 API로 받지 않으려고 private이다. 검사(owner Alive와 Borrow, 컨테이너가 owner
    // 저장소 안, index < Size(), field가 그 원소 블록 안 — 아니면 InvalidArgument), 대상 규칙,
    // shade, encode 고정 Busy는 공개 (배열, index) 오버로드와 같다.
    template <class T>
    Result<void> SetElementReference(const ObjectAccessContext& access, ObjectHandle owner,
                                     const detail::StructElementLocation& element,
                                     ObjectRef<T>& field, ObjectHandle target) noexcept {
        return SetEdge(access, owner, field.handle_, target, 0, NativeTypeKey<T>(), &element);
    }

    Result<RootRef<Object>> NewRoot(const ObjectAccessContext&, TypeId, const void*,
                                    ObjectHandle outer, StringView name) noexcept;
    Result<uint64> BeginConstruction(const ObjectAccessContext&) noexcept;
    // 생성 오류 순서 1–12(Docs/Design/ObjectFoundations.ko.md 1.5절)를 따른다. 0단계(트랜잭션
    // 상태·저널 예약)는 ConstructionContext가 먼저 한다.
    Result<ObjectHandle> CreatePending(const ObjectAccessContext&, TypeId, uint64, const void*,
                                       ObjectHandle outer, StringView name) noexcept;
    Result<void*> GetPending(const ObjectAccessContext&, ObjectHandle, uint64,
                             const void*) noexcept;
    // ConstructionContext::Commit(roots, out)의 Registry 몫이다. roots는 이 트랜잭션 객체여야 하고
    // (StaleHandle), out은 roots와 크기가 같고 비어 있다(호출자가 확인한다).
    Result<void> Commit(const ObjectAccessContext&, uint64, Span<const ObjectHandle> staged,
                        Span<const ObjectHandle> roots, Span<RootRef<Object>> out) noexcept;
    void Rollback(uint64, Span<const ObjectHandle>) noexcept;
    // transaction이 0이 아니면 그 트랜잭션의 공개 전 객체도 받는다(ConstructionContext 판).
    Result<OuterSummary> DescribeOuterChecked(const ObjectAccessContext&, ObjectHandle outer,
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
    uint32 ReclaimEligible(uint32 maximum = 0xffffffffu, uint64 deadline = 0) noexcept;
    // 잠금 보유: Alive·DestroyRequested 슬롯을 Retired로 바꾸고 이름 색인·부모 고리에서 떼어 분류
    // 전 목록 끝에 붙인다(sweep).
    void RetireLocked(Slot&) noexcept;
    // 잠금 보유: 어느 목록에도 없는 Retired 슬롯을 queue 목록 끝에 붙인다.
    void LinkRetiredLocked(Slot&, RetiredQueue) noexcept;
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
    // scanNamedInners가 거짓이면 hasNamedInners를 구하지 않는다(자식 고리를 훑지 않는다 — 이름 있는
    // 객체 생성은 그 값을 쓰지 않는다).
    OuterSummary DescribeSlotLocked(const Slot&, bool scanNamedInners = true) const noexcept;
    // (outer, 비교 키)의 색인 항목 슬롯이다. published면 공개된 객체만, 아니면 점유 기준(Reserved
    // 포함)이다. cutoff white 항목은 건너뛴다. 없으면 nullptr.
    Slot* FindNamedLocked(ObjectHandle outer, uint64 hash, StringView key,
                          bool published) const noexcept;
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
    uint64 markEpoch_ = 0;
    GcReport lastGc_;
    Array<ObjectHandle> gcWork_;
    usize gcCursor_ = 0;
    GcPhase gcPhase_ = GcPhase::Idle;
    GcReport cycleGc_;
    // 증분 sweep 위치: 남은 chunk 수(다음은 sweepChunk_ - 1번 chunk — 새 chunk부터 옛 chunk로)와
    // 그 chunk 안 슬롯 위치다. 0이면 끝났다.
    uint32 sweepChunk_ = 0;
    uint32 sweepOffset_ = 0;
    // 이름 색인: 개방 주소 표(2의 거듭제곱 크기, (live + tombstone) ≤ 1/2). 표 할당·반환은 잠금 밖.
    NameEntry* names_ = nullptr;
    uint32 nameCapacity_ = 0;
    uint32 nameLive_ = 0;
    uint32 nameTombstones_ = 0;
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
