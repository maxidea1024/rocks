# 노드 그래프(NodeGraph) 설계

## 13. 검증

에셋 시스템·Sparks 구현 시험과 같은 철저함을 요구한다. 규칙마다 양성·음성 사례를 둔다. undo 일치 판정은 정규 저장 바이트로 한다.

| ID | 시험 | 판정 |
|---|---|---|
| GT1 | 명령별 양성·음성 | 8.1절 명령마다 성공 사례와 불변식 GI1–GI10·schema 판정 위반 사례. 위반이면 문서 바이트 불변 |
| GT2 | undo·redo 왕복 | 명령마다 `적용 → undo`가 원 문서 바이트와 같고, `undo → redo`가 적용 결과와 같다. 통지 순서도 golden과 대조 |
| GT3 | 무작위 명령열 | seed 무작위 명령열(단계 수는 이름 있는 상수, 1만 이상). 매 단계 전체 불변식 검사, 임의 지점까지 undo 후 redo 결과 일치. 실패 시 seed·단계·명령 종류 목록 출력(최소화는 하지 않는다 — 같은 seed로 재현한다) |
| GT4 | bracket | 확정·취소·중첩 거부·bracket 중 다른 출처 `Busy`·같은 필드 반복 병합. 취소 후 바이트 불변 |
| GT5 | 조각 왕복 | 복사 → 붙여넣기의 id 재발급·간선 재연결·바깥 간선 정책. 같은 seed면 같은 바이트. 다른 도메인 거부. `propertiesRecord` 노드 바이트 보존. 미등록 텍스트 노드 제외와 진단 |
| GT6 | 실패 주입 | 할당 실패를 검증·예약 단계 각 지점에 주입. 문서 불변·`Result` 오류·누수 0. 적용 단계에서 할당 0 확인 |
| GT7 | migration | 버전 사슬, 키 재지정, 고아 핀 생성·재결합, 1:1 교체, 모르는 노드 왕복 바이트 불변, 에디터와 쿠커의 hash 일치 |
| GT8 | 등록·해제 | 해제 순서(조회에서 빼기 → 통지 → lease 놓기), 해제 뒤 문서 바이트·revision 불변과 자리 표시, 재등록 복원·이력 낡음(10.1절) |
| GT9 | 의미·배치 분리 | 배치 명령과 재경유 편집이 의미 바이트를 바꾸지 않음. `semanticTouched` 표지가 정확함. 도메인 정규화(기본값 제외)가 적용됨 |
| GT10 | golden dump | 표본 문서의 `graph-dump` 출력 golden |
| GT11 | 화면 CPU 시험 | 가짜 플랫폼(UiToolkit UT1)에서 hover·marquee·연결 drag·노드 drag·메뉴 검색·편집 명령 결합의 명령 결과, 통지 → 증분 갱신 대상 수 |
| GT12 | 화면 픽셀 | UiToolkit UV6 방식 픽셀 비교(노드·핀·연결선·LOD 단계) |
| GT13 | 성능 | 11.3절 측정, 노드 하나 변경의 갱신 비용이 크기와 무관함 |
| GT14 | 정책 | NodeGraph의 의존이 ObjectRuntime·CoreTypeBindings뿐이고 `InstancedStruct`·`TypeLease`·`ObjectHandle`·객체 Registry 헤더 include 0건(policy `nodegraph-boundary`), NodeGraphTools·GraphView의 도메인 의존 0건, GraphView의 Game·Client·Server 도달 0건(BuildTargets BD20) |
