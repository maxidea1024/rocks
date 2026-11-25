# 메시·스켈레톤·애니메이션 에셋 설계

기준일: 2026-11-25, 개정 15(MA2b를 MA2b1–MA2b3으로 나누고 구현 명세 — 5.5절). 개정 14(MA2b 선행을 MaterialSystem MS3에서 MS3a — `MaterialBase` 골격 — 로 좁힘, 13절). 개정 13(MA 묶음 리뷰 반영 — 3.7절 12, 5.4절 16·17, 6.4절 6, 7.5절 10, 12·13절). 개정 12(MA3a 구현 — 4.8절·12.6절). 개정 11(MA6a 구현 — 7.5절·12.5절). 개정 10(MA5a 구현 — 6.5절·12.4절). 개정 9(MA3·MA5·MA6을 a·b로 나눔). 개정 8(MA4a 구현 — 6.4절·12.3절). 개정 7(MA2a 구현 — 5.4절·12.2절). 개정 6(MA2a·MA4a 분리). 개정 5(MA1 구현 — 3.7절). 개정 4(MA2 전 결정 반영 — index 형식·통합 bone map·메시 LOD 스트리밍). 개정 3(15절 요구를 다른 문서에 반영하고 상태 열을 더함). 개정 2(교차 검증 반영 — 17절). 정적 메시·스켈레톤·스킨 메시·애니메이션 클립의 에셋 자료구조를 정한다. 범위는 원본 데이터(에디터 데이터), 저장 설정, 빌드 계약, 쿠킹 데이터, 모듈 배치다. [엔진 구현 계획](../EngineImplementationPlan.ko.md) 13.1절(StaticMeshAsset)·15절(Skeleton·SkeletalMesh·AnimationClip)의 개요를 구현할 수 있는 수준으로 구체화한다. 텍스처는 [에셋 시스템](../AssetSystem/12-Textures/README.ko.md) 12절, 머티리얼은 [머티리얼 시스템](../MaterialSystem.ko.md) 4절이 이미 정했으므로 이 문서는 그 둘을 참조만 한다.

- 배경: 사용자 지시(2026-11-19). 엔진 시험용으로 UE에서 텍스처·정적 메시·머티리얼·애니메이션을 가져오는 기능을 넣는다. Rocks에 메시·애니메이션 자료구조가 아직 없으므로 함께 정한다. 가져오는 경로는 [Unreal 교환 형식](../UnrealExchange.ko.md)이 정한다. 이 문서의 자료구조는 그 경로와 상관없이 쓰는 엔진 기반이며, 뒤에 올 glTF·FBX 임포터도 같은 구조로 들어온다.
- UE 근거: UE 5.8.4(`release`, commit `f45729c0b6f0`, 로컬 클론 `C:\github\UnrealEngine`)를 읽어 확인했다. 경로 약어는 다음과 같다. `E/` = `Engine/Source/Runtime/Engine`, `RT/` = `Engine/Source/Runtime`, `Dev/` = `Engine/Source/Developer`. UE 이름은 대응표와 근거에만 쓰고, 타입 이름은 Rocks 이름으로 짓는다. 빌드 설정·축소 설정처럼 업계 공용 서술 이름은 그대로 쓴다.
- 좌표·단위는 UE 5.8과 같다(왼손, +Z 위, cm, `Rotator`는 도). 원본·쿠킹 데이터 어디에도 좌표 변환 계층을 두지 않는다. 삼각형 winding도 UE 원본 순서를 그대로 저장한다. 앞면 판정은 렌더 쪽 몫이다([셰이더 시스템](../ShaderSystem/README.ko.md)의 음수 행렬식 규칙).
- 착수 시점: 고정 구현 순서(… → A1–A6 → 화면 마일스톤)를 바꾸지 않는다. 이 문서의 단계(MA, 13절)를 언제 시작할지는 사용자가 정한다. 에셋 시스템 A1–A4(패키지·AssetDatabase·에셋 너겟·쿠커)가 선행 조건이다.

## 목차

- [1. 결정 요약](01-Summary.ko.md)
- [2. UE 5.8 대응](02-UeMapping.ko.md)
- [3. 원본 메시 `MeshSource`](03-MeshSource.ko.md)
- [4. 메시 공통 설정과 구성 요소](04-CommonSettings.ko.md)
- [5. 정적 메시 `StaticMesh`](05-StaticMesh.ko.md)
- [6. 스켈레톤과 스킨 메시](06-SkeletonSkinned.ko.md)
- [7. 애니메이션 클립](07-AnimationClip.ko.md)
- [8. 모듈 배치](08-Modules.ko.md)
- [9. 저장 세부](09-Storage.ko.md)
- [10. 임포트 서비스](10-ImportServices.ko.md)
- [11. 진단 코드](11-Diagnostics.ko.md)
- [12. 시험](12-Tests.ko.md)
- [13. 단계](13-Stages.ko.md)
- [14. 결정 사항](14-Decisions.ko.md)
- [15. 다른 문서에 반영할 요구](15-Requirements.ko.md)
- [16. 개정 이력](16-Revisions.ko.md)
- [17. 교차 검증 기록(개정 2)](17-CrossValidation.ko.md)
