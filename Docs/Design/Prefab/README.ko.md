# prefab(원형 기반 재사용 객체 묶음) 설계

기준일: 2026-11-20, 개정 5(결정 D22 추가 — 레벨 저장 참조가 펼친 노드를 가리킬 때, [레벨 스크립트](../LevelScript.ko.md) LD17 — 3.6·3.11·4절, LevelScript 비범위 — 1.3절, 9절). 개정 4(2026-11-19)는 결정 D1–D21 확정(4절)과 편집 위치 규칙 반영(9절)이다. 개정 1은 사용자 지시("설계문서만 작성해놔")로 쓴 첫 초안이고, 개정 2는 교차검증 1차(치명 1·중요 14·사소 13·추가 2) 결과를 반영했다(9절). 구현하지 않는다. 착수 시점은 고정 구현 순서(리플렉션 B9j–B9m → Curve 데이터 계층 → A0-G → A1–A6 → 화면 마일스톤)를 따르며 이 문서는 그 순서를 바꾸지 않는다.

근거: [엔진 구현 계획](../EngineImplementationPlan.ko.md)(이하 EIP) 5.3절(클래스 기본값·서브오브젝트·공통 instancing)·20.3절(프리팹·중첩·변형·인스턴스 오버라이드)·20.5절(프리팹 식별자), [런타임 클래스 메타데이터](../RuntimeClassMetadata.ko.md)(이하 RCM) '클래스 기본값 결정', [리플렉션 B9](../ReflectionExtensions.ko.md)(이하 RX), [리플렉션·빌드 보강](../ReflectionFoundations.ko.md)(이하 RF), [객체·그래프 보강](../ObjectFoundations.ko.md)(이하 OF), [에셋 시스템](../AssetSystem/README.ko.md)(이하 AS), [엔진 런타임](../EngineRuntime/README.ko.md)(이하 ER), [GC 대규모 처리](../GarbageCollectionScale.ko.md)(이하 GCS — 13절 클러스터는 개정 2, `experiment/gc-scale` 브랜치 `b293feb`), [Sparks 언어](../SparksLanguage/README.ko.md)(이하 SL), 현재 코드(`Core/Source/Runtime/ObjectRuntime`·`ObjectSerialization`, `Engine/Source/Runtime/World`). UE 대조는 UE 5.8(5.8.4, `Engine/Build/Build.version` 5.8.4) 원본을 파일:행으로 확인했다(2절). 레거시·deprecated 코드는 대조에서 뺐다(2.0절).

원칙(RF·OF와 같다): 실패 원자성(spawn은 전부 공개 또는 전부 rollback, 공개 뒤 실패는 정상 파괴 경로), 결정성(같은 원형·override·spawn 인자 → 같은 객체 그래프와 같은 파생 ID), 모든 길이·개수·깊이에 이름 붙은 한도, 하위호환 없음. 구조는 UE 5.8을 따르고 이름은 Rocks식 서술형으로 짓는다. UE 이름은 6절 대응표에만 둔다(업계 공용어 prefab·archetype·variant는 그대로 쓴다). 좌표·단위·Transform은 UE 5.8과 같다(3.10절). 객체 규모는 엔진이 감당한다 — 1M+ 객체를 기준으로 하며 "객체 수를 줄이라"는 처방을 1차 해법으로 두지 않는다(3.9절).

약어:

- UE: `CUO/` = Engine/Source/Runtime/CoreUObject/, `ENG/` = Engine/Source/Runtime/Engine/, `ED/` = Engine/Source/Editor/, `CORE/` = Engine/Source/Runtime/Core/.
- Rocks: `OR` = Core/Source/Runtime/ObjectRuntime/Public/Object, `WD` = Engine/Source/Runtime/World/Public/World.

## 장 목차

- [목적·범위·비범위](01-Scope.ko.md) — 1절
- [UE 5.8 조사](02-UeResearch.ko.md) — 2절
- [원형·인스턴스 생성·ID·override](03-ArchetypeSpawnOverride.ko.md) — 3.1–3.5절
- [저장·전파·중첩·실행 비용·Transform·오류·스레드](04-StoragePropagationRuntime.ko.md) — 3.6–3.13절
- [결정·선행 조건·단계·시험](05-DecisionsStages.ko.md) — 4–5절
- [UE 대응표·다른 문서와 맞출 점·미확인·개정 기록](06-MappingAlignmentRevisions.ko.md) — 6–9절
