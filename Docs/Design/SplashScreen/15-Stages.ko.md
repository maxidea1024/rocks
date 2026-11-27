# 시작 화면(스플래시) — 15. 단계

[목차](README.ko.md)

## 15. 단계

| 단계 | 내용 | 선행 | 완료 조건 |
|---|---|---|---|
| SP0 | 이 문서, 기본 이미지·템플릿 이미지·생성기 | — | 이 개정 |
| SP1 | `SplashScreen` 모듈·판정·이미지 찾기·기본 배치·모델·가짜 presenter, Windows presenter(스레드 DPI), `Engine` 모듈의 `ui.splash.*`, `Window::ShowAndActivate`, 대화상자 hook, Sandbox `--window` 연결(`--no-splash`, 제목·불확정 막대, 숨긴 주 창 넘기기) | — | 시험 1–4a·6·8·9, 사람 확인(Windows) |
| SP1b | 칸 배치 덮어쓰기·Custom 칸·막대 덮어쓰기(6.5절, 코드 API), `Sandbox --splash-self-test`(시험 10) | SP1 | 시험 4 덮어쓰기·10 |
| SP2 | `StartupProgress`·`StartupSplashSource`, `Engine::Initialize`·`ModuleLoader::StartPhase` 구간 보고, 작업 표시줄 진행, 기본 가중치 측정 | ProgressReporting PG2 | 시험 5, 사람 확인 — **완료**(2026-11-27. 시험 `StartupProgress_*`, 로더 시작 관찰자·Engine `ModuleStartProgress`, Sandbox 연결. 작업 표시줄 진행은 Studio 스플래시만이라 사람 확인은 SP4 때. 기본 가중치 측정은 8.3절) |
| SP3 | macOS presenter | MacPlatform(`feature/platform-mac`) 병합 | 사람 확인(Mac) |
| SP4 | Studio 연결(`StudioSplash`, 끌기, Suspend·Resume 자리) | Studio | 사람 확인 |
| SP5 | 측정 보정(8.2절) | SP2 | 시험 7 |
| SP6 | 게임 패키지·설치형 SDK에 스플래시 낱개 파일 싣기(엔진 데이터 폴더와 함께) | 패키징 단계 | 패키지 실행 확인 |
