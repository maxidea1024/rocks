# 엔진 컴포넌트 도입 설계(UE 5.8 컴포넌트 대응)

기준일: 2026-11-24, 개정 8(CM0 앞당김 — 사용자 결정, 4.7절 셰이더 선행, 개정 기록 11절). 개정 7: ER에 PlayerState 명칭 반영(9.9절). 개정 6: 3부 플러그인 컴포넌트(9절). 결정 CD1–CD24는 권장안으로 적었다(6·8.7·9.8절). 구현하지 않는다. 착수 시점은 고정 구현 순서(리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 따르며 이 문서는 그 순서를 바꾸지 않는다(5절). 예외는 CM0이다(사용자 결정 2026-11-24 — 구현됨, ER 23절).

이 문서는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)의 `Engine/Source/Runtime/Engine/Classes/Components` 폴더(헤더 73개)에 있는 컴포넌트를 Rocks에 들이는 범위와 순서를 정한다. 사용자 지시(2026-11-23): 기본은 전부 가져오되 폐기된 것과 BSP는 제외한다. 필수 컴포넌트 목록(3절)과 선행 작업 목록(4절)을 따로 둔다. 1–7절(1부)은 이 폴더, 8절(2부)은 폴더 밖(Engine 모듈의 다른 폴더, Runtime의 다른 모듈), 9절(3부)은 엔진 플러그인이다.

- 근거 문서: [엔진 런타임](../EngineRuntime/README.ko.md)(이하 ER) 7절, [엔진 구현 계획](../EngineImplementationPlan.ko.md)(이하 EIP) 10·13–16·18·20절, [렌더러](../Renderer.ko.md)(이하 RN) 3·4·17절, [오디오](../AudioSystem.ko.md)(이하 AU) 3.8·3.15절, [Prefab](../Prefab.ko.md)(이하 PF) 5절, [Sparks 엔진 연동](../SparksEngineIntegration.ko.md)(이하 SE) 2.3절, [메시·애니메이션 에셋](../MeshAnimationAssets.ko.md)(이하 MA), [물리 어댑터](../PhysicsJoltAdapter.ko.md), [Cylinder](../CylinderShape.ko.md), [커브](../CurveData.ko.md), [공간 색인](../SpatialIndex.ko.md), [텍스처 스트리밍](../TextureStreaming.ko.md).
- UE 경로 약어: `EC/` = `Engine/Source/Runtime/Engine/Classes/Components/`, `EE/` = `Engine/Source/Runtime/Engine/Classes/Engine/`.
- 좌표·단위·행렬은 UE 5.8과 같다(ER 7.3절). 이 문서의 모든 Transform·bounds·감쇠 거리는 cm다.

## 목차

- [1. 범위](01-Scope.ko.md)
- [2. 분류 결과](02-Classification.ko.md)
- [3. 필수 컴포넌트](03-RequiredComponents.ko.md)
- [4. 선행 작업](04-Prerequisites.ko.md)
- [5. 묶음 순서](05-BundleOrder.ko.md)
- [6. 결정(권장안)](06-Decisions.ko.md)
- [7. 다른 문서 개정](07-OtherDocRevisions.ko.md)
- [8. 폴더 밖 컴포넌트(2부)](08-OutsideFolder.ko.md)
- [9. 플러그인 컴포넌트(3부)](09-Plugins.ko.md)
- [10. 확인하지 못한 것](10-Unverified.ko.md)
- [11. 개정 기록](11-Revisions.ko.md)
