# 엔진 컴포넌트 도입 — 5. 묶음 순서

[목차](README.ko.md)

## 5. 묶음 순서

- 고정 순서를 바꾸지 않는다. CM0은 World 단계 작업이라 렌더를 기다리지 않지만, 착수는 A6 뒤 화면 마일스톤 첫 단계로 둔다(화면 출력을 앞당기는 제안이 아니다). **예외(사용자 결정 2026-11-24):** CM0은 엔진 런타임 S7–S14 바로 뒤로 앞당겼다. 상세 설계는 [엔진 런타임](../EngineRuntime/23-ComponentFoundation.ko.md) 23절이다. CM1 이후는 그대로다. 화면 마일스톤에 드는 것은 CM0과 카메라(CR11)까지다.
- 정식 렌더러는 화면 마일스톤 다음이고 첫 단계가 RS0, 그다음 RS1이다(RN 17절 841행). 그래서 CM1a(정적 메시)는 RS0과, CM1b(광원)는 RS1·RS3과 함께 한다.
- CM2 이후는 각 선행(렌더 RS 단계·물리·애니메이션·오디오 AU3·입력)이 들어오는 시점에 그 기능 단계와 함께 한다. 컴포넌트만 먼저 만들어 "동작하는 척"하지 않는다(시스템이 없으면 그 컴포넌트를 만들지 않는다).
- **예외(사용자 결정 2026-11-24, 데이터 먼저):** 렌더·물리 없이 정할 수 있는 컴포넌트의 데이터(UE 5.8 속성·기본값·단위·리플렉션·Mobility)는 먼저 넣고, 시스템 연결(CR12 `SceneChangeList`·RS 단계·물리 sweep)은 그 시스템이 들어올 때 붙인다. 지금 엔진만으로 완전히 동작하는 것(스플라인, sweep 없는 이동)은 끝까지 구현한다. 순서: D1 광원 + 라이트 액터 → D2 `PrimitiveComponent`·`MeshComponent`·`StaticMeshComponent` + `StaticMeshActor` → D3 `SplineComponent` → D4 이동(`MovementComponent`·`RotatingMovementComponent`·`ProjectileMovementComponent`·`SpringArmComponent`·`PathMoverComponent`, sweep 없이) → D5 대기·안개·decal·후처리·반사 캡처 데이터. 선행 시스템 없이는 빈 껍데기가 되는 것(skinned 계열·입력·오디오·ChildActor·Timeline·TextRender·SceneCapture)은 넣지 않는다. 상세 설계는 [엔진 런타임 24장](../EngineRuntime/24-ComponentData.ko.md)이다. 데이터만 들어간 컴포넌트는 렌더·물리 연결 전까지 아무것도 그리거나 충돌하지 않는다는 점을 각 클래스 주석에 적는다.
- 단계마다 완료 조건:
  1. 수명 순서 헤드리스 시험: 등록 → Awake → Enable → Start → Disable → Destroy → 해제에서 시스템 상태 생성·삭제가 한 번씩, 등록 실패 시 부분 등록 해제(CR1).
  2. 변경 목록 결정성: 같은 World 조작 열 → 같은 `SceneChangeList` 열(렌더 계열), 같은 물리 명령 열(충돌 계열).
  3. Transform·부착: 부착 규칙 셋 × 절대 플래그 셋 × 부모 비균일 scale 조합, socket 부착, Mobility 위반(Static 이동) 진단.
  4. GPU 계열은 RN 17절의 해당 RS 시험에 컴포넌트 경로를 더한다(기준 영상은 RS 단계 것을 그대로).
  5. UE 대조표(7절)의 속성 중 들이지 않은 것은 사유를 적는다.
