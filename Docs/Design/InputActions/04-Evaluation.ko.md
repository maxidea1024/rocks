# 입력 액션 계층 — 4. 평가

[목차](README.ko.md)

## 4. 평가

`PlayerActionInput`은 로컬 플레이어 하나의 컨텍스트 목록과 액션 상태다. UE `UEnhancedPlayerInput`과 입력 subsystem의 컨텍스트 목록(`IEnhancedInputSubsystemInterface`)을 합친 것이다. 용어는 다음과 같다.
- **chord 바인딩**: `ChordAction` 트리거를 가진 바인딩(또는 그 트리거를 가진 액션의 바인딩)이다.
- **chord 대상 액션**: 그 트리거가 가리키는 액션이다.

### 4.1 컨텍스트

API: `AddContext(bundle, priority, ContextOptions)` / `UpdateContext(bundle, forceImmediately)` / `RemoveContext(contextId, options)` / `ClearContexts()` / `HasContext(contextId)`. 컨텍스트는 `InputContextBundle` 사본으로 넣고 id로 가리킨다(2.7절). `AddContext`는 bundle을 검증하고(실패면 `InvalidArgument`, 상태는 그대로) 옮겨 온다(실패면 `OutOfMemory`, 상태·bundle은 그대로). bundle의 allocator는 컨텍스트가 빠질 때까지 살아 있어야 한다.

`ContextOptions`(UE `EnhancedInputSubsystemInterface.h:47-100`):

| 옵션 | 기본 | 뜻 |
|---|---|---|
| `ignorePressedKeysUntilRelease` | 참 | 다시 만들 때 눌려 있던 키를 놓을 때까지 무시한다(Boolean 액션만) |
| `forceImmediately` | 거짓 | 다음 평가를 기다리지 않고 바로 다시 만든다 |
| `notifyUserSettings` | 거짓 | 사용자 설정에 컨텍스트를 등록·해제한다(5.2절) |

동작:
- 추가·제거는 다시 만들기를 **예약**하고, 다음 평가 처음에 한 번 한다(UE 모듈 Tick — `EnhancedInputModule.cpp:298-328`).
- 예약된 요청들의 `ignorePressedKeysUntilRelease`는 AND로 합친다. 다시 만든 뒤 참으로 되돌린다(UE cpp:433·1201).
- `UpdateContext`는 있는 id의 사본만 바꾸고 다시 만들기를 예약한다(우선순위·등록 횟수 그대로, 없는 id면 `NotFound` — 2.7절).
- 이미 있는 컨텍스트(같은 id)를 다시 추가할 때 데이터는 건드리지 않는다(UE와 같다):
  - `Untracked`면 우선순위를 바꾸고 다시 만든다.
  - `CountRegistrations`면 횟수만 늘린다(우선순위 변경·다시 만들기 없음 — UE cpp:273-285). 제거는 횟수가 0이 될 때 빠진다.
- `RemoveContext`는 없는 id면 `NotFound`다(UE는 조용히 넘어간다 — 호출자가 무시할 수 있다). `ClearContexts()`는 모두 빼고 다시 만들기를 예약한다(UE `ClearAllMappings` — 기본 옵션).
- `SetKeyProfile(profileId)`: 활성 키 프로필이다(빈 값 = 프로필 없음 — UE `GetActiveKeyProfile`이 null). 바꾸면 다시 만들기를 예약한다. 사용자 설정(5.2절)이 부른다.
- 다시 만들기 시점: `ApplyPendingChanges(const InputState&, gamepad)`가 예약된 다시 만들기를 한다. `Evaluate`가 처음에 부르고, 시험·도구도 직접 부를 수 있다. `forceImmediately`는 그 자리(`AddContext`·`RemoveContext`·`UpdateContext` 안)에서 다시 만든다. 이때는 입력 스냅숏이 없으므로 "지금 눌려 있음"(4.2절 6)은 마지막 평가에서 눌려 있던 키로 본다(UE는 PlayerInput의 키 상태 — 마지막 입력 처리 결과와 같다). 그 기록은 모든 키(묶이지 않은 키 포함)의 고정 비트 집합이다(`InputKeySlot` — 할당 없음). `HasPendingRebuild()`로 예약 여부를 본다.
- 다시 만들기 실패(할당)면 이전 목록을 그대로 두고 예약을 남긴다(다음 `ApplyPendingChanges`에서 다시 시도하고 그 결과로 실패를 알린다). `forceImmediately` 호출은 등록·제거가 반영되었으므로 그 다시 만들기가 실패해도 성공을 돌려준다(호출자가 다시 시도하면 등록 횟수가 늘거나 `NotFound`가 되므로).
- 결과 보기(진단·시험): `BindingCount()`·`Binding(index)`는 다시 만든 바인딩 하나(액션 id, 컨텍스트 id, 키, 트리거·모디파이어 — 복제된 chord·넣은 Down·ChordBlocker 포함, 무시 중 여부)를 돌려준다. `ActionCount()`·`ActionId(index)`는 액션 표다.

### 4.2 다시 만들기(UE `EnhancedInputSubsystemInterface.cpp:886-1203`)

1. **정렬**: 우선순위 내림차순, 같은 우선순위는 추가한 순서다. UE는 불안정 정렬(`ValueSort`)이라 순서가 정해져 있지 않다. Rocks의 안정 순서는 UE와 다른 점이다(7.3절).
2. **바인딩 모음**: 컨텍스트마다 활성 프로필의 바인딩을 쓴다(`profileOverrides`에 활성 프로필 이름이 있으면 그것, 없으면 기본). 바인딩의 매핑 설정(`settingBehavior` — InheritFromAction은 액션의 것, Override는 바인딩의 것, Ignore는 없음)이 `supportedProfiles`를 가지고 활성 프로필이 거기 없으면 그 바인딩은 넣지 않는다(UE :986-990 — 활성 프로필이 없으면 거르지 않는다). 키 이름을 `ParseInputKey`로 읽는다. 빈 키는 그 바인딩을 조용히 건너뛰고, 모르는 이름은 Warning `InputBindingKeyUnknown`(컨텍스트 id·바인딩 번호·이름)과 함께 건너뛴다(2.7절). 리바인딩 가능한 바인딩은 키를 사용자 키로 바꾼다(5.2절).
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
   - 짝이 있으면 실행 상태를 넘긴다. 매개변수는 새 값을 쓴다. 넘기는 칸은 UE `OnTriggerReinstanced`·`OnModifierReinstanced`와 같다: 트리거는 직전 값·누름 시간·Hold 발사 여부·Pulse 횟수(RepeatedTap의 탭 진행은 새로 — UE도 새 instance), 모디파이어는 Smooth·SmoothDelta 상태 전부(`InputTriggers.cpp:68-80·133-139·199-207·336-344`, `InputModifiers.cpp:67-74·335-344`). 무시 중 표지도 넘긴다(UE는 옛 매핑을 옮겨 쓴다).
   - 액션 수준 상태(값·트리거 상태·시간)는 액션이 남아 있는 한 그대로다. 액션 트리거·모디파이어 실행 상태는 바인딩과 같은 규칙으로 넘긴다: 종류 열이 같으면 상태를 넘기고 매개변수는 새 값, 다르면 초기화한다(UE는 instance를 만들 때 복제해서 다시 생길 때까지 옛 트리거를 쓴다 — 7.3절).
   - 새로 생긴 Boolean 바인딩의 키가 지금 눌려 있고 `ignorePressedKeysUntilRelease`면, 놓을 때까지 무시한다.
   - 다시 만든 뒤 모든 바인딩에 dt 0으로 모디파이어를 한 번 돌려 상태를 맞춘다(UE :1188-1192).
7. **사라진 액션**: 바인딩에서 사라진 액션(진행 중 — 상태가 None이 아니거나 이벤트가 있던 것)은 값을 0으로 두고 표에 남긴다. 다음 평가에서 이벤트 없는 액션으로 마무리되어(상태 None) Canceled·Completed를 낸 뒤 지운다(UE `EnhancedPlayerInput.cpp:1171-1200`). 평가 전에 다시 만들기가 또 오면 다시 남긴다(UE도 매번 다시 모은다).
8. **액션 표**: 설정 원천은 정렬 순 bundle의 `actions`에서 처음 나온 사본이다(키 소비·필터와 무관 — 2.7절). 표 순서는 바인딩이 가리키는 액션(바인딩에 처음 나온 순) → 사라진 액션 → 주입 전용 액션(4.3절 3)이다. chord 색인은 앞쪽만 가리킨다(사라진 액션은 chord 대상이 아니다). 표 색인은 다시 만들 때와 평가 끝 정리(사라진 액션·놓인 주입 전용 액션을 지움)에서 바뀐다. chord 상태 작업 배열(앞쪽 칸 수)은 다시 만들 때 할당한다(평가 무할당 — 4.6절).

### 4.3 평가 한 번(`Evaluate(const InputState&, const ActionEvalContext&)`)

UE `EnhancedPlayerInput.cpp:488-717` 순서와 같다.

1. 앞 평가의 전달 뒤 정리(6)를 아직 안 했으면 먼저 한다. 그다음 예약된 다시 만들기를 한다. 다시 만들기가 실패(할당)하면 이전 목록으로 평가를 마치고 `OutOfMemory`를 돌려준다.
2. **바인딩 루프**: 바인딩마다 키를 읽어 키 이벤트를 정한다(UE :591-603).

   | 키 이벤트 | 조건 |
   |---|---|
   | `Actuated` | 이번 평가에 눌렸다(직전에 눌려 있지 않았다), 또는 이번 평가에 놓였다 |
   | `Held` | 눌린 채 이어진다. 아날로그가 계속 nonzero여도 Held다 |
   | `None` | 그 밖 |

   - 한 평가 안에 눌렀다 뗀 키는 눌림 값(1)으로 Actuated 한 번이다(UE :603-612·294-308).
   - 판정: 이번 눌림은 `KeySample`의 `down || pressed`(한 평가 탭 포함), 직전 눌림은 직전 평가에서 그 키가 `down || pressed`였는지다(모든 키의 고정 비트 집합 — UE `KeyDownPrevious = bDownPrevious || IE_Pressed`, 그래서 한 평가 탭의 다음 평가는 값 0으로 평가되어 Released·Tap이 나고 Pressed의 직전 값이 풀린다). 둘 다 거짓이면 None이다(Actuated와 Held는 평가 여부만 가른다 — 둘 다 평가한다).
   - 터치 키(UE `bCorrectTouchBoolActionKeys`): Boolean 액션은 값을 `down || pressed`의 1·0으로, 축 액션은 닿아 있지 않으면 0으로 둔다.
   - 무시 중인 바인딩은 키가 눌려 있으면 건너뛴다. 키가 놓여 있거나 이번 평가에 놓임이 있었으면(놓고 다시 누름 포함) 무시를 풀고 그 평가부터 평가한다(UE :575-586, `InputKey`가 IE_Released에서 바로 푼다 — :309-323).
   - chord 대상 상태: 바인딩을 평가할 때마다 그 액션의 바인딩 수준 결합 상태를 chord 작업 배열(4.2절 8)에 쓴다. 뒤 바인딩의 ChordAction·ChordBlocker가 읽는다(재정렬로 대상이 먼저 온다).
   - **None이면 그 바인딩을 평가하지 않는다.** 값 초기화도 트리거 갱신도 하지 않는다. 단 alwaysTick 트리거가 있는 바인딩은 None이어도 평가한다(UE :220).
   - 평가하는 바인딩은 다음 순서로 처리한다.
     1. 액션의 이번 평가 첫 바인딩이면 액션 값을 0으로 초기화한다.
     2. 바인딩 모디파이어를 적용하고 바인딩 트리거를 평가한다(결합 3.3절).
     3. 값의 MagnitudeSquared가 **정확히 0이 아닐 때만** 누적한다(UE :249). 방식은 둘이다.
        - `TakeHighestAbsoluteValue`: 성분마다 절댓값이 큰 쪽이다. 같으면 나중 값이다(`>=`).
        - `Cumulative`: 합이다.
     4. 바인딩 트리거 결합 결과는 액션마다 **가장 진전된 하나**를 남긴다. 기준은 상태(None < Ongoing < Triggered)이고, 같으면 트리거를 평가한 바인딩, 그것도 같으면 나중 바인딩이다(UE :281 `FMath::Max`).
3. **주입 입력**: 바인딩 루프 뒤다(UE :619-667).
   - `InjectValue(const InputActionData& action, value, triggers, modifiers)` — UE `InjectInputForAction`. 액션 표에 그 id가 있으면 표의 설정을, 없으면 넘긴 설정을 쓴다(주입 전용 항목 — 놓은 뒤 Completed·Canceled를 내면 지운다).
   - 한 평가에 여러 번 주입하면 각각 처리한다.
   - 직전 평가에도 주입되었으면 Held, 아니면 Actuated다. 이번에 다시 주입하지 않은 액션은 값 0으로 한 번 평가한다(놓기).
   - 연속 주입(`StartContinuousInjection`·`UpdateContinuousInjection`·`StopContinuousInjection`)은 평가마다 다시 주입한다.
   - 주입의 트리거·모디파이어 상태는 (액션, 주입 트리거·모디파이어 종류 열)마다 평가기가 둔다(4.6절). 매개변수는 마지막으로 넘긴 값이다.
   - 주입은 넘긴 설정과 트리거·모디파이어를 검증한다(2.7절 규칙 — 실패면 `InvalidArgument`). 할당 실패는 `OutOfMemory`이고 상태는 그대로다. 놓기 평가는 트리거·모디파이어 없이 값 0이다(UE :640-650).
4. **액션 마무리**: 이번 평가에 이벤트(바인딩·주입)가 있었던 액션만 한다(UE :671-717). 없던 액션은 상태 None이고 액션 트리거를 돌리지 않는다.
   1. 액션 모디파이어를 적용한다.
   2. 트리거가 없는 액션은 결합 상태를 다시 정한다. 모디파이어가 값을 바꿨을 때만 nonzero 기준으로 다시 정한다(UE :687-690).
   3. 액션 트리거를 바인딩 결합 상태에 이어서 평가한다(같은 결합 규칙).
   4. 바인딩 트리거가 있었으면(남긴 결합의 표지 — UE `bMappingTriggerApplied`) 결과를 min(새 상태, 바인딩 상태)로 제한한다(UE :695).
   5. 일시정지 중이고 `!triggerWhenPaused`면 None이다.
   6. 전이표(3.4절)로 이벤트를 만든다. 이벤트 없던 액션도 직전 상태에서 None으로의 전이(Canceled·Completed)를 낸다. 경과 시간은 상태가 None이 아니면, Triggered 시간은 이벤트에 Triggered가 있으면 dt를 더한다(UE :703-704 — Started·Triggered가 한 평가면 공개 이벤트는 Triggered).
   - 사라진 액션(4.2절 7)은 바인딩이 없으므로 이벤트 없는 액션으로 마무리된다.
5. **결과**:
   - `ActionState(actionId)` → `ActionStateView`: 값·트리거 상태·이번 이벤트·경과/Triggered 시간, 표에 있는지다. 표에 없는 액션은 빈 상태다.
   - `Events()` → `ActionEventRecord` 목록: 이번 평가에 이벤트가 있는 액션마다 (액션 id, ActionEvent, 값, 경과·Triggered 시간)이다. 액션 표 순서(바인딩에 처음 나온 순 → 사라진 액션 → 주입 전용)다.
   - `ChordDependencies()`: 다시 만들 때 모은 (chord 트리거를 가진 액션, 그 대상 액션) 짝이다(4.4절·5.1절 전달 억제용 — UE `DependentChordActions`).
6. **전달 뒤**(`FinishDispatch()` — 전달을 마친 쪽이 부른다. 안 부르면 다음 `Evaluate`가 처음에 한다, UE :455-480):
   - 공개 이벤트(Triggered가 있으면 Triggered, 아니면 그 이벤트)가 None·Canceled·Completed인 액션은 경과 시간을 0으로, Triggered가 아닌 액션은 Triggered 시간을 0으로 둔다.
   - 사라진 액션과 주입이 끝난 주입 전용 액션(이번에 주입하지 않았고 상태 None)을 표에서 지운다.

`Flush()`(UE `FlushPressedKeys` :1021-1066):
- 마지막 평가에서 키가 눌려 있던 디지털 바인딩(키보드·마우스 버튼·패드 버튼·터치 — 축은 아니다, UE `IsDigital = !IsAnalog`)을 놓을 때까지 무시하고 그 바인딩의 트리거·모디파이어 상태를 처음으로 둔다. Rocks `InputState`는 평가기 밖이므로 키 상태 자체는 지우지 않는다. UE는 키 상태를 0으로 둬서 놓을 때 Released·Tap이 늦게 나지 않는데, Rocks는 상태 초기화로 같은 결과를 낸다.
- 진행 중이던 액션은 다음 평가에 Completed·Canceled를 낸다. UE 기본 `bSendTriggeredEventsWhenInputIsFlushed`가 참이다.

### 4.4 chord 전달 억제

chord 트리거를 가진 액션(UE `SourceAction` — 예: Ctrl+S의 Save)이 이번 평가에 Triggered로 전달된 평가에서는 그 대상 액션(UE `DependantAction` — Ctrl)의 **모든 이벤트** 전달을 막는다(UE `ShouldOnlyTriggerLastActionInChord` 기본 참 — `EnhancedPlayerInput.h:290-297`, :846-861). "Triggered로 전달됨"은 Triggered에 묶인 바인딩이 실행 목록에 든 것이다(UE `TriggeredActionsThisTick` — :808-812). 전달은 5.1절이다(IA3).

### 4.5 평가 문맥

`ActionEvalContext`:
- `double deltaSeconds`: **dilation을 뺀 dt**다. 게임 `TickContext::unscaledDeltaSeconds`다(UE `NonDilatedDeltaTime` :488-489).
- `double timeDilation`: 시간 트리거가 `affectedByTimeDilation`일 때 곱한다.
- `bool paused`
- `float fov`(도): FovScaling용(0 이하면 카메라 없음)
- `bool hasPlayer`: 평가기가 플레이어에 붙어 있는지(FovScaling — 3.1절)
- `uint32 gamepad`: 이 플레이어 패드 번호

UE는 모디파이어·트리거·경과 시간 모두에 dilation을 뺀 dt를 넘긴다(:492·615·683·703-704). 일시정지 중에도 Rocks의 unscaled dt가 흐르므로 Hold 등이 진행된다(UE와 같다).

### 4.6 자원

- 바인딩 목록·상태 배열은 다시 만들 때만 할당한다(평가는 할당하지 않는다).
- `Events()` 목록은 다시 만들 때와 주입 전용 액션을 넣을 때 액션 수만큼 잡는다. 전달 중(평가 뒤 `FinishDispatch` 전)에는 전달하는 쪽이 목록을 돌고 있으므로 잡지 않는다(즉시 다시 만들기가 목록을 무효로 만들지 않는다). 그 경우 다음 평가가 용량을 넘으면 커진다.
- 주입은 호출마다 트리거·모디파이어 사본을 할당한다(평가 밖). 쓰지 않는 주입 항목(넣을 값·연속 주입이 없고 액션이 표에 없거나 놓인 것)은 전달 뒤 정리에서 지운다(해제만).
- 주입이 걸린 사라진 액션은 전달 뒤 정리에서 지우지 않고 주입 전용으로 바꾼다.
- 할당 실패: 다시 만들기 실패면 이전 목록을 유지하고 다음 평가에서 다시 시도한다(`OutOfMemory`를 `Evaluate` 결과로 돌려준다).
