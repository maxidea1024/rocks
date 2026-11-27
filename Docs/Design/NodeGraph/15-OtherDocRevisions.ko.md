# 노드 그래프(NodeGraph) 설계

## 15. 다른 문서 개정 요청

GR1·GR2·GR3·GR5는 개정 3에서 반영했다. GR4는 오디오 문서를 다른 작업이 고치는 중이라 그 뒤에 반영한다.

| ID | 문서 | 개정 내용 | 상태 |
|---|---|---|---|
| GR1 | [UI 툴킷](../UiToolkit.ko.md) 2·6·9절 | "그래프 편집" 행을 `GraphView` 모듈(Editor 계층, 이 문서 11절)로 바꾸고, EditorFramework 책임에서 그래프 Element를 뺀다. UT6의 "그래프 Element"를 NG5–NG7로 잇는다 | 반영(UiToolkit 초안 2.4) |
| GR2 | [머티리얼 시스템](../MaterialSystem/README.ko.md) 3.2·5.2·5.5·14·15절 | 3.2절: MaterialSchema가 NodeGraph에 의존한다. 5.2절: 형식은 이 문서 6절을 참조하고, "재경유·주석은 화면 노드"를 "재경유는 의미 비교 때 접히는 공통 노드, 주석은 배치 데이터"로 고친다. 5.5절: semantic hash는 이 문서 8.4절 의미 바이트 + `NormalizeSemanticNode`(파라미터 기본값 제외)로 정의하고, 입력 기본값·활성 상태·고아 핀 포함을 명시하며, migration 뒤 계산임을 적는다. 14절: E1–E4·E13의 제공 모듈에 NodeGraph를 더한다. 15.1절: MS1 선행의 "리플렉션 B9(`InstancedStruct`) 노드 속성 저장"을 NG0–NG3(Record 바이트, GD23)으로 바꾼다 | 반영(MaterialSystem 개정 7) |
| GR3 | [엔진 구현 계획](../EngineImplementationPlan.ko.md) 14.6·22.2절 | 14.6절 MaterialSchema 행과 그래프 canvas 재사용 문장에 NodeGraph·GraphView를 적는다. 22.2절에 "노드 그래프 편집 patch는 노드 그래프 설계 8절"을 더한다(22.2절의 graph patch는 객체 계층용이므로 구분) | 반영(계획 개정 1.18) |
| GR4 | [오디오 시스템](../AudioSystem/06-Phases.ko.md) AU10 | `SoundGraph`가 NodeGraph 위에 선다고 적는다(오디오 개정 29로 `SoundEvent`는 없어짐) | 미반영 |
| GR5 | [공간 색인](../SpatialIndex.ko.md) | 2D 균일 격자 후속을 공용 구현으로 정하고, 그래프 화면 요구(GD16: 캔버스 좌표 bbox 질의·갱신 빈도)를 적는다. NG6 선행이다 | 반영(SpatialIndex 개정 8, 5.3절) |
