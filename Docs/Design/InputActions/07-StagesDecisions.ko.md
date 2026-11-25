# 입력 액션 계층 — 7. 단계·결정·시험

[목차](README.ko.md)

## 7. 단계·결정·시험

### 7.1 단계

| 단계 | 내용 | 시험 |
|---|---|---|
| IA1 | `InputActions` 모듈, `InputState::GamepadAxisRawValue`, 값·키 읽기(2.1·2.2절), 모디파이어·트리거·결합·전이(3절) — 순수 함수 | 모디파이어마다 UE 식 대조, 트리거마다 시간열 golden, 결합 규칙 조합표, 전이표 8칸 |
| IA2a | 리플렉션 타입(enum·`InputTrigger`·`InputModifier`·`InputBinding`·`InputActionSettings`·`InputContextData`·`InputAction`·`InputContext`), `InputContextBundle`·`CollectInputContext`·`ValidateInputContextBundle`(2.7절) | 필드 기본값 = UE, 모으기(액션 공유·Borrow 실패·할당 실패 상태 보존), 검증 규칙마다 양성·음성 |
| IA2b | `PlayerActionInput` 컨텍스트·다시 만들기(4.1·4.2절) | 우선순위·안정 순서·등록 횟수·소비(같은 컨텍스트 키 공유)·3단 재정렬·chord 복제·ChordBlocker(+ Down)·상태 보존 짝짓기·무시-놓을 때까지·사라진 액션·할당 실패 |
| IA2c | 평가 한 번·주입·flush·chord 전달 억제(4.3–4.6절) | 키 이벤트·None 건너뛰기·alwaysTick·누적 두 방식·가장 진전된 바인딩·액션 마무리·일시정지·전이·시간 초기화·주입(한 번·연속)·flush·억제 |
| IA3 | Gameplay 연결(5.1절) | 바인딩 전달 순서·Started 먼저·콜백 중 변경·값 바인딩·pause |
| IA4 | 사용자 설정·리바인딩·저장(5.2·5.3절) | 등록 slot, Map·Unmap, 프로필 전환, 재생성 반영, JSON 왕복·모르는 이름 |
| IA5 | 햅틱(6절), `input::MotorValues`, `GamepadBackend::SetMotors`(Windows) | 커브 평가·duration·loop·tag 교체·pause·최댓값 합성·scale·끔, XInput 모터 변환(순수 함수로 시험) |
| IA6 | 에셋 감싸기·편집기(후속) | — |

단계마다 구현 → 바뀐 시험 → 리뷰어 1명 → 커밋·push다.

### 7.2 결정

| ID | 항목 | 결정 | 근거·버린 대안 |
|---|---|---|---|
| IAD1 | 구조 | UE Enhanced Input과 같은 계층(액션·컨텍스트·바인딩·모디파이어·트리거·평가기) | 사용자 규칙(애매하면 UE). 상세 계획 21절의 processor 등록·컴파일된 profile은 버린다(UE에 없고 이득이 불분명하다) |
| IAD2 | 모디파이어·트리거 표현 | 종류 enum + 값 struct, 상태는 평가기 | UE Instanced 객체 복제 없이 결정적·할당 적음. 사용자 정의는 Sparks 단계 |
| IAD3 | 시간 | Tick dt만(RepeatedTap 포함) | 결정적 시험(벽시계 금지) |
| IAD4 | 키 이름 | UE `EKeys` 이름 그대로 | UE 프로젝트 설정·리바인딩 저장을 옮길 수 있다. 키 이름은 업계 공용 식별자다 |
| IAD5 | 소비 | 다시 만들 때 바인딩을 빼는 UE 방식 | 런타임 판정보다 단순하고 UE와 같은 결과다 |
| IAD6 | 출력 경계 | Gameplay는 모터 값만 내고 호스트가 플랫폼에 쓴다 | 입력 원천과 같은 방향. 시험이 장치 없이 결정적이다 |
| IAD7 | 저장 | JSON(리플렉션 struct) 바이트, 위치는 호출자 | SaveGame 체계 없음 |
| IAD10 | 객체 경계 | 바인딩은 액션을 Guid로, 컨텍스트는 `actions` ObjectRefArray로 GC 간선, 평가기는 bundle 사본만, 갱신은 `UpdateContext`(2.7절) | Borrow는 GC를 막고, `ObjectRef`·`StructRefArray`는 복사가 안 되며, chord 대상 참조는 RMC2304(참조 배열 중첩)에 걸리고, World 객체는 에셋을 lease로 쥔다. 버린 대안: `StructRefArray<InputBinding>`(chord 참조·사본 불가), 평가기가 Weak 참조를 쥐고 다시 만들 때 pin(평가기가 Registry·접근 문맥에 묶이고 시험이 객체를 요구하며 World → 에셋 경로와 맞지 않는다) |
| IAD11 | 키 저장 | 바인딩 키는 UE `EKeys` 이름 String, 평가기가 다시 만들 때 `InputKey`로 읽는다 | 저장물이 Rocks enum 순서에 묶이지 않는다(IAD4). 버린 대안: `InputKey{kind, code}` 숫자 저장(순서가 바뀌면 깨진다), 이름 codec 확장(리플렉션에 사용자 codec 기반이 아직 없다) |
| IAD8 | 패드 축 | dead zone 전 원시 값을 읽고 dead zone은 모디파이어 | UE와 같다(게임패드 암묵 dead zone 없음). InputState 적용 값을 쓰면 이중 적용 |
| IAD9 | slot | 이름마다 slot 한 열(장치 칸 없음) | UE 기본 구현이 장치 ID를 Invalid로 돌려줘 실제로 한 열이다 |

### 7.3 UE와 다른 점

| 항목 | UE | Rocks | 이유 |
|---|---|---|---|
| RepeatedTap 시간 | 벽시계 | Tick 시간(dilation 없음) | IAD3 |
| 숫자패드 Enter | `EKeys::Enter` 하나 | `Key::NumpadEnter`(이름 `NumPadEnter`)가 따로 있고, `Enter` 키가 둘을 함께 읽는다 | Input 모듈 키 집합. UE 바인딩 "Enter"는 같은 동작 |
| 시간 누적 | float `HeldDuration` | double | 임계와 정확히 같은 경계의 반올림만 다를 수 있다 |
| 같은 우선순위 순서 | 정해지지 않음(불안정 `ValueSort`) | 추가 순서 | 결정성 |
| 바인딩 3단 재정렬 | `RemoveAtSwap`(순서 바뀜) | 안정 분할 | 결정성 |
| 모디파이어·트리거 | UObject 파생 | 내장 enum | IAD2 |
| 바인딩의 액션 참조 | 객체 포인터 | 액션 id(Guid), 평가기는 사본 | IAD10 |
| 액션 수준 ChordAction이 읽는 대상 상태 | 대상 tracker(대상이 먼저 마무리되었으면 액션 트리거까지 반영 — TMap 순서) | 대상의 바인딩 수준 결합(늘 같은 시점) | 결정성(평가 순서에 따라 흔들리지 않는다) |
| chord 짝 모으기 | `ReorderMappings`에서 컨텍스트 전체 매핑(중복 포함) | 소비·프로필 필터를 통과한 바인딩에서 중복 없이 | 전달 억제에 쓰는 짝만 남긴다. 결과 차이는 걸러진 바인딩의 chord뿐 |
| 주입 처리 순서 | 액션마다 호출 순서 | (액션, 종류 열) 항목마다 호출 순서 | 같은 액션에 종류 열이 다른 주입을 섞을 때만 다르다(TakeHighest에서 절댓값이 같고 부호가 다를 때 결과가 다르다) |
| 실행 중 액션 트리거·모디파이어 편집 | instance를 다시 만들 때까지 옛 복제본 | 다시 만들 때 종류 열이 같으면 상태를 넘기고 새 매개변수 | IAD10(사본이 다시 만들 때 바뀐다) |
| 액션 식별 | 에셋 경로 | Guid `id` | 상세 계획 21.1절, 이름 변경에 강하다 |

### 7.4 들이지 않는 것(사유)

| UE | 사유 |
|---|---|
| GameplayTag 입력 모드 필터(`InputModeFilterOptions`) | GameplayTag 체계 없음 |
| 입력 컴포넌트 스택(`bBlockInput`, 층별 `bConsumes`) | CR26 입력 컴포넌트 스택 단계 |
| 레거시 축 설정 자동 모디파이어, `UE4_BackCompat` FOV | 레거시 |
| `Combo` 트리거 | 5.8 deprecated |
| `PerDeviceOverrides`·`DeviceProperties`(트리거 저항·라이트바) | 해당 장치 backend 없음 |
| `UForceFeedbackComponent`(위치 감쇠) | World 감쇠 설정 체계 없음 |
| 플랫폼 컨텍스트 redirect(`MappingContextRedirects`) | 플랫폼별 설정(BuildTargets·Config 기기 프로필)과 함께 |
| 사용자 정의 모디파이어·트리거 | Sparks 바인딩 단계(SE) |
| 디버그 표시(`ShowDebug EnhancedInput`) | 디버그 표시(CR23) |
| 콘솔 명령 `Input.+action` 등 | IA3 뒤 콘솔 명령 체계로 추가 |
| 디지털 트리거 버튼(`Gamepad_LeftTrigger`)·스틱 방향 버튼·`AnyKey` | Rocks 키 집합에 없다(InputState가 만들지 않는다). 키 집합을 넓힐 때 |
