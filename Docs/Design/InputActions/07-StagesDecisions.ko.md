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
| IA5b | Android 패드 진동(6.4절 — 입력 스레드 JNI, `VIBRATE` 권한) | 세기 바이트 변환·1초 다시 쓰기·포커스 0(시각을 인자로 준 시간열), Android 빌드 |
| IA6a | 에셋 타입(2.8절 — `InputAction`·`InputContext`의 `Asset` 파생, `HapticEffectAsset`, 실효 id, lease 읽기 도우미) | 리플렉션 형태(기반 `Asset`·필드), 쿠킹 패키지 로드(Hard 참조 액션 함께 로드)와 모은 bundle = source에서 모은 bundle(트리거·모디파이어·매핑 설정·프로필 override까지), 하위 객체 액션, 실효 id(0이면 AssetId), 도우미 `TypeMismatch`·무효·회수 lease·`WrongThread`·할당 실패(시도마다) 시 출력 그대로, 햅틱 사본·lease 재생과 그 실패 |
| IA6b | PlayerController 에셋 컨텍스트(5.4절) | Add(쥠·같은 에셋 다시 넣기는 lease를 가져가 놓음·다른 원천 같은 id 거부·실패 시 lease 그대로)·Remove(등록 횟수 0에 놓음)·Refresh(데이터 변경 반영·id 변경 거부·회수 lease)·직접 뺀 컨텍스트와 빼고 다시 넣은 같은 id(세대) 정리·EndPlay 해제·World 전체 Refresh |
| IA6c | 편집기(입력 에셋 편집 화면·키 선택·액션 고르기가 실효 id를 적음·복제 때 `id` 0으로 되돌림 — 2.8절) | Studio 단계 |
| IA7 | 함수 바인딩(`BindFunction` — 5.1절, 재사용 값 `FunctionBinding` — [이벤트 리플렉션](../EventReflection.ko.md) EV1a), `ActionEventData`·`InputValue` 리플렉션 struct, `ActionEvent` Flags enum | 함수 호출과 받은 데이터(이벤트 마스크 적용), 콜백과 섞인 실행 순서·`consumes`, 바인딩 거부(없음·static·반환·서명·None 마스크·null·회수된 대상), 대상 사라짐(StaleHandle) 뒤 바인딩 정리·파괴 대기 건너뛰기(경고 없음), 호출 실패에도 consumes, 다른 스레드 전달의 Warning 한 번, Remove·Clear |
| IA8 | `InputComponent`(priority·blockInput·바인딩)와 PlayerController 입력 스택(쌓기·정렬·위층부터 전달·층 사이 소비·막기·Push/Pop·EnableInput/DisableInput), Pawn 플레이어 입력 컴포넌트·`OnSetupPlayerInput` — 5.5절(CR26·CM4 입력 부분, 2026-11-27 당김) | 쌓는 순서·priority 정렬(같으면 쌓인 순)·위층부터 실행, 층 사이 consumes, blockInput 아래층 미전달·값 0, Push 자리·같은 priority 순서·중복 제거·Pop 값 0·사라진 항목 정리, 전달 중 Pop·파괴·UnPossess·Push(재진입), EnableInput 생성·재사용·조종 Pawn은 켜기·자기 층 끄기·다른 Controller Pawn 거부, Pawn 층 순서·Push한 Pawn 컴포넌트 한 번, Possess 때 hook·UnPossess 때 파괴·다시 Possess, Pawn 입력 끄기 |
| IA9 | 커브 에셋 slot(2.9절 — 응답 커브 모디파이어·진동 채널, CurveAssets CA5) | 액션·바인딩(기본·프로필)·채널 slot의 평탄화(인라인 대체·slot 비움), 범위 밖·null 원소·풀리지 않는 참조의 오류와 출력 그대로, 평탄화되지 않은 slot 검증 거부, 코드로 만든 값(slot 없음) 그대로, 쿠킹 로드 뒤 모으기(Hard 참조 함께 로드) |

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
| IAD12 | 사용자 설정 저장 | Core JSON으로 직접 쓰고 읽는다(5.3절) | 플레이어 설정은 Registry 밖 값이다. 버린 대안: 리플렉션 struct + ReflectionText JSON(Registry·타입 등록이 필요하다) |
| IAD11 | 키 저장 | 바인딩 키는 UE `EKeys` 이름 String, 평가기가 다시 만들 때 `InputKey`로 읽는다 | 저장물이 Rocks enum 순서에 묶이지 않는다(IAD4). 버린 대안: `InputKey{kind, code}` 숫자 저장(순서가 바뀌면 깨진다), 이름 codec 확장(리플렉션에 사용자 codec 기반이 아직 없다) |
| IAD8 | 패드 축 | dead zone 전 원시 값을 읽고 dead zone은 모디파이어 | UE와 같다(게임패드 암묵 dead zone 없음). InputState 적용 값을 쓰면 이중 적용 |
| IAD13 | 에셋 타입 | `InputAction`·`InputContext`가 바로 `Asset` 파생, 햅틱은 `HapticEffectAsset`이 값 struct를 감싼다, `id` 0이면 AssetId가 실효 id(2.8절) | UE `UDataAsset` 파생과 같다. 버린 대안: 감싸는 에셋 클래스(객체 두 겹, 얻는 것 없음), 복제·생성 hook으로 Guid 다시 만들기(AssetDatabase에 장치가 없고 코드 객체와 규칙이 갈린다) |
| IAD14 | 에셋 컨텍스트 보유 | PlayerController가 lease를 쥐고, 한 컨텍스트 id는 한 원천(평가기 칸 세대로 확인), 다시 모으기는 `UpdateContext`(5.4절) | World 객체는 에셋을 lease로 쥔다(AssetSystem 6.3절). 평가기는 사본만 가진다(IAD10). 버린 대안: 평가기가 lease를 쥠(InputActions 평가기가 AssetManager에 묶이고 시험이 에셋을 요구한다) |
| IAD15 | 함수 바인딩 | `FunctionBinding`을 쥐고 `ActionEventData` 하나를 넘긴다, 사라진 대상은 바인딩을 지운다(그 뒤로 소비도 멈춘다) | 이벤트 마스크까지 콜백과 같은 정보(UE는 값·시간·액션 넷), 목록이 자라지 않고 사라진 객체가 입력을 막지 않게(UE는 남겨 계속 소비) |
| IAD16 | 입력 스택 | `InputComponent`가 바인딩 층, PlayerController가 매 틱 UE 순서로 쌓고 priority 정렬, Actor 입력 켜기는 PlayerController API | UE BuildInputStack·ProcessInputStack. World는 Gameplay 아래라 Actor에 입력 API·필드를 두지 않는다. 레벨 스크립트 고정 자리는 Sparks 연결 뒤 |
| IAD17 | 커브 에셋 | 소유 에셋의 `curveAssets` 참조 목록 + 값 struct의 slot 번호, 사본을 만들 때 평탄화 | 값 struct에 참조를 둘 수 없다(2.7절). UE 하드 참조와 같은 로드·쿠킹 의미, 평가 경로는 Registry 없이 그대로 |
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
| 액션 식별 | 에셋 경로 | 실효 id(Guid `id`, 0이면 AssetId — 2.8절) | 상세 계획 21.1절, 이름 변경에 강하다. AssetId 대체로 새 에셋·복제가 겹치지 않는다 |
| 입력 컴포넌트 클래스 | `UInputComponent`(옛 입력) + `UEnhancedInputComponent` | `InputComponent` 하나 | 옛 입력이 없다 |
| Actor 입력 켜기 | `AActor::EnableInput(PC)`, Actor의 `bBlockInput`·`InputPriority` | `PlayerController::EnableInput(actor)`, 컴포넌트 필드 | IAD16 |
| 레벨 스크립트 입력 | 스택에 고정 자리 | `EnableInput`으로 넣는다 | IAD16 |
| 응답 커브 모디파이어의 커브 | `TObjectPtr<UCurveFloat>`(에셋만) | 인라인 `TangentCurve` + 에셋 slot | IAD17 — 코드로 만든 모디파이어도 커브를 갖는다 |
| 빠진·풀리지 않는 커브 에셋 | 응답 커브는 그 축만 0, 진동은 인라인으로 대신(`FRuntimeFloatCurve::GetRichCurveConst`) | 그 모으기·사본 전체가 실패(InvalidArgument·Resolve 오류) | CurveAssets CAD8 — 데이터 문제를 숨기지 않는다. null 원소는 Required라 저장·쿠킹이 먼저 막는다 |
| Pop·입력 끔의 값 바인딩 | Enhanced 값은 마지막 값을 쥔다(`ClearBindingValues`는 옛 축만) | 0으로 한다 | 빠진 층이 누른 값을 영구히 쥐지 않게(5.5절) |
| Push한 Pawn 플레이어 입력 컴포넌트 | Pawn 층에도 다시 넣어 두 번 처리 | Pawn 층에서 뺀다(한 번) | 한 프레임 두 번 실행은 버그 원천 |
| `DisableInput(actor)` 범위 | Actor의 `InputComponent` 하나 | 그 Actor 소유 스택 항목 모두 | Actor에 입력 컴포넌트 필드가 없다(IAD16) |
| 플레이어 입력 hook 시점 | `RegisterComponent` 전 | `AddComponent`(등록) 뒤 | 전달이 다음 틱이라 결과 같음 |
| `priority`·`blockInput` | UPROPERTY 아님 | ReadOnly 리플렉션 필드 | Inspector 표시·prefab 기본값 |
| 객체 함수 바인딩 인자 | `(FInputActionValue, float ElapsedTime, float TriggeredTime, const UInputAction*)` | `const ActionEventData&`(이벤트 포함) | IAD15 |
| 사라진 객체의 함수 바인딩 | 남겨 두고 `IsBound()`로 호출만 건너뜀, 소비는 계속 | 전달 뒤 지운다(소비도 멈춘다) | IAD15 |
| 컨텍스트 추가 인자 | 객체 포인터 | `AssetManager` + lease(PlayerController가 쥔다) | IAD14 |
| 같은 컨텍스트 id의 다른 원천 | 객체 포인터가 키라 없다 | `InvalidArgument` | 한 id는 한 원천(5.4절) |
| 컨텍스트 데이터 변경 반영 | 다시 만들기 요청만(객체를 그대로 읽는다) | 다시 모아 `UpdateContext` | 평가기는 사본(IAD10) |
| 겹친 바인딩 합치기 | 뒤 바인딩 값으로 앞 항목을 다시 만든다 | 정렬 순 첫 항목을 둔다 | 2.7절 첫 사본 규칙과 같게(종류가 같아 값만 다르다) |
| 매핑 이름 slot 넘침 | 8번째 Unspecified, 그 뒤 Max 칸 | 7개 넘는 바인딩은 행에 넣지 않고 경고 | 칸 7개만 둔다(5.2절) |
| 사용자 설정 불러오기 | 저장본에서 새 객체 | 같은 객체에 덮어쓰기(모든 매핑을 기본 키로 되돌린 뒤 적용) | 평가기가 가리키는 설정을 그대로 둔다(5.3절) |

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
