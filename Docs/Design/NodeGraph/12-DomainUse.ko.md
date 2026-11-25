# 노드 그래프(NodeGraph) 설계

## 12. 도메인 적용

| 도메인 | 문서·단계 | 적용 |
|---|---|---|
| 머티리얼 | [머티리얼 시스템](../MaterialSystem.ko.md) MS1·14절, 계획 14.5–14.9절 | `MaterialGraph`는 `GraphDocument`(domain `Material`)다. 5.2절 노드·핀·간선 정의는 이 문서 6절과 같다. `MaterialNodeDescriptor`는 `NodeTypeDescriptor` + 머티리얼 확장이다(단계·capability, lowering은 MaterialCompiler 쪽 등록). 대응: E1 → 7절 메뉴, E2 → 8절, E3 → `CanConnect`, E4 → GD19, E13 → GD14·10절. 출력 노드는 `CreateDefaultNodes` + 문서당 하나 capability. 파라미터 기본값 제외는 `NormalizeSemanticNode` |
| 오디오 | [오디오 시스템](../AudioSystem/06-Phases.ko.md) AU10 `SoundEvent`, M7 `SoundGraph` | `SoundEvent`(랜덤·레이어·switch·지연·loop): 출력 하나로 모이는 트리. 자식 목록은 동적 입력 핀(속성의 입력 수)으로 둔다(GD4). `SoundGraph`(MetaSounds형): 데이터 핀 + 실행 trigger 핀(`Flow`). 블록 크기·실시간 안전성 검증은 도메인 몫. 런타임 그래프 생성이 필요하면 NG8 |
| 애니메이션 | 계획 15절(1522행) blend tree·state machine·graph compiler | blend tree는 데이터 그래프. state machine은 `Structural` 핀(전이)과 하위 그래프(상태마다). cook 때 cycle·타입·누락 핀 검증 → task plan |
| VFX | 계획 21절 | 모듈 스택·데이터 그래프. 컴파일은 도메인 |
| PCG | 계획 24.3절 | 데이터 그래프, 타입 변환 노드 자동 삽입(`MakeWithConversion`) |
| AI | 계획 24.1절 StateTree 계열 | 필요하면 `Structural` 핀과 `ordered` 출력 간선으로 표현. BT형 좌표 순서는 쓰지 않는다(GD4) |
| Sparks 시각 편집 | [레벨 스크립트](../LevelScript.ko.md) LD2(보류) | 만들게 되면 이 모델 + Sparks 코드 생성기. 실행은 Sparks. 공통 등록표에 gameplay 노드를 두지 않는다(GD20) |
