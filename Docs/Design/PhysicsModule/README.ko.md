# Rocks 물리 모듈(Physics) 상세 설계

기준일: 2026-11-27, 개정 16(PH4 구현 중). 개정 16: PH4a 완료, PH4b 구현 세부(7.1.11절)·구현·리뷰 반영. 개정 15: PH3 완료, PH4 하위 단계·PH4a 구현 세부(7.1.10절), 5.1절 pullback 정정. 개정 14: PH3c1 완료, PH3c2 구현 세부(7.1.9절). 개정 13: PH3b 완료, PH3c 하위 단계·PH3c1 구현 세부(7.1.8절). 개정 12: PH3a 완료, PH3b 구현 세부(7.1.7절). 개정 11: PH2 완료, PH3 하위 단계·PH3a 구현 세부(7.1.6절). 개정 10: PH2c 완료, PH2d 구현 세부(7.1.5절). 개정 9: PH2b 완료, PH2c 구현 세부(7.1.4절). 개정 8: PH2a 완료, PH2b 구현 세부(7.1.3절). 개정 7: PH1 완료, PH2 하위 단계·PA 배정(7.1.2절). 개정 6: PH1 착수와 구현 세부(7.1.1절, PMD20–PMD22). 개정 5: PMU3·PMU4 닫음 — 선속도 상한 없음(PMD18), DOF 잠금은 월드 축·생성 시점 기준(PMD19). 개정 4: PMU2 닫음 — `normalImpulse`는 solver 충격량(6.4절, 어댑터 경계 10.6절). 개정 3: UnrealJolt 대조 근거를 3.3·6.4절에 이었다(본문은 어댑터 경계 11절 — 설계 변경 없음). 개정 2: 리뷰어 1명의 대조(치명 2, 중요 8, 경미 8) 반영 — object 층 32 bit와 운동·참여 bit(6.1절), Kinematic 정지·즉시 이동(3.3절), 시뮬레이션 컴포넌트 떼기(2.3.1절), 결과 반영 대상(3.4절), Jolt 마찰은 body 값(6.6절) 등. 7.6절.

이 문서 묶음은 M4 물리 단계(전체 구현 계획 16절, 컴포넌트 도입 CR16)에서 만들 `Physics` 모듈의 상세 설계다. 컴포넌트와 물리 장면 사이의 통합 구조는 UE 5.8.4를 따르고(사용자 지시 "애매하면 UE 방식"), solver는 Jolt다(Chaos 고유 기능은 재현하지 않는다). 사용자 결정(2026-11-25): 스펙은 계획 순서와 무관하게 지금 쓰고, 구현은 M4 순서대로 한다.

근거 문서:

- [전체 구현 계획](../EngineImplementationPlan.ko.md) 16절(통합 구조·소유권·질의·Jolt 빌드·큰 월드).
- [물리 어댑터 경계](../PhysicsJoltAdapter.ko.md)(좌표·단위 변환, Jolt 검증 결과, 라이브러리 반입 JL1).
- [충돌 데이터](../CollisionData.ko.md)(채널·응답·profile 표·`BodySettings`, CD1 구현·CD2 연결 — CD3이 이 문서 6장).
- [Cylinder 형상](../CylinderShape.ko.md) 4절(CY2), [컴포넌트 도입](../ComponentCatalog/README.ko.md) 3.10절·CM3·CM12–CM14, [엔진 런타임](../EngineRuntime/README.ko.md) 8·15·16·23·24장.
- UE 5.8.4 원본(로컬 클론). 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`, `PC/` = `Engine/Source/Runtime/PhysicsCore/`, `CH/` = `Engine/Source/Runtime/Experimental/Chaos/`.

| 장 | 내용 |
|---|---|
| [1. 범위·모듈·이름](01-ScopeModules.ko.md) | 범위, 모듈 배치와 의존, UE ↔ Rocks 이름 대응 |
| [2. 장면과 body](02-SceneBodies.ko.md) | 물리 장면 소유, World 쪽 접점, body 수명·재생성, 운동 종류, 힘·속도 API, 잠듦, weld |
| [3. step과 결과 반영](03-Stepping.ko.md) | 고정 스텝 통합, 쓰기 규칙, 결과 반영, teleport 종류, UE와 다른 점 |
| [4. 장면 질의](04-Queries.ko.md) | trace·sweep·overlap API, 질의 인자, 결과, 필터 판정, 정렬 |
| [5. sweep 이동과 이벤트](05-MovementEvents.ko.md) | `MoveComponent` sweep, 침투 해소, overlap 이벤트, hit 이벤트 |
| [6. Jolt 사상(CD3)](06-JoltMapping.ko.md) | 층·필터 bit, 질의 전용·탐침, body 값 사상, job·할당, 설정 |
| [7. 단계·시험·결정](07-StagesDecisions.ko.md) | PH 단계, 시험, 결정 표, 미확인, 다른 문서 개정 요청, 개정 이력 |
| [7.1 단계별 구현 세부(PH1–PH3)](07a-StageDetailsPH1-PH3.ko.md) | PH1–PH3 하위 단계의 구현 세부·리뷰 반영(7.1.1–7.1.9절) |
| [7.1 단계별 구현 세부(PH4)](07b-StageDetailsPH4.ko.md) | PH4 하위 단계의 구현 세부(7.1.10절 이후) |
