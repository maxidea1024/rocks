# 노드 그래프(NodeGraph) 설계

## 17. 개정 기록

- 개정 1(2026-11-22): 초판. 사용자 요청("언리얼의 노드그래프 시스템을 분석하고 rocks에 도입하기 위한 계획")에 따라 작성했다. UE 5.8.4 조사는 세 갈래로 했다.
  - EdGraph 데이터·GraphEditor 화면
  - 도메인 사용례: 머티리얼·Sound Cue·애니메이션 그래프·Behavior Tree·Niagara·PCG·Dataflow
  - 모델 우선 구조: RigVM·MetaSound Frontend·실험 플러그인

  결정 GD1–GD22는 모두 권장안이다.
- 개정 2(2026-11-22): 교차 검증 1회 반영. UE 인용 약 50개를 대조한 결과, 줄 범위·수치는 맞았다. 반영 내용은 다음과 같다.
  - **필수 9.**
    - 모델 모듈의 ObjectRuntime 의존 제거: 속성을 Record 바이트로 보관(GD23), NodeGraphTools 분리(GD21)
    - 모르는 노드 보존을 리플렉션 기능 없이 성립시키고, 해제 때 lease 문제 해소(GD14·10절)
    - `OrderedMap` 삽입 순서 대신 정렬 배열 정규 순서(6.3절)
    - 재경유 규정 통일(6.1·8.4절, GR2)
    - revision 하나 + semantic hash 판정(GD8)
    - 의미 바이트의 도메인 정규화 hook과 migration 뒤 hash(8.4절)
    - 간선 순번은 출력 핀 전용, `ReorderEdges` 추가, 조각에 order(GD4·8.1·9.2절)
    - JSON 쓰기는 Canonical 엄격 JSON(GD12·9.2절)
    - 최소 메뉴·편집 명령 결합을 NG5로 이동(14절)
  - **권장 14.**
    - bracket 중 다른 출처 `Busy`, interactive 미리보기(8.2절)
    - 고아 핀 재결합·제거 기록·타입 변경 처리(6.4절, GI8)
    - 해제 통지(10절), 검증 단계 용량 예약(8.2절)
    - 파생 색인과 지역 검사(6.1·6.3·11.3절)
    - UiCore hit grid 재사용과 공용 2D 격자(GD16·GR5)
    - `PinKey` = Name(GD2), 활성 상태 의미와 `DevelopmentOnly` 제외(6.5절)
    - 하위 그래프 경계 노드(GD25), 스레드 소유(GD24)
    - 계획 22.2절 인용 정정과 undo budget(GD6·1절·GR3)
    - 배치 struct 분리로 EditorOnly(9.1절)
    - seed 발급기 충돌 방지(8.5절·GI9)
    - UE 공통 명령 추가: 북마크·상하류 선택·연결 없는 핀 숨기기·삭제 후 다시 잇기·핀 연결 끊기·기본값 복원·복제·맞춤 zoom(11.2절)
  - **참고 11.**
    - `ImportNodesFromText` 파일 수, MetaSound Tick 호출 경로, `INodeUpdateTransform` 실험 버전
    - LOD 표현, 머티리얼 PostUndo 인용, Core 경로
    - 8.1절 명령 나열, 진단의 AssetId, `rocks::nodegraph` 이름공간
    - B9 구현 완료 표기(1절), Runtime 배치 근거(GD21)

  리플렉션 개정 요청(개정 1의 GR3)은 GD23으로 필요 없어져 지우고, 요청 번호를 GR1–GR5로 다시 매겼다.
- 개정 4(2026-11-22): NG0 착수 전 정리. 리플렉션 생성 codec이 ObjectRuntime 헤더를 써서 "리플렉션 struct로 저장"과 "ObjectRuntime 비의존"이 함께 성립하지 않는 것을 확인했고, 사용자 결정으로 문서 타입을 리플렉션 struct로 두고 NodeGraph가 ObjectRuntime에 의존하게 했다(원칙 1·GD21·GD23·11.1절·GT14). 객체·`TypeLease`·`InstancedStruct`는 계속 쓰지 않는다. 함께 정한 것: 이름 키의 NamePool 토큰과 원문 정렬 규칙, 배치 필드(`Vec2`·`LinearColor`, 그래프 이름은 배치), `domainRole`, 공통 노드 `Graph.Reroute`, 불변식 GI11–GI14와 알 수 없는 종류의 검사 범위, 의미 바이트 v1 형식과 재경유 접기의 순번 규칙, NG0의 `GraphSchema` 부분과 구현 범위(14절).
- 개정 5(2026-11-23): NG0 구현을 반영했다. 모듈 `Engine/Source/Runtime/NodeGraph`(14절 NG0 구현 범위의 헤더), 시험 53개(규칙마다 양성·음성, 경계 사례, 의미 바이트 golden hash), policy `nodegraph-boundary`(GT14). 리뷰어 1명 교차 검증(필수 2·권장 7·참고 9)을 반영했다.
  - 잘못된 Name 토큰은 검사를 오류로 끝내지 않고 GI11로 보고한다(6.3절).
  - 동적 핀도 정적 핀과 같은 핀 규칙으로 검사하고, 동적 핀 계산 오류는 GI13으로 보고한다. 속성이 틀린 노드는 동적 핀을 계산하지 않는다(6.2·6.3절).
  - 재경유 핀은 값 정책 Hidden이고, 재경유의 입력 기본값(GI14)·고아 핀(GI13)을 거부한다(6.2·6.3절).
  - 의미 바이트는 입력 배열 순서와 무관하게 정규 순서로 쓰고, schema가 무시한 쓰기 오류도 실패로 돌려준다. 머리의 그래프 수와 간선 필드 순서를 적었다(8.4절).
  - 동적 핀 표시 이름의 수명 규정을 하나로 맞추고, schema 등록 함수 이름을 코드(`RegisterSchema`)에 맞췄다(6.2·7절).
  - 함께 고친 것: 시험 실행기의 시험 수 상한(4,096 → 이름 있는 상수 16,384, EngineTests가 4,142개로 넘었다), policy의 여러 줄 의존 목록·`DynamicModules` 검사.
  - 남긴 것(NG1 이후): 핀·종류 조회의 선형 탐색(파생 색인과 함께), `MaxElements` 리터럴과 상수의 컴파일 시점 연결, `WriteSemanticBytes`의 바이트 복사 1회.
- 개정 6(2026-11-23): NG1 착수 전 정리. 8.2절 적용 절차를 "차례로 적용·검증, 실패하면 무할당 되돌리기"로 바꾸고, 8.6절(명령 목록, 맞바꾸기 op와 patch·합치기, 파생 색인, 명령별 검증 규칙, bracket·통지·Guid 발급기, schema 추가)을 더했다. revision 충돌 오류는 `StaleHandle`, 통지 원인에 `Cancel`을 더했다.
- 개정 8(2026-11-23): NG2 구현(NG2a–NG2d, 단계마다 리뷰어 1명 반영 — 9.4절 각 "구현에서 정한 점"). 착수 전 정리로 9.4절(레코드 값 텍스트·조각·문서 형식, 복사·붙여넣기 절차, 공통 명령 `InsertNode`·`InsertEdge`, schema `CanPasteNode`·`AllowExternalEdgeReconnect`, diff, commandlet)을 더했다. 속성 텍스트화에 필요한 독립 struct Record codec이 없어서 리플렉션 보강 26절로 기반에 더한다(사용자 결정, NG2a). 9.2절 예시를 레코드 값 형식으로 고쳤다.
- 개정 7(2026-11-23): NG1 구현을 반영했다(14절 NG1 구현 범위). 리뷰어 1명 교차 검증(필수 6·권장 9·참고 다수)을 반영해 8.6절을 보강했다.
  - undo·redo LIFO를 상태 토큰으로 검사(`StaleHandle`), bracket 취소의 할당 실패 의미(bracket 유지), 확정 무할당.
  - 명령 규칙: 고아 핀 자동 정리(GI7), 고아 핀 가림 거부(GI8), 재경유 순환 거부(GI10), 중복 id, 값·그래프 크기 상한(`MaxGraphNodes` 추가), 변환 노드의 동적 핀·`SinglePerDocument`·찬 입력, 순번 다시 매기기의 결정적 순서, `EdgeOrderChanged`는 핀마다 하나, 같은 바이트 속성은 무동작.
  - 발급한 id는 실패해도 돌려놓지 않음, 빈 목록도 revision을 올림, `InvariantBroken`(시험용 전체 검사).
  - 등록표 hash 조회의 완전성 표지, `WouldCreateCycle` 서명·비용 문구, GT3 출력 문구.
  - NG3·NG4로 미룬 것: 통지의 AssetId, 원인 Migration·Reload·Registry, `DocumentReloaded`·`NodeTypesChanged`, `RemoveGraph`의 부모 참조 정리, 경계 노드 이동.
- 개정 3(2026-11-22): 다른 문서 개정 요청 GR1·GR2·GR3·GR5를 해당 문서에 반영하고 15절에 상태 열을 더했다. GR4(오디오)는 미반영이다. GD16의 2D 격자 참조를 공간 색인 5.3절로 고쳤다.
