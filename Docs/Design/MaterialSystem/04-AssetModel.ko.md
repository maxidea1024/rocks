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
| `Material` | `graph : MaterialGraph`(EditorOnly), `traits : MaterialTraitsDesc`(UE `UMaterial` 속성 대응 전부 — 19.9절), 물리 재질 필드(19.9절, TODO MPQ-PhysicalMaterial), `usage : VertexInputUsage`, `parameterCollections : ObjectRefArray<ParameterCollection>`(Hard — 함수 Build 의존은 hook, 4.4·4.6.1절), 미리보기 설정(EditorOnly) | `MaterialArtifact` 목록(품질 수준별), 텍스처·컬렉션 Hard 참조 |
| `MaterialInstance` | `parent : ObjectRef<MaterialBase>`(Hard), 종류별 override 배열(4.3절, 19.11절), static override 셋(`staticSwitchOverrides`·`staticComponentMaskOverrides`·`staticEnumOverrides`), `traitOverrides : MaterialTraitOverrides`(값마다 Optional, 19.11절), `overridesLayerStack`·`layerStack`(EditorOnly) | static 변형이 있으면 자기 `MaterialArtifact`, 없으면 평탄화한 override 값만 |
| `MaterialFunction` | `graph`(EditorOnly), `usage : {General, Layer, LayerBlend}`, `exposeToLibrary`(EditorOnly), 입출력 선언 | 없다(쿠킹 런타임에 함수는 필요 없다. 함수를 쓰는 머티리얼에 펼쳐져 들어간다) |
| `MaterialFunctionInstance` | `parent : ObjectRef<MaterialFunctionBase>`, 파라미터 기본값 override(모두 EditorOnly — 4.6.1절) | 없다 |
| `ParameterCollection` | `scalars`·`vectors : Array<CollectionParameter>`(`ParameterId`·이름·기본값), `base`(Hard)·`scalarBaseOverrides`·`vectorBaseOverrides`(4.6.1절) | layout과 기본값(World 상태가 쓴다) |

`DynamicMaterialInstance`는 에셋이 아니다. World 쪽 객체가 만들어 소유하며, 부모 머티리얼은 `SoftObjectRef`와 `AssetLease`로 붙잡는다(AssetSystem 6.3절. 계획 7.1절 예시의 `SoftObjectRef<MaterialAsset>`과 같은 규칙). dynamic 파라미터만 바꿀 수 있고 static 값·기본 속성은 바꿀 수 없다(`UnsupportedSetting`). UE도 static 변형 갱신 함수가 에디터 전용이다(`RE/Public/Materials/MaterialInstance.h:1042–1083`). 다만 UE에서 실행 중 인스턴스가 static override를 거부하는 경로 전체를 추적하지는 않았다(미확인).

**함수 인스턴스는 쿠킹 런타임에 남지 않는다.** UE도 함수 호출은 호출한 쪽 그래프에 인라인된다(`RE/Private/Materials/MaterialExpressions.cpp:15774–15824`). Rocks는 함수·함수 인스턴스를 Build 의존으로만 다룬다(4.4절).

### 4.3 리플렉션과 저장

- **그래프 저장.** 머티리얼 에셋은 NodeGraph `GraphDocument`(도메인 `Material`)를 필드로 갖는다([노드 그래프](../NodeGraph/05-Decisions.ko.md) GD1·9.1절). 노드별 속성은 노드 타입이 정한 `ROCKS_STRUCT`의 리플렉션 Record 바이트(`PropertyRecord`, 노드 그래프 GD23)로 담는다. 속성 안의 에셋 참조는 `AssetId`로 저장해 AssetSystem 6.2절 의존 종류로 보고한다(노드 그래프 6.2절). 노드 위치·크기·접힘·주석 상자 같은 배치 데이터는 `GraphDocumentLayout`으로 따로 두고 EditorOnly로 표시한다(semantic hash 밖, 5.5절).
- **모르는 노드.** 등록되지 않은 `NodeTypeId`(plugin 비활성 등)는 속성 바이트를 그대로 보존하는 자리 노드로 읽는다(계획 14.6절, 노드 그래프 GD14). 출력에 닿으면 쿠킹 오류다(5.4절 V12).
- **스키마 변경.** 노드 타입마다 `schemaVersion`을 두고 migration 함수를 등록한다(계획 14.6절, 노드 그래프 10절). 핀 이름 변경은 `PinKey`가 안정 키라 간선이 보존된다. 사라진 핀에 간선이나 사용자 기본값이 있으면 고아 핀으로 남기고(노드 그래프 6.4절), 고아 핀에 닿은 간선은 번역 오류로 보고한다.
- **파라미터 override.** 종류별 배열 `scalarOverrides`·`vectorOverrides`·`textureOverrides`·`dynamicBoolOverrides`·`staticSwitchOverrides`·`staticComponentMaskOverrides`·`staticEnumOverrides` 등으로 둔다(전체 목록은 19.11절). 원소는 `{ key : ParameterKey, value }` 구조체이고 `ParameterKey`는 9.2절이다. 텍스처 값은 `ObjectRef<Texture>` 멤버라 AssetSystem 6.1절의 패키지 사이 Hard 간선 관찰 대상(구조체 배열 원소의 참조 멤버)에 든다. `InstancedStruct`에 담으면 그 관찰 대상 밖이라 쓰지 않는다.

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

### 4.6 클래스·타입 배치(단계 MS3b)

사용자 지시(2026-11-25): "타입만 임시로 선행해서 작업해놓은건데, 이젠 제대로 해놔야함. 그리고 MaterialBase, Material(MaterialInstance)는 각각 다른 파일로". 클래스당 파일 하나 규칙에 따라 다음과 같이 둔다. MS3a 골격을 파일로 나누는 일은 동작이 같은 이동 커밋으로 먼저 한다.

**모듈 `Material`(에셋, `Engine/Source/Runtime/Material/Public/Material/`)**

| 파일 | 클래스 | 필드 |
|---|---|---|
| `MaterialBase.h` | `MaterialBase`(Abstract) | `subsurfaceProfiles`·`specularProfiles`·`toonProfiles`(TODO)·`neuralProfile`(TODO)·`lightBakeSettings`(TODO)·`textureSamplingScales`(19.11절) |
| `Material.h` | `Material` | `graph : MaterialGraph`(EditorOnly)·`layout : GraphDocumentLayout`(EditorOnly)·`traits : MaterialTraitsDesc`·`usage : VertexInputUsage`·`parameterCollections`·`physicalMaterial`·`physicalMaterialMask`·`physicalMaterialMap`·`renderTracePhysicalMaterials`(TODO)·`virtualizedGeometryOverride`(TODO)·`preview : MaterialPreviewSettings`(EditorOnly) |
| `MaterialInstance.h` | `MaterialInstance` | `parent`·종류별 override 배열·static override 셋·`traitOverrides : MaterialTraitOverrides`·`lightBakeOverrides`·`overridesLayerStack`·`layerStack`(EditorOnly)·물리 재질·profile override(19.11절) |
| `MaterialFunctionBase.h` | `MaterialFunctionBase`(Abstract, UE `UMaterialFunctionInterface`) | — |
| `MaterialFunction.h` | `MaterialFunction` | `graph`·`layout`(EditorOnly)·`usage : MaterialFunctionUsage {General, Layer, LayerBlend}`·`exposeToLibrary`·`libraryCategories`·`description`·`userExposedCaption`(모두 EditorOnly) |
| `MaterialFunctionInstance.h` | `MaterialFunctionInstance` | `parent : ObjectRef<MaterialFunctionBase>`·파라미터 override 배열(모두 EditorOnly) |
| `ParameterCollection.h` | `ParameterCollection` | `scalars`·`vectors : Array<CollectionParameter>`·`base`·`scalarBaseOverrides`·`vectorBaseOverrides` |

**모듈 `MaterialSchema`(장치·객체 없는 값 타입 — 번역·쿠킹·런타임이 함께 쓴다)**

| 파일 | 타입 | 내용 |
|---|---|---|
| `MaterialTraitsDesc.h` | `MaterialTraitsDesc` | 19.9절 "필드" 행 전부. 하위 struct `MaterialTranslucencyTraits`·`MaterialForwardTraits`·`MaterialPostProcessTraits`·`MaterialDisplacementTraits`(각 파일) |
| `MaterialTraitOverrides.h` | `MaterialTraitOverrides` | 19.11절 base property override 16개(`Optional<값>`) |
| `MaterialTraitEnums.h` | `DecalResponse`·`TranslucencyPass`·`TranslucencyLightingMode`·`RefractionMethod`·`RefractionCoverageMode`·`PixelDepthOffsetMode`·`MaterialShadingRate`·`BlendableLocation`·`StencilCompare`·`MaterialFunctionUsage` | 값은 UE enum과 같은 순서·같은 뜻이다(UE `EMaterialDecalResponse` 등, 근거는 각 enum 주석). UE의 숨은 별칭·deprecated 값(예: `BL_BeforeTranslucency`)은 넣지 않는다 |
| `ParameterKey.h` | `ParameterKey`·`ParameterScope` | 9.2절 |
| `ParameterOverride*.h` | `ScalarParameterOverride`·`VectorParameterOverride`·`DoubleVectorParameterOverride`·`TextureParameterOverride`·`StaticSwitchOverride`·`StaticComponentMaskOverride` 등 | `{ key, value }`. 텍스처 값은 4.3절대로 Material 모듈 쪽 `ObjectRef<Texture>` 멤버 struct |
| `MaterialAssetReference.h` | `MaterialAssetReference` | 19.4.3절(노드 속성 안 에셋 참조) |
| `VertexInputUsage.h` | `VertexInputUsage` | 사용 대상 정점 입력 타입 이름 집합(8.4절, 19.10절) |

- **번호표.** 영역·블렌드 모드·shading model 열거는 Renderer 스펙이 정의 권한을 가지며 C++ 정의는 `RenderData` 모듈에 둔다(Renderer 3.2·14.2절, 결정 RN5). `RenderData`가 아직 없으므로 MS3b에서 번호표 세 열거만 가진 `RenderData` 모듈을 만든다. 장면 기술 값 타입 등 나머지는 Renderer RS0이 더한다. 리플렉션 열거라 ObjectRuntime에 의존한다(16절 MRD15). `TranslucentColoredTransmittance`는 Renderer 14.2절이 넣지 않기로 했지만, 머티리얼 쪽은 UE 값 전부를 받는 사용자 지시에 따라 TODO 값으로 끝에 둔다(`MPQ-ColoredTransmittance`, 16절 MRD15).
- **TODO 필드.** 대상 에셋 클래스가 없는 참조 필드는 `MaterialAssetReference`(19.4.3절)로 두어, 클래스가 생겨도 migration이 없다. 기본값이 아닌 값이 있으면 쿠킹 진단 `PrerequisiteMissing`이다(19.9절).

#### 4.6.1 MS3b 리뷰 반영(개정 11)

MS3b 코드 리뷰(필수 4·권장 8·참고 4)를 다음과 같이 반영한다.

- **함수 참조(필수).** UE는 레이어·혼합·함수 인스턴스 부모를 `UMaterialFunctionInterface`로 둔다(`RE/Public/Materials/MaterialLayersFunctions.h`, `MaterialFunctionInstance.h` `Parent`). Rocks도 추상 기반 `MaterialFunctionBase`(파일 `MaterialFunctionBase.h`)를 두고 `MaterialFunction`·`MaterialFunctionInstance`가 상속한다. 함수 참조(`MaterialLayerEntry.layer`·`blend`, `MaterialFunctionInstance.parent`)는 `ObjectRef<MaterialFunctionBase>`이고 **EditorOnly**다. 그래서 쿠킹 패키지에 Hard 간선이 생기지 않는다(4.4절 "Build"). Build 의존은 Material 모듈의 `DependencyDeclarer` hook(`RegisterMaterialAssetHooks`, AssetSystem 16.2절)이 선언한다: 인스턴스 → 부모·레이어 스택 함수, 함수 인스턴스 → 부모. 머티리얼 그래프 안 함수 호출의 Build 의존은 함수 호출 노드 속성이 생기는 MS2e에서 같은 hook에 더한다. `MaterialInstance.layerStack`·`overridesLayerStack`도 EditorOnly다(레이어 스택은 static 변형 번역의 입력일 뿐 실행 때 필요 없다).
- **Static Enum override(필수).** `StaticEnumOverride { key, int32 value }`와 `staticEnumOverrides` 배열을 `MaterialInstance`·`MaterialFunctionInstance`에 둔다(9.3절 Static Enum).
- **Font override(필수).** UE `FFontParameterValue`(`FontValue`·`FontPage`)와 같게 `FontParameterOverride { key, font : MaterialAssetReference, page : int32 }`를 둔다.
- **사용 대상 override(필수).** UE `bOverride_UsageFlags`는 플래그별로 덮고 나머지는 부모 값을 물려받는다(`RE/Private/Materials/MaterialInstance.cpp:2676–2680`). `MaterialTraitOverrides`는 `addedVertexInputs`·`removedVertexInputs` 두 집합을 둔다. 결과 = (부모 ∪ added) − removed다.
- **null로 덮기(참고 16).** 물리 재질·profile·가상화 geometry 대체 override는 `Optional<MaterialAssetReference>`다. 값이 없으면 부모 값을 쓰고, 값이 null 참조면 "없음으로 덮기"다(UE `bOverride*` = true + null과 같다).
- **조명 굽기(권장 7).** `MPQ-LightBake`(정적 조명 굽기 도구)를 19.4.1절에 더한다. `MaterialBase.lightBakeSettings`가 기본값과 다르면 TODO 진단이다. 인스턴스는 `lightBakeOverrides : MaterialLightBakeOverrides`(값마다 `Optional`, UE `bOverride*` 넷과 같다)를 둔다.
- **함수 인스턴스 override(권장 8).** `MaterialFunctionInstance`도 UE처럼(`MaterialFunctionInstance.h:26–74`) 컬렉션·텍스처 컬렉션·글꼴·RVT·SVT override를 가진다. TODO 검사 오버로드를 더한다.
- **파라미터 컬렉션 상속(권장 9).** UE 5.8 컬렉션 상속(`MaterialParameterCollection.h:217–228` `Base`·`ScalarParameterBaseOverrides`·`VectorParameterBaseOverrides`)을 들인다. `ParameterCollection.base : ObjectRef<ParameterCollection>`(Hard — 실행 때 layout과 기본값이 부모에서 온다), `scalarBaseOverrides`·`vectorBaseOverrides : Array<{ parameterId : Guid, value }>`. UE `BaseStateId`는 Rocks에서 layout 서명으로 대신한다(12.2절).
- **shading model preset(권장 10).** 인스턴스의 shading model override는 Renderer 번호표가 아니라 UE `EMaterialShadingModel` 값 전부를 가진 `ShadingModelPreset`(MaterialSchema)이다. `PreintegratedSkin`·`FromMaterialExpression`도 받는다. `SurfaceFromShadingModel`이 번호표로 바꾼다(10.3절).
- **컬렉션 Hard 참조(권장 11).** `Material.parameterCollections : ObjectRefArray<ParameterCollection>`(Hard)를 둔다. 그래프의 컬렉션 노드에서 번역이 채우고, 실행 때 컬렉션 layout·기본값을 함께 로드한다(4.4절). 함수 Build 의존은 위 hook이다.
- **물리 재질 표(권장 6).** UE `PhysicalMaterialMap[8]`(`Material.h:470`, MIC에도 있음)을 `physicalMaterialMap : MaterialAssetReference[8]`로 둔다(TODO `MPQ-PhysicalMaterial`).
- **texture sampling scale(참고 14).** 기본값을 UE `FMaterialTextureInfo`와 같게 한다: `samplingScale` 0, `uvChannel` −1(`NoUvChannel`). UE는 텍스처 이름·참조로 잇고 Rocks는 번역 결과 텍스처 slot 번호로 잇는다(12.5절).
- **사용 대상 집합(참고 15).** `VertexInputUsage`는 선언 순서를 보존하고 중복이 없다. `AddVertexInput`·`ContainsVertexInput` 도우미로만 넣는다.
- **레이어 EditorOnly 칸(참고 17).** UE `FMaterialLayersFunctionsEditorOnlyData`의 `LayerStates`·`LayerNames`·`RestrictToLayerRelatives`·`RestrictToBlendRelatives`·`LayerGuids`·`LayerLinkStates`·`DeletedParentLayerGuids`를 들인다. 레이어마다 `visible`·`displayName`·`restrictToLayerRelatives`·`restrictToBlendRelatives`·`layerId`·`linkState : MaterialLayerLinkState`이고, 스택에 `deletedParentLayerIds`가 있다. `MaterialFunction`에 UE `UserExposedCaption`(EditorOnly)을 `userExposedCaption`으로 더한다.
- **이름.** 19.9·19.11절 표의 필드 이름을 코드에 맞춘다(아래 표 고침).

#### 4.6.2 MS3b 재리뷰 반영(개정 12)

MS3b 재리뷰(필수 1·권장 4·참고 1)를 다음과 같이 반영한다.

- **Build hook의 패키지 판정(필수).** hook은 참조 대상의 outermost를 `ObjectRegistry::GetOutermost`로 구한다(PackageWriter와 같은 방법). outermost가 Package가 아니면(패키지 없는 객체) `InvalidArgument`로 실패한다. 조용히 건너뛰면 Build 의존이 빠진 채 저장되어 쿠킹 키가 낡는다. PackageWriter도 같은 경우를 저장 실패로 다룬다. 패키지 id가 null이어도 `InvalidArgument`다. 자기 패키지 판정은 대상의 outermost와 `context.object`의 outermost를 handle로 비교한다. 저장 id(`context.asset`)로 비교하면 복제 때 원본 패키지 안 객체가 원본 id로 보고되기 때문이다. Registry 조회 오류는 그대로 돌려준다.
- **함수 인스턴스는 모든 필드가 EditorOnly(권장 2).** 4.2절대로 함수 인스턴스는 쿠킹 데이터가 없다. UE는 override 배열을 일반 `UPROPERTY`로 두지만(`MaterialFunctionInstance.h:26–74`), Rocks는 함수 인스턴스 패키지가 텍스처 등을 Hard로 끌고 쿠킹되지 않도록 `parent`와 override 배열 전부를 EditorOnly로 둔다. 함수를 쓰는 머티리얼이 번역 때 값을 펼쳐 넣는다. 버린 대안은 쿠커가 `MaterialFunctionBase` 계열을 쿠킹 대상에서 빼는 것이다. 이 방법은 타입별 예외를 쿠커에 두게 된다.
- **인스턴스 물리 재질 mask·map(재리뷰 확인 16).** UE MIC는 `PhysMaterialMask`와 `PhysicalMaterialMap`을 부모로 내려가지 않고 자기 값만 쓴다(`RE/Private/Materials/MaterialInstanceConstant.cpp:64–66` `GetPhysicalMaterialMask`, `RE/Private/Materials/MaterialInstance.cpp:2187–2194` `GetPhysicalMaterialFromMap`). Rocks도 같게 `MaterialInstance.physicalMaterialMask : MaterialAssetReference`와 `physicalMaterialMap : MaterialAssetReference[8]`을 "자기 값, 상속 없음"으로 둔다. `physicalMaterialOverride`만 UE `bOverridePhysMaterial`처럼 `Optional`이다. 값이 없으면 부모 값이고, null 값이면 엔진 기본 물리 재질이다(`MaterialInstance.cpp:2157–2168`).
- **스펙 표(권장 1).** 4.2·4.3·4.6절 표의 필드 이름을 코드에 맞춘다(이 개정).
- **시험(권장 3·4).** 함수 인스턴스 왕복, 컬렉션 `base`·`parameterCollections`의 Hard 의존과 로드 연결, `vectorBaseOverrides`, 레이어 스택의 참조 아닌 칸 왕복, 쿠킹 패키지에 함수·함수 인스턴스 참조 항목이 없음, hook의 Outer 2단·null·자기 패키지·복제 id·패키지 없는 대상, 함수 인스턴스 TODO 검사 양성·음성, 선행 ID 16개 전수(enum 순서 = 19.4.1절 표 순서)를 더한다.
- **19.4.1절 표 순서(참고).** `MPQ-LightBake` 행을 끝으로 옮겨 enum 순서와 맞춘다.
- **RenderData 의존(앞 리뷰 12).** `RenderData` 모듈이 ObjectRuntime에 의존하는 것(리플렉션 열거)은 Renderer 스펙이 정할 일이다. 16절 MRD15로 렌더 세션에 요청하는 일로 남긴다(요청 대상 세션이 정해지면 보낸다).
