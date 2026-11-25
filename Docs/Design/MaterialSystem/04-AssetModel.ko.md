# 머티리얼 시스템(MaterialSystem) 설계

## 4. 에셋 모델

### 4.1 UE 5.8 계층

| 클래스 | 근거 | 핵심 필드 |
|---|---|---|
| `UMaterialInterface`(추상) | `RE/Public/Materials/MaterialInterface.h:350–351` | `CachedExpressionData`(`:1564`), `GetRenderProxy()`(`:725`), 형식별 `GetMaterialResource`(`:841`) |
| `UMaterial` | `RE/Public/Materials/Material.h:448` | `ExpressionCollection`(`:416`), `bUseMaterialAttributes`(`:1066`), `MaterialResources`(`:1264`) |
| `UMaterialInstance`(추상) | `RE/Public/Materials/MaterialInstance.h:625–626` | `Parent`(`:647`), 타입별 override 배열(`:773–805`), `BasePropertyOverrides`(`:821`), `bHasStaticPermutationResource`(`:681`), `StaticPermutationMaterialResources`(`:883`) |
| `UMaterialInstanceConstant` / `…Dynamic` | `MaterialInstanceConstant.h:19–20` / `MaterialInstanceDynamic.h:13–14` | Dynamic은 `Create`(`:176`), index 기반 설정(`:41, 48`) |
| `UMaterialFunctionInterface`·`UMaterialFunction` | `MaterialFunctionInterface.h:57–58`, `MaterialFunction.h:38–39` | 용도 `{Default, MaterialLayer, MaterialLayerBlend}`(`MaterialFunctionInterface.h:32–37`) |
| `UMaterialFunctionInstance` | `MaterialFunctionInstance.h:15–16` | `Parent`·`Base`, 타입별 override(`:26–74`) |
| 레이어·혼합 함수 | `MaterialFunctionMaterialLayer.h:15–28`, `MaterialFunctionMaterialLayerBlend.h:18–33` | 필드 없는 특수화 |
| 레이어 목록 | `RE/Public/Materials/MaterialLayersFunctions.h:131–139` | `Layers`·`Blends`, 에디터 데이터에 `LayerGuids`(`:62–81`) |
| `UMaterialParameterCollection` | `RE/Public/Materials/MaterialParameterCollection.h:77–94` | `ScalarParameters`·`VectorParameters` |

UE는 파라미터 기본값을 `FMaterialCachedExpressionData`의 타입별 병렬 배열로 캐시한다(`RE/Public/MaterialCachedData.h:209, 340–373`). 레이어 스택 노드의 일반화판(`UMaterialExpressionLayerStack`)은 Experimental이다(`RE/Public/Materials/MaterialExpressionLayerStack.h:30–31`). Experimental은 레거시가 아니므로 노드 `Material.LayerStack`으로 들인다(19장, 결정 MD11).

### 4.2 Rocks 에셋 클래스

모든 클래스는 `ROCKS_CLASS`이고 Outer는 Package다(AssetSystem 6.1절). "EditorOnly" 필드는 쿠킹 데이터에서 빠진다(AssetSystem 6.2·10절).

| 클래스 | 필드(EditorOnly 표시) | 쿠킹 데이터 |
|---|---|---|
| `MaterialBase`(Abstract) | profile 목록·조명 굽기 설정·텍스처 sampling scale(19.11절) | sampling scale |
| `Material` | `graph : MaterialGraph`(EditorOnly), `traits : MaterialTraitsDesc`(UE `UMaterial` 속성 대응 전부 — 19.9절), 물리 재질 필드(19.9절, TODO MPQ-PhysicalMaterial), `usage : VertexInputUsage`, `functionRefs`(EditorOnly, Build 의존)·`collectionRefs`(Hard + Build, 4.4절), 미리보기 설정(EditorOnly) | `MaterialArtifact` 목록(품질 수준별), 텍스처·컬렉션 Hard 참조 |
| `MaterialInstance` | `parent : ObjectRef<MaterialBase>`(Hard), 종류별 override 배열(4.3절, 19.11절), `staticOverrides`, `traitOverrides : MaterialTraitOverrides`(값마다 Optional, 19.11절), `layerStackOverrides` | static 변형이 있으면 자기 `MaterialArtifact`, 없으면 평탄화한 override 값만 |
| `MaterialFunction` | `graph`(EditorOnly), `usage : {General, Layer, LayerBlend}`, `exposeToLibrary`(EditorOnly), 입출력 선언 | 없다(쿠킹 런타임에 함수는 필요 없다. 함수를 쓰는 머티리얼에 펼쳐져 들어간다) |
| `MaterialFunctionInstance` | `parent : ObjectRef<MaterialFunction>`, 파라미터 기본값 override | 없다 |
| `ParameterCollection` | `scalars`·`vectors : Array<CollectionParameter>`(`ParameterId`·이름·기본값) | layout과 기본값(World 상태가 쓴다) |

`DynamicMaterialInstance`는 에셋이 아니다. World 쪽 객체가 만들어 소유하며, 부모 머티리얼은 `SoftObjectRef`와 `AssetLease`로 붙잡는다(AssetSystem 6.3절. 계획 7.1절 예시의 `SoftObjectRef<MaterialAsset>`과 같은 규칙). dynamic 파라미터만 바꿀 수 있고 static 값·기본 속성은 바꿀 수 없다(`UnsupportedSetting`). UE도 static 변형 갱신 함수가 에디터 전용이다(`RE/Public/Materials/MaterialInstance.h:1042–1083`). 다만 UE에서 실행 중 인스턴스가 static override를 거부하는 경로 전체를 추적하지는 않았다(미확인).

**함수 인스턴스는 쿠킹 런타임에 남지 않는다.** UE도 함수 호출은 호출한 쪽 그래프에 인라인된다(`RE/Private/Materials/MaterialExpressions.cpp:15774–15824`). Rocks는 함수·함수 인스턴스를 Build 의존으로만 다룬다(4.4절).

### 4.3 리플렉션과 저장

- **그래프 저장.** 머티리얼 에셋은 NodeGraph `GraphDocument`(도메인 `Material`)를 필드로 갖는다([노드 그래프](../NodeGraph/05-Decisions.ko.md) GD1·9.1절). 노드별 속성은 노드 타입이 정한 `ROCKS_STRUCT`의 리플렉션 Record 바이트(`PropertyRecord`, 노드 그래프 GD23)로 담는다. 속성 안의 에셋 참조는 `AssetId`로 저장해 AssetSystem 6.2절 의존 종류로 보고한다(노드 그래프 6.2절). 노드 위치·크기·접힘·주석 상자 같은 배치 데이터는 `GraphDocumentLayout`으로 따로 두고 EditorOnly로 표시한다(semantic hash 밖, 5.5절).
- **모르는 노드.** 등록되지 않은 `NodeTypeId`(plugin 비활성 등)는 속성 바이트를 그대로 보존하는 자리 노드로 읽는다(계획 14.6절, 노드 그래프 GD14). 출력에 닿으면 쿠킹 오류다(5.4절 V12).
- **스키마 변경.** 노드 타입마다 `schemaVersion`을 두고 migration 함수를 등록한다(계획 14.6절, 노드 그래프 10절). 핀 이름 변경은 `PinKey`가 안정 키라 간선이 보존된다. 사라진 핀에 간선이나 사용자 기본값이 있으면 고아 핀으로 남기고(노드 그래프 6.4절), 고아 핀에 닿은 간선은 번역 오류로 보고한다.
- **파라미터 override.** 종류별 배열 `scalarOverrides`·`vectorOverrides`·`textureOverrides`·`dynamicBoolOverrides`·`staticOverrides`로 둔다. 원소는 `{ key : ParameterKey, value }` 구조체이고 `ParameterKey`는 9.2절이다. 텍스처 값은 `ObjectRef<Texture>` 멤버라 AssetSystem 6.1절의 패키지 사이 Hard 간선 관찰 대상(구조체 배열 원소의 참조 멤버)에 든다. `InstancedStruct`에 담으면 그 관찰 대상 밖이라 쓰지 않는다.

### 4.4 참조와 의존(AssetSystem 6.2절 종류)

| 참조 | 종류 | 이유 |
|---|---|---|
| 머티리얼 → 텍스처 기본값 | Hard | 쿠킹·로드에 함께 가야 한다 |
| 인스턴스 → 부모 | Hard + Build | 실행 때 부모 산출물이 필요하고, static 변형은 부모 그래프로 번역한다 |
| 머티리얼·함수 → 함수 | Build(EditorOnly 그래프 필드 안) | 번역 입력이다. 쿠킹 런타임에는 필요 없다 |
| 머티리얼 → 파라미터 컬렉션 | Hard + Build | 실행 때 컬렉션 layout·기본값, 번역 때 layout |
| 인스턴스 → 레이어 스택의 함수·함수 인스턴스 | Build | static 변형 번역 입력 |

Build 의존 순환(함수가 자기 자신을 부름, 인스턴스 부모 사슬 순환)은 오류다(계획 14.6·14.7절 "순환 거부"). AssetSystem의 지원 SCC 규칙을 쓰지 않고 `UnsupportedCycle`로 실패한다.

### 4.5 너겟 키와 cookKey에 들어가는 것(요약, 상세는 12절)

| 단계 | 키 입력 |
|---|---|
| 번역 너겟 | 공통 항목(AssetSystem 9절) + 그래프 semantic hash(파라미터 기본값 제외, 5.5절) + 의존 키(함수 키·컬렉션 layout 서명, static 변형은 루트 머티리얼, 12.2절) + 정규화한 static 값·기본 속성 + 품질 수준(쓰지 않으면 기본 표지) + 프로필 기능 등급 집합·공통 상한 + 노드 타입별 (schemaVersion·lowering 버전) + 번역 처리기 버전 Guid + `MaterialCommon.rsh` 계약 버전 |
| 셰이더 job 너겟 | 셰이더 시스템 10절 그대로. 생성 원문은 전처리 결과(7항)에 들어간다 |
| cookKey | AssetSystem 10절 그대로. 처리기 build key에 번역 키와 job 키 목록이 들어가고, 유도 사용 대상(8.4절)이 지역 입력으로 들어간다 |

**파라미터 기본값**(scalar·vector·텍스처·dynamic bool)은 번역 키에 넣지 않는다. 기본값은 uniform 빈도라 접히지 않으므로 원문을 바꾸지 않는다(6.5절). 텍스처는 종류(2D·Cube 등)와 샘플 종류만 원문을 바꾼다. 쿠킹 처리기는 기본값과 기본 텍스처를 그래프에서 직접 읽어 쿠킹 데이터에 넣고, 기본값 변경은 cookKey(에디터 패키지 콘텐츠 해시)로만 다시 쿠킹된다. static 파라미터 기본값은 원문을 바꾸므로 키에 들어간다. UE는 텍스처 참조 hash를 셰이더 맵 키에 넣는다(`RE/Public/MaterialShared.h:1370–1379` `TextureReferencesHash`). Rocks는 기본값 교체로 셰이더를 다시 만들지 않는다.
