# Rocks 런타임 클래스 메타데이터 설계

## 목적과 현재 상태

클래스를 실행 중 조회하는 정보와 더불어 전달·선택·저장·생성할 수 있는 데이터로 제공한다. 일반 코드의 사용법은 짧고 예측 가능해야 하며 타입 안전성, 등록 Registry, World, 객체와 모듈 수명을 생략해서는 안 된다. 기능의 범위는 Epic의 [UClass API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UClass), [객체 생성](https://dev.epicgames.com/documentation/unreal-engine/creating-objects-in-unreal-engine), [TSubclassOf](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/TSubclassOf)의 공개 설명을 참조한다. Rocks에서 해당 자료구조의 내부 구현이나 API 호환을 약속하지 않는다.

현재 `TypeId`, 불변 `TypeSchema`·필드 메타데이터, `RuntimeTypeOps`, `TypeRegistry`, `TypeLease`, 등록된 타입에 대한 상속·추상 정책과 낮은 수준의 객체 생성 기반이 있다. `ObjectRegistry::NewObject<Object>(access, typeId)`는 등록된 런타임 타입 ID를 넘겨 생성하는 경로다. 실제 지원 범위·테스트는 [Reflection 구현 상태](../Implementation/ReflectionMetadata.ko.md)와 [Foundation 상태](../Implementation/FoundationStatus.ko.md)를 따른다. 아래 메타클래스·클래스 참조·에디터·동적 모듈 기능은 목표이며 완료 선언이 아니다.

## 사용자 작업과 공개 형태

사용자는 다음 작업을 내부 Registry의 수동 조립 없이 표현할 수 있어야 한다. 표의 이름은 개념 예시이며 공개 API와 구현 완료를 의미하지 않는다.

| 작업 | 목표와 안전한 결과 |
|---|---|
| 정적 클래스 조회 | 현재 생성 헤더를 포함한 타입에서 `TypeIdOf<T>()`와 `TypeOf<T>()`를 제공한다. 직접적인 `T::StaticType()` 멤버는 생성 선언 누락, 파생 타입의 정적 함수 상속, 헤더·링크·동적 모듈 경계를 해결한 뒤 확정할 목표다. |
| 객체의 실제 클래스 | 현재 객체의 `GetType()` 및 `TryGetType()`으로 등록 타입의 lease를 얻는다. 유효한 Borrow·소유 스레드와 타입/모듈 수명을 확인한다. 명시된 World 문맥은 검사하지만, 현재 기본 설정에서 scope가 없으면 호출자의 실제 World는 판별하지 못한다. 정적 C++ 포인터 타입으로 대신하지 않는다. |
| 클래스를 값으로 전달 | 실행 시 등록된 클래스를 나타내는 안전한 클래스 객체 또는 핸들을 전달한다. 원시 메타데이터 주소와 영속 ID, Registry별 등록 세대를 혼동하지 않는다. |
| 기반 타입 제한 | `ClassRef<Base>`에 해당하는 타입 제한 참조로 Base 또는 유효한 파생 타입만 선택한다. 영속 값 타입 `ClassRef<Base>`(TypeId 하나, 쓰기·encode·복원에서 등록·클래스·Base 파생 검사)는 [리플렉션 B9](ReflectionExtensions.ko.md) B9d가 정하고 B9d2에서 구현했다(`Object/ClassRef.h` — `Resolve(TypeRegistry&)`, `TypeRegistry::AcquireDerivedClass`, ReflectionExtensions 5.2절). 메타데이터 상속만으로 안전한 C++ 포인터 변환을 추정하지 않는다. |
| 선택한 클래스로 생성 | 선택된 클래스의 추상·생성 가능성, 생성 대상 Registry·World, allocator·module lease를 검사하고 편의 생성 경로로 결과를 반환한다. 실패 시 임시 객체와 subobject를 롤백한다. |
| 클래스 기본값 | 타입 등록 세대(revision)에 붙은 불변 기본값 레코드를 경로로 조회하고, override 표시·기본값으로 되돌리기와 패키지 차이 저장의 기준으로 쓴다. 객체가 아니므로 World별 인스턴스와 변경 가능한 상태를 공유하지 않는다. 서브오브젝트 기본값은 prefab과 함께 후속이다('클래스 기본값 결정'). |
| 필드와 함수 | 상속된 필드·함수의 식별자, 선언 타입, 접근·호출 정책, 인자·결과·진단을 탐색한다. 현재 필드 지원 범위와 함수 반영 후속 범위를 구분한다. |
| 에디터와 에셋 | 타입 제한에 따라 선택 후보를 표시하고 추상·미지원 클래스에 이유를 제공한다. 선택 결과를 안정 ID로 저장하고 비동기 로드·누락·이름 변경을 처리한다. |

Blueprint는 범위에서 제외한다. C++ 및 자체 언어의 타입은 같은 등록·수명·검증 계약을 공유하되 언어별 호출과 메타데이터 생성은 어댑터로 확장한다. 에디터 머티리얼 그래프와 전문 도구 그래프는 이 결정과 별개다.

## 식별·등록·수명

- **선언 스키마:** 생성된 정적 schema와 stable `TypeId`는 선언의 식별과 내용이며 런타임 등록이나 모듈 수명 보유를 뜻하지 않는다. 생성 schema의 참조는 코드가 포함된 모듈 수명에 종속된다.
- **등록 클래스:** 특정 TypeRegistry의 `TypeId`·등록 세대·revision과 기반 관계·생성 정책을 나타낸다. 실행 중 사용할 메타데이터와 함수 포인터에는 `TypeLease` 또는 동등한 수명 핸들이 필요하다. 복사·소멸·unregister·shutdown의 의미를 명시한다.
- **타입 제한 참조:** 영속적으로 저장 가능한 ID 표현과 현재 Registry에서 검증·사용 가능한 등록 핸들을 구분한다. 등록 핸들의 재해석·캐시에는 Registry identity, generation과 module revision을 사용한다. 임시 참조에서 빌린 이름·필드·함수 포인터를 핸들 수명 밖으로 노출하지 않는다.
- **모듈 reload:** 구 등록 레코드·코드가 사용 중이면 unload를 대기하거나 `Busy`로 거부한다. 재등록 시 이전 handle을 새 객체로 해석하지 않는다. stale 캐시와 비동기 완료는 세대·revision 검사로 차단한다.
- **World·Tryout:** 메타데이터 레코드 공유 여부와 독립 World별 객체를 분리한다. 클래스 기본값은 World와 무관한 불변 레코드이며 World별로 두지 않는다('클래스 기본값 결정'). 자동 실행 문맥이 없는 경우 호출자의 World를 추측하거나 전역 singleton을 사용하지 않는다. 다른 Registry에 속한 타입 핸들과 callback의 혼합을 거부한다.
- **동시성:** 현재 객체 Registry의 소유 스레드와 안전점 규약을 유지한다. 타입 메타데이터의 불변 조회, 모듈 종료와 타입 등록 변경의 동기화·lease 반환을 별도 계약으로 정의한다. 무효 포인터나 소멸된 C++ 객체를 사후 조회해 안전하다고 주장하지 않는다.

## 타입 제한과 생성

기반 타입으로 제한한 클래스 참조는 할당·역직렬화·에디터 선택·등록 재연결·생성 직전에 실제 등록 타입의 상속 및 정책을 확인한다. 추상 클래스는 참조로 보관할 수 있는지와 실제 인스턴스 생성을 허용하는지를 별도 정책으로 정한다. 타입 없음·미등록·다른 Registry·추상·생성 불가·잘못된 기반·모듈 종료·stale revision·할당 실패는 혼합하지 않고 구분한다. 추상 생성은 현재 `Unsupported` 경로와의 호환성을 검토한다.

동적 생성은 클래스가 지정한 검증된 `RuntimeTypeOps`와 트랜잭션을 사용한다. 다른 Registry의 callback을 타입 ID만으로 섞지 않는다. 생성자 완료 전 비공개 상태, 기본값 적용(C++ 타입은 생성자가 같은 값을 만들므로 레코드를 다시 적용하지 않고 Debug에서 대조만 하며, Sparks 타입은 필드 초기값 덮기만 적용한다 — '클래스 기본값 결정'), 필요한 subobject 생성(prefab과 함께 후속), 불변식 검사와 공개를 순서대로 수행하며 실패 시 역순 정리한다. 소유권·GC root·World 문맥·발행 시점을 문서화한다. 사용자의 일상 코드는 소유 Registry나 context의 매 단계 조립 없이 의도에 맞는 호출을 사용하되, 복구할 실패와 불변식 위반을 감추지 않는다.

## 기본값·직렬화·도구

클래스 기본값은 타입 등록 세대(revision)에 묶인 불변 기본값 레코드다(아래 '클래스 기본값 결정'). 기본값을 객체로 두지 않으므로 Unreal의 CDO 같은 살아 있는 기본 객체가 없고, World별 기본값 override도 두지 않는다. default subobject graph의 객체 복제, 서로 공유되는 참조, 순환 참조, 수명과 쓰기 장벽은 레코드 밖이며 default subobject가 prefab과 함께 들어올 때 검증한다.

에셋·장기 저장에는 포인터나 현재 프로세스의 native key가 아닌 stable `TypeId`와 필요한 schema/version 정보를 기록한다. 타입 이름 변경·이전 이름·미등록 ID·폐기된 ID·충돌·병합 및 migration 실패를 진단한다. 비동기 로드가 늦게 완료되거나 Tryout·World가 종료된 경우 새 인스턴스를 부활시키지 않는다. 에디터는 기반 타입, 추상 여부, 생성 가능성, 플랫폼/모듈 가용성을 표시하고 검색·선택·누락을 구분한다. 사용자에게는 코드용 내부 구현보다 작업 가능한 상태와 진단을 제공한다.

### 클래스 기본값 결정 (2026-11-14)

CDO 기능은 필요하지만 UE식 살아 있는 객체 CDO 대신 타입별 불변 기본값 레코드를 두는 권고안이다(사용자 승인 2026-11-14). 설계 교차검증(실현성, 적대적 반례와 당시 기준 UE 5.6 대조, 최종 검증)의 지적을 반영했다. 아래 UE 사실은 2026-11-15에 UE 5.8(5.8.4, commit `f45729c0b6f0`) 원본으로 다시 확인했다(경로는 `Engine/Source/Runtime/CoreUObject/` 기준).

**UE 5.8에서 확인한 것.** UE는 클래스마다 실제 UObject인 CDO를 둔다(`UClass::CreateDefaultObject`가 부모 CDO를 먼저 만들고 `RF_Public|RF_ClassDefaultObject|RF_ArchetypeObject`로 할당하며 — Private/UObject/Class.cpp 5044·5059·5091행, 시작 때 compiled-in 클래스의 CDO를 모두 만든다 — UObjectBase.cpp `UObjectLoadAllCompiledInDefaultProperties` 882행). 저장은 비교 기준(archetype: 보통 CDO, 구성요소는 소유자 archetype 안의 같은 이름 객체)과 `Identical`한 속성을 쓰지 않는다(`UStruct::SerializeVersionedTaggedProperties` — Class.cpp 1577행, 판정 1992–2018행). 트랜잭션(undo) 저장은 이 생략을 끈다(`!IsTransacting()`, 1992행). 정수 `Identical`은 `==`이고 float·double은 `UE::PreciseFPEqual`이라 -0.0과 +0.0을 같게, NaN끼리는 payload와 무관하게 같게 본다(PropertyFloat.cpp 33행, PropertyDouble.cpp 34행, CorePreciseFP/Private/Math/PreciseFP.cpp). 일반 struct는 멤버 단위로 비교하고(`UScriptStruct::CompareScriptStruct`, Class.cpp 3663행), 배열은 통째로 비교·교체하며 원소는 기준 없이 쓴다(PropertyArray.cpp 89·188행). native 클래스는 인스턴스마다 생성자가 값을 채우고, `FObjectInitializer::InitProperties`는 기준이 CDO일 때 config 등 `PostConstructLink` 속성만 복사한다(UObjectGlobals.cpp 4450·4539–4573행). CDO는 `GetMutableDefault`와 config 로드로 실행 중 바뀐다. 값 비교만으로는 "기본값과 같은 값을 일부러 넣은 것"을 잃으므로 실험 기능 `FOverriddenPropertySet`으로 명시 override를 따로 기록한다(Public/UObject/OverriddenPropertySet.h 491행, "experimental, not supported" 표기). remote object 기능은 native CDO가 실행 중 바뀔 수 있어 이주의 차이 기준으로 쓰지 못하므로, 생성자로 새로 만든 불변 기준 객체(`ImmutableDefaultObject`, 비native 클래스는 CDO를 복제해 만든다 — Class.cpp 5717행)를 따로 둔다(5.8에서 바뀜: 5.6은 `UE_WITH_REMOTE_OBJECT_HANDLE`로 켰고, 5.8은 `UE_WITH_IMMUTABLE_CDO` = `UE_WITH_REMOTE_OBJECT_HANDLE && UE_AUTORTFM`으로 켠다 — Public/UObject/Class.h 144행. 동작은 같다).

Rocks는 같은 이점(기본값 변경 전파, 차이 저장, 기본값 조회·override 표시)을 얻되 기본값을 객체로 두지 않는다.

- **형태와 수명:** 기본값은 타입마다 하나인 **불변 기본값 레코드**다. 리플렉션 필드의 값을 그래프 저장과 같은 codec·wire 형식으로 인코딩한 값 블록이며(null 참조도 같은 wire), Registry 객체·GC 대상·World 장부 항목이 아니다. NATIVE 멤버와 Transient 필드는 담지 않는다. 레코드는 TypeRegistry 등록 레코드(등록 세대)에 붙어 TypeLease로만 접근하고 ModuleLease를 유지한다. 처음 필요할 때 지연 생성해 한 번만 게시한다.
- **레코드 생성 실패:** 생성자·할당·결정성·참조 제한 위반은 레코드를 요구한 작업(저장·로드·조회)의 오류로 돌려주고 전부 저장으로 대체하지 않는다. 할당 실패가 아닌 실패는 그 등록 세대 동안 기억한다. CI와 쿠킹은 모든 레코드를 강제로 만들어 실패를 미리 드러내고, CI가 Windows·macOS의 지문 목록을 대조한다.
- **레코드가 없는 타입:** 추상 타입, 필드 레이아웃이 완전하지 않은 타입(리플렉션 base를 둔 native 파생 — 저장 자체가 Unsupported, ReflectionFoundations 결정 21), 생성 함수표가 없는 수동 함수표 타입. 앞의 둘은 기본값 조회가 Unsupported이고, 저장 가능한 수동 함수표 타입은 늘 전부 저장한다.
- **만드는 법(C++):** 생성자를 실행한 임시 인스턴스(Registry에 공개하지 않음)를 인코딩한 뒤 파괴한다. 지금의 필드 encode는 Registry handle을 받으므로 Registry 밖 인스턴스를 인코딩하는 경로를 새로 두며, 그래프 저장과 같은 인코딩 본문을 쓴다. 생성자는 강한 참조를 설정할 수 없으므로(SetReference·AddReference는 Alive owner 필요) C++ 레코드의 객체 참조는 구조적으로 null이다.
- **생성자 결정성 계약:** 생성자는 값만 설정하고 Registry·World·전역 상태·다른 객체를 건드리지 않으며, 시각·난수·전역 계수·주소·설정과 호스트·구성마다 달라지는 CRT 초월 함수를 쓰지 않는다(상수식이나 A0-C 결정적 수학만). 레코드를 만들 때 두 번 생성해 인코딩을 비교하고, 다르면 진단과 함께 실패한다. 인스턴스마다 고유한 값(Guid 등)은 생성 뒤 도구·spawn이 넣는다 — 기준(빈 값)과 달라 늘 저장된다. 이 계약은 UE식 CDO를 써도 똑같이 필요하다.
- **만드는 법(Sparks):** Sparks 클래스는 C++ 엔진 타입을 상속하므로, 레코드는 가장 가까운 C++ base의 생성자 결과(Abstract여서 공개 레코드가 없어도 같은 방식으로 만든다) 위에 컴파일된 필드 초기값(상수식)을 덮어 합성한다. 지문은 base 결과에 의존한다. component 생성 초기값(SparksLanguage 4.5절)은 default subobject가 들어올 때까지 미지원이다.
- **적용 순서:** native 생성자 → Sparks 필드 초기값(이 둘의 결과가 레코드다) → prefab·variant → instance override → spawn parameter(EngineImplementationPlan 5.3절, SparksLanguage 4.5절). C++ 타입은 생성·로드에서 레코드를 다시 적용하지 않는다(생성자가 같은 값을 만든다, Debug에서 대조만). Sparks 타입은 초기값 덮기만 적용한다.
- **override 판정:** 저장할 값 = **명시 override 집합 ∪ 기준과 인코딩이 다른 값**. 집합의 단위는 값 경로의 끝(최상위 단일 값·배열 필드, struct는 멤버 끝까지 내려간 경로)이며 struct 필드 자체는 단위가 아니다. 편집기와 경로 쓰기가 기록하고, 차이 형식 로드는 파일에 있는 값 경로를 집합으로 삼는다. "기본값으로 되돌리기"는 레코드 값을 쓴 뒤(이 쓰기는 기록하지 않는다) 집합에서 지운다. C++ 코드가 멤버에 직접 쓴 값은 집합에 기록되지 않아 기준과 다를 때만 저장된다. 그래서 기본값과 같은 값을 일부러 넣은 값도 기본값이 바뀐 뒤 유지된다. Atomic struct(리플렉션 B9b — ReflectionExtensions 3.1절)는 그 struct 필드 경로가 단위이며 멤버로 내려가지 않는다.
- **같음의 정의:** 기준과 같은 wire 형식으로 인코딩한 byte의 비교다(UE `PreciseFPEqual`과 다르다: -0.0과 +0.0은 다르고 NaN은 payload까지 같아야 같다, 문자열은 정규화 없는 UTF-8 byte). non-null 참조는 byte가 아니라 대상 정체성(AssetId·객체 경로)으로 비교한다(패키지 상대 index는 패키지마다 다르다).
- **struct·배열:** struct 필드는 기본적으로 **멤버 단위**로 비교·저장한다(UE 태그 직렬화의 멤버별 비교와 같다). 단 `Atomic` struct는 **통째 단위**다: 멤버 하나라도 기준과 다르면 모든 멤버를 쓰고, override 단위·되돌리기도 그 struct 경로다. Core 값 타입 미러 17종 중 `Transform`(UE FTransform 대응)만 비Atomic이고 나머지는 Atomic이다. UE는 수학 타입을 native Serializer나 Immutable 이진 직렬화로 통째 쓰고, `FTransform`만 멤버 단위로 쓴다(ReflectionExtensions 3.1·18.2절). 비Atomic struct에서 저장 대상 멤버가 하나라도 있으면 그 struct Record를 쓰고 대상 멤버만 넣는다(중첩 struct는 재귀). 로드는 빠진 멤버를 기준 값으로 둔다. 배열 필드는 통째로 비교하고 저장된 배열은 통째로 교체한다. 배열 원소 struct는 기준이 없으므로 모든 멤버를 쓴다(UE와 같다). 원소 단위 차이는 안정 키를 가진 컨테이너에만 후속으로 허용하고 색인 기준은 쓰지 않는다.
- **차이 저장의 범위:** 패키지 저장(에셋·레벨·prefab)에만 쓴다. 세이브게임·undo·스냅샷·복제(duplicate)는 전부 저장이다(패치로 기본값이 바뀌어도 그 상태가 바뀌면 안 된다). "전부 저장"은 기준 차이 없이 모든 값을 쓴다는 뜻이다. 세이브게임의 필드 범위는 SaveGame 플래그가 정하고, 복제는 DuplicateTransient 계열 필드를 뺀다(리플렉션 B9h — ReflectionExtensions 9.1절). undo·스냅샷·복제의 전부 저장은 override 집합(값 경로 목록)을 함께 기록하고 로드가 복원한다(복제는 DuplicateTransient 계열 경로를 뺀다). "파일에 있는 값 = 집합"은 차이 형식 로드에만 적용한다.
- **누락 = 기준(형식 규칙):** 빠진 필드·멤버는 정책이 아니라 형식 규칙으로 허용되며 기준 값을 뜻한다(C++ 타입에서는 지금의 "누락 필드는 생성자 값 유지"와 같다). Required 진단은 기준을 적용한 뒤의 실효 값으로 낸다(빠졌다고 null로 보지 않는다). 레코드 인코딩은 `rejectMissingRequired`를 끄고 한다. 이 규칙은 레코드가 없어도 성립하므로 A0-O의 graph v2가 처음부터 갖는다(A5에서 형식 버전을 다시 올리지 않는다). A0-O는 1단계의 present 판정을 override 집합의 자리로 남기고, 전부 저장(undo·스냅샷·복제)의 override 집합은 그래프 바이트 밖 컨테이너에 두며, 차이 형식인지는 형식 비트가 아니라 호출자 옵션으로 정한다([ObjectFoundations](ObjectFoundations.ko.md) 4절).
- **마이그레이션:** 빠진 값은 "저장 당시 그 버전의 기본값"이다. 기본값만 바꾸는 것은 버전을 올리지 않는다(전파가 목적이다). 기존 인스턴스에 전파하지 않으려 하거나 필드의 의미(단위·분할)를 바꾸면 버전을 올리고, migrate가 값의 존재 여부를 받아 빠진 값에 옛 기본값(마이그레이션 코드의 상수)을 써 넣는다. struct 멤버의 기본값은 소유 클래스 생성자가 정하므로, struct 멤버의 전파 차단은 소유 클래스 버전을 올려 클래스 migrate가 한다. 엔진은 버전별 옛 레코드를 보관하지 않는다.
- **참조 기본값과 의존:** C++ 레코드의 강한 참조는 구조적으로 null이다. non-null 참조 기본값(Sparks `Asset(...)`·Soft·Row 초기값, SparksLanguage 3.2·7.2절)은 다음과 함께 허용한다 — 비교는 대상 정체성, 패키지 Dependencies = 저장된 참조 ∪ override하지 않은 값의 레코드 참조이며 로드 대조(AssetSystem 5.2절)도 기준 적용 결과로 한다, World 객체 참조는 거부. 그 전까지 레코드의 참조 기본값(Soft 포함)은 null만 허용하고 위반은 레코드 생성 오류다. 이 의존 수집과 default subobject는 Sparks의 해당 도입 단계(L2)의 선행 조건이다.
- **지문과 쿠킹:** 레코드 지문은 레코드 byte의 SHA-256이며 대상 구성(editor·cooked)의 EditorOnly 포함 여부와 codec wire 버전을 넣는다. 에셋 너겟 키는 처리기가 읽은 유효 값으로 만들므로 지문을 넣지 않는다(타입 전체 지문은 무관한 필드 변경까지 무효화한다). cookKey 지역 입력에 패키지 export 구체 클래스별 (TypeId, 지문) 정렬 목록을 더하고(base 기본값은 파생 레코드에 이미 들어 있다), 패키지 Summary 타입 표의 클래스 행에 저장 당시 지문을 기록한다. 쿠킹 패키지도 차이 형식이며(UE도 쿠킹 데이터를 기준 차이로 저장한다), 로드 때 지문이 다르면 `UnsupportedVersion`(AssetSystem 5.3절 타입 데이터 축, 다시 쿠킹)이다. 편집기 패키지는 지문이 다르면 "기본값이 바뀐 타입·영향받은 객체"를 진단하고 계속 연다.
- **조회·되돌리기:** 기본값 읽기는 레코드를 경로(PropertyPath)로 해석해 돌려준다. 되돌리기는 단일 값·원소·멤버부터 지원하고 struct·배열 통째 되돌리기는 후속이다(Atomic struct의 되돌리기는 통째이며 A5에서 이 후속과 함께 구현한다 — ReflectionExtensions 3.1절). ReadOnly 필드와 소유 컨테이너(`components` 등)는 되돌리기 대상이 아니다.
- **reload(SparksLanguage 12.4절):** reload는 새 등록 revision과 새 레코드를 만들고, 같은 revision의 레코드는 바뀌지 않는다. 살아 있는 인스턴스에서 override 집합에 없고 인코딩이 이전 레코드와 같은 값은 새 값으로 바꾼다. override 집합의 값은 유지하고 편집기에 표시한다. 이전 레코드는 reload가 끝날 때까지 보관한다.
- **기준의 일반화:** 조회 API와 차이 형식은 "기준(base)"을 명시한다. 지금 기준은 클래스 레코드이고, prefab이 생기면 archetype 사슬이 된다(UE도 archetype 기준이다).
- **아직 닿지 않는 곳:** OnAwake에서 만드는 구성요소는 소유자 코드가 넣은 값이 구성요소 타입 레코드와 비교되어 저장된다. default subobject(SubobjectGuid·인스턴스별 clone/remap, CoreObjectInputDetailedPlan 12.2절)가 prefab과 함께 들어올 때까지 구성요소에는 기본값 전파가 적용되지 않는다. World별 기본값 override는 두지 않는다.
- **불변성:** 실행 중 레코드를 바꾸는 경로를 두지 않는다(`GetMutableDefault`·CDO config 로드에 해당하는 것 없음). 프로젝트 설정으로 기본값을 바꾸는 기능이 필요해지면 레코드를 만들 때 합성하는 별도 계층으로 두고 지문은 최종 레코드로 계산한다.
- **시점:** 처음 필요한 곳은 A5(텍스처 설정의 UnsupportedSetting 판정, 빌드 키의 "기본값 포함")다. A0-O의 graph v2가 누락=기준 규칙과 실효 값 Required 진단을 처음부터 갖고, A5는 레코드·차이 쓰기·override 집합·지문·cookKey·Summary 지문 열을 더한다(A1 Summary, A4 cookKey 산출물을 그때 고친다). A5 전에 만든 패키지·fixture는 A5에서 다시 만든다(하위호환을 두지 않는 방침). Tick 설정(`TickSettings`)은 struct 멤버 단위 비교와 함께 A5에서 리플렉션으로 옮긴다(ReflectionFoundations 결정 21). 표현 방식(override 집합 자료구조, Registry 밖 인코딩 경로, 레코드 경로 해석기)은 A5 구현 명세에서 정한다.
- **거부한 대안 — UE식 객체 CDO:** WorldOnly 타입의 CDO는 어느 World에도 속하지 않는 장부 밖 객체가 되고, 코드 곳곳에 `IsDefaultObject` 분기가 생긴다. 상주 객체라 메모리·GC root를 차지하고 모듈 재등록 때 교체해야 한다. 가변 CDO는 이후 생성·차이 저장의 기준을 실행 중 바꿔 결정성을 깬다 — UE도 이 때문에 원격 이주에는 불변 기준 객체를 따로 만든다. 생성자 순수성 계약은 두 방식 모두 필요하므로 거부 사유가 아니다.

### 클래스 기본값 구현 명세 — A5a 레코드 (2026-11-23)

'클래스 기본값 결정'이 "A5 구현 명세에서 정한다"고 남긴 표현 방식 가운데 레코드 부분이다(차이 쓰기·override 집합은 A5b, 지문 연결은 A5c — [AssetSystem](AssetSystem/12-Textures.ko.md) 12.7절). 근거 조사는 2026-11-23 현재 코드이고, 설계 교차 검증(필수 4 — ObjectRegistry 의존·Soft 검출·수동 함수표 표지·할당 소유)을 반영했다.

**데이터와 자리.**
- `ClassDefaults`(ObjectRuntime `Object/ClassDefaults.h`, 불변): `editorRecord`(그래프 저장과 같은 wire의 클래스 Record byte — 저장 정책 editor: Transient만 뺀다), `cookedRecord`(저장 정책 cooked: EditorOnly 실효 필드·멤버도 뺀다 — 쿠킹 저장과 같은 `excludedFlags`·`includeFlags`로 다시 인코딩한다), `editorFingerprint`·`cookedFingerprint`. 구성은 저장 정책의 제외 플래그 집합으로 식별한다. 다른 구성(예: 전용 서버 쿠킹의 클라이언트 전용 제외)이 생기면 구성을 더한다.
- 자리는 `TypeRegistry::Record`(등록 세대 하나)의 지연 슬롯이다. 원자 칸 하나에 상태(없음·게시됨·실패 기억)와 게시 포인터를 함께 담는다. 읽기는 acquire 적재로 잠금 없이 하고, 게시는 release 저장이다. 생성은 TypeRegistry의 생성 전용 잠금(`defaultsGate_` — `gate_`와 다른 잠금, ClassRef 판정이 `gate_`를 잡으므로) 안에서 한 번만 한다. 잠금을 잡은 뒤 상태를 다시 보므로 같은 타입을 두 번 만들지 않는다. `DestroyRecord`가 해제한다. 등록 세대가 바뀌면(재등록·reload) 새 슬롯이라 옛 레코드·실패 기억이 따라오지 않는다.
- **할당 소유**: 게시하는 `ClassDefaults`와 그 byte 배열, 실패 기억의 진단(필드 경로 포함)은 TypeRegistry allocator로 만든다(게시 시도가 TypeRegistry 안에서 복사한다 — `DestroyRecord`가 같은 allocator로 해제). 그래서 TypeRegistry 계약에 "allocator는 스레드 안전해야 한다(레코드 게시는 생성 스레드 밖에서도 할당한다)"를 더한다. 구현 때 기존 TypeRegistry 생성처(엔진 기본 allocator·시험의 할당 실패 allocator)가 넘기는 allocator를 점검한다. 임시 인스턴스와 작업 버퍼는 호출자 allocator다.
- `TypeLease`에 접근자를 더한다: `ResolveClassDefaults(builder, user, diagnostic)`(게시된 상태 읽기, 없으면 생성 잠금 안에서 builder를 한 번 불러 게시하거나 실패를 기억 — 잠금은 TypeRegistry가 잡는다)와 `HasGeneratedFieldCodecs()`(base 사슬 표지 판정). 레코드 수명은 lease가 쥔 등록 레코드 수명이다(lease 밖으로 byte를 넘기지 않는다).
- 만드는 쪽은 인코딩 본문이 있는 ObjectSerialization이다: `AcquireClassDefaults(const TypeLease& lease, Allocator& scratch, ClassDefaultsDiagnostic* diagnostic = nullptr) → Result<const ClassDefaults*>`. ObjectRegistry를 받지 않는다 — 레코드는 TypeRegistry 등록 세대에 속하므로 특정 ObjectRegistry의 상태(소유 스레드·GC 처리 중·종료)에 성패가 걸리면 안 된다. 그래서 어느 스레드에서나 부를 수 있다.
  - ClassRef 기본값(값은 TypeId — 생성자가 non-null을 넣을 수 있다)은 복원 1단계처럼 `TypeRegistry::Acquire`로 판정한다(잠금 보호).
  - Name은 C++ 생성자가 풀에 닿을 수 없어 None만 만든다. 문맥의 Name 풀은 없고, None이 아닌 Name이 나오면 결정적 codec 위반이다.
  - BulkDataRef(AssetRuntime)는 생성자가 bulk id를 만들 수 없어 구조적으로 비어 있다(저장의 bulk 재번호 hook과 엇갈리지 않는다).

**레코드가 없는 타입**(Unsupported — 기억한다, 호출자는 전부 저장):
- 추상(`construct` 없음), `completeFieldLayout` 거짓.
- 생성 코덱 표지가 없는 타입(`ManualFields`). MetaCompiler가 생성 함수표에 `ops.generatedFieldCodecs = true`를 한 줄 더 낸다. 표지는 모든 필드 thunk가 생성 코드(`EncodeGeneratedField`·`EncodeBitfieldField`)라는 뜻이고, base 사슬의 모든 등록 레코드에 표지가 있어야 한다(생성 클래스가 손 함수표 base를 상속하면 base 필드가 원본 모드를 모른다). 손으로 쓴 함수표는 encode가 원본 모드를 모르므로 표지가 없고, 레코드 없이 전부 저장한다.
- Sparks 등록 타입은 지금 손 함수표로 등록되어 표지가 없으므로 같은 규칙으로 레코드가 없다. 초기값 합성(위 결정)은 Sparks L2 단계에서 그 등록이 표지와 합성 경로를 함께 가져온다.

**Registry 밖 인코딩 경로**:
- 필드 encode thunk 서명의 Registry 인자를 `ObjectRegistry*`로 바꾼다(원본 모드에는 Registry가 없다 — 가짜 Registry를 넘기지 않는다. 생성 코드의 thunk 람다 한 줄과 손 함수표·시험 thunk가 바뀐다, 그 밖에는 null이 아니다). `FieldEncodeContext`에 원본 모드를 더한다: 가상 `RawSource() → const Object*`(기본 null). 생성 class 필드 thunk(`EncodeGeneratedField` — 컨테이너·Optional·Map·InstancedStruct·정적 배열·struct·Text 모두)와 bitfield thunk(`EncodeBitfieldField`)는 `RawSource()`가 있으면 Registry `Resolve`·staged 대신 그 객체에서 멤버를 읽는다. 선례는 같은 두 함수의 staged 분기다. 그 뒤는 같은 `EncodeMemberValue`라 byte가 같다. struct 멤버 thunk는 원래 `const void*`를 받는다. 수동 함수표 경로(`EncodeThroughFieldValue`)는 원본 모드에서 Unsupported다(표지 판정이 먼저 막지만 방어로 둔다 — 실제로 나오면 다른 codec Unsupported와 구별되지 않아 `EncodeFailed`다).
- 객체 하나의 필드 루프(필드 범위·제외 판정·ops 찾기·encode·wire 재확인·`RecordBuilder::AddField` — 지금 `SaveNodeFields` 375–400행)를 공용 함수로 꺼낸다. pin·`BeginObject`·budget·위치 rebase·파일 byte 누계는 그래프 저장이 그 앞뒤에서 한다. 필드마다 그래프 저장만 하는 일이 있으면 그 지점을 콜백 인자로 둔다(구현 때 확정). `ValidateLayout`은 레코드도 먼저 부른다.
- 레코드 문맥 `ClassDefaultsEncodeContext`(ObjectSerialization 내부):
  - 강한 참조 null은 그래프 저장과 같은 `U32 0`이다. non-null 강한·weak 참조는 실패한다(이유 `NonNullReference` + 필드 경로).
  - Soft 참조는 hook을 거쳐서만 문맥에 닿으므로(비가상 `SubstituteSoftReference`·`CollectSoftReference`) 레코드 전용 내부 hook을 둔다: `CollectSoftReference`는 경로를 담아 `NonNullReference`로 실패, `SubstituteStruct`·`SubstituteSoftReference`는 값 그대로, `Report`는 진단을 모은다(치환 없음, 수집은 거부용).
  - 한도는 그래프 저장 기본 한도(`ObjectGraphLimits{}`)다. 객체 하나의 Record는 그래프 전체 한도 안에 들므로, 같은 객체를 저장할 수 있으면 레코드도 한도에 걸리지 않는다(결정적 CapacityExceeded로 기억되어 저장 가능한 타입이 막히는 일이 없다).
  - Required 검사는 끈다(`rejectMissingRequired` 거짓 — 결정의 레코드 인코딩 규칙).

**만드는 순서**(생성 잠금 안):
1. 슬롯 상태를 다시 본다(게시됨이면 그것, 실패 기억이면 그 오류·진단).
2. 레코드가 없는 타입 판정.
3. 임시 인스턴스 둘을 동시에 산 채로 만든다(주소에 기대는 값이 저장소 재사용으로 가려지지 않게): `ops.size`·`ops.alignment`로 받은 저장소마다 `ops.construct(memory, scratch)`. 소멸은 `ops.destroy(object)` 뒤 **저장소 시작 주소**를 반환한다(construct가 준 `Object*`는 vfptr 등으로 저장소 시작과 다를 수 있다). handle이 없는 객체라 생성자에서 Registry API를 쓸 수 없다(Create도 construct 뒤에 Registry를 세운다 — 생성자 순수성 계약과 같다). 생성자는 생성 잠금 안에서 돌므로 순수성 계약에 "잠금을 잡지 않고 레코드 생성을 다시 부르지 않는다"를 더한다. 아직 게시되지 않은 타입을 같은 스레드가 builder 안에서 다시 부르면 Fatal이다(모든 구성 — 게시된 타입은 잠금 없는 빠른 경로로 바로 돌아간다).
4. 두 인스턴스의 editor 인코딩을 byte 비교한다. 다르면 이유 `NonDeterministic`(첫 다른 필드 id)이다.
5. 첫 인스턴스를 cooked 정책으로 한 번 더 인코딩한다.
6. 지문: `SHA-256("RocksClassDefaults" ‖ 구성 byte ‖ ObjectGraphFormatVersion(u32 LE) ‖ 레코드 byte)`. `ObjectGraphFormatVersion`은 그래프 형식 버전(지금 `graph_detail::FormatVersion` = 2 — 공개 상수로 올린다)이고 결정의 "codec wire 버전"이 이것이다. Record 머리에 schema id·version이 있으므로 타입 버전이 오르면 지문도 바뀐다(의도 — 쿠킹 패키지가 다시 쿠킹된다). 구성 byte 표: 0 = editor(제외 {Transient}), 1 = cooked(제외 {Transient, EditorOnly}). 구성이 늘면 다음 번호를 주고 번호를 다시 쓰지 않는다.
7. 게시. 기억하는 실패는 결정적 이유뿐이다: 레코드 없는 타입(셋), `NonDeterministic`, `NonNullReference`, 결정적 codec 위반(InvalidArgument·TypeMismatch 등 입력이 같으면 늘 같은 오류). OutOfMemory·Busy·ShuttingDown·NotFound(ClassRef 대상이 아직 등록 전)·접근 오류는 기억하지 않는다(다음 호출이 다시 만든다). ClassRef 대상만 재등록되어 "base 파생 아님"으로 기억한 실패가 이 타입 세대에 남는 경우는 C++ 타입이 재등록되지 않아 지금 없다 — Sparks L2(reload)에서 다시 본다.

**경로 조회**: `FindDefaultPayload(lease, const ClassDefaults&, const PropertyPathBuffer& path, ClassDefaultsConfiguration, Allocator& scratch) → Result<DefaultPayload{wire, bytes}>`. path는 그 타입 스키마로 해석한 것이다(`ParsePropertyPath`). 필드 → (비Atomic struct면) 멤버 사슬까지 Record를 따라 내려가 그 값의 wire와 payload byte를 준다. 정적 배열 원소는 구현 때 Unsupported로 정했다(ValueArray reader가 원소 byte 구간을 주지 않는다 — 되돌리기는 정적 배열 필드 통째로 한다). Atomic struct는 그 struct 경로까지만이다(멤버 경로는 InvalidArgument). 동적 배열 원소·Map 키·Set·Optional 안쪽도 Unsupported다(기준이 없다 — 결정의 원소 단위 차이 후속). cooked로 조회한 EditorOnly 경로와 레코드에 없는 경로는 NotFound다. 쓰는 곳: A5b 되돌리기(payload를 그 경로에 decode), A5d 계약 설정 판정(필드 payload byte 비교). 값으로 푸는 텍스트 조회는 ReflectionText 쪽 후속이다.

**전부 만들기**: `ForEachClassDefaults(TypeRegistry&, Allocator& scratch, callback)` — 등록 목록의 TypeId를 `gate_` 아래에서 복사한 뒤(어느 스레드에서나) 하나씩 Acquire해 lease를 쥐고, 등록된 구체 클래스(추상은 뺀다)의 레코드를 만들어 (TypeId, 지문 둘 또는 실패 진단)을 넘긴다. 쿠킹 시작과 CI 시험이 쓴다(결정의 "실패를 미리 드러낸다"). Windows·macOS 지문 대조는 이 목록의 golden 덤프 시험이다(A5c).

**진단** `ClassDefaultsDiagnostic{reason, error, type, fieldPath}`(fieldPath는 고정 크기 `PathStep` 배열 — 할당 없이 복사·기억한다, 텍스트는 `FormatPropertyPath`) — reason: `None`·`Abstract`·`IncompleteLayout`·`ManualFields`·`NonDeterministic`·`NonNullReference`·`EncodeFailed`(그 밖의 codec 오류 — 오류 코드 보존). 오류 코드는 Unsupported(앞 셋)·InvalidArgument(NonDeterministic·NonNullReference)·codec 오류 그대로·OutOfMemory. 실패를 기억하면 진단(경로 포함)도 함께 기억해 두 번째 호출이 같은 진단을 낸다.

**버린 대안**: 레코드를 살아 있는 객체로 두기(결정의 거부 대안), 별도 raw thunk 칸(생성 코드가 커지고 같은 본문을 두 경로가 부른다 — 문맥 모드 + 표지 한 줄로 충분하다), 레코드를 ObjectRegistry마다 두기·ObjectRegistry로 만들기(레코드는 타입 등록 세대에 속한다 — Registry 상태에 성패가 걸리고 일시 오류가 기억된다), CAS 경쟁 게시(진 쪽이 헛일을 하고 TypeRegistry allocator 사용이 두 배다 — 생성 잠금으로 한 번만), cooked 레코드를 editor 레코드에서 걸러 만들기(struct 안 EditorOnly 실효 멤버는 다시 인코딩해야 정확하다).

**시험(A5a)**:
- 같음: 기본값 fixture 클래스(정수·float·문자열·enum·struct(비Atomic·Atomic)·배열·Set·Map·Optional·정적 배열·Text·InstancedStruct(빈·비어 있지 않은)·Name None·ClassRef(null·non-null)·null 강한 참조(필드·struct·컨테이너 안)·EditorOnly 필드·EditorOnly 전파(struct 필드 플래그·EditorOnly struct 타입)·Transient·bitfield·상속 필드·가상 함수가 있는 클래스)의 editor 레코드 = 같은 객체의 editor 그래프 저장 Record, cooked 레코드 = cooked 그래프 저장 Record.
- 지문: 값 고정(같은 입력 같은 값, 구성마다 다름, 타입 버전이 오르면 다름).
- 레코드 없는 타입 셋(손 함수표는 `EncodeThroughFieldValue`가 아닌 직접 만든 encode 포함)과 기억.
- 비결정 생성자 NonDeterministic 기억, non-null Soft(필드·struct·배열·Map·Optional·InstancedStruct 안 — 경로) NonNullReference 기억, 두 번째 호출 진단 같음.
- 할당 실패 모든 지점 OutOfMemory·기억 안 함·다시 성공(TypeRegistry allocator·scratch 둘 다).
- ClassRef 대상 미등록 NotFound 기억 안 함 → 등록 뒤 성공.
- 스레드: 다른 스레드가 만든 레코드를 이 스레드가 받음(builder 한 번, 같은 레코드). 생성 잠금 경쟁 자체는 sleep 없이 결정적으로 만들 수 없어(잠금 대기를 관찰할 수 없다) 잠금 뒤 다시 보기는 코드 검토로 확인한다. 같은 스레드 재진입 Fatal은 프로세스를 끝내 시험하지 않는다.
- ObjectRegistry 독립: ObjectRegistry 둘(같은 TypeRegistry)에서 저장 비교 기준이 같은 byte.
- 재등록 뒤 새 슬롯.
- 경로 조회: 필드·struct 멤버·두 단계 멤버·Atomic struct 통째·Atomic 멤버 InvalidArgument·정적 배열 원소 Unsupported·cooked의 EditorOnly 필드·멤버 NotFound·빈 경로.
- `ForEachClassDefaults` 목록(추상 제외·실패 진단 포함).

## 단계와 완료 기준

1. **타입 접근 기반:** 정적 생성 정보와 동적 등록 정보를 구분하는 API, lease·World·Borrow 및 오류 경로 검증. 생성기가 반영 클래스를 누락하거나 파생 클래스가 부모의 정적 함수로 잘못 식별되지 않아야 한다.
2. **타입 제한 참조:** 영속 값 타입 `ClassRef<Base>`는 리플렉션 B9 B9d2가 더했다(ReflectionExtensions 5.2절 — 값은 TypeId만 담고 `Resolve`가 쓸 때마다 등록·Base 파생을 다시 확인한다). 이 단계는 그 위의 등록 핸들(세대·revision 캐시)을 다룬다. 적합한 기반·파생·추상 타입과 무관한 타입, 동일 ID의 다른 Registry, unregister·재등록·module revision, 입력 실패 시 기존 참조 보존을 검증한다.
3. **안전한 동적 생성과 기본값:** 정상·추상·실패 생성, 기본값 레코드(생성 실패·결정성 대조·지연 게시), rollback, GC 도중 생성 및 World 종료를 검증한다. subobject·공유 참조·순환은 default subobject가 prefab과 함께 들어올 때 검증한다('클래스 기본값 결정'). 오류마다 남은 객체·lease·할당은 0이어야 한다.
4. **필드·함수와 자체 언어:** 자동 metadata와 안전 호출, 인자·반환 유형·접근 권한·예외 경계·module lease를 검증한다. Blueprint는 도입하지 않는다.
5. **에디터·에셋·Tryout:** 후보 필터·저장·재로딩·타입 이름 변경 및 누락, 여러 World와 Tryout 세션, reload·늦은 비동기 완료를 실제 통합 하네스로 검증한다.

각 단계는 정상·음성 단위 테스트와 공개 API만을 사용하는 짧은 실제 코드, 실패 진단, allocator·lease 회계, Windows/macOS 결과 및 독립 교차 검토를 완료 조건으로 한다. 해당 운영체제나 대상 기능을 실행하지 못했으면 완료로 보고하지 않는다.
