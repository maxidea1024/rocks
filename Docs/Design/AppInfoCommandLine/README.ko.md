# 앱 정보와 명령줄 설계

기준일: 2026-11-22, 개정 4(오디오 실행 성격 `RunTraits::audioOutput`·`audioMuted`와 `--no-sound`·`--mute-audio`·`--allow-commandlet-audio` — [오디오 시스템](../AudioSystem/03-Design/03.08-EngineAndWorld.ko.md) 3.8.4절). 개정 3: 크래시 수집·실행 진입점 반영. 사용자 지시("설계문서만 작성해놔")로 쓴 설계이고 구현하지 않는다. 단, AP1의 일부(`ProcessCommandLine`·대화상자 억제 API·진입점 연결)는 [크래시 처리·실행 진입점](../CrashHandlingAndLaunch.ko.md)(LC3·LC4)이 먼저 구현했다(12절·18절). 결정은 권장안으로 확정했다(19절). 범위가 큰 항목은 19절에 따로 표시했다. 바뀐 점은 21절에 있다.

**착수 시점.** 고정 구현 순서(Curve → A0-G → A1–A6 → 화면 마일스톤)를 바꾸지 않는다. 이 문서의 단계(AP·CL — 18절)는 그 순서 안에 끼워 넣지 않으며 착수 시점은 사용자가 정한다. AP3(프레임 시간 전달)은 렌더 기반 RB1의 프레임 packet과 CVar 설계의 worker 묶음 장치에 기대므로 그 단계들과 함께 간다.

이 문서는 두 가지를 정한다.
- **앱 정보.** 프로세스 전역 정보(프로젝트 이름, 인스턴스·세션 ID, 빌드 정보, 실행 성격, 프레임 시간)를 어디에 두고, 누가 언제 쓰고, 어느 스레드가 읽는지.
- **명령줄.** 프로세스 명령줄 보관과, C++·C# 도구가 함께 쓰는 선언형 명령줄 틀(선언 한 곳에서 파싱·검증·도움말을 모두 만든다). 편의성 기준점은 C#의 System.CommandLine이고, C++이 그 수준을 따라간다.

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론 `C:\github\UnrealEngine`)다. 경로는 `Engine/Source/` 기준이다. 아래 약자를 쓴다.

| 약자 | 파일 |
|---|---|
| `APP` | `Runtime/Core/Public/Misc/App.h` |
| `APPC` | `Runtime/Core/Private/Misc/App.cpp` |
| `AT` | `Runtime/Core/Public/Misc/AppTime.h` |
| `ATC` | `Runtime/Core/Private/Misc/AppTime.cpp` |
| `LEL` | `Runtime/Launch/Private/LaunchEngineLoop.cpp` |
| `UEN` | `Runtime/Engine/Private/UnrealEngine.cpp` |
| `CLH` | `Runtime/Core/Public/Misc/CommandLine.h` |
| `CLC` | `Runtime/Core/Private/Misc/CommandLine.cpp` |
| `PAR` | `Runtime/Core/Public/Misc/Parse.h`, 구현 `Runtime/Core/Private/Misc/Parse.cpp`(`PARC`) |
| `CMD` | `Runtime/Engine/Classes/Commandlets/Commandlet.h` |

레거시·deprecated 경로는 대응하지 않는다. 구체적으로 다음을 뺐다.
- `FAppTime::Get_UnsafeNoCheck`(`AT` 107–112, `UE_DEPRECATED(5.8)`)
- `FApp`의 시간 setter 열 개(`APP` 661–756). 각 줄에 `// UE_DEPRECATED(5.8, "@todo delta time refactor - deprecate this function")`가 붙어 있어 폐기 예정이다. Rocks는 setter 대신 "게임 lane이 프레임 값 하나를 통째로 게시"하는 `FAppTime::Set`(`AT` 115) 방식만 따른다.

UE 이름은 6절 대응표와 근거 문장에만 쓴다.

## 목차

- [1. 요약](01-Summary.ko.md)
- [2. 범위](02-Scope.ko.md)
- [3. UE 5.8 `FApp` 조사](03-UnrealApp.ko.md)
- [4. UE 5.8 명령줄 조사](04-UnrealCommandLine.ko.md)
- [5. Rocks 현황](05-CurrentState.ko.md)
- [6. 타입과 이름](06-TypesAndNames.ko.md)
- [7. 모듈 배치와 코드 크기](07-ModulesAndSize.ko.md)
- [8. 앱 정보 `AppInfo`](08-AppInfo.ko.md)
- [9. 프레임 시간 `FrameTiming`](09-FrameTiming.ko.md)
- [10. 실행 성격과 명령줄·설정](10-RunTraits.ko.md)
- [11. 빌드 정보 주입](11-BuildInfo.ko.md)
- [12. 프로세스 명령줄 `ProcessCommandLine`](12-ProcessCommandLine.ko.md)
- [13. 명령줄 파서(C++) — 선언](13-ParserDeclaration.ko.md)
- [14. 명령줄 파서(C++) — 동작](14-ParserBehavior.ko.md)
- [15. C++과 C# 나란히 보기](15-CppAndCsharp.ko.md)
- [16. C# 도구](16-CsharpTools.ko.md)
- [17. 시험 계획](17-Tests.ko.md)
- [18. 구현 단계](18-Stages.ko.md)
- [19. 결정 목록](19-Decisions.ko.md)
- [20. 확인하지 못한 것](20-Unverified.ko.md)
- [21. 변경 이력](21-Revisions.ko.md)
