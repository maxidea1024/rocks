# 엔진 런타임·월드·그래픽 최소 경로 설계

기준일: 2026-11-12. 2026-11-25 갱신: 12절 끝에 엔진 셰이더 등록부 Freeze·lane 시작 순서(렌더 기반 9.12절), 10절 셰이더 서술을 쿠킹 코드·장치 셰이더 구조로(셰이더 시스템 SH2e2). 2026-11-24 갱신: 24–27절 컴포넌트 데이터 먼저(D1 광원·D2 Primitive·Shape·Trigger·D3 SplineComponent·D4 이동·D5 환경 상세 설계, ERD34–ERD53 — 사용자 결정으로 렌더·물리 없이 정해지는 데이터를 먼저), 23절 CM0 상세 설계(ERD25–ERD33 — 사용자 결정으로 CM0을 S7–S14 뒤로 앞당김). 2026-11-23 갱신: 15–22절 S7–S14 상세 설계(결정 ERD1–ERD24, 작업 순서 22.3절), 장별 파일로 분리, 2절 명칭에 PlayerState([컴포넌트 도입](../ComponentCatalog/09-Plugins.ko.md) CR48), 3절 `Gameplay` 행 상태, 7.3절 Component 보강 예정 항목, 8절 Tick 단계의 UE 대응 결정(물리 결과 반영·skeletal mesh 물리 마무리 엔진 단계, [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR1·CR2·CR8a·CR26·CD11). 2026-11-22 갱신: 6절 종료 게이트([오디오 시스템](../AudioSystem.ko.md) AD23). 2026-11-20 갱신: 4.6절 Level `levelScript` 필드·레벨 편입 순서·레벨 저장 검증, 7.2절 Level 파괴 순서와 LevelScript 파괴 규칙([레벨 스크립트](../LevelScript.ko.md) LD4–LD8·LD16·LD17). 2026-11-19 갱신: 7.4절 우회 파괴 복구 비용 측정, GC 일정(STW 병렬 전체 수집 요청·주기, 회수 예산, 엔진 소유 GC scheduler — GC 설계 17절). 2026-11-18 갱신: 4.5절 위치 교체 제거, 7.2·7.4절 Level 배열·우회 파괴 복구(매 프레임, Level·Component 포함, 근거 정정)·encode 고정 계약·시작 큐, 4.6절 편입의 배열 위치·Component 색인, 7.5절 World 소스 파일 구성, 8절 증분 틱 순서(GC 실험 [16절](../../Implementation/GcScaleExperiment.ko.md) 선형 검색 점검). 이 문서는 Core·Object 위에 처음 올리는 엔진 계층의 구조와 계약을 정한다. 범위는 **엔진 틱 → 월드 → Actor/Component → 텍스처 에셋 → D3D12·OpenGL로 화면에 그리기**까지의 수직 경로와, 그 위에 올릴 게임플레이 프레임워크(GameInstance·GameDirector·GameState·Controller·Pawn)의 골격이다. 전체 계획은 [엔진 구현 계획](../EngineImplementationPlan.ko.md)을 따르며, 이 문서와 충돌하는 부분은 개정 1.8에서 이 문서 기준으로 바꾼다.

## 목차

- [1. 범위와 단계](01-ScopeAndStages.ko.md)
- [2. 명칭](02-Naming.ko.md)
- [3. 모듈과 의존 방향](03-Modules.ko.md)
- [4. Object 시스템과의 결합](04-ObjectSystem.ko.md)
- [5. 시간](05-Time.ko.md)
- [6. Engine](06-Engine.ko.md)
- [7. World·Actor·Component](07-WorldActorComponent.ko.md)
- [8. Tick Scheduler](08-TickScheduler.ko.md)
- [9. 입력](09-Input.ko.md)
- [10. Graphics](10-Graphics.ko.md)
- [11. 에셋](11-Assets.ko.md)
- [12. 실행 경로(S6 완료 시)](12-ExecutionPath.ko.md)
- [13. 시험](13-Tests.ko.md)
- [14. 알려진 제한](14-KnownLimits.ko.md)
- [15. Tick 의존성 완성(S7)](15-TickDependencies.ko.md)
- [16. Subsystem 의존성(S8)](16-Subsystems.ko.md)
- [17. Actor 수명 완성(S9)](17-ActorLifecycle.ko.md)
- [18. GameInstance·LocalPlayer(S10)](18-GameInstance.ko.md)
- [19. GameDirector·GameState·PlayerState(S11)](19-GameDirector.ko.md)
- [20. Controller·Pawn(S12)](20-ControllerPawn.ko.md)
- [21. 초기화·종료 흐름과 시나리오(S13)](21-InitFlow.ko.md)
- [22. 진단·결정·작업 순서(S14)](22-DiagnosticsAndPlan.ko.md)
- [23. 컴포넌트 기반 보강(CM0)](23-ComponentFoundation.ko.md)
- [24. 컴포넌트 데이터 먼저(D1–D5)](24-ComponentData.ko.md)
- [25. SplineComponent(D3)](25-SplineComponent.ko.md)
- [26. 이동 컴포넌트(D4)](26-MovementComponents.ko.md)
- [27. 환경 컴포넌트 데이터(D5)](27-EnvironmentComponents.ko.md)
- [28. 영역 형상과 후처리 볼륨 혼합(CR17)](28-VolumeShape.ko.md)
