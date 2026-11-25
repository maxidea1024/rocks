# Rocks 충돌 데이터: 채널·응답·profile·body 값

기준일: 2026-11-25, 개정 2(CD1 구현). 개정 2: CD1 구현과 리뷰어 1명의 UE 대조(치명 0, 중요 4, 경미 9) 반영 — 새 모듈 `Collision`(CDD1), redirect 우선순위·중복 규칙을 UE와 맞춤, 없는 profile 해석의 UE 차이와 CD2 의무, UE와 다른 점 표(4.1절), CD3 항목 보강.

이 문서는 물리 시스템 없이 정할 수 있는 충돌 데이터를 정한다. 다루는 것은 충돌 채널·응답·충돌 켬 상태, profile 표(UE `UCollisionProfile`), 컴포넌트의 body 값(UE `FBodyInstance`의 영속 필드)이다. 물리 모듈(CR16)과 Jolt 어댑터는 이 데이터를 읽기만 한다. 상태 표기는 **설계**(코드 없음)와 **구현**(단계 표 9절)이다. UE 경로 약어는 `E/` = `Engine/Source/Runtime/Engine/`, `PC/` = `Engine/Source/Runtime/PhysicsCore/`, `Config/` = UE `Engine/Config/`(UE 5.8.4)다.

근거:

- 사용자 결정(2026-11-24): 렌더·물리 없이 정할 수 있는 컴포넌트 데이터는 먼저 넣는다(엔진 런타임 24장).
- 사용자 선택(2026-11-25): `PrimitiveComponent`(엔진 런타임 24.3절 D2)는 D2 세션이 맡고, 충돌 데이터(채널·응답·profile·body 값)는 Jolt 담당 세션이 별도 파일로 설계·구현한다. D2 세션이 가져다 쓴다(9절 CD2).
- D2의 현재 자리: `PrimitiveComponent::collisionProfileName`(영속 `String`, 빈 값 = 클래스 기본)만 있고 표는 없다(엔진 런타임 24.3.4절, ERD40). 이 문서가 그 표를 만든다.
- [물리 어댑터 경계](PhysicsJoltAdapter.ko.md)(좌표·단위), [컴포넌트 도입](ComponentCatalog/04-Prerequisites.ko.md) CR16, [설정 체계](ConfigSystem.ko.md)(설정 struct·배열 연산).

## 1. 범위

| 들인다(CD1) | 나중 |
|---|---|
| 채널 64개, 응답 셋, 충돌 켬 여섯 값과 판정 함수 | 질의 API(trace·sweep·overlap — CR16) |
| 응답 묶음(채널별 응답 64칸)과 연산 | Jolt 필터(ObjectLayer·BroadPhaseLayer) 사상(CR16, 8절) |
| profile 설정 struct와 엔진 기본 데이터(UE `BaseEngine.ini`와 같은 내용) | 편집기 profile 편집 화면 |
| profile 표 생성(검사·진단)과 이름 조회 | 설정 hot reload 때 표 다시 만들기(CD2 뒤) |
| profile 이름 + 컴포넌트 값 → 실효 충돌 값 해석 | 컴포넌트 setter·World 연결(CD2, D2 세션) |
| body 값 struct(`BodySettings`) | 물리 재질 에셋 참조(물리 재질 에셋 단계) |

## 2. 기본 타입

모듈은 새 Engine 런타임 모듈 `Collision`이다(`Engine/Source/Runtime/Collision`, 의존 ObjectRuntime·CoreTypeBindings·Config). 헤더는 `<Collision/CollisionTypes.h>`·`CollisionResponses.h`·`CollisionSettings.h`·`CollisionProfileTable.h`·`BodySettings.h`이고, 리플렉션 등록 함수는 `RegisterCollisionTypes`, 설정 등록은 모듈 진입점의 `RegisterSettings`다. `World`(`PrimitiveComponent`)와 `Mesh`(에셋 단순 충돌 `SimpleCollision`)가 이 모듈에 의존한다(CDD1).

### 2.1 채널

`CollisionChannel : uint8`(리플렉션 enum, UE `ECollisionChannel` — `E/Classes/Engine/EngineTypes.h:1098-1168`). 값과 순서는 UE 5.8과 같다.

| 번호 | 이름 | 종류(기본) |
|---|---|---|
| 0 | `WorldStatic` | object, 정적 |
| 1 | `WorldDynamic` | object |
| 2 | `Pawn` | object |
| 3 | `Visibility` | trace |
| 4 | `Camera` | trace |
| 5 | `PhysicsBody` | object |
| 6 | `Vehicle` | object |
| 7 | `Destructible` | object |
| 8–13 | `EngineTraceChannel1`–`EngineTraceChannel6` | 엔진 예약(이름이 없으면 미사용). 8 = `GizmoCollisionChannel`(UE `COLLISION_GIZMO` — `EngineTypes.h:1183`) |
| 14–63 | `GameTraceChannel1`–`GameTraceChannel50` | 프로젝트 사용자 채널(이름이 없으면 미사용) |

- `CollisionChannelCount = 64`. UE의 `ECC_OverlapAll_Deprecated`·`ECC_MAX`는 들이지 않는다(사용 중단 값, 하위호환 없음).
- 사용자 지정 가능 범위는 8–63이다(UE `IS_VALID_COLLISIONCHANNEL` = `Destructible` 초과·`OverlapAll_Deprecated` 미만 — `E/Private/Collision/CollisionProfile.cpp:14`).
- 채널 이름은 UE 표시 이름(`ECC_` 접두를 뗀 이름)과 같다. 사용자 채널에 이름을 주면 그 이름이 표시 이름이 된다(4절).

### 2.2 응답과 충돌 켬

- `CollisionResponse : uint8 { Ignore, Overlap, Block }`(UE `ECollisionResponse` — `EngineTypes.h:1346-1352`, `ECR_MAX` 제외).
- `CollisionEnabled : uint8 { NoCollision, QueryOnly, PhysicsOnly, QueryAndPhysics, ProbeOnly, QueryAndProbe }`(UE `ECollisionEnabled::Type` — `EngineTypes.h:1805-1830`).
- 판정 함수(UE와 같은 규칙 — `EngineTypes.h:1847-1900`):
  - `CollisionEnabledHasQuery`: `QueryOnly`·`QueryAndPhysics`·`QueryAndProbe`.
  - `CollisionEnabledHasPhysics`: `PhysicsOnly`·`QueryAndPhysics`.
  - `CollisionEnabledHasProbe`: `ProbeOnly`·`QueryAndProbe`.
  - `CollisionEnabledIntersection(a, b)`: UE의 5×5 행렬(`EngineTypes.h:1883-1891`)을 그대로 쓰고, 한쪽이 `NoCollision`이면 `NoCollision`이다. UE 주석의 규칙 문장(query는 둘 다, probe는 어느 한쪽, physics는 둘 다이고 probe 없음)은 근사라 행렬과 다른 칸이 있다(예: `QueryOnly` ∩ `ProbeOnly` = `NoCollision`). 시험이 36쌍 전부를 UE 행렬과 대조한다.

### 2.3 응답 묶음

`CollisionResponses`(UE `FCollisionResponseContainer` — `EngineTypes.h:1445-1780`). 채널 번호로 색인하는 `CollisionResponse` 64칸 값 타입이다. 리플렉션 타입이 아니다(영속은 2.4절의 차이 목록으로 한다).

| 연산 | 뜻 | UE |
|---|---|---|
| 생성(응답 하나) | 64칸을 그 응답으로 | `FCollisionResponseContainer(ECollisionResponse)` |
| `Get(channel)`·`Set(channel, response)` | 한 칸. `Set`은 바뀌었는지를 돌려준다 | `GetResponse`·`SetResponse` |
| `SetAll(response)` | 전부 | `SetAllChannels` |
| `Replace(old, new)` | old인 칸만 new로. old인 칸이 있었으면 참(old = new여도 — UE와 같다) | `ReplaceChannels`(`E/Private/PhysicsEngine/PhysUtils.cpp:234-247`) |
| `Min(a, b)` | 칸마다 작은 응답(Ignore < Overlap < Block) | `CreateMinContainer` |
| `==` | 64칸 비교 | `operator==` |

### 2.4 채널 응답 한 칸(영속)

`ChannelResponse`(리플렉션 struct, UE `FResponseChannel` — `EngineTypes.h:1415-1440`): `String channel`(채널 표시 이름), `CollisionResponse response = Block`. profile 설정(3절)과 컴포넌트의 사용자 지정 응답(6절)이 같이 쓴다. 채널을 번호가 아니라 이름으로 적는 것은 UE와 같다(사용자 채널 번호를 바꿔도 이름으로 따라간다).

## 3. profile 설정

`CollisionSettings`(설정 struct — `ROCKS_STRUCT(Settings(Branch = "Engine", Section = "Collision"))`, UE `UCollisionProfile`의 globalconfig 필드 — `E/Classes/Engine/CollisionProfile.h:167-184`).

| 필드 | 원소 | UE |
|---|---|---|
| `profiles` | `CollisionProfileSetup`: `name`, `collisionEnabled`(기본 `NoCollision`), `objectTypeName`(채널 이름, 빈 값 = `WorldStatic`), `customResponses`(`ChannelResponse` 배열), `helpMessage`, `canModify`(기본 참) | `FCollisionResponseTemplate`(`:44-90`, 기본값 `CollisionProfile.cpp:20-27`) |
| `channels` | `CollisionChannelSetup`: `channel`(8–63, 기본 `WorldStatic` — 빠뜨리면 설정할 수 없는 채널로 진단, UE 기본값과 같다), `name`, `defaultResponse`(기본 `Block`), `traceType`(거짓 = object), `staticObject` | `DefaultChannelResponses`·`FCustomChannelSetup`(`:96-120`) |
| `profileEdits` | `CollisionProfileEdit`: `name`, `customResponses` | `EditProfiles`·`FCustomProfile`(`:142-153`) |
| `profileRedirects`·`channelRedirects` | `CollisionNameRedirect`: `oldName`, `newName` | `ProfileRedirects`·`CollisionChannelRedirects`(`FRedirector`) |

- 이름은 `String`이고 비교는 ASCII 대소문자 무시다(UE `FName` 비교와 같고, D2의 `collisionProfileName`과 같은 규칙). Rocks `Name`을 쓰지 않는 이유: `Name`은 실행 세션 pool 토큰이라 설정 struct·표가 pool 수명에 묶인다. 이름 수가 적어 문자열 비교 비용이 문제 되지 않는다.
- bool 필드에 `b` 접두를 붙이지 않는다(Rocks 규칙 — `canModify`·`traceType`·`staticObject`).
- `helpMessage`는 UE에서 `WITH_EDITORONLY_DATA`(기본 "Needs description")다. Rocks 설정 struct에는 아직 출시 제외 지정자가 없어(설정 체계 7절 — `ShippingExcluded`는 CF5 쿠킹 때 더한다) 지금은 일반 필드이고, CF5 때 `ShippingExcluded`를 단다.
- **엔진 기본 데이터**는 `Engine/Config/Engine.json`의 `"Collision"` 섹션에 둔다(UE `Config/BaseEngine.ini:3102-3121`의 `+Profiles` 19개와 같은 내용 — 이름·충돌 켬·object type·사용자 지정 응답·설명·`canModify`). 코드 기본값은 빈 배열이다(`TracePresets`와 같은 방식 — 설정 체계 1절, 기본 데이터는 엔진 층 파일). 프로젝트는 자기 `Config/Engine.json`에서 배열 연산(`$add`·`$remove` 등 — 설정 체계 4.2절)으로 profile·채널을 더한다.
- **들이지 않는 UE 기본 데이터**: `+OldProfiles` 5개, `+ProfileRedirects` 5개, `+CollisionChannelRedirects` 4개(`BaseEngine.ini:3123-3138`)는 옛 이름을 새 이름으로 옮기는 하위호환 데이터라 들이지 않는다(하위호환 없음 원칙). redirect 기능 자체는 두어 프로젝트가 profile·채널 이름을 바꿀 때 쓴다(기본 목록은 빈 배열).
- `DefaultProjectile` 같은 UE 상수 이름 중 `BaseEngine.ini`에 없는 것은 게임이 정의하는 것이라 엔진 데이터에 넣지 않는다.

## 4. profile 표

`CollisionProfileTable`은 `CollisionSettings`에서 만드는 불변 값이다. `BuildCollisionProfileTable(const CollisionSettings&, Allocator&) -> Result<CollisionProfileTable>`로 만든다. 할당 실패만 오류이고, 아래의 의미 오류는 그 항목을 빼고 진단(`CollisionProfileDiagnostic` 목록 — 종류·대상 이름·관련 이름)을 남긴다. 이것은 UE와 같다(UE도 로그를 남기고 항목을 지운다). 모양 오류(모르는 키·타입 불일치)는 설정 import가 이미 거부한다.

만드는 순서(UE `LoadProfileConfig` — `CollisionProfile.cpp` 와 `FillProfileData`·`LoadCustomResponses`와 같은 순서):

1. **채널 이름과 종류.** 0–7은 2.1절 이름·종류다. 8–63의 기본 이름은 enum 이름이고 종류는 "미사용"이다. `channels`는 `channel` 순으로 정렬해 처리한다(같은 channel은 설정 순서). 범위(8–63) 밖 채널은 진단 후 무시한다. 이미 이름이 주어진 채널에 또 이름을 주면 그 항목 전체(기본 응답 포함)를 진단 후 무시한다(UE `CollisionProfile.cpp` "Cannot map multiple responses to the same collision channel" — 같은 동작). `name`이 비어 있으면 진단(`UnnamedChannel` — UE도 "Name can't be empty" 경고, `:476`)을 남기고 이름·종류는 바꾸지 않은 채 기본 응답만 정한다. 이름이 다른 채널의 표시 이름이나 **enum 이름**(이름 없는 사용자 채널의 조회 이름 포함)과 같으면(대소문자 무시) 진단 후 이름만 무시하고 기본 응답은 적용한다(Rocks 추가 검사 — UE는 검사하지 않고 이름까지 받아 조회가 모호해진다. enum 이름은 사용자 채널 이름으로 쓸 수 없다고 정한다).
2. **기본 응답 묶음.** 전부 `Block`, `GizmoCollisionChannel`만 `Ignore`이고, `channels`의 `defaultResponse`를 그 채널에 넣는다(UE `CollisionProfile.cpp` — `DefaultResponseContainer.SetAllChannels(ECR_Block)`·`SetResponse(COLLISION_GIZMO, ECR_Ignore)`).
3. **채널 redirect.** `channelRedirects` 중 두 이름이 비어 있지 않고 새 이름의 채널이 있는 것만 받고, 나머지는 진단 후 무시한다(UE는 대상 채널을 확인하지 않는다 — Rocks 추가 검사). 채널 이름 조회는 표시 이름을 먼저, 없으면 redirect를 본다.
4. **profile.**
   - 이름이 `Custom`(예약 — `UCollisionProfile::CustomCollisionProfileName`, `CollisionProfile.cpp:17`, UE도 지운다 `:281-289`)이거나 빈 profile(Rocks 추가 — UE는 빈 이름을 지우지 않는다)은 진단 후 뺀다.
   - 같은 이름이 여럿이면 **마지막 것만 남긴다**(UE `FillProfileData` — 앞 것을 지운다). 진단은 남기지 않는다(층 병합의 정상 결과일 수 있다). 표의 profile 순서는 남은 것의 원래 순서다.
   - `objectTypeName`을 채널로 바꾼다. 빈 값이면 `WorldStatic`(UE 템플릿 기본값). 없는 이름이거나 trace 채널이거나 미사용 채널이면 진단 후 그 profile을 뺀다(UE와 같다).
   - 응답 = 기본 응답 묶음 → `customResponses` 차례로 적용 → 같은 이름의 `profileEdits` 첫 항목의 `customResponses` 적용. 없는 채널 이름은 진단 후 그 칸만 건너뛴다(UE `LoadCustomResponses`와 같다). 표에 없는 profile을 가리키는 edit는 조용히 무시한다(UE와 같다).
5. **profile redirect.** `profileRedirects` 중 두 이름이 비어 있지 않고 새 이름의 profile이 표에 있는 것만 받는다(UE와 같다). 나머지는 진단 후 무시한다.

조회(모두 ASCII 대소문자 무시):

| 함수 | 반환 |
|---|---|
| `FindProfile(StringView name)` | `const CollisionProfile*`. redirect를 **먼저** 보고(UE `ReadConfig` — `CollisionProfile.cpp:190-191`: `CheckRedirect` 다음 `GetProfileTemplate`), 없으면 이름 그대로 찾는다. 같은 옛 이름의 redirect가 여럿이면 마지막 것(UE `ProfileRedirectsMap`은 TMap이라 뒤 항목이 덮어쓴다 — `:576`). 없으면 null. `CollisionProfile` = 실효 이름, `collisionEnabled`, `objectType`, `responses`(64칸), `helpMessage`, `canModify` |
| `Profiles()` | 표 순서의 profile 목록 |
| `FindChannel(StringView name)` | `Optional<CollisionChannel>`(표시 이름 → redirect 순, UE `ReturnContainerIndexFromChannelName`과 같다. 같은 옛 이름은 마지막 redirect — `:546`) |
| `ChannelName(channel)` | 표시 이름(미사용 사용자 채널은 enum 이름) |
| `ChannelKind(channel)`·`IsStaticObjectChannel(channel)` | 1단계 종류(`Unused`·`Object`·`Trace`)와 정적 object 묶음. object 질의의 정적/동적 묶음은 UE `FCollisionQueryFlag`와 같다 |
| `DefaultResponses()` | 2단계 묶음 |
| `Diagnostics()` | 만들 때 남긴 진단 |

### 4.1 UE와 다른 점

| 항목 | UE | Rocks | 이유 |
|---|---|---|---|
| 빈 profile 이름 | 지우지 않는다 | 진단 후 뺀다 | 이름 없는 profile은 찾을 수 없다 |
| 사용자 채널 이름이 다른 채널의 표시·enum 이름과 겹침 | 검사하지 않는다(이름·기본 응답 둘 다 받는다) | 이름만 무시, 기본 응답은 받는다 | 이름 조회가 모호해진다 |
| 채널 redirect 대상이 없음 | 받는다(조회가 실패할 뿐) | 진단 후 무시 | 설정 실수를 알린다 |
| body 사용자 지정 응답의 없는 채널 이름 | 목록에서 지운다 | 해석에서만 건너뛴다(목록 보존) | 해석이 데이터를 고치지 않는다 |
| 없는 profile 이름 | 이름을 `Custom`으로 바꾼다 | 이름은 그대로, `profileMissing` | 데이터를 고치지 않는다(5절 — 값은 CD2가 UE와 같게 맞춘다) |
| profile 값 저장 | body에 복사해 저장 | 이름만 영속, 해석 함수로 계산(CD2가 setter에서 사용자 지정 칸에 마지막 해석 값을 둔다) | CDD7 |

## 5. 실효 충돌 값 해석

`ResolveCollision(const CollisionProfileTable&, StringView profileName, const BodySettings&) -> ResolvedCollision`은 순수 함수다. `ResolvedCollision` = 실효 profile 이름, `collisionEnabled`, `objectType`, `responses`, `fromProfile`(표의 profile에서 왔는지), `profileMissing`(이름이 있는데 표에 없었는지).

- `profileName`이 `Custom`이면 `BodySettings`의 사용자 지정 값(6.1절)을 쓴다: `collisionEnabled`, `objectType`, 응답 = 기본 묶음 + `customResponses`(채널 이름은 redirect를 따른다). 없는 채널 이름은 해석에서 건너뛰되 **목록에서 지우지 않는다**. UE `UpdateResponseContainerFromArray`(`E/Private/PhysicsEngine/BodyInstance.cpp:298-318`)는 그 항목을 지운다 — Rocks는 해석이 데이터를 고치지 않으므로, 나중에 그 채널 이름이 생기면 값이 되살아난다.
- 표에 있으면 profile 값을 쓴다(UE `ReadConfig` — `CollisionProfile.cpp:184-205`).
- 표에 없으면 `Custom`처럼 `BodySettings` 값을 쓰고 `profileMissing`을 참으로 둔다. UE는 일반 경로(`BodyInstance.cpp:4507-4512`)에서 경고 없이 이름을 `Custom`으로 바꾸고(경고는 검증 경로 `:4497-4501`만), body에 남아 있는 값을 쓴다. UE body에는 **마지막으로 해석한 profile 값**이 복사되어 저장돼 있다(`ReadConfig`가 `CollisionEnabled`·`ObjectType`·응답을 body에 넣고 응답 차이 목록도 갱신 — `CollisionProfile.cpp:197-200`, `BodyInstance.cpp:196-205`). 그래서 Rocks가 같은 결과를 내려면 CD2가 profile 이름을 정하는 **모든** 경로(이름 setter 포함 — UE `SetCollisionProfileName`, `BodyInstance.cpp:698-717`도 `Custom`으로 바꿀 때 직전 값을 유지한다)에서 해석 결과를 `MakeCustomCollision`으로 `BodySettings`에 옮겨 둔다(9절 CD2). 그러면 profile이 사라져도 마지막 해석 값으로 돌아간다(UE와 같다). Rocks는 영속 이름을 바꾸지 않고(데이터를 고치지 않는다 — 표에 profile이 다시 생기면 되살아난다) 경고는 호출자(CD2 — 컴포넌트 등록 때 한 번)가 남긴다.
- `profileName`이 비어 있으면 호출자가 클래스 기본 이름을 먼저 넣는다(D2 `CollisionProfileName()` — 이 함수는 빈 이름을 `Custom`으로 다룬다).
- 사용자 지정으로 바꾸기: `MakeCustomCollision(const CollisionProfileTable&, const ResolvedCollision&, BodySettings&)`는 실효 값을 `BodySettings`의 사용자 지정 필드에 옮긴다(응답은 기본 묶음과 다른 칸만, 채널 번호 순으로 그 채널의 표시 이름을 적는다 — UE `UpdateArrayFromResponseContainer`, `BodyInstance.cpp:269-290`). D2의 `SetCollisionEnabled`·`SetCollisionObjectType`·`SetCollisionResponseToChannel`(UE처럼 profile 이름을 `Custom`으로 바꾸는 setter)이 이것을 쓴다(CD2).

## 6. body 값 `BodySettings`

`BodySettings`(리플렉션 struct)는 UE `FBodyInstance`·`FBodyInstanceCore`의 영속 필드 중 Jolt에서 의미가 있는 것이다(`E/Classes/PhysicsEngine/BodyInstance.h`, `PC/Public/BodyInstanceCore.h`). 이름은 Rocks 이름이다(UE 대응은 이 표에만 — Rocks 고유 이름 원칙). 실행 중 body handle은 물리 모듈의 `PhysicsBodyHandle`이고(전체 구현 계획 16.1절) 이 struct에 없다. `PrimitiveComponent`의 영속 필드 `bodySettings`로 둔다(CD2). 단위는 Rocks 공개 물리 API 규칙(cm·kg·도 — 어댑터 경계 7절)이다.

### 6.1 사용자 지정 충돌(`Custom`일 때만 쓰인다)

| 필드 | 기본값 | UE |
|---|---|---|
| `collisionEnabled` | `QueryAndPhysics` | `CollisionEnabled`(`BodyInstance.cpp:365`) |
| `objectType` | `WorldStatic` | `ObjectType`(`:363`) |
| `customResponses` | 빈 배열(기본 묶음과 다른 칸만) | `CollisionResponses.ResponseArray` |

### 6.2 시뮬레이션·질량

| 필드 | 기본값 | 단위 | UE(기본값 위치) | Jolt 대응(CR16) |
|---|---|---|---|---|
| `simulatePhysics` | 거짓 | — | `bSimulatePhysics`(`PC/Private/BodyInstanceCore.cpp:14` — 이 표에서 파일명 없는 `BodyInstanceCore.cpp:N`은 이 파일, `BodyInstance.cpp:N`은 `E/Private/PhysicsEngine/BodyInstance.cpp`) | `EMotionType::Dynamic` |
| `startAwake` | 참 | — | `bStartAwake`(`:20`) | `EActivation` |
| `enableGravity` | 참 | — | `bEnableGravity`(`:16`) | `mGravityFactor` 1 / 0 |
| `overrideMass` | 거짓 | — | `bOverrideMass`(`:15`) | `EOverrideMassProperties` |
| `massInKgOverride` | 100 | kg | `MassInKgOverride`(`BodyInstance.cpp:399`) | `MassProperties::ScaleToMass` |
| `massScale` | 1 | 배 | `MassScale`(`:405`) | 질량 계산 뒤 배율 |
| `centerOfMassOffset` | (0, 0, 0) | cm(body 공간) | `COMNudge`(`:404`) | `OffsetCenterOfMassShape` |
| `inertiaTensorScale` | (1, 1, 1) | 배(축별) | `InertiaTensorScale`(`:407`) | 계산 관성 주축값 배율(축은 어댑터 A로) |
| `updateMassWhenScaleChanges` | 참 | — | `bUpdateMassWhenScaleChanges`(`BodyInstanceCore.cpp:22`, cvar 기본 0 → 참) | scale 바뀔 때 질량 재계산 |
| `linearDamping` | 0.01 | 1/s | `LinearDamping`(`BodyInstance.cpp:401`) | `mLinearDamping`(Jolt 기본 0.05에 맡기지 않는다) |
| `angularDamping` | 0 | 1/s | `AngularDamping`(`:402`) | `mAngularDamping` |
| `gyroscopicTorque` | 거짓 | — | `bGyroscopicTorqueEnabled`(`BodyInstanceCore.cpp:18`) | `mApplyGyroscopicForce` |
| `overrideMaxAngularVelocity` | 거짓 | — | `bOverrideMaxAngularVelocity`(`BodyInstance.cpp:386`) | — |
| `maxAngularVelocity` | 3600 | 도/s | `MaxAngularVelocity`(생성자에서 `UPhysicsSettings` 값 — `PC/Private/PhysicsSettingsCore.cpp:43`) | `mMaxAngularVelocity`(rad/s, 도 → rad). Jolt 기본 0.25·π·60 rad/s(2700 도/s)에 맡기지 않는다 |
| `continuousCollision` | 거짓 | — | `bUseCCD`(`BodyInstance.cpp:369`) | `EMotionQuality::LinearCast` |
| `smoothEdgeCollisions` | 거짓 | — | `bSmoothEdgeCollisions`(`BodyInstance.cpp:377`) | `mEnhancedInternalEdgeRemoval` |
| `overrideSolverIterations` | 거짓 | — | `bOverrideIterationCounts`(`BodyInstance.cpp:389`) | — |
| `positionIterations`·`velocityIterations` | 8·2 | 회 | `PositionSolverIterationCount`·`VelocitySolverIterationCount`(`BodyInstance.cpp:360-361`) | `mNumPositionStepsOverride`·`mNumVelocityStepsOverride`. Chaos와 Jolt의 반복 의미가 달라 값은 그대로 옮기되 같은 결과를 약속하지 않는다 |
| `autoWeld` | 거짓 | — | `bAutoWeld`(`BodyInstanceCore.cpp:19`) | 부모 body compound에 합침 |

### 6.3 이벤트·자유도·보행

| 필드 | 기본값 | UE | Jolt 대응(CR16) |
|---|---|---|---|
| `notifyRigidBodyCollision` | 거짓 | `bNotifyRigidBodyCollision`(`BodyInstance.cpp:375`, hit 이벤트) | `ContactListener` 값 복사(16.2절) |
| `generateWakeEvents` | 거짓 | `bGenerateWakeEvents`(`BodyInstanceCore.cpp:21`) | `BodyActivationListener` |
| `dofMode` | `Default` | `DOFMode`(`EDOFMode` — `BodyInstance.h:39-57`: Default·SixDOF·YZPlane·XZPlane·XYPlane·CustomPlane·None) | `mAllowedDOFs`(월드 축 bit — 어댑터 A로 재배치) |
| `lockTranslation`·`lockRotation` | 참·참 | `bLockTranslation`·`bLockRotation`(`:378-379`) | 평면 모드의 이동·회전 잠금 |
| `lockXTranslation`…`lockZRotation`(6개) | 모두 거짓 | `bLockXTranslation`…(`:380-385`) | SixDOF 모드의 축 잠금 |
| `customDofPlaneNormal` | (0, 0, 0) | `CustomDOFPlaneNormal`(`:403`) | CustomPlane 모드 |
| `walkableSlope` | {`Default`, 0도} | `WalkableSlopeOverride`(`FWalkableSlopeOverride` — `EngineTypes.h:3694-3757`, struct `:3724`: Default·Increase·Decrease·Unwalkable, 각도 도) | 물리가 아니라 캐릭터 이동(CM13)이 읽는다 |
| `overrideWalkableSlope` | 거짓 | `bOverrideWalkableSlopeOnInstance`(`:388`) | 위와 같다 |

- `Default` DOF 모드는 프로젝트 물리 설정의 기본 모드(UE `UPhysicsSettings::DefaultDegreesOfFreedom`)를 따른다. 그 설정은 물리 설정 struct(CR16)에 둔다.
- UE DOF 잠금 축이 월드 축인지 body 초기 방향 축인지는 물리 단계에서 확인한다(미확인 — 8절).

### 6.4 들이지 않는 UE 필드(사유)

| UE 필드 | 사유 |
|---|---|
| `bUseMACD`, `bAllowPartialIslandSleep`, `bIgnoreAnalyticCollisions`, `bInertiaConditioning`, `bOneWayInteraction`, `bOverrideSolverAsyncDeltaTime`·`SolverAsyncDeltaTime`, `ProjectionSolverIterationCount`, `StabilizationThresholdMultiplier`, `GravityGroupIndex`, `bUpdateKinematicFromSimulation`, `bInterpolateWhenSubStepping` | Chaos solver 고유 기능이다. Jolt에 대응이 없다(Chaos 고유 기능 미재현 — 전체 구현 계획 16.1절) |
| `bOverrideMaxDepenetrationVelocity`·`MaxDepenetrationVelocity` | Jolt에 body별 침투 해소 속도 상한이 없다(전역 `mMaxPenetrationDistance`·Baumgarte뿐 — 어댑터 경계 J12) |
| `SleepFamily`·`CustomSleepThresholdMultiplier` | Jolt sleep 임계는 전역(`mPointVelocitySleepThreshold`)이고 body별로는 `mAllowSleeping`뿐이다. body별 sleep 금지가 필요해지면 그때 `allowSleeping`을 더한다 |
| `PhysMaterialOverride` | 물리 재질 에셋이 아직 없다. 에셋 참조 필드는 그 에셋 단계에서 더한다(엔진 런타임 24.2.8절과 같은 방식) |
| `CollisionProfileName` | D2가 `PrimitiveComponent::collisionProfileName`으로 이미 둔다(ERD40). UE처럼 body 안으로 옮기지 않는다 |
| `ResponseToChannels`(transient)·`InstanceBodyIndex`·`Scale3D`·`bPendingCollisionProfileSetup`·`bShapeCollision*IsSet`·`bDirtyMassProps`·`ExternalCollisionProfileBodySetup`·`WeldParent`·`PhysicsBlendWeight`·`InstanceBlendWeight` | 실행 중 상태·편집기 상태·skeletal 전용 값이다. 실효 응답은 5절이 계산한다 |
| `bContactModification` | UPROPERTY가 아니다(실행 중 설정) |

## 7. 시험(CD1)

1. 채널·응답·충돌 켬 enum 값과 이름이 UE와 같다(64채널 번호, `GizmoCollisionChannel` = 8). `CollisionEnabledIntersection` 36쌍과 Has* 판정 18개가 UE 함수 결과표와 같다.
2. `CollisionResponses` 연산: 생성·Set의 변경 여부·SetAll·Replace·Min(응답 3 × 3 = 9쌍)·비교.
3. 엔진 기본 데이터: `Engine/Config/Engine.json`을 설정 import로 읽어 표를 만들면 진단 0이고, profile 19개의 이름·순서·충돌 켬·object type·`canModify`가 UE `BaseEngine.ini`와 같으며, profile마다 64칸 응답이 UE 규칙으로 계산한 기대값(시험에 UE ini 줄을 옮긴 고정 기대표)과 같다.
4. 표 생성 규칙: 사용자 채널 이름·종류·기본 응답, 범위 밖·중복 채널 진단, 같은 이름 채널 진단, 예약 이름 `Custom`·빈 이름 제거, 같은 이름 profile은 마지막만, 없는·trace·미사용 object type 제거, 없는 채널 응답 칸만 건너뜀, `profileEdits` 적용, 두 redirect(받음·대상 없음 진단), 대소문자 무시 조회.
5. 해석: profile 이름 → profile 값, `Custom` → `BodySettings` 값(+ 없는 채널 건너뜀), 없는 이름 → `Custom` 값 + `profileMissing`, redirect 이름, `MakeCustomCollision` 왕복(해석 → 사용자 지정으로 옮김 → `Custom`으로 해석 = 같은 값, 차이 칸만 기록).
6. `BodySettings` 기본값이 6절 표와 같다. 영속 왕복은 컴포넌트 저장 경로로 CD2에서 확인한다(`PrimitiveComponent::bodySettings` — 실제로 저장되는 자리).
7. 사용자 채널을 정의한 프로젝트 층(배열 `$add`)을 엔진 층 위에 합친 설정으로 표를 만들면 채널 이름·기본 응답·profile 응답이 기대대로다(설정 체계 배열 연산과의 연결).
8. redirect·edit·층 규칙(리뷰 반영): 같은 옛 이름 redirect는 마지막 것, profile 이름과 겹치는 redirect가 먼저, 같은 이름 edit는 첫 것만·customResponses를 덮음, 없는 profile의 edit 무시, 프로젝트 층 `$add`의 같은 이름 profile이 엔진 것을 대신(끝 자리), body 응답 채널 이름의 redirect, `MakeCustomCollision`이 사용자 이름·enum 이름으로 적음. 엔진 기본 데이터는 설명 원문도 대조한다. 할당 실패 때 body가 그대로인지는 실패 주입 할당기가 생기면 더한다(미시험).

## 8. 물리 단계로 넘기는 것(CR16)

- **Jolt 필터 사상.** object type(64채널 중 object 채널)과 응답 묶음을 Jolt `ObjectLayer`·`ObjectLayerPairFilter`·`BroadPhaseLayer`로 옮기는 방식. 후보: body마다 object type을 ObjectLayer 상위 bit, 응답 묶음 색인(같은 묶음은 표로 공유)을 하위 bit에 두고 쌍 필터가 두 body의 응답 중 작은 것(UE 규칙 — 두 쪽 응답의 최소)으로 판정. `JPH_OBJECT_LAYER_BITS` 16으로 모자라면 32로 넓힌다(어댑터 경계 10.2절 — 반입 정의 변경). 질의 필터는 질의 채널의 응답(trace 채널)과 object type 질의로 나눈다.
- **Overlap.** Jolt sensor와 접촉 콜백으로 overlap을 만드는 방식(UE overlap은 query 결과이고 Jolt sensor는 시뮬레이션 쪽이다 — 같은 의미인지 확인).
- **Probe.** UE probe(접촉 데이터만, 반작용 없음)의 Jolt 대응(`ContactListener::OnContactValidate`·`ContactSettings::mIsSensor` 후보).
- **DOF 축.** UE 잠금 축의 기준(월드·body)과 Jolt `mAllowedDOFs`(월드 축)의 대응.
- **기본 응답 계산 결과의 정적 공유.** 같은 응답 묶음을 쓰는 body가 많으므로 해석 결과를 표 색인으로 공유하는 방식.
- **Jolt 기본값에 맡기지 않을 body 값**(리뷰 반영): `mMaxLinearVelocity`(Jolt 기본 500 m/s = 50,000 cm/s 상한 — UE body에는 이 상한이 없다. 물리 설정의 값으로 명시), `mAngularDamping`(Jolt 기본 0.05 — UE 기본 0을 넣는다), `mLinearDamping`(Jolt 0.05 — UE 0.01).
- **Static·Kinematic 구분.** `simulatePhysics`가 거짓인 body가 Jolt `Static`인지 `Kinematic`인지(컴포넌트 Mobility 기준), 실행 중 전환에 필요한 `mAllowDynamicOrKinematic`.
- **축별 관성 배율.** `inertiaTensorScale`은 축별인데 Jolt에는 스칼라 `mInertiaMultiplier`뿐이라 `EOverrideMassProperties::MassAndInertiaProvided`로 직접 계산해 넣는다(축은 어댑터 A로).
- **AllObjects 질의.** UE `AllObjects` 질의 bit에는 이름 없는 채널도 든다(`E/Classes/Engine/CollisionQueryParams.h:361`). 질의 필터 사상에서 같은 의미를 정한다.

## 9. 단계

| 단계 | 내용 | 맡는 곳 | 완료 기준 |
|---|---|---|---|
| CD0 | 이 문서 | Jolt 담당 세션 | 리뷰어 1명 반영 |
| CD1 | 2–6절 타입·설정 struct·`Engine.json` 기본 데이터·표 생성·해석·`BodySettings`·7절 시험 | Jolt 담당 세션 | 7절 시험, format·policy |
| CD2 | `PrimitiveComponent::bodySettings` 필드, 충돌 setter(UE처럼 `Custom`으로 바꾸는 것 포함. profile 이름을 정하는 모든 경로 — 이름 setter 포함 — 에서 해석 결과를 `MakeCustomCollision`으로 body에 옮겨 둔다: 5절, 없는 profile이 UE처럼 마지막 해석 값으로 돌아가게), World가 초기화 때 설정 문맥에서 표를 만들어 갖기(설정 문맥이 없으면 빈 표 — 모든 이름이 `profileMissing`), 컴포넌트 등록 때 `profileMissing` 경고 한 번 | D2 세션(엔진 런타임 24.3절) | D2 시험에 해석 경로 추가 |
| CD3 | Jolt 필터·overlap·probe·DOF 사상(8절) | 물리 모듈 단계(CR16, M4) | 어댑터 시험 |

## 10. 결정

| ID | 결정 | 근거·버린 대안 |
|---|---|---|
| CDD1 | 새 모듈 `Collision`(World·Mesh 아래) | 구현 중 `Mesh` 모듈(`Mesh/SimpleCollision.h`, MA3a)에 같은 `rocks::CollisionEnabled`가 이미 있음을 찾았다(리플렉션 ID 중복 RMC2101). 같은 개념을 두 번 정의하지 않고, World는 에셋 모듈에 의존하지 않으므로(엔진 런타임 3절·24.3.1절) 둘 아래 모듈을 만들어 옮겼다. Mesh의 enum은 같은 타입 ID로 옮겼다(`ids move`). 버린 대안: World에 두고 World→Mesh 의존(에셋 의존 금지 위반), Mesh enum을 World가 쓰기(같음), Core에 두기(Core는 가볍게 — 엔진 기능 데이터) |
| CDD2 | 채널 64개(UE 5.8) | UE 5.8이 GameTraceChannel을 50까지 늘렸다. 32개로 줄이면 UE 프로젝트 설정을 옮길 수 없다 |
| CDD3 | 이름은 `String`, ASCII 대소문자 무시 | D2와 같은 규칙. `Name`은 pool 수명에 묶인다 |
| CDD4 | profile 데이터는 설정(`Engine.json` "Collision"), 코드 기본값은 빈 배열 | UE와 같은 자리(엔진 설정 파일). 코드에 표를 굳히면 프로젝트가 배열 연산으로 바꾸는 흐름이 둘로 갈린다 |
| CDD5 | 옛 이름 redirect 기본 데이터는 들이지 않고 기능만 둔다 | 하위호환 없음 원칙 |
| CDD6 | 의미 오류는 항목을 빼고 진단(실패 아님) | UE와 같다. 프로젝트 설정 하나의 실수로 엔진 시작이 막히지 않게 한다. 모양 오류는 설정 import가 거부한다 |
| CDD7 | 실효 값은 저장하지 않고 해석 함수로 계산 | UE는 profile 값을 body에 복사해 저장하지만, Rocks는 이름(+`Custom` 값)만 영속이라 표가 바뀌면 다시 계산된다(D2 ERD40과 같은 방향) |
| CDD8 | 없는 profile 이름 → `Custom` 값으로 해석, 영속 이름은 그대로 | UE는 이름을 `Custom`으로 바꾼다. Rocks는 데이터를 고치지 않아 표에 profile이 다시 생기면 되살아난다 |
| CDD9 | body 값 이름 `BodySettings` | Rocks 고유 이름(UE `FBodyInstance`는 실행 상태까지 담는 이름). Jolt `BodyCreationSettings`와 겹치지 않는다(Jolt 이름은 adapter 밖으로 나가지 않는다) |
| CDD10 | Jolt에 대응 없는 Chaos 필드는 들이지 않는다(6.4절) | 동작하지 않는 데이터를 두지 않는다(컴포넌트 도입 5절 원칙) |

## 11. 개정 이력

- 개정 1(2026-11-25): 초안.
