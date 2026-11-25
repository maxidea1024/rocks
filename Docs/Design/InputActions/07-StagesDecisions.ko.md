# 입력 액션 계층 — 7. 단계·결정·시험

[목차](README.ko.md)

## 7. 단계·결정·시험

### 7.1 단계

| 단계 | 내용 | 시험 |
|---|---|---|
| IA1 | `InputActions` 모듈, `InputState::GamepadAxisRawValue`, 값·키 읽기(2.1·2.2절), 모디파이어·트리거·결합·전이(3절) — 순수 함수 | 모디파이어마다 UE 식 대조, 트리거마다 시간열 golden, 결합 규칙 조합표, 전이표 8칸 |
| IA2 | `InputAction`·`InputContext`·`InputBinding` 리플렉션 타입, `PlayerActionInput`(4절 전체) | 우선순위·소비·같은 컨텍스트 키 공유·chord·ChordBlocker·누적 두 방식·상태 보존·무시-놓을 때까지·flush·주입·일시정지 |
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
| IAD8 | 패드 축 | dead zone 전 원시 값을 읽고 dead zone은 모디파이어 | UE와 같다(게임패드 암묵 dead zone 없음). InputState 적용 값을 쓰면 이중 적용 |
| IAD9 | slot | 이름마다 slot 한 열(장치 칸 없음) | UE 기본 구현이 장치 ID를 Invalid로 돌려줘 실제로 한 열이다 |

### 7.3 UE와 다른 점

| 항목 | UE | Rocks | 이유 |
|---|---|---|---|
| RepeatedTap 시간 | 벽시계 | Tick 시간 | IAD3 |
| 같은 우선순위 순서 | 정해지지 않음(불안정 `ValueSort`) | 추가 순서 | 결정성 |
| 바인딩 3단 재정렬 | `RemoveAtSwap`(순서 바뀜) | 안정 분할 | 결정성 |
| 모디파이어·트리거 | UObject 파생 | 내장 enum | IAD2 |
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
