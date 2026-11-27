# 작업 진행 보고와 취소 — 개정 이력

[목차](README.ko.md)

## 개정 이력

- 개정 1(2026-11-23): 초안. UE 5.8 세 층 구조 조사, Rocks 구조(어느 스레드에서나 보고·스냅샷 pull·소비자별 사건 cursor·UI 펌프 hook·프로세스 경계), API·오류 코드·한도 상수, 단계 PG1–PG6. 교차 검증 1차(리뷰어 1명, 필수 15·권장 8) 반영: UI 스레드 막는 작업(UI 펌프 hook), 발행 상태 seqlock 합성, 비율 합성 규칙, 자동 바인딩 제거, `RunParallelFor` 호출 스레드 처리(`ProgressBindingSuspend`·`ProgressParallelFor`), `Text` Clone 계약, 원문·id 추출, 기존 `ErrorCode`만 사용, 등록소 UE 정합(대상별·`Shutdown`·매개변수·순환 수락)과 Engine 배치·소유 스레드, 자식 pipe 소유·stderr·`--parent-control`, 셰이더 `job` 줄의 ShaderSystem 선개정, 소비자별 cursor, push 관찰자 제거.
- 개정 2(2026-11-27): 문서를 장 파일로 나눴다. 사용자 결정으로 시작 화면 세션이 PG1·PG2를 맡았다. PG1 완료(4장 구현 메모·13장 상태), 12장에 시작 화면 받는 쪽 행.
- 개정 3(2026-11-27): PG2 완료. 상수 이름에서 `k` 접두를 뗐다(Rocks 규칙 — `ProgressUiPumpInterval`·`DefaultProgressEventLogCapacity`는 뜻이 드러나게 바꿈). 하위 작업은 `externalCancel`을 받지 않는다(5.2절 — 취소 source는 부모 하나에만 연결), 오류 표 보강(빈 handle·`Initialize` 두 번·설정 범위·사슬 깊이), `ProgressBinding`은 같은 스레드 안 이동만, `ProgressParallelFor` 실제 서명(6.5절), 펌프는 활성 구간에서만, cursor 의미·마지막 진행 시각·재초기화·`Stats`(7장), 새 상수 넷(11장), 모듈 의존(3장), 비용 측정치(15.5절), 시험 파일(14장). 리뷰어 1명 교차 검증 반영: UI 펌프 등록 경쟁, 바인딩 소멸 검사에 기록 비교, 끝 사건 비율(5.3절), 작업 수준 계수 우선(5.5절), 끝난 작업 위 구간 no-op·깊이 넘친 구간의 취소(6.2절), `Shutdown` 로그를 잠금 밖으로, seqlock 읽기 회전 pause, 시험 보강.
