# 작업 진행 보고와 취소 — 개정 이력

[목차](README.ko.md)

## 개정 이력

- 개정 1(2026-11-23): 초안. UE 5.8 세 층 구조 조사, Rocks 구조(어느 스레드에서나 보고·스냅샷 pull·소비자별 사건 cursor·UI 펌프 hook·프로세스 경계), API·오류 코드·한도 상수, 단계 PG1–PG6. 교차 검증 1차(리뷰어 1명, 필수 15·권장 8) 반영: UI 스레드 막는 작업(UI 펌프 hook), 발행 상태 seqlock 합성, 비율 합성 규칙, 자동 바인딩 제거, `RunParallelFor` 호출 스레드 처리(`ProgressBindingSuspend`·`ProgressParallelFor`), `Text` Clone 계약, 원문·id 추출, 기존 `ErrorCode`만 사용, 등록소 UE 정합(대상별·`Shutdown`·매개변수·순환 수락)과 Engine 배치·소유 스레드, 자식 pipe 소유·stderr·`--parent-control`, 셰이더 `job` 줄의 ShaderSystem 선개정, 소비자별 cursor, push 관찰자 제거.
- 개정 2(2026-11-27): 문서를 장 파일로 나눴다. 사용자 결정으로 시작 화면 세션이 PG1·PG2를 맡았다. PG1 완료(4장 구현 메모·13장 상태), 12장에 시작 화면 받는 쪽 행.
