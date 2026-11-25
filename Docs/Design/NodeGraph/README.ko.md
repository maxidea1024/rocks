# 노드 그래프(NodeGraph) 설계

기준일: 2026-11-24, 개정 10. 이 문서는 여러 전용 편집기(머티리얼·사운드·애니메이션·VFX·PCG 등)가 함께 쓰는 노드 그래프 체계를 정한다. 범위는 다음과 같다.

- 그래프 데이터 모델과 도메인 schema 접점
- 변경·undo·통지
- 복사·붙여넣기·텍스트 형식
- 노드 등록·버전
- 그래프 편집 화면(`GraphView`)

UE 5.8 대응은 다음과 같다.

- 데이터: EdGraph(`UEdGraph`·`UEdGraphNode`·`UEdGraphPin`·`UEdGraphSchema`)
- 화면: GraphEditor 모듈(`SGraphEditor`·`SGraphPanel`·`SNodePanel`·`SGraphNode`·`SGraphPin`)
- 모델 우선 구조 대조: RigVM·MetaSound Frontend·PCG·Dataflow

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, commit `f45729c0b6f0`)다. 경로 약어는 다음과 같다. UE 이름은 대응표와 근거에만 적는다.

- `RE/` = `Engine/Source/Runtime/Engine/`
- `ED/` = `Engine/Source/Editor/`
- `PL/` = `Engine/Plugins/`

상태: **설계 초안, NG0 구현 완료(개정 5), NG1 구현 완료(개정 7), NG2 구현 완료(개정 8), NG3 구현 완료(개정 9·10)**. 결정 GD1–GD25는 모두 권장안이다(GD21·GD23의 ObjectRuntime 의존은 개정 4에서 사용자 결정). 교차 검증 1회를 반영했다(필수 9·권장 14·참고 11, 17절).

구현 단계는 NG0–NG8(14절)이다. 착수는 계획 순서(A0-B → A0-O → 리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 따르고, 이 문서 때문에 화면 단계를 앞당기지 않는다.

- 화면이 없는 NG0–NG4는 [머티리얼 시스템](../MaterialSystem.ko.md) MS1과 같은 시점이거나 그 직전이다.
- 화면 단계 NG5–NG7은 [UI 툴킷](../UiToolkit/README.ko.md) UT3·UT4 뒤다.

## 요약

**모델 우선(GD1).**
- 권위 데이터는 도메인과 무관한 그래프 모델(`GraphDocument`) 하나다. Object가 아닌 일반 데이터다.
- 화면은 이 모델을 직접 읽는다. UE EdGraph 같은 편집용 사본을 두지 않는다.
- UE 시스템들은 "도메인 모델 ↔ EdGraph" 양방향 동기화 때문에 보정 코드가 쌓였다(2.7절).

**식별과 저장(GD2–GD4·GD23).**
- 노드는 `NodeId`(Guid)로 식별한다.
- 핀은 노드 종류 서술이 선언한 안정 키 `PinKey`(Name)로 식별한다.
- 간선은 `(출력 NodeId, PinKey) → (입력 NodeId, PinKey)`로 한 곳에 저장한다. 핀 객체·양방향 포인터는 두지 않는다.
- 노드 속성은 리플렉션 Record 바이트(struct Guid + 버전 + 내용)로 보관한다. 그래서 모델 모듈이 Object 층에 의존하지 않고, 모르는 노드도 그대로 보존한다.
- 저장·비교는 ID 순 정규 순서로 한다.

**변경 경로는 하나(GD5–GD7).**
- 모든 변경은 `GraphMutator` 명령으로만 한다.
- 명령은 검증 → 되돌릴 수 있는 `GraphPatch` 생성 → EditorBackend Transaction 기록 → 세분 통지(`GraphChange`) 순서로 처리된다.
- undo는 역 patch를 같은 경로로 적용한다. 그래서 화면·컴파일러가 받는 통지가 편집 때와 같다.
- RigVM의 "controller + 명령 기록" 방식이다. 다만 UE 트랜잭션과 따로 도는 별도 스택은 두지 않는다.

**revision과 hash(GD8).**
- 문서 revision은 모든 편집에서 오르고, 충돌 검사에 쓴다.
- 컴파일 판정은 의미 데이터의 semantic hash로 한다.
- 위치·크기·주석·색·접힘 같은 배치 데이터는 별도 struct에 두고 hash에서 뺀다(계획 14.6·14.8절).

**schema는 정책만(GD9).**
- 도메인은 `GraphSchema`로 타입·연결 규칙·변환 노드 삽입·메뉴·검증을 제공한다.
- 색·위젯은 화면 쪽 시각 등록(`GraphVisualRegistry`)이 맡는다.

**복사·붙여넣기(GD12).**
- 모델 조각을 정규 JSON 텍스트로 내보낸다. 쓰기는 엄격 JSON이고, 읽기는 JSON5도 받는다.
- 붙여넣기는 공통 층이 ID를 다시 발급한다.
- UE의 "모델 객체를 EdGraph 노드 밑으로 잠시 옮기는" 소유권 꼼수는 쓰지 않는다.

**화면(GD15–GD17).**
- `GraphView`는 UE GraphEditor의 기능 범위를 따른다.
- 갱신은 통지 단위의 증분 갱신이다. culling·hover에는 공용 2D 격자 색인을 쓴다.
- 특정 도메인(Kismet 같은) 의존은 공통 층에 넣지 않는다.

**범위 밖(GD20).**
- Blueprint와 범용 gameplay 비주얼 스크립팅은 만들지 않는다.
- 공통 층은 실행 VM을 갖지 않는다.
- Sparks 시각 편집은 [레벨 스크립트](../LevelScript.ko.md) LD2대로 보류 선택지다.

## 목차

- [1. 현재 상태](01-CurrentState.ko.md)
- [2. UE 5.8 분석](02-UnrealAnalysis.ko.md)
- [3. 범위와 원칙](03-ScopeAndPrinciples.ko.md)
- [4. 용어 대응](04-Terms.ko.md)
- [5. 결정](05-Decisions.ko.md)
- [6. 데이터 모델](06-DataModel.ko.md)
- [7. 그래프 schema](07-GraphSchema.ko.md)
- [8. 변경·undo·통지](08-ChangesUndo.ko.md)
- [9. 복사·붙여넣기·직렬화](09-CopyPaste.ko.md)
- [10. 노드 등록·버전](10-NodeRegistration.ko.md)
- [11. 모듈과 화면](11-ModulesAndView.ko.md)
- [12. 도메인 적용](12-DomainUse.ko.md)
- [13. 검증](13-Verification.ko.md)
- [14. 단계](14-Stages.ko.md)
- [15. 다른 문서 개정 요청](15-OtherDocRevisions.ko.md)
- [16. 위험](16-Risks.ko.md)
- [17. 개정 기록](17-Revisions.ko.md)

