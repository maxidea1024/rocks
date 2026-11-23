# 엔진 런타임 — 6. Engine

[목차](README.ko.md)

## 6. Engine

```text
Uninitialized → Initializing → Running → Stopping → Stopped
                     └──(실패)──→ Failed(이미 초기화한 단계를 역순 정리)
```

- 모듈 로더([모듈 로더](../ModuleLoader.ko.md) 3.11절, ML1): `EngineConfig::modules`(프로세스 로더, 필수)에 엔진 `TypeRegistry`의 타입 등록기를 붙이고(이미 시작한 모듈의 타입을 몰아서 등록) 엔진 묶음(`PreDefault`–`PostDefault`, 동시에 여러 Engine이면 참조 수)을 시작한다. 엔진 단계 모듈(World·Engine 등)의 타입은 그 모듈 `Startup` 직전에 등록된다. 클라이언트 시작 뒤 `PostEngineInit` 단계를 돈다. 종료는 ObjectRegistry 정리·최종 GC 뒤 엔진 묶음 종료 → 등록기 떼기 → `TypeRegistry::Shutdown`이다.
- `Initialize(EngineConfig)`: TypeRegistry·ModuleRegistration → 타입 등록(모듈 로더 등록기와 엔진 묶음 — World 리플렉션 타입 등 → `EngineConfig.registerModuleTypes(ModuleRegistration&)` — 게임 리플렉션 모듈, 같은 registration → `EngineConfig.registerTypes(TypeRegistry&)` — native 타입, 리플렉션 타입을 base로 둘 수 있으므로 마지막) → 엔진 Registry → EngineSubsystem(의존 순서) → 클라이언트(`EngineClient`, 예: GameInstance) 순. 어느 단계든 실패하면 앞 단계를 역순 정리하고 `Failed`.
- 종료(`Shutdown`과 초기화 실패 정리 모두): World 파괴 → Subsystem 역순 정리 → 엔진 Registry 정리 → ModuleRegistration 파괴(붙잡은 lease 해제) → `TypeRegistry::Shutdown`. Engine은 타입을 Unregister하지 않는다 — 게임이 토큰을 버린 native 파생이 리플렉션 base의 dependent로 남아 Unregister가 Busy로 멈추기 때문이다. `TypeRegistry::Shutdown`은 남은 lease만 검사하고 레코드를 dependent 순서로 반납한다. ModuleRegistration 멤버는 TypeRegistry 멤버 뒤에 선언해 먼저 소멸한다.
- 종료 게이트(2026-11-22 설계, [오디오 시스템](../AudioSystem.ko.md) AD23): 외부 실행 문맥(장치 콜백·전용 스레드)이 Subsystem 소유 메모리를 쥐고 있을 수 있는 Subsystem은 `CompleteInFlightWork`에서 그 문맥이 멈췄음을 확인하지 못하면 `Busy`를 돌려준다. Engine은 `Busy`를 다른 실패처럼 누적하지 않고 그 자리에서 `Shutdown`을 멈추고 `Busy`를 돌려준다(Subsystem 해제·Registry 정리로 넘어가지 않는다 — 해제 뒤 접근 방지). `Shutdown`을 다시 부르면 아직 완료하지 않은 Subsystem부터 `CompleteInFlightWork`를 이어 부른다(끝낸 것은 다시 부르지 않는다). 엔진 호스트는 `MaxShutdownGateRetries`(= 3)번까지 다시 부르고, 그래도 `Busy`면 `platform::Fatal`로 끝낸다(소유자를 풀지 않고 끝내는 것이 해제 뒤 접근보다 안전하다 — [크래시 처리·실행](../CrashHandlingAndLaunch.ko.md) 20절). 대기 시간 상한은 Subsystem 안에 있다(오디오 `ShutdownDrainTimeoutMs`). 지금 코드는 모든 실패를 `KeepFirst`로 누적하고 해제로 진행한다(`Engine/Source/Runtime/Engine/Private/Engine.cpp:767–794`) — 오디오 AU3에서 바꾼다.
- `RunFrame()`: ① hook `PumpEvents` ② 시간 갱신 ③ hook `FrameBegin` ④ WorldContext마다 `World::Tick` ⑤ 엔진 지연 작업 ⑥ hook `FrameEnd`(렌더 추출·제출).
- `Run()`: `RequestExit()`까지 `RunFrame` 반복. `Shutdown()`은 상태와 무관하게 여러 번 호출해도 안전하다.
- `EngineClient`는 Engine이 Gameplay를 모르게 하는 경계다. GameInstance가 이를 구현한다.
