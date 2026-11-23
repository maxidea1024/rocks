# 엔진 런타임 — 9. 입력

[목차](README.ko.md)

## 9. 입력

- `Input` 모듈은 OS와 무관한 `InputEvent`(키 down/up·문자·마우스 이동·버튼·휠·게임패드 연결/해제)와 `InputState`를 정의한다. `InputState`는 한 프레임 동안 변하지 않는 스냅샷이다: 키·마우스 버튼·게임패드 버튼의 현재 눌림과 이번 프레임 edge(`WasPressed`/`WasReleased`), 마우스 위치·이동량·휠, 게임패드 최대 4개의 스틱·트리거(데드존 적용)와 버튼.
- `PlatformWindow`의 입력 backend가 창 메시지(키보드·마우스)와 XInput 폴링(게임패드)을 `InputEvent`로 바꿔 `InputQueue`에 넣는다. 키 식별은 물리 위치 기준 `Key` 열거(스캔코드 기반)이며 문자 입력은 별도 `Text` 이벤트다(IME 텍스트와 키 바인딩 분리).
- 프레임 순서는 계획 8.2절대로 입력 수집이 먼저다: hook `PumpEvents`가 메시지를 펌프하고 게임패드를 폴링한 뒤 `InputState::BeginFrame(queue)`가 큐를 소비해 새 스냅샷을 만든다. edge는 그 프레임에만 유효하다.
- pause 중에도 펌프와 스냅샷 갱신은 계속한다. 게임플레이가 소비하지 못한 edge는 다음 프레임으로 넘기지 않으므로 resume 시 과거 edge를 재발사하지 않는다. 창이 포커스를 잃으면 눌린 키를 모두 해제 이벤트로 정리한다.
- 액션 매핑·리바인딩·컨텍스트 우선순위는 이번 범위가 아니다. PlayerController(S12)가 LocalPlayer의 `InputState`를 읽는 경계까지만 만든다.
- 입력을 월드 이동·시점으로 바꾸는 매핑은 아직 없다(계획). 만들 때는 UE 5.8과 같은 축을 쓴다: 앞 입력은 전방(+X, `GetForwardVector`), 오른쪽 입력(D 키·왼쪽 스틱 +X)은 오른쪽(+Y, `GetRightVector`), 이동 속도는 cm/s, 시점 회전은 `Rotator` 도이며 yaw +는 위에서 볼 때 시계 방향(전방이 오른쪽으로 돈다)이다([수학 구현](../../Implementation/Math.ko.md) 2절).
- Sandbox는 WASD·방향키로 스프라이트를 움직이고, 마우스 위치로 삼각형을 회전시키며, 좌클릭으로 색을 바꾸고, 게임패드 왼쪽 스틱도 같은 이동에 연결해 확인한다.
