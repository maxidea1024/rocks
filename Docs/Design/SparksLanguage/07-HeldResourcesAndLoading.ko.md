## 7. 유지 자원과 비동기 로딩

### 7.1 유지 자원

멈출 때까지 계속되는 것(반복 사운드·지속 VFX·입력 context·로딩 요청·표시한 widget·카메라 점유)은 **유지 자원**이다. 사용자가 알아야 할 규칙은 3개다. 로딩 요청 handle(`LoadRequest<T>`)도 유지 자원이며 규칙 2·3을 따르지만, 생성이 실패하지 않으므로 `T?`가 아니고 `Start*` 이름 규칙에서 제외한다(§7.3).

1. **담아야 만들 수 있다.** 유지 자원은 지역 변수(`var`·`let`)의 선언 초기값(`?? return` 포함) 또는 필드·`var` 지역 변수 대입의 우변에서만 생성할 수 있다. 필드 초기값에서는 만들 수 없으므로 유지 자원 필드는 `var` `T?`뿐이다. 로딩 요청은 지역 변수에만 담는다(§7.3). `await`의 피연산자나 `All`·`Race`의 인자로 바로 쓰면 대기가 끝날 때까지 그 실행이 보관한다. 결과를 버리면(`_ =` 포함) "멈출 방법이 없다"는 오류다. 일회성 효과(`World.Audio.PlayAt`)는 handle 없이 호출한다.
2. **변수가 끝나면 멈춘다.** 필드나 `var` 지역 변수에 새 값·`null`을 넣을 때, 지역 변수의 블록을 떠날 때(`return`·`break`·`?? Exit`·fault·취소 포함, 반복문은 회차마다), 객체가 비활성화·종료될 때 멈춘다. 같은 블록의 지역 자원은 선언 역순으로 멈춘다.
3. **옮길 수 없다.** 복사, 다른 이름으로 옮기기(`if (let)`, 담긴 변수를 좌변으로 한 `??`, 대입), 배열·Map·struct·이벤트 인자·함수 인자(native 포함, `All`·`Race`의 인자로 기다리는 경우 제외)·반환값·다른 객체·`@save`/`@edit`/`@net` 필드 보관은 오류다. 쓸 때는 담긴 변수의 멤버를 호출한다(`Hum?.Pause()`).

유지 자원(클래스 본문 일부):

~~~text
var Hum: Sound?;
on Enable() { Hum = World.Audio.StartLoop(EngineCue, Owner); }   // 새 자원을 얻은 뒤 이전 값을 해제
@skipWhileBusy on Owner.Alarm() {
    let siren = World.Audio.StartLoop(SirenCue, Owner);           // 이 블록이 끝나면 멈춤
    await Delay(5.0, Clock.Game);
}
fn Quiet() { Hum?.Pause(); }                                      // 담긴 자리에서 호출
~~~

- 대입 순서는 새 자원 획득 → 이전 자원 해제 → 저장이다. 유지 자원 획득 API는 `T?`를 반환하고 유지 자원 필드는 `T?`로 선언한다. 획득이 할당량·예산·비활성 상태로 실패하면 결과는 `null`이며 이전 자원은 해제되고 디버거에 보고한다.
- 지역 유지 자원을 얻은 지점부터 블록 끝까지 어느 경로에도 `await`가 없으면 오류다. 그 자원은 같은 스크립트 단계 안에서 멈추기 때문이다. 수정 예는 계속 유지하려면 필드에 담기, 정해진 시간만 유지하려면 블록 안에서 `await Delay(...)`다.
- Disable·Detach가 해제한 필드는 `null`이 된다. 다시 획득하는 것은 사용자 코드이며 `on Enable`에서 한다.
- 필드 초기값에서 획득할 수 없고(상수식과 component 생성만 허용), 필드가 reload로 삭제되면 그 자원을 해제한다. 타이머 handle은 없다. 기다림은 `await Delay`, 반복은 handler 안의 반복문이다.
- binding schema가 자원 종류(유지·일회성)를 정하므로 사용자는 반환 타입으로 구분한다(§10.2). 유지 자원을 반환하는 API는 `Start*`, handle을 반환하지 않는 재생(일회성 효과와 `await`하는 재생)은 `Play*`로 이름을 짓고, 반복 설정된 에셋을 일회성 API에 넘기면 컴파일·쿡 오류다(§10.3).

### 7.2 에셋 참조

- **하드 참조** `@edit var OpenSound: SoundAsset = Asset("...")`: 이 클래스를 사용하는 prefab·셀의 의존성으로 함께 로드된다(스크립트 모듈의 의존성이 아니다). 엔진 표현은 `SoftObjectRef<T>` 필드와 instance가 보유하는 `AssetLease`이며, "하드"는 그 World 데이터를 담는 레벨·prefab 패키지의 Dependencies(Hard)로 나타낸다(AssetSystem 6.3절 — World 객체는 에셋을 `ObjectRef`로 가리킬 수 없다). 함수 본문의 하드 `Asset(...)` 리터럴도 같은 방식으로 클래스의 의존성이 된다. instance override가 값을 바꾸면 바뀐 에셋이 의존성이 된다. null이 아니다. 필드 초기값의 참조는 클래스 기본값 레코드에 들어가며, override하지 않은 값의 레코드 참조도 패키지 Dependencies에 넣고 로드 대조도 기준 적용 결과로 한다 — 이 의존 수집이 L2의 선행 조건이며 그 전에는 레코드의 참조 기본값(Soft 포함)이 null만 허용된다([RuntimeClassMetadata](../RuntimeClassMetadata.ko.md) '클래스 기본값 결정').
- **소프트 참조** `Soft<T>`: AssetId만 가진다. 빈 값이 없으므로 선택 값은 `Soft<T>?`로 쓴다(SD19). `T`가 필요한 자리에 쓰면 오류이며 빠른 수정이 `try await X.Load()`를 삽입한다.
- prefab·PrimaryAsset별 하드 의존 크기 예산을 두고 초과하면 쿡 오류다. 셀 경계를 넘는 하드 edge도 쿡이 진단한다(전체 계획 §10.2).
- 로드한 결과 `T`는 lease를 공유하는 복사 가능한 값이며, 보관하는 동안 상주를 유지한다. `@net`·RPC·`@save`에는 `T`를 둘 수 없고 `Soft<T>`를 쓴다(빠른 수정 제공).

### 7.3 로드

로드(클래스 멤버):

~~~text
@skipWhileBusy on Owner.Hit() {
    let effect = try await HitFx.Load();           // 기본 형태: 실패하면 이 실행을 끝내고 기록
    World.Effects.PlayAt(effect, Owner.Location);
}
@skipWhileBusy on Owner.Equipped() {
    Fx = try await HitFx.Load();                   // 필드에 보관: instance가 끝날 때까지 상주
}

async fn LoadPreviews() {                          // 진행률이 필요할 때(WidgetController)
    let req = Assets.LoadAll<Texture>(Previews);   // 지역 전용 요청 handle (유지 자원)
    while (!req.IsDone) {
        Widget.Progress.Value = req.Progress;
        await NextTick();
    }
    Loaded = try await req;                        // Array<Texture>, 이후 Loaded가 상주를 유지
}
~~~

- `X.Load()`와 `Assets.LoadAll<T>(list)`는 요청 handle `LoadRequest<T>`를 반환하며 `await`의 결과는 `Result<T>`다. 요청 생성은 실패하지 않으며 상한·예산을 넘으면 admission(새 요청의 수락)이 지연된다. 요청 handle은 지역 변수에만 둘 수 있고 한 번만 `await`한다. 상주를 유지하는 방법은 결과 값을 보관하는 것 하나다.
- `LoadAll`은 전부 성공하거나 하나의 오류를 반환하고 전체 진행률을 제공한다.
- 같은 package 요청은 엔진이 하나의 로드로 합친다. 한 실행의 취소는 다른 실행의 요청을 취소하지 않는다(전체 계획 §9.2).
- 결과는 현재 target이 요구하는 준비 mask(CPU 객체·render·physics 등)를 충족한 뒤 반환된다. 임시 fallback 표시 상태를 완료로 보지 않는다. 준비 mask 타입은 아직 코드에 없다(AssetManager 단계 — 연동 요구 SE-A1).
- 해제된 package는 상한이 있는 cache(전체 계획 §29.6 allowlist)에 잠시 남아 같은 에셋을 반복 로드할 때의 비용을 줄인다. 같은 호출 위치의 반복 로드는 ledger에 표시한다.
- 시간 제한은 `await Race(req, Delay(5.0, Clock.UI));` 다음 `req.IsDone`을 확인한다. `All`·`Race`의 인자로 쓰는 것은 요청 handle의 1회 `await`로 세지 않는다. `Race`에서 진 요청은 취소되지 않고 변수의 범위가 끝날 때까지 계속 진행한다(규칙 2). 그래서 "5초 뒤 안내를 띄우고 계속 기다리기"는 `Race` 다음 `try await req`로 쓴다. 결과를 꺼내는 `try await req`는 1회만 허용하고 두 번째는 `ContractViolation` fault다.

### 7.4 실패·취소와 전체 계획 §9.2 계약

- 결과는 `Result<T>`다: `NotFound`(삭제된 에셋, 예: 오래된 저장 데이터), `LoadFailed`(IO·검증·등록 실패). 등록 자체가 실패하면 이미 종료된 요청을 반환하고 `await`에서 `LoadFailed`를 받는다.
- 실행이 취소되면 요청 handle이 해제되어 이 실행의 관심만 반환한다. 결과를 받을 실행이 없는 완료는 binding의 고립 완료 경로가 lease를 반환한다(NL03 fixture).
- 한 LoadTicket에는 OnReady callback이 하나뿐이므로 같은 요청을 기다리는 여러 대기자는 binding이 분배한다.
- 쿡은 스크립트 상수·`@edit` 필드·DataTable 행에서 도달 가능한 모든 소프트 대상을 chunk에 배정하고, 배정되지 않으면 쿡 오류로 처리한다.
- instance별 진행 중 로드 수에 상한을 두어 취소된 로드가 누적되지 않게 한다.
- 로드한 값을 범위가 다른 곳(예: 셀과 함께 unload되는 Actor의 실행 → `WorldService` 필드)에 저장하면 엔진이 새 범위로 lease를 다시 발급한다. scope revoke 순서는 실행 취소 → 소유 범위의 정상 해제 → revoke이며, revoke 시점에 남은 항목은 ledger 실패다.
- 편집기에서 에셋이 다시 import되면 스크립트가 보유한 lease를 스크립트 단계 경계에서 새 revision으로 교체한다(편집기 전용).

### 7.5 서버와 준비 mask

- 준비 mask는 타입이 요구하는 mask와 target capability의 교집합이다. 전용 서버에서 외형 전용 타입(texture·사운드 등)의 로드는 즉시 효과 없는 stub으로 완료되고, 그것을 받는 API는 효과가 없다. 서버 쿡에서 제거된 하드 참조도 같은 stub이다. 따라서 같은 코드가 서버에서 실패해 보상 지급 등이 누락되는 일이 없다.
- Tryout의 서버 session은 서버 capability와 서버 쿡 filter를 적용해 이 경로를 출시 전에 시험한다.
- 복제 spawn된 소프트 prefab은 클라이언트가 fixup 전에 로드한다. `@net` `Soft<T>` 필드의 `Changed`에서는 명시적으로 로드한다.

### 7.6 생성·이동·travel·스트리밍

- **생성:** `World.Spawn(prefab, at)`은 상주한 prefab만 받으며 Prefab의 instancing 경로(ConstructionContext, 미공개 graph rollback)로 생성한다 — 엔진 `World::SpawnActor`에는 아직 prefab·transform 인자가 없으므로 Prefab PF 단계가 선행 조건이다. 한 실행 조각에서 1,024개, 한 스크립트 단계에서 8,192개를 넘으면 `BudgetExceeded`다(§8.4). 소프트 prefab은 `let p = try await DoorRef.Load(); let door = try World.Spawn(p, at);`로 쓴다. 생성된 Actor는 prefab 의존 묶음의 lease를 스스로 보유하므로 `p`가 사라져도 prefab이 해제되지 않는다.
- **이동:** `try await Owner.Teleport(position)`은 목적지 prefetch·commit gate를 내부에서 처리하고 관심을 streaming source로 이관한다(전체 계획 §10.2). 직접 prefetch(`World.Prefetch`)는 full이다.
- **travel:** `Session.Travel(map): Result<void>`은 서버·단독 실행 전용이며 기다리지 않고 요청만 접수한다. 클라이언트 호출은 `NotAuthority`, 중복 호출은 `AlreadyTraveling`이다. 이전 World에 속한 호출 실행은 World 종료 취소로 끝나며 fault로 보고하지 않는다. 로딩 화면은 엔진이 프로젝트 설정으로 관리하여 PrepareDestination에 표시하고 새 World의 초기 준비 mask가 충족된 뒤 숨긴다(전체 계획 §10.3). 직접 제어하는 로딩 화면·seamless travel은 full이다.
- **다른 셀의 Actor:** `@edit var Target: Door?`가 다른 셀의 Actor를 가리키면 SoftEntityId로 저장되고, 셀이 상주할 때 자동 해석되며 unload되면 null이다. `on Target.Opened()` 연결도 이를 따른다. 셀 unload는 그 셀 객체들의 정상 Disable·Destroy(§5.1 종료 경로 표)이며 scope revoke가 아니다.
- **data layer(full):** `World.SetDataLayer(layer, DataLayerState.Active)`는 서버 전용이며 복제·저장되는 World 상태다. 준비는 `await World.DataLayerReady(layer)`로 기다린다. late join 클라이언트는 현재 상태를 받는다.
- **컷씬 준비:** `await World.Sequencer.Prepare(seq)`는 shot 예측으로 texture mip·오디오 chunk를 미리 상주시킨다. 일반 로드의 준비 mask에는 이것이 포함되지 않는다(전체 계획 §9.5).
