# 머티리얼 시스템(MaterialSystem) 설계

## 19. 노드 목록과 머티리얼 체계 대응(UE 5.8 전수)

5.7절은 처음에 "1차 범위" 노드 묶음만 정했다. 2026-11-25 사용자 지시로 범위를 UE 5.8 전체로 넓혔다(결정 MD11·MD12). 지시는 두 가지였다: "언리얼의 모든 MaterialExpression 및 매터리얼 체계를 구현", "선행작업이 필요한건 TODO로 해놓는 식으로라도 뼈대를 잡아놔야함". 이 장은 두 가지를 정한다.
- UE 5.8의 노드 클래스 전부를 Rocks 노드에 대응시키고, 노드마다 상태·단계·공급 조건을 정한다.
- `UMaterial`·`UMaterialInstance`의 속성과 사용 플래그를 Rocks 필드에 대응시킨다.

파일은 다음과 같다.
- [노드 표 1 — 값·수학·문맥 입력·분기·파라미터·텍스처](NodeTableCore.ko.md)(19.6.1–19.6.10절)
- [노드 표 2 — 렌더링 기능·함수·속성·표면·출력·플러그인·제외](NodeTableRendering.ko.md)(19.6.11절부터)
- [머티리얼 속성·사용 플래그·인스턴스 대응](MaterialProperties.ko.md)(19.9–19.12절)

설계 교차 검증 1회를 반영했다(범위 리뷰 16건·정확성 리뷰 18건, 개정 10).

### 19.1 조사 범위와 방법

**대상.** UE 5.8.4 원본에서 `UMaterialExpression`을 상속하는 UCLASS 전부다. 다음 선언 형식을 모두 찾았다: 상속이 다음 줄에 있는 선언, `class UE_DEPRECATED(...) UMaterialExpression…`, `UDEPRECATED_` 접두. 모두 365개다.

| 위치 | 수 | 비고 |
|---|---|---|
| `RE/Public/Materials/`, `RE/Internal/Materials/` | 315 | 기반 `UMaterialExpression` 1개 포함. `MaterialExpressionSubstrate.h` 한 파일에 25개 |
| `Engine/Source/Runtime/Landscape/Classes/Materials/` | 8 | Landscape 모듈 |
| `Engine/Plugins/Interchange/Runtime/Source/Import/…/MaterialX/` | 32 | MaterialX 임포터 전용(`meta=(Private)`). deprecated 3개 포함 |
| `Engine/Plugins/2D/Paper2D`, `Runtime/RenderTrace`, `Experimental/VirtualHeightfieldMesh`, `Experimental/MeshPartition` | 1·1·1·5 | 플러그인 |
| `Engine/Plugins/Runtime/AR/AppleAR`, `…/Google/GoogleARCore` | 1·1 | `UDEPRECATED_` 접두(deprecated) |
| 합계 | 365 | |

**UE 표지 확인.**
- 팔레트에서 빼는 규칙은 `ED/MaterialEditor/Private/MaterialExpressionClasses.cpp:97–124`다. 빠지는 것은 다음과 같다: abstract·deprecated 클래스, `MaterialLayerOutput`, `meta=(NewMaterialTranslator)`(IR 번역기를 끈 기본 상태), `meta=(Private)`, `Comment`·`Parameter`·`NamedRerouteUsage`.
- 메뉴 범주는 UE 5.8부터 생성자가 아니라 `CFG/BaseMaterialExpressions.ini`에 있다(`+MenuCategories`). Rocks 범주(19.6절)는 이 범주를 바탕으로 묶었다.
- 같은 ini의 `[/Script/Engine.MaterialExternalCodeCollection]`은 렌더러가 공급하는 값 101개를 선언한다(`CFG/BaseMaterialExpressions.ini:1037–`). 선언마다 반환 타입·HLSL 정의·단계·허용 영역·미분·환경 정의가 있다. Rocks는 이것을 문맥 값 표(19.3.2절)로 받는다.
- deprecated 노드는 7개다.
  - `AtmosphericFogColor`: 표시 이름 "(deprecated)"
  - `LegacyBlendMaterialAttributes`: 헤더 주석 "Deprecated blend expression"(`MaterialExpressionBlendMaterialAttributes.h:70`)
  - MaterialX `Place2D`·`Rotate2D`·`Swizzle`: `UE_DEPRECATED`
  - AR passthrough 2개: `UDEPRECATED_`
  - 노드 전체가 아니라 값 하나만 deprecated인 것도 있다: `Transform`·`TransformPosition`의 particle 공간(`MaterialExpressionTransform.h:30–31, 56–57`, `MaterialExpressionTransformPosition.h:42–43`).
- UE Experimental 표지는 레거시가 아니므로 범위에 넣는다(결정 MD11).
  - UCLASS `Experimental` 지정자: `LayerStack.h:30`, `MotionVectorWorldOffsetOutput.h:11`, `TemporalResponsivenessOutput.h:10`, `VolumetricAdvancedMaterialInput.h:24`, `VolumetricAdvancedMaterialOutput.h:115`
  - meta `DevelopmentStatus = "Experimental"`: `SubstrateToonBSDF`(`MaterialExpressionSubstrate.h:860`)

**출력·속성.** UE 출력은 노드가 아니라 `UMaterial`의 입력이다. `EMaterialProperty`(`RE/Public/SceneTypes.h:149–190`)는 출력 노드에 대응시킨다(19.8절). `UMaterial`의 나머지 속성·사용 플래그·인스턴스 override는 에셋 필드에 대응시킨다(19.9–19.12절).

### 19.2 상태

| 상태 | 뜻 | 이 작업에서 만드는 것 | 완료 판정 |
|---|---|---|---|
| **구현** | lowering이 머티리얼 층 안에서 완결된다 | 노드 서술(핀·속성 struct·메뉴)·lowering·HLSL·시험 | 골든 원문 시험(MS-T3)과 V 규칙 음성 시험 |
| **TODO** | 머티리얼 층 자체에 선행이 없다(참조할 에셋 타입·자원 종류가 없다) | 노드 서술과 속성 struct. lowering 자리는 `PrerequisiteMissing` 진단(선행 ID 포함)을 낸다 | 진단 시험. 선행이 생기면 그 단계에서 구현으로 바꾼다 |
| **플러그인** | UE에서도 엔진 밖 모듈의 노드다 | 등록 API(`RegisterMaterialNode`)와 플러그인 등록 시험만. 노드 자체는 그 모듈이 생길 때 그 모듈이 만든다 | 등록·해제 시험(가짜 플러그인) |
| **추상** | UE 기반 클래스, 노드가 아니다 | 없다. 공통 속성은 서술 묶음 규칙으로 나눈다 | — |
| **공통** | NodeGraph 공통 기능이 대신한다 | 없다(NodeGraph에 있다) | — |
| **제외** | deprecated | 없다 | — |

**속성 TODO.** 노드 자체는 구현이고 속성 하나가 없는 에셋을 참조하는 경우가 있다. 예: `SurfaceSlab`의 subsurface·specular profile, `Aggregate`의 UserDefined 종류. 이런 노드는 상태가 "구현"이고 표의 "공급·선행" 열에 `속성 TODO: MPQ-…`로 적는다. 그 속성을 비워 두면 정상 번역된다. 값을 넣으면 lowering이 그 선행 ID의 `PrerequisiteMissing`을 낸다.

**"구현"과 공급자는 다르다.** 렌더러가 채우는 값을 읽는 노드도 있다(시간·카메라·정점 색·scene texture·파티클 값 등). 이런 노드는 lowering이 문맥 값을 읽는 원문을 내면 머티리얼 쪽이 끝난다. 값을 채우는 것은 pass 원문과 정점 입력 타입의 일이다(7.3절 "채우는 쪽", 19.3절). 그래서 공급자가 없다고 노드를 TODO로 두지 않는다. 표의 "공급·선행" 열 키는 세 종류다.
- 소문자 키: 문맥 공급 기능(19.3절)
- `src:` 키: 원본 공급 interface(19.4.2절)
- `MPQ-` 키: 머티리얼 층 선행(19.4.1절)

#### 19.2.1 TODO 표기와 진단 규칙(결정 MD12)

- 코드의 TODO는 `// TODO(MPQ-<이름>): <무엇이 생기면 무엇을 바꾸는가>` 한 형식만 쓴다. 이름은 19.4.1절 표의 ID다. 다른 모양의 TODO는 쓰지 않는다.
- TODO 노드와 속성 TODO의 lowering은 `MaterialDiagnosticCode::PrerequisiteMissing`과 선행 ID를 담은 진단을 노드 위치에 낸다(5.4절 진단 형식). 0·검은색 같은 대체 값을 내지 않는다(계획 11.1절 "검은 표면으로 처리하지 않는다").
- **쓰지 않는 노드로는 막지 않는다.** `PrerequisiteMissing`과 V12(모르는 노드)는 다음 순서를 지나서도 살아 있는 노드에만 낸다.
  1. static 해석: static 파라미터, 품질 수준, 형식 등급 스위치(19.3.4절)
  2. DCE: 진입점에서 닿지 않는 값 제거
  - 조건 값(Render Path·Capability Tier·pass 조건 스위치)의 한 가지에만 있는 노드도 있다. 이런 노드는 쿠킹하는 (형식, pass) 조합이 그 가지를 고를 때만, 조합별 진단(V9과 같은 방식)으로 낸다.
  - 그래서 쿠킹하지 않는 품질 수준이나 Mobile 가지에 있는 TODO 노드는 데스크톱 쿠킹을 막지 않는다.
  - 에디터는 모든 가지의 진단을 "경고(이 조합에서만 오류)"로 보인다. Quality Switch의 검증용 전체 입력 컴파일(UE `MaterialExpressions.cpp:9088–9102`)도 같은 등급이다.
- 서술 표(MaterialSchema)의 노드 정의는 상태(`Implemented`·`PrerequisiteMissing`)와 선행 ID를 데이터로 가진다(`MaterialNodeDefinition`).

#### 19.2.2 단일 원본과 대조

- **기계 판독 목록** `Engine/Source/Runtime/MaterialSchema/MaterialNodeCatalog.json`이 UE 클래스 → Rocks 노드 대응의 단일 원본이다. 항목마다 UE 클래스, 노드 id, 상태, 단계, 범주, 공급 기능, 원본 interface, 선행, 속성 선행을 담는다. 키와 값은 영어 식별자만 쓴다(빌드 설정 영어 규칙).
- 19.6절 표는 이 파일에서 만든다. 비고 열만 문서에서 쓴다.
- **대조.**
  - (a) 시험 MS-T12: 등록된 노드 정의(id·상태·선행·범주)가 목록 파일의 구현·TODO 행과 같다. 목록은 시험 데이터로 빌드에 넣는다.
  - (b) policy 검사 세 가지:
    - 문서 표의 (UE 클래스, 노드, 상태, 단계, 공급·선행) 열이 목록 파일과 같다.
    - 코드의 TODO 표기(주석 첫머리의 TODO, 괄호·쌍점이 붙은 `TODO(`·`TODO (`·`TODO:`)가 `TODO(MPQ-<이름>)` 형식이고, 이름이 19.4.1절 표에 있다. 문장 가운데의 낱말 TODO("상태가 TODO 노드")는 산문이라 보지 않는다.
    - 19.4.1절의 MPQ ID마다 코드 표기나 노드가 하나 이상 있다.

### 19.3 문맥 공급

UE는 렌더러가 공급하는 값을 세 가지로 다룬다.
- `FMaterialPixelParameters`·`FMaterialVertexParameters` 필드
- `USES_*`·`NEEDS_*` 환경 정의
- 외부 코드 선언: 선언마다 허용 단계·영역·미분을 데이터로 가진다(`CFG/BaseMaterialExpressions.ini:1038–`).

Rocks는 이것을 두 층으로 나눈다.
- **문맥 공급 기능**(`MaterialContextFeature`, 24개): 공급 단위다. 렌더러 쪽 누가 채우는가를 나타낸다.
- **문맥 값**(`MaterialContextValue`): 노드가 읽는 값 하나(필드 또는 함수)다. 허용 영역·단계·최소 기능 등급·미분 속성을 가진다.

#### 19.3.1 기능과 공급 판정

- **공급자는 pass와 정점 입력 타입 둘이다.**
  - pass 셰이더 타입이 채우는 기능: view·scene·decal·post 등
  - 정점 입력 타입이 채우는 기능: vertex·skin·particle·speedtree·primitive의 instance 부분
  - 둘 다 공급 기능 집합을 선언한다(16절 MRD13·MRB9). 판정 단위는 파이프라인(VS 타입, PS 타입, 정점 입력 타입)이고, 공급 집합은 pass ∪ 정점 입력이다.
- **진입점·단계별 사용 집합.** 생성 파일은 진입점(7.3절)마다 쓰는 기능을 고정된 정의로 낸다(`MATERIAL_<진입점>_USES_<기능> 1` — 쓰는 쌍만 내고 정의 없는 이름은 전처리에서 0, 예: `MATERIAL_SURFACE_USES_PARTICLE` — 6.9절). 정의는 머티리얼 번역 결과 하나에 대해 고정이다. pass가 어떤 진입점을 부르든 바뀌지 않으므로 셰이더 집합 id(8.6절)가 하나로 유지된다.
  - pass는 자기가 부르는 진입점들의 합집합으로 필요한 기능을 계산한다(`MATERIAL_CONTEXT_<기능>`은 `MaterialContext.rsh`가 pass의 공급 표지 `MATERIAL_SUPPLY_<기능>`과 이 합집합으로 정의한다 — 6.9절).
  - 픽셀 진입점이 쓰는 정점 기원 값(정점 색·파티클 값 등)은 정점 단계가 보간값으로 넘긴다. 이 보간 벡터는 V9 보간 예산에 들어간다. UE도 같다: 기능별 단계 정의(`NEEDS_PARTICLE_COLOR` Pixel|Compute, `CFG/BaseMaterialExpressions.ini:1056`), 보간 정의(`INTERPOLATE_VERTEX_COLOR`, `:1082`).
- **공급하지 않은 값(결정 MD15).** UE는 공급하지 않은 문맥 필드를 0으로 두고, 파티클 색만 1로 둔다(`SH/Private/MaterialTemplate.ush:665–670, 761`). 그래서 UE 머티리얼은 모든 정점 입력 타입에서 컴파일된다. Rocks도 같게 한다.
  - 문맥 값 표(19.3.2절)의 값마다 UE 기본값을 둔다. 공급하지 않는 파이프라인에서는 그 기본값을 쓴다.
  - 오류(V5·V6)가 되는 것은 UE가 오류로 거부하는 조건뿐이다: 허용 영역·블렌드 모드·단계·기능 등급·렌더 경로. 예: 불투명 Surface에서 scene color, Surface에서 decal 값.
  - 계획 11.1절 "검은 표면 금지"는 셰이더·머티리얼을 통째로 잃을 때의 대체 규칙이다. UE 기본값과 같은 문맥 값의 기본값과는 다른 문제다.
  - 기본값을 쓰는 조합은 쿠킹 보고서에 정보로 남긴다(머티리얼, 파이프라인, 기능).

#### 19.3.2 문맥 값 표

- 표는 MaterialSchema의 데이터(`MaterialContextValueTable`)다. UE 외부 코드 선언 101개와 템플릿 필드(위치·기저·UV·정점 색 등)를 모두 담는다.
- 항목마다 담는 것: Rocks 이름, 반환 타입, 필드인지 함수인지와 함수 인자, 공급 기능, 허용 영역·단계, 최소 기능 등급, 미분(Zero·Valid), 공급하지 않았을 때의 UE 기본값, previous 변형(19.3.5절).
- 함수형 값(대기·DOF·노출 역함수·sky light 표본 등)은 렌더러 자원(LUT 등)을 읽는다. 그래서 pass의 렌더러 텍스처 합계(Renderer 7.4절)가 쓰는 기능에 따라 달라진다. 이 계약은 Renderer가 기능별 자원 수를 선언하는 것으로 받는다(MRD13).
- **정본은 IR이다.** 노드가 어느 문맥 값을 읽는지는 lowering이 IR extern 값으로 기록한다(6.4절 capability). 다른 노드가 끌어오는 값(예: `SphericalParticleOpacity`·`DepthFade`가 쓰는 scene depth)과 속성에 따라 달라지는 값(예: `TransformVector`의 instance·view·periodic 공간)도 여기서 잡힌다. 19.6절 표의 "공급·선행" 열과 노드 정의의 `features`는 설명이다. MS-T13 fixture에 위 사례를 넣는다.
- 허용 영역·단계는 값 단위로 UE와 같게 한다. 기능 묶음으로 뭉뚱그리지 않는다. 예:
  - `LightVector`: LightFunction·DeferredDecal(`:1049`)
  - `IsFirstPerson`: Surface·Volume(`:1047`)
  - `SkyLightEnvironmentSample`: Surface만(`:1107`)
  - `PreSkinned*`: 정점 단계 전용
  - `LightmapUvs`·`PrecomputedAoMask`: 최소 SM5(`:1050, 1073`)

#### 19.3.3 대표 노멀(PixelNormal 규칙)

UE `PixelNormalWS`는 `MP_Normal` 출력을 읽는다. Normal 입력 경로에서 쓰면 순환 오류다(`HLSLMaterialTranslator.cpp:11784–11787`, `MaterialIRValueAnalyzer.cpp:395–399`). Rocks는 Normal 출력 핀 대신 BSDF 노드의 Normal 핀을 쓴다(10.3절). 그래서 다음을 정한다.
- **대표 노멀**은 closure 하나로 접은 표면(10.3절)의 노멀이다. 접기 규칙에서 shading model을 정하는 BSDF(최상위 slab 등)의 Normal 입력이고, 연결이 없으면 정점 노멀이다.
- `PixelNormal`·`ReflectionVector`(CustomWorldNormal 미연결)·`Fresnel`(Normal 미연결 때 PixelNormal — UE `MaterialExpressions.cpp:11940`)은 대표 노멀을 읽는다.
- 이 값들이 닿으면 순환 오류(V2)인 경로: 모든 BSDF 노드의 Normal·Tangent 핀, `ClearCoatNormalOutput`·`TangentOutput`·`BentNormalOutput`.

#### 19.3.4 스위치 해석 시점

UE는 형식·플랫폼에 따라 갈리는 스위치를 플랫폼별 번역 때 정적으로 푼다(RequiredSamplers `MaterialExpressions.cpp:9784–9797`, DataDrivenShaderPlatformInfo `:9405–9416`, Bindless `:9527–9535`, DistanceFields `:12603–`, VirtualTexture `:3844–`). Rocks의 처리 방식은 다음과 같다.

| 종류 | 노드 | 해석 | 번역 키 |
|---|---|---|---|
| 번역 상수 | Static Switch·static 파라미터·Quality Switch | 번역 때(6.3절 2항) | static 값·품질 수준 |
| **형식 등급** | `FormatFactSwitch`·`SamplerBudgetSwitch`·`BindlessFeatureSwitch` | 번역 때. 결과가 형식에 따라 달라지면 형식 등급(같은 답을 내는 형식 묶음)마다 번역한다 | 그 머티리얼이 쓰는 형식 사실의 값 집합(쓰지 않으면 키에 없음) |
| **프로필 기능** | `DistanceFieldFeatureSwitch`·`VirtualTextureFeatureSwitch` | 번역 때 | 프로필의 그 기능 값 |
| 기능 등급 | `CapabilityTierSwitch` | 조건 값(`#if`). 품질 Low면 Mobile 입력(UE `:9224–9229`) | — |
| 렌더 경로 | `RenderPathSwitch` | 조건 값(6.3절 3항) | — |
| **pass 조건** | `ShadowPassSwitch`·`GlobalIlluminationPassSwitch`·`LightBakePassSwitch`·`ProxyBakePassSwitch`·`VirtualizedGeometryPassSwitch`·`RuntimeVirtualTexturePassSwitch`·`RayTracingQualitySwitch`·`PathTracing*Switch`·`StageSwitch`·`PreviousFrameSwitch` | pass·진입점 정의로 고르는 조건 값 | — |
| **실행 중 값** | `ReflectionCaptureSwitch`(view 값, UE `Common.ush:2306–2309`), `MeshPaintTextureSwitch`(primitive 값, `HLSLMaterialTranslator.cpp:9142–9144`), `DynamicBranch` | 동적 분기 | — |

- 형식 등급 번역을 따로 두는 것은 결정 MD4의 "생성 원문은 모든 형식에 하나"의 좁은 예외다.
  - 형식 스위치를 `#if`로 남기면 두 가지가 모두 자원 layout에 남는다. 그러면 `SamplerBudgetSwitch`가 GLES 예산을 줄이지 못한다(MS-T10의 목적과 반대).
  - 이 노드를 쓰지 않는 머티리얼은 번역이 하나다.
- `RayTracingQualitySwitch`는 ray tracing pass와 GI 표면 캐시 capture pass에서 RayTraced 입력을 고른다(UE `Common.ush:2171–2177`, `LUMEN_CARD_CAPTURE`).

#### 19.3.5 이전 프레임 평가

- UE는 이전 프레임을 번역할 때 Time·ViewProperty·변환 값을 Prev 값으로 바꾼다(`bCompilingPreviousFrame`, `HLSLMaterialTranslator.cpp:5257, 5280, 11176`). DeltaTime은 바꾸지 않는다(`:5327`).
- Rocks는 진입점 문맥(현재·이전)을 lowering 문맥으로 둔다. 문맥 값 표에서 previous 변형이 있는 값은 이전 진입점(`EvaluateMaterialVertexPrevious`)에서 그 변형을 읽는다. `PreviousFrameSwitch`는 진입점에서 푼다.
- hash-consing 키에 진입점 문맥이 들어가므로 현재·이전 값이 섞이지 않는다(6.2절).
- 이전 값은 공급 기능 `view`·`primitive`의 previous 부분이다. Velocity pass가 그 공급을 선언한다(MRD13).

### 19.4 선행

#### 19.4.1 머티리얼 층 선행(TODO 노드·속성 TODO·필드 TODO)

| ID | 없는 것 | 막는 것 | 생기는 곳 |
|---|---|---|---|
| `MPQ-TextureCollection` | 텍스처 컬렉션 에셋과 자원 바인딩 종류 | `TextureCollection`·`TextureCollectionParameter`·`TextureObjectFromCollection` | AssetSystem 12절 텍스처 후속 |
| `MPQ-SparseVolumeTexture` | sparse volume texture 에셋·자원 종류 | `SparseVolumeTexture*` 4개 | 텍스처 후속 |
| `MPQ-VirtualTexture` | 가상 텍스처 page table 바인딩(RVT·메시 페인트·머티리얼 캐시 공용) | `RuntimeVirtualTextureSample`·`…SampleParameter`, `MeshPaintTextureObject`, `MaterialCacheOutput`, 속성 `relaxVirtualTextureRestrictions` | 가상 텍스처 단계(TextureStreaming 후속) |
| `MPQ-ExternalTexture` | 외부(영상) 텍스처 자원 종류 | 핀 타입 `TextureExternal`(UE `MCT_TextureExternal`) | 미디어 단계 |
| `MPQ-Font` | 미리 구운 Font 에셋·페이지 텍스처 | `FontSample`·`FontSampleParameter` | [글꼴](../../Font/07-Consumers.ko.md) FT7 |
| `MPQ-MaterialAggregate` | aggregate 에셋(UE `UMaterialAggregate`, `RE/Public/Materials/MaterialAggregate.h:83`)과 IR aggregate 타입 | `Aggregate` 속성(UserDefined) | MS2e 뒤 |
| `MPQ-SubsurfaceProfile` | subsurface profile 에셋(UE `RE/Classes/Engine/SubsurfaceProfile.h:177`) | `SurfaceSlab`·`SurfaceEye`·`SurfaceFromShadingModel`·`SurfaceFromAttributes` 속성 | Renderer SSS 단계(RS9) |
| `MPQ-SpecularProfile` | specular profile 에셋(`SpecularProfile.h:73`) | `SurfaceSlab` 속성 | Renderer 단계 |
| `MPQ-ToonShading` | Toon shading model(Renderer 14.2절 목록에 없음)과 toon profile 에셋(`ToonProfile.h:159`) | `SurfaceToon` | Renderer(16절 MRD14) |
| `MPQ-NeuralProfile` | neural profile 에셋(`NeuralProfile.h:140`) | 머티리얼 속성 `neuralProfile`(19.9절) | 추론 백엔드 |
| `MPQ-PhysicalMaterial` | 물리 재질·마스크 에셋(PhysicsModule) | 머티리얼 속성 `physicalMaterial`·`physicalMaterialMask`·`renderTracePhysicalMaterials` | [물리 모듈](../../PhysicsModule/README.ko.md) |
| `MPQ-VirtualizedGeometry` | 가상화 geometry·테셀레이션 pass | 속성 displacement·tessellation·override 머티리얼(19.9절) | 후속(스펙 없음) |
| `MPQ-ColoredTransmittance` | dual-source blending 반투명 경로 | 블렌드 모드 `TranslucentColoredTransmittance` | Renderer RS5 뒤 |
| `MPQ-AlphaHoldout` | holdout pass | 블렌드 모드 `AlphaHoldout`(10.3절) | Renderer 후속 값(14.2절) |
| `MPQ-LightBake` | 정적 조명 굽기 도구 | 머티리얼 속성 `lightBakeSettings`·인스턴스 `lightBakeOverrides`(19.11·19.12절) | 조명 굽기 단계(스펙 없음) |
| `MPQ-NoiseTexture` | 렌더러 noise 시스템 텍스처(UE View PerlinNoiseGradientTexture·PerlinNoise3DTexture) | `Noise` 함수 SimplexTex·GradientTex·GradientTex3D(6.13절 MS2f3) | 렌더러 시스템 텍스처(렌더 세션) |

#### 19.4.2 원본 공급 interface(`src:`)

함수 호출·레이어·다른 머티리얼 참조·컬렉션·커브를 쓰는 노드는 번역 입력으로 다른 에셋의 snapshot이 필요하다(6.3절 1항). 에셋 클래스는 MS3·MS6·MS7에서 생기지만, 번역은 에셋 객체가 아니라 snapshot만 본다. 그래서 MaterialCompiler는 snapshot을 받는 interface를 둔다.

```text
MaterialSourceProvider
  FindFunction(AssetId)     → MaterialGraphSnapshot | NotFound
  FindMaterial(AssetId)     → MaterialGraphSnapshot | NotFound   (MaterialSample)
  FindCollection(AssetId)   → ParameterCollectionLayout | NotFound
  FindColorCurve(AssetId)   → MaterialColorCurveSnapshot | NotFound (ColorRamp — 6.13절 MS2f4)
```

lowering(펼치기·layout 결합·커브 구간식)은 이 interface 위에서 구현한다. 시험은 메모리 provider로 한다. 컬렉션은 MS2c에서 번역 입력의 값 목록(`collections` — 6.10절)으로 구현했으므로 `FindCollection`은 두지 않는다(6.12절 MS2e2). 에셋 객체를 snapshot으로 바꾸는 쪽은 해당 에셋 클래스 단계에서 붙인다. 그래서 `src:` 노드는 "구현"이다.

#### 19.4.3 노드 속성의 에셋 참조 표현

노드 속성의 에셋 참조는 지금 정한 한 형식 `MaterialAssetReference { assetId : AssetId, expectedClass : Name }`으로 저장한다(4.3절 "AssetId로 저장").
- `expectedClass`는 기대 클래스의 도구 이름이다(예: `"Texture2D"`, `"SubsurfaceProfile"`).
- 참조 대상 클래스가 아직 없어도 형식은 같다. 그래서 그 클래스가 생겨도 속성 struct의 schemaVersion을 올리거나 migration할 필요가 없다.
- 번역은 snapshot provider로 대상을 찾는다. 클래스가 아직 없으면 그 속성은 속성 TODO 진단이 된다.

### 19.5 이름 규칙

- 노드 종류 id는 `Material.<이름>`이다(NodeGraph 권장 형식). 플러그인 노드는 그 모듈 이름을 접두로 쓴다(`Landscape.LayerBlend`). 19.6.29절 접두는 가칭이다. 모듈 스펙이 Rocks 모듈 이름을 정하면 그 이름을 쓴다.
- id는 PascalCase이고 약어도 단어로 쓴다(`HsvToRgb`·`Ddx`·`ParticleSubUv`·`DecalBufferTexture`).
- UE 접두 `MaterialExpression`은 뗀다. UE 좌표 접미 `WS`도 뗀다. Rocks 노드의 world 공간은 기본이고, 공간을 고를 수 있는 노드는 속성으로 고른다(`CameraPositionWS` → `CameraPosition`).
- UE 제품·기능 상표는 기능 이름으로 바꾼다.
  - Substrate → `Surface…`
  - Nanite → `VirtualizedGeometry…`
  - Lightmass → `LightBake…`
  - Lumen card → `SurfaceCache…`
  - Paper2D → `Sprite2D`
- UE 내부 용어도 Rocks 용어로 바꾼다.
  - ShadingPath → `RenderPath`
  - FeatureLevel → `CapabilityTier`
  - DataDrivenShaderPlatformInfo → `FormatFact`
  - RequiredSamplers → `SamplerBudget`
  - DBuffer → `DecalBuffer`
  - LWC(TruncateLWC) → `WorldPosition`(`TruncateWorldPosition`)
- 업계 공용어는 그대로 쓴다: HSV·DDX·Fresnel·Sobol·blue noise·MaterialX. `SpeedTree`는 제품명이지만 그 데이터 형식(바람·LOD 정점 데이터)의 이름이라 그대로 쓴다. FBX를 형식 이름으로 쓰는 것과 같다.
- `…Replace` 노드는 하는 일에 따라 이름을 바꾼다.
  - pass 조건이면 `…PassSwitch`: `ShadowReplace` → `ShadowPassSwitch`
  - 프로필 기능이면 `…FeatureSwitch`: `DistanceFieldsRenderingSwitch` → `DistanceFieldFeatureSwitch`
  - 실행 중 값이면 `…Switch`: `MeshPaintTextureSwitch`
- 표시 이름(메뉴)은 영어다. 핀 키(`PinKey`)는 UE 입력 이름을 기본으로 쓴다. UE 에셋 변환(UnrealExchange)이 핀을 이름으로 잇기 때문이다. 예외는 노드 정의 주석에 적는다.
- UE 클래스 이름은 19.6절 표, 19.9–19.12절 대응표, 근거에만 쓴다.

### 19.6 노드 표

[노드 표 1](NodeTableCore.ko.md)·[노드 표 2](NodeTableRendering.ko.md)다. 열은 다음과 같다.
- **단계**: 15.2절 단계(MS2a–MS2f).
- **공급·선행**: 19.2절의 키.
- **비고**: UE와 다른 점과 근거.

| 상태 | 수 |
|---|---|
| 구현 | 282 |
| TODO | 15 |
| 플러그인 | 45 |
| 추상 | 12 |
| 공통 | 4 |
| 제외 | 7 |
| 합계 | 365 |

### 19.7 플러그인 노드 등록

UE도 Landscape·MaterialX·Paper2D 노드는 엔진 머티리얼 모듈 밖에 있다. Rocks도 같다(엔진 층에는 핵심만, 결정 MD14).
- 플러그인 모듈은 `RegisterMaterialNode(정의 묶음, lowering 표, module lease)`로 등록한다(계획 14.6절). 정의와 lowering은 같은 lease가 지킨다. 해제 drain은 NodeGraph 등록표 규칙(노드 그래프 10절)을 따른다.
- 모듈이 없는 동안 그 노드가 든 그래프(예: UE 변환 결과)는 모르는 노드로 속성 바이트를 보존한다(노드 그래프 GD14). static 해석·DCE 뒤에도 살아 있으면 V12 쿠킹 오류다(19.2.1절).
- MaterialX 노드는 UE에서도 MaterialX 임포터만 만드는 노드(`meta=(Private)`)다. MaterialX 임포터가 생기면 그 모듈이 등록한다.

### 19.8 출력 속성 대응

5.6절 출력 노드의 Surface 영역 핀과 UE `EMaterialProperty`의 대응이다.
- 표면 속성 값(BaseColor·Metallic·Specular·Roughness·Anisotropy·Normal·Tangent·Subsurface·ClearCoat 두 값·Emissive)은 출력 노드에 고전 입력으로 두지 않는다. `Surface` 입력의 BSDF 노드 핀이다(결정 MD1, 10절).
- 고전 입력 그래프는 `SurfaceFromShadingModel`·`SurfaceFromAttributes`가 받는다.

| UE `EMaterialProperty` | Rocks 출력 노드 핀 | 상태 |
|---|---|---|
| `MP_FrontMaterial` | `Surface` | 구현(MS2d) |
| `MP_Opacity`·`MP_OpacityMask` | `Opacity`·`OpacityMask` | 구현(MS2a) |
| `MP_WorldPositionOffset` | `WorldPositionOffset` | 구현(MS2a) |
| `MP_AmbientOcclusion` | `AmbientOcclusion` | 구현(MS2a) |
| `MP_PixelDepthOffset` | `PixelDepthOffset` | 구현(MS2a) |
| `MP_CustomizedUVs0–7` | `CustomizedUv0`–`7` | 구현(MS2b) |
| `MP_Refraction` | `Refraction` | 구현(MS2d). 공급은 `scene`(반투명 RS5) |
| `MP_SurfaceThickness` | `SurfaceThickness` | 구현(MS2d) |
| `MP_Displacement` | `Displacement` | 구현(MS2d). 쓰는 pass(가상화 geometry·테셀레이션 — `MPQ-VirtualizedGeometry`)가 생기기 전에는 정보 진단 |
| `MP_ShadingModel` | `ShadingModel` 노드 → `SurfaceFromShadingModel` | 구현(MS2d) |
| `MP_MaterialAttributes` | `Attributes`(속성 `useMaterialAttributes`일 때) | 구현(MS2e) |
| `MP_CustomOutput` | 추가 출력 노드(19.6절 "추가 출력") | 구현(MS2d) |
| 고전 표면 값 11개 | `Surface` 아래 BSDF 핀 | 구현(MS2d) |
| `MP_DiffuseColor`·`MP_SpecularColor` | 없음(조명 굽기 도구가 IR에서 유도) | UE 내부 값 |
| `MP_WorldDisplacement_DEPRECATED`·`MP_TessellationMultiplier_DEPRECATED` | 없음 | 제외(deprecated) |

### 19.9–19.12 머티리얼 속성·사용 플래그·인스턴스·플랫폼 설정

[머티리얼 속성 대응](MaterialProperties.ko.md)이 정한다.
- `UMaterial` 속성 167개 → `Material` 필드(`traits` 등)
- 사용 플래그 28개 → 사용 대상
- `UMaterialInterface`·`UMaterialInstance`·base property override 속성 → `MaterialBase`·`MaterialInstance` 필드
- custom attribute 등록
- 플랫폼 품질 override와 조명 굽기 설정

### 19.13 노드 정의 코드(MS1b)

[노드 정의 코드](NodeDefinitionCode.ko.md)다: 파일 배치, UE 속성·핀·메뉴 변환 규칙, 동적 핀의 속성 읽기, 문맥 값 표 형식, MS-T12 (a) 시험.
