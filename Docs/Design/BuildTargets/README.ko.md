# 빌드 타깃·모듈 의존·플러그인 조건 설계

기준일: 2026-11-25, 개정 38. Rocks.Build에 없는 UE 5.8 빌드 기능 네 가지를 정한다.
- **타깃 종류 확장**: Editor·Client·Server 타깃과, 타깃 종류에 따라 코드·데이터를 컴파일에서 빼는 매크로.
- **모듈 의존 공개 범위**: 의존마다 Public(소비자에게 include·정의를 넘김)과 Private(이 모듈 안에서만 씀)을 나눈다.
- **플러그인 활성 조건**: 프로젝트가 플러그인을 특정 타깃·플랫폼·구성에서만 켠다.
- **링크 방식**: Editor 타깃은 UE와 같이 모듈마다 공유 라이브러리로 링크한다(모듈 API 매크로, 모듈 사이 공유 상태 규칙).

**전용 서버는 필수 요구다**(사용자 요구 2026-11-19). 네트워크가 구성되면 Rocks 게임은 렌더링 없는 전용 서버(dedicated server)로 돌 수 있어야 한다. 서버 플랫폼은 Windows·macOS·Linux다. 구현은 네트워크 단계에서 하지만 빌드 구조가 서버를 받을 수 있도록 스펙은 지금 잡는다. [전체 계획](../EngineImplementationPlan.ko.md)도 같은 요구를 담고 있다: 130행 플랫폼 표의 "Linux x64 서버", 103행 "M5 Linux 서버 통합", 2498행 "M4 server service boot/shutdown", 1610행 PC08 "Linux 서버와 각 플랫폼 client". 그래서 이 문서에서 Server 타깃과 Linux 빌드 플랫폼은 선택이 아니라 필수 항목이다(3.7절).

출발점은 UE 5.8 `Templates`(C++ 템플릿 8종의 `*.Target.cs`·`*.Build.cs`, `.uproject` 36개)와 Rocks.Build를 대조한 결과다. C++ 템플릿은 모두 `*Editor.Target.cs`를 가진다. 모듈 규칙은 `PublicDependencyModuleNames`·`PrivateDependencyModuleNames`를 나눠 쓴다. `.uproject` 21개가 `ModelingToolsEditorMode` 플러그인을 참조하고 그중 17개가 `TargetAllowList: ["Editor"]`로 에디터에서만 켠다. `TargetDenyList: ["Server"]`(TP_VehicleAdv·TP_VehicleAdvBP)와 `PlatformAllowList`(TP_UEIntro_BP)도 쓴다.

대상 코드는 `Programs/Rocks.Build`(ActionGraph·Descriptors·DescriptorModules·BuildInfo·NativeCompileProfile·InputValidation), `Programs/Rocks.Build.Abstractions/BuildRules.cs`, `Programs/Rocks.Build.Cli`(Program·MetadataRunner·MetadataLayout·InstalledSdk), `Programs/Rocks.Tooling.Checks`(ReleaseExclusion·PolicyChecks.TargetMacros), `Build/Rules/`(SourceModuleRules·ThirdPartyModuleRules), `Programs/FoundationTests/FoundationTests.Build.cs`다.

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, commit `f45729c0b6f0`)다. UBT 경로는 `Engine/Source/Programs/UnrealBuildTool/` 기준이다(약어 `UBT/`). UE 이름은 대응표와 근거에만 적는다. 레거시·deprecated 경로(`[Obsolete]` `bBuildEditor`, `BuildSettingsVersion`, `IncludeOrderVersion`)는 따르지 않는다.

상태: **설계 초안**(결정 BD1–BD44, BD17은 확정(사용자 결정 — UE 방식), BD18은 확정(요구), 나머지는 권장안). 구현 단계는 BS0–BS11(6절)이며 착수 시점은 사용자가 정한다. 초안 작성 뒤 서브에이전트 3개로 교차 검증하고, 반영본을 서브에이전트 1개로 다시 검증했다(10절). PCH·유니티 빌드는 [유니티 빌드·PCH 설계](../UnityBuildPch.ko.md)가 맡으며 이 문서의 범위가 아니다.

## 목차

- [1. 현재 상태](01-CurrentState.ko.md)
- [2. 용어 대응](02-Terminology.ko.md)
- [3. 설계](03-Design/README.ko.md)
- [4. 결정 요약](04-DecisionSummary.ko.md)
- [5. 결정 사항](05-Decisions.ko.md)
- [6. 단계](06-Stages.ko.md)
- [7. UE와 다르게 정한 점](07-DifferencesFromUnreal.ko.md)
- [8. 다른 문서 개정](08-OtherDocRevisions.ko.md)
- [9. 범위 밖과 채택하지 않은 UE 요소](09-OutOfScope.ko.md)
- [10. 개정 이력](10-Revisions.ko.md)
