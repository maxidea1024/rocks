# 엔진 런타임 — 2. 명칭

[목차](README.ko.md)

## 2. 명칭

- 게임 규칙 조율 객체는 **GameDirector**, 진행 상태는 **GameState**, 플레이어별 상태는 **PlayerState**, 제어 주체는 **Controller/PlayerController**, 제어 대상은 **Pawn**, 월드 배치 객체는 **Actor**다. 기존 계획 20.4·20.5절의 MatchRules·MatchState·PlayerAgent·ControlledEntity·Entity 명칭은 이 이름으로 대체한다. PlayerState는 계획 20.5절과 같은 이름이다(UE `APlayerState` 대응 — possession·재접속을 넘어 유지되는 플레이어 상태, 모든 참여자에게 복제).
- 그래픽 장치 추상화는 **Graphics**(`rocks::graphics`)다. 타입에 접두사를 붙이지 않는다(`graphics::Device`, `graphics::SwapChain`). 기존 계획의 "RHI" 표기는 Graphics로 바꾼다.
- 수명 hook은 계획 20.2절(2026-11-12 결정)을 따른다: `OnAwake`(Initialize), `OnEnable`, `OnStart`(BeginPlay), `OnTick`·`OnFixedTick`·`OnLateTick`, `OnDisable`, `OnDestroy`(EndPlay).
- Tick phase는 `PrePhysics`, `Physics`, `PostPhysics`, `LateUpdate`, `PreRender`(실행 순서)다. `Physics` phase만 고정 스텝으로 실행한다.
- 편집 World를 복제해 에디터 안에서 실행하는 기능은 **Tryout**이다(UE PIE 대응, 2026-11-19 결정). 뜻·표기 규칙·변형 이름(Tryout · Simulate, Tryout · Standalone)은 계획 22.6절을 따른다.
