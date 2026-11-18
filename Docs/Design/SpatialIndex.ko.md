# Rocks 공간 인덱스: 구조 선택과 Loose Octree 설계

기준일: 2026-11-19, 개정 6. 2026-11-17: 길이 단위를 UE 5.8과 같은 cm로 바꿨다(설정값 표·11절 예시·LO02·LO03).

이 문서는 엔진의 공간 분할·탐색 구조를 용도별로 무엇으로 할지 검토하고, 그 결과로 `SpatialIndex` 모듈에 둘 범용 영역 질의 구조 `LooseOctree`의 설계를 정한다. OT0–OT2(선행 SIP2·SIP4·SIP6·SIP7·SIP10, 저장 구조·삽입·삭제·박스 검색·분할·이동·루트·유지보수)는 구현했고(2026-11-18, 10절), 그 밖은 설계다. 수치는 모두 측정 전 제안값이다. 상태 표기는 **구현**(코드와 시험이 있음), **설계**(이 문서가 정한 계약, 코드 없음), **결정 필요**(14절), **후속**(범위 밖)이다.

이 문서는 고정 구현 순서(A0-B → A0-O → 리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 바꾸지 않는다. OT0–OT2는 사용자 결정(2026-11-18)으로 메인 줄과 별도인 `feature/spatial-index` 브랜치에서 먼저 진행하며, 메인 줄의 순서는 그대로 둔다(12절).

참조 기준은 다음 두 가지로 고정한다. 다른 버전의 구현을 섞지 않는다.

- UnityOctree: `https://github.com/Nition/UnityOctree` commit `bbc473571c8a0879024077a928ca658a52ecbd99`(2021-06-08, BSD 2-Clause). 약칭 BO = `Scripts/BoundsOctree.cs`, BN = `Scripts/BoundsOctreeNode.cs`.
- Unreal Engine: 5.8.4-release, commit `f45729c0b6f0`(로컬 클론 `C:\github\UnrealEngine`). 경로는 `Engine/Source/Runtime/` 기준이다. 약칭 GO = `Core/Public/Math/GenericOctree.h`.

두 참조 모두 소스를 읽어 얻은 설계 사실만 사용한다. 실행으로 확인하지 않은 해석은 "(해석)"으로 표시한다. UE 소스는 Unreal Engine EULA 적용 대상이므로 코드를 옮기지 않고 설계 사실의 대조에만 쓴다. UnityOctree는 BSD 2-Clause이며 벤치마크 비교군 B의 이식에만 쓴다(9.1절).

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 전체 전략 | 구조 하나로 통일하지 않고 소비자별로 질의 형태에 맞는 구조를 둔다. EngineImplementationPlan 10.4절의 분담을 유지한다 | 3.3절. UE 5.8.4도 용도별로 구조가 다르다 |
| 매 프레임 가시성 | 평면 SoA 경계 배열의 병렬 전수 판정(10.4절). 트리를 쓰지 않는다 | 3.2절. UE의 프러스텀 컬링 기본 경로가 이 방식이고 옥트리 경로는 기본 꺼짐이다 |
| `LooseOctree`의 자리 | `SpatialIndex` 모듈의 범용 영역 질의 부품. 소비자별 인스턴스이며 단일 월드 옥트리가 아니다 | 5절. 10.4절의 "단일 월드 octree를 두지 않는다"를 따른다 |
| 소비자 배정 | 이동 primitive–local light 상호작용과 CPU 판정 경로의 그림자 caster 수집은 `LooseOctree`(primitive 인스턴스, local light 인스턴스). 형상 기반 게임플레이 질의는 Jolt scene query | 사용자 결정(2026-11-18), EngineImplementationPlan 개정 1.13. UE 5.8.4와 같은 분담이다(3.5절) |
| 등록 방식 | 프록시 하나는 정확히 한 곳(노드 하나 또는 범위 밖 목록)에 저장한다 | 6.2절. 결과 중복 제거가 필요 없다 |
| 식별 | 세대 번호 핸들(`SpatialProxyHandle{SlotHandle}`). 논리 핸들과 물리 위치를 분리한다 | 6.3절. UE의 요소 ID는 구조 변경마다 바뀌고 낡은 ID를 검출하지 못한다 |
| 이동 | 현재 노드의 확장 경계 안이면 경계만 갱신한다. 벗어나면 조상에서 재삽입한다. 채택은 9.6절 기준으로 확정한다 | 6.5절. UnityOctree와 `TOctree2`에는 이동 API가 없다 |
| 노드 저장 | 평탄 배열, 자식 8개 인접 블록, 32비트 색인. 대안 두 가지를 측정으로 비교한다 | 6.3절 |
| 분할·병합 | 분할은 분리 level을 계산한 뒤 수행하고, 병합은 작업 수 예산의 유지보수로 지연한다 | 6.4·6.6절 |
| 동시성 | 변경 단계와 읽기 단계를 분리한다. 검색은 읽기 범위에서 얻은 reader로만 할 수 있다 | 6.9절 |
| kd-tree | 씬 질의용으로 두지 않는다 | 5.2절 |
| BVH | 물리 scene query는 물리 adapter 소유(10.4절). 정적 삼각형 메시 BVH는 후속이다 | 5.2절 |

## 2. 범위

**이 문서가 정하는 것**

- 엔진 공간 구조의 용도별 선택(3·5절). 10.4절 분담을 UE 5.8.4 소스로 다시 확인한 결과를 포함한다.
- `LooseOctree`의 책임, 공개 API, 불변 조건, 저장 구조, 갱신 정책, 동시성 계약(6절).
- 선행 작업(7절), 정확성 시험(8절), 벤치마크 계획(9절), 단계(10절).

**이 문서가 정하지 않는 것**

- 평면 SoA 가시성 판정, 계층 공간 해시, 후속 2D 격자, World Partition grid. EngineImplementationPlan 10.2·10.4절이 담당한다.
- 물리 충돌 질의와 trigger. 16절 물리 adapter가 담당한다. `LooseOctree`는 trigger event를 만들지 않고 충돌 형상 overlap을 대체하지 않는다.
- 정밀 메시 교차, Actor 소유 관계, 부착 계층, 객체 수명, 스트리밍 정책. 옥트리는 AABB 후보만 돌려준다.

## 3. 참조 구현 분석

### 3.1 UnityOctree (commit bbc4735)

| 항목 | 동작 | 근거 |
|---|---|---|
| 경계 | looseness를 [1, 2]로 자른다. 확장 변 길이 = looseness × 기본 길이. 자식 확장 경계는 부모의 기본 길이 기준이다 | BO:62, BN:445-462 |
| 배치 | 객체 AABB 중심으로 자식 하나를 고르고, 그 자식의 확장 경계에 전체가 들어갈 때만 내린다. 아니면 현재 노드에 둔다. 단일 등록이다 | BN:394-396, 509-517, 593-595 |
| 분할 | 잎에 8개가 있는 상태에서 다음 삽입 시, 자식 기본 길이가 minSize 이상이면 분할한다. 임계 8은 상수다 | BN:38, 475-487 |
| 병합 | 삭제 경로에서 (자기 + 직계 자식 객체 수) ≤ 8이고 손자가 없으면 병합한다 | BN:601-614 |
| 진동 | 분할 임계와 병합 임계가 맞닿아 있다. 9개에서 하나를 빼면 병합되고 다시 넣으면 분할된다(해석). 분할마다 노드 8개와 부속 객체를 새로 할당한다 | BN:477, 613 |
| 빈 가지 | 검색이 빈 자식도 8개 모두 방문한다 | BN:201, 230, 251 |
| 삭제 | 경계 없는 삭제는 트리 전체 DFS다. 경계를 주는 삭제는 삽입 때와 같은 경로를 만드는 경계가 필요하고 실패 시 예비 탐색이 없다 | BN:78-103, 111-116, 526-549 |
| 이동 | API가 없다. 삭제 후 재삽입뿐이다 | BO:73-203 |
| 루트 확장 | 객체 방향으로 2배씩 키운다. 옛 루트에 객체가 있으면 새 루트의 한 자식으로 끼우고, 비어 있으면 버린다. 삽입 실패가 21회 누적되면 21번째 확장 뒤에는 재시도 없이 로그만 남기고 반환값 없이 끝나며, 트리는 21단계 커진 채 남는다 | BO:75-82, 263-294 |
| 루트 축소 | 삭제 성공마다 1단계만 시도한다. 빈 트리는 축소하지 않는다. 삭제마다 재귀로 자식이 비었는지 검사한다 | BN:325-387, 402-412 |
| 검색 | 확장 경계로 가지치기하고 방문한 모든 노드의 객체를 전수 검사한다. 완전 포함 판정이 없다. ray는 자식 0–7 고정 순서다. 조기 종료는 `IsColliding`뿐이다 | BN:123-255 |
| 저장 | 노드는 class이고 노드마다 객체 List와 자식 경계 배열을 따로 할당한다. 풀이 없다 | BN:6-34, 454 |
| 검증·동시성 | 불변식 검사, 통계, 스레드 규약, 시험이 없다 | Scripts 전체 |

### 3.2 Unreal Engine 5.8.4의 씬 옥트리

**`TOctree2`의 구조**

| 항목 | 동작 | 근거 |
|---|---|---|
| 노드 | `{자식 블록 시작 인덱스 uint32, 서브트리 요소 수 uint32}` 8바이트. 경계는 저장하지 않고 순회 중 `FOctreeNodeContext`로 계산한다 | GO:397-406, 263-285 |
| 배열 | `TreeNodes`, `ParentLinks`(자식 8개 블록당 부모 1개), `TreeElements`(노드별 요소 배열) 세 배열. 자식 8개를 인접 블록으로 할당하고 블록 번호 스택으로 재사용한다. 배열은 줄지 않는다(TODO) | GO:409-411, 514-546 |
| looseness | 분모 16 고정. 자식의 확장 extent = tight × (1 + 1/16) | GO:269-270, 302-307 |
| 배치 | 가장 가까운 자식 하나만 검사하고, 안 들어가면 현재 내부 노드에 둔다. 내부 노드 요소 수에 상한이 없다 | GenericOctree.inl:55-82, GO:583-589 |
| 분할 | 잎이 `MaxElementsPerLeaf`를 넘고 extent가 최소 잎 extent보다 크면 분할하고 기존 요소를 모두 재삽입한다 | GO:548-598 |
| 병합 | 삭제 시 부모 링크로 루트까지 요소 수를 줄이고, `MinInclusiveElementsPerNode` 미만이 된 가장 위 노드로 하위 요소를 모은다. 씬·내비게이션 옥트리 기준으로 분할 임계(16 또는 256)와 병합 임계(7)가 달라 이력이 생긴다 | GO:844-897 |
| 빈 가지 | 서브트리 요소 수가 0인 가지는 검색이 건너뛴다 | GO:620-742 |
| 요소 ID | `{노드 인덱스, 요소 인덱스}`. swap-remove, 분할(재삽입), 병합 때마다 다른 요소의 ID가 바뀌어 `SetElementId` 콜백으로 통지한다. 세대 번호가 없다 | GenericOctreePublic.h:14-62, GO:563-577, 849-855, 890-894 |
| 이동 | API가 없다. 프리미티브, 광원, 내비게이션 모두 삭제 후 재삽입이다 | Renderer/Private/RendererScene.cpp:6227-6283, 3472-3477, NavigationSystem/Private/NavigationOctree.cpp:293-299 |
| 루트 | 생성 시 고정이며 확장 코드가 없다. 범위 검사도 없다. 씬은 ±1048576으로 고정한다 | GO:1043-1049, RendererScene.cpp:1163 |
| 검색 | 전부 재귀다. 박스, predicate, 첫 요소(조기 종료), 점 근처 요소, 전체 열거 질의가 있고 ray·프러스텀 전용 질의는 없다 | GO:620-742, 754-764 |
| SIMD | 경계를 center/extent 4성분으로 두어 교차를 한 번의 비교로 판정한다. 교차 자식 집합을 6비트 마스크로 구한다 | GO:41-154, GenericOctree.inl:27-53 |
| 검증 | 무결성 검증 함수가 없고 단언은 느린 빌드 전용이다. 스레드 규약 주석이 없다 | GO 전체 |

**씬에서의 실제 역할**

- 프러스텀 컬링의 기본 경로는 옥트리가 아니다. `r.Visibility.FrustumCull.UseOctree`의 기본값이 false이며(Renderer/Private/SceneVisibility.cpp:343-349), 기본 경로는 `Scene.PrimitiveBounds` 선형 배열과 뷰별 비트 배열을 32비트 워드 단위로 병렬 판정한다(같은 파일 :860-924, 4043-4050).
- 옥트리를 켠 경우에도 보조다. 노드당 2비트(교차, 일부 밖)를 채우고 프리미티브는 선형 루프에서 자기 노드의 비트만 조회한다(:730-772, 948-968).
- 씬 옥트리를 검색하는 곳은 로컬 광원–프리미티브 상호작용(Renderer/Private/LightSceneInfo.cpp:116-119, RendererScene.cpp:3480-3483)과 그림자 대상 수집(Renderer/Private/ShadowSetup.cpp:5409-5456, ShadowSetupMobile.cpp:207-215)이다. 데칼과 반사 캡처는 쓰지 않는다.
- 그림자 수집의 옥트리 사용에도 끄는 스위치가 있고, 그 설명문이 자료구조 순회의 캐시 미스를 비용으로 든다(ShadowSetup.cpp:364-369).
- 플랫폼에 따라 Nanite 메시는 씬 옥트리에 넣지 않는다(Renderer/Private/PrimitiveSceneInfo.cpp:1918-1931).
- 프리미티브 옥트리의 잎 용량 256은 그림자 수집 작업 단위와 묶인 값이다(Renderer/Public/PrimitiveSceneInfo.h:843-850).
- 등록·제거는 렌더 스레드에서 프레임당 일괄 처리한다(RendererScene.cpp:5385, 5534-5539, 6444-6446).
- 2023년 변경(commit `b523fcdaa1ef`)은 요소 버퍼를 제거 시 줄이지 않게 했다. 프리미티브 수만 개가 변동할 때 재할당 비용이 컸다는 설명이다.

### 3.3 Unreal Engine 5.8.4의 용도별 구조

| 용도 | 구조 | 갱신 | 근거 |
|---|---|---|---|
| 메인 뷰 프러스텀 컬링 | 선형 SoA + 비트 배열 | 병렬 배열의 교환과 축소 없는 제거로 조밀 유지 | Renderer/Private/SceneVisibility.cpp:860-968, RendererScene.cpp:5716-5785 |
| 광원–프리미티브, 그림자 수집 | `TOctree2` | 삭제 후 재삽입 | 3.2절 |
| Nanite·GPU Scene 인스턴스 컬링 | 계층 공간 해시(8³ 블록, 반 셀 확장) + 컬링 불가 목록 | change set 비동기, dirty 블록 | Renderer/Private/SceneCulling/SceneCulling.h:160, SceneCulling.cpp:131-151, 3086-3091 |
| 물리 브로드페이즈·씬 쿼리 | 이진 AABB 트리. 정적·동적 버킷 분리 | 잎 안 갱신, dirty 목록, 재삽입 | Experimental/Chaos/Public/Chaos/AABBTree.h:646-660, 795-796, 1940-1963, Experimental/Chaos/Private/Chaos/PBDRigidsEvolution.cpp:40-41 |
| 삼각형 메시 충돌 | 정적 BVH | 재구축 | Experimental/Chaos/Public/Chaos/TriangleMeshImplicitObject.h:46-70 |
| 내비게이션 관련 요소 색인 | `TOctree2` | 삭제 후 재삽입 | NavigationSystem/Public/NavigationOctree.h:111-141 |
| World Partition 스트리밍 | 다레벨 격자 + 셀 해시 매핑 | 쿠킹 시 구축 | Engine/Public/WorldPartition/WorldPartitionRuntimeSpatialHash.h:95-165 |
| Mass·AI·SmartObject | 2D 계층 해시 격자 + spill 목록 | 셀이 바뀔 때만 이동 | AIModule/Public/HierarchicalHashGrid2D.h:17, 325-334, 515-537 |
| 지오메트리 도구 | 동적 sparse octree(루트 셀 격자, 25% 확장, spill 목록) | 현재 셀에 들어가면 유지 | GeometryCore/Public/Spatial/SparseDynamicOctree3.h:145-162, 185-192, 249, GeometryCore/Private/Spatial/SparseDynamicOctree3.cpp:185-222 |
| 포즈 매칭 | kd-tree(nanoflann, 고차원 최근접) | 정적 | Plugins/Animation/PoseSearch/Source/Runtime/Private/KDTree.cpp:7-14 |

판정은 다음과 같다.

- "일반 씬은 Loose Octree, 그 외는 BVH·kd-tree"라는 구도는 절반만 맞다. 씬에 옥트리가 있으나 맡는 일은 드문 영역 질의이고, 매 프레임 가시성은 선형 배열이다.
- 물리는 BVH다. Engine/Source/Runtime과 Plugins에서 런타임 씬 질의용 kd-tree를 찾지 못했다.
- 가장 널리 쓰이는 것은 계층 공간 해시·격자 계열이다(인스턴스 컬링, 스트리밍, Mass·AI).
- 씬 옥트리, World Partition 격자, Chaos 가속 구조는 역할과 수명이 서로 다르다. 이 문서의 벤치마크는 이들을 같은 자료구조로 비교하지 않는다.

**10.4절과의 대조**

- 10.4절의 분담(이동 대상은 평면 SoA, 정적 인스턴스는 계층 해시, 충돌은 물리 scene query)은 위 표와 일치한다.
- 33.10절이 미확인으로 남긴 네 항목(SceneCulling cell의 loose 여부, level 산정식, 도입 버전, World Partition의 level 선택식) 가운데 앞의 두 항목이 5.8.4 소스에서 확인된다.
  - level은 max(FirstLevel, floorLog2(uint32(2 × 반지름)))이고 셀 크기는 2^(level+1)이다(Engine/Public/Rendering/RenderingSpatialHash.h:86-107, 124-129, Renderer/Private/SceneCulling/HierarchicalSpatialHashGrid.h:357-365). `FirstLevel`은 최소 셀 크기 설정(기본 4096)에서 나온다(Renderer/Private/SceneCulling/SceneCulling.cpp:131-138).
  - 셀 경계는 반 셀 확장한다(HierarchicalSpatialHashGrid.h:393-406).
  - 10.4절 규약 2·3과 같은 방향이다. 도입 버전과 WP level 선택식은 여전히 미확인이다(15절).
- 10.4절 본문의 UE 근거는 5.4 기준의 공개 문서와 2차 출처다. 5.8 기준 재확인은 14절 결정 필요 6이다.
- 10.4절은 이동 primitive–local light 상호작용과 reflection capture·probe 선택을 "같은 해시 코드의 별도 light 인스턴스"로 정했다(EngineImplementationPlan 10.4 광원·decal·capture). UE 5.8.4는 광원 상호작용에 `TOctree2`를 쓴다. 개정 1.13(2026-11-18 사용자 결정)에서 Rocks도 옥트리로 정했다(5.1절).

### 3.5 Unreal Engine 5.8.4의 게임플레이 공간 질의

| 질의 | 수단 | 근거 |
|---|---|---|
| trace·sweep·overlap | 물리 인터페이스(`FPhysicsInterface::RaycastMulti`, `GeomOverlapMulti` 등) | Engine/Private/Collision/WorldCollision.cpp:144-205 |
| AI 시야 | 물리 trace(동기·비동기) | AIModule/Private/Perception/AISense_Sight.cpp:556, 571 |
| EQS 클래스별 액터 수집 | 공간 구조 없이 액터 전체 순회 후 반경으로 거름 | AIModule/Private/EnvironmentQuery/Generators/EnvQueryGenerator_ActorsOfClass.cpp:57, 71 |
| 네트워크 관련성 | 2D 격자(Replication Graph, Iris grid filter) | Plugins/Runtime/ReplicationGraph/Source/Public/ReplicationGraph.h:579, Net/Iris/Public/Iris/ReplicationSystem/Filtering/NetObjectGridFilter.h:88 |
| Mass·SmartObject | 2D 계층 해시 격자 | AIModule/Public/HierarchicalHashGrid2D.h |

형상 질의는 물리로, 대량·비충돌 관심 판정은 가벼운 격자로 나눈다. 충돌하지 않는 대상을 물리 질의에 올리는 방식(query 전용 collision)은 일반 지식이며 이번에 소스로 확인하지 않았다.

### 3.4 채택 판단 비교표

| 항목 | UnityOctree | UE `TOctree2` | 참조의 장단점 | Rocks 채택안 | 변경 이유 | 검증 지표 |
|---|---|---|---|---|---|---|
| 경계 | 기본 셀과 확장 경계 분리, looseness 1–2 | looseness 1/16 고정 | 분리 정의는 포함 관계가 자동 성립(장점). UE 값은 경계에 걸친 요소를 상위 노드에 쌓는다(단점) | 분리 유지. looseness k는 [1, 2]의 1/64 단위 설정값(초기 2) | 값은 측정으로 고른다 | k별 방문 노드 수, 프록시 검사 수, 내부 노드 저장 비율 |
| 등록 | 단일 | 단일 | 결과 중복 제거 불필요(장점) | 단일 | 변경 없음 | 결과 중복 0건 |
| 메모리 배치 | 노드별 개별 할당 | 평탄 배열, 8개 인접 블록, 노드별 요소 배열 | UE는 포인터 없는 구조(장점), 노드별 동적 배열 할당(단점). Unity는 할당이 많다(단점) | 평탄 배열과 8개 인접 블록. 요소는 색인 풀의 고정 크기 청크 | 노드별 동적 배열의 할당을 없앤다 | 할당 횟수, 예약 메모리, 검색 시간 |
| 식별·삭제 | 객체 동등성 탐색 | 불안정 ID + 콜백 | UE 삭제는 O(깊이)(장점), 외부 ID 저장소와 콜백이 필요하고 낡은 ID를 검출하지 못한다(단점) | 세대 번호 핸들 → 프록시 레코드 → (노드, 슬롯). 삭제는 직접 수행 | 콜백을 없애고 낡은 핸들을 검출한다 | 삭제 시간, 핸들 조회 간접 비용 |
| 이동 | 없음 | 없음 | 둘 다 삭제 후 재삽입(단점) | 확장 경계 안이면 제자리 갱신, 아니면 조상에서 재삽입 | 두 참조 모두의 약점이다 | 재삽입 비율, 갱신 시간 분포 |
| 분할·병합 | 임계 맞닿음, 즉시 | 임계 분리, 즉시 | UE는 이력으로 진동 방지(장점). 둘 다 분할 효과를 보지 않는다(단점) | 분할은 분리 level 계산 후, 병합은 지연. `mergeThreshold ≤ splitThreshold / 2` 강제 | 진동과 효과 없는 분할을 막는다 | 분할·병합 횟수, 진동 시험 |
| 빈 가지 | 빈 자식도 방문 | 요소 수 0 가지 생략 | UE 방식이 순회를 줄인다(장점) | 서브트리 수 + 자식 점유 마스크 | 점유 자식만 순회한다 | 방문 노드 수 |
| 과밀 | minSize 잎에 무제한 누적 | 최대 깊이 잎과 내부 노드에 무제한 누적 | 객체를 버리지 않는다(장점). 같은 분석을 반복한다(단점) | 분리 불가능하면 큰 잎(청크 연결)과 보류 상태 | 반복 평가와 무의미한 체인을 막는다 | 과밀 노드 수, 과밀 분포 검색 시간 |
| 검색 API | 박스·ray·프러스텀, 결과 List | 박스·predicate·첫 요소, 콜백 | UE 콜백은 할당이 없다(장점). 둘 다 완전 포함 생략이 없다(단점) | 박스·구·프러스텀·ray·최근접, visitor와 호출자 버퍼, 존재·개수 질의 분리 | 완전 포함 생략과 ray 순서를 더한다 | 검색 시간, 방문 수 |
| 동시성 | 없음 | 규약 없음(실사용은 렌더 스레드 단독 쓰기) | 문서화된 계약이 없다(단점) | 변경 단계·읽기 단계 분리, reader로만 검색, 위반 검출 | 계약을 API로 강제한다 | 병렬 검색 확장 효율, 위반 검출 시험 |
| 관측·검증 | 없음 | 통계 출력만 | 검증 수단이 없다(단점) | 통계, 불변 조건 검증, 질의별 계수 | 두 참조에 없다 | 8절 시험 |
| 루트 | 확장·축소, 조용한 실패 | 고정 | Unity는 크기 제한이 없다(장점), 실패가 조용하다(단점). UE는 범위 밖 처리가 없다(단점) | 확장 전 수용 판정, 한도 밖은 범위 밖 목록, 축소는 유지보수 | 두 참조의 결함을 피한다 | 루트 변경 횟수와 시간 |

UE의 다른 구조에서 가져오는 것은 다음과 같다.

- 구조에 넣기 부적합한 객체의 별도 목록. Chaos `GlobalPayloads`는 extent가 한도를 넘거나 경계가 없는 객체, `FSparseDynamicOctree3`의 spill은 루트 셀에 안 들어가는 객체, SceneCulling의 컬링 불가 목록은 최대 셀보다 크거나 정밀도 범위를 벗어난 객체를 담는다.
- 이동 시 "현재 경계에 아직 들어가면 구조를 바꾸지 않음"(`FSparseDynamicOctree3`의 재삽입 판정, Chaos 잎 안 갱신).
- 요소 버퍼를 제거 시 줄이지 않음(commit `b523fcdaa1ef`).
- 시간 예산 확인을 N개마다만 수행(Chaos 타임슬라이스 구축).

## 4. 용어

| 용어 | 뜻 |
|---|---|
| 프록시 | 옥트리에 등록하는 경량 레코드. AABB, 필터 마스크, 사용자 값으로 이루어진다 |
| 기본 셀 | 노드의 겹치지 않는 정육면체 영역 |
| 확장 경계 | 기본 셀을 중심 기준으로 k배 한 정육면체. 저장 가능 여부와 검색 가지치기에 쓴다 |
| 직접 저장 | 프록시가 그 노드의 요소 청크에 들어 있음 |
| 범위 밖 목록 | 루트 확장 한도 안에 들어가지 않는 프록시를 담는 선형 목록. 모든 검색이 항상 판정한다 |
| 유지보수 | 정확성과 무관하게 검색 품질과 메모리를 개선하는 지연 작업(병합, 빈 블록 회수, 하강 재배치, 분할 재평가, 루트 축소) |
| 변경 단계, 읽기 단계 | 6.9절. 한 시점에 둘 중 하나만 진행한다 |

## 5. 구조 선택 전략

### 5.1 소비자와 구조

전략은 UE 5.8.4와 같은 방식이다(사용자 결정 2026-11-18, EngineImplementationPlan 개정 1.13).

1. 형상 기반 게임플레이 질의(trace·sweep·overlap)는 물리 adapter의 Jolt scene query를 쓴다.
2. 이동 primitive–local light 상호작용과 CPU 판정 경로의 그림자 caster 수집은 `LooseOctree`를 쓴다.
3. 매 프레임 view 가시성은 평면 SoA, 정적·대량 instance는 계층 공간 해시를 쓴다.
4. 네트워크 관심·대량 군중의 관심 판정용 2D 격자는 필요해질 때 도입한다.

| 소비자 | 구조 | 상태 |
|---|---|---|
| 이동 대상의 뷰 가시성 | 평면 SoA(10.4절) | 변경 없음 |
| 정적 mesh·instanced·foliage의 가시성 | 계층 공간 해시(10.4절) | 변경 없음 |
| 그림자 caster 수집, CPU 판정 경로 | `LooseOctree` primitive 인스턴스를 caster volume으로 질의. 노드 묶음 단위로 worker에 분배(6.8절) | 설계 |
| 그림자 caster 수집, GPU-driven 경로 | GPU 판정(10.4절). 옥트리를 쓰지 않는다 | 변경 없음 |
| 이동 primitive–local light 상호작용(그림자 cache 무효화) | `LooseOctree` 두 인스턴스. 광원이 추가·이동하면 primitive 인스턴스를 광원 범위로, primitive가 추가·이동하면 local light 인스턴스를 primitive 범위로 질의 | 설계 |
| forward·mobile의 reflection capture·probe 선택 | 평면 SoA(UE 5.8.4도 옥트리를 쓰지 않음) | 변경 |
| 에디터 비가시 대상 선택, headless | EditorBackend 해시 + double 정밀 판정(10.4절). `LooseOctree`는 비교 후보 | 결정 필요 2 |
| 충돌 trace·sweep·overlap·trigger, 형상 기반 게임플레이 질의 | Jolt scene query(10.4절, 16절). 비충돌 대상은 query 전용 body | 설계(물리 adapter 소관) |
| 네트워크 관심, 대량 군중 | 후속 2D 격자 | 후속 |
| 스트리밍, 내비게이션 | 10.2절 grid, 19절 tile index | 변경 없음 |
| 도구·소규모 동적 집합 | `LooseOctree` | 설계 |

근거는 다음과 같다.

- UE 5.8.4가 같은 분담을 쓴다(3.2·3.5절).
- 옥트리는 크기 편차가 큰 대상을 한 구조에 담고, 큰 질의에서 완전 포함된 가지를 통째로 처리한다. 광원 영향 범위와 caster volume처럼 질의 크기가 대상 크기와 크게 다른 경우에 맞는다.
- Rocks는 CPU 판정 장치(mobile·D3D11·OpenGL ES)를 정식 경로로 둔다. GPU-driven 경로가 옥트리를 쓰지 않아도 CPU 경로에는 그림자 caster 후보 수집 구조가 필요하다.
- 광원 수가 적으면 평면 전수 판정이 더 빠를 수 있다. 옥트리가 이기는 규모는 9.6절 측정으로 기록한다.

33.10절의 결론은 그대로 적용된다. loose 확장만으로는 직선 이동의 노드 변경 빈도가 줄지 않는다. 제자리 갱신(6.5절)이 없애는 것은 경계 부근 왕복과, 상위 노드에 저장된 대상의 여유 범위 안 이동이다. 10.4절이 같은 원리의 위치 hysteresis를 SI05 측정으로 채택하는 것처럼, 제자리 갱신도 9.6절 기준으로 확정한다.

**렌더 소비자가 쓸 경우의 전제.** `LooseOctree`는 받은 AABB만 다룬다.

- 렌더 소비자는 이전·현재 pose bounds의 합집합에 최대 WPO 변위를 더한 AABB를 넘긴다(10.4 가시성 보수성).
- bounds가 소속 노드 밖으로 나간 프록시의 재배치는 `TryUpdateBounds` 호출 안에서 끝난다. 다음 읽기 단계 전에 재배치가 완료된다(10.4 가시성 보수성의 "같은 commit 안에서 재배치").
- 렌더 전용 인스턴스의 소유·갱신 코드는 Renderer에 둔다. `LooseOctree` 자체는 `SpatialIndex`에 있으므로 DedicatedServer에 링크될 수 있으며, SI10의 "렌더 인덱스 미포함"은 렌더 인스턴스 코드의 배치로 지킨다.

### 5.2 다른 구조의 담당

| 구조 | 판단 | 담당 |
|---|---|---|
| 동적 AABB 트리(BVH) | 자체 구현하지 않는다. 물리 middleware의 scene query가 제공한다 | EngineImplementationPlan 16절 |
| 정적 삼각형 메시 BVH | 후속. 물리 cook과 에디터 headless의 double 정밀 ray/triangle 판정(10.4 에디터 선택)에서 필요해질 때 정한다 | 16절, 에디터 문서 |
| 계층 공간 해시 | 10.4절 규약대로 둔다 | 10.4절 |
| kd-tree | 씬 질의용으로 두지 않는다. UE 런타임 씬 질의에 사례가 없고, 균형 kd-tree는 대상이 움직이면 재구축 또는 재균형이 필요하다(일반적 성질). 고차원 점 집합의 최근접 질의(예: 애니메이션 포즈 매칭)가 필요한 기능이 생기면 그 기능의 문서에서 정한다 | 해당 기능 문서 |
| 게임플레이 공간 질의 | 자체 인덱스를 두지 않는다. Jolt scene query와 결과 정렬 계층을 쓴다 | EngineImplementationPlan 10.4·16절 |
| 2D 격자 | 네트워크 관심·대량 군중용으로 필요해질 때 도입한다. 물리 query 전용 body 비용이 SI09 기준을 넘는 용도가 대상이다 | EngineImplementationPlan 10.4절 |
| quadtree | 두지 않는다. 2D 관심 판정은 후속 2D 격자가 담당한다 | 10.4절 |

## 6. LooseOctree 설계

### 6.1 책임과 공개 API

배치는 엔진 계층 `SpatialIndex` 모듈(`Engine/Source/Runtime/SpatialIndex`)이다. Core는 기반 계층으로 가볍게 두므로 엔진 기능인 공간 질의 구조는 엔진에 두고, 수학 타입·판정(`ConvexVolume` 등)과 범용 컨테이너(`InlineArray`·`IndexPool`)는 Core에 둔다(2026-11-18 사용자 결정). 의존은 Core뿐이다(EngineImplementationPlan 3.1절: World·RHI·Object 의존 금지). 네임스페이스는 `rocks::spatial`이다. 인스턴스는 소비자(World, RenderScene, 도구)가 소유하며 전역 singleton을 두지 않는다. 수학 타입은 `rocks`의 `Box<T>`, `Sphere<T>`, `Ray<T>`, `Vec3d`를 쓴다.

```cpp
namespace rocks::spatial {

struct SpatialProxyHandle {
    SlotHandle slot;

    [[nodiscard]] constexpr bool IsNull() const noexcept;
    friend constexpr bool operator==(SpatialProxyHandle, SpatialProxyHandle) noexcept = default;
};

struct SpatialProxyDesc {
    Box<double> bounds;  // 월드 AABB. Box::IsValid()가 참이어야 한다
    uint32 filterMask;   // 0이면 어떤 질의에도 걸리지 않는다
    uint64 userValue;    // 호출자의 식별 값(EntityHandle 등). 옥트리는 해석하지 않는다
};

struct SpatialProxyHit {
    SpatialProxyHandle handle;
    uint64 userValue;
    uint32 filterMask;
};

enum class QueryControl : uint8 { Continue, Stop };

struct QueryOutcome {
    uint32 visited;  // visitor 호출 수
    bool stopped;    // visitor가 Stop을 돌려줌
};

struct BufferQueryOutcome {
    uint32 written;  // out에 쓴 수
    uint64 total;    // 전체 후보 수
    bool truncated;  // total > out.Size()
};

struct RayHitResult {
    bool hit;
    double distance;  // hit일 때만 의미가 있다
};

struct NearestHit {
    bool found;
    SpatialProxyHit proxy;
    double distance;
};

class LooseOctree {
public:
    [[nodiscard]] static Result<LooseOctree> TryCreate(const LooseOctreeConfig& config,
                                                       Allocator& allocator) noexcept;

    LooseOctree(LooseOctree&& other) noexcept;
    LooseOctree& operator=(LooseOctree&& other) noexcept;
    LooseOctree(const LooseOctree&) = delete;
    LooseOctree& operator=(const LooseOctree&) = delete;
    ~LooseOctree() noexcept;

    // 변경 단계 전용
    [[nodiscard]] Result<SpatialProxyHandle> TryAdd(const SpatialProxyDesc& desc) noexcept;
    [[nodiscard]] Result<void> TryRemove(SpatialProxyHandle handle) noexcept;
    [[nodiscard]] Result<void> TryUpdateBounds(SpatialProxyHandle handle,
                                               const Box<double>& bounds) noexcept;
    [[nodiscard]] Result<void> TrySetFilter(SpatialProxyHandle handle, uint32 filterMask) noexcept;
    [[nodiscard]] Result<void> TrySetUserValue(SpatialProxyHandle handle, uint64 userValue) noexcept;
    [[nodiscard]] Result<void> TryReserve(uint32 proxyCount) noexcept;
    MaintenanceReport RunMaintenance(const MaintenanceBudget& budget) noexcept;
    void Clear() noexcept;
};

// 읽기 단계: SpatialReadScope가 살아 있는 동안만 reader를 얻는다(6.9절)
class LooseOctreeReader {
public:
    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome> QueryBox(const Box<double>& box, uint32 filterMask,
                                                Visitor&& visitor,
                                                QueryStats* stats = nullptr) const noexcept;
    // QuerySphere, QueryFrustum(ConvexVolume), QueryRay(ray, maxDistance)도 같은 형태다

    [[nodiscard]] Result<BufferQueryOutcome> QueryBoxInto(const Box<double>& box, uint32 filterMask,
                                                          Span<SpatialProxyHit> out) const noexcept;
    [[nodiscard]] Result<bool> AnyInBox(const Box<double>& box, uint32 filterMask) const noexcept;
    [[nodiscard]] Result<uint64> CountInBox(const Box<double>& box,
                                            uint32 filterMask) const noexcept;
    // Into·Any·Count는 구·프러스텀·ray에도 같은 이름 규칙으로 둔다

    template <typename HitTest>
    [[nodiscard]] Result<NearestHit> QueryRayNearest(const Ray<double>& ray, double maxDistance,
                                                     uint32 filterMask, HitTest&& hitTest,
                                                     QueryStats* stats = nullptr) const noexcept;

    [[nodiscard]] bool IsValid(SpatialProxyHandle handle) const noexcept;
    [[nodiscard]] Result<SpatialProxyDesc> TryGet(SpatialProxyHandle handle) const noexcept;
    [[nodiscard]] uint32 Count() const noexcept;
    [[nodiscard]] OctreeStats Stats() const noexcept;
    [[nodiscard]] Result<void> Validate() const noexcept;
    template <typename NodeVisitor> void VisitNodes(NodeVisitor&& visitor) const noexcept;

    // 노드 묶음 수집: 그림자 caster 수집처럼 결과가 많은 질의를 worker에 나눌 때 쓴다
    [[nodiscard]] Result<BufferQueryOutcome> CollectNodeBatches(const ConvexVolume& volume,
                                                                Span<SpatialNodeBatch> out) const noexcept;
    template <typename Visitor>
    [[nodiscard]] Result<QueryOutcome> QueryBatch(const SpatialNodeBatch& batch,
                                                  const ConvexVolume& volume, uint32 filterMask,
                                                  Visitor&& visitor) const noexcept;
};

} // namespace rocks::spatial
```

계약은 다음과 같다.

- **후보와 실제 교차의 구분.** 모든 질의는 프록시 AABB와 질의 형상의 교차 후보를 돌려준다. 후보 집합은 정확한 AABB 교차 집합을 포함한다(누락 없음). 6.8절에 정한 한계 안에서 초과 후보가 있을 수 있다. 정밀 형상 교차는 호출자가 한다.
- **visitor.** `visitor(const SpatialProxyHit&)`는 `QueryControl`을 돌려준다. `Stop`이면 순회를 즉시 끝내고 `stopped`를 참으로 한다. `Function`은 호출마다 할당하므로 쓰지 않고 템플릿 인자로 받는다.
- **호출자 버퍼.** `Into` 질의는 `out`에 들어가는 만큼 쓰고 끝까지 세어 `total`을 돌려준다. 결과를 조용히 버리지 않는다.
- **존재와 개수.** `Any*`는 첫 후보에서 끝난다. `Count*`는 결과를 내지 않고 센다. 결과 수 제한은 visitor의 `Stop`으로 한다. 옥트리가 결과를 임의로 자르지 않는다.
- **필터.** `(proxy.filterMask & query.filterMask) != 0`인 프록시만 후보가 된다. 필터는 AABB 판정보다 먼저 적용한다. 질의 mask 0은 아무것도 돌려주지 않으며 오류가 아니다.
- **질의 입력 오류.** 질의 형상이 유효하지 않으면(`Box::IsValid` 거짓, `Ray::IsValid` 거짓, `maxDistance`가 음수 또는 NaN, 평면이 없거나 유효하지 않은 `ConvexVolume`) 아무것도 방문하지 않고 `InvalidArgument`를 돌려준다. "결과 없음"과 구분된다. `maxDistance`는 +Inf를 허용한다(Intersection.h의 Ray 계약).
- **결과 순서.** 같은 초기 상태에 같은 연산열(작업 수 예산의 `RunMaintenance` 포함)을 적용하면 같은 결과 순서가 나온다. 시간 예산 유지보수를 쓰면 구조가 실행마다 달라질 수 있으므로 순서를 보장하지 않는다. 복제본 사이의 결정적 순서가 필요한 소비자는 작업 수 예산만 쓰고, 그래도 `userValue`의 stable ID로 정렬한다(10.4절의 결과 정렬 규약).
- **ray 결과.** `QueryRay`의 결과는 거리순이 아니다. 노드 방문 순서만 가까운 쪽 우선이다(6.8절).
- **변경 오류.** 빈 핸들은 `InvalidArgument`, 낡은 핸들은 `StaleHandle`, 유효하지 않은 AABB(NaN, 무한대, min > max)는 `InvalidArgument`, 프록시 수 한도 초과는 `CapacityExceeded`, 색인 폭 초과는 `Overflow`, 할당 실패는 `OutOfMemory`다.
- **실패 시 상태.** 실패한 변경 호출은 논리 상태(프록시 집합, 각 프록시의 AABB·필터·사용자 값·핸들)를 바꾸지 않는다. 구조(루트 level, 분할 여부)는 달라질 수 있으며 그 경우에도 불변 조건은 모두 성립한다. 구체 절차는 6.4·6.5절이다.
- **이동과 수명.** 옥트리는 복사할 수 없고 이동할 수 있다. 이동 뒤 기존 핸들은 이동 대상에서 유효하다. 이동된 원본은 빈 상태이며 새 핸들을 세대 1부터 발급하므로, 원본에 옛 핸들을 쓰면 검출되지 않는다(`SlotArray` 규칙과 같다). 다른 인스턴스가 발급한 핸들도 검출하지 않는다. 읽기 범위가 열려 있는 동안 이동하거나 파괴하면 `platform::Fatal`이다. allocator는 옥트리보다 오래 살아야 한다.
- **`Clear`.** 모든 프록시를 지우고 루트를 초기 상태로 되돌린다. 핸들 세대 번호는 유지되므로 지우기 전 핸들은 `StaleHandle`이 된다. 예약 용량은 유지한다.
- **노드 묶음.** `CollectNodeBatches`는 질의 형상과 교차하는 노드를 직접 저장 프록시 단위로 묶어 `SpatialNodeBatch{노드 색인, 프록시 수, 완전 포함 여부, 형상 종류, 트리, 변경 세대}` 목록을 돌려준다(박스·구 형태도 같은 이름 규칙으로 둔다). 범위 밖 목록은 마지막 묶음 하나다. 각 묶음은 서로 다른 프록시를 담으므로 여러 worker가 `QueryBatch`로 나눠 처리해도 결과 합집합이 단일 질의와 같고 중복이 없다. 완전 포함 묶음은 프록시 공간 판정을 생략한다. UE 5.8.4 그림자 수집의 노드 단위 작업 분배(ShadowSetup.cpp:4997-5010, 5416)에 대응한다. 묶음은 모은 뒤 트리가 변경되지 않은 동안만 유효하다. 변경 세대가 다르면 `StaleHandle`, 다른 트리나 다른 종류의 형상이면 `InvalidArgument`다. 변경 세대는 이동·대입과 같은 주소에 다시 만든 트리에서도 겹치지 않아야 한다(프로세스 전역 단조 계수). 같은 종류의 다른 형상으로 `QueryBatch`를 부르는 것은 검출하지 않는 호출자 계약 위반이다.
- **일괄 API.** 프레임당 일괄 적용(UE `FScene`의 방식)은 소비자가 변경을 모아 변경 단계에서 차례로 호출하는 것으로 한다. 선확보를 한 번에 하는 일괄 추가·갱신 API는 9절 측정에서 호출당 비용이 문제로 확인되면 추가한다(후속).
- **원점 이동.** 좌표는 절대 월드 double이며 origin epoch 변경에 저장 값을 재계산하지 않는다(10.4 규약 7과 같다). 결과의 WorldId·tick·origin epoch 정합은 소유자 책임이다.

### 6.2 경계와 불변 조건

**격자.** 모든 노드의 기본 셀은 `anchor`와 `minCellSize`가 정하는 격자에 정렬한다.

- level ℓ의 기본 셀 한 변은 L_ℓ = minCellSize · 2^ℓ이다.
- 노드는 (level, 정수 원점 o)로 표현하며 기본 셀은 각 축 [anchor + o · minCellSize, anchor + (o + 2^ℓ) · minCellSize]다.
- 초기 루트는 level R₀ = `initialRootLevel`(1 이상), 정수 원점 o₀ = −2^(R₀−1)이다. 즉 `anchor`를 중심으로 한다.
- `minCellSize`는 2의 거듭제곱이고 `anchor`는 `minCellSize`의 정수배다. 따라서 노드 경계와 중심(level 0에서 반정수)의 double 계산은 절대값이 2^52 · minCellSize 미만인 범위에서 정확하다. 루트 확장 뒤에도 기존 노드의 월드 경계는 비트 단위로 같다.

**확장 경계.** 기본 셀의 반 변이 h이면 확장 경계의 반 변은 k·h이고 중심은 같다.

- k는 [1, 2]에서 1/64 단위로 고른다(1, 65/64, …, 2). 2진 유리수이므로 k·h가 정확하게 계산된다. 삽입, 갱신, 검색, `Validate()`는 모두 같은 식 하나로 확장 경계를 계산한다.
- 자식의 확장 경계는 부모 중심에서 (1 + k)·h/2까지 닿고, 이는 k ≥ 1일 때 k·h 이하다. 따라서 자식의 확장 경계는 부모의 확장 경계에 포함된다.

**배치 규칙.** 프록시 AABB의 중심 c = min/2 + max/2(합의 overflow를 피하는 식)로 자식 후보 하나를 고른다. 축마다 c가 노드 중심 이상이면 양의 쪽이다(세 축 모두 같은 규칙). AABB 전체가 그 자식의 확장 경계에 들어갈 때만 내려보내고, 아니면 현재 노드에 직접 저장한다. 포함 판정은 닫힌 구간이다.

**수용 보장.** 노드의 반 변을 h라 하자. 중심이 선택한 자식의 기본 셀 안에 있고 AABB의 최대 반 변이 (k − 1)·h/2 이하이면 그 자식에 반드시 들어간다. k = 2이면 크기가 자식 기본 셀 한 변 이하인 프록시는 반드시 내려간다. k = 1이면 보장이 없다. 이 보장의 전제(중심이 자식 기본 셀 안)는 루트 확장 조건(6.5절)이 중심을 루트 기본 셀 안에 두므로 모든 level에서 성립한다.

**내부 노드의 직접 저장.** 잎이 아닌 노드에 직접 저장된 프록시는 삽입 시점에 자식에 들어가지 않았거나, 이후 제자리 갱신으로 들어갈 수 있게 된 것이다. 앞의 경우는 분할 대상이 아니고, 뒤의 경우는 하강 재배치 대기로 처리한다(6.6절).

**불변 조건.** `Validate()`가 모두 검사한다.

1. 노드에 직접 저장된 프록시의 AABB는 그 노드의 확장 경계에 포함된다.
2. 자식의 확장 경계는 부모의 확장 경계에 포함된다.
3. 프록시 레코드의 (노드, 슬롯)은 실제 저장 위치와 일치하고, 그 위치의 슬롯 색인은 해당 레코드를 가리킨다.
4. 살아 있는 프록시는 정확히 한 곳(노드 하나 또는 범위 밖 목록)에 있다.
5. 노드의 서브트리 프록시 수는 직접 저장 수와 자식들의 서브트리 수의 합이다.
6. 자식 점유 마스크의 비트 i는 자식 i의 서브트리 프록시 수가 0보다 큰 것과 같다.
7. 저장된 float 경계 min_f는 double로 계산한 (min − 노드 중심)보다 작고, max_f는 (max − 노드 중심)보다 크다(6.3절의 외향 보정).
8. 잎이 아닌 노드는 자식 블록을 갖고, 자식 블록의 부모 링크는 그 노드를 가리킨다. 노드 관리 데이터의 (level, 정수 원점)은 부모에서 유도한 값과 같다.
9. 유지보수 대기열의 항목은 살아 있는 노드를 가리키고 노드당 한 번만 들어 있다.

불변 조건 1은 "더 깊은 노드에 들어갈 수 있는데 상위에 있음"을 금지하지 않는다. 그것은 품질 문제이며 정확성 문제가 아니다(6.5·6.6절).

**설정값**

길이는 cm다(UE 5.8과 같은 월드 단위 — [전체 구현 계획](EngineImplementationPlan.ko.md) 2.2절). `minCellSize`의 초기 제안은 m 기준 1 m였으나 100 cm는 2의 거듭제곱이 아니어서 가장 가까운 128 cm(2^7 cm)로 바꿨고, 루트 크기는 같은 level 수에서 1.28배가 된다.

| 설정 | 범위 | 초기 제안 | 의미 |
|---|---|---|---|
| `anchor` | `minCellSize`의 정수배 | 원점 | 격자 원점. 초기 루트의 중심 |
| `minCellSize` | 2의 거듭제곱 | 128 cm | 분할 하한. 자식 기본 셀이 이보다 작아지면 분할하지 않는다 |
| `initialRootLevel` | 1 – `maxRootLevel` | 10 (131,072 cm, 약 1.31 km) | 초기 루트 크기이자 축소 하한 |
| `maxRootLevel` | ≤ 31 | 24 (2^31 cm, 약 21,475 km) | 루트 확장 한도. 최대 깊이는 별도 설정 없이 `maxRootLevel + 1`이다 |
| `looseness` | [1, 2], 1/64 단위 | 2 | 확장 경계의 배율. 크면 프록시가 더 깊이 내려가고 검색 시 겹침이 커진다 |
| `splitThreshold` | ≥ 4 | 16 | 잎의 분할 평가 시작 수. 수용 한도가 아니다 |
| `mergeThreshold` | ≤ `splitThreshold / 2`(정수 나눗셈) | 6 | 병합 후보 기준 |
| `maxProxies` | ≤ `SlotArray` 한도 | 2^20 | 프록시 수 상한 |
| `maxProxyGeneration` | `SlotArray` 규칙 | `SlotArray` 기본값 | 핸들 세대 상한. 시험에서 retire를 검사하려고 낮춘다 |
| `parentWarnCount` | ≥ 1 | 1024 | 한 노드의 직접 저장 수가 이 값을 넘으면 진단 이벤트를 낸다 |
| `outOfRangeWarnCount` | ≥ 1 | 64 | 범위 밖 목록이 이 값을 넘으면 진단 이벤트를 낸다 |
| `pendingLimitFloor` | ≥ 1 | 64 | 유지보수 대기량 강제 처리 한도의 하한(6.6절) |

`chunkCapacity`(요소 청크의 프록시 수)는 컴파일 시점 상수(초기 16)다. 런타임 값이면 청크 크기가 가변이 되어 색인 풀의 원소 크기를 고정할 수 없다. 측정에서 다른 값이 필요하면 템플릿 인자로 바꾼다.

`TryCreate`는 다음을 검사하고 위반하면 `InvalidArgument`를 돌려준다.

- 위 표의 범위.
- `|anchor| + 2^(maxRootLevel+1) · minCellSize < 2^52 · minCellSize`(격자 계산의 정확 범위).
- `k · 2^maxRootLevel · minCellSize`가 float 정규 수 범위 안(저장 float의 overflow 방지). 하한은 `minCellSize`가 float 정규 수 최소값의 2^24배 이상.
- 검색 스택의 정적 용량이 `maxRootLevel`에서 유도한 필요량 이상(6.8절).

**입력 검증.** `TryAdd`와 `TryUpdateBounds`는 `Box::IsValid()`로 검증하고, 거짓이면 `InvalidArgument`로 거부한다(NaN, 무한대, min > max). 크기 0(점, 면)은 유효하다. 루트 확장 한도 안에 들어가지 않는 위치나 크기는 오류가 아니며 범위 밖 목록에 등록하고 성공을 돌려준다. 이때 통계와 진단 이벤트로 알린다.

10.4절 규약 1은 비유한 위치를 격리 목록에 넣어 항상 판정하도록 정했다. `LooseOctree`는 비유한 값을 거부하므로, 가시성 보수성이 필요한 렌더 소비자는 거부된 대상을 자기 격리 목록에서 판정해야 한다. 이 차이는 14절 결정 필요 4다.

### 6.3 저장 구조

**프록시 레코드(관리용).** `SlotArray<ProxyRecord>`에 둔다. 핸들의 세대 번호 규칙은 `SlotArray`를 그대로 따른다(1부터 시작, `maxProxyGeneration`에서 retire). 레코드는 정확한 double AABB(48바이트), 노드 색인, 슬롯, 필터, 사용자 값을 가진다. 범위 밖 목록에 있는 프록시는 노드 색인에 영역 상수 `OutOfRangeNode`를 둔다.

swap-remove 뒤 옮겨진 프록시의 레코드를 슬롯 색인으로 찾아야 하므로 `SlotArray`에 색인 접근과 색인에서 핸들을 얻는 API가 필요하다. 현재 공개 API는 핸들만 받는다(선행 SIP6).

**요소 청크(검색용).** 노드의 직접 저장 프록시는 고정 크기 청크의 연결 목록에 둔다. 청크 하나는 SoA로 다음을 가진다.

- float 경계 6성분 × `chunkCapacity`. 소속 노드 중심 기준 상대 좌표다.
- 필터 마스크, 슬롯 색인, 세대 번호, 사용자 값 × `chunkCapacity`.
- 사용 수, 다음 청크 색인.

프록시당 검색용 데이터는 44바이트다(경계 24, 필터 4, 핸들 8, 사용자 값 8). 박스 질의가 결과를 낼 때 레코드 배열을 읽지 않으려고 핸들과 사용자 값을 청크에 둔다(구·프러스텀·ray는 6.8절대로 레코드의 double AABB를 읽는다). 대안(32바이트, 결과마다 레코드를 한 번 읽음)은 9.4절에서 비교한다. 청크 안과 청크 사이 모두 조밀하게 유지한다(마지막 청크만 덜 찰 수 있다). 큰 잎과 과밀 노드는 청크를 더 잇는 것으로 수용한다.

**float 외향 변환.** 노드 중심을 m이라 할 때 min_f는 double 뺄셈 (min − m)의 결과를 float로 내림 변환한 뒤 한 float ulp 더 내린 값이다. max_f는 대칭으로 올린다. double 뺄셈은 최근접 반올림이라 외향이 아니므로, 한 ulp 보정이 불변 조건 7을 문자 그대로 성립시킨다. 프록시가 다른 노드로 옮겨질 때는 float 값을 다시 변환하지 않고 레코드의 double AABB에서 새로 계산하므로 오차가 누적되지 않는다. 대안(검색용 데이터에 double 직접 저장)은 9.4절에서 비교한다.

**노드.** 평탄 배열 두 개로 나눈다.

- 검색용(16바이트): 자식 블록 시작 색인, 첫 요소 청크 색인, 직접 저장 수, 서브트리 프록시 수.
- 관리용: level과 정수 원점 3성분, 자식 점유 마스크(8비트), 상태 플래그(분할 보류, 재평가 대기, 재배치 대기, 유지보수 대기), 보류 시점의 직접 저장 수, 보류 이후 변경 횟수, 재평가 간격, 대기열 연결 색인.

구현(OT1)은 노드 번호를 다음과 같이 정했다. 루트는 옥트리 객체 안에 두고 번호 0을 쓴다. 블록 b의 자식 i는 번호 1 + 8b + i다. 검색용 블록과 관리용 블록은 각각 색인 풀(`IndexPool`)에 두고, 둘을 함께 확보한 뒤 같은 번호로 할당한다. 그래서 빈 트리와 이동된 원본은 노드 할당이 없다. 프록시 레코드의 "슬롯"은 노드 안 선형 위치가 아니라 (청크 색인, 청크 안 위치)다. swap-remove를 O(1)로 하기 위해 청크에 이전 청크 연결을, 노드 관리 데이터에 마지막 청크 색인을 둔다.

관리용 데이터에 위치를 두는 이유는 가장 빈번한 경로인 제자리 갱신이 노드의 확장 경계를 O(1)에 구하게 하려는 것이다. 위치를 저장하지 않으면 매 갱신이 부모 링크를 루트까지 따라가야 한다. 자식 점유 마스크는 검색이 읽으므로 측정 결과에 따라 검색용으로 옮길 수 있다. 노드 크기와 정렬은 강제하지 않고 측정한다.

**자식 배치.** 초기안은 자식 8개를 인접 블록으로 할당한다(자식 i = 블록 시작 + i, 부모 링크는 블록당 1개, 해제한 블록은 재사용 목록으로). 다음 두 대안을 같은 API 뒤에서 구현해 측정한다.

| 방식 | 빈 슬롯 | 자식 접근 | 비고 |
|---|---|---|---|
| 8개 인접 블록(초기안) | 빈 자식도 자리를 차지한다 | 덧셈 한 번 | UE `TOctree2` 방식 |
| 존재하는 자식만 압축 | 없음 | 마스크의 popcount | 자식이 생길 때 블록을 재배치한다 |
| 풀에 개별 저장 | 없음 | 자식별 색인 8개 | 노드가 커지고 형제가 흩어진다 |

빈 슬롯을 허용한 인접 배치가 압축 배치보다 빠를 수 있으므로, 색인 사용만으로 캐시 효율이 나아졌다고 판단하지 않는다.

**풀.** 요소 청크와 자식 블록은 평탄 저장소와 uint32 재사용 목록으로 이루어진 색인 풀에 둔다. `PoolAllocator`는 쓰지 않는다. 모든 할당·반환이 mutex로 동기화되고, 반환이 chunk 수에 비례하는 선형 검색이며, 포인터를 돌려줘 색인 연결과 맞지 않고, 이동할 수 없기 때문이다(PoolAllocator.h:14-38, 84). 범용 색인 풀은 Core에 선행 작업으로 추가하고 모듈 안에 자체 구현하지 않는다(선행 SIP7).

**범위 밖 목록.** 프록시의 경계를 double로 저장하는 별도 선형 목록이다. 기준이 되는 노드 중심이 없으므로 float 상대 경계를 쓰지 않는다.

**용량과 재배치.**

- 색인은 모두 32비트다. "없음" 표지는 영역 상수(`NoNode`, `NoChunk`, `OutOfRangeNode`)로 `NumericLimits<uint32>::Max()`에서 정의한다.
- 프록시 수가 `maxProxies`를 넘으면 `CapacityExceeded`, 노드·청크 수가 32비트 색인 폭을 넘으면 `Overflow`를 돌려준다. 프록시를 버리지 않는다.
- 배열이 커져 재배치되어도 핸들과 색인은 유효하다. 포인터를 저장하지 않는다.
- 배열과 풀은 제거 시 줄이지 않는다. 축소는 `MaintenanceBudget`의 명시적 축소 요청으로만 한다.

**정적 데이터.** 변경이 없는 집합을 Morton 순서로 선형화한 읽기 전용 트리는 후속이다. 정적 mesh의 렌더 판정은 대상이 아니다(10.4절 계층 해시 담당). 동적 트리의 전체 재배치를 매 프레임 수행하지 않는다.

### 6.4 삽입과 분할

**삽입.**

1. 입력을 검증한다.
2. 6.5절의 루트 조건을 확인한다. 루트 확장이 필요하면 수용 판정을 먼저 하고, 한도 안에 들어가지 않으면 범위 밖 목록에 등록하고 끝낸다.
3. 저장할 노드를 결정한다. 루트에서 배치 규칙으로 내려가다가 잎이거나 자식에 들어가지 않는 노드에서 멈춘다.
4. 그 노드의 청크에 빈 자리가 없으면 청크를 확보한다. 실패하면 `OutOfMemory`를 돌려주고 아무것도 바꾸지 않는다.
5. 저장하고, 루트부터 저장 노드까지 경로의 서브트리 프록시 수를 1씩 늘리며, 0에서 1이 된 노드의 부모 점유 비트를 켠다.
6. 저장 노드가 잎이고 직접 저장 수가 `splitThreshold`를 넘으면 분할을 평가한다. 보류 중이면 재평가 조건만 확인한다.

프록시 하나마다 최대 깊이까지 내려가지 않는다. 자식 블록은 분할 평가를 통과했을 때만 만든다.

**분할 평가.** `splitThreshold`는 평가를 시작하는 수이며 수용 한도가 아니다. 평가는 할당 없이 O(직접 저장 수)로 다음을 계산한다.

- 하강 수 d와 잔류 수 s: 배치 규칙으로 자식에 들어가는 수와 남는 수.
- 자식별 분포: 8개 자식 각각이 받는 수.

판정은 다음 순서다.

1. 자식 기본 셀이 `minCellSize`보다 작아지면 보류한다.
2. d < `splitThreshold / 2`(정수 나눗셈)이면 보류한다. 대부분이 부모에 남는 분할은 순회 비용만 늘린다.
3. 하강할 프록시가 둘 이상의 자식으로 나뉘면 한 level만 분할한다.
4. d개가 모두 한 자식으로 가면 실제 분리 level을 정확히 구한다.
   - 하강할 프록시 중심의 level 0 정수 셀 좌표를 축별로 최소·최대로 구한다. 축별 (최소 XOR 최대)의 최상위 비트 위치 중 가장 큰 값이 ℓ_sep이다. level ℓ_sep + 1의 노드에서 중심들이 처음으로 둘 이상의 자식으로 갈린다. 모든 중심이 같은 level 0 셀에 있으면 분리 level이 없다.
   - 하강 보장 level ℓ_fit은 L_ℓ ≥ 2e/(k − 1)인 가장 낮은 ℓ이다. e는 하강할 프록시의 최대 반 변이다. k = 1이면 크기 기준의 보장이 없으므로 ℓ_fit = 0으로 두고 아래 재계수로 판정한다.
   - 분리 level이 있고 ℓ_sep ≥ ℓ_fit이면 level ℓ_sep + 1의 노드에서 배치 규칙으로 하강 수 d′를 다시 센다. d′가 판정 2를 통과하고 둘 이상의 자식으로 나뉘면, 현재 노드부터 그 노드까지의 단일 자식 체인과 그 노드의 자식 블록을 한 번에 만든다.
   - 그 밖에는 보류한다.

판정 4가 뜻하는 것은 다음과 같다.

- 좁지만 분리 가능한 군집은 단일 자식 구간을 지나 실제로 나뉘는 깊이까지 분할한다. "모두 한 자식에 들어간다"는 이유만으로 멈추지 않는다.
- 위치와 AABB가 거의 같은 집합은 분리 level이 없거나 ℓ_sep < ℓ_fit이다. 큰 잎으로 두고 끝에서 나뉘지 않는 체인을 만들지 않는다.
- 체인 중간 노드에는 하강할 프록시의 직접 저장이 생기지 않는다. 다만 중심이 확장 띠에 있거나 크기 때문에 체인 자식에 들어가지 못하는 프록시는 중간 노드에 남는다. 체인 길이는 `maxRootLevel` 이하다.
- ℓ_sep은 노드 원점 기준의 상대 정수 셀 좌표로 계산하고, 확장 띠에 있는 중심은 노드 셀의 끝 칸으로 자른다. 노드 level이 초기 루트 level 이상이면 격자가 2^ℓ 배수에 정렬되지 않기 때문이다. ℓ_sep은 분할 깊이를 고르는 근거일 뿐이며, 분할 여부는 실제 배치 규칙을 모의한 d′ 재계수로 정한다. 체인 중간에서 형제 잎으로 빠지는 프록시는 옮기되 d′에는 넣지 않는다.
- 단일 자식 경로의 압축은 후속이며 측정으로 효과가 확인될 때 넣는다.

경계 중첩과 추가 순회 비용은 평가식에 넣지 않는다. 판정 2의 임계(`splitThreshold / 2`)와 `splitThreshold`를 9.4절 측정으로 조정해 반영한다.

**분할 수행.** 필요한 블록을 모두 먼저 확보한다. 하나라도 실패하면 분할하지 않고 보류로 처리하며, 삽입 호출은 성공을 돌려준다. 확보 뒤 내려간 모든 프록시의 레코드(노드, 슬롯), 서브트리 프록시 수, 자식 점유 마스크, float 경계(새 노드 중심 기준으로 double에서 재계산)를 갱신한다. 분할로 생긴 자식이 `splitThreshold`를 넘으면 같은 호출 안에서 그 자식을 이어서 평가한다. 이어지는 평가의 총 비용은 이번 분할로 이동한 프록시 수에 비례한다.

**보류 상태.** 분할하지 않기로 한 노드는 보류 플래그와 그 시점의 직접 저장 수 n₀를 기록한다. 보류 중인 노드는 삽입마다 평가를 반복하지 않는다.

- 재평가 조건: 직접 저장 수가 2·n₀ 이상이 되거나, 보류 이후 그 노드에서의 삽입·제거·경계 갱신 횟수가 재평가 간격(처음 n₀/2) 이상이 된다.
- 재평가는 삽입 호출 안에서 바로 하고, 제거·경계 갱신 호출에서는 재평가 대기 플래그만 세운다. 경계 갱신의 재삽입 경로(조상에서 다시 내려가기, 범위 밖 목록에서 트리로)는 보류 여부와 관계없이 분할 평가를 하지 않고 대기만 표시한다. 대기 중인 재평가는 다음 삽입(그 잎에 들어올 때)이나 유지보수가 수행한다. 제거·갱신이 분할 때문에 실패하거나 O(n) 비용을 지는 일이 없게 하려는 것이다.
- 재평가가 다시 보류로 끝나면 n₀를 그 시점의 직접 저장 수로 바꾸고 재평가 간격을 두 배로 늘린다(상한 n₀ · 2^8). n₀를 그대로 두면 2·n₀에 닿은 뒤 삽입마다 O(n) 평가가 반복된다. 분할에 성공하면 보류 정보를 지운다.
- 직접 저장 수가 `splitThreshold` 이하로 줄면 보류를 해제한다.

평가 비용은 변경당 상각 O(1)이다. 보류 중에도 프록시는 그 노드에서 항상 검색된다.

### 6.5 삭제, 이동, 루트 변경

**삭제.**

1. 핸들 → 레코드 → (노드, 슬롯)으로 찾는다.
2. 노드의 마지막 프록시를 빈 자리로 옮기고(swap-remove) 옮겨진 프록시의 레코드 슬롯을 고친다. 비게 된 마지막 청크는 풀에 돌려준다.
3. 부모 링크를 따라 루트까지 서브트리 프록시 수를 줄이고, 1에서 0이 된 노드의 부모 점유 비트를 끈다.
4. 서브트리 프록시 수가 `mergeThreshold` 이하가 된 잎 아닌 노드와, 점유 마스크가 0이 된 잎 아닌 노드를 유지보수 대기열에 넣는다.

비용은 O(깊이)다. 삭제는 할당을 하지 않으므로 실패하지 않는다(핸들 오류 제외).

**경계 갱신.**

1. 새 AABB를 검증한다.
2. 새 AABB가 현재 노드의 확장 경계에 들어가면 float 경계와 레코드의 double AABB만 고치고 끝낸다. 현재 노드가 잎이 아니고 새 AABB가 중심으로 고른 자식의 확장 경계에도 들어가면 그 노드를 재배치 대기로 표시한다(판정 O(1)).
3. 벗어나면 대상 위치를 먼저 계산한다. 부모 링크를 따라 올라가며 확장 경계가 새 AABB를 포함하고 기본 셀이 새 AABB 중심을 포함하는 첫 조상 A를 찾고, A에서 배치 규칙으로 내려가 새 노드 N을 정한다. 그런 조상이 없으면 6.5절 루트 조건(확장 또는 범위 밖 목록)으로 처리한다. 중심 조건은 필수다. 중심이 A의 확장 띠에 있으면 A가 고른 자식에 들어가지 못해 프록시가 A(내부 노드)에 남고, 어느 자식에도 맞지 않으므로 하강 재배치로도 내려가지 않는다. OT5 측정에서 이 조건이 빠져 순간 이동 3,000회 뒤 내부 노드 직접 저장 2,431개, 작은 질의 판정 수 25배가 생겼고 유지보수로 회복되지 않았다(2026-11-18 수정, 시험 `LooseOctree_LO06_UpdateReinsertMatchesAddQuality`). 중심이 A의 기본 셀 안이면 루트에서 A까지가 배치 규칙의 경로이므로(불변 조건 2) A에서의 하강이 `TryAdd`와 같은 노드에 닿는다.
4. N의 청크 자리(필요하면 새 청크)와 루트 확장 블록을 모두 확보한다. 하나라도 실패하면 `OutOfMemory`를 돌려주고 프록시는 옛 AABB로 옛 위치에 남는다.
5. 옛 위치에서 swap-remove하고 N에 저장한다. 서브트리 수는 옛 노드부터 A의 자식까지(A 제외) 줄이고, A의 자식부터 N까지(A 제외) 늘린다. 점유 비트와 대기 등록은 삽입·삭제와 같은 규칙이다.

핸들은 갱신 전후로 같다. 갱신은 변경된 프록시만 다룬다. 순간 이동과 크기 변경도 같은 경로다.

2번에서 끝난 프록시는 새 AABB가 더 깊은 노드에 들어갈 수 있게 되었어도 상위 노드에 남는다(객체 축소, 자식 쪽으로의 이동). 정확성은 유지되고 검색 품질만 떨어진다. 재배치 대기로 표시된 노드는 유지보수가 예산 안에서 처리한다(6.6절).

**범위 밖 목록.** 범위 밖 목록의 프록시가 갱신되면 수용 판정을 다시 해서, 한도 안에 들어오면 트리에 삽입한다. 목록의 크기가 `outOfRangeWarnCount`를 넘으면 진단 이벤트를 낸다.

**루트 조건과 확장.** AABB가 루트의 확장 경계에 들어가지 않거나, 중심이 루트의 기본 셀 밖이면 루트를 확장한다. 중심 조건을 두는 이유는 루트의 기본 셀과 확장 경계 사이 띠에 중심을 둔 프록시가 어떤 자식에도 들어가지 못하고 루트에 무제한 쌓이는 것을 막으려는 것이다. 제자리 갱신으로 중심이 루트 기본 셀 밖으로 나간 경우는 품질 문제로 두고 확장하지 않는다.

1. **수용 판정.** 확장 전에 정수 연산으로 방향 규칙을 모의해, `maxRootLevel` 이하에서 조건을 만족하고 노드 정수 좌표가 `TryCreate`가 검사한 격자 정확 범위(±2^(maxRootLevel+1) 셀) 안인 루트가 있는지 계산한다. 격자 범위 조건은 빈 트리 재설정으로 루트가 원점에서 멀어진 뒤 확장할 때 필요하다. 비용은 O(maxRootLevel)이고 트리를 바꾸지 않는다. 없으면 루트를 바꾸지 않고 범위 밖 목록에 넣는다.
2. **방향 규칙.** 축마다 AABB 중심이 루트 중심보다 작으면 그 축의 음의 방향으로 넓힌다(정수 원점에서 2^R을 뺀다). 아니면 양의 방향으로 넓힌다. 조건을 만족할 때까지 level을 하나씩 올린다.
3. **블록 확보.** 필요한 단계 수만큼 블록을 먼저 확보한다. 실패하면 호출이 `OutOfMemory`로 끝나고 루트는 바뀌지 않는다.
4. **단계마다의 갱신.** 새 블록을 만들고 옛 루트의 내용(직접 저장 청크 연결, 자식 블록 색인, 플래그, 관리 데이터)을 새 블록의 해당 자식 자리로 옮긴다. 옛 루트의 직접 저장 프록시 레코드의 노드 색인, 옛 루트 자식 블록의 부모 링크, 대기열 항목을 고친다. 새 루트의 서브트리 수는 옛 루트의 값, 점유 마스크는 해당 자식 비트 하나다. 옛 루트가 비어 있으면 내용 이동 없이 새 루트만 만든다. 기존 노드의 월드 경계는 바뀌지 않는다.

이 비용은 단계마다 옛 루트의 직접 저장 수에 비례한다. 한 호출의 확장 횟수는 `maxRootLevel − 현재 루트 level` 이하다.

**루트 축소.** 유지보수에서만 수행한다. 다음이 모두 참인 상태가 유지보수 4회 연속으로 관측될 때 한 단계 축소한다. 연속 관측 조건은 확장과 축소가 프레임마다 왕복하는 것을 막으려는 이력이다.

- 루트 level이 `initialRootLevel`보다 크다.
- 루트의 직접 저장 수가 0이다.
- 점유된 자식이 정확히 하나다.

축소는 확장의 역이다. 그 자식의 내용을 루트 자리로 옮기고, 해당 레코드·부모 링크·대기열 항목을 고치고, 빈 형제 7개와 그 하위 블록을 해제한다. 해제하는 블록의 대기 항목은 제거한다(6.6절).

루트에 직접 저장된 프록시가 하나라도 있으면 축소하지 않는다. 그 프록시들은 자식의 확장 경계에 들어가지 않아 루트에 있는 것이므로, 축소하면 불변 조건 1이 깨진다.

**빈 트리.** 프록시가 0개가 되면 유지보수가 루트를 `initialRootLevel`로 되돌리고 모든 블록을 해제한다. 빈 트리의 첫 삽입은 그 프록시의 중심을 포함하는 level `initialRootLevel`의 격자 셀로 루트를 다시 잡는다. 정수 원점은 o = o₀ + 2^R₀ · ⌊(x − x₀) / L_R₀⌋(x₀ = anchor + o₀ · minCellSize)이다. 그래서 루트가 멀리 이동한 뒤 비워졌다가 다시 채워져도 다단 확장을 반복하지 않는다.

### 6.6 빈 노드, 병합, 유지보수

"현재 노드가 빔"(직접 저장 수 0)과 "서브트리 전체가 빔"(서브트리 프록시 수 0)을 구분한다. 검색은 자식 점유 마스크로 서브트리가 비지 않은 자식만 순회한다.

**정확성을 위한 갱신과 유지보수의 구분.** 삽입, 삭제, 경계 갱신, 필터 변경은 호출 안에서 불변 조건을 모두 만족시킨다. 유지보수는 다음 작업이며, 수행하지 않아도 검색 결과는 정확하다.

| 순서 | 작업 | 등록 조건 | 내용 |
|---|---|---|---|
| 1 | 병합 | 잎 아닌 노드의 서브트리 수 ≤ `mergeThreshold` | 자식 8개가 모두 잎인 노드만 처리한다. 자식의 프록시를 그 노드로 모으고 자식 블록을 해제한다. 처리 뒤 부모가 조건을 만족하면 부모를 등록한다 |
| 2 | 빈 블록 회수 | 잎 아닌 노드의 점유 마스크가 0 | 자식 8개가 모두 잎인 노드만 처리한다. 자식 블록을 해제하고 부모를 다시 검사한다 |
| 3 | 분할 재평가 | 재평가 대기 플래그 | 6.4절 판정을 수행한다 |
| 4 | 하강 재배치 | 재배치 대기 플래그 | 직접 저장 프록시 중 자식에 들어가는 것을 자식으로 내린다 |
| 5 | 루트 축소 | 매 `RunMaintenance` 끝에서 확인 | 6.5절 |

병합과 회수는 아래에서 위로만 진행한다. 자식에 잎이 아닌 노드가 남아 있으면 그 자식을 먼저 처리할 수 있게 등록하고, 이번 항목은 다시 대기시킨다. 따라서 항목 하나의 처리 비용은 블록 하나와 프록시 `mergeThreshold`개로 묶인다. 병합에 필요한 청크를 확보하지 못하면 그 항목을 건너뛰고 다시 대기시킨다.

**대기열.**

- 대기열은 노드 관리 데이터 안에 연결 색인을 둔 이중 연결 목록이다. 노드당 한 번만 들어가며 대기량은 노드 수를 넘지 않는다.
- 블록을 해제할 때 그 블록 8개 노드의 대기 항목을 제거한다. 해제된 노드나 재사용된 블록 번호를 가리키는 항목이 남지 않는다(불변 조건 9).
- 처리 시점에 조건을 다시 확인하고, 조건이 사라진 항목은 버린다.

**예산.** `RunMaintenance(budget)`은 `MaintenanceBudget`에 따라 처리한다.

- 작업 수 예산: 처리할 항목 수. 결과가 결정적이다. 시험과 결정성이 필요한 소비자는 이것만 쓴다.
- 시간 예산: 경과 시간 한도. 시계 확인은 16개 항목마다 한다. 처리량이 실행마다 달라지므로 구조와 결과 순서가 결정적이지 않다.
- 축소 요청: 배열과 풀의 예약 용량을 사용량에 맞춘다. 레코드 `SlotArray`는 세대 기록을 보존해야 하므로 줄이지 않는다.
- 작업 수와 시간이 모두 0이면 대기열 처리와 루트 확인(축소 이력, 빈 트리 재설정)을 하지 않는다. 축소 요청은 그래도 수행한다. 모든 대기 항목을 처리하는 `MaintenanceBudget::All()`을 둔다.
- 시간 예산의 마감 시각은 포화 덧셈으로 구한다. 처리 중 청크 확보에 실패하면 그 항목을 다시 대기시키고 이번 실행을 멈춘다.

결과 `MaintenanceReport`는 처리 수, 남은 수, 작업별 수(병합, 회수, 재평가, 재배치, 루트 축소), 경과 시간을 담는다.

**진동 방지.** `TryCreate`가 `mergeThreshold ≤ splitThreshold / 2`를 강제한다. 병합 직후 노드의 프록시 수는 `splitThreshold / 2` 이하이므로, 다시 분할 평가가 시작되려면 그 이상의 삽입이 필요하다. 임계 부근에서 삽입과 삭제가 번갈아 와도 분할·병합 한 주기에 최소 `splitThreshold / 2`번의 변경이 필요하므로 구조 변경 비용이 상각된다.

**누적 방지.** 대기량이 max(`pendingLimitFloor`, 노드 수 / 4)를 넘으면 변경 호출이 한도 아래로 내려갈 만큼을 그 자리에서 작업 수 예산으로 처리한다. 항목 하나의 비용이 위와 같이 묶이므로 변경 호출의 최악 추가 지연은 (초과량 × 항목 비용 상한)이다. 대기량, 처리 수, 강제 처리 횟수는 통계로 보고한다.

**풀 재사용.** 해제한 자식 블록과 요소 청크는 각자의 재사용 목록으로 돌아간다.

### 6.7 과밀 상황

| 상황 | 처리 |
|---|---|
| 좁지만 분리 가능한 군집 | 6.4절 판정 4로 실제 분리 level까지 분할한다 |
| 위치와 AABB가 거의 같은 집합 | 분리 level이 없거나 하강 보장이 없으므로 큰 잎(청크 연결)으로 두고 보류한다. 재평가 간격을 지수로 늘린다 |
| 큰 객체가 부모에 누적 | 내부 노드의 직접 저장으로 수용한다. 노드별 직접 저장 수의 분포를 통계로 내고, `parentWarnCount`를 넘으면 진단 이벤트를 낸다 |
| 순간적인 대량 생성·이동 | 프록시는 호출 안에서 검색 가능한 위치에 놓인다. 분할 평가는 상각 O(1)이고 병합·재배치는 유지보수로 미룬다. `TryReserve`로 용량을 미리 잡는다 |
| 결과 수가 많은 검색 | 결과 K개를 내는 비용은 K 이상이다. 결과를 자르지 않는다. 완전 포함 판정으로 공간 검사를 생략한다(6.8절) |

- 분할 임계 초과만으로 프록시를 버리지 않는다. 실제 용량 부족은 `CapacityExceeded`나 `Overflow`로 보고한다.
- 구조 최적화를 미뤄도 프록시는 항상 검색 가능한 위치에 있다.
- 큰 잎의 순차 검사는 조밀한 SoA 청크, 필터의 선적용, SIMD(선행 SIP3)로 효율을 높인다.
- 큰 객체 전용 목록과 노드 내부 BVH는 후속이다. 9절에서 내부 노드 누적이 검색 시간의 주된 비용으로 측정될 때 도입을 검토한다(14절 결정 필요 8). 모든 경계가 실제로 겹치는 집합은 보조 트리로도 줄지 않는다.
- 장대 객체 하나가 루트를 높은 level에 고정하고 축소를 막을 수 있다. 이 경우도 큰 객체 목록의 검토 대상이다.

### 6.8 검색

**노드 판정.** 질의 형상에 대해 노드의 확장 경계를 제외, 완전 포함, 부분 교차로 나눈다. 노드 판정은 double로 한다.

- 제외: 그 서브트리를 건너뛴다.
- 완전 포함: 그 서브트리의 모든 프록시가 공간 조건을 만족한다(불변 조건 1·2). 하위에서는 노드 판정과 프록시 AABB 판정을 생략하고, 필터 판정과 결과 출력만 한다. 이 판정은 정확한 double 경계에 기반하므로 불변 조건 7에 의존하지 않는다.
- 부분 교차: 직접 저장 프록시를 하나씩 판정하고 점유된 자식으로 내려간다.
- ray에는 완전 포함 분류가 없다.

내부 노드에도 프록시가 있으므로 방문한 모든 노드의 직접 저장 프록시를 검사한다. 잎만 검사하지 않는다.

**접촉 규약.** 모든 판정은 닫힌 집합 규약이다. 접촉은 교차이고, "밖"은 엄격 부등식이다. Core `Intersection.h`의 접촉 = Hit 규약과 같다. 프록시 max와 노드 확장 경계 max가 같은 값일 수 있으므로 노드 제외 판정에 열린 비교를 쓰지 않는다.

**프록시 판정의 정밀도.**

- 박스: 질의를 노드 확장 경계로 double에서 잘라낸 뒤(clamp) 노드 중심 기준 상대 좌표의 float로 바꾸고, min은 내림·max는 올림으로 변환한다. 저장 경계와 질의가 모두 외향이므로 정확한 double AABB가 교차하는 프록시는 반드시 후보가 된다. 초과 후보의 폭은 level에 따라 커진다. 루트 level 24(한 변 2^31 cm)에서 상대 좌표 2^23 cm 이상 구간의 float ulp는 1 cm 이상이고, 루트 반폭 2^30 cm 근처에서는 128 cm다.
- 구·프러스텀·ray(OT3 구현): float 넓힘 대신 레코드의 정확한 double AABB에 정답 기준과 같은 Core 술어(`Overlap(sphere, box)`, `Overlap(volume, box)`, `Raycast(ray, box, maxDistance)`)를 쓴다. 노드 가지치기와 완전 포함 판정도 같은 술어를 노드 확장 경계(double)에 적용한다. 이 술어들은 box 좌표에 대해 단조인 고정 순서 식(부호별 꼭짓점 선택과 고정 순서 내적, 성분별 clamp 거리 제곱합과 가장 먼 꼭짓점 거리, slab 진입·이탈)이고 IEEE 반올림도 단조이므로, 프록시 AABB ⊆ 확장 경계이면 반올림 뒤에도 "확장 경계 밖 → 프록시 비후보", "확장 경계 완전 포함 → 프록시 후보", "노드 진입 거리 ≤ 프록시 진입 거리"가 성립한다. 따라서 후보 집합은 정답 술어 집합과 정확히 같다(누락·초과 0). 이 성질은 Core 술어의 식 순서에 기대므로, 그 식을 단조성이 깨지는 형태(중심·반폭 분해 등)로 바꾸면 안 된다(Intersection.h 주석과 단조성 시험). 대가는 걸친 노드의 후보마다 레코드를 읽는 비용이며, float + 넓힘 방식과의 비교는 9.4절 측정 항목이다.
- Core 술어 자체의 한계: 독립 리뷰가 정수 128비트 정확 판정과 대조한 결과, 실제 기하의 접촉 경계에서 1 ulp 수준의 차이가 있다. 구–박스는 제곱 합이 53비트를 넘는 큰 좌표의 정확한 접촉을 놓칠 수 있고(시험 사례의 1.4%), 볼록체–박스는 평면이 꼭짓점을 지나는 경우 양쪽으로 틀릴 수 있으며, ray–박스는 넓히는 방향으로만 틀렸다. 옥트리 결과는 이 술어와 정확히 같으므로, 이 한계는 옥트리가 아니라 Core 술어 계약(접촉 판정은 반올림 한계 안)에 속한다.

**프러스텀.** `ConvexVolume`은 안쪽을 향하는 법선의 평면 집합이다. 후보 술어는 "어느 평면에 대해서도 완전히 바깥(엄격)이 아닌 AABB"다. 이 술어는 모서리 근처에서 볼록체와의 정확한 교차보다 넓다. 정답 기준(8절)은 같은 술어를 double로 계산한다.

**ray.** `QueryRay`는 ray(`maxDistance`까지의 선분)와 확장 경계가 교차하는 노드만 방문한다. 자식은 진입 거리가 가까운 순으로 방문하고, 진입 거리가 같으면 자식 색인 오름차순이다(원점이 여러 확장 경계 안에 있으면 진입 거리가 모두 0이다).

**최근접 ray.** `QueryRayNearest`는 후보마다 `hitTest(const SpatialProxyHit&, double currentMax)`를 호출하고 `RayHitResult`를 받는다.

- 전제: `hitTest`가 돌려주는 거리는 Core `Raycast`가 그 프록시 AABB에 대해 계산한 진입 거리 이상이다(정밀 형상이 등록 AABB 안에 있음). 정밀 판정을 다른 식으로 계산해 AABB 면에서 1–2 ulp 작게 나올 수 있으면 hitTest가 그 진입 거리로 clamp해야 한다. 이 전제를 어기면 ulp 차이만큼 먼 프록시가 답이 될 수 있다.
- `hit`가 참이고 거리가 현재 최단 거리보다 작을 때만 최단 거리를 줄인다.
- 노드 가지치기는 double 확장 경계 진입 거리가 현재 최단 거리보다 엄격하게 클 때 한다. 스택에 쌓을 때와 꺼낼 때 모두 검사한다.
- 프록시 가지치기는 레코드 double AABB의 Core `Raycast` 진입 거리가 현재 최단 거리보다 엄격하게 클 때만 한다.
- 첫 후보는 거리가 `maxDistance` 이하면 받는다. NaN·음수·`maxDistance` 초과 거리는 받지 않는다. 같은 거리면 먼저 찾은 후보가 남는다.
- AABB 진입 거리만으로는 최단 거리를 줄이지 않는다.

**순회 방식.** 초기안은 고정 용량의 명시적 스택이다.

- 필요 용량은 7 · (D − 1) + 1이다(D = 깊이 상한 `maxRootLevel + 1`). 항목은 노드 색인, level, 정수 원점, 완전 포함 플래그로 약 40바이트이며, D = 32에서 약 9 KB다.
- 스택 배열의 정적 용량(선행 SIP4)은 `maxRootLevel` 31에서 유도한 값으로 고정하고, `TryCreate`가 설정이 그 안에 드는지 검사한다. 초과가 일어나지 않는다.
- visitor 안의 재진입 검색은 스택을 하나 더 쓴다. 워커 스택 한도에 대비해 재진입 깊이를 소비자가 제한한다.
- 재귀 구현과의 비교는 9.4절에서 측정한다. 검색은 힙 할당을 하지 않는다.

**방문 표시.** 단일 등록이므로 같은 핸들이 두 번 나오지 않는다. 프록시별 query stamp를 두지 않는다. 향후 여러 인덱스의 결과를 합칠 때 중복 제거가 필요하면 프레임 번호가 아닌 질의 식별자와 질의별 방문 집합을 쓴다. 프록시에 넣은 단일 stamp는 병렬·중첩 질의에 안전하지 않고 atomic으로도 해결되지 않는다.

**통계.** `QueryStats`는 호출자가 넘기는 질의별 구조체다(방문 노드 수, 프록시 검사 수, 완전 포함으로 생략한 수, 결과 수). 트리의 공유 상태에 쓰지 않으므로 병렬 검색에서 경합이 없다. 타이밍 측정 빌드에서는 계수 코드를 컴파일 시점에 제거한다(9.5절).

### 6.9 동시성

기본 모델은 변경 단계와 읽기 전용 단계의 분리다.

- 변경 단계: 한 스레드만 `LooseOctree`의 변경 함수(`Try*`, `RunMaintenance`, `Clear`)를 호출한다.
- 읽기 단계: 여러 스레드가 동시에 검색한다. 검색은 트리의 어떤 상태도 바꾸지 않는다. 각 검색은 자기 스택, 결과 버퍼, `QueryStats`를 쓴다.
- 두 단계는 겹치지 않는다. visitor 안에서 같은 트리를 변경하는 것은 위반이다. visitor 안에서 같은 트리를 다시 검색하는 것은 허용한다.
- 읽기 단계가 읽는 데이터는 평탄 배열뿐이며 const 접근이 내부 상태를 바꾸지 않는다. `HashMap`·`OrderedMap`을 쓰지 않는다(10.4 규약 8의 const 반복 문제와 무관하다).

**API로 강제.** 검색 함수는 `LooseOctreeReader`에만 있고, reader는 `SpatialReadScope`에서만 얻는다. 범위 없이 검색하는 코드는 컴파일되지 않는다. `TryGet`, `IsValid`, `Count`, `Stats`, `Validate`, `VisitNodes`도 reader에 둔다.

**위반 검출.**

- 검출은 Core 범용 검출기 `AccessOverlapChecker`(`Core/Diagnostics/AccessOverlapChecker.h`, SIP9)가 한다. 읽기 수와 변경 수를 담은 atomic 단어 하나에 범위 진입·종료마다 seq_cst 읽고-쓰기 연산을 하고, 상대 범위가 열려 있으면 `platform::Fatal`이다. 한 객체에 대한 읽고-쓰기 연산은 하나의 수정 순서를 이루므로, 겹친 두 범위 중 나중에 진입한 쪽의 진입이나 먼저 진입한 쪽의 종료가 반드시 상대를 본다. UE 5.8.4 `FRWAccessDetector`(Core/Public/Misc/MTAccessDetector.h)와 같은 방식이며, UE와 달리 모든 빌드 구성에서 켠다.
- `SpatialReadScope`는 읽기 범위, 모든 변경 함수는 변경 범위를 연다. LooseOctree는 재귀 정책 `ReadsOnly`(같은 스레드의 읽기 안 읽기만 허용, 변경 재귀 금지)를 쓴다. 변경끼리의 겹침도 `platform::Fatal`이다.
- 옥트리를 이동·대입·파괴할 때 열린 범위가 있으면 `CheckIdle`로 `platform::Fatal`이다. 검출기 자체는 이동하지 않고, 이동 대상은 새 검출기를 쓴다.
- 범위는 질의 단위가 아니라 읽기 묶음(예: 한 프레임의 질의 작업 전체) 단위로 두어, 검색 호출마다 atomic을 건드리지 않는다. reader는 여러 스레드에 복사해 넘길 수 있다.
- 범위와 reader는 저장하거나 yield 경계를 넘겨 들고 있지 않는다. 물리의 `QueryReadScope`(EngineImplementationPlan 16.3절)와 같은 규칙이며 TK06의 read/write 충돌 시험과 같은 방식으로 검사한다.

lock-free 구조와 전체 트리의 이중 버퍼는 두지 않는다. 읽기와 변경이 겹쳐야 하는 소비자가 생기면 그 소비자가 변경을 command로 모아 단계 경계에서 적용한다(10.4절의 commit 규약).

### 6.10 통계와 디버그 검증

`Stats()`는 다음을 돌려준다.

- 프록시 수, 범위 밖 목록 수, 노드 수, 잎 수, 최대 깊이, 루트 level.
- 노드별 직접 저장 수의 분포(최대, 히스토그램), 내부 노드 저장 비율, 보류 노드 수, `parentWarnCount` 초과 노드 수.
- 사용·예약·피크 메모리(배열별), 풀 재사용 목록 길이, 할당·해제 횟수와 살아 있는·피크 바이트. 할당 계수는 옥트리가 소유하는 계수 allocator adapter(`TryCreate`가 받은 allocator에서 따로 할당, 옥트리 이동에도 주소 불변) 기준이며 요청 크기로 센다. 배열별 예약 용량 피크는 변경 함수가 끝날 때 갱신하고, 생성 또는 이동 대상이 된 뒤부터 센다.
- 누적 횟수: 분할, 체인 분할, 병합, 회수, 재삽입, 제자리 갱신, 하강 재배치, 루트 확장·축소, 분할 평가, 보류, 재평가.
- 유지보수 대기량(작업별), 강제 처리 횟수.

`Validate()`는 6.2절 불변 조건 전체를 검사하고 `Result<OctreeValidationReport>`를 돌려준다(Core `DebugAllocator::Validate()`와 같은 관례). `Result`는 검사를 수행했는지만 뜻하고, 실제 위반은 성공한 결과의 `invariant`(0 정상, 1–9 불변 조건 번호, 10 저장 구조 일관성: 청크 연결·풀 사용량·범위 밖 목록·필터·사용자 값 대조)와 `node`로 확인한다. 전용 `ErrorCode`는 두지 않는다. 비용이 O(노드 + 프록시)이므로 시험과 진단에서만 호출한다. 디버그 그리기는 옥트리가 직접 하지 않고, 노드 경계와 직접 저장 수를 순회하는 `VisitNodes`를 제공한다.

## 7. 현재 코드베이스와 선행 작업

HEAD `1c0ef8a` 기준이다.

| 있는 것 | 위치 | 용도 |
|---|---|---|
| `Vec3`/`Vec3d`, `Box<T>`, `Sphere`, `Plane`, `Ray`, `Segment` | Core/Source/Runtime/Core/Public/Core/Math/Vector3.h, Shapes.h | 질의·프록시 경계 |
| `Overlap`, `Raycast`, `SegmentCast`, `ClassifyPlaneSide` | Core/Source/Runtime/Core/Public/Core/Math/Intersection.h | 판정과 정답 기준 |
| `Box2::Contains(Box2)`, `Union`, `ExpandedBy` | Core/Source/Runtime/Core/Public/Core/Math/Rect.h | 3D 보조 연산의 이름 선례 |
| `SlotArray`/`SlotHandle`, `Array`(`TryRemoveAtSwap`), `BitArray`, `Span` | Core/Source/Runtime/Core/Public/Core/Containers/ | 레코드, 평탄 배열 |
| `Allocator` 인터페이스, `NFailAllocator` | Core/Public/Core/Memory/, Core/Source/Developer/TestSupport/ | 저장소, 할당 실패 주입 |
| `PoolAllocator` | Core/Public/Core/Memory/PoolAllocator.h | 사용하지 않음(6.3절 풀) |
| `RandomStream`(PCG32) | Core/Source/Runtime/Core/Public/Core/Math/RandomStream.h | 고정 시드 입력 |
| `Register4f`(SSE2/NEON/scalar) | Core/Source/Runtime/PlatformBase/Public/Platform/Math/ | SIMD 판정의 기반 |
| `WorkerScheduler`, atomic, `Mutex` | Core/Source/Runtime/Task/, Platform/Atomics.h, Platform/Sync.h | 병렬 검색 측정, 위반 검출 |
| `MonotonicNanoseconds` | Platform/Clock.h | 벤치 시간 측정 |
| 시험 틀(`TEST`/`CHECK`/`REQUIRE`), `ProcessProbe`, `HostFailure` | Core/Source/Developer/TestSupport/Public/TestSupport/ | 시험, Fatal 자식 프로세스 대조 |
| 독립 벤치 실행 파일 형식 | Programs/CompressionBench, Programs/CryptoBench | 벤치 프로그램 골격 |

없는 것은 공간 구조 전체다(Octree, BVH, SpatialIndex, Frustum, Culling은 C++ 코드에 0건). 엔진 쪽 소비 경로(transform commit과 변경 통지, 경계를 가진 primitive 컴포넌트, RenderScene 추출)도 없다.

**선행 작업.** 접두사 SIP는 이 문서의 선행 작업이며 다른 문서의 P 번호(셰이더 선행, 제품 단계, 우선순위)와 무관하다.

| ID | 작업 | 담당 문서 | 상태 | 막는 단계 |
|---|---|---|---|---|
| SIP1 | `ConvexVolume<T>`(평면 최대 32개, 법선 안쪽, 힙 없음)과 `Containment`·`ClassifyContainment`(밖·안·교차)·`Overlap(volume, box|sphere)` | CoreDetailedSpec G04·G06 | 구현 | OT3 |
| SIP2 | `Box<T>` 보조 연산: `Contains`, `Union`, `ExpandedBy`, `ToFloatOutward`와 `math::ToFloatRoundedDown/Up`·`NextUp/NextDown`(정수 비트 연산, 부동소수 환경 무관). 이름은 `Box2` 선례를 따른다 | CoreDetailedSpec G04·G05 | 구현 | OT1 |
| SIP3 | SIMD 비교·mask·select·movemask 연산(`Register4f`의 `Compare*`·`Bit*`·`Select`·`MoveMask`, scalar·SSE2·NEON) | CoreDetailedSpec G08 | 구현 | OT5(SIMD 변형) |
| SIP4 | 고정 용량 배열 `InlineArray<T, N>`(객체 안 저장, 힙 넘침 없음) | CoreDetailedSpec C01 | 구현 | OT3(검색 스택) |
| SIP5 | 범위 분할 병렬 실행 helper | CoreDetailedSpec에 항목 추가 필요(현재 대응 ID 없음) | 후속 | OT6(병렬 검색 측정) |
| SIP6 | `SlotArray`의 색인 접근(`IsOccupiedAt`·`FindAt`·`ElementAt`)과 색인 → 핸들(`HandleAt`), 사전 확보 `TryEnsureCapacity` | CoreDetailedSpec C03 | 구현 | OT1 |
| SIP7 | 범용 색인 풀 `IndexPool<T>`(평탄 저장소 + uint32 LIFO 재사용 목록, 할당 없는 사전 확보 경로) | CoreDetailedSpec C03 | 구현 | OT1 |
| SIP8 | 벤치마크 실행 파일 `Programs/SpatialIndexBench`(묶음 분위수·JSON·실행 스크립트 `RunBench.ps1`) | 이 문서 | 구현 | OT5 |
| SIP9 | 읽기·변경 접근 겹침 검출기 `AccessOverlapChecker`(재귀 정책 `ReadsOnly`·`NestedWrites`·`NestedWritesAndReads`, 범위 객체 `ReadAccessScope`·`WriteAccessScope`) | 이 문서, EngineImplementationPlan 8.4 | 구현 | OT4 |
| SIP10 | `SpatialIndex` 모듈(`Engine/Source/Runtime/SpatialIndex`, Build.cs, EngineTests 등록) | EngineImplementationPlan 3.1·3.2 | 구현 | OT1 |

기반이 부족한 곳은 옥트리 안에서 우회하지 않고 해당 기반을 보강한다. SIP1–SIP7은 Core 쪽 작업이며 옥트리 전용 사본을 만들지 않는다.

## 8. 정확성 시험

정답 기준은 조밀한 배열의 전수 검색이며 정확한 double AABB로 판정한다. 프러스텀은 6.8절의 평면 술어를 double로 계산한다. 참조 구현(비교군 B·C)과 결과가 다르면 독립적인 기하 판정으로 어느 쪽이 맞는지 확인한다. 참조 구현을 정답으로 간주하지 않는다.

접두사 LO는 이 문서의 시험이며 EngineImplementationPlan 10.4절의 SI01–SI10과 별개다.

| ID | 시험 | 판정 |
|---|---|---|
| LO01 | 전수 검색 대조: 박스·구·프러스텀·ray, 8개 분포(9.2절), 필터 조합, 질의 mask 0 | 누락 0건, 중복 0건 |
| LO02 | 보수성 한계: 박스는 후보 집합이 외향 변환 폭만큼 넓힌 전수 결과에 포함되는지, 구·프러스텀·ray는 후보 = 같은 Core 술어의 전수 결과인지, 원점에서 10⁹ cm(10⁷ m) 포함 | 박스는 초과 후보가 한계 안, 나머지는 초과·누락 0 |
| LO03 | 경계 사례: 경계 접촉, 크기 0, 루트보다 큰 객체, 동일 AABB 1만 개, 음수 좌표, 원점에서 10⁹ cm(10⁷ m), 루트 여유 띠의 점 다수 | LO01 기준 충족, `Validate()` 통과 |
| LO04 | 입력 검증: NaN, 무한대, min > max, 빈 핸들, 낡은 핸들, 이중 삭제, 잘못된 질의(유효하지 않은 Box·Ray, 음수·NaN `maxDistance`, 빈 `ConvexVolume`) | 정해진 오류 코드, 상태 불변 |
| LO05 | 고정 시드의 삽입·이동·삭제·검색 시퀀스(연산 10⁶회 이상), 일정 간격으로 `Validate()`와 전수 대조 | 위반 0건 |
| LO06 | 이동: 경계 왕복, 순간 이동, 크기 확대·축소, 제자리 갱신과 재삽입의 구분, 재배치 대기 표시 | LO01 기준 충족, 통계의 재삽입 수가 기대와 일치 |
| LO07 | 루트: 확장(각 방향), 수용 판정으로 한도 밖일 때 루트 불변, 범위 밖 목록과 복귀, 축소 이력, 빈 트리 재설정, 확장 전후 기존 노드 경계의 비트 동일 | 위반 0건 |
| LO08 | 분할·병합: 분리 가능한 군집의 실제 분리 level, 동일 AABB 집합의 보류, 재평가 조건과 간격 증가, 보류 해제, 임계 부근 삽입·삭제 반복 시 분할·병합 횟수 상한 | 기대 구조와 일치 |
| LO09 | 유지보수: 예산 0으로 미뤘을 때의 검색 정확성, 대기량 상한, 강제 처리, 상향식 병합, 블록 해제 시 대기 항목 제거, 전체 삭제 후 재삽입 | 위반 0건 |
| LO10 | 저장: 풀 확장, 슬롯 재사용, `maxProxyGeneration`을 낮춘 세대 retire, N번째 할당 실패 주입 | 실패 시 논리 상태 불변, 불변 조건 성립 |
| LO11 | 동시성: 읽기 범위 안의 변경과 변경 중 범위 생성이 Fatal(자식 프로세스 대조, 오류 대화상자 억제), 병렬 검색 결과가 단일 스레드와 같음 | 일치 |
| LO12 | 조기 종료와 버퍼: `Stop`, `Any*`, `Count*`, 용량 부족 시 `truncated`와 `total` | 계약과 일치 |
| LO13 | 최근접 ray: 정밀 판정 콜백의 거리로만 가지치기되는지, 근접 동률 입력 포함, 전수 최근접과 대조 | 일치 |
| LO14 | API 계약: `TrySetFilter`, `TrySetUserValue`, `TryReserve`, `Clear` 뒤 낡은 핸들, 이동한 인스턴스의 핸들, visitor 안 재검색, ray 동률 방문 순서 | 계약과 일치 |
| LO15 | 결정성: 같은 연산열과 작업 수 예산에서 결과 순서 동일, 같은 입력의 `Stats()` 값이 독립 계수와 일치 | 일치 |

**OT0–OT2 구현 시험(2026-11-18).** `Core/Source/Runtime/SpatialIndex/Tests`에 LooseOctree 시험 27개가 있다. LO01(박스)·LO03–LO10·LO11(변경 겹침, 읽기 범위가 열린 채 이동·대입·파괴의 Fatal을 자식 프로세스로 대조)·LO12(박스)·LO15(독립 모형 대조)가 들어 있고, 부동소수 FTZ·DAZ 상태의 Validate·질의 시험도 있다. LO05는 기본 12만 회이며, 질의의 20%는 범위 밖 목록까지 닿는 먼 질의다. 10⁶회 이상은 같은 시퀀스를 이어서 도는 `FoundationTests --filter LooseOctree_LO05 --repeat 9 [--seed N]`로 실행한다(2026-11-18 seed 1·7에서 1,080,000회 위반 0건). 구현 뒤 독립 리뷰가 별도 퍼저로 약 4,600만 연산(할당 실패 1.8만 회 주입 포함)을 돌려 위반 0건을 확인했다.

시험은 수면이나 경쟁 운에 기대지 않는다. LO11의 순서는 명시적 event로 만든다.

## 9. 벤치마크 계획

### 9.1 비교군

| 군 | 내용 | 표시 |
|---|---|---|
| A1 | 조밀한 배열의 전수 검색, scalar double | 기준선 |
| A2 | 조밀한 float SoA + SIMD 전수 검색. D와 같은 정밀도와 필터 선적용. SIP3 이후 | 기준선(10.4절 평면 SoA 경로에 대응) |
| B | UnityOctree(commit bbc4735)의 동작과 자료구조를 보존한 C++ 이식 | "UnityOctree C++ 이식" |
| C | `TOctree2`(UE 5.8.4)의 설계에 기반한 독립 구현 | "TOctree2 설계 기반 독립 구현". Unreal의 성능으로 표시하지 않는다 |
| D | `LooseOctree` | |
| E | 계층 공간 해시(10.4절) | 참고 측정. 구현된 뒤에 추가하며 판정 기준에는 쓰지 않는다 |

**공통 조건.**

- 모든 군은 같은 입력 정밀도(double AABB 입력), 같은 `Allocator` 구현, 같은 필터, 같은 입력 순서를 쓴다. allocator 종류를 결과에 기록한다.
- 결과 처리는 "미리 예약한 버퍼에 식별 값 기록 + 합계 검증"으로 같게 한다. D의 float 보수성 때문에 후보 수가 다를 수 있으므로 후보 수를 함께 보고한다.
- A1·A2는 삽입(끝에 추가), 갱신(덮어쓰기), 삭제(swap-remove) 비용도 측정한다.

**B의 조건.**

- 노드별 개별 할당과 노드별 동적 배열, 임계값, 순회 순서, 루트 확장 한도를 원본대로 둔다. 최적화를 섞지 않는다.
- 원본에 핸들이 없으므로, 삭제는 경계를 주는 삭제(원본의 빠른 경로)를 쓰고 식별 값 → 객체 대응 비용은 측정 구간 밖에 둔다. 경계 없는 삭제(DFS)는 별도 행으로 측정한다.
- Unity `Bounds`의 경계 포함 규칙은 이식 전에 확정해야 한다(15절).
- 이식에서 달라진 자료형·할당기·기능을 기록한다. 이식 파일 머리에 BSD 2-Clause 고지 원문과 기준 commit을 적는다. 저장소의 외부 코드 정책(`ThirdParty/`, `ThirdParty.json`)과의 관계는 14절 결정 필요 9다. 엔진 바이너리에는 넣지 않는다.

**C의 조건.**

- 3.2절의 설계 사실(8바이트 노드, 인접 블록, 노드별 요소 배열, looseness 1/16, 재귀 순회, 삭제 후 재삽입, `SetElementId` 콜백)을 따르는 독립 작성 코드다. UE 코드를 옮기지 않는다.
- ID 콜백과 외부 ID 저장소 갱신을 삭제·이동의 측정 구간에 넣는다.
- 루트는 고정이며 월드 범위를 데이터에 맞춰 정하고 그 값을 기록한다. 잎 용량은 16과 256 두 가지로 측정한다. 원본의 SIMD 박스 판정은 넣고, 넣었음을 표시한다.

**제외 항목.**

- 실제 `TOctree2`의 직접 측정은 UE 빌드 환경 안에서만 가능하고, 이용 조건의 해당 조항을 확인하지 못했다. 기본 계획에서 제외하고 미실행 항목으로 기록한다(14절 결정 필요 5).
- UnityOctree 원본(C#)의 시간은 측정하지 않는다. C#과 C++의 시간 차이를 자료구조 효과로 해석하지 않는다.

**비교 구분.**

- 공통 API(삽입, 삭제, 박스·ray 검색)는 같은 의미로 비교한다. B·C에 없는 기능(이동, 프러스텀, 구, 최근접)은 삭제 후 재삽입 또는 predicate로 대응시키고 별도 표로 측정한다.
- 동일 설정 비교(같은 잎 용량, 같은 looseness, 같은 최소 셀)와 구현별 조정 비교를 구분해 보고한다.

### 9.2 입력

고정 시드(`RandomStream`)로 다음 분포를 만든다.

- 균일한 3D, 지형형(높이 범위가 좁은 2.5D), 군집, 넓고 희소, 객체 크기 편차가 큰 분포, 셀 경계 집중, 분리 가능한 과밀, 실제 AABB가 겹치는 과밀.

프록시 수는 10³, 10⁴, 10⁵, 10⁶이다. 환경 한계로 실행하지 못한 단계는 사유와 함께 기록한다. 이동 비율은 0%, 1%, 10%, 100%다.

### 9.3 측정 항목

- **연산별**: 구축, 삽입, 삭제, 작은 이동, 경계 통과, 순간 이동, 크기 변경, 박스·구·프러스텀·ray·최근접·존재·개수 검색. 검색은 결과가 없는 경우, 적은 경우, 많은 경우를 나눈다.
- **프레임 작업 부하**: 이동 비율 r의 갱신 N회 + 질의 M회 + 작업 수 예산의 유지보수를 한 프레임으로 합산한다. 광원 상호작용과 그림자 caster 수집의 대표 부하(광원 수와 반경 분포, 이동 primitive 수, 프레임당 질의 수, caster volume 크기)를 정의해 9.6절 판정에 쓴다.
- **별도 시나리오**: 루트 확장·축소, 대량 생성·삭제, 분할·병합 집중, 유지보수 비용(변경 시간에 합산한 값과 분리한 값을 모두 보고한다).
- **트리 상태 구분**: 질의는 구축 직후, 변동 뒤 유지보수 완료, 유지보수 대기 중의 세 상태에서 따로 측정한다.
- **지표**:
  - 구축 시간.
  - 검색·갱신 시간의 중앙값, P95, P99, 관측 최대값.
  - 방문 노드 수, 프록시 검사 수, 결과(후보) 수.
  - 재삽입·분할·병합·루트 변경 횟수.
  - 노드 수, 최대 깊이, 과밀 노드 수.
  - 사용·예약·피크 메모리, 할당·해제 횟수.
  - 유지보수 대기량과 처리 시간.
- **하드웨어 계수기**: 캐시 미스와 분기 예측 실패는 초기 계획에서 제외한다. 저장소에 수집 코드가 없고 수집 도구와 대상 환경이 정해지지 않았다(14절 결정 필요 10). 정해지기 전에는 "미측정"으로 기록한다.

### 9.4 단계별 효과 확인

기준 설정 하나(6.2절 초기 제안값)에서 한 요인씩 바꾼다. 분포와 크기는 대표 부분집합(균일·군집·크기 편차·겹치는 과밀 × 10⁴·10⁶)으로 한다. 전체 조합은 실행하지 않는다.

- 자식 배치 세 방식(6.3절).
- 요소 저장: 고정 청크와 노드별 동적 배열, 44바이트(핸들·사용자 값 포함)와 32바이트(레코드 조회), float 상대 경계와 double 경계, 검색용·관리용 데이터 분리 여부.
- 구·프러스텀·ray의 프록시 판정: double Core 술어(OT3 구현)와 float + 넓힘(레코드 읽기 비용 비교).
- 자식 점유 마스크, 완전 포함 생략.
- 핸들 기반 직접 삭제와 경로 탐색 삭제.
- 제자리 갱신과 삭제 후 재삽입.
- 지연 병합과 즉시 병합, 분리 level 체인 분할과 한 level씩 분할.
- 명시적 스택과 재귀.
- looseness 1, 65/64, 5/4, 3/2, 2. `splitThreshold` 8, 16, 32, 64. `chunkCapacity` 8, 16, 32.
- 루트 방식: 단일 루트 확장과 루트 셀 격자(`FSparseDynamicOctree3` 방식). 후자는 변형으로만 측정한다.

방문 노드 수가 줄어도 캐시 미스가 늘면 느릴 수 있으므로, 계수만으로 채택하지 않고 시간과 함께 판단한다. 특정 분포의 최고 성능만으로 일반적인 우위를 주장하지 않는다.

### 9.5 측정 방법과 재현성

- 연산 하나가 수십 ns이면 시계 호출 비용과 해상도가 측정값을 지배한다. 시간 분포는 연산 K회 묶음 단위로 재고, P95·P99는 묶음 시간의 분포로 정의한다. K를 결과에 기록한다.
- 비교군마다 별도 프로세스에서 N회 반복하고 중앙값과 최소값을 보고한다. 이상치를 버리지 않고 관측 최대값으로 함께 보고한다.
- 워밍업을 두고 비교군의 실행 순서를 회전시킨다. 코어 고정, 전원 설정, 동시 실행 프로세스를 기록한다.
- 계수 수집은 별도 빌드로 하고 타이밍 빌드에서는 컴파일 시점에 제거한다. B·C도 같은 방식으로 계수를 넣는다.
- 컴파일러와 버전, 최적화 설정(Release), 하드웨어, 스레드 수, 시드, 실행 명령을 결과 파일에 기록한다.
- 데이터 생성과 정답 검증은 타이밍 구간 밖에서 한다.
- 원시 측정값은 JSON으로 남기고 보고서는 그 파일에서 만든다.
- 측정하지 않은 값과 개선율을 적지 않는다.

### 9.6 판정 기준(제안)

측정 전에 기준을 정해 사후 해석을 막는다. 수치는 측정 전 제안값이며 OT5 결과로 재산정할 수 있으나, 재산정하면 그 사유를 기록한다.

| 판정 | 기준 |
|---|---|
| 제자리 갱신 채택 | 이동 비율 10%·100%에서 재삽입이 30% 이상 줄고 검색 시간 증가가 10% 이하 |
| 평면 전수 판정으로 대신하는 규모 | 대표 부하에서 A2(평면 SoA 전수)가 D보다 빠른 프록시 수·광원 수 구간을 기록한다. 그 구간에서는 소비자가 옥트리 대신 평면 전수 판정을 쓸 수 있다 |
| 그림자 caster 수집 노드 묶음 | 노드 묶음 병렬 수집이 단일 질의 대비 worker 8개에서 확장 효율 0.6 이상 |
| 저장 변형 채택 | 대표 부분집합 전체에서 중앙값이 나빠지는 분포가 없고, 어느 한 분포에서 10% 이상 나아짐 |

**OT5 예비 결과(2026-11-18, [보고서](../Benchmarks/SpatialIndex/2026-11-18/report.ko.md)).** 측정 내내 기기 CPU가 대부분 80–100%여서 프로세스 간 잡음이 −37%~+17%였다. 몇 배 단위 차이와 계수만 근거로 쓴다. 기기가 한가할 때 다시 잰다.

- 대표 부하 정의: 광원 64개(16·256 변형) 구 질의, 반경 3–30 m, caster 프러스텀 4개, 걷기 이동 ±50 cm, 유지보수 `Operations(256)`.
- 제자리 갱신: 경계 갱신 결함(6.5절) 수정 뒤 10⁴ 대표 4분포에서 재삽입 93–100% 감소, 프레임 질의 시간 −31%~+5%로 **기준 충족**(`rerun-update-fix/`). 10⁶ 행은 수정 뒤 다시 재지 않았다.
- 평면 전수 대체 규모: A2가 D보다 빠른 구간은 10³의 여러 분포, 10⁴·10⁵의 분리 가능 과밀·겹치는 과밀, 10⁶의 겹치는 과밀이다(결함 수정 전 D 기준, 보고서 6.2절).
- 저장 변형: 모두 **미충족**. looseness 1–5/4는 균일·군집·겹치는 과밀에서 크게 좋아지고 크기 편차에서 크게 나빠진다(큰 프록시가 위 노드에 쌓임). 박스 질의는 float 상대 청크가 double 레코드보다 빠르다.
- TOctree2 설계 기반 독립 구현(C)이 D보다 작은 질의에서 2–8배 빠르다. 계수로 D는 노드 163개, C는 9개를 판정한다(looseness 2의 겹침, 데이터보다 큰 초기 루트). 결정 필요 8과 함께 다룬다.
- 결과가 많은 구·프러스텀·ray는 후보마다 레코드를 읽는 D가 순차 전수보다 3–6배 느리다. 6.8절 float + 넓힘 대안은 미구현이라 비교하지 못했다.

## 10. 단계

접두사 OT는 이 문서의 단계다. OT0–OT3은 별도 브랜치에서 먼저 진행하고, OT4 이후는 착수 시점을 따로 정한다(12절).

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| OT0 | 선행 SIP2·SIP4·SIP6·SIP7·SIP10 — **구현**(2026-11-18) | 결정 필요 7 | 각 기반의 시험 통과 |
| OT1 | 저장 구조, 삽입, 삭제, `Validate()`, 박스 검색 — **구현**(2026-11-18) | OT0 | LO01(박스)·LO03·LO04·LO10 |
| OT2 | 분할 평가·보류, 경계 갱신, 루트 확장·축소, 유지보수 — **구현**(2026-11-18) | OT1 | LO05–LO09 |
| OT3 | 구·프러스텀·ray·최근접, 버퍼·존재·개수 질의, 노드 묶음 — **구현**(2026-11-18) | OT2, SIP1 | LO01 전체·LO02·LO12·LO13·LO14 |
| OT4 | 읽기·변경 단계 검증 — **구현**(2026-11-18) | OT3, SIP9 | LO11·LO15 |
| OT5 | `SpatialIndexBench`, 비교군 A·B·C, 단계별 변형 측정 — **구현·예비 측정**(2026-11-18, 부하 있는 환경) | OT4, SIP3, SIP8 | 9절 원시 결과와 보고서 |
| OT6 | 병렬 검색·노드 묶음 수집 측정, 초기값 확정, 비교군 E 참고 측정 | OT5, SIP5 | 9.6절 판정 |

엔진 소비자 연결(World·RenderScene)은 transform commit과 경계를 가진 컴포넌트가 생긴 뒤의 후속이다. 최종 저장 구조와 갱신 정책은 OT5·OT6의 측정으로 확정하며, 성능이 나빠지는 조건과 적용 한계도 보고한다.

## 11. 사용 예

```cpp
LooseOctreeConfig config;
config.anchor = Vec3d(0.0, 0.0, 0.0);
config.minCellSize = 128.0; // cm
config.initialRootLevel = 10;
config.maxRootLevel = 24;
config.looseness = 2.0;
config.splitThreshold = 16;
config.mergeThreshold = 6;
config.maxProxies = 1u << 20;

Result<LooseOctree> created = LooseOctree::TryCreate(config, allocator);
if (!created) {
    return Result<void>::Failure(created.Error());
}
LooseOctree octree = Move(created.Value());

// 변경 단계
SpatialProxyDesc desc;
desc.bounds = bounds;
desc.filterMask = FilterLocalLightReceiver;
desc.userValue = entityId;
Result<SpatialProxyHandle> added = octree.TryAdd(desc);
if (!added) {
    return Result<void>::Failure(added.Error());
}
SpatialProxyHandle handle = added.Value();

Result<void> updated = octree.TryUpdateBounds(handle, movedBounds);
if (!updated) {
    return updated;
}
octree.RunMaintenance(MaintenanceBudget::Operations(256));

// 읽기 단계
{
    SpatialReadScope read(octree);
    LooseOctreeReader reader = read.Reader();
    Result<QueryOutcome> queried = reader.QueryBox(
        lightBounds, FilterLocalLightReceiver, [&](const SpatialProxyHit& hit) noexcept {
            if (!receivers.TryPushBack(hit.userValue)) {
                pushFailed = true;
                return QueryControl::Stop;
            }
            return QueryControl::Continue;
        });
    if (!queried) {
        return Result<void>::Failure(queried.Error());
    }
}
```

## 12. 고정 순서와의 관계

- 메인 줄(A0-B → … → A1–A6 → 화면 마일스톤)의 순서는 바꾸지 않는다.
- OT0–OT2는 사용자 결정(2026-11-18)으로 별도 브랜치 `feature/spatial-index`에서 먼저 진행한다. 선행 작업(SIP2·SIP4·SIP6·SIP7·SIP10)도 그 브랜치에서 하고, 준비되면 `feature/engine-runtime`에 바로 병합한다.
- 다른 단계가 같은 Core 기반을 먼저 고치고 있으면 그쪽 변경 위에서 작업한다. 공용 파일(`SlotArray.h`·`Shapes.h`·FoundationTests 등록 목록)은 병합 충돌을 확인한다.
- OT3(구·프러스텀·ray·최근접, 노드 묶음)은 사용자 결정(2026-11-18)으로 같은 브랜치에서 이어 진행한다. OT4 이후(단계 검증, 벤치마크)의 착수 시점은 따로 정한다.
- 비교군 E(참고 측정)는 계층 공간 해시가 렌더러 단계에서 구현된 뒤에만 측정할 수 있다.

**OT0–OT2 구현에서 알려진 한계.**

- 체인 분할은 d′가 `splitThreshold / 2`일 때 서브트리 수가 `mergeThreshold` 이하인 내부 노드를 만들 수 있고, 이 노드는 대기열에 들어가지 않는다. 판정 2를 그대로 따른 결과이며 품질 문제다.
- 작은 트리는 병합이 루트로 모이면서 루트에 직접 저장이 생겨 루트 축소가 서지 않을 수 있다. 6.7절의 장대 객체 문제와 같은 부류다.
- 이동된 원본은 받은 allocator에 직접 묶이고, 다음 `TryAdd`·`TryReserve`가 계수 adapter를 다시 만든다. 그 할당이 실패하면 `OutOfMemory`다. `TryCreate`도 adapter 할당 때문에 `OutOfMemory`로 실패할 수 있다.

## 13. 다른 문서 개정

이 문서를 확정하거나 결정 필요 사항을 판정할 때 함께 개정할 문서다. 이 문서는 아래 문서를 직접 고치지 않는다.

| 문서 | 개정 내용 | 시점 |
|---|---|---|
| EngineImplementationPlan 3.1·10.4·31(R21)·33.10·33.18 | 공간 질의 전략, `LooseOctree` 배정, 5.8.4 근거, SI09–SI11 | 완료(개정 1.13, 2026-11-18) |
| EngineImplementationPlan 10.4 | 결정 필요 2·4의 판정, 33.10절 미확인 표기 갱신 | 판정 시 |
| CoreDetailedSpec G04·G05·G06·G08·C01·M03 | SIP1–SIP4·SIP7의 요구 반영 | OT0 착수 전 |
| CoreDetailedSpec(새 항목) | SIP5(범위 분할 병렬 helper), SIP6(`SlotArray` 색인 접근) | OT0 착수 전 |

## 14. 결정 필요 사항

1. **이동 primitive–local light 상호작용과 reflection capture·probe 선택의 구조.** 결정됨(2026-11-18): 광원 상호작용과 CPU 경로 그림자 caster 수집은 `LooseOctree`, reflection capture·probe 선택은 평면 SoA. EngineImplementationPlan 개정 1.13.
2. **에디터 비가시 대상 선택과 headless 질의.** 10.4절은 EditorBackend 해시와 double 정밀 판정으로 정했다. 제안: 에디터 문서 작성 시점에 OT5 측정 결과로 비교한다.
3. **게임플레이 공간 질의.** 결정됨(2026-11-18): Jolt scene query를 쓰고 SpatialQueryService는 폐기한다. 대량 대상의 관심 판정은 SI09 기준을 넘을 때 후속 2D 격자로 옮긴다. EngineImplementationPlan 개정 1.13.
4. **비유한 입력의 처리.** 10.4절은 격리 목록에 넣어 항상 판정하고, 이 문서는 거부한다. 제안: 범용 부품은 거부하고, 가시성 보수성이 필요한 렌더 소비자가 자기 격리 목록을 둔다. 10.4절 개정 때 문구를 맞춘다.
5. **실제 `TOctree2`의 직접 측정 여부.** UE 빌드 안에서 같은 입력으로 측정할 수 있으나 EULA의 관련 조항을 확인하지 못했다. 제안: 기본 계획에서 제외하고 독립 구현(C)만 측정한다.
6. **10.4절의 UE 근거 갱신.** 10.4절의 근거는 5.4 기준이다. SceneCulling의 level 산정과 loose 여부는 5.8.4 소스에서 확인되었다(3.3절). 제안: 다음 EngineImplementationPlan 개정 때 33.10절의 미확인 표기를 갱신하고 5.8 기준으로 재확인한다.
7. **착수 시점.** 결정됨(2026-11-18): OT0–OT2는 별도 브랜치에서 지금 진행한다. OT3은 OT2 병합 뒤 이어 진행한다(2026-11-18). OT4 이후는 따로 정한다.
8. **큰 객체 전용 목록과 looseness 기본값.** OT5에서 looseness를 줄이면 대부분의 분포가 크게 좋아지나 크기 편차 분포가 크게 나빠졌다(9.6절). 제안: OT6에서 큰 객체 목록(기준 크기를 넘는 프록시를 별도 선형 목록에 두는 변형)과 작은 looseness를 함께 구현해 같은 부하로 측정하고, 크기 편차를 포함한 대표 부분집합 전체에서 9.6절 기준을 충족하는 조합을 기본값으로 정한다.
9. **비교군 B 이식 코드의 외부 코드 정책.** 결정됨(2026-11-18): `ThirdParty.json`이 원본 파일 SHA-256을 대조하므로 이식 코드 자체는 등록할 수 없다. `Engine/Source/ThirdParty/UnityOctree/`에 `kind: data`로 등록하고 commit archive의 `LICENCE` 원문만 원본 디렉터리에 둔다(archive·LICENCE SHA-256, BSD-2-Clause, `binaryNoticeRequired`). 이식 파일(`Programs/SpatialIndexBench/Private/UnityOctreePort.*`)은 머리에 BSD 원문과 기준 commit을 두고, manifest `notes`에 위치와 달라진 점을 적는다. 엔진 바이너리에는 들어가지 않는다.
10. **하드웨어 계수기 수집 도구와 대상 환경.** 결정됨(2026-11-18): OT5에서는 제외하고 미측정으로 기록했다. 수집 도구는 필요해질 때 정한다.
11. **검색용 경계 정밀도, 루트 방식, 자식 배치.** 제안: 9.4절 측정으로 정한다.

## 15. 미확인 사항

- UE EULA의 코드 열람·이식·측정 공표 관련 조항. 저장소에는 링크 한 줄만 있다.
- Unity `Bounds`의 `Contains`·`IntersectRay`·`GeometryUtility.TestPlanesAABB`는 native라 소스가 공개되지 않았다. 비교군 B는 닫힌 구간·slab 진입 거리·평면 술어로 가정했다(`Intersects`와 `min`·`max`는 UnityCsReference cf583fbe에서 확인).
- UnityOctree 저장소의 보관 상태는 GitHub 표시이며 로컬 클론으로는 확인되지 않는다.
- `TOctree2`에서 루트 밖 요소가 가장자리 노드에 저장되는 동작. 코드 해석이며 실행으로 확인하지 않았다.
- `TOctree2` 분할 재삽입 루프의 안전 조건(해석).
- UE `IntersectBox8Plane`의 SIMD 구현 세부와 가시성 작업 크기 산정식.
- SceneCulling의 도입 버전과 World Partition의 level 선택식(33.10절의 미확인 중 남은 두 항목).
- Chaos `TAABBTree`의 dirty 요소 한도 분기 조건, World Partition `RuntimeHashSet`의 내부 구조. 스트리밍 격자가 쿠킹 시 구축된다는 서술은 헤더 선언만 보았다.
- kd-tree 부재 확인은 Engine/Source/Runtime과 Plugins 범위다. Editor·Developer는 검색하지 않았다.
- 이 문서의 모든 성능 판단. 구현과 측정이 없다.

## 16. 개정 이력

- 개정 1(2026-11-16): 초안. UnityOctree bbc4735와 UE 5.8.4(f45729c0b6f0) 소스 조사, 용도별 구조 선택, `LooseOctree` 설계, 시험·벤치마크 계획. 반영 전 교차검증 3건(설계 반례, 참조 사실, 계획 정합·누락)의 지적을 반영하였다. 주요 변경은 분할 판정의 분리 level 계산, 루트 확장의 사전 수용 판정과 중심 조건, 구·프러스텀·ray의 오차 넓힘, 실패 시 논리 상태 보존 절차, 상향식 병합과 대기 항목 제거, reader 기반 동시성 강제, 10.4절 소비자 배정의 결정 필요 분리다.
- 개정 2(2026-11-18): 사용자 결정으로 공간 질의 전략을 UE 5.8.4와 같게 정했다(EngineImplementationPlan 개정 1.13). 결정 필요 1·3을 결정됨으로 바꾸고, 5.1절 소비자 표를 다시 썼다. 3.5절(UE 게임플레이 공간 질의 근거), 그림자 caster 수집용 노드 묶음 API(6.1절), 9.6절 판정 기준(평면 전수 대체 규모, 노드 묶음 확장 효율)을 추가했다. 비교군 E는 참고 측정으로 내렸다.
- 개정 3(2026-11-18): OT0–OT2 구현 결과를 반영했다. 노드 번호와 레코드 슬롯 표현(6.3절), 분리 level 계산과 체인 중간 노드의 잔류(6.4절), 경계 갱신 중 분할 평가 지연과 보류 기준 수 갱신(6.4절), 루트 수용 판정의 격자 범위 조건(6.5절), 예산 0·축소 요청·마감 포화(6.6절), 할당 계수 통계와 `Validate()` 보고 반환(6.10절), 선행 작업 상태(7절), 시험 실행 방법(8절)을 고쳤다.
- 개정 4(2026-11-18): OT3 구현 결과를 반영했다. 구·프러스텀·ray의 프록시 판정을 double Core 술어로 바꾸고 단조성 근거와 Core 술어의 접촉 한계를 적었다(6.8절). 최근접 전제와 거리 처리, 노드 묶음 필드·유효성(6.1절), LO02 기준을 고쳤고 δ(ℓ) 미확인 항목을 지웠다. SpatialIndex 모듈을 엔진 계층으로 옮겼다(6.1절).
- 개정 5(2026-11-18): OT4 구현을 반영했다. 위반 검출을 Core `AccessOverlapChecker`(SIP9, UE `FRWAccessDetector` 대응, 단일 atomic 단어의 읽고-쓰기 연산, 진입·종료 양쪽 검사)로 옮기고 6.9절을 고쳤다.
- 개정 6(2026-11-19): OT5(벤치마크, SIP3·SIP8) 예비 결과를 반영했다(9.6절). 측정에서 드러난 경계 갱신 재삽입의 조상 선택 결함을 고치고 6.5절에 중심 조건을 넣었다. 결정 9·10을 결정됨으로, 결정 8을 looseness 기본값과 함께 OT6 측정으로 정하도록 바꿨다. Unity `Bounds` 미확인 항목을 갱신했다.
