# 입력 액션 계층 — 4. 평가

[목차](README.ko.md)

## 4. 평가

`PlayerActionInput`은 로컬 플레이어 하나의 컨텍스트 목록과 액션 상태다. UE `UEnhancedPlayerInput`과 입력 subsystem의 컨텍스트 목록(`IEnhancedInputSubsystemInterface`)을 합친 것이다. 용어는 다음과 같다.
- **chord 바인딩**: `ChordAction` 트리거를 가진 바인딩(또는 그 트리거를 가진 액션의 바인딩)이다.
- **chord 대상 액션**: 그 트리거가 가리키는 액션이다.

### 4.1 컨텍스트

API: `AddContext(context, priority, ContextOptions)` / `RemoveContext(context, options)` / `ClearContexts()` / `HasContext(context)`.

`ContextOptions`(UE `EnhancedInputSubsystemInterface.h:47-100`):

| 옵션 | 기본 | 뜻 |
|---|---|---|
| `ignorePressedKeysUntilRelease` | 참 | 다시 만들 때 눌려 있던 키를 놓을 때까지 무시한다(Boolean 액션만) |
| `forceImmediately` | 거짓 | 다음 평가를 기다리지 않고 바로 다시 만든다 |
| `notifyUserSettings` | 거짓 | 사용자 설정에 컨텍스트를 등록·해제한다(5.2절) |

동작:
- 추가·제거는 다시 만들기를 **예약**하고, 다음 평가 처음에 한 번 한다(UE 모듈 Tick — `EnhancedInputModule.cpp:298-328`).
- 예약된 요청들의 `ignorePressedKeysUntilRelease`는 AND로 합친다. 다시 만든 뒤 참으로 되돌린다(UE cpp:433·1201).
- 이미 있는 컨텍스트를 다시 추가할 때:
  - `Untracked`면 우선순위를 바꾸고 다시 만든다.
  - `CountRegistrations`면 횟수만 늘린다(우선순위 변경·다시 만들기 없음 — UE cpp:273-285). 제거는 횟수가 0이 될 때 빠진다.

### 4.2 다시 만들기(UE `EnhancedInputSubsystemInterface.cpp:886-1203`)

1. **정렬**: 우선순위 내림차순, 같은 우선순위는 추가한 순서다. UE는 불안정 정렬(`ValueSort`)이라 순서가 정해져 있지 않다. Rocks의 안정 순서는 UE와 다른 점이다(7.3절).
2. **바인딩 모음**: 컨텍스트마다 활성 프로필의 바인딩을 쓴다(`profileOverrides` → 기본). 리바인딩 가능한 바인딩은 키를 사용자 키로 바꾼다(5.2절).
   - 3단으로 다시 정렬한다: chord 대상 액션의 바인딩 → chord 바인딩 → 나머지(UE `ReorderMappings` :776-884).
   - Rocks는 원래 순서를 지키는 안정 분할이다. UE는 `RemoveAtSwap`이다.
3. **소비는 이 시점에 정한다.**
   - 앞(상위) 컨텍스트들이 이미 쓴 키의 바인딩은 넣지 않는다.
   - 액션이 `consumeInput`이고 chord 트리거가 없을 때만 키를 차단 집합에 넣는다.
   - 차단 집합은 컨텍스트 하나를 다 처리한 뒤 합친다. 그래서 같은 컨텍스트 안에서는 같은 키를 여러 바인딩이 쓸 수 있다(UE :966-967·1041·1052-1055·1091).
4. **chord 복제**: 액션 수준 `ChordAction` 트리거를 그 액션의 바인딩 수준에도 복제한다(UE :1076-1085).
5. **ChordBlocker**: 모든 컨텍스트를 합친 전체 바인딩 목록에서 처리한다(UE `InjectChordBlockers` :579-612).
   - chord 바인딩보다 뒤에 있고 같은 키를 쓰는 바인딩에 그 chord 바인딩의 액션을 대상으로 한 ChordBlocker를 넣는다. 그래서 뒤쪽 컨텍스트에도 들어간다.
   - 그 바인딩과 그 액션에 Explicit 트리거가 하나도 없으면, 임계 1e-8 Down도 함께 넣는다(UE :596).
6. **상태 보존**(UE :1113-1192):
   - 새 바인딩마다 이전 목록에서 액션·키·트리거 종류 열·모디파이어 종류 열이 같은 **첫** 바인딩을 찾아 1:1로 짝짓는다(쓴 것은 뺀다).
   - 짝이 있으면 트리거·모디파이어 실행 상태를 넘긴다. 매개변수는 새 값을 쓴다.
   - 액션 수준 상태(값·트리거 상태·시간)는 액션이 남아 있는 한 그대로다.
   - 새로 생긴 Boolean 바인딩의 키가 지금 눌려 있고 `ignorePressedKeysUntilRelease`면, 놓을 때까지 무시한다.
   - 다시 만든 뒤 모든 바인딩에 dt 0으로 모디파이어를 한 번 돌려 상태를 맞춘다(UE :1188-1192).
7. **사라진 액션**: 바인딩에서 사라진 액션은 다음 평가에서 값 0으로 한 번 평가해 Canceled·Completed를 낸 뒤 지운다(UE `EnhancedPlayerInput.cpp:1171-1200`).

### 4.3 평가 한 번(`Evaluate(const InputState&, const ActionEvalContext&)`)

UE `EnhancedPlayerInput.cpp:488-717` 순서와 같다.

1. 예약된 다시 만들기를 한다.
2. **바인딩 루프**: 바인딩마다 키를 읽어 키 이벤트를 정한다(UE :591-603).

   | 키 이벤트 | 조건 |
   |---|---|
   | `Actuated` | 이번 평가에 눌렸다(직전에 눌려 있지 않았다), 또는 이번 평가에 놓였다 |
   | `Held` | 눌린 채 이어진다. 아날로그가 계속 nonzero여도 Held다 |
   | `None` | 그 밖 |

   - 한 평가 안에 눌렀다 뗀 키는 눌림 값(1)으로 Actuated 한 번이다(UE :603-612·294-308).
   - 무시 중인 바인딩은 건너뛴다. 키를 놓으면 무시를 푼다.
   - **None이면 그 바인딩을 평가하지 않는다.** 값 초기화도 트리거 갱신도 하지 않는다. 단 alwaysTick 트리거가 있는 바인딩은 None이어도 평가한다(UE :220).
   - 평가하는 바인딩은 다음 순서로 처리한다.
     1. 액션의 이번 평가 첫 바인딩이면 액션 값을 0으로 초기화한다.
     2. 바인딩 모디파이어를 적용하고 바인딩 트리거를 평가한다(결합 3.3절).
     3. 값의 MagnitudeSquared가 **정확히 0이 아닐 때만** 누적한다(UE :249). 방식은 둘이다.
        - `TakeHighestAbsoluteValue`: 성분마다 절댓값이 큰 쪽이다. 같으면 나중 값이다(`>=`).
        - `Cumulative`: 합이다.
     4. 바인딩 트리거 결합 결과는 액션마다 **가장 진전된 하나**를 남긴다. 기준은 상태(None < Ongoing < Triggered)이고, 같으면 트리거를 평가한 바인딩이다(UE :281).
3. **주입 입력**: 바인딩 루프 뒤다(UE :619-667).
   - `InjectValue(action, value, triggers, modifiers)` — UE `InjectInputForAction`.
   - 한 평가에 여러 번 주입하면 각각 처리한다.
   - 직전 평가에도 주입되었으면 Held, 아니면 Actuated다. 이번에 다시 주입하지 않은 액션은 값 0으로 한 번 평가한다(놓기).
   - 연속 주입(`StartContinuousInjection`·`UpdateContinuousInjection`·`StopContinuousInjection`)은 평가마다 다시 주입한다.
   - 주입의 트리거·모디파이어 상태는 (액션, 주입 트리거·모디파이어 종류 열)마다 평가기가 둔다(4.6절).
4. **액션 마무리**: 이번 평가에 이벤트(바인딩·주입)가 있었던 액션만 한다(UE :671-717). 없던 액션은 상태 None이고 액션 트리거를 돌리지 않는다.
   1. 액션 모디파이어를 적용한다.
   2. 트리거가 없는 액션은 결합 상태를 다시 정한다. 모디파이어가 값을 바꿨을 때만 nonzero 기준으로 다시 정한다(UE :687-690).
   3. 액션 트리거를 바인딩 결합 상태에 이어서 평가한다(같은 결합 규칙).
   4. 바인딩 트리거가 있었으면 결과를 min(새 상태, 바인딩 상태)로 제한한다(UE :695).
   5. 일시정지 중이고 `!triggerWhenPaused`면 None이다.
   6. 전이표(3.4절)로 이벤트를 만든다. 경과 시간·Triggered 시간을 누적한다.
5. **결과**:
   - `ActionState(action)`: 값·트리거 상태·이번 이벤트·경과/Triggered 시간이다.
   - `Events()`: 이번 평가 이벤트 목록 (액션, ActionEvent, 값, 시간)이다. 바인딩 목록 순 → 주입 순으로 처음 나타난 액션 순서다.
6. **전달 뒤**: Completed·Canceled가 난 액션의 경과·Triggered 시간을 0으로 둔다. 전달이 끝난 뒤에 한다(UE :455-466).

`Flush()`(UE `FlushPressedKeys` :1021-1066):
- 눌린 디지털 바인딩을 다시 누를 때까지 무시한다.
- 진행 중이던 액션은 다음 평가에 Completed·Canceled를 낸다. UE 기본 `bSendTriggeredEventsWhenInputIsFlushed`가 참이다.

### 4.4 chord 전달 억제

chord 대상 액션(SourceAction)이 Triggered인 평가에서는 그 chord에 묶인 액션(DependantAction — chord 트리거를 가진 쪽)의 **모든 이벤트** 전달을 막는다(UE `OnlyTriggerLastActionInChord` 기본 1 — :846-861). 전달은 5.1절이다.

### 4.5 평가 문맥

`ActionEvalContext`:
- `double deltaSeconds`: **dilation을 뺀 dt**다. 게임 `TickContext::unscaledDeltaSeconds`다(UE `NonDilatedDeltaTime` :488-489).
- `double timeDilation`: 시간 트리거가 `affectedByTimeDilation`일 때 곱한다.
- `bool paused`
- `float fov`(도): FovScaling용
- `uint32 gamepad`: 이 플레이어 패드 번호

UE는 모디파이어·트리거·경과 시간 모두에 dilation을 뺀 dt를 넘긴다(:492·615·683·703-704). 일시정지 중에도 Rocks의 unscaled dt가 흐르므로 Hold 등이 진행된다(UE와 같다).

### 4.6 자원

- 바인딩 목록·상태 배열은 다시 만들 때만 할당한다(평가는 할당하지 않는다).
- 단 `Events()` 목록은 용량을 넘을 때 커진다(평가마다 비운다).
- 주입 상태 표는 처음 주입할 때 할당한다.
- 할당 실패: 다시 만들기 실패면 이전 목록을 유지하고 다음 평가에서 다시 시도한다(`OutOfMemory`를 `Evaluate` 결과로 돌려준다).
