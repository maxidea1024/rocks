# 에셋 시스템 설계

기준일: 2026-11-23, 개정 13(5.7절 4항 객체 단위 EditorOnly를 켜는 시점 — [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR5). 개정 12([텍스처 스트리밍](../TextureStreaming.ko.md) T1·T5·T8 반영 — 12.1절 플랫폼 데이터 메타, 12.5.1절 덩어리 hash·상주 꼬리 수·비스트리밍·시네마틱 mip·bias 용어. 시네마틱 mip은 UE 5.8 원본 확인 결과 쿠킹이 따로 남기지 않는다). 개정 11([레벨 스크립트](../LevelScript.ko.md) 저장·쿡·로드 진단 — 4.6절). 개정 10(메시·애니메이션 스펙 [MeshAnimationAssets](../MeshAnimationAssets.ko.md) MR9·MR10과 [Unreal 교환 형식](../UnrealExchange.ko.md) UXR1–UXR3·UXR6 반영 — 3절 모듈, 4.6절 진단, 7절 임포터 태그, 12.2절 손실 원본 예외, 12.4.5절 메시 LOD 그룹, 14.1절 결정적 ID 발급기, 18절). 개정 9(A0-G 구현 — 1절 A0-G 행에 범위·스펙 차이, 2절 그래픽 행). 개정 8(텍스처 기반 클래스 `StreamableRenderAsset` — 12.1절, 사용자 결정). 개정 7(개정 6 최종 검증 반영: 저장 쪽 값 치환·SCC 키 튜플·Commit 간선 보고·Windows 디렉터리 동기화·PreserveBorder 정의). 원본 파일 임포트부터 쿠킹된 런타임 데이터 로드까지 에셋의 전체 수명주기와 첫 수직 단면(PNG → Texture Asset → 설정 수정·재임포트 → 플랫폼별 쿠킹 → 런타임 로드)을 정한다. [엔진 구현 계획](../EngineImplementationPlan.ko.md)의 결정을 따른다: §5.2(Outer·객체 경로), §5.3(instancing), §6.1(참조 종류), §7(직렬화·스키마 변경), §9(에셋 파이프라인·로딩·패키지·재임포트·텍스처 스트리밍), §24.2(PrimaryAsset·bundle). 계획을 바꾸는 곳은 17절에 모으고 계획 문서도 같이 고친다. 에디터 UI 연동은 범위 밖이며(사용자 결정) 서비스 API와 명령행 도구로 같은 기능을 제공한다.

원칙: 기반이 부족하면 우회하지 않고 기반을 보강한다(사용자 지시). 동작하지 않는 기능을 완성처럼 표시하지 않는다. 상태 표기: **구현**(이번 범위에서 동작·시험), **계약**(API·스키마·포맷이 있고 호출하거나 기본값이 아닌 값을 주면 명시 오류 — `Unsupported`·`UnsupportedSetting`·`UnsupportedCapability` — 를 반환), **후속**(이번 범위에 API도 없음).

## 목차

- [1. 단계와 완료 기준](01-Stages.ko.md)
- [2. 사전 조사(2026-11-12)](02-Research.ko.md)
- [3. 개념과 모듈](03-ConceptsModules.ko.md)
- [4. 식별과 경로](04-IdentityPaths.ko.md)
- [5. 패키지 포맷 v1](05-PackageFormat.ko.md)
- [6. 에셋 타입·객체 통합·참조](06-AssetTypes.ko.md)
- [7. AssetIndex(에셋 레지스트리)](07-AssetIndex.ko.md)
- [8. 동시성](08-Concurrency.ko.md)
- [9. 에셋 너겟](09-AssetNuggets.ko.md)
- [10. Cooker](10-Cooker.ko.md)
- [11. 런타임 로딩과 수명(계획 §9.2 계약)](11-RuntimeLoading.ko.md)
- [12. 텍스처](12-Textures.ko.md)
- [13. 명령행(Programs/Commandlet)](13-CommandLine.ko.md)
- [14. 검증 시나리오](14-Verification/README.ko.md)
  - [14.1 시험 fixture 에셋](14-Verification/14.1-TestFixtures/README.ko.md) — 단계별 대조표·coverage: [14.1.1–](14-Verification/14.1-TestFixtures/14.1.1-A1FormatManifestLoader.ko.md), [14.1.7–](14-Verification/14.1-TestFixtures/14.1.7-A1ManagerEngineSigning.ko.md), [14.1.13–](14-Verification/14.1-TestFixtures/14.1.13-A2EditingBase.ko.md), [14.1.27–](14-Verification/14.1-TestFixtures/14.1.27-A2IndexAliasesMoves.ko.md), [14.1.45–](14-Verification/14.1-TestFixtures/14.1.45-A2Import.ko.md), [14.1.53–](14-Verification/14.1-TestFixtures/14.1.53-A3Nuggets.ko.md), [14.1.61–](14-Verification/14.1-TestFixtures/14.1.61-A4Cooker.ko.md)
- [15. PNG 디코더 범위](15-PngDecoder.ko.md)
- [16. 임포트·빌드 의존](16-ImportBuildDependencies.ko.md)
- [17. 계획 문서 개정 사항](17-PlanRevisions.ko.md)
- [18. 범위 밖과 후속](18-OutOfScope.ko.md)
