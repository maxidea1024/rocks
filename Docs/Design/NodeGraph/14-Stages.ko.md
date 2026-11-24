# 노드 그래프(NodeGraph) 설계

## 14. 단계

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| NG0 | 문서·배치 타입, 서술·등록표, 정규 순서, 불변식 검사, 의미 바이트 — **구현**(2026-11-23) | ObjectRuntime 리플렉션·Serialization(구현됨) | GT1 중 구조 사례(GI1–GI14 규칙마다 양성·음성), GT9 바이트 부분, GT14 의존 |
| NG1 | `GraphMutator`·공통 명령·patch·파생 색인·통지·bracket·Guid 발급기 — **구현**(2026-11-23) | NG0 | GT1–GT4, GT6 |
| NG2 | NodeGraphTools: 조각 JSON codec·붙여넣기·`graph-dump`·`graph-validate`·`graph-diff`(9.4절 NG2a–NG2d) — **구현**(2026-11-23) | NG1, ReflectionText(구현됨), 독립 struct Record(리플렉션 보강 26절 — NG2a) | GT5, GT10 |
| NG3 | migration·고아 핀·자리 노드·등록 해제·하위 그래프 경계 노드(10.1절 NG3a–NG3c) — **구현**(2026-11-24, NG3a·NG3b·NG3c) | NG1 | GT7·GT8, 접기·펼치기 왕복 |
| NG4 | EditorBackend Transaction·명령 schema 연결(계획 22.1절 RPC 포함), lane 소유 | NG1, EditorBackend Transaction(UT4 중 headless 부분) | Transaction 왕복·expected revision 충돌·`Busy` 시험 |
| NG5 | `GraphCanvas`·기본 노드·핀 시각·기본값 편집기·연결선·선택·drag·연결 drag, 최소 노드 메뉴(검색 목록), 편집 명령 결합(복사·붙여넣기·복제·삭제·undo·redo) | NG4, NG2, UiToolkit UT3 | GT11 기본, GT12 |
| NG6 | palette·주석·재경유·정렬·북마크·찾기·상하류 선택·2D 격자·성능 측정 | NG5, 공용 2D 격자(GR5) | GT11 전체, GT13 측정과 목표 확정 |
| NG7 | diff 화면·overlay hook·하위 그래프 이동·접기·펼치기 화면 | NG6, NG3 | diff·접기 화면 시험, 머티리얼 MAT M4 "정상 제작 Gate" 그래프 부분 |
| NG8 | 후속: 여러 노드 → 하나 migration, wildcard 해석 도우미, 런타임 그래프 생성(도메인 요구 시), 3-way merge(계획 22.4절 Linked 갱신) | 요구 시 | 각 항목 착수 때 이 문서 개정 |

**NG0 구현 범위.**
- 헤더(`Engine/Source/Runtime/NodeGraph/Public/NodeGraph/`): `GraphDocument.h`(문서·배치 리플렉션 타입), `NodeTypeDescriptor.h`(핀·노드 서술), `GraphSchema.h`(NG0 부분), `NodeTypeRegistry.h`(schema·서술 등록표, 핀 목록 계산), `GraphNameOrder.h`(Name 정규 순서), `GraphValidation.h`(정규화·전체 불변식 검사), `GraphSemantic.h`(의미 바이트·hash).
- 등록표는 NG0에서 소유 lane 하나가 쓰는 단일 스레드 객체다. 해제 drain·`NodeTypesChanged`·여러 스레드 조회는 NG3에서 정한다.
- 미루는 것: migration·`MigrateFn`(NG3), 경계 노드(NG3), `GraphMutator`·patch·통지·파생 색인·Guid 발급기(NG1), `GraphDiagnostic` 변환(NG1).

**NG1 구현 범위.**
- 헤더: `GraphCommand.h`(명령·사유 코드·실패 정보), `GraphPatch.h`, `GraphMutator.h`(pimpl), `GraphChange.h`(통지), `GraphIdSource.h`(무작위·seed 발급기), `GraphReadView.h`(읽기 view·순환 검사), `GraphDiagnostic.h`(진단 변환), `GraphClone.h`(문서 복제), `GraphSchema.h`(`CanConnect`·`CreateDefaultNodes`·`Validate`).
- 구현은 `Private/GraphMutatorCore.*`(op 적용·patch·통지·bracket·undo)와 `GraphMutatorCommands.cpp`(명령 규칙)로 나눴다.
- 시험: 명령별 왕복(GT1·GT2), 규칙 경계(중복 id·고아 핀 정리·GI8·GI10·크기 상한·LIFO·통지 중 구독 해제), 무작위 명령열 1만 단계(bracket 포함, GT3), bracket(GT4), 할당 실패 주입 세 목록(명령·구조·bracket 취소, GT6).

머티리얼 쪽 연결은 다음과 같다(계획 14.5절 표의 그래프 편집 행).
- MS1은 NG0–NG3을 쓴다.
- 머티리얼 에디터 M3 "최소 편집"은 NG4–NG5를 쓴다. 노드 추가(최소 메뉴)·연결·삭제·복사·붙여넣기·undo가 NG5에 들어 있다.
- M4 "정상 제작 Gate"는 NG6–NG7을 쓴다.
