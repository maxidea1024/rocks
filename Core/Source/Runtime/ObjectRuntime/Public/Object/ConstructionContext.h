#pragma once

#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

namespace rocks {

class PreparedStructElements;

// 소유 스레드에 묶인 비공개 생성 트랜잭션. 생성된 객체는 Commit 전 Resolve/TryPin할 수 없다.
// Commit하지 않고 범위를 벗어나면 생성 역순으로 롤백한다. 객체 포인터를 외부에 유출하지 않는다.
// Registry보다 먼저 파괴해야 한다. 현재 활성 트랜잭션이 있으면 Collect는 Busy다.
//
// 트랜잭션 상태 규칙(Create·Commit·DescribeOuter·FindObject 공통): 고정된 오류가 있으면 그 오류를
// 돌려준다 — 시작 실패(생성자의 접근 WrongThread·WrongContext, 종료 ShuttingDown, 재진입 Busy
// 등)와 실패한 Create의 오류이며, Rollback 뒤에도 남는다. 고정된 오류가 없고 끝난(Commit·Rollback)
// 트랜잭션이면 InvalidArgument다.
//
// Required 검사 중 표시(Docs/Design/ObjectFoundations.ko.md 4절): 그래프 로드가 적용 뒤·Commit 전에
// staged 객체의 실효 값을 encode 경로로 검사하는 동안(FieldEncodeContext::BeginRequiredCheck부터 그
// 문맥이 끝날 때까지) Create·SetReference·AssignReferences·AssignElements·SetElementReference·
// GetForWrite(등록된 생성 staged 쓰기 writeStaged와 그래프 적용의 쓰기가 이것으로 owner를 얻는다)·
// Commit은 Busy다(encode 고정의 staged판 — hook·allocator 콜백이 검사 중인 값을 바꾸지 못한다). 이
// Busy는 트랜잭션 상태를 바꾸지 않는다(오류로 고정하지 않는다). Get·DescribeOuter·FindObject 같은
// 읽기는 된다(Get이 준 포인터로 값을 바꾸는 것은 막지 못한다 — 콜백 계약이 1차 방어다). 표시 중의
// Rollback·소멸은 검사가 읽는 객체를 없애므로 Fatal이다(계약 위반).
class ROCKS_OBJECTRUNTIME_API ConstructionContext {
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

    // owner(이 트랜잭션의 공개 전 객체 — 방금 Create한 소유 객체)의 클래스 선언 subobject를 만들고
    // 배선한다(UE CreateDefaultSubobject 대응 — Docs/Design/Prefab 10.3절). SpawnActor·graph 복원·
    // prefab spawn이 모두 이 한 경로를 쓴다. 펼친 subobject 표 순서로:
    //   1. Create(표 타입, Outer = owner, 이름 = 소유 필드 도구 이름) — 생성 출처 ClassSubobject,
    //   2. 소유 필드 ← subobject, 소유 목록(SubobjectList) ← 표 순서로, Root 필드 ← Root subobject,
    //      subobject의 부착 부모(SubobjectAttach) ← 표의 부착 부모,
    //   3. owner 타입 base 사슬의 InitSubobjectDefaults를 가장 먼 base부터 부른다.
    // out(표 크기 이상)에 만든 handle을 표 순서로 쓰고 그 수(표가 없으면 0 — 아무것도 하지
    // 않는다)를 돌려준다. 클러스터 가입과 World 장부는 호출자가 한다.
    //
    // 오류(트랜잭션을 그 오류로 고정한다 — Commit 전이라 전부 되돌려진다): 트랜잭션 상태, owner가
    // 이 트랜잭션의 객체가 아님(StaleHandle 등 AcquireType 규칙), out이 모자람·소유 목록이 비어
    // 있지 않음·지정자 접근자가 없음(InvalidArgument), Create 오류(이름 충돌 AlreadyExists 등),
    // 참조 쓰기 오류, InitSubobjectDefaults가 돌려준 오류.
    //
    // 사용 예:
    //   auto actor = transaction.Create<Actor>(type);
    //   ObjectHandle parts[MaxClassSubobjects];
    //   auto made = transaction.CreateDeclaredSubobjects(actor.Value(), parts);
    //   if (!made) {
    //       return Result<void>::Failure(made.Error());
    //   }
    Result<usize> CreateDeclaredSubobjects(ObjectHandle owner, Span<ObjectHandle> out) noexcept;

    // outer를 Outer로 쓸 때의 요약이다(ObjectRegistry::DescribeOuter와 같고 이 트랜잭션의 공개 전
    // 객체도 받는다). 트랜잭션 상태(클래스 주석의 규칙) → outer가 null이면 InvalidArgument →
    // 생존·이 트랜잭션의 공개 전 객체가 아니면 StaleHandle이다.
    Result<OuterSummary> DescribeOuter(ObjectHandle outer) noexcept;

    // outermost 아래 객체 경로를 이 트랜잭션의 공개 전 객체와 공개된 객체 모두에서 푼다
    // (Docs/Design/ObjectFoundations.ko.md 5.4절 — A1 SCC 로드가 같은 트랜잭션에 올린 다른 패키지의
    // 객체를 import 대상으로 해석할 때 쓴다). 규칙은 ObjectRegistry::FindObject와 같고(빈 경로 =
    // 대표 자식, 대소문자·정규화 형태 무시, 조회 전에 모든 구성 요소의 형식 검사) 보이는 객체만
    // 넓힌다: outermost와 경로의 각 객체는 공개된 객체(Alive, RequestDestroy만 받은
    // DestroyRequested)이거나 이 트랜잭션의 공개 전 객체다. 다른 트랜잭션의 공개 전 객체는
    // outermost면 StaleHandle, 경로 구성 요소면 NotFound다(그 이름을 점유하지만 이 트랜잭션에는
    // 보이지 않는다). 읽기라 Required 검사 중 표시 동안에도 된다. 소유 스레드 전용이다. 결과
    // handle은 공개 전 객체일 수 있으므로 Commit 전에는 Resolve할 수 없고 이 트랜잭션의 참조
    // 대상(SetReference·그래프 imports)으로만 쓴다.
    //
    // 순서와 오류: 트랜잭션 상태(클래스 주석의 규칙) → 접근(WrongThread·WrongContext) → 종료·재진입
    // (ShuttingDown·Busy) → outermost(무효·다른 Registry·다른 트랜잭션의 공개 전 StaleHandle, GC
    // cutoff white·Retired Destroyed, Outer가 있으면 InvalidArgument) → 경로 형식(4 KiB 초과·빈
    // 구성 요소·이름 형식 위반 InvalidArgument, NFC 판정 할당 실패 OutOfMemory) → 비교 키 할당
    // (OutOfMemory) → 없는 구성 요소 NotFound.
    //
    // 사용 예(SCC 두 패키지를 한 트랜잭션에 올린 뒤 B의 import를 A의 객체로 해석):
    //   auto target = transaction.FindObject(packageA, "Body.Socket");
    //   if (!target) {
    //       return Result<void>::Failure(target.Error());   // NotFound면 MissingDependency
    //   }
    Result<ObjectHandle> FindObject(ObjectHandle outermost, StringView objectPath) noexcept;

    // outer 바로 아래 이름이 name인 객체를 이 트랜잭션의 공개 전 객체와 공개된 객체 모두에서 찾는다
    // (FindObject의 구성 요소 한 단계 — outer는 outermost가 아니어도 된다). A1 패키지 로드가
    // import를 앞서 해석한 outer import 아래에서 이름 하나로 풀 때 쓴다(경로 전체를 다시 걷지 않아
    // import 수에 비례한다). 빈 name은 outer의 대표 자식이다. 보이는 객체·이름 비교 규칙은
    // FindObject와 같다(다른 트랜잭션의 공개 전 객체는 outer면 StaleHandle, 자식이면 NotFound).
    // 읽기라 Required 검사 중 표시 동안에도 된다. 소유 스레드 전용이다. 결과 handle의 수명 규칙도
    // FindObject와 같다.
    //
    // 순서와 오류: 트랜잭션 상태(클래스 주석의 규칙) → 접근(WrongThread·WrongContext) → 종료·재진입
    // (ShuttingDown·Busy) → outer(무효·다른 Registry·다른 트랜잭션의 공개 전 StaleHandle, GC cutoff
    // white·Retired Destroyed) → 이름 형식(ValidateNameComponent 위반 InvalidArgument, NFC 판정
    // 할당 실패 OutOfMemory) → 비교 키 할당(OutOfMemory) → 없으면 NotFound.
    //
    // 사용 예(import "Gear.Tooth"를 앞서 푼 Gear 아래에서):
    //   auto tooth = transaction.FindChild(gear, "Tooth");
    //   if (!tooth) {
    //       return Result<void>::Failure(tooth.Error());   // NotFound면 MissingDependency
    //   }
    Result<ObjectHandle> FindChild(ObjectHandle outer, StringView name) noexcept;

    // object의 실제 등록 타입 lease다. 이 트랜잭션의 참조 대상으로 쓸 handle의 타입을 쓰기 전에
    // 확인할 때 쓴다 — 단계형 그래프 복원이 외부 참조 대상(imports — 생존 객체 또는 같은 트랜잭션에
    // 올린 SCC 형제의 객체)을 선언 타입·기대 형식과 대조한다(Docs/Design/ObjectFoundations.ko.md
    // 5.4절). 받는 객체는 ObjectRegistry::AcquireType이 아니라 staged 참조 쓰기의 대상 규칙을
    // 따른다: 생존(Alive, GC cutoff 아님) 또는 이 트랜잭션의 공개 전 객체(생성자 실행 뒤). 그래서
    // Registry AcquireType과 두 곳이 다르다 — 이 트랜잭션의 공개 전 객체를 받고(Registry는
    // StaleHandle), 파괴 요청(DestroyRequested)·cutoff 객체는 Destroyed가 아니라 StaleHandle이다
    // (staged 참조 대상이 될 수 없다. ConstructionContext::FindObject는 DestroyRequested 객체도
    // 찾으므로 그 결과를 import로 주면 여기서 StaleHandle이다). 읽기라 Required 검사 중 표시
    // 동안에도 된다. 소유 스레드 전용이다. lease는 타입 레코드 수명만 지키고 객체 수명은 지키지
    // 않는다.
    //
    // 순서와 오류: 트랜잭션 상태(클래스 주석의 규칙) → 접근(WrongThread·WrongContext) → 종료·재진입
    // (ShuttingDown·Busy) → object가 null·무효·다른 Registry·회수됨·DestroyRequested·cutoff·다른
    // 트랜잭션의 공개 전 객체면 StaleHandle.
    //
    // 사용 예(SCC 형제의 객체를 import로 쓰기 전에 타입을 본다):
    //   auto target = transaction.FindObject(packageA, "Body");
    //   auto type = target ? transaction.AcquireType(target.Value())
    //                      : Result<TypeLease>::Failure(target.Error());
    //   if (!type || !type.Value().IsNativeA(NativeTypeKey<Mesh>())) {
    //       return Result<void>::Failure(type ? ErrorCode::TypeMismatch : type.Error());
    //   }
    Result<TypeLease> AcquireType(ObjectHandle object) noexcept;

    // ---- 클러스터 지정(엔진 계층 전용 — World·AssetManager, Docs/Design/
    // GarbageCollectionScale.ko.md 13.3절) ----
    // 클러스터는 루트 객체 하나와 소속 객체 집합이다. 루트가 살면 소속이 모두 산다(루트 → 소속 암묵
    // 강한 간선)고 GC 표식은 소속 사이 간선을 따라가지 않는다. 가입은 이 트랜잭션의 Commit 때만
    // 하고 되돌릴 수 없다. 소속 객체는 RequestDestroy·Discard 때 빠지고(이탈), 루트의
    // RequestDestroy· Discard는 클러스터를 해체한다. 클러스터를 켠
    // Registry(RegistryOptions::clusters)만 받고 그 밖은 Unsupported다. 지정은 Commit 전까지 이
    // 트랜잭션에만 보이고 Rollback이 지운다.
    //
    // 공통 순서와 오류: 트랜잭션 상태 → 접근(WrongThread·WrongContext) → 종료·재진입(ShuttingDown·
    // Busy) → 클러스터를 끈 Registry Unsupported → object가 이 트랜잭션의 공개 전 객체(생성자 실행
    // 뒤)가 아니면 StaleHandle → 이미 지정했으면 InvalidArgument.

    // object를 새 클러스터의 루트로 지정한다. object 타입이 완전한 필드 배치
    // (RuntimeTypeOps::completeFieldLayout)가 아니면 Unsupported다.
    //
    // 사용 예(World가 Level을 만들 때):
    //   auto level = transaction.Create<Level>(LevelType);
    //   if (auto rooted = transaction.SetClusterRoot(level.Value()); !rooted) { return rooted; }
    Result<void> SetClusterRoot(ObjectHandle object) noexcept;

    // object를 root의 클러스터에 넣는다. root는 이 트랜잭션에서 SetClusterRoot로 지정한 객체이거나
    // 해체되지 않은 클러스터의 생존(Alive) 루트여야 한다(아니면 StaleHandle — Commit 때 다시 본다).
    // object 타입이 완전한 필드 배치가 아니면 가입하지 않고 성공한다(클러스터 밖 일반 객체로 남는다
    // — 정확성은 같고 최적화만 받지 못한다).
    //
    // 사용 예(SpawnActor):
    //   if (auto joined = transaction.JoinCluster(actor, level); !joined) { return joined; }
    Result<void> JoinCluster(ObjectHandle object, ObjectHandle root) noexcept;

    ObjectRegistry& Registry() const noexcept {
        return registry_;
    }

    const ObjectAccessContext& Access() const noexcept {
        return access_;
    }

    // 이 트랜잭션의 Registry가 객체 생성자에 주입하는 allocator(ObjectRegistry::ObjectAllocator)다
    // (리플렉션 B9e — Docs/Design/ReflectionExtensions.ko.md 1.7절). staged 쓰기와 그래프 복원
    // 2단계가 공개 전 객체의 빈 자리(unset Optional의 String·값 struct 등)에 새 값을 만들 때 쓴다.
    // 수명은 Registry와 같고(트랜잭션이 끝나도 유효하다 — rollback은 그 값을 담은 객체와 함께
    // 없앤다), 소유 스레드 계약은 다른 트랜잭션 API와 같다. 오류는 없다.
    //
    // 사용 예:
    //   String text(transaction.ObjectAllocator());
    Allocator& ObjectAllocator() const noexcept {
        return registry_.ObjectAllocator();
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

        ++writes_;
        return Get<T>(handle);
    }

    // staged 쓰기 접근(GetForWrite·참조·컨테이너 쓰기 판정)의 누계다. 값이 같으면 그 사이 이
    // 트랜잭션의 staged 객체에 쓰기가 없었다(경로 쓰기의 검증 상자 경로가 분리와 게시 사이 재진입
    // 변경을 잡는다 — Docs/Design/CurveData/README.ko.md 17.2절).
    uint64 WriteCount() const noexcept {
        return writes_;
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

    // 이 트랜잭션의 staged 객체 owner 안 ObjectRefMap 내용을 (keys[i], targets[i]) 항목으로 통째로
    // 바꾼다(리플렉션 B9f4 — Docs/Design/ReflectionExtensions.ko.md 7.4절, 그래프 로드가 한 번에
    // 채운다). 순서는 keys 순서다. owner·컨테이너 위치·값 대상 규칙은 AssignReferences와
    // 같다(null은 ObjectHandle{}로 정규화, 생존 객체 또는 이 트랜잭션의 Reserved 객체, 그 밖
    // StaleHandle, T의 파생이 아니면 TypeMismatch). keys와 targets의 길이가 다르거나 같은 키가
    // 둘이면 InvalidArgument, 컨테이너를 순회 중이면 Busy다. 키 사본(String·Soft는 컨테이너
    // allocator로 복사)과 저장소는 레지스트리 처리 표시(processing_) 아래 잠금 밖에서 새 컨테이너로
    // 만들고(그동안의 Registry 재진입은 Busy, 할당 실패는 그 오류) 한 잠금 안에서 게시한다. 옛
    // 항목은 호출이 돌아온 뒤 반환한다. 생존 대상만 shade하고 Reserved 대상은 Commit이 shade하며,
    // Commit은 owner Trace로 값을 다시 검증한다. 실패하면 컨테이너는 바뀌지 않는다. 트랜잭션이
    // 끝났거나 오류 상태면 InvalidArgument다.
    //
    // 키 형식은 map에서 정해진다(keys는 Span<const K>로 바뀌는 무엇이든 — 배열·Array의 View).
    //
    // 사용 예(owner·inventory는 같은 트랜잭션의 Create·Get 결과):
    //   const Name keys[] = {sword, shield};
    //   const ObjectHandle targets[] = {first, {}};
    //   if (auto assigned = transaction.AssignMapReferences(owner, inventory->slots_, keys,
    //   targets);
    //       !assigned) {
    //       return assigned;
    //   }
    template <class K, class T>
    Result<void> AssignMapReferences(ObjectHandle owner, ObjectRefMap<K, T>& map,
                                     Span<const TypeIdentityT<K>> keys,
                                     Span<const ObjectHandle> targets) noexcept {
        if (auto writable = CheckStagedWrite(); !writable) {
            return writable;
        }

        return registry_.AssignMapReferencesStaged(access_, owner, map, keys, targets,
                                                   transaction_);
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

    // 클래스 주석의 트랜잭션 상태 규칙을 판정한다: 고정된 오류가 있으면 그 오류, 고정된 오류 없이
    // 끝났으면(Commit·Rollback) InvalidArgument, 열려 있으면 성공이다. 상태를 바꾸지 않고
    // Registry를 부르지 않는다(Required 검사 중 표시와 무관하다). 단계형 그래프
    // 복원(ObjectGraphStage)처럼 트랜잭션을 받아 일하는 쪽이 일을 시작하기 전에 확인할 때 쓴다.
    //
    // 사용 예:
    //   if (auto open = transaction.CheckOpen(); !open) {
    //       return open; // 고정된 오류 또는 InvalidArgument
    //   }
    Result<void> CheckOpen() const noexcept;

    // 이 트랜잭션을 reason 오류로 고정한다(공개 전 객체를 지금 되돌리지 않고 공개만 막는다). 이후
    // Create·Commit·DescribeOuter·FindObject·CheckOpen은 그 오류를, staged 쓰기는 InvalidArgument를
    // 돌려주고, 객체는 Rollback 또는 소멸자가 생성 역순으로 되돌린다 — 같은 트랜잭션에 올린 다른
    // 단계(예: SCC의 다른 그래프)가 handle을 쥐고 있어도 지금 객체를 없애지 않으면서 "전부 공개
    // 또는 전부 rollback"을 보장한다. 단계형 그래프 복원이 객체를 만든 뒤 실패할 때
    // 부르고(ApplyFields· CheckRequired), 호출자가 자기 단계(import 해석·prepare 등)의 실패로
    // 트랜잭션을 버릴 때도 쓴다. Rollback과 달리 Required 검사 중 표시 동안에도 된다(값을 바꾸지
    // 않는다).
    //
    // 순서와 오류: reason이 None이면 InvalidArgument(아무것도 바꾸지 않는다) → 접근(WrongThread·
    // WrongContext — 소유 스레드 전용) → 이미 고정된 오류가 있으면 그 오류를 유지하고 성공(먼저 난
    // 오류가 원인이다) → 고정된 오류 없이 끝난(Commit·Rollback) 트랜잭션은
    // InvalidArgument(공개했거나 되돌린 뒤라 고정할 것이 없다) → 고정하고 성공.
    //
    // 사용 예(import 해석 실패로 SCC 트랜잭션 전체를 버린다):
    //   auto target = transaction.FindObject(packageA, path);
    //   if (!target) {
    //       (void)transaction.Fail(target.Error()); // 이후 Commit은 실패하고 소멸자가 되돌린다
    //       return Result<void>::Failure(target.Error());
    //   }
    Result<void> Fail(ErrorCode reason) noexcept;

private:
    friend class ObjectRegistry;
    // staged 배열 원소 지우기의 게시(리플렉션 B9i3c4)가 트랜잭션 문맥으로 압축한다.
    friend class PreparedStructElements;
    // staged 자리의 상자 게시가 staged 쓰기 판정(Required 검사 중 Busy)을 따른다.
    friend class PreparedAttach;
    // 검사 중 표시를 세우고 내린다(BeginRequiredCheck·문맥 소멸).
    friend class FieldEncodeContext;

    // staged 쓰기(SetReference·컨테이너 쓰기)의 공통 판정이다: 끝났거나 오류 상태면
    // InvalidArgument, Required 검사 중 표시 동안 Busy다.
    Result<void> CheckStagedWrite() const noexcept {
        if (finished_ || error_ != ErrorCode::None) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (requiredCheck_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        ++writes_;
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
    mutable uint64 writes_ = 0;
    // Required 검사 중 표시다(클래스 주석).
    bool requiredCheck_ = false;
};

} // namespace rocks
