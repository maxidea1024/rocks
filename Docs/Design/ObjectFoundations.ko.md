# 객체·그래프 보강(A0-O) 설계

기준일: 2026-11-14, 확정안(설계 교차검증 2건 — 구현 가능성, 적대적 반례와 UE 5.6 대조 — 과 최종 검토 1건 반영, 사용자 승인 권고안으로 진행). [에셋 시스템 설계](AssetSystem.ko.md)(이하 AS) 1절 A0-O 단계의 상세이며 아직 구현 전이다(기준 코드 `5ee4dc5` — A0-B B1–B8 구현 완료).

근거: AS 1절 A0-O 행과 3·5·6·7·11절, [리플렉션·빌드 보강 설계](ReflectionFoundations.ko.md)(이하 RF)의 A0-O 언급(2.2·2.3·3.3·5.5·6.1–6.4절, 결정 16·21·22), [런타임 클래스 메타데이터](RuntimeClassMetadata.ko.md)(이하 RCM) '클래스 기본값 결정', [엔진 구현 계획](EngineImplementationPlan.ko.md)(이하 EIP) §5.2 "Outer와 객체 경로", 현재 코드(`Core/Source/Runtime/ObjectRuntime`·`ObjectSerialization`). UE 대조는 모두 `EpicGames/UnrealEngine` 5.6 브랜치 원본으로 확인했다(10절).

원칙(RF와 같다): 실패 원자성(읽기 실패는 출력 불변, 쓰기 실패는 미게시, 로드는 전부 공개 또는 전부 rollback), 결정성(같은 입력 → 같은 byte, 저장 집합은 GC 시점과 무관), 모든 길이·개수·작업량에 한도, 하위호환 없음(graph v1·`allowMissingFields`·단일 root Commit을 바로 바꾸고 시험을 고친다). 생성 코드(MetaCompiler 출력) 형식은 바꾸지 않는다 — 바뀌는 것은 라이브러리 템플릿(`GeneratedFieldCodec.h`)과 직렬화기다(RF 6.1 "A0-O·A1이 생성 코드를 다시 고치지 않도록").

## 0. 범위와 현재 상태

| 항목 | 현재(`5ee4dc5`) | A0-O 뒤 |
|---|---|---|
| Outer·Name | 없음(`Object`는 `registry_`·`handle_`만, Slot에도 없음) | 생성 때 정하는 불변 Outer·이름, Registry의 이름 색인·자식 고리, 경로 조회, inner → outer GC 간선 |
| 패키지 소속 보존 | — | 패키지 정책 Registry에서 outer → inner도 GC 강한 간선(자식 고리가 export 표다 — AS 6.1 `Package.exports` 목록을 없앤다) |
| 파괴 | `RequestDestroy`(이름 개념 없음) | + `Discard`(부분 트리 논리 파괴 + 이름 색인·자식 고리에서 즉시 제거, 메모리는 GC). 패키지 정책 Registry는 Discard만(RequestDestroy Unsupported) |
| Registry 정책 | `requireLifecycleScope`만 | `RegistryPolicy`(패키지 정책·WorldOnly 거부) |
| 패키지 사이 간선 | 관찰 없음 | `ReferenceObserver`(순 다중도 합산 표 — 편집·Commit·Discard) |
| Commit | 첫 staged 객체 하나에 RootRef(`ObjectRegistry.cpp` 1663행) | 호출자가 고른 root마다 RootRef(다중 root), 공개 객체의 패키지 사이 간선 다중도 보고 |
| 누락 필드 | 정책(`FieldDecodePolicy::allowMissingFields`·`allowMissingFlags`, 기본 NotFound) | 형식 규칙: 빠진 필드·멤버 = 기준 값(NotFound 없음). present 판정 구조는 A5 override 집합용으로 남긴다 |
| Required 진단(로드) | 1단계에서 파일 값·누락으로 판정(누락은 null로 간주) | 적용 뒤(Commit 전) staged 객체의 실효 값으로 판정 |
| 그래프 형식 | v1: 단일 root, BFS local ID, U64 참조, 외곽에 타입·버전, 고아 거부, 내부 Commit | v2: 호출자 저장 집합·순서 = localId, U32 참조 + 외부 참조 비트, 노드 타입은 노드 표(export 표) 기준, 단계형 복원(호출자 Commit), 외부 참조 목록·번호 재배정 |
| 한도 | 객체 1024, 바이트 16 MiB, 작업량 64Mi | 객체 4096, 외부 참조 65536, 바이트 64 MiB, 작업량 식(5.6절) |

이 단계가 하지 않는 것: Rename·Outer 변경(EIP §5.2가 허용하는 조건부 이동 — 11절 결정 2), SubobjectGuid·default subobject(prefab 단계), 객체 단위 EditorOnly(Exports 표 플래그 — RF 12절 열린 문제, A1), 클래스 기본값 레코드·차이 쓰기·override 집합(A5 — 자리만 남긴다, 4절), Soft 참조 대상 해석(A1), 패키지 파일 형식과 노드별 (offset, size) 표(A1), 상주 단위·union-find(A1 AssetManager), World registry의 외부 생성 거부(EngineRuntime 4절 "정책 자리" — 1.3절 `worldOnlyCreationKey` 후속), 고유 이름 생성·이름 정제 도우미(A2 — 1.2절 자리), Discard의 되돌리기·참조 정리(A2·CU4 — 11절 열린 결정 N5), Instanced 하위 객체 배열(UE Instanced 대응 — A1 또는 prefab 단계).

## 1. Outer·Name·Registry 정책 (A0-Oa)

### 1.1 규칙

- **이름 형식**: 이름은 AS 4.2절 경로 구성 요소 규칙을 따른다 — UTF-8 NFC, 1–64 byte, 허용 문자 General_Category L\*·N\*·M\*와 `_`·`-`(첫 문자는 L\*·N\*), Default_Ignorable_Code_Point 거부, Windows 예약 이름(대소문자 무시 `CON PRN AUX NUL COM0-9 LPT0-9 COM¹²³ LPT¹²³`) 거부. 따라서 `.`·`/`가 들어갈 수 없다(AS 4.1). Registry는 정규화하지 않고 검사만 한다(비 NFC 입력은 InvalidArgument — 조회 결과가 입력과 달라지지 않게). 정규화는 입력 계층(A2 경로 정책·명령행)이 한다.
- **비교 키**: 같은 Outer 안 유일성과 조회는 `TryMakeCaselessKey`(AS 4.2 — NFC(simple case folding(NFD(x))))로 비교한다. 키 생성은 정규화 버퍼(Normalizer segment)를 할당하므로 조회 API도 OutOfMemory를 돌려줄 수 있다 — ASCII만으로 된 이름은 할당 없는 빠른 경로(ASCII 소문자화)로 만든다(같은 결과임을 시험으로 고정). 정렬은 원래 이름의 NFC UTF-8 byte 순이다(AS 4.2). Core `Name`(NamePool)은 쓰지 않는다 — ASCII만 대소문자를 무시하고 `None`·번호 접미 규칙이 있어 AS 4.2와 맞지 않는다(Docs/Implementation/Name.ko.md).
- **빈 이름**: (a) Outer가 없는 객체는 빈 이름이어도 된다(지금 모든 객체가 이렇다 — 이름 색인에 넣지 않는다). (b) Outer가 있는 객체의 빈 이름은 **대표 자식**뿐이다: 그 Outer는 Outer가 없는 객체(outermost)여야 하고, 대표 자식이 있는 Outer는 다른 자식을 가질 수 없으며 그 반대도 같다. 패키지 정책에서는 대표 자식이 대표 에셋이다(AS 4.1·5.1 "export 1만 빈 이름·outerIndex 0"). (c) 그 밖의 빈 이름은 InvalidArgument다. 이 규칙의 "Inner가 있다"는 이름 점유와 같은 기준이다 — 다른 트랜잭션의 Reserved도 세고, cutoff white·Discard된 객체는 세지 않는다.
- **유일성**: (Outer, 비교 키)가 같은 객체는 하나다. Outer가 없는 이름 있는 객체는 최상위 이름 공간에서 유일하다. 클래스는 키가 아니다(UE도 같다 — 10절). 공개 전(Reserved) 객체도 생성 때부터 이름을 점유하므로 동시에 열린 두 트랜잭션이 같은 이름을 만들 수 없다(뒤의 Create가 AlreadyExists). GC cutoff white(Unreachable) 객체는 이름을 점유하지 않는다 — 충돌 검사에서 만나면 그 항목을 그 자리에서 뺀다(UE의 기본 조회가 Unreachable을 빼는 것과 같은 효과).
- **객체 경로**: outermost 아래에서 그 객체까지의 이름을 `.`로 이은 것이고 대표 자식의 빈 이름은 건너뛴다(AS 4.1 예: 대표 에셋 아래 `Body` 아래 `Socket` = `Body.Socket`). outermost와 대표 자식의 경로는 빈 문자열이다(SoftObjectRef의 빈 경로 = 대표 에셋, RF 3.3). 경로는 4 KiB 이하다(AS 5.1 문자열 상한) — 넘는 객체는 만들 수 없다(CapacityExceeded). 따라서 Outer 깊이도 경로로 묶인다(최대 2048).
- **불변**: Outer와 이름은 생성 때 정하고 바꾸지 않는다(Rename은 11절 결정 2 — UE는 대소문자만 바꾸는 Rename을 허용하지만 Rocks는 이름 자체가 불변이다). 그래서 소속 패키지(outermost)도 객체 수명 동안 고정이다 — `ReferenceObserver`(3절)가 이것에 기댄다.
- **GC**: inner → outer는 언제나 GC가 추적하는 강한 간선이다(EIP §5.2, UE도 같다 — 10절). outer → inner는 정책 없는 Registry에서는 간선이 아니고(EIP §5.2 "outer가 inner를 암묵적으로 살리지 않는다"), 패키지 정책 Registry에서는 강한 간선이다(1.7절 — 결정 8).
- **패키지 소속**: 객체의 소속 패키지는 outermost다(UE의 external package 같은 별도 소속은 없다). 패키지 정책에서 패키지(outermost)는 이름을 갖지 않는다(이름이 있으면 InvalidArgument) — 정체성은 AssetId(A1 `Package` 필드)이고 패키지 경로는 Registry 밖(AS 4.2 파일 위치)에 있다.

### 1.2 공개 API

Core(`Core/Text/NameComponent.h`, 할당은 NFC 검사 scratch뿐):

```cpp
inline constexpr usize MaxNameComponentBytes = 64;

enum class NameComponentError : uint8 {
    None, Empty, TooLong, NotNfc, InvalidFirst, InvalidCharacter, DefaultIgnorable, ReservedDeviceName };

// AS 4.2 경로 구성 요소 규칙을 검사한다(객체 이름과 A2 논리 경로 구성 요소가 같이 쓴다).
// 위반은 InvalidArgument이고 reason에 사유, offset에 위반 위치(byte)를 채운다. NFC 판정 scratch 할당
// 실패는 OutOfMemory다.
//
// 사용 예:
//   NameComponentError reason{};
//   if (auto valid = ValidateNameComponent(name, scratch, &reason); !valid) {
//       // 도구 계층이 reason으로 InvalidPath 진단을 만든다
//   }
Result<void> ValidateNameComponent(StringView component, Allocator& scratch,
                                   NameComponentError* reason = nullptr,
                                   usize* offset = nullptr) noexcept;
```

ObjectRuntime(`Object/ObjectRegistry.h`·`ConstructionContext.h` — 모두 소유 스레드 전용, 접근 검사 순서는 기존 API와 같다):

```cpp
// Outer로 쓸 객체의 요약이다(그래프 1단계가 아직 없는 Outer를 기술할 때도 쓴다 — 5.4절).
struct OuterSummary {
    ObjectHandle outermost{};         // Outer 사슬 끝(자기 자신이 outermost면 자기)
    bool isOutermost = false;         // 이 객체에 Outer가 없는지
    bool outermostIsPackage = false;  // outermost가 정책의 패키지 타입인지(정책이 없으면 false)
    // Inner 유무는 이름 점유와 같은 기준이다(다른 트랜잭션의 Reserved는 세고, cutoff white·Discard된
    // 객체는 세지 않는다).
    bool hasRepresentative = false;   // 대표 자식(빈 이름 Inner)이 있는지
    bool hasNamedInners = false;      // 이름 있는 Inner가 있는지
    uint32 pathBytes = 0;             // 이 객체의 객체 경로 byte 수

    // 아직 만들지 않은 outermost(예: 로더가 곧 만들 Package)의 요약이다.
    static constexpr OuterSummary ForNewOutermost(bool isPackage) noexcept;
};

enum class InnerOrder : uint8 {
    Direct,      // 직계 Inner만
    DepthFirst   // 전위 깊이 우선(각 Inner 바로 뒤에 그 부분 트리) — AS 5.1 export 순서
};

class ConstructionContext {
public:
    // outer는 Outer(없으면 ObjectHandle{}), name은 이름(빈 문자열은 이름 없음 또는 대표 자식)이다.
    // outer는 생존 객체이거나 이 트랜잭션의 공개 전 객체여야 한다.
    template <class T>
    Result<ObjectHandle> Create(TypeId type, ObjectHandle outer = {}, StringView name = {}) noexcept;

    // 공개 전 객체도 받는 DescribeOuter다(이 트랜잭션의 Reserved 객체 또는 생존 객체).
    Result<OuterSummary> DescribeOuter(ObjectHandle outer) noexcept;

    ObjectRegistry& Registry() const noexcept;
    const ObjectAccessContext& Access() const noexcept;
    // 이 트랜잭션의 ID다(관찰자 Commit 기록의 transaction과 같다 — 3절). 끝난 트랜잭션은 0이다.
    uint64 TransactionId() const noexcept;
    // Commit은 1.6절, FindObject는 5.4절(A0-Od2)
};

class ObjectRegistry {
public:
    template <class T>
    Result<RootRef<T>> NewObject(const ObjectAccessContext&, TypeId, ObjectHandle outer,
                                 StringView name) noexcept;   // 기존 NewObject(access, type)도 남는다

    // Alive·DestroyRequested 객체에 쓸 수 있다(논리 정체성은 회수 전까지 남는다).
    Result<ObjectHandle> GetOuter(const ObjectAccessContext&, ObjectHandle) const noexcept;     // 없으면 ObjectHandle{}
    Result<ObjectHandle> GetOutermost(const ObjectAccessContext&, ObjectHandle) const noexcept;
    // 잠금 안에서 64 byte 스택 버퍼로 복사한 뒤 잠금 밖에서 out(그 allocator)에 붙인다 — 할당 콜백이
    // Registry에 재진입해 그 객체를 회수해도 해제된 이름을 읽지 않는다.
    Result<void> GetName(const ObjectAccessContext&, ObjectHandle, String& out) const noexcept;
    // 같은 방식으로 4 KiB 스택 버퍼에 경로를 만든 뒤 붙인다(경로 상한이 4 KiB라 넘치지 않는다).
    Result<void> GetObjectPath(const ObjectAccessContext&, ObjectHandle, String& out) const noexcept;

    // 이름 색인 조회다. 공개된 객체(Alive, RequestDestroy만 받은 DestroyRequested)만 돌려준다 —
    // Reserved·Discard된 객체·cutoff white는 NotFound다. outer가 null이면 최상위 이름 공간이다.
    // 비교 키 생성 할당 실패는 OutOfMemory다.
    Result<ObjectHandle> FindChild(const ObjectAccessContext&, ObjectHandle outer,
                                   StringView name) const noexcept;
    // outermost 아래 객체 경로(1.1절)를 푼다. 빈 경로는 대표 자식이다. 경로는 4 KiB 이하.
    //
    // 사용 예(A1 Soft 참조 해석):
    //   auto target = registry.FindObject(access, package, softRef.objectPath.View());
    //   if (!target) { /* NotFound면 MissingDependency 진단 */ }
    Result<ObjectHandle> FindObject(const ObjectAccessContext&, ObjectHandle outermost,
                                    StringView objectPath) const noexcept;
    // Create가 AlreadyExists가 될지다(Reserved도 점유로 본다). 형식 오류는 InvalidArgument.
    Result<bool> IsNameTaken(const ObjectAccessContext&, ObjectHandle outer,
                             StringView name) const noexcept;
    Result<OuterSummary> DescribeOuter(const ObjectAccessContext&, ObjectHandle outer) const noexcept;

    // outer의 Inner를 이름 순(빈 이름이 먼저)으로 out 끝에 붙인다. Alive 객체만(공개 전·파괴 요청·
    // cutoff 제외). out은 호출자 allocator로 늘린다. 실패하면 out은 호출 전 길이로 돌아간다.
    //
    // 사용 예(A1 writer의 export 순서):
    //   Array<ObjectHandle> exports(allocator);
    //   if (auto collected = registry.CollectInners(access, package, InnerOrder::DepthFirst, exports);
    //       !collected) {
    //       return collected;
    //   }
    Result<void> CollectInners(const ObjectAccessContext&, ObjectHandle outer, InnerOrder,
                               Array<ObjectHandle>& out) noexcept;
};
```

- **`CollectInners` 순서**: (1) 잠금 안에서 부분 트리(Direct면 직계) 개수 n을 센다 → (2) `processing_`을 세우고 잠금 밖에서 `out.TryReserve(기존 + n)`과 보조 배열(n × `{Slot*, uint32 부모 위치, uint32 자식 시작}`)을 할당한다(그 사이 재진입 변경은 Busy) → (3) 재잠금해 보조 배열을 큐로 써서 수준 순서(같은 Outer의 자식이 연속)로 `Slot*`를 모으고, 개수가 늘었으면 계약 위반으로 Busy → (4) 잠금 밖에서 같은 Outer 구간마다 `NameRecord`로 제자리 정렬(`Slot*`로 비교 — `FindSlot`의 선형 탐색 없음) → (5) Direct는 그대로, DepthFirst는 부모 위치·자식 시작으로 스택 없이 전위 순서를 만들어 out에 handle로 붙인다 → `processing_`을 내리고 보조 배열을 반환한다. 정렬·순서 구간에는 할당·사용자 콜백이 없으므로 `NameRecord`가 회수될 수 없다(회수는 소유 스레드의 명시 호출뿐이다).
- **이름 도우미 자리(A2)**: `Result<void> MakeUniqueName(const ObjectAccessContext&, ObjectHandle outer, StringView base, String& out)`(UE `MakeUniqueObjectName` 대응 — `base`, `base_1`, …)와 표시 이름 정제(허용 문자만 남기기)는 A2 임포트·복제가 필요할 때 이 헤더에 더한다. A0-O는 이름만 정한다.

### 1.3 Registry 정책(타입·규칙은 A0-Oa, 관찰자 관련 규칙은 A0-Ob)

```cpp
struct RegistryPolicy {
    // null이 아니면 패키지 정책이다: 이 native 타입(과 파생 — TypeLease::IsNativeA)의 객체는 Outer와
    // 이름이 없어야 하고, 그 밖의 객체는 Outer가 있어야 하며 outermost가 이 타입이어야 한다(AS 6.1
    // "패키지 없는 객체 생성은 거부한다"). 이 Registry에서는 outer → inner도 GC 간선이다(1.7절).
    // TypeId가 아니라 native 키로 정한다(TypeId 별명도 같은 판정).
    const void* packageNativeType = nullptr;
    // WorldOnly 타입(TypeFlags::WorldOnly, 상속됨) 생성을 Unsupported로 거부한다(에셋 객체 Registry —
    // RF 2.2·결정 21, AS 11.4의 AssetLease 누수 차단).
    bool rejectWorldOnly = false;

    bool operator==(const RegistryPolicy&) const noexcept = default;
};

struct RegistryOptions {
    // 기존 칸(maxSlots·maxGeneration·requireLifecycleScope)은 그대로다.
    RegistryPolicy policy;
    // 붙이면 3절 규칙으로 패키지 사이 강한 간선 변화를 기록한다(A0-Ob). 패키지 정책 Registry에만 붙고,
    // Registry보다 오래 살아야 하며 Registry 하나에만 붙는다(정책 없는 Registry에 주거나 이미 붙은
    // 관찰자를 주면 Registry 생성이 Fatal — 생성자가 실패를 돌려줄 수 없다).
    ReferenceObserver* referenceObserver = nullptr;
    // 패키지 정책 Registry에서 Alive 소속 객체가 Discard 없이 회수되면 Debug에서 assert한다(1.7절 —
    // 그 경로를 시험하는 Registry만 false로 둔다. 계수는 언제나 센다).
    bool assertRetiredWithoutDiscard = true;
};

const RegistryPolicy& ObjectRegistry::Policy() const noexcept;   // 생성 뒤 불변 — 어느 스레드나
```

- 판정 위치: `CreatePending`(1.5절 5·8단계), 그래프 1단계 노드 검사(5.4절 — 정책 사본으로), 그래프 `CreateObjects`의 사전 확인(실제 Registry 정책과 사본이 같은지 — 다르면 WrongContext).
- 정책 검사는 `NewObject`(NewRoot)에도 그대로 적용된다 — 패키지 정책 Registry에서 Outer 없는 `NewObject`는 이름 없는 패키지 타입만 된다.
- AssetManager(A1)는 `{packageNativeType = NativeTypeKey<Package>(), rejectWorldOnly = true}`와 관찰자로 에셋 객체 Registry를 만든다. World registry와 시험 Registry는 기본값(정책 없음)이다.
- 패키지 정책 Registry의 `RequestDestroy`·`Object::Destroy`는 관찰자 유무와 무관하게 Unsupported다(A0-Ob, 11절 결정 1) — "소속 객체는 Discard로만 죽는다"(1.7절)가 정책 전체의 불변식이다. 관찰자는 패키지 정책 Registry에만 붙는다.
- **후속 자리(World 정책)**: `RegistryPolicy::worldOnlyCreationKey`(불투명 키 — World API만 가진 키를 Create에 함께 줄 때만 WorldOnly 타입 생성을 허용)로 EngineRuntime 4절 134행 "World registry의 외부 생성 거부"를 구현한다. A0-O는 이름과 뜻만 정하고 구현은 World 정책 단계다(EngineRuntime 134행을 이 절로 연결 — 12절).

### 1.4 자료구조(`ObjectRegistry.cpp` 내부)

- `Slot`에 더한다: `ObjectHandle outer`(생성 때 고정), `Slot* outermost`(자기면 자기), `Slot* parent·firstChild·nextSibling·prevSibling`, `NameRecord* name`(이름이 있거나 대표 자식이면), `uint16 pathBytes`, 상태 비트 `indexed`(이름 색인에 있음)·`linked`(부모 고리에 있음)·`discarded`. 고리는 슬롯 번호가 아니라 `Slot*`다 — chunk는 Registry 소멸 전까지 이동·반환되지 않으므로 포인터가 안정하고, 슬롯 번호로 이으면 `SlotAt`의 chunk 선형 탐색(92–101행) 때문에 순회가 O(n·chunks)가 된다.
- 불변식: Reclaiming·Free 슬롯의 `outer`·`outermost`·고리 칸은 읽지 않는다. Retired 슬롯은 자기를 부모 고리에서 이미 뗐지만, Retired 부모의 `firstChild`는 그 자식들이 스스로 뗄 때까지 유효하다(아래 회수 조건). 트랜잭션 저널·sweep·회수만 슬롯 상태로 걸러 접근한다.
- `NameRecord`: 할당 하나에 `{uint64 keyHash; uint16 nameBytes; uint16 keyBytes; char data[]}`(NFC 이름 뒤에 비교 키). Create가 잠금 밖에서 만들고(`processing_` 아래 — 지금 `CreatePending`의 할당 방식), 회수(`ReclaimEligible`)가 잠금 밖에서 반환한다. 잠금 안에서는 할당·반환하지 않는다(지금 "User destructors and allocator callbacks never execute under the registry gate" 규칙).
- 이름 색인: 개방 주소 표, 항목 `{Slot* slot; uint64 keyHash}`(rehash가 슬롯을 읽지 않는다), 2의 거듭제곱 크기, 적재율 (live + tombstone) ≤ 1/2, 삭제는 tombstone. 키는 (outer handle 전체 — domain·slot·generation, keyHash, 비교 키 byte)이며 비교는 슬롯의 `outer`·`NameRecord`로 한다. 성장과 tombstone 정리는 Create의 잠금 밖 할당 구간에서 새 표(live가 1/4를 넘으면 두 배, 아니면 같은 크기 rebuild)를 받아 잠금 안에서 옮긴다(옛 표 반환은 잠금 밖). Outer handle에 generation이 있으므로 회수된 Outer를 가리키는 옛 항목이 새 객체와 섞이지 않는다.
- 자식 고리: 이중 연결(잠금 안, 무할당). 생성 때 부모 고리 끝에 붙고(Reserved 포함), Discard·Retire 때 자기 자신을 뗀다(O(1)). 고리 해체는 하지 않는다: white 부모에 연결된 자식은 모두 white라(살아 있는 자식은 inner → outer로 부모를 표식한다) 같은 sweep에서 Retire되며 스스로 떨어지고, rollback은 생성 역순이라 자식이 먼저다. 그 사이 부모 슬롯이 재사용되지 않게 회수 조건에 `firstChild == nullptr`를 더한다(`ReclaimEligible` 2161행, `HasReclaimableLocked` 1851행 — 그래서 sweep이 끝나기 전의 `DrainRetired`·rollback 회수(2228·1695행)도 부모를 회수하지 않는다). `processed`는 지금처럼 슬롯당 1이다(`GcBudget` 주석은 바꾸지 않는다).
- 통계: `ObjectStats`에 `namedObjects`(색인 항목 수)를 더한다(패키지 정책 Registry의 `retiredWithoutDiscard`는 1.7절).
- 종료: `Shutdown`의 회수가 `NameRecord`를 모두 반환하고, 소멸자가 색인 표를 반환한다.

### 1.5 GC: inner → outer 간선과 오류 순서

- **inner → outer**: Registry가 직접 방문한다(타입 Trace가 아니다) — 객체를 trace하는 모든 곳, `Collect`(2114행), `StepCollection`의 순차·병렬 경로(1937·1966행), `Commit`의 staged 검증(1637행)에서 `ops.trace` 뒤에 `visitor.Visit(slot->outer)`를 부른다. `outer`는 공개 전에 정해져 바뀌지 않으므로 worker trace가 읽어도 경합이 없다.
- 삽입 장벽: Marking 중 Commit은 새 객체를 shade하고 그 trace가 outer를 방문한다. Sweeping 중 생성은 cutoff white Outer를 StaleHandle로 거부하고(Create), Commit 검증(`ValidateStagedEdge`)이 Outer도 대상처럼 검사한다(Alive 또는 같은 트랜잭션 Reserved, cutoff 아님 — 아니면 StaleHandle).
- Retire(sweep — 2017·2130행, rollback — 1692행): 같은 잠금 안에서 이름 색인 항목과 자기 고리를 뗀다(1.4절). Discard로 이미 뗀 객체는 건너뛴다.
- Registry의 Outer·자식 방문은 `gate_`를 쥐지 않고 한다 — `MarkEdge`(1711행)·`ValidateStagedEdge`(1597행)가 같은 `gate_`를 잡으므로 쥔 채 부르면 자기 교착이다. 그래도 안전한 이유는 GC 슬라이스·Commit 검증이 `processing_` 아래라 그동안 `outer`·고리가 바뀌지 않기 때문이다.

오류 순서(Create·NewObject): 0. 트랜잭션 상태(끝난 트랜잭션 InvalidArgument, 고정된 오류는 그 오류)·저널 예약(OutOfMemory) → 1. 접근(WrongThread·WrongContext) → 2. 종료·재진입(ShuttingDown·Busy) → 3. 타입(NotFound·TypeMismatch) → 4. Abstract·생성 함수 없음(Unsupported) → 5. Outer 상태(StaleHandle — 생존·같은 트랜잭션 Reserved가 아님, 다른 Registry의 handle(FindSlot의 domain 불일치), cutoff white, Discard됨) → 6. WorldOnly 정책(Unsupported) → 7. 이름 형식(InvalidArgument, NFC 판정 scratch OutOfMemory) → 8. 패키지 정책·패키지 이름·빈 이름·대표 자식 규칙(InvalidArgument) → 9. 경로 길이(CapacityExceeded) → 10. 비교 키·이름 기록·색인 성장 할당(OutOfMemory) → 11. 이름 충돌(AlreadyExists) → 12. 객체 할당(OutOfMemory)·슬롯 상한(CapacityExceeded). 지금처럼 Create 실패는 트랜잭션을 오류 상태로 고정한다(도구는 `IsNameTaken`으로 먼저 확인한다). WrongContext는 접근 문맥 불일치에서만 나온다.

규칙 6–9는 순수 함수 하나로 둔다: `detail::CheckNewObjectIdentity(const RegistryPolicy&, const TypeLease& type, const OuterSummary* outer, StringView name, Allocator& scratch) -> Result<void>`. 규칙 5는 호출자가 Outer 요약을 만들 때 판정한다(Registry는 슬롯 상태로, 그래프 1단계는 노드 표로). Create와 그래프 1단계·`CreateObjects` 사전 확인(5.4절)이 같이 쓴다 — 규칙이 갈라지지 않게.

### 1.6 다중 root Commit

```cpp
class ConstructionContext {
public:
    // roots의 객체마다 RootRef를 out의 같은 위치에 하나씩 돌려준다(out.Size() == roots.Size()이고
    // out의 원소는 비어 있어야 한다 — 아니면 InvalidArgument). roots는 이 트랜잭션의 객체여야 한다
    // (아니면 StaleHandle). 같은 handle을 두 번 주면 RootRef도 둘이다. roots가 비어도 된다 — 공개한
    // 객체는 다른 생존 객체(패키지 정책이면 생존 Outer)가 붙잡지 않으면 다음 GC에서 회수된다.
    // 순서: 트랜잭션 상태·roots·out 검사(InvalidArgument·StaleHandle) → staged 간선·Outer 검증과 관찰자
    // 쌍 집계(잠금 밖, processing_ 아래) → 관찰자 예약 → 한 잠금 안에서 공개·root 증가·관찰자 기록. 공개 전 단계의 실패는 비공개 상태를 유지한다(호출자가
    // Rollback하거나 소멸자가 한다).
    //
    // 사용 예(패키지 둘을 한 트랜잭션으로 공개 — 하드 순환 SCC):
    //   const ObjectHandle roots[] = {packageA, packageB};
    //   RootRef<Object> held[2];
    //   if (auto committed = transaction.Commit(roots, held); !committed) {
    //       return committed;
    //   }
    Result<void> Commit(Span<const ObjectHandle> roots, Span<RootRef<Object>> out) noexcept;

    // 첫 Create 객체 하나를 root로 공개한다(지금의 Commit과 같다).
    Result<RootRef<Object>> Commit() noexcept;
};
```

- Registry 내부 `Commit(access, transaction, staged, roots, out)`이 지금의 `Commit`(1606행)을 대신한다. root 계수 증가는 공개와 같은 잠금 안이다(지금 `++FindSlot(staged[0])->roots`).
- 이름 색인 항목은 Create 때 이미 들어 있으므로 Commit은 상태만 바꾼다(조회가 Alive부터 보게 된다).
- 관찰자 집계는 Oa에서 자리만 둔다(Commit 내부 단계와 실패 지점 — 집계 맵 삽입은 gate 밖). 기록은 Ob가 채운다. 집계는 발견 순(staged 저널 순 × trace 순)으로 `Add`한다(handle 해시 순 금지 — 3절).

### 1.7 패키지 소속 보존 — outer → inner 강한 간선 (A0-Ob, 결정 8)

- 패키지 정책 Registry에서는 객체를 trace할 때 Registry가 inner → outer와 함께 **자식 고리의 모든 자식**도 방문한다(같은 지점 — 1.5절의 세 곳). 자식 고리가 곧 export 표이므로 AS 6.1의 `Package.exports`(`ObjectRefArray<Object>`) 목록을 없앤다. 정책 없는 Registry는 지금처럼 outer → inner가 간선이 아니다(EIP §5.2 그대로 — 정정 문구는 12절).
- 병렬 안전: `StepCollection` 슬라이스는 `processing_` 아래라 슬라이스 동안 Create·Discard·고리 변경이 Busy다. 그래서 worker trace가 `gate_` 없이 고리를 읽고 자식마다 `visitor.Visit(handle)`을 불러도 경합·교착이 없다(1.5절). 열린 ConstructionContext가 있으면 GC가 진행하지 않으므로 Reserved 자식을 만나지 않는다.
- 장벽: 슬라이스 사이에 생긴 자식은 새 객체이고 Commit이 shade한다(Marking) 또는 epoch를 찍는다(Sweeping). 슬라이스 사이 Discard로 고리에서 빠진 자식은 다른 참조가 없으면 회수된다(의도한 동작).
- 결과: 패키지가 살아 있으면(AssetManager RootRef, 또는 어느 소속 객체든 외부에서 참조되면 inner → outer → 패키지 → 자손) 모든 소속 객체가 산다. 패키지 소속 객체는 **Discard로만 죽는다**. 저장 집합(`CollectInners`)이 GC 시점과 무관해진다.
- 지킴: 패키지 정책 Registry에서 Alive 소속 객체가 Discard 없이 Retire되면 `ObjectStats::retiredWithoutDiscard`를 올리고, `RegistryOptions::assertRetiredWithoutDiscard`(기본 true)면 Debug에서 assert한다(값은 언제나 0이어야 한다 — AssetManager가 Discard 없이 패키지 RootRef를 놓은 버그). 이 경로의 시험은 그 칸을 false로 둔 Registry에서 계수만 본다. 종료(`closed_`) 뒤의 회수는 세지 않는다.
- 근거: 지금 API로는 공개된 Package가 같은 트랜잭션 밖의 Reserved 하위 객체를 목록에 넣을 수 없고(`CheckTargetLocked`·`SetEdge`의 owner 검사 — `ObjectRegistry.cpp` 383–398·436–442행), 목록 편집은 패키지 소유 간선이라 관찰되지 않아(3절 구조 규칙) 계수가 새며, 목록에 빠진 소속 객체는 GC 시점에 따라 저장 집합에서 사라진다. UE는 패키지가 export를 붙잡지 않는다(에디터는 GC된 export가 있는 패키지를 다시 로드한다 — 10절) — Rocks는 저장 결정성을 위해 다르게 정한다.
- 비용: 소속 객체마다 부모 고리에서 한 번 방문된다(전체 O(객체 수)). 비정책 Registry에는 비용이 없다.

## 2. Discard (A0-Ob)

```cpp
class ObjectRegistry {
public:
    // handle과 Outer 사슬이 handle을 지나는 모든 객체(부분 트리)를 논리 파괴(DestroyRequested)하고
    // 이름 색인·자식 고리에서 즉시 뺀다. 이후 FindChild·FindObject·CollectInners는 이 객체들을 보지
    // 못하고 같은 이름의 새 객체를 바로 만들 수 있다. 패키지 정책이면 고리에서 빠졌으므로 패키지가 더
    // 붙잡지 않는다. 저장소는 남은 RootRef·Borrow·간선이 풀린 뒤 GC가 회수한다(Resolve·WeakRef::TryPin은
    // Destroyed). 관찰자가 있으면 부분 트리에서 다른 패키지로 나가는 강한 간선 다중도를 Discard 원인의
    // 음수 변화로 한 번 기록한다(3절). 이미 Discard된 객체는 성공(무동작). RequestDestroy만 받은 객체도
    // 받는다(이름을 이때 뗀다). 되돌릴 수 없다(같은 객체를 다시 공개하는 경로가 없다 — 11절 N5).
    //
    // 사용 예(AssetLoader::Pump의 Closing 처리 — AS 11.4):
    //   for (ObjectHandle package : unit.packages) {
    //       if (auto discarded = registry.Discard(access, package); !discarded) {
    //           return discarded;   // Busy면 RootRef를 쥔 채 다음 Pump에서 다시(Discard한 패키지는 무동작)
    //       }
    //   }
    //
    //   unit.roots.Clear();   // 모두 Discard한 뒤에만 RootRef를 놓는다(Busy면 쥔 채 다음 Pump)
    Result<void> Discard(const ObjectAccessContext&, ObjectHandle) noexcept;
};
```

- 오류 순서: 접근 → 종료·재진입(ShuttingDown·Busy) → handle(StaleHandle — 무효·다른 Registry·Reserved, Destroyed — cutoff white·Retired) → 부분 트리에 공개 전(Reserved) 객체가 있으면 Busy(그 트랜잭션의 Commit이 Outer 검증으로 실패할 상태를 만들지 않게) → 할당(부분 트리 목록·간선 집계·관찰자 예약 — OutOfMemory) → 적용.
- 단계: (1) 잠금 안에서 부분 트리 슬롯을 센다 → (2) `processing_` 아래 잠금 밖에서 목록을 할당하고, 관찰자가 있으면 부분 트리 객체마다 타입 Trace로 패키지 밖 강한 간선을 쌍별로 집계하고(Outer가 있는 소유자만 — 3절) 관찰자 공간을 예약한다 → (3) 한 잠금 안에서 상태 변경·색인 해제·고리 해제·관찰자 기록. `processing_` 동안 다른 변경은 Busy이므로 (1)과 (3) 사이에 부분 트리가 바뀌지 않는다.
- 음수 보고는 trace가 본 간선 값 전부다 — 대상의 GC 색과 무관하다. 덧붙여, Sweeping 중 cutoff가 아닌 소유자의 대상은 표식 불변식과 삽입 장벽(cutoff 대상 연결 거부) 때문에 cutoff white일 수 없고, 1.7절 보존으로 생존 패키지의 소속 객체는 white가 되지 않는다(시험으로 확인).
- 부분 트리 안의 순서는 의미가 없다(같은 잠금에서 한 번에 바뀐다). 부분 트리 밖 같은 패키지 객체에서 안으로 오는 간선은 같은 패키지라 계수에 없다. 다른 패키지에서 안으로 오는 간선은 AssetManager의 불변식(보유 수 0일 때만 Discard — AS 11.4)이 막는다(Registry는 검사하지 않는다).
- 패키지를 다시 로드할 때 이름 충돌은 원래 없다: 패키지는 이름이 없고 하위 이름은 새 패키지 handle 아래다. Discard의 즉시 이름 해제가 필요한 곳은 살아 있는 패키지 안의 부분 Discard(도구가 하위 객체를 지우고 같은 이름으로 다시 만들 때)와 조회 차단이다(AS 11.4 문구 정정 — 12절).
- GC와의 관계: DestroyRequested 객체는 지금처럼 도달하면 trace된다(남은 참조가 가리키는 저장소를 지킨다). Discard는 GC 단계와 무관하게 할 수 있다(상태 비트·고리만 바꾼다).
- **순서(AssetManager)**: Pump는 단위의 패키지마다 Discard(부분 트리)하고, 모두 성공한 뒤에 RootRef를 놓는다. Discard가 Busy면 RootRef를 쥔 채 다음 Pump에서 다시 한다. RootRef를 먼저 놓으면 그 사이 GC 조각이 Discard 없이 소속 객체를 회수해 관찰자 계수가 새고 `retiredWithoutDiscard`가 오른다(1.7절).
- 남은 참조: Discard된 객체를 남은 참조가 붙잡으면 inner → outer 간선으로 그 Outer 사슬(패키지 객체까지)도 DestroyRequested로 남는다. 형제는 고리에서 빠졌으므로 남지 않는다.
- `ObjectLifecycle`은 그대로다(Discard된 객체도 DestroyRequested).

## 3. ReferenceObserver (A0-Ob)

AS 6.1의 계약을 구체화한다. 관찰자는 Registry가 쓰는 표이고(가상 콜백이 아니다), AssetManager가 Pump에서 꺼낸다 — 잠금 안에서 사용자 코드를 부르지 않는다. 같은 (소유 패키지, 대상 패키지, 원인, 트랜잭션) 키의 변화는 순 다중도로 합산하므로 Pump 사이 편집이 많아도 크기가 키 수(`maxKeys`)로 묶인다.

```cpp
enum class ReferenceChangeCause : uint8 { Edit, Commit, Discard };

// 패키지 사이 강한 간선 다중도의 순 변화 하나다. 패키지는 outermost 객체다.
struct ReferenceEdgeDelta {
    ObjectHandle ownerPackage{};
    ObjectHandle targetPackage{};
    int64 delta = 0;      // 합산한 순 변화(0인 키는 꺼낼 때 빠진다)
    ReferenceChangeCause cause = ReferenceChangeCause::Edit;
    uint64 transaction = 0;   // Commit이면 ConstructionContext::TransactionId(), Edit·Discard는 0
};

class ReferenceObserver {
public:
    // maxKeys는 TakeChanges 사이에 둘 수 있는 키 수 상한이다. 새 키가 이것을 넘게 되는 기록 지점은
    // CapacityExceeded이고 아무것도 바뀌지 않는다(0이 된 키도 TakeChanges 전까지 센다).
    explicit ReferenceObserver(Allocator&, usize maxKeys = 1u << 20) noexcept;
    ~ReferenceObserver();   // 붙은 Registry가 남아 있으면 Fatal
    ReferenceObserver(const ReferenceObserver&) = delete;
    ReferenceObserver& operator=(const ReferenceObserver&) = delete;

    // 쌓인 변화를 out과 맞바꾼다. 0이 된 항목은 맞바꾸기 전에 제자리에서 지우고, 순서는 키가 처음 생긴
    // 순서다(같은 작업 순서면 같은 결과 — 결정적). 이 순서는 시간 순서가 아니다: 한 키의 합 안에 앞뒤
    // 변화가 섞여 있으므로 소비자는 묶음 전체를 더한 뒤에만 다중도·Closing을 판정한다(중간 합은 음수일
    // 수 있다). out은 비어 있고 관찰자와 같은 allocator여야 한다(아니면 InvalidArgument). 할당하지
    // 않는다. 붙은 Registry의 소유 스레드 전용이고(아니면 WrongThread) Registry가 예약 중(할당 콜백
    // 재진입)이면 Busy다.
    //
    // 사용 예(AssetLoader::Pump — Closing 처리 전에):
    //   Array<ReferenceEdgeDelta> changes(allocator);
    //   if (auto taken = observer.TakeChanges(changes); !taken) { return taken; }
    //   for (const ReferenceEdgeDelta& change : changes) {
    //       // (소유 패키지, 대상 패키지) 다중도 갱신, Commit이면 transaction으로 로드 작업의 내부 보유를
    //       // 실제 간선 다중도로 바꾸고, 단위 사이 순환이면 union-find로 합친다
    //   }
    Result<void> TakeChanges(Array<ReferenceEdgeDelta>& out) noexcept;
    usize PendingCount() const noexcept;   // 붙은 Registry의 소유 스레드 전용

private:
    friend class ObjectRegistry;
    // 잠금 밖 — 새 키 newKeys개를 받을 공간(합산 표 색인 + 항목 배열)을 반환 시점에 보장한다. 새 표·배열을
    // 모두 할당한 뒤 옮기며, 그 할당 콜백 중 Add로 옛 표에 들어간 항목도 옮긴다. 할당 콜백 안에서 다시
    // 부르면(중첩 예약) Busy, maxKeys를 넘으면 CapacityExceeded다.
    Result<void> Reserve(usize newKeys) noexcept;
    // 잠금 안 — 무할당. 키가 있으면 더하고, 없으면 예약한 공간에 넣는다.
    void Add(ObjectHandle owner, ObjectHandle target, int64 delta, ReferenceChangeCause,
             uint64 transaction) noexcept;
    // 잠금 안 — 지금 남은 새 키 공간이다(재검증용).
    usize FreeKeys() const noexcept;
};
```

**기록 규칙**

- 관찰 대상: 소유자가 Alive이고 **Outer를 가진**(= 패키지 자신이 아닌) 객체이며, 소유자의 outermost P와 대상의 outermost Q가 다른 강한 간선(`ObjectRef`·`ObjectRefArray` 원소·`StructRefArray` 원소의 참조 멤버). 패키지 객체가 소유한 간선(AS 6.1의 Package 하드 의존 목록 — AssetManager가 직접 유지하는 사본)은 이 구조 규칙으로 빠진다(11절 결정 10). Outer 간선과 1.7절의 outer → inner 방문은 같은 패키지라 기록하지 않는다.
- 편집(Edit, transaction 0): `SetEdge`(live `SetReference`·`SetElementReference`·경로 쓰기) — 옛 대상 X → 새 대상 Y면 (P, pkg(X), −1)과 (P, pkg(Y), +1)이고 옛 대상 키를 먼저 더한다. pkg(X)와 pkg(Y)가 같으면 기록·예약하지 않는다. `InsertArrayEdge` +1, `ReplaceArrayEdge` −1/+1, `RemoveArrayEdge` −1, `ClearArrayEdges`는 쌍별로 먼저 합친 뒤 기록한다. `RemoveStructElement`·`ClearStructElements`는 **떼어 낸 원소만** trace한다: `detail::StructElementOps`에 `const void* nativeType`을 더해 `StructElementOpsOf<S>::Value`에서 `&NativeTypeIdentity<S>`로 채우고(`NativeTypeKey`는 constexpr가 아니다 — `Object.h` 82–89행. 같은 주소라 TypeRegistry의 struct native 키와 맞는다. trace 칸은 생성 헤더 없는 곳의 인스턴스화 문제로 넣지 않는다 — `StructRefArray.h` 19–29행), Registry가 새 `TypeRegistry::AcquireStructByNative(const void*)`로 `RuntimeStructOps::trace`를 얻어 원소 블록을 trace한다. 이 조회는 아래 다중 간선 규칙의 `processing_` 구간에서 한다. 조회 실패는 관찰자 Registry에서만 Unsupported다. `InsertStructElement`·`AddElement`는 새 원소의 참조가 null이라 없다.
- 공개(Commit, transaction = 그 트랜잭션): 공개하는 객체들의 간선을 쌍별로 합쳐 +n(1.6절 — staged 검증 trace에서 같이 센다). 대상은 같은 트랜잭션 객체이거나 이미 생존한 객체다. 키는 발견 순(staged 저널 순 × trace 순)으로 더한다.
- 파괴(Discard, transaction 0): 부분 트리에서 나가는 간선을 쌍별로 합쳐 −n(2절), 키는 부분 트리 목록 순 × trace 순으로 더한다. 이것이 AS 6.1·11.4의 "Discard가 나가는 계수를 한 번만 반납한다"이다 — AssetManager가 따로 빼지 않는다. 집계에 handle 해시 순서를 쓰지 않는다(결정성).
- 관찰하지 않는 것: 공개 전 객체의 staged 쓰기(Commit이 한꺼번에 보고), DestroyRequested 소유자의 제거·비우기(Discard가 이미 반납했다), 같은 패키지 안 간선.
- **예약 규칙 — 단일 간선 지점**(`SetEdge` live 쓰기, `InsertArrayEdge`, `ReplaceArrayEdge`, `RemoveArrayEdge` — 새 키 ≤ 2): 지금 컨테이너 성장 규칙(`InsertArrayEdge` 542–620행)을 따른다 — 잠금 → owner·대상 검증과 필요한 새 키 수 계산(옛·새 대상 패키지가 같으면 0) → `FreeKeys()`가 모자라면 잠금을 풀고 `Reserve`(배열 버퍼 성장도 같은 구간) → 재잠금해 다시 검증·계산 → 여전히 모자라면 Busy → shade가 성공한 뒤 게시와 `Add`(옛 대상 키 먼저). `processing_`을 세우지 않는다.
- **예약 규칙 — 다중 간선 지점**(`ClearArrayEdges`, `RemoveStructElement`, `ClearStructElements`): Commit·Discard와 같은 규칙이다 — 잠금 안에서 owner·위치를 검증한 뒤 `processing_`을 세운다 → 잠금 밖에서 떼어 낼 값의 쌍을 집계한다(`ObjectRefArray`는 handle을 읽고, struct 원소는 `AcquireStructByNative`의 `RuntimeStructOps::trace`로 원소 블록을 trace, 대상 패키지 조회는 간선마다 잠금, 집계 표 삽입은 잠금 밖) → `Reserve` → 한 잠금 안에서 떼기·`Add`·`processing_` 내림 → 잠금 밖에서 원소 소멸. `processing_` 동안 다른 변경이 Busy라 재검증이 필요 없다(관찰자 Registry에서는 이 구간에 다른 스레드의 RootRef 복제도 Busy다).
- 예약·집계 할당 실패는 그 변경의 OutOfMemory, `maxKeys` 초과는 CapacityExceeded이고 아무것도 바뀌지 않는다. 관찰자 없는 Registry는 지금 경로 그대로다.
- 지킴: 1.7절 `retiredWithoutDiscard`.

## 4. 누락 = 기준·실효 값 Required 진단 (A0-Oc)

RCM 결정 22를 그래프 형식 규칙으로 고정한다. 레코드가 없어도 성립하므로 A0-O가 처음부터 갖고, A5는 형식 버전을 다시 올리지 않는다.

**누락 = 기준**

- Record에 없는 영속 필드·struct 멤버는 오류가 아니며 그 위치의 값은 **기준 값**이다. A0-O의 기준은 생성 직후 값이다: 최상위 필드와 struct 필드의 멤버는 소유 객체 생성자가 만든 값(struct는 제자리 decode — `DecodeStructRecord`가 생성자 값 위에 있는 멤버만 쓴다), 배열 원소 struct의 멤버는 원소 기본 생성자(`S(Allocator&)`·`S()`) 값, 빠진 배열은 생성자가 만든 배열이다. 이는 지금의 "누락 필드는 생성자 값 유지"(`allowMissingFields`)와 같은 값이다. A5 뒤에도 C++ 타입은 생성자가 레코드와 같은 값을 만들므로 같고, Sparks 타입은 초기값 덮기 뒤의 값이 기준이다(RCM 적용 순서).
- 지우는 것: `FieldDecodePolicy::allowMissingFields`·`allowMissingFlags`, `FieldDecodeContext::IsMissingAllowed`, `detail::CheckMissingField`·`ReportMissingRequired`, `DecodeNode`의 누락 순회(`GraphLoad.cpp` 846–858행)와 `DecodeStructRecord`의 누락 순회(`GeneratedFieldCodec.h` 957–969행). NotFound는 그래프 로드에서 사라진다.
- **남기는 것 — override 집합 자리**: present 판정(`DecodeNode`의 `present` 배열, `DecodeStructRecord`의 present bitset — `GeneratedFieldCodec.h` 921·954행)은 지우지 않는다. 1단계가 필드·멤버마다 "파일에 있음"을 판정하는 지점을 문맥의 보호 가상 함수 `FieldDecodeContext::NotePresent(const FieldCodecFrame&)`(A0-O 기본 무동작)로 모아 둔다. A5는 이것으로 차이 형식 로드의 override 집합("파일에 있는 값 경로 = 집합" — RCM)을 채우고(공개 hook `FieldCodecHooks::ObservePresentValue` 또는 Stage 출력 중 A5가 정한다), 형식은 바뀌지 않는다. 덧붙여 정한다: (b) 전부 저장(undo·스냅샷·복제)의 override 집합은 graph v2 바이트 밖 — 그 컨테이너가 노드별 값 경로 목록으로 담는다. (c) "차이 형식인지"는 형식 비트가 아니라 호출자 옵션이다(같은 바이트 규칙 — 누락 = 기준 — 으로 둘 다 읽힌다). 그래서 "A5에서 형식 버전을 다시 올리지 않는다"(RCM)가 유지된다.
- 새 정책 칸 `FieldDecodePolicy::excludedFlags`(저장의 `FieldEncodePolicy::excludedFlags`와 짝): 이 데이터에 없어야 하는 유효 플래그다(쿠킹 로드 = EditorOnly). 그런 필드·멤버·원소가 있으면 InvalidArgument(쿠킹 데이터는 정확히 일치 — AS 5.3), 없으면 기준 값이며 Required 진단에서도 뺀다. RF 2.3의 "쿠킹 데이터 로드에서 EditorOnly 필드 누락은 NotFound가 아니다"는 이것으로 성립한다.

**실효 값 Required 진단(2c 단계)**

- Required 진단은 적용 뒤, Commit 전에 staged 객체의 실효 값으로 낸다. 파일 값이든 기준 값이든 같은 경로다 — 1단계는 Required를 판정하지 않는다(`ReportParsedValue`의 Required 보고와 `FieldDecodeContext::ReportIfRequiredNull`을 지운다). 빠졌다고 null로 보지 않으므로 생성자 기본값이 non-null인 SoftObjectRef는 누락이어도 진단하지 않고, C++ 생성자가 강한 참조를 설정할 수 없으므로(RCM "구조적으로 null") 빠진 Required `ObjectRef`는 진단한다.
- 구현은 encode 경로를 재사용한다(11절 결정 11): 검사 문맥 `RequiredCheckContext : FieldEncodeContext`(직렬화기 내부)가 Required를 품은 최상위 필드(필드 자신이 Required이거나, struct 필드·struct 원소 배열의 struct가 `ContainsRequired`)만 staged 객체에서 encode하고 출력은 버린다. `FieldEncodeContext`에 검사 모드를 더한다:

```cpp
class FieldEncodeContext : public FieldCodecContext {
public:
    // 실효 값 검사 문맥인지다. 이 모드에서는 Required null을 거부하지 않고 RequiredMissing으로 보고하며
    // (정책과 무관), 필드별 MaxElements(ArrayTooLong)·SubstituteStruct hook·Soft 수집을 하지 않는다.
    bool IsRequiredCheck() const noexcept;
    // staged 객체를 읽는 문맥이면 그 트랜잭션이다. 생성 encode thunk(EncodeGeneratedField)는 이것이
    // 있으면 Resolve 대신 ConstructionContext::Get<Owner>로 소유자를 얻는다(encode 고정을 쥐지 않는다 —
    // 아래 검사 중 표시가 대신한다).
    ConstructionContext* StagedSource() const noexcept;

protected:
    // staged 트랜잭션에 검사 중 표시를 세운다(문맥이 끝날 때 내린다). 표시 동안 그 트랜잭션의 Create·
    // SetReference·AssignReferences·AssignElements·SetElementReference·Commit은 Busy다 — encode 고정의
    // staged판이라 hook·allocator 콜백이 검사 중인 값을 바꾸지 못한다.
    void BeginRequiredCheck(ConstructionContext& staged) noexcept;
};
```

- `CheckRequired(isNull)`: Required이고 null이면 검사 모드는 `Report(RequiredMissing)` 뒤 성공, 아니면 지금처럼 `rejectMissingRequired`일 때 보고 뒤 InvalidArgument. 검사 모드의 `Report` hook은 Borrow 없이 불린다(staged 객체는 Borrow를 얻을 수 없다 — 대신 검사 중 표시가 값을 지킨다).
- 검사 문맥의 `WriteReferenceWire`는 대상을 검사하지 않고 자리값만 쓴다(대상은 같은 트랜잭션 객체이거나 이미 검증한 import). `IsExcluded`는 로드 정책의 `excludedFlags`와 Transient를 쓴다.
- 출력: 필드마다 같은 writer를 `Clear`해 다시 쓰고 크기 한도는 두지 않는다(값 한도·그래프 바이트 한도는 1단계가 파일 값에 걸었고, 생성자 값은 신뢰 상태다). 작업량도 무제한이다(2단계처럼 예산으로 실패하지 않는다 — 실패는 할당·계약 위반뿐이며 호출자가 rollback). 이 버퍼의 피크는 가장 큰 Required 품은 필드 하나다(11절 결정 4의 피크 문서화에 넣는다).
- 수동 함수표(`detail::EncodeThroughFieldValue`)는 `read`가 Registry Resolve를 쓰므로 staged 객체를 읽지 못한다. `RuntimeFieldOps` 끝에 선택 칸 `Result<void> (*readStaged)(ConstructionContext&, ObjectHandle, FieldValue&) noexcept = nullptr`를 더하고, 검사 모드에서 그것이 없으면 Unsupported다(Required를 품은 수동 필드에만 해당 — 11절 결정 9). 생성 코드는 바꾸지 않는다(생성 thunk는 멤버 포인터로 직접 읽는다).
- 진단 경로·`objectIndex`는 encode 프레임으로 만든다(배열은 `[f[i]]`, struct는 멤버 경로). `objectIndex`는 노드 위치(0부터) = localId − 1 = export index − 1이다(12절 14번).
- hook 계약 개정: "복원의 진단·수집은 1단계에서 한 번만 온다" → "RequiredMissing은 2c에서 한 번, 그 밖의 진단·Soft 수집은 1단계에서 한 번". Soft 수집은 파일 값만이다(A1 Dependencies 대조가 파일 내용과 맞다 — 기준 값의 Soft 참조를 Dependencies에 넣는 것은 RCM의 L2 이후 일).
- A0-Oc는 이 규칙을 지금의 graph v1 envelope 위에서 먼저 넣는다(형식 변경과 규칙 변경을 따로 검증하려고). `LoadObjectGraph`의 단계는 1단계 → 생성 → 적용 → Required 검사 → Commit이 된다.

## 5. graph v2 (A0-Od1·A0-Od2·A0-Oe)

### 5.1 형식

모든 정수 little-endian, Guid·문자열·Bytes는 Archive 규칙이다(정수는 고정 폭 — `Archive.h`).

```
offset size 필드
0      8    magic "ROCKSGRF"
8      4    formatVersion = 2           (그 밖은 Unsupported — v1은 읽지 않는다)
12     4    flags: NodeTable(1) | BaseOuter(2)   (BaseOuter는 NodeTable과만, 정의되지 않은 비트는 InvalidArgument)
16     4    nodeCount (1..maxObjects)
20     4    externalCount (0..maxExternalReferences)
24     —    [NodeTable] nodeCount × { Guid type, u32 outer, String name }
              outer: 0 = 그래프 밖(BaseOuter 비트가 있으면 기준 Outer, 없으면 Outer 없음),
                     1..자기 번호−1 = 노드 k
       —    nodeCount × Bytes(Record)   Record = ROCKSREC(schema = 노드 타입, version = 저장 당시 스키마 버전)
끝     —    남는 byte 없음
```

- **노드 타입의 원천**: NodeTable이 없으면(A1 패키지 Objects 섹션) 복원 호출자가 노드 표(`GraphNodeDesc` — Exports에서 만든다)를 주고, NodeTable이 있으면(독립 그래프 — 시험·스냅샷·복제·B8 Level 구조 시험) 그것을 쓴다. 둘 다 있거나 둘 다 없으면 InvalidArgument. Record의 schema ID는 노드 타입과 같아야 한다(InvalidArgument). 외곽에 타입·버전을 따로 쓰지 않는다(AS 5.1 "객체 타입의 원천은 Exports.typeId 하나").
- **outer 0의 뜻**: 노드 표의 outer 0은 "그래프 밖"이다 — 기준 Outer가 있으면(NodeTable의 BaseOuter 비트, 또는 Stage 입력의 `hasBase`) 기준 Outer, 없으면 Outer 없음. AS 5.1의 Exports `outerIndex` 0(= 패키지)과 같은 값이라 A1이 그대로 넘긴다. 한 그래프 안에서 "Outer 없음"과 "기준 Outer"를 섞을 수 없다(저장이 InvalidArgument).
- **참조 값(강한 참조, wire U32 — `ReferenceWireType()`)**: 0 = null, 1..nodeCount = 노드, `0x80000000 | i`(1 ≤ i ≤ externalCount) = 외부 참조 i. 그 밖(nodeCount 초과, 0x80000000, i > externalCount)은 InvalidArgument(A1이 CorruptPackage로 바꾼다). `ObjectRefArray` 원소도 ValueArray의 U32다. 고정 4 byte라 외부 번호 재배정(5.3절)이 제자리 덮어쓰기다.
- **고아 허용**: v1의 "root에서 도달하지 않는 노드 거부"를 없앤다 — 저장 집합은 참조 순회가 아니라 호출자가 정한다(AS 5.1 "export 집합은 Outer 표에서 정한다"). 편의 복원만 도달 검사를 유지한다(5.5절).
- 노드별 (offset, size) 표는 두지 않는다 — 노드 단위 지연 로드·부분 해석이 필요한지는 A1이 정한다.

### 5.2 저장 API (A0-Od1)

```cpp
enum class GraphExternalPolicy : uint8 {
    Reject,   // 저장 집합 밖 참조는 ExternalReference 진단 뒤 InvalidArgument(기본)
    Admit,    // hooks->AdmitExternalReference가 허락하면 외부 참조로 쓴다(A0-Oe)
    Include   // 저장 집합 끝에 노드로 더한다(발견 순 BFS — 편의 SaveObjectGraph, v1과 같은 순서)
};

struct ObjectGraphSaveOptions {
    ObjectGraphLimits limits;
    FieldEncodePolicy policy;              // rejectMissingRequired·excludedFlags(쿠킹 = EditorOnly)
    FieldCodecHooks* hooks = nullptr;      // 진단·Soft 수집·struct 치환(bulkId 재매핑 — B4 구현)·외부 참조 허락
    GraphExternalPolicy externals = GraphExternalPolicy::Reject;
    bool writeNodeTable = false;
};

// 외부 참조 대상 하나다(slot 순서 = 첫 출현 순).
struct GraphExternal {
    ObjectHandle target{};
    TypeId type{};             // 대상의 실제 등록 타입
    uint32 occurrences = 0;
    uint32 commonFlags = ~0u;  // 모든 출현 위치 유효 플래그의 AND(EditorOnly로만 쓰였는지 — A1 Dependencies 종류)
    bool required = false;     // Required 위치에서 한 번이라도 쓰였는지(A1 Dependencies.required)
};

// 저장 결과다. 바이트·외부 참조 목록·노드 목록을 소유하고, 외부 참조 값의 위치를 기억해 번호를
// 다시 매길 수 있다. 결과는 Registry를 참조하지 않는다(handle 값만 담는다).
class ObjectGraphSaveResult {
public:
    Span<const uint8> Bytes() const noexcept;
    Array<uint8> TakeBytes() noexcept;                       // 이후 다른 접근은 InvalidArgument
    Span<const ObjectHandle> Nodes() const noexcept;         // localId 순(Include면 늘어난 집합)
    Span<const GraphExternal> Externals() const noexcept;    // slot 1..N(첫 출현 순)
    // slot k의 모든 참조 값을 0x80000000 | indexBySlot[k−1]로 바꾸고 header externalCount를 count로
    // 쓴다. indexBySlot은 1..count의 서로 다른 값이어야 한다(아니면 InvalidArgument — 실패하면 불변).
    // CRC·SHA-256 같은 바이트 지문은 이 호출 뒤에 계산한다(A1 5.5 저장 순서).
    Result<void> AssignExternalIndices(Span<const uint32> indexBySlot, uint32 count) noexcept;
};

// nodes가 저장 집합이고 순서가 localId다(1부터). 각 노드는 이 Registry의 생존 객체이고 서로 달라야
// 한다. 노드마다 encode 동안 고정(PinForEncode)하고, 첫 노드의 Borrow를 저장 끝까지 쥐어 GC를 막는다.
Result<ObjectGraphSaveResult> SaveObjectGraphSet(Allocator&, ObjectRegistry&, const ObjectAccessContext&,
                                                 Span<const ObjectHandle> nodes,
                                                 ObjectGraphSaveOptions = {}) noexcept;

// 편의 API: root에서 영속 강한 참조로 닿는 객체를 발견 순으로 모아 NodeTable과 함께 쓴다
// (externals = Include, writeNodeTable = true로 바꿔 부른다 — v1 SaveObjectGraph와 같은 집합·순서에
// 아래 Outer 규칙을 더한다).
Result<Array<uint8>> SaveObjectGraph(Allocator&, ObjectRegistry&, const ObjectAccessContext&,
                                     ObjectHandle root, ObjectGraphSaveOptions = {}) noexcept;
```

- 노드 encode는 지금 `SaveNodeFields`와 같다(필드마다 frame, `IsExcluded`, wire 재확인, Record 조립, 작업량 청구). 참조 쓰기: 대상이 집합(handle → localId 해시)에 있으면 생존·기대 형식(스키마·정적 대상) 검사 뒤 localId, 없으면 정책대로 Reject·Admit·Include. 대상 검사(생존·문맥·TypeMismatch)는 모든 정책에서 같다.
- **Include의 Outer 순서**: Include가 발견한 노드 T를 더할 때, T의 조상 중 아직 집합에 없는 것을 먼저 (바깥쪽부터) 더한다. 조상 순회는 집합 안 조상, 기준 Outer(노드 1의 Outer가 집합 밖이면 그것), null 중 먼저 만나는 곳에서 멈춘다. 노드 1은 언제나 호출자가 준 root다(그 Outer는 집합에 넣지 않고 기준 Outer가 된다). 그래서 모든 노드의 Outer가 그 노드보다 앞에 있거나 기준 Outer다. Include가 기준 Outer나 노드 1의 조상, 또는 (패키지 정책 Registry에서) 노드 1과 outermost가 다른 객체를 만나면 `ExternalReference` 진단 뒤 InvalidArgument다(11절 결정 17 — 그대로 더하면 Outer 없는 다른 패키지가 들어가 "Outer 없음·기준 Outer 혼합" 규칙에, 기준 Outer를 노드로 더하면 "Outer는 자기보다 앞" 규칙에 걸린다). 정책 없는 Registry의 B8 Level 구조 저장은 영향이 없다.
- NodeTable: 노드 타입은 실제 객체 타입 ID, 이름은 Registry 이름, outer는 집합 안이면 그 localId(자기보다 앞이어야 한다 — 아니면 InvalidArgument), 집합 밖이면 0과 BaseOuter 비트(집합 밖 Outer를 가진 노드들의 Outer가 모두 같아야 하고, Outer 없는 노드와 섞이면 InvalidArgument).
- 마지막에 모든 노드의 생존을 다시 확인한 뒤 결과를 게시한다(v1은 root만 확인).
- 저장 쪽 값 치환 hook(AS A0-O 행의 "bulkId 재매핑")은 B4가 `FieldCodecHooks::SubstituteStruct`로 이미 구현했다 — graph v2는 그대로 쓴다.

### 5.3 외부 참조와 번호 재배정 (A0-Oe)

- `FieldCodecHooks`에 더한다(RF 6.4 "A0-O가 외부 참조 보고를 같은 묶음에 더한다"):

```cpp
struct ExternalReferenceInfo {
    ObjectHandle target{};
    TypeId targetType{};
    TypeId expectedType{};         // 그 위치의 스키마 기대 타입(0 = Object)
    uint32 effectiveFlags = 0;
    bool required = false;
    uint32 objectIndex = 0;
    Span<const PathStep> path{};   // 호출 동안만 유효
    uint32 slot = 0;               // 이 대상에 붙을 외부 번호(1부터, 첫 출현 순 — Null이면 0)
};

enum class ExternalAdmission : uint8 {
    Import,   // 외부 참조로 쓴다(0x80000000 | slot)
    Null,     // 이 위치를 null(0)로 쓰고 ExternalReference 진단(value = 1)을 남긴다
    Reject    // 저장 실패(InvalidArgument)
};

// 저장 집합 밖 대상의 출현마다 부른다(Admit 정책 — 같은 대상의 두 번째 출현부터는 첫 결정이 Import면
// 부르지 않고 집계만 한다). 실패를 돌려주면 저장이 그 오류로 실패한다. 기본은 Reject다.
//
// 사용 예(A1 writer):
//   Result<ExternalAdmission> AdmitExternalReference(const ExternalReferenceInfo& info) noexcept override {
//       auto outermost = registry_.GetOutermost(access_, info.target);
//       if (!outermost || outermost.Value() == package_) {
//           return Result<ExternalAdmission>::Success(ExternalAdmission::Reject);   // UnpackagedReference
//       }
//       return Result<ExternalAdmission>::Success(ExternalAdmission::Import);
//   }
virtual Result<ExternalAdmission> AdmitExternalReference(const ExternalReferenceInfo&) noexcept;
```

- `Null`을 지금 두는 이유(택함): 객체 단위 EditorOnly(A1)에서 쿠킹 저장이 런타임 필드가 가리키는 EditorOnly 객체를 만나면 거부·null 중 하나를 골라야 하는데, hook 서명을 A1에서 다시 바꾸지 않게 자리를 둔다. 진단을 남기므로 UE의 "저장 불가 대상은 오류 없이 null"(10절)과 달리 조용하지 않다. 어느 경우에 Null을 쓰는지는 A1이 정한다.
- 진단 `FieldDiagnosticKind::ExternalReference`(새 값, path = 참조 위치, value = 0 거부·1 null)를 Reject 정책·hook Reject·Null 때 보고한다.
- **번호 결정(11절 결정 7 — 택함: 위치 표)**: 저장은 외부 참조를 `0x80000000 | slot`(첫 출현 순)으로 쓰고 값 위치를 u32 출력 byte offset으로 기록한다(출력은 64 MiB 이하라 u32로 충분). 위치 표는 참조 하나에 4 byte(slot은 그 자리 값에서 읽는다)이고 그 메모리를 그래프 바이트 누계와 작업량에 청구한다. A1은 `Externals()`로 ObjectImports를 (assetId, 객체 경로) 순으로 정렬하고(AS 5.2 — outer import도 더한다) `AssignExternalIndices`로 제자리 덮어쓴다. 위치 기록은 writer offset → Record 안 offset(`RecordBuilder`가 마지막 필드 payload의 시작을 알려 준다 — Serialization에 조회 하나를 더한다) → 출력 offset 순으로 바꾼다.
  - 대안 비교: (가) 두 번 encode(수집 → 정렬 → 다시 쓰기) — 저장·쿠킹 CPU 2배. (나) 그래프에 slot → import 번호 표를 두고 값은 slot으로 남기기 — 재배정이 표 하나라 싸지만 형식에 표가 더해지고 AS 5.1 "0x80000000 | i = object import index i"가 간접 번호로 바뀌어 로드마다 한 번 더 찾으며, A1 Dependencies 대조도 간접 번호를 거친다. 위치 표는 형식·로드 경로를 그대로 두고 저장에서만 O(출현 수)를 쓰므로 택한다.
- 한도: 서로 다른 외부 대상 수 ≤ `maxExternalReferences`(65536), 아니면 CapacityExceeded.

### 5.4 복원 API — 단계형 (A0-Od2, 외부 참조는 A0-Oe)

A1 SCC 로드(AS 11.3 allocate-all → deserialize-all → fixup-all → publish)와 worker 해석(AS 11.2 DeserializeNative)을 위해 복원을 단계로 나누고 Commit은 호출자가 한다. 한 트랜잭션에 여러 그래프를 올릴 수 있다.

```cpp
struct GraphNodeDesc {
    TypeId type{};
    uint32 outer = 0;     // 0 = 그래프 밖(hasBase면 기준 Outer, 아니면 Outer 없음), 1..자기 번호−1 = 노드
    StringView name{};    // Stage 수명 동안 유효
};

struct ObjectGraphInput {
    Span<const uint8> bytes;                 // Stage 수명 동안 유효·불변(결과는 빌리지 않는다)
    Span<const GraphNodeDesc> nodes;         // 비면 그래프의 NodeTable
    bool hasBase = false;                    // NodeTable이면 BaseOuter 비트와 같아야 한다(아니면 InvalidArgument)
    OuterSummary base;                       // 기준 Outer 요약(만들기 전이면 ForNewOutermost)
    Span<const TypeId> importTypes;          // 외부 참조 i의 선언 타입(i−1 위치), 크기 = externalCount
};

// 외부 참조 하나의 쓰임이다(저장의 GraphExternal과 같은 규칙 — A1 Dependencies 대조용, A0-Oe).
struct GraphExternalUse {
    uint32 occurrences = 0;
    uint32 commonFlags = ~0u;
    bool required = false;
};

class ObjectGraphStage {
public:
    // registry와 types가 같은 등록 저장소가 아니면 이후 모든 단계가 WrongContext다. 소유 스레드에서 만들고
    // Registry 정책을 복사한다.
    ObjectGraphStage(Allocator&, ObjectRegistry&, TypeRegistry&, const ObjectAccessContext&,
                     ObjectGraphLoadOptions = {}) noexcept;

    // 1단계: envelope·노드 표·Record·값·참조를 검증하고 이후 단계의 작업량을 미리 청구한다. 생성자를
    // 실행하지 않고 ObjectRegistry에 닿지 않으므로 어느 스레드에서나 부를 수 있다. 그 스레드에서 hook·
    // 마이그레이션 콜백이 불리므로 worker에서 부르는 호출자는 스레드 안전한 hook을 준다.
    Result<void> Validate(const ObjectGraphInput&) noexcept;
    // Validate 뒤: import i마다 쓰임(i−1 위치, A0-Oe).
    Span<const GraphExternalUse> ExternalUses() const noexcept;

    // 2a: 노드 객체를 순서대로 만든다(소유 스레드). 만들기 전에 Registry 상태를 확인한다(아래 사전 확인).
    Result<void> CreateObjects(ConstructionContext&, ObjectHandle baseOuter = {}) noexcept;
    Span<const ObjectHandle> Nodes() const noexcept;             // 2a 뒤 localId 순
    Result<uint32> StoredVersion(usize node) const noexcept;    // Record 버전(A1 Summary 대조)

    // 2b: 필드를 staged 객체에 쓴다. imports[i−1]은 외부 참조 i의 대상이다(생존 객체 또는 같은
    // 트랜잭션 객체). 쓰기 전에 모두 확인한다(아래).
    Result<void> ApplyFields(ConstructionContext&, Span<const ObjectHandle> imports = {}) noexcept;

    // 2c: 실효 값 Required 진단(4절).
    Result<void> CheckRequired(ConstructionContext&) noexcept;
};

class ConstructionContext {
public:
    // outermost 아래 객체 경로를 이 트랜잭션의 공개 전 객체와 생존 객체 모두에서 푼다(SCC 안 import
    // 해석 — 같은 트랜잭션에 올린 다른 패키지의 객체). 규칙은 ObjectRegistry::FindObject와 같다.
    Result<ObjectHandle> FindObject(ObjectHandle outermost, StringView objectPath) noexcept;
};
```

- 상태 기계: 생성 → Validate → CreateObjects → ApplyFields → CheckRequired. 순서를 어기거나 앞 단계가 실패한 뒤 부르면 InvalidArgument. 2a 이후 실패는 그 트랜잭션을 버려야 한다(호출자 Rollback — 부분 공개 없음).
- **1단계 노드 검사**(노드 k마다): 타입 Acquire(NotFound) → Abstract·생성 함수 없음(Unsupported) → `ValidateLayout` → outer 값(0·1..k−1) → `CheckNewObjectIdentity`(정책 사본, 앞 노드들로 계산한 Outer 요약 또는 outer 0이면 `hasBase ? input.base : Outer 없음`) → 그래프 안 형제 유일성((outer, 비교 키) 해시 — 중복은 InvalidArgument, 파일 손상이므로). 이어서 지금 `DecodeNode`의 Record 검증(버전·마이그레이션·필드·`excludedFlags` 존재 거부), 참조 검사(노드 참조의 범위·기대 형식, 외부 참조의 범위와 `importTypes`로 기대 형식 — 고아 검사는 없다). 외부 참조 token은 지금처럼 (대상 번호, 기대 형식 위치)로 남긴다.
- **2a 사전 확인**(만들기 전): 정책 사본 일치(WrongContext) → `hasBase`면 `baseOuter`가 생존 객체 또는 이 트랜잭션 객체이고 `DescribeOuter` 결과의 `isOutermost`·`outermostIsPackage`·`pathBytes`가 `input.base`와 같음(InvalidArgument — handle·Inner 유무는 비교하지 않는다) → 기준 Outer 노드마다 실제 요약으로 `CheckNewObjectIdentity`를 다시 실행(대표 자식 규칙이 실제 Inner 상태로 판정된다) → 기준 Outer 노드 이름의 Registry 점유(AlreadyExists), `hasBase`가 아니면 outer 0 이름 있는 노드의 최상위 이름 공간 점유(AlreadyExists). 이후 실제 Create가 실패할 수 있는 이유는 할당·슬롯 상한·종료·재진입뿐이다.
- **2b 확인**(쓰기 전): `imports.Size() == importTypes.Size()`(InvalidArgument), import마다 생존 객체 또는 이 트랜잭션 객체(StaleHandle), 실제 타입이 선언 타입의 native 파생(InvalidArgument — 호출자 계약), 그리고 1단계가 기록한 (import i, 기대 형식) 쌍마다 실제 대상이 그 기대를 만족(TypeMismatch). `importTypes`는 1단계 형식 검사와 A1의 `expectedTypeId` 대조에만 쓰이고, 실제 연결의 판정은 기록한 쌍이다. 이어서 지금 `ConstructGraph`의 적용 루프다(외부 참조 token을 `imports`의 handle로 바꾼다).
- 1단계가 청구하는 2단계 몫: 노드 생성(노드 수 + 이름 byte) + Commit 몫 1 + staged 간선 수 + 적용 몫(지금과 같다). 2c(Required)는 작업량 무제한이다(4절).

### 5.5 편의 복원 (A0-Od1)

```cpp
struct ObjectGraphLoadOptions {
    ObjectGraphLimits limits;
    FieldDecodePolicy policy;              // allowMissing* 없음, excludedFlags 추가(4절)
    ObjectGraphMigration migrate = nullptr;
    ObjectGraphMigration migrateStruct = nullptr;
    void* migrationUser = nullptr;
    FieldCodecHooks* hooks = nullptr;
    // 편의 LoadObjectGraph 전용:
    ObjectHandle baseOuter{};              // BaseOuter 비트가 있는 그래프의 기준 Outer(생존 객체)
    Span<const ObjectHandle> imports;      // 외부 참조 대상(타입은 대상에서 얻는다)
    // 적용·Required 진단 뒤, Commit 전에 부른다(AS 6.1 prepare — Transient 필드·AssetId 설정). 실패하면
    // rollback이다. staged 객체만 바꾼다(Registry의 다른 객체를 바꾸지 않는다).
    Result<void> (*prepare)(ConstructionContext&, Span<const ObjectHandle> nodes, void* user) noexcept = nullptr;
    void* prepareUser = nullptr;
};

// NodeTable이 있는 그래프를 한 트랜잭션으로 복원하고 노드 1을 root로 Commit한다(v1과 같은 서명).
Result<RootRef<Object>> LoadObjectGraph(Allocator&, ObjectRegistry&, TypeRegistry&,
                                        const ObjectAccessContext&, Span<const uint8>,
                                        ObjectGraphLoadOptions = {}) noexcept;
```

- 단계: ConstructionContext → `Stage.Validate`(base = `DescribeOuter(baseOuter)`, importTypes = imports의 실제 타입) → **도달 검사** → `CreateObjects` → `ApplyFields` → `CheckRequired` → prepare → `Commit()`(노드 1 root). 다중 root·여러 그래프는 `ObjectGraphStage`와 `Commit(roots, out)`을 직접 쓴다. 이 서술은 Od2 뒤의 모양이다 — Od1은 같은 단계를 편의 API 안에서 직접 구현하고 Od2가 Stage 위로 옮긴다.
- **도달 검사(택함: 편의 API에만 유지)**: 노드 1에서 영속 강한 참조, inner → outer(노드 표의 outer), 패키지 정책이면 outer → inner와 기준 Outer 아래 보존(1.7절)으로 닿지 않는 노드가 있으면 생성 전에 InvalidArgument다. 대안(닿지 않는 노드에도 RootRef)은 반환 서명이 RootRef 하나라 표현할 수 없고, 검사 없이 공개하면 다음 GC가 조용히 지운다 — v1의 "조용한 손실 없음"을 편의 API에서 지킨다. Stage API는 고아를 허용한다(호출자가 root·Outer로 붙잡는다). 편의 저장(Include + Outer 순서 규칙)이 만든 그래프는 언제나 이 검사를 통과한다.
- A1 로더 흐름(예):

```cpp
// worker: Objects 섹션을 검증한다(패키지 객체는 아직 없다 — 요약으로 기술한다)
ObjectGraphInput input;
input.bytes = objectsSection;
input.nodes = exportNodes;                          // Exports 표에서 만든 노드 설명(outerIndex 그대로)
input.hasBase = true;
input.base = OuterSummary::ForNewOutermost(true);   // 곧 만들 Package
input.importTypes = importTypes;                    // ObjectImports의 typeId
if (auto valid = stage.Validate(input); !valid) {
    return valid;                                   // A1이 CorruptPackage 등으로 바꾼다
}

// Game 스레드(한 Pump 안): 만들고, 연결하고, 공개한다
ConstructionContext transaction(registry, access);
auto package = transaction.Create<Package>(PackageType);   // Outer·이름 없음
if (!package) {
    return Result<void>::Failure(package.Error());
}

if (auto created = stage.CreateObjects(transaction, package.Value()); !created) {
    return created;
}

// import 해석: 이미 생존한 패키지는 registry.FindObject, 같은 트랜잭션의 SCC 형제는
// transaction.FindObject로 객체 경로를 handle로 바꾼다
if (auto applied = stage.ApplyFields(transaction, importHandles); !applied) {
    return applied;
}

if (auto checked = stage.CheckRequired(transaction); !checked) {
    return checked;
}

// prepare: 대표 에셋의 AssetId(Transient) 등을 Commit 전에 쓴다
const ObjectHandle roots[] = {package.Value()};
RootRef<Object> held[1];
return transaction.Commit(roots, held);
```

### 5.6 한도(`ObjectGraphLimits`)

| 칸 | 지금 | A0-O |
|---|---|---|
| `maxObjects` | 1024 | 4096(= AS export 상한 — 레벨 패키지(WorldOnly, 후속)에서 다시 본다) |
| `maxExternalReferences` | — | 65536(= AS object import 상한) |
| `maxTotalFields` | 1,048,576 | 그대로 |
| `maxTotalElements` | 4,194,304 | 그대로 |
| `maxBytes` | 16 MiB | 64 MiB(AS 비-Bulk 섹션 상한) |
| `maxValueBytes` | 4 MiB | 64 MiB(11절 결정 4) |
| `maxWork` | 64Mi | 8 × (maxTotalFields + maxTotalElements) + 4 × maxBytes(11절 결정 5) |
| `maxMigrationBytes` | 16 MiB | 64 MiB(바이트 상한과 맞춘다) |

- `maxValueBytes` 64 MiB의 결과(결정 4 조건): `BudgetAllocator`의 단일 할당 한도도 64 MiB가 되고(`GraphInternal.h` 108–111행), 저장 피크는 필드 writer + Record builder + 출력으로 최대 약 3배(약 192 MiB), 로드는 원본을 view로 빌리므로 피크가 staged 값 복사와 2c 검사 버퍼(가장 큰 Required 품은 필드 하나)다. README와 Serialization 구현 문서에 적는다.
- 이름(64 byte)·객체 경로(4 KiB)는 Registry 규칙(1.1절)이고 Soft 경로 4 KiB는 B3 그대로다. 일반 String 필드 값에는 4 KiB를 걸지 않는다(11절 결정 6).

### 5.7 오류 순서

- 저장: 입력(노드 0개·중복·null handle InvalidArgument, 개수 CapacityExceeded) → 접근·생존(노드 Borrow·AcquireType — StaleHandle·Destroyed·WrongContext) → 배치(`ValidateLayout` Unsupported) → 노드 encode(지금 순서 — 필드 wire TypeMismatch, Required InvalidArgument, ArrayTooLong CapacityExceeded, 값 InvalidArgument·Overflow) 안의 참조(대상 생존 → 기대 형식 TypeMismatch → 집합 밖 정책: Reject·hook Reject InvalidArgument, 외부 수 CapacityExceeded, Include의 노드 수 CapacityExceeded) → NodeTable(Outer 순서·기준 Outer 불일치·혼합 InvalidArgument) → 바이트 상한(위치 표 포함) CapacityExceeded → 마지막 생존 재확인.
- 복원(지금 코드 순서): 저장소 불일치 WrongContext(`UsesTypeRegistry`) → 옵션(hook 없는 ReportAndDefault·LoadNewerSkippingUnknown InvalidArgument) → envelope(magic InvalidArgument, 버전 Unsupported, flags·개수·길이 InvalidArgument·CapacityExceeded) → 노드 출처·BaseOuter 일치(InvalidArgument) → 노드 검사(5.4절 순서) → Record(지금 순서 — 버전 Unsupported, 마이그레이션, 모르는 필드 정책, Transient 존재 Unsupported, excludedFlags 존재 InvalidArgument, 값) → 참조(범위 InvalidArgument, 형식 TypeMismatch, 기대 타입 미등록 NotFound) → 예산(CapacityExceeded). 편의 API의 도달 검사(InvalidArgument)는 그 뒤. 2a·2b·2c는 사전 확인·Registry 상태·할당·hook 실패·계약 위반만.

## 6. 스레드·수명·GC 장벽

- **스레드**: Outer·Name 조회·Discard·Commit·관찰자 `TakeChanges`·그래프 저장·`ObjectGraphStage`의 생성자·CreateObjects·ApplyFields·CheckRequired·`ConstructionContext::FindObject`는 소유 스레드 전용(WrongThread). `ObjectGraphStage::Validate`는 ObjectRegistry에 닿지 않으므로 어느 스레드나(TypeRegistry 조회는 잠금으로 보호된다 — `TypeRegistry.h` 24행), hook·마이그레이션은 그 스레드에서 불린다. `RegistryPolicy`는 생성 뒤 불변.
- **잠금 안 금지**: 이름 기록·비교 키·색인 성장·관찰자 예약·간선 집계 할당은 잠금 밖(`processing_` 또는 재판단)에서 한다. 잠금 안에서는 상태·색인·고리·관찰자 `Add`만(무할당). 잠금 밖에서 Registry 내부 기록을 읽는 구간(이름 복사·정렬)은 할당·사용자 콜백이 없거나 `processing_` 아래다.
- **수명**: `NameRecord`는 슬롯 회수 때 반환. `ObjectGraphStage`는 입력 바이트·노드 이름 view를 빌리고 TypeLease를 쥔다(소멸 때 반환). `ObjectGraphSaveResult`는 handle 값만 담는다(그 사이 대상이 회수되면 handle은 stale — A1은 저장 호출 동안 쓴다).
- **GC 장벽**: 열린 ConstructionContext가 있으면 GC가 진행하지 않으므로(`Collect` Busy, `Begin/Step`은 활성 생성 0인 safepoint) 그래프 2a–2c와 Commit 사이에 회수가 없다. Outer 간선은 1.5절, outer → inner 보존은 1.7절, Discard는 2절.
- **재진입**: hook·마이그레이션·prepare·allocator 콜백은 Registry·입력·진행 중인 래퍼를 바꾸지 않는다는 지금 계약(`ObjectGraph.h`)을 그대로 둔다. 관찰자 예약은 재검증 규칙(3절)으로, 중첩 예약은 Busy로 막는다.

## 7. 현재 코드와의 차이(바꾸는 것)

| 위치 | 바꾸는 것 |
|---|---|
| Core `Text/NameComponent.h`(새) | `ValidateNameComponent`·`NameComponentError` |
| `ObjectRegistry.h/.cpp` | `RegistryPolicy`·`RegistryOptions.policy·referenceObserver·assertRetiredWithoutDiscard`, Slot 칸(`Slot*` 고리), 이름 색인, Outer·Name 조회, `CollectInners`, `Discard`, `NewObject(outer, name)`, `CreatePending(outer, name)`, 다중 root `Commit`, Outer 방문과 패키지 정책의 자식 방문(3곳, `gate_` 밖), Retire의 자기 고리 해제, 회수 조건 `firstChild == nullptr`, Reclaim의 `NameRecord` 반환, 관찰자 기록(단일·다중 간선 예약 규칙), 패키지 정책 Registry의 `RequestDestroy` Unsupported, `ObjectStats` 칸 |
| `ObjectRegistry.h` 컨테이너 API 주석 | 지금 200·209·224행 "할당하지 않는다" — 관찰자 Registry에서는 `SetReference`·`SetElementReference`·`SetReferenceAt`·`RemoveReferenceAt`·`RemoveReference`·`ClearReferences`·`RemoveElementAt`·`ClearElements`가 관찰자 allocator로 할당할 수 있고 OutOfMemory·Busy·CapacityExceeded를 돌려준다고 고친다(Ob) |
| `ConstructionContext.h/.cpp` | `Create(type, outer, name)`, `Commit(roots, out)`, `DescribeOuter`, `Registry()`·`Access()`·`TransactionId()`, `FindObject`, Required 검사 중 표시(표시 동안 쓰기·Commit Busy) |
| `Object/ReferenceObserver.h`(새) | 관찰자 합산 표 |
| `StructRefArray.h` | `StructElementOps::nativeType`(`&NativeTypeIdentity<S>`) |
| `TypeRegistry.h/.cpp` | `AcquireStructByNative(const void*)`(native 키 색인 — 등록 규칙이 이미 struct native 키를 유일하게 한다) |
| `FieldCodec.h` | `FieldDecodePolicy`에서 `allowMissingFields`·`allowMissingFlags` 삭제·`excludedFlags` 추가, `IsMissingAllowed`·`ReportIfRequiredNull` 삭제, `NotePresent` 자리, `FieldEncodeContext` 검사 모드·`StagedSource`, `FieldCodecHooks::AdmitExternalReference`·`ExternalAdmission`, `FieldDiagnosticKind::ExternalReference`, 주석(hook 계약·objectIndex) |
| `RuntimeTypeOps.h` | `RuntimeFieldOps::readStaged`(선택, 끝 칸) |
| `GeneratedFieldCodec.h` | `CheckMissingField`·`ReportMissingRequired` 삭제, `DecodeStructRecord` 누락 순회 삭제(present bitset은 `NotePresent`로 유지)·`excludedFlags` 존재 거부, `ReportParsedValue`의 Required 보고 삭제, `EncodeGeneratedField` staged 분기, `EncodeArrayField` 검사 모드의 MaxElements 생략, `EncodeStructRecord` 검사 모드의 치환 생략, `EncodeThroughFieldValue`의 `readStaged` |
| `ObjectGraph.h`·`GraphSave.cpp`·`GraphLoad.cpp`·`GraphInternal.h` | v2 형식·`SaveObjectGraphSet`·`ObjectGraphSaveResult`·`GraphExternalPolicy`·`ObjectGraphStage`·편의 API(도달 검사)·한도, v1 envelope·내부 Commit 삭제 |
| Serialization `Record.h` | `RecordBuilder`의 마지막 필드 payload offset 조회(외부 번호 위치 기록용) |
| 시험 | allowMissing* 사용 시험, v1 byte를 직접 만드는 시험, Required 진단 시점 기대, ObjectRuntime 쪽 그래프 사용 시험(`ClassPropertyTests.cpp`, `AutoIdentityCompatibilityTests.cpp` 91행 근처), World 시험의 Level 저장(편의 API로) |
| 문서 | `ObjectSerialization/README.ko.md`, Docs/Implementation `Serialization.ko.md`·`ObjectLifecycle.ko.md`·`IncrementalGarbageCollection.ko.md`(outer 간선·자식 방문)·`ReflectionMetadata.ko.md`(WorldOnly·EditorOnly 행), 12절 정정 대상 |

MetaCompiler·생성 헤더 형식·TypeRegistry 등록 규칙은 바꾸지 않는다.

## 8. 커밋 분할과 검증

순서는 Oa → Ob → Oc → Od1 → Od2 → Oe다(Oc는 Oa·Ob와 독립이라 앞당길 수 있다). 각 단계는 구현 뒤 다수 서브에이전트 교차검증을 거친다. 커밋마다 Debug 빌드·format·바뀐 영역 시험(ObjectRuntime·ObjectSerialization·World 시험 타깃 — Oc·Od는 ObjectRuntime 쪽 그래프 사용 시험 포함)을 돌리고, Release·Asan·Android 빌드·policy·전체 시험은 Oe 뒤에 한 번 돌린다.

| 커밋 | 범위 | 검증 |
|---|---|---|
| A0-O0 [Docs] | 이 문서와 12절 (가) 정정 | 문서 검사 |
| A0-Oa [Core] Outer·Name·정책·다중 root Commit | `ValidateNameComponent`, Slot 칸(`Slot*`)·이름 색인·자식 고리, `Create/NewObject(outer, name)`, 조회 6종·`IsNameTaken`·`DescribeOuter`·`CollectInners`·`TransactionId`, inner → outer GC 간선, Retire·Rollback 해제, `RegistryPolicy` 타입과 규칙 5·8(패키지 이름 없음 포함), 다중 root Commit(관찰자 집계 자리까지) | 9절 Oa 시험 + 기존 ObjectRuntime 시험 전부 |
| A0-Ob [Core] Discard·관찰자·소속 보존 | `Discard`, `ReferenceObserver`(합산 표·transaction)와 모든 기록 지점(`StructElementOps::nativeType`·`AcquireStructByNative` 포함), 관찰자 Registry의 RequestDestroy Unsupported, 패키지 정책의 outer → inner 강한 간선(자식 고리 GC 방문), `retiredWithoutDiscard` | 9절 Ob 시험 |
| A0-Oc [Core] 누락 = 기준·실효 값 Required | 4절 전부(v1 envelope 위 — present 판정 유지, 누락 순회만 제거), allowMissing* 시험 개정 | 9절 Oc 시험 + ObjectSerialization·ObjectRuntime 그래프 시험 전부 |
| A0-Od1 [Core] graph v2 형식·저장 | 5.1·5.2(Reject·Include·Outer 순서)·5.5·5.6·5.7, v1 삭제, byte 시험 재작성, World Level 저장 시험 | 9절 Od1 시험 + World·Engine 시험 |
| A0-Od2 [Core] 단계형 복원 | 5.4(외부 참조 없이), 2a 사전 확인, `ConstructionContext::FindObject`, 편의 복원을 Stage 위로 | 9절 Od2 시험 |
| A0-Oe [Core] 외부 참조 | wire 외부 비트, Admit hook·`ExternalAdmission`·진단, `GraphExternal`·위치 표·`AssignExternalIndices`, Stage의 imports·`ExternalUses`·기록한 기대 형식 기준 검사 | 9절 Oe 시험 + 전체 체인(Release·Asan·Android·policy) |

## 9. 시험 목록

**Oa — Outer·Name·정책**

- 이름 규칙 표: 빈 문자열·65 byte·비 NFC(분해형)·첫 문자 `_`·`-`·M\*·공백·`.`·`/`·Default_Ignorable(U+200B·U+FE0F)·`CON`·`com1`·`LPT¹`·한글·결합 문자, 사유·offset.
- 비교 키: ASCII 빠른 경로와 일반 경로가 같은 키, 비ASCII 조회의 할당 실패 OutOfMemory(조회·Create 모두, Create는 AlreadyExists보다 먼저).
- 유일성: 같은 Outer의 대소문자만 다른 이름 충돌, NFC끼리의 caseless 동치 충돌, simple folding이 같게 보지 않는 쌍(`Straße`와 `STRASSE` — full folding만 같게 본다)은 공존, 다른 Outer 같은 이름 허용, 최상위 이름 공간, 두 트랜잭션 동시 생성 AlreadyExists, rollback 뒤 이름 재사용, cutoff white 이름 회수(Sweeping 중 같은 이름 생성 성공).
- 빈 이름·대표 자식: Outer 없는 빈 이름 여럿, 대표 자식 하나, 대표 자식 있는 Outer에 이름 자식 거부와 그 반대, 깊은 객체 빈 이름 거부.
- 경로: `GetObjectPath`(대표 에셋 아래 `Body.Socket`), `FindObject`(빈 경로 = 대표, 대소문자 무시, 없는 구성 요소 NotFound, 4 KiB 초과 InvalidArgument), 4 KiB 경로 경계(4096 성공·4097 CapacityExceeded).
- 조회 상태: `FindChild`가 Reserved·cutoff white에 NotFound, `IsNameTaken`이 Reserved에 true, DestroyRequested 객체의 `GetName`·`GetObjectPath` 성공.
- 재진입: `GetName`·`GetObjectPath`의 out allocator 콜백이 그 객체를 `RequestDestroy`하고 `Collect`해도 ASan 무결(Discard판은 Ob), `CollectInners`의 예약 중 재진입 변경 Busy.
- `CollectInners`: Direct·DepthFirst 순서가 AS 5.1 export 순서와 같음(대표 먼저, 이름 byte 순), Reserved·DestroyRequested 제외, 실패 시 out 길이 불변, 깊은 트리(2048 수준)에서 스택 사용 일정.
- Outer 상태: Reserved Outer(같은 트랜잭션 성공·다른 트랜잭션 StaleHandle), DestroyRequested·Discard된 Outer StaleHandle, 다른 Registry의 Outer handle StaleHandle, 다른 접근 문맥 WrongContext.
- 정책(Oa로 옮김): 패키지 타입 Outer 거부, 패키지 이름 거부, 비패키지 Outer 없음 거부, outermost가 패키지가 아님 거부, `NewObject`, WorldOnly(상속 포함) Unsupported, 정책 없는 Registry는 지금과 같음.
- GC: inner만 root여도 outer 생존(Collect·증분 Step·병렬 trace), 정책 없는 Registry에서 outer만 root면 inner 회수, Marking 중 생성·Commit 뒤 outer 생존, Sweeping 중 cutoff Outer 거부, 부모가 자식보다 먼저 Retire되는 경우 자식이 떨어질 때까지 부모 미회수(`firstChild` 조건 — sweep 중 `DrainRetired`·rollback 회수가 끼어도 ASan 무결), 회수 뒤 `NameRecord` 반환(할당 통계 0).
- 이름 색인: tombstone이 많은 상태에서 같은 크기 rebuild, 성장 중 할당 실패 시 색인 불변, 슬롯 1만 개에서 고리 순회가 chunk 수에 비례하지 않음(H2).
- 다중 root Commit: root 둘·중복 root·빈 roots(다음 GC 회수)·out 크기 불일치·비어 있지 않은 out InvalidArgument·다른 트랜잭션 handle StaleHandle, 실패 후 비공개 유지·Rollback, 기존 `Commit()` 동작 유지.
- 할당 실패 스윕: Create의 모든 할당 지점(비교 키·이름 기록·색인 성장·저널) 실패 시 슬롯·색인·통계 불변.

**Ob — Discard·관찰자·소속 보존**

- Discard: 부분 트리 전체 DestroyRequested·조회 NotFound·즉시 같은 이름 재생성(GC 전), RootRef가 남은 객체 Discard 뒤 Resolve Destroyed·WeakRef TryPin Destroyed·GC 뒤 회수, 남은 참조가 Outer 사슬만 남기고 형제는 회수, 재Discard 무동작, (정책 없는 Registry에서) RequestDestroy 뒤 Discard, Reserved 자식 Busy, cutoff Destroyed, Marking·Sweeping 중 Discard, 할당 실패 스윕(목록·집계·예약) 뒤 상태·색인·고리 불변, 재진입(`GetName` 콜백의 Discard) ASan 무결.
- 소속 보존(결정 8): 패키지 RootRef만 있어도 export 목록 없이 모든 소속 객체 생존(Collect·증분·병렬), 부분 Discard한 객체만 회수, 패키지 Discard 뒤 전체 회수, 슬라이스 사이 생성·Discard, 패키지 정책 Registry에서 RootRef를 Discard 없이 놓으면 `retiredWithoutDiscard` 증가(`assertRetiredWithoutDiscard = false` Registry로), 종료 뒤 회수는 세지 않음, 정책 없는 Registry는 outer → inner를 방문하지 않음, 패키지 정책 Registry의 RequestDestroy·Object::Destroy Unsupported(관찰자 없어도), 정책 없는 Registry에 관찰자를 주면 Fatal(사망 시험).
- 관찰자: SetReference(null→A, A→B, B→null, 같은 패키지 무기록), 배열 삽입·교체·삭제·비우기(쌍별 합침), StructRefArray 원소 제거·비우기(원소만 trace — 소유자 Trace 호출 수 0), 경로 쓰기(B6)로 바꾼 참조, 패키지 소유 간선 무기록, staged 쓰기 무기록·Commit 합계와 `transaction` = `TransactionId()`, Discard 음수 합계(transaction 0), DestroyRequested 소유자 제거 무기록, 합산(+1·−1이 0이 되어 꺼낼 때 빠짐, 키 처음 생긴 순서), 예약 실패 OutOfMemory 시 변경 없음, 예약 중 재진입(할당 콜백이 다른 SetReference) 재검증, 중첩 예약 Busy, TakeChanges 맞바꿈·allocator 불일치 InvalidArgument·예약 중 Busy·다른 스레드 WrongThread, 같은 패키지 안 대상 교체 무기록·무예약, `maxKeys` 초과 CapacityExceeded·불변(0이 된 키도 셈), 다중 간선 지점의 할당 실패 불변·`processing_` 동안 재진입 Busy, 묶음 순서(Edit −1 키가 Commit +1 키보다 먼저 생겨도 묶음 합으로 판정 — 소비자 시험), `AcquireStructByNative` 실패 시 Unsupported. "Discard Busy면 RootRef 유지"(2절 순서)는 AssetManager 시험(A1)으로 기록한다.
- 무작위 편집 대조: 임의 순서의 편집·Commit·Discard 뒤 관찰자 누계 = 전체 Trace로 다시 센 패키지 쌍 다중도(속성 시험, 시드 고정).

**Oc — 누락 = 기준·실효 값 Required**

- 빠진 필드·struct 멤버·배열 원소 struct 멤버·배열 필드가 생성자 값(기준)을 유지, NotFound 없음, present 판정 지점이 필드·멤버마다 한 번 불림(시험용 문맥으로 확인 — 동작은 무변화).
- Required: 파일 null 진단, 빠진 ObjectRef 진단, 생성자 non-null Soft 누락 무진단·생성자 null Soft 누락 진단, 빠진 struct 안 Required 멤버(경로), Required ObjectRefArray null 원소 `[f[i]]`, StructRefArray 원소 안 Required 참조 멤버, 진단이 적용 뒤 한 번(1단계에서 없음 — hook 호출 순서 기록), hook 실패 시 rollback(객체·root 0).
- excludedFlags: EditorOnly 필드가 있으면 InvalidArgument, 없으면 기준 값·Required 무진단(쿠킹 로드 모의).
- 검사 중 표시: hook·allocator 콜백이 그 트랜잭션에 Create·SetReference·Commit을 부르면 Busy, 표시는 검사 뒤 내려간다.
- 검사 모드: MaxElements 넘은 배열을 가진 파일 로드 성공(검사가 ArrayTooLong을 내지 않음), SubstituteStruct hook 미호출, Soft 수집 1단계 한 번, 수동 함수표 Required 필드(`readStaged` 있음 성공·없음 Unsupported), staged encode가 Registry 상태를 바꾸지 않음, 검사 writer가 필드마다 재사용되고 값 한도를 넘는 생성자 배열도 통과.

**Od1 — graph v2 형식·저장**

- 왕복: 모든 값 종류·struct·배열·순환·공유 정체성(기존 시험의 v2판), 같은 입력 같은 byte, 다른 Registry 복원 후 재저장 byte 동일.
- 저장 집합: 호출자 순서 = localId, 고아 노드 허용, 집합 밖 참조 Reject 진단·InvalidArgument, Include 순서 = v1 BFS 순서, Include가 Outer 사슬을 먼저 더함(노드 1은 root 유지, 기준 Outer에서 멈춤), Include가 기준 Outer·노드 1 조상·(패키지 정책) 다른 outermost 객체를 만나면 진단·InvalidArgument, 중복·0개·null handle, 4096 경계.
- NodeTable: Outer·이름 왕복(대표 자식·깊은 경로), BaseOuter 비트와 outer 0의 두 뜻, Outer가 뒤에 오면 InvalidArgument, 기준 Outer 불일치·Outer 없음과 혼합 InvalidArgument.
- 편의 복원 도달 검사: 닿지 않는 노드 InvalidArgument(생성 계수 0), inner → outer로만 닿는 Outer 노드 통과, 패키지 정책의 기준 Outer 아래 노드 통과.
- 형식 손상: 버전 1·3 Unsupported, 정의되지 않은 flags·NodeTable 없는 BaseOuter, 개수·길이 불일치, 참조 값 경계(nodeCount+1·0x80000000·외부 비트 — Od1은 externalCount 0만), 남는 byte.
- 한도: 64 MiB 경계(값 하나 64 MiB 포함), 최대 필드·원소·바이트 fixture가 기본 작업량 안에서 통과(결정 5), 작업량 선형(입력 두 배 → 청구 두 배 이내).
- World: B8 Level 구조 저장·복원이 편의 API로 같은 결과.

**Od2 — 단계형 복원**

- 노드 표(A1 모의): 타입이 Record schema와 다르면 InvalidArgument, 형제 중복, 정책 위반(WorldOnly·패키지 규칙·패키지 이름)이 생성자 실행 전에 거부(생성 계수 0).
- 사전 확인: base 요약 불일치(isOutermost·outermostIsPackage·pathBytes) InvalidArgument, 기준 Outer에 이미 대표 자식이 있으면 대표 노드 거부(실제 요약으로 재판정), 기준 Outer 아래 이름 점유 AlreadyExists, outer 0 이름 있는 노드의 최상위 이름 점유 AlreadyExists, 정책 사본 불일치 WrongContext.
- 단계: 순서 위반 InvalidArgument, Validate를 worker 스레드에서 실행(ObjectRegistry 미접근, hook이 그 스레드에서 불림), 한 트랜잭션에 그래프 둘과 다중 root Commit(Od2는 그래프 사이 참조 없이 — 서로 참조는 Oe), prepare가 Transient 필드 설정·실패 시 rollback, `ConstructionContext::FindObject`가 같은 트랜잭션 Reserved와 생존 객체를 찾고 다른 트랜잭션 Reserved는 NotFound.

**Oe — 외부 참조**

- Admit: 첫 출현 결정(같은 대상 여러 위치 occurrences·commonFlags·required 집계), Reject 진단·InvalidArgument, Null이 그 위치 0과 진단(value 1)을 남기고 slot을 쓰지 않음, slot 첫 출현 순.
- `AssignExternalIndices`: 역순·임의 순열 재배정 뒤 복원이 같은 대상 연결, 잘못된 표(중복·0·범위 밖·길이) InvalidArgument·불변, header externalCount 갱신(outer import 추가로 count > slot 수), 위치 표 메모리가 바이트·작업량 청구에 들어감.
- 복원 imports: 생존 대상·같은 트랜잭션 대상(SCC 두 그래프가 서로 외부 참조 — allocate-all 뒤 apply-all, `transaction.FindObject`로 해석), 선언 타입 불일치 InvalidArgument·기록한 기대 형식 불만족 TypeMismatch(쓰기 전), 개수 불일치, 65536 경계, `ExternalUses`가 저장의 `GraphExternal`과 같은 집계.

## 10. UE 5.6 대조

모두 `gh api -H "Accept: application/vnd.github.raw" "repos/EpicGames/UnrealEngine/contents/<path>?ref=5.6"`로 받은 원본에서 확인했다(줄 번호는 근사). `CUO/` = `Engine/Source/Runtime/CoreUObject/`.

| 주제 | UE 5.6 | Rocks A0-O |
|---|---|---|
| Outer·Name 저장 | `UObjectBase`의 `NamePrivate`·`OuterPrivate`(CUO/Public/UObject/UObjectBase.h 346–366), 전역 해시 `Hash`·`HashOuter`(multimap — UObjectHash.cpp 676–680) | Registry Slot 칸과 Registry 소유 이름 색인(객체 본체에 두지 않는다 — `Object`는 handle만) |
| 유일성 | 생성·Rename 때 클래스 없이 (Outer, Name)으로 조회해 강제(UObjectGlobals.cpp 3622, Obj.cpp 292), FName 대소문자 무시(Obj.cpp 307 주석) | 같은 키((Outer, caseless 키)), Unicode caseless(AS 4.2) |
| 같은 이름으로 생성 | 같은 클래스면 같은 주소에서 교체, 다른 클래스면 Fatal "Cannot replace existing object of a different class."(UObjectGlobals.cpp 3625–3640, 3671) | AlreadyExists(교체 없음) |
| Outer 없는 객체 | 기본 Outer = Transient 패키지(UObjectGlobals.h 1889), `check(InOuter \|\| (InClass == UPackage::StaticClass() && InName != NAME_None))` — Outer 없는 것은 패키지뿐이고 이름이 있어야 한다(UObjectGlobals.cpp 3587) | 가짜 Outer 없음(EIP §5.2). 정책 없는 Registry는 Outer 없는 객체 허용, 패키지 정책은 "패키지만 Outer 없음"(UE와 같다)이되 패키지는 **이름이 없다**(UE와 반대 — 정체성은 AssetId) |
| GC와 Outer | 참조 수집이 Outer를 참조로 방문 `Dispatcher.HandleImmutableReference(Outer, EMemberlessId::Outer, …)`(CUO/Public/UObject/FastReferenceCollector.h 995), 에디터는 external package도 방문(996–999, WITH_EDITOR) | inner → outer 강한 간선(같다). EIP §5.2의 "5.6 소스로 대조하지 않았다"를 이것으로 확인 |
| 패키지와 export 수명 | 패키지는 export를 붙잡지 않는다 — 에디터는 GC된 export가 있는 패키지를 다시 로드한다(`s.ReloadPackagesWithGCedExports`, AsyncLoading2.cpp 500·1487, WITH_EDITOR) | 패키지 정책 Registry에서 outer → inner 강한 간선(1.7절) — 저장 집합 결정성을 위해 UE와 다르게 정한다 |
| Rename | `REN_Test`면 충돌 시 false, 아니면 Fatal(Obj.cpp 311–320), 대소문자만 바꾸는 Rename은 허용(Obj.cpp 300–308), `LowLevelRename`이 unhash → 필드 교체 → rehash(UObjectBase.cpp 296–303), 플래그 `REN_Test·DoNotDirty·DontCreateRedirectors·NonTransactional·ForceGlobalUnique·SkipGeneratedClasses·SkipComponentRegWork·AllowPackageLinkerMismatch`, `ForceNoResetLoaders`는 5.5부터 deprecated(ObjectMacros.h 2184–2202) | A0-O에 없음 — 이름 불변(11절 결정 2) |
| 파괴 표시와 이름 | `MarkAsGarbage`는 플래그만(UObjectBaseUtility.h 176–190), 기본 조회 제외는 `Unreachable \| AutoRTFMConstructionAborted`(UObjectHash.cpp 58–59) — Garbage 객체는 계속 찾히고 이름을 점유. 예: `UWorld::DestroyActor`는 MarkAsGarbage만(Engine/Private/LevelActor.cpp 1054) | RequestDestroy는 이름 유지(같다), cutoff white는 조회 제외(같은 효과) |
| Garbage를 가리키는 참조 | garbage 제거 GC가 가변 참조를 null로 만든다(`KillReference` — GarbageCollection.cpp 2858, `ProcessReferenceDirectly`의 KillFlag 분기 2928–2944, EliminateGarbage 경로 4071–4078) | 강한 참조를 null로 만들지 않는다(DestroyRequested 저장소를 남은 참조가 지킨다 — 지금 GC 그대로). 상주 계수가 0일 때만 Discard하는 불변식이 대신한다 |
| 파괴 되돌리기 | undo가 `ClearGarbage`로 객체를 되살린다(Editor/UnrealEd/Private/EditorTransaction.cpp 871–878) | Discard는 되돌릴 수 없다 — undo의 삭제 취소는 CU4가 새 객체로 다시 만드는 방식으로 정한다(11절 N5) |
| 이름이 빠지는 시점 | GC가 Unreachable로 판정한 뒤 `UnhashUnreachableObjects` → `ConditionalBeginDestroy`(GarbageCollection.cpp 6068–6092) → `BeginDestroy`의 `LowLevelRename(NAME_None)`(Obj.cpp 1053–1055) | Retire 때 해제(같은 시점), Discard는 즉시 |
| 이름 즉시 비우기 | 공용 장치 없음. 호출자가 Rename: `TRASH_%s` + `MakeUniqueObjectName`(Engine/Private/ActorConstruction.cpp 209–211), `%s_DEADPACKAGE`(CUO/Private/UObject/PackageReload.cpp 467). `UPackageTools::UnloadPackages`는 Rename 없이 RF_Standalone 해제·ResetLoaders·CollectGarbage(Editor/UnrealEd/Private/PackageTools.cpp 549–594) | `Discard`가 공용 장치(가짜 이름 없이 색인에서 뺀다) |
| 경로 | `Pkg.Asset:Sub.Nested` — 패키지 바로 아래 객체 뒤에만 `:`(UObjectBaseUtility.cpp 66–75) | `.`만, 대표 에셋은 빈 이름(AS 4.1) |
| 이름 문자 검사 | `INVALID_OBJECTNAME_CHARACTERS`(Core/Public/UObject/NameTypes.h 168–177)와 `FName::IsValidObjectName`(UnrealNames.cpp 3975–3982)이 있으나 Rename·StaticAllocateObject는 검사하지 않음 | 생성 때 검사(AS 4.2 규칙) |
| 패키지 소속 | `GetPackage()`는 Outer 사슬에서 처음 만나는 external package(OFPA) 우선, `GetOutermost()` = `GetPackage()`(UObjectBaseUtility.cpp 200–212·225–228) | outermost만(external package 없음) |
| 저장 집합 | 루트(TopLevelFlags 객체 또는 Asset — SavePackage2.cpp 377–392)에서 참조 순회, 패키지 안은 export·밖은 import(PackageHarvester.cpp 994–1003), 닿지 않는 패키지 내 객체는 빠짐(SavePackage.h 71–75) | Outer 표 기준 전체(AS 5.1) — 호출자가 집합을 준다 |
| export·import 순서 | `SortExports`·`SortImports`가 `GetFullName()`(클래스 이름이 첫 키)을 `Stricmp`로 정렬(SavePackageUtilities.cpp 1120–1205) — Outer가 앞선다는 보장이 없다 | "Outer가 앞 + 깊이 우선 + 이름 byte 순"(AS 5.1)과 import (assetId, 객체 경로) 순(AS 5.2) — 더 엄격한 자체 규칙 |
| 저장 불가 대상 | Transient·Garbage·Transient 패키지 객체 참조는 오류 없이 null로 저장(SaveContext.cpp 118–135, LinkerSave.cpp 228), 다른 패키지 private 객체는 "Illegal reference to private object"(SavePackage2.cpp 784) | 조용한 null 없음 — 집합 밖은 외부 참조 허락, 진단 붙은 Null, 또는 거부(A1의 UnpackagedReference) |
| Import 표 | `FObjectResource`의 `ObjectName`·`OuterIndex`(0 = 최상위 패키지 — ObjectResource.h 191–203), `FObjectImport`의 `ClassPackage`·`ClassName`(443–463), import의 Outer도 import(PackageHarvester.cpp 888–892) | AS 5.2 ObjectImports(같은 구조), graph는 번호만 쓰고 표는 A1 |
| 패키지 참조 계수 | Zen 로더 `FLoadedPackageRef::RefCount`(AsyncLoading2.cpp 1296)는 import하는 패키지의 로드 동안만 쥐는 GC pin(1786·1820, 유휴 때 0이어야 함 — 1838). 로딩 패키지가 자기 패키지 참조도 같은 저장소로 더하고 놓으며(`AddPackageReference`·`ReleasePackageReference` 2347–2369), Verse cell export도 로딩 패키지가 참조하는 동안 pin한다(4924–4930). 로드 뒤 속성 쓰기의 참조 변화 관찰 장치는 확인되지 않음(`OnObjectPropertyChanged`는 에디터 전용) | `ReferenceObserver`는 UE에 없는 장치(상주 단위 계수용 — AS 11.4) |
| 누락 속성 | 저장은 archetype과 `Identical`이면 쓰지 않음(Class.cpp 1992–2018), 로드는 스트림 태그만 적용(1662–1690)하고 빠진 속성은 생성 때 archetype에서 복사한 값 유지(LinkerLoad.cpp 5594, UObjectGlobals.cpp 4558–4570) | 누락 = 기준(같은 효과, A0-O의 기준은 생성자 값) |

## 11. 결정(권고안)과 열린 결정

**결정**

1. **패키지 정책 Registry의 RequestDestroy** — 관찰자 유무와 무관하게 Unsupported(파괴는 Discard 하나 — "소속 객체는 Discard로만 죽는다"가 정책 전체의 불변식, 관찰자가 붙은 Registry만으로 두던 초안을 넓혔다). 관찰자는 패키지 정책 Registry에만 붙는다. 단서: Discard는 되돌릴 수 없고, Discard 대상을 가리키는 참조의 저장 정책·강제 삭제의 참조 재작성·undo의 삭제 취소는 A2·CU4의 열린 문제다(N5).
2. **Rename·Outer 변경** — A0-O에 두지 않는다. EIP §5.2가 조건부로 허용하지만 A1–A6 소비자가 없고, 패키지 사이 이동은 관찰자·소속 불변식을 깨므로 넣을 때 "같은 패키지 안 이름·Outer 변경만, SubobjectGuid와 함께"로 따로 설계한다(UE Rename의 redirector 대응은 AS 4.3 별칭이 패키지 수준에서 맡는다).
3. **빈 이름·대표 자식 규칙(1.1절)** — 그대로. Outer가 있는 빈 이름은 outermost의 유일한 자식만 — 객체 경로가 모호하지 않고 AS 5.1의 export 1 규칙과 같다. 패키지 정책의 패키지는 이름이 없다.
4. **`maxValueBytes`** — 64 MiB(= 그래프 상한, RF 결정 16의 "한도 확장(64 MiB)"을 필드 값 한도로 읽는다). 조건: `BudgetAllocator` 단일 할당 한도·저장 피크(약 3배)·2c 버퍼를 문서화한다(5.6절).
5. **작업량 상한** — `8 × (maxTotalFields + maxTotalElements) + 4 × maxBytes`(약 3.1억). AS·RF의 "8 × (필드 + 원소)"는 바이트 작업을 빼먹어 지금 64Mi보다 작다. 바이트·필드·원소 상한을 각각 채운 세 fixture가 통과함을 시험으로 고정한다.
6. **"문자열 4 KiB"(AS A0-O 행)** — 객체 경로·Soft 경로(이미 4 KiB)·노드 이름(64 byte)에만 적용하고 일반 String 필드 값은 `maxValueBytes`로 둔다.
7. **외부 번호 결정** — 첫 출현 slot과 u32 위치 표로 제자리 재배정, 재배정은 CRC·SHA 계산 전(5.3절에 대안 비교).
8. **패키지 소속 객체의 생존(뒤집음)** — 패키지 정책 Registry에서 outer → inner를 GC 강한 간선으로 방문하고 자식 고리를 export 표로 쓴다. `Package.exports` 목록을 없애고, 소속 객체는 Discard로만 죽는다(`retiredWithoutDiscard` — `assertRetiredWithoutDiscard`면 Debug assert, 종료 뒤 제외). AssetManager는 모두 Discard한 뒤에만 RootRef를 놓는다(2절). 저장 집합이 GC 시점과 무관해진다. EIP §5.2에 "단 패키지 정책 Registry에서는 outer가 자손을 붙잡는다"를 덧붙인다(12절 정정).
9. **수동 함수표의 Required 실효 값 읽기** — 선택 칸 `readStaged`(없으면 그 필드의 2c 검사가 Unsupported).
10. **관찰 제외 규칙** — "패키지 객체(Outer 없음)가 소유한 간선은 관찰하지 않는다"는 구조 규칙.
11. **Required 진단 위치** — 적용 뒤 staged encode 검사. 전제: 검사 writer는 필드마다 재사용하고 크기 한도가 없으며, encode 고정 대신 staged 배타성에 기댄다.
12. **관찰 기록의 원인·트랜잭션** — `cause`와 `uint64 transaction`(Commit만, Edit·Discard는 0), `ConstructionContext::TransactionId()` 공개. AssetManager가 Commit 기록을 로드 작업에 맞춰 내부 보유를 실제 간선 다중도로 바꾼다(AS 11.4).
13. **편의 복원의 도달 검사** — 편의 API에만 유지(5.5절 근거).
14. **외부 참조 허락 결과** — `ExternalAdmission {Import, Null, Reject}`를 지금 둔다(5.3절 근거).
15. **자식 고리와 회수** — 고리 해체를 하지 않는다. white 부모의 자식은 같은 sweep에서 스스로 떨어지고, 회수 조건 `firstChild == nullptr`가 그 사이 부모 슬롯 재사용을 막는다. Retire는 객체당 O(1), `processed`는 슬롯당 1 그대로(부모가 먼저 Retire될 때 고리를 해체하던 초안을 대신한다).
16. **관찰자 키 상한** — `ReferenceObserver(Allocator&, usize maxKeys = 1u << 20)`, 넘으면 CapacityExceeded(N6의 상한).
17. **편의 저장 Include의 거부 조건** — Include가 기준 Outer·노드 1의 조상·(패키지 정책 Registry에서) 노드 1과 다른 outermost의 객체를 만나면 `ExternalReference` 진단 뒤 InvalidArgument(초안의 "받아들이고 문서화"는 NodeTable 규칙에 걸려 실패하므로 바꿨다).

**열린 결정(권고안 포함 — 권고안으로 진행한다)**

- **N1·N2** — 결정 1·17로 정했다.
- **N3. `AcquireStructByNative`의 범위** — 관찰자 기록에만 필요한 TypeRegistry 조회를 새로 둔다. 권고: TypeRegistry에 native 키 → struct 레코드 색인(등록·해제 때 유지)을 두고 lease를 돌려준다. 호출 위치는 다중 간선 지점의 `processing_` 구간이고(3절), 키는 `&NativeTypeIdentity<S>`다. 대안: `StructRefArrayStorage`에 원소 struct `TypeId`를 staged 배정 때 기록 — 수동으로 만든 컨테이너(live `AddElement`만 쓴 경우)에서 비어 있을 수 있어 택하지 않는다.
- **N4. present 판정 공개 방식** — A0-O는 보호 가상 `NotePresent`만 둔다. 권고: 공개 hook(`FieldCodecHooks::ObservePresentValue`)과 Stage 출력 중 선택은 A5가 override 집합 자료구조와 함께 정한다(지금 정하면 자료구조를 미리 고정하게 된다).
- **N5. Discard와 편집 도구** — Discard 대상을 가리키는 다른 패키지 참조의 저장 정책(null·진단·거부), 강제 삭제·consolidate의 참조 재작성, undo의 삭제 취소(새 handle로 다시 만들기와 참조 재연결)는 A2(asset-delete `--force` — AS 4.4)·CU4(Transaction)의 열린 문제다. 권고: A2·CU4 확정안에서 정하고, A0-O는 "Discard는 되돌릴 수 없고 handle은 다시 살지 않는다"만 보장한다.
- **N6. 관찰자 표의 키에 transaction을 넣은 결과** — 같은 패키지 쌍이라도 Commit 기록은 트랜잭션마다 따로 남아 키 수가 "Pump 사이 Commit 수 × 쌍"이 된다. 권고: 받아들인다(AssetManager가 Pump마다 비운다). 상한은 `maxKeys`(결정 16)이고, Pump 안 Commit 수는 로드 예산(AS 11.5 1번)이 묶는다.
- **N7. `worldOnlyCreationKey`의 시점** — 권고: World 정책 단계(EngineRuntime 134행)에서 구현하고 A0-O는 이름·뜻만 둔다. 지금 두면 쓰는 곳 없는 칸이 생긴다.
- **N8. 고유 이름 도우미의 위치** — 권고: A2(임포트·복제가 첫 소비자)에서 `ObjectRegistry::MakeUniqueName`으로 더한다(1.2절 자리).

## 12. 다른 문서 정정

**(가) 설계 문서 — 이 문서를 넣을 때 함께 고쳤다**(A0-O0)

AS(`Docs/Design/AssetSystem.ko.md`):

1. **1절 A0-O 행**: 이 문서를 가리키고, "Discard" → "부분 트리 Discard", "ReferenceObserver(큐 전달)" → "순 다중도 합산 표", "외부 참조 resolver 또는 거부" → "외부 참조 허락 hook(Import·Null·Reject) 또는 거부", "저장 쪽 값 치환 hook" → "B4 `SubstituteStruct` 사용", 한도의 작업량 식(결정 5)·문자열 4 KiB 범위(결정 6)·값 64 MiB(결정 4)를 고친다.
2. **2절 사전 조사 표**: 객체 행의 보강 칸에 "다중 root Commit(root마다 RootRef)"과 이 문서를, 직렬화 행의 보강 칸에 A0-O 뒤 모양(graph v2 — 이 문서 5절)을 단다. "있는 것" 칸("RootRef는 첫 객체에만", "단일 root, 내부 Commit, …")은 2026-11-12 조사 기록이라 그대로 둔다.
3. **5.1절**: "export 집합은 Outer 체인이 이 패키지에 닿는 모든 객체다"에 "Registry 자식 고리가 붙잡으므로 GC 시점과 무관하다(이 문서 1.7절)"를, graph v2 노드 outer 0 = 패키지(Exports outerIndex 0)와 같음을 더한다.
4. **5.5절 205행 "값 치환 hook(A0-O)"** → "B4 `SubstituteStruct`".
5. **6.1절 `Package : Object` 항목**: "export 목록(`ROCKS_PROPERTY(Transient) ObjectRefArray<Object>` — 강한 간선)"을 지우고 "소속 객체는 Registry 자식 고리가 붙잡는다(패키지 정책 — outer → inner 강한 간선), export 순서는 `CollectInners(DepthFirst)`"로 바꾼다. "하드 의존 패키지 목록 — 하드 의존 보존"은 결정 8 뒤 GC 보존 역할이 중복이므로 "AssetManager 조회용 사본(GC 역할 없음 — 존치 여부는 A1이 정한다)"으로 낮춘다. "inner → outer 강한 간선(계획 §5.2)"에 outer → inner를 더한다. "에셋 객체 Registry의 모든 객체는 Package이거나 Outer 체인이 Package에 닿는다"에 "패키지는 이름이 없다(정체성은 AssetId)"를 더한다.
6. **6.1절 관찰자 규칙(228–232행)**: "`ObjectRef` 설정·해제" → "`ObjectRef`·`ObjectRefArray` 원소·`StructRefArray` 원소의 참조 멤버 설정·해제", 관찰 제외 "Package의 하드 의존 목록" → "패키지 객체가 소유한 간선 전부"(결정 10), "Discard가 나가는 계수를 한 번만 반납한다" → "Discard가 관찰자에 음수 다중도로 한 번 보고하고 AssetManager는 그것으로 반납한다", "이벤트를 큐에 넣기만 하고 … 큐를 비워" → "순 다중도 합산 표에 더하고 … `TakeChanges`로 꺼내 묶음 전체를 적용한 뒤 판정"(3절).
7. **11.2절**: DeserializeNative(worker)와 Game 단계의 경계 — `ObjectGraphStage::Validate`(worker 가능, 정책 사본·Outer 요약, hook 스레드 안전 요구)와 `CreateObjects` 사전 확인(Game)을 적는다.
8. **11.4절 344–345행**: "Pump가 처리하면 RootRef를 놓고 단위의 모든 패키지·export를 Discard한다" → "Pump가 처리하면 단위의 패키지마다 Discard(부분 트리)하고, 모두 성공한 뒤에 RootRef를 놓는다. Discard가 Busy면 RootRef를 쥔 채 다음 Pump에서 다시(이미 Discard한 패키지는 무동작). RootRef를 먼저 놓으면 그 사이 GC 조각이 Discard 없이 소속 객체를 회수해 관찰자 계수가 샌다", "이름이 비므로 같은 ID의 새 요청은 기다리지 않고" → "AssetManager의 ID 표에서 빠지므로 같은 ID의 새 요청은(패키지는 이름이 없고 하위 이름은 새 패키지 handle 아래라 충돌하지 않는다)", 근거 문단 "패키지에 속하고(6.1절)"에 소속 보존(결정 8)과 "Discard가 나가는 계수를 관찰자 보고로 한 번 반납"을 더한다.

RF(`Docs/Design/ReflectionFoundations.ko.md`):

9. **2.2절 122행·2.3절 129행**: graph v1 누락 정책(`allowMissingFields`) 서술을 지우고 누락 = 기준만 남긴다. 129행 EditorOnly 행에 "쿠킹 로드에서 EditorOnly 필드·멤버·원소가 있으면 InvalidArgument(`excludedFlags`)"를 더한다.
10. **4.6절 333행·6.2절 605행·6.4절 622행**: graph v1 누락·Required 서술(NotFound, "파일에 없음" 진단)은 구현된 B4·B3c 계약 설명이라 남기고, 이미 있던 graph v2 문장에 진단 시점(적용 뒤 2c)·present 판정의 A5 자리·이 문서 연결을 더한다. v1 서술은 Oc 구현 때 지운다.
11. **5.5절 405행**: 작업량 식(결정 5).
12. **5.6절 453행** encode 고정 계약에 "staged 검사 encode(2c)는 ConstructionContext 검사 중 표시로 대신한다"를 더한다.
13. **6.1절 565행 "A0-O가 채운다"·6.3절 "값 치환 hook 구현"** → "B4 `SubstituteStruct`".
14. **6.4절 591·598·601행**: 591행 `ObjectIndex()`를 "노드 위치(0부터) = localId − 1 = export index − 1"로, 598행에 `allowMissingFields`·`allowMissingFlags`는 A0-O에서 없어지고 `excludedFlags`로 바뀜을, 601행 "hook은 소유 객체를 Borrow한 상태에서 불리므로"에 "Required 실효 값 진단은 staged 객체라 Borrow 없이"를 더하고 "옵션 구조체는 다시 바뀌지 않는다"를 지운다(A0-O가 Load·Save 옵션을 바꾼다). hook 계약 "진단·수집은 1단계에서 한 번만"(605행)은 10번에서 고친다.
15. **결정 16**: "한도 확장(64 MiB)" → 필드 값 한도 64 MiB(결정 4).
16. **12절 1004행** "Instanced 하위 객체 배열(A0-O·A1)" → "(A1 또는 prefab 단계 — A0-O 범위 밖)".

EIP(`Docs/Design/EngineImplementationPlan.ko.md`) §5.2:

17. 471행 "Outer는 이름·패키지 계층이며 GC 소유권을 자동 의미하지 않는다", 475행 "소유·수명 관리는 Outer로 표현하지 않는다", 477행 "outer가 inner를 암묵적으로 살리지는 않는다 … 패키지 export table로 붙잡는다"에 "단 패키지 정책 Registry(에셋 객체 Registry)에서는 outer가 자손을 붙잡는다 — 자식 고리가 export table이다(저장 집합 결정성, ObjectFoundations 1.7절)"를 단다. 478행 "Outer 변경(Rename/이동)은 … 허용"은 A0-O 범위 밖임을 적는다(결정 2). 480행 근거 문단의 "5.6 소스로 대조하지 않았다"를 "5.6에서 Outer가 GC 참조임을 확인했다(FastReferenceCollector.h 995)"로 고치고 "UE 패키지는 export를 붙잡지 않는다 — Rocks의 패키지 정책은 이 점에서 UE와 다르다"를 더한다.

RCM(`Docs/Design/RuntimeClassMetadata.ko.md`):

18. **'클래스 기본값 결정' 66·75행**: "이 규칙은 … A0-O의 graph v2가 처음부터 갖는다"에 이 문서를 연결하고 "override 집합 자리(present 판정 `NotePresent`)도 A0-O가 남기고, 전부 저장의 집합은 그래프 바이트 밖, 차이 형식 여부는 호출자 옵션"을 덧붙인다.

EngineRuntime(`Docs/Design/EngineRuntime.ko.md`):

19. **131행** "graph v1로 Level 루트를 저장할 수 있다" → A0-O(Od1) 뒤에는 graph v2 편의 API(`SaveObjectGraph(root)`)로 같은 구조 시험을 한다는 것을 단다.
20. **134행** "World registry의 외부 생성 거부(정책 자리)" → 이 문서 1.3절 `worldOnlyCreationKey` 후속으로 연결한다.

ReflectionMetadata(`Docs/Implementation/ReflectionMetadata.ko.md` — A0-O 계획을 가리키는 문장만):

21. **89·92행**의 "(A0-O)" 언급을 이 문서로 연결한다(구현된 계약 서술은 바꾸지 않는다).

**(나) 코드 주석·구현 문서 — 구현 커밋에서 고친다**(지금 고치면 현재 코드와 어긋난다)

22. `ObjectRegistry.h` 컨테이너 API 주석(200·209·224행 "할당하지 않는다") — Ob(7절 표).
23. `FieldCodec.h`의 `FieldDecodePolicy` 주석(47–48행)·objectIndex(71행)·hook 계약(96–102행) — Oc·Od1.
24. `ObjectGraph.h` 주석, `ObjectSerialization/README.ko.md`("고아 객체 거부"·`allowMissingFields`·한도·형식) — Oc·Od1·Od2·Oe.
25. Docs/Implementation `Serialization.ko.md`(graph 형식·필드 codec)·`ObjectLifecycle.ko.md`(Outer·Name·Discard·다중 root Commit)·`IncrementalGarbageCollection.ko.md`(outer 간선·패키지 정책의 자식 방문·회수 조건) — Oa·Ob·Oc·Od.
26. `GcBudget` 주석 — 결정 15(고리 해체 없음)로 바꿀 것이 없다.

