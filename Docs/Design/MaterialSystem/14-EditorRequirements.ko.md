# 머티리얼 시스템(MaterialSystem) 설계

## 14. 에디터 요구

그래프 편집 화면은 범위 밖이다. 아래는 에디터가 쓰도록 이 문서의 모듈이 제공해야 하는 기능이다. 그래프 데이터·편집 명령·undo는 공통 `NodeGraph`, 화면은 `GraphView` 모듈을 쓴다([노드 그래프](../NodeGraph/11-ModulesAndView.ko.md) 11절, UiToolkit 6절 "그래프 편집" 행).

| ID | 기능 | 제공 모듈 | 계획 대응 |
|---|---|---|---|
| E1 | 노드 타입 목록·검색 메타데이터(분류·키워드·핀 설명) | MaterialSchema(NodeGraph 노드 등록표·메뉴 질의, 노드 그래프 7·10절) | 14.5 "node 검색·palette" |
| E2 | 편집 명령 API(노드 추가·삭제·연결·속성 변경)와 expected revision transaction, undo/redo 단위 | NodeGraph `GraphMutator`·patch(노드 그래프 8절) + EditorMaterial(EditorBackend transaction 연결, 도메인 명령) | 14.8 |
| E3 | 연결 가능성 질의(핀 타입·단계·암묵 변환 결과) | MaterialSchema `GraphSchema::CanConnect`(타입 표·단계, 노드 그래프 7절) + MaterialCompiler(V1 부분 검사, 동기·빠름) | 14.5 "typed pin 연결" |
| E4 | 진단 목록(노드·핀 위치, 심각도) | MaterialCompiler(형식은 NodeGraph `GraphDiagnostic`, 노드 그래프 GD19) | MT02 |
| E5 | 컴파일 상태 이벤트(대기·번역·job 진행·성공·실패·마지막 성공과 다름) | CompileCoordinator | 14.8 |
| E6 | 미리보기 세션(메시·조명·환경·카메라·렌더 경로 전환) offscreen 렌더 | PreviewSession + Renderer | 14.5 "실시간 확인" |
| E7 | 노드 미리보기 요청·해상도·동시 수 상한 | CompileCoordinator | 14.5, 14.8 |
| E8 | 생성 원문 보기와 원문 위치 지도 | MaterialCompiler | 14.5 "generated HLSL" |
| E9 | 통계: IR 기준(텍스처·샘플러·보간·상수 크기·값 수), (pass, 퍼뮤테이션, 형식)별 머티리얼 텍스처 상한과 사용 수(Renderer 7.4절·RQ18), 형식별 컴파일 결과(명령 수 등, 도구가 주는 값). 정적 지표와 GPU 측정을 구분 | MaterialCompiler·ShaderCompiler | 14.5 "진단·비용" |
| E10 | 퍼뮤테이션 수(프로필별)·static 변형 추가 비용 표시 | MaterialCook(조회 API) | 14.1, 14.5 |
| E11 | 인스턴스 편집 데이터(파라미터 그룹·정렬·범위·override 여부·reset) | Material(에셋 모듈) | 14.5 "인스턴스 편집" |
| E12 | 의존 추적(이 함수를 쓰는 머티리얼, 이 머티리얼의 인스턴스) | AssetIndex 역방향 의존 | 14.5 "재사용" |
| E13 | 모르는 노드 자리 표시·migration 결과 보고 | NodeGraph(자리 노드·migration 실행, 노드 그래프 10절) + MaterialSchema(migration 함수) | 14.6, MT10 |
| E14 | headless 번역·쿠킹 명령(`material-compile`·`asset-cook`) | MaterialCook·Programs/Commandlet | MT08 |
| E15 | 새 노드 초기 배열 원소(UE 생성자가 더하는 Custom 입력 하나, Operator `Arity`개, Switch 입력 하나 — 정적 정의에는 배열 기본값이 없다, 19.13.3절) | EditorMaterial(노드 추가 명령이 정의별 초기 원소를 채운다) | 14.5 "node 검색·palette" |

화면 코드는 shader 의미·검증을 다시 구현하지 않는다(계획 14.6절). E3·E4는 화면이 부르는 유일한 검증 경로다.
