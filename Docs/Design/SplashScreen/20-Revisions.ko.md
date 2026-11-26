# 시작 화면(스플래시) — 개정 이력

[목차](README.ko.md)

## 개정 이력

- 개정 1(2026-11-26): 초안. UE 5.8 스플래시 조사(플랫폼별 구현·표시 지점·이미지 찾기·에디터 진행·한계), Rocks 구조(`SplashScreen` 모듈·표시 판정·`@2x`/DPI·글자 칸·진행 중계 당기기·단계 가중치와 측정 보정·스레드·주 창 넘기기·대화상자 hook·설정·명령줄), API, 한도, 시험, 단계 SP0–SP6, 결정 SPD1–SPD12. 기본 이미지(Studio·게임)와 Foundation 템플릿 이미지, 생성기를 함께 넣었다. 같은 날 교차 검증 1차(리뷰어 1명, 필수 6·권장 8·사소 11)를 반영했다: 시작 경로를 시작 작업 Primary 구간과 단계 계수로만 보고(하위 작업 합성이 단계 가중치를 무력화), `Result<UniquePtr<StartupProgress>>`, Windows Show 비동기와 가짜 presenter seam, 스레드 단위 DPI 문맥(프로세스 DPI는 PlatformWindow 별도 작업), 설정을 branch `Engine` 섹션 `Splash`로·struct는 `Engine` 모듈(SPD13)·사용자 층 지정자·출시 세션 층 제약, `opensMainWindow`와 호스트별 입력 표(`NoMainWindow`), 단계를 호스트가 경계를 정할 수 있는 단위로(`HostServices`·`Engine`), macOS `RunOnMainThread`·`NSWindow`·common modes 타이머·잠금 규칙·원자 표시 hook, 원천 소유와 `release`, 주 창 넘기기의 추가 present·전경 실패, 불확정 막대 조건, 배율 1.0 초과면 `@2x`, `TaskbarButtonCreated`, 보정 시계·경로, UE 행 번호·`.bmp`·로그 수준 이름 정정.
