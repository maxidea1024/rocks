# 엔진 런타임 — 13. 시험

[목차](README.ko.md)

## 13. 시험

- `Engine` 타깃의 헤드리스 테스트: GameClock, Engine 상태 기계·실패 복구, World·Actor·Component 수명 순서, Tick phase·priority·interval·pause·지연 변경, 복수 World 격리, 지연 파괴, BMP 파싱, 에셋 파일 왕복, AssetManager 참조 계수.
- World 리플렉션(B8b — `WorldReflectionTests`·`WorldTests`·`EngineTests`): 네 타입의 WorldOnly·필드 형태와 멱등 등록, native 파생의 base 상속, GC(Spawn 생존·제거 회수·native 파생의 Component 생존·Marking 중 AddComponent의 newborn shade·Marking 중 참조 쓰기의 삽입 장벽·root 참조 방문과 파괴 시 비우기·우회 파괴 수리), Level 그래프 저장 → 별도 Registry 로드 → 필드·간선·root·소프트 참조 → 재저장 byte 동일(NATIVE 기본값, 계층 비저장), native 파생이 든 Level 저장 Unsupported, 장부 밖 객체 API 거부, null 원소 순회, 경로 읽기와 World 필드 쓰기 거부·fixture 쓰기와 옛 값, Engine 등록 순서와 native 파생을 둔 Shutdown·초기화 실패 정리.
- 규모 경로(2026-11-18 — 실험 16절): 같은 Level에서 끝 원소를 포함한 여러 Actor의 World API 밖 파괴, 우회 파괴와 `DestroyActor`가 같은 프레임에 겹치는 경우(둘 다 `Level::Actors()`에 남지 않고 회수된다), `RemoveReferenceAtSwap`(끝 원소가 DestroyRequested여도 성공, 장부·관찰자), `TakeDestroyRequests`(꺼낸 뒤 비고, 회수된 요청은 빠지고, 묶음이 작으면 나눠 꺼낸다), Level 우회 파괴(GC idle 중·cycle 도중 — 소속 Actor 전부 정리·회수), Component 단독 우회 파괴(틱 있음·없음), Tick hook·OnDestroy hook 안의 우회 파괴, Level encode 고정 위반의 Fatal 메시지(판정 함수), hook 안에서 활성화된 꺼진 Actor의 시작, TickScheduler의 끝에 붙이기·이미 지켜지는 선행 추가의 재구성 생략·의존받는 항목의 미룬 해제·도달 검사 번호 한 바퀴(모듈 Private의 `TickSchedulerTestAccess.h` — 시험 실행 파일은 엔진 계층에도 `Engine/Source/Runtime`을 include 경로로 둬 `<Module/Private/...>`로 읽는다). 각 시험은 고친 지점마다 음성 대조(옛 구현·잘못된 조건을 넣으면 실패)로 확인한다.
- GPU 시험은 별도 실행(`Sandbox --self-test`): D3D12(WARP 허용)와 OpenGL에서 offscreen 또는 backbuffer readback으로 삼각형·텍스처 픽셀을 확인한다. GPU 없는 CI에서는 실행하지 않고 NOT RUN으로 보고한다.
- 각 단계는 Debug·ASan 테스트와 `clang-format` 검사를 통과한 상태로 커밋한다.
