# 시작 화면(스플래시) 설계

기준일: 2026-11-27, 개정 9(PG5 — Sandbox `Shaders` 단계). 이전 개정 8(SP5 측정 보정)·7(SP2 시작 진행 연결)·6(기본 그림 교체)·5(SP1b 배치 덮어쓰기). 사용자 지시("언리얼처럼 엔진이든 별도 프로젝트든 시작할 때 스플래시를 띄우고 진행도를 보여줘야 한다. 데스크톱에서만 쓰고, 경우에 따라 안 띄울 수도 있다", "Rocks 스플래시 이미지와 프로젝트 템플릿 기본 스플래시도 제대로 만들어 둘 것")로 쓴 설계다. 결정은 권장안으로 확정했다(사용자 기준, 애매하면 UE 5.8 방식). 이미지(12절)는 이 개정과 함께 넣었고, 코드 구현의 착수 시점은 사용자가 정한다(15절 단계).

**범위.** 데스크톱(Windows·macOS)에서 게임·Rocks Studio·엔진 프로그램(Sandbox)이 시작할 때 OS 창으로 띄우는 스플래시와 시작 진행 표시다. 표시 여부, 이미지 찾기와 DPI, 글자 칸 배치, 진행 연결([진행 보고](../ProgressReporting/README.ko.md)), 시작 단계 가중치와 보정, 스레드 모델, 숨김 시점과 대화상자, 설정·명령줄, 기본 이미지, 시험을 정한다.

**범위 밖.** 모바일의 OS 실행 화면(Android splash theme, iOS LaunchScreen — 플랫폼 패키징 단계), 게임 중 맵 로딩 화면(UE `MoviePlayer`·`PreLoadScreen`의 `EngineLoadingScreen`), UI 툴킷으로 그리는 사용자 정의 시작 화면(UE `CustomSplashScreen`·`EarlyStartupScreen` — UiToolkit 뒤 별도 설계), Linux(BuildTargets BS6 보류와 같이 간다), 전용 서버(창이 없다).

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)다. 경로는 `Engine/Source/` 기준이다. deprecated API(`SetSplashText(TCHAR*)`)는 근거로 쓰지 않았다.

| 약자 | 파일 |
|---|---|
| `GPS` / `GPSC` | `Runtime/ApplicationCore/Public/GenericPlatform/GenericPlatformSplash.h` / `Private/GenericPlatform/GenericPlatformSplash.cpp` |
| `WPS` | `Runtime/ApplicationCore/Private/Windows/WindowsPlatformSplash.cpp` |
| `MPS` / `LPS` | `Runtime/ApplicationCore/Private/Mac/MacPlatformSplash.cpp` / `Private/Linux/LinuxPlatformSplash.cpp` |
| `LEL` | `Runtime/Launch/Private/LaunchEngineLoop.cpp` |
| `FCE` | `Editor/UnrealEd/Private/FeedbackContextEditor.cpp` |
| `UEG` / `UEM` | `Editor/UnrealEd/Private/UnrealEdGlobals.cpp` / `Editor/UnrealEd/Private/UnrealEdMisc.cpp` |
| `GE` | `Runtime/Engine/Private/GameEngine.cpp` |

## 목차

- [1. UE 5.8 조사](01-UnrealSurvey.ko.md)
- [2. 요구](02-Requirements.ko.md)
- [3. 구조 개요](03-Structure.ko.md)
- [4. 표시 여부](04-Visibility.ko.md)
- [5. 이미지](05-Images.ko.md)
- [6. 글자 칸과 배치](06-TextSlots.ko.md)
- [7. 진행 연결](07-Progress.ko.md)
- [8. 시작 단계 가중치와 보정](08-StartupWeights.ko.md)
- [9. 스레드와 수명](09-Threading.ko.md)
- [10. 설정과 명령줄](10-Settings.ko.md)
- [11. API 초안](11-Api.ko.md)
- [12. 기본 이미지와 템플릿](12-DefaultImages.ko.md)
- [13. 한도 상수](13-Limits.ko.md)
- [14. 시험](14-Tests.ko.md)
- [15. 단계](15-Stages.ko.md)
- [16. 결정](16-Decisions.ko.md)
- [17. 버린 대안과 후속](17-Alternatives.ko.md)
- [18. 다른 문서 개정](18-OtherDocs.ko.md)
- [19. 대응표](19-UnrealMapping.ko.md)
- [개정 이력](20-Revisions.ko.md)
