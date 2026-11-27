# 작업 진행 보고와 취소 — 12. 적용 대상과 지금 지킬 것

[목차](README.ko.md)

## 12. 적용 대상과 지금 지킬 것

| 기능 | 담당 | 모양 | 지금 지킬 것(PG 전) |
|---|---|---|---|
| 셰이더 컴파일 관리자(SH1d) | 렌더 세션 | 작업 수준 계수 + 대기 작업 종류 "Shaders" | **구현(PG5, 2026-11-27).** `Compile`의 `ShaderCompileControl`(취소 token·진행 sink 목록)과 `MakeShaderCompileCounterSink`(sink → `ProgressCounter`), 취소된 job 계수, 진행 중 묶음 취소(워커 강제 종료). 대기 작업 "Shaders"는 엔진이 가진 컴파일 관리자의 주인(Development 공급자)이 Engine `CountedPendingWork`로 등록한다(8장 PG5 연결). 워커 `job` 줄은 ShaderSystem 개정 27에서 들어왔다(10.3절). 자세한 규칙은 [셰이더 시스템](../ShaderSystem/11-WorkerCommunication.ko.md) 11.1절 PG5 구현 메모 |
| `shader-compile` 명령 | 렌더 세션 | 작업 수준 계수 | **구현(PG5).** 최상위 작업 "Compiling shaders"(Background, 사용자 취소 가능)의 작업 수준 계수, `--progress=console|jsonl`·`--no-progress`·`--parent-control`은 `CommandProgress`(9.5절)로, 취소면 종료 코드 130 |
| Development 셰이더 공급자(SH2e·SH4) | 렌더 세션 | 작업 + 작업 수준 계수, 대기 작업 "Shaders", 시작 단계 계수 | **구현(PG5).** 형식 라이브러리 만들기는 Background 작업 "Compiling shaders"(취소 불가), 감시 재컴파일은 "Recompiling shaders"(사용자 취소 가능 — 취소면 이전 셰이더 유지), 엔진 셰이더 준비는 `EngineShaderHost` 준비 진행으로도 알려 시작 화면 `Shaders` 단계 계수가 된다([시작 화면](../SplashScreen/08-StartupWeights.ko.md) 8.3절) |
| 임포트(A2f) | 에셋 세션 | 파일별 하위 작업 + 구간 | 임포터 단계 경계에서 `CancelToken`을 확인하는 자리를 둔다 |
| 쿠킹(A4–A5) | 에셋 세션 | 작업 수준 계수(패키지) + 하위 작업 | 같음. 쿠킹 진행 로그 줄은 9.1절 형식을 따른다 |
| 너겟 일괄 요청(A3) | 에셋 세션 | 구간 계수 | 요청 객체가 완료 수를 내놓는다 |
| 텍스처·오디오·메시 빌드 | 에셋·오디오 세션 | 구간, 비동기면 대기 작업 종류 | — |
| 언리얼 교환·Sparks 컴파일·저장 | 각 세션 | 작업 + 구간 | — |
| 시작 화면(스플래시) | 시작 화면 세션 | 받는 쪽: 스플래시 스레드가 시작 작업 스냅샷을 직접 당긴다(UI 펌프 hook 안 씀) | 시작 경로는 시작 작업 Primary 구간·단계 계수로만 보고([시작 화면](../SplashScreen/07-Progress.ko.md) 7절, SP2) |
