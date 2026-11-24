# 셰이더 시스템 설계

기준일: 2026-11-24, 개정 40(초안). 셰이더 원문을 HLSL 하나로 쓰고 오프라인에서 D3D12·D3D11·Vulkan·Metal·OpenGL ES용 코드로 바꾸는 구조와, 그 구현에 앞서 갖춰야 할 **선행 작업**을 정한다. 개정 7은 현재 코드베이스(HEAD `0be1e64`, 2026-11-14)와 다른 설계 문서의 상태를 다시 조사해 선행 작업과 순서를 새로 세웠다(4–6절).

UE 근거는 UE 5.8.4(EpicGames/UnrealEngine `release` 브랜치 commit `f45729c0b6f097038e72b52d366a62baeca3275b`)의 원본이다. 2026-11-14 사용자 지시로 기준을 5.6에서 5.8로 올렸고, 개정 6까지의 5.6 근거를 모두 5.8.4로 다시 확인했다(17절). 경로는 `Engine/Source/` 기준이며 `Engine/Shaders`·`Engine/Config`·`Engine/Build`는 그대로 적는다. UE 밖 도구(DXC·SPIRV-Cross·SPIRV-Tools·Vulkan)에 관한 사실은 그 출처를 따로 적는다. 개정 6까지의 조사 문서(UE 5.6 기준)는 [Rocks 셰이더 시스템](https://claude.ai/artifact/U5uNh2EWGvXKQiNYXnuc1Q)이다.

[엔진 구현 계획](../EngineImplementationPlan.ko.md)·[엔진 런타임](../EngineRuntime/README.ko.md)·[에셋 시스템](../AssetSystem/README.ko.md)과 충돌하는 부분은 이 문서 기준으로 바꾸고 그 문서들도 같이 고친다(14절). 개정은 이 문서가 확정될 때 함께 하되, 에셋 시스템 단계(A0-G·A1·A3·A4)에 넣을 셰이더 요구는 그 단계 착수 전에 반영해야 한다(6절).

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 선행 작업 가운데 P1·P3·P4·P6a·P9·P10·P11과 SH0(Windows)이 구현되었고, SH1은 2026-11-23 사용자 지시로 착수한다(11.1절). 처음에는 엔진 기반이 준비될 때까지 스펙으로만 두었다(2026-11-13 사용자 지시). 착수 위치는 6절, 착수 전에 정할 것은 15절이다.

## 목차

- [1. 결정](01-Decisions.ko.md)
- [2. 형식과 변환 경로](02-FormatsAndConversion.ko.md)
- [3. 구조 요약](03-StructureSummary.ko.md)
- [4. 현재 코드베이스(2026-11-14)](04-CurrentCodebase.ko.md)
- [5. 선행 작업](05-Prerequisites.ko.md)
- [6. 전체 순서](06-OverallOrder.ko.md)
- [7. P2 바인딩 규약](07-BindingConventions.ko.md)
- [8. P3 ThirdParty 확장과 도구 빌드](08-ThirdPartyToolBuild.ko.md)
- [9. P4·P10 도구 로드와 Release 배제](09-ToolLoadingReleaseExclusion.ko.md)
- [10. P5 에셋 너겟과 셰이더 job 키](10-NuggetJobKey.ko.md)
- [11. SH1 워커 통신](11-WorkerCommunication.ko.md)
- [12. P6a·P14·P6b 형식 정의·셰이더 타입·TargetProfile·쿠킹 매니페스트](12-FormatDefinitionsCooking.ko.md)
- [13. P7 MetaCompiler 셰이더 파라미터](13-MetaCompilerParameters.ko.md)
- [14. P8 다른 문서 개정](14-OtherDocRevisions.ko.md)
- [15. 결정 필요 사항](15-OpenDecisions.ko.md)
- [16. 이후 단계(선행 작업 뒤)](16-LaterStages.ko.md)
- [17. 개정 이력](17-Revisions.ko.md)
