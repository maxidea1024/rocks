## 8. 다른 문서 개정

개정 2(2026-11-22)에서 아래를 반영했다. 남은 것은 표의 "남음" 칸이다.

| 문서 | 고친 것 | 남음 |
|---|---|---|
| [엔진 구현 계획](../EngineImplementationPlan.ko.md) 개정 1.17 | 18절: 전용 AudioRT 스레드 + 출력 링(AD2), 이 문서 링크, 표지 1.17 항목 | — |
| [동기화](../Synchronization.ko.md) 개정 3 | 9.6절 `RealtimeWakeSignal`, D34, 단계 SY12(AD26). D27(AudioRT SPSC 입력)은 그대로 | — |
| [AppInfo·명령줄](../AppInfoCommandLine.ko.md) 개정 4 | `RunTraits::audioOutput`·`audioMuted`, `--no-sound`·`--mute-audio`·`--allow-commandlet-audio`, 8.6절 볼륨 CVar | — |
| [엔진 런타임](../EngineRuntime/06-Engine.ko.md) 6절 | 종료 게이트(`Busy`면 멈춤·이어 부르기, AD23) | — |
| [크래시 처리·실행](../CrashHandlingAndLaunch.ko.md) 개정 13 | 20절 호스트 재시도·Fatal | — |
| [플랫폼 계층](../PlatformLayer.ko.md) | "앱 수명 이벤트" 절(PR9) | — |
| [빌드 타깃](../BuildTargets/README.ko.md) 개정 20 | 오디오 모듈 유형·Server 쿠킹 payload 제외(AD24) | — |
| [에셋 시스템](../AssetSystem/README.ko.md) | 진단 `IgnoredSourceChunks`, `ReadyMask`와 오디오 준비 티켓의 관계(AD18) | 오디오 쿠킹 처리기·sidecar 청크 게시 세부는 A4 설계 때(AU5·AU6 전) |
| [Sparks 엔진 통합](../SparksEngineIntegration.ko.md) 개정 2 | SE-G7 오디오 의미 | `Engine.sparkbind.json` 스키마 변경(`Play2D`)은 AU3b 코드와 함께 |
| [Trace 시스템](../TraceSystem.ko.md) | — | `Audio`·`AudioMixer` 채널(AU8 전) |
