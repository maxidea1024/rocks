# 머티리얼 시스템(MaterialSystem) 설계

기준일: 2026-11-18, 개정 1(초안). [엔진 구현 계획](EngineImplementationPlan.ko.md) 14.1·14.4–14.9절의 머티리얼 요구를 구현 스펙으로 정한다. 범위는 다음과 같다.
- 머티리얼 에셋 모델(머티리얼·인스턴스·함수·레이어·파라미터 컬렉션)과 쿠킹 데이터
- 그래프 스키마, typed IR, IR에서 HLSL을 만드는 코드 생성
- 퍼뮤테이션, 파라미터 layout과 상수 식, 인스턴스 override와 static 변형
- 표면 출력 모델(shading model·Substrate 대응)과 렌더러에 넘기는 계약
- 비동기 컴파일·대체 머티리얼·미리보기·hot reload, 쿠킹·에셋 너겟·실행 시 로드
- backend 제약, 에디터가 요구하는 런타임·도구 기능

머티리얼 에디터 화면(그래프 편집 UI)은 에디터 단계의 일이라 범위 밖이다. 이 문서는 에디터가 쓸 런타임·도구 기능만 요구로 적는다(14절).

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론) 원본이다. 경로 약어는 다음과 같다.
- `RE/` = `Engine/Source/Runtime/Engine/`
- `RR/` = `Engine/Source/Runtime/Renderer/`
- `RC/` = `Engine/Source/Runtime/RenderCore/`
- `ED/` = `Engine/Source/Editor/`
- `SH/` = `Engine/Shaders/`
- `CFG/` = `Engine/Config/`
- 그 밖은 `Engine/Source/` 기준으로 적는다.

확인하지 못한 사실은 "미확인"으로 적는다. 근거 없이 채운 항목은 없다.

**관계 문서.**
- [셰이더 시스템](ShaderSystem.ko.md)(개정 11): 원문 규약·바인딩 규약 7.2절·job 키·셰이더 타입과 퍼뮤테이션(P14)·파라미터 구조체(P7)·라이브러리. 이 문서는 그 위에 머티리얼이 만드는 원문과 layout을 얹는다. 충돌은 그 문서의 결정을 따르고, 고칠 곳은 16절에 모은다.
- [렌더 기반](RenderBase.ko.md)(개정 9): 셰이더 집합·바인딩 지도·상수 블록·공유 상수 slot·파이프라인 캐시·정점 입력 타입.
- [에셋 시스템](AssetSystem.ko.md)(개정 8): 에셋·참조 종류·에셋 너겟 키·Cooker.
- [텍스처 스트리밍](TextureStreaming.ko.md)(개정 3) 결정 T4: 재질 sampling scale 공급.
- [Renderer](Renderer.ko.md)(개정 2, `feature/renderer-spec` 2dc9068, 미병합): GBuffer 배치·조명 평가·pass 목록과 pass 셰이더, 그리고 14절 "렌더러가 머티리얼에 요구하는 계약"(`SurfaceAttributes`, shading model·블렌드 모드·영역 번호표, pass별 퍼뮤테이션, 경계표). 이 문서는 그 계약에 맞춘다(10.1절). 다른 점은 10.4절에 모았다. 이 문서에서 "Renderer 스펙"이라 부른다.

**용어 방침.** UE 구조는 따르되 이름은 Rocks 이름을 쓴다. UE의 클래스·함수·CVar 이름은 대응표(2절)와 "UE 5.8 방식" 표·근거에만 적는다. shader·pass·BSDF·closure·GBuffer·uniform·sampler·permutation처럼 업계에서 함께 쓰는 말은 그대로 쓴다. 계획 문서에 이미 있는 Rocks 이름(MaterialSchema·MaterialCompiler·MaterialRuntime·MaterialCook·CompileCoordinator·PreviewSession)은 그대로 쓴다.

**UE 레거시 제외.** UE가 하위 호환을 위해 남긴 경로는 따르지 않는다(사용자 원칙). 다만 UE 5.8은 "이름은 legacy이지만 기본값인" 경로를 갖고 있어(6.1절 번역기, 10.2절 shading model) 조사에서 기본 여부와 레거시 표시를 따로 확인했다.

상태 표기는 AssetSystem과 같다: **구현**(동작·시험), **계약**(API·형식이 있고 미지원 값은 명시 오류), **후속**(API도 없음). 이 문서 전체는 **설계**이며 아무것도 구현되지 않았다.

## 1. 결정 요약

"결정 MDn"은 17절 항목이다. 2026-11-18 사용자 지시("결정사항들은 권장안대로 하면됨")로 MD1–MD9를 확정했다. MD4는 같은 날 확정된 Renderer 결정 RN13에 맞췄다(17절).

| 항목 | 결정(권장안 포함) | 근거 |
|---|---|---|
| 컴파일 구조 | 그래프 → **typed IR(SSA DAG)** → 분석·최적화 → HLSL. 노드가 HLSL 조각을 직접 내는 번역기는 두지 않는다(결정 MD2) | UE 5.8 기본은 노드가 코드를 직접 내는 번역기다(`RE/Private/Materials/MaterialShared.cpp:3683–3704`). IR 번역기는 실험이다(`r.Material.Translator.EnableNew` 기본 false, "prototype (WIP)", `:108–112`). Rocks는 HLSL 말고도 IR 소비자가 있다(계획 14.10절 CPU bake target `BakeMaterialProfile`, M8 `RayMaterialProfile`) |
| IR 성질 | 값 해시 공유(hash-consing)·빈도 분석(상수·static·uniform·정점·픽셀)·결정적 번호 매김. 같은 그래프·같은 도구 빌드는 같은 바이트 | UE IR도 값 해시 공유를 쓴다(`RE/Public/Materials/MaterialIR.h:97–118`). 결정성은 AssetSystem 10절 규칙 |
| 표면 출력 모델 | **Substrate형 BSDF 그래프**를 closure 하나로 접어 Renderer 스펙 `SurfaceAttributes`(고전 필드 + shading model 번호)로 내보낸다. shading model은 BSDF로 내려가는 preset 노드다. 여러 closure는 계획 14.4절 연구 gate(결정 MD1) | UE 5.8 새 프로젝트는 Substrate·Blendable GBuffer가 켜진다(`ED/GameProjectGeneration/Private/GameProjectUtils.cpp:199–211`). Blendable은 closure 1개를 강제한다(`RC/Private/RenderUtils.cpp:2328–2373`). Rocks 형식 가운데 Adaptive 대상(UE는 SM6 계열만)은 `D3D12_SM6`뿐이다(10.2절) |
| 렌더러 경계 | 머티리얼은 `EvaluateMaterialSurface(MaterialInputs) → SurfaceAttributes`와 정점 함수(WPO 현재·이전)를 생성한다. `SurfaceAttributes` 필드, shading model·블렌드 모드·영역 번호표, GBuffer 인코딩, 직접광 평가, pass 원문은 Renderer 스펙이 소유한다(Renderer 결정 RN5) | Renderer 14.1·14.2·14.4·14.6절, 계획 11.1절 표 "공통 Material IR에서 GBuffer 출력 / 같은 IR에서 direct lighting·surface 평가" |
| 생성 원문 연결 | 텍스트 템플릿 치환을 하지 않는다. 생성 파일 `/Engine/Generated/Material.rsh`(job마다 연결)를 정적 템플릿 `MaterialCommon.rsh`가 include한다 | UE는 `%{name}` 문자열 템플릿을 채운다(`RE/Private/Materials/StringTemplate.h:13–29`, `MaterialSourceTemplate.h:12–52`). Rocks는 정점 입력 타입의 고정 가상 경로 방식(RenderBase 10.2절)과 같게 한다 |
| 파라미터 식별 | 파라미터는 이름이 아니라 (정의한 에셋, 안정 `ParameterId`(Guid), scope 경로(전역·함수 호출 위치·레이어 Guid))로 식별한다. 이름 바꾸기·레이어 순서 바꾸기에도 override가 남는다 | 계획 14.7절. UE는 이름+연관+레이어 index로 식별하고 index를 다시 매긴다(`RE/Public/Materials/MaterialParameters.h:33–46, 78, 134`) |
| 상수 식 | uniform 빈도 식은 CPU **상수 식 프로그램**으로 내보내고 렌더 lane이 상수 블록을 채운다 | UE preshader(`RE/Public/Shader/Preshader.h:19–75`, 평가 `RE/Private/Materials/MaterialRenderProxy.cpp:458`) |
| 인스턴스 해석 | override는 로드·편집 때 부모 사슬을 평탄화한 값 배열로 만든다. 그리기 경로에서 부모를 거슬러 찾지 않는다 | UE는 렌더 스레드에서 부모 proxy로 재귀한다(`RE/Private/Materials/MaterialInstance.cpp:371–395`) |
| static 변형 | static 파라미터·기본 속성 override를 가진 인스턴스는 자기 static 변형을 쿠킹한다. 실행 중 만드는 인스턴스는 static 값을 바꿀 수 없다(`UnsupportedSetting`) | UE `bHasStaticPermutationResource`(`RE/Private/Materials/MaterialInstance.cpp:2415–2424`), 변형 갱신은 에디터 전용(`RE/Public/Materials/MaterialInstance.h:1042–1083`) |
| 퍼뮤테이션 축 | (머티리얼 static 변형) × (pass 셰이더 타입·pass 퍼뮤테이션) × 정점 입력 타입 × 형식 × 품질 수준. 사용 대상 선언·쿠킹 유도·형식 거르기·품질 차이 검사로 줄이고 쿠커가 수를 보고한다 | UE `FMeshMaterialShaderType` 두 단계 거르기(`RE/Private/Materials/MeshMaterialShader.cpp:178–186`), 품질 사용 검사(`RE/Private/Materials/MaterialCachedData.cpp:625–637`), 계획 14.1절 증가량 보고 |
| 파라미터 layout 형식 | MetaCompiler가 만드는 C++ 파라미터 구조체와 머티리얼 컴파일러가 만드는 **동적 layout**이 같은 `ShaderParameterLayout` 데이터를 낸다(셰이더 결정 4 권장안과 맞물림, 결정 MD3) | UE도 머티리얼 uniform buffer는 매크로가 아니라 실행 중 layout을 만든다(`RE/Private/Materials/MaterialUniformExpressions.cpp:524, 971–981`) |
| 텍스처·샘플러 한도 | 머티리얼 텍스처 상한은 (pass, 퍼뮤테이션)마다 `16 − 렌더러 합계`다. 머티리얼 컴파일러가 pass별 예산을 계산해 진단·쿠킹 검사에 쓴다(결정 MD4 = Renderer RN13) | 셰이더 시스템 7.2절 4·9항(공통 상한 16, 텍스처마다 샘플러 하나), Renderer 7.4절. UE는 공유 샘플러로 샘플러 슬롯을 아낀다(`RE/Classes/Engine/EngineTypes.h:282–293`) |
| 대체 머티리얼 | 도메인별 엔진 기본 머티리얼을 엔진 셰이더 라이브러리에 넣고 시작 때 완결성을 검사한다. 에디터는 마지막 성공 결과를 유지한다. 쿠킹 실행에서 빠진 셰이더는 기본 머티리얼 + 진단이다(검은 표면 금지) | UE `GetDefaultMaterial(Domain)`(`RE/Private/Materials/Material.cpp:1094–1109`), 기본 머티리얼은 동기 컴파일(`RE/Private/Materials/MaterialShared.cpp:2351–2354`). 계획 11.1절 "검은 표면으로 처리하지 않는다", 14.8절 "이전 검증 shader 유지" |
| 쿠킹 단위 | 두 층 너겟: **번역 너겟**(그래프 → 원문·layout·상수 식)과 셰이더 시스템 **job 너겟**. 셰이더 코드는 셰이더 라이브러리에, 패키지에는 셰이더 집합 id(번역 결과 내용 hash, 호스트 무관)만 둔다 | 셰이더 시스템 10·12.3절, RenderBase SX5(라이브러리 묶음 id). UE는 번역 결과(`"MaterialTranslation"`)와 셰이더 맵(`"MaterialShaderMap"`)을 따로 캐시한다(`RE/Private/Materials/HLSLMaterialTranslator.cpp:209`, `RE/Private/Materials/MaterialShader.cpp:320–324`) |
| 좌표·단위·행렬 | UE 5.8과 같다: 행 벡터 `mul(v, M)`·행 우선, cm, 좌표 공간 이름과 축. 머티리얼 노드의 변환도 이 규약으로 낸다 | 셰이더 시스템 7.2절 6항, 사용자 절대 규칙 |

## 2. 용어 대응

결정 MD8(확정)대로다. 영역·블렌드 모드·shading model의 이름과 번호는 Renderer 스펙 14.2절(`RendererShaders` 소유, 결정 RN5)을 따른다.

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 머티리얼 기반 클래스 `MaterialBase` | `UMaterialInterface` | 그리기에 쓸 수 있는 머티리얼 에셋의 공통 기반(추상) |
| 머티리얼 `Material` | `UMaterial` | 그래프를 가진 머티리얼 에셋 |
| 머티리얼 인스턴스 `MaterialInstance` | `UMaterialInstanceConstant` | 부모의 파라미터를 override하는 에셋 |
| 동적 머티리얼 인스턴스 `DynamicMaterialInstance` | `UMaterialInstanceDynamic` | 실행 중 만드는 인스턴스(저장하지 않음, dynamic 파라미터만) |
| 머티리얼 함수 `MaterialFunction`(용도: 일반·레이어·레이어 혼합) | `UMaterialFunction`·`UMaterialFunctionMaterialLayer`·`…LayerBlend` | 재사용 그래프 조각 |
| 머티리얼 함수 인스턴스 `MaterialFunctionInstance` | `UMaterialFunctionInstance` | 함수의 파라미터 기본값 override(주로 레이어) |
| 레이어 스택 `LayerStack` | `FMaterialLayersFunctions`·Material Attribute Layers 노드 | 레이어·혼합 함수 목록 파라미터 |
| 파라미터 컬렉션 `ParameterCollection` | `UMaterialParameterCollection` | 여러 머티리얼이 공유하는 scalar·vector 묶음 에셋 |
| 컬렉션 상태 `ParameterCollectionState` | `UMaterialParameterCollectionInstance` | World마다 가진 컬렉션 값 |
| 머티리얼 그래프 `MaterialGraph` | `UMaterial::ExpressionCollection` | 노드·간선 데이터(EditorOnly) |
| 노드 `MaterialNode`, 노드 타입 `NodeTypeId` | `UMaterialExpression`(클래스) | 그래프 노드 |
| 출력 노드 `MaterialOutput` | 머티리얼 루트 노드(`UMaterial`의 입력 핀들) | 도메인별 typed 출력 |
| 머티리얼 IR `MaterialIr` | `FMaterialIRModule`(실험) | typed SSA DAG |
| 번역 결과 `MaterialTranslation` | `FMaterialCompilationOutput` + 생성 코드 | IR에서 만든 원문·layout·상수 식·특성 |
| 상수 식 프로그램 `ConstantProgram` | preshader(`FUniformExpressionSet`) | uniform 빈도 식의 CPU 평가 프로그램 |
| 머티리얼 상수 블록 `MaterialConstants` | Material uniform buffer | 머티리얼 파라미터 값의 상수 버퍼 |
| 머티리얼 셰이더 특성 `MaterialShaderTraits` | `FMaterialShaderParameters` | pass 셰이더 거르기에 넘기는 머티리얼 요약 |
| 머티리얼 셰이더 집합 `MaterialShaderSet` | `FMaterialShaderMap`(+`FMeshMaterialShaderMap`) | 한 머티리얼 변형의 셰이더 묶음 |
| 머티리얼 산출물 `MaterialArtifact` | `FMaterialResource` | (머티리얼 변형, 품질 수준)의 쿠킹 결과와 실행 상태 |
| static 변형 `StaticVariant` | static permutation resource | static 값 조합 하나에 대한 머티리얼 산출물 |
| 머티리얼 렌더 상태 `MaterialRenderState` | `FMaterialRenderProxy` | 렌더 lane 쪽 머티리얼 상태(상수 블록·텍스처 handle) |
| 사용 대상 `VertexInputUsage` | `EMaterialUsage`(`bUsedWith*`) | 머티리얼을 쓸 정점 입력 타입 집합 |
| 정점 입력 타입 | vertex factory | RenderBase 10절 |
| 품질 수준 `MaterialQuality` | `EMaterialQualityLevel` | Low·Medium·High·Epic |
| 기능 등급 `CapabilityTier` | `ERHIFeatureLevel`(머티리얼 switch 용도) | 형식 정의가 주는 등급(Mobile·SM5·SM6) |
| 텍스처 샘플 종류 `TextureSampleKind` | `EMaterialSamplerType` | 색·선형·노멀·마스크 등 해석 방식 |
| 머티리얼 공통 원문 `MaterialCommon.rsh` | `MaterialTemplate.ush` | 생성 원문을 감싸는 정적 원문 |
| 생성 원문 `/Engine/Generated/Material.rsh` | `/Engine/Generated/Material.ush` | job마다 연결되는 생성 파일 |
| 표면 속성 `SurfaceAttributes`(Renderer 14.1절 소유) | `FPixelMaterialInputs`·GBuffer 데이터 | pass 셰이더가 읽는 픽셀 표면 값 |
| 머티리얼 입력 `MaterialInputs`·`MaterialVertexInputs` | `FMaterialPixelParameters`·`FMaterialVertexParameters` | 생성 함수가 받는 픽셀·정점 문맥 |
| 표면 층 노드 `SurfaceSlab` | Substrate Slab | 기본 BSDF 노드 |
| 표면 연산 노드 `SurfaceMix`·`SurfaceStack`·`SurfaceAdd`·`SurfaceCoverage`·`SurfaceSelect` | Horizontal Blend·Vertical Layer·Add·Coverage Weight·Select | BSDF 결합 |
| shading preset 노드 | Substrate Shading Models(legacy 변환 노드)·`EMaterialShadingModel` | 고전 shading model 입력을 BSDF로 바꾸는 노드 |
| 기본 머티리얼 `DefaultMaterial`(도메인별) | `UMaterial::GetDefaultMaterial` | 대체 머티리얼 |
| 컴파일 조정자 `CompileCoordinator`(계획 14.6) | 머티리얼 에디터 갱신 + 셰이더 맵 컴파일 | 편집 → 컴파일 요청 관리 |
| 미리보기 세션 `PreviewSession`(계획 14.6) | 머티리얼 에디터 preview·`FMatExpressionPreview` | 미리보기 렌더 |

## 3. 모듈 배치와 의존

### 3.1 UE 5.8 배치

| 역할 | UE 5.8 위치 | 근거 |
|---|---|---|
| 에셋 클래스·번역기·uniform expression·셰이더 맵 | Engine 모듈 하나(`RE/Public/Materials/`, `RE/Private/Materials/`) | `RE/Public/Materials/Material.h:448`, `RE/Private/Materials/HLSLMaterialTranslator.h:233` |
| 머티리얼 셰이더 기반 클래스·pass 셰이더 등록 | Renderer 모듈. 셰이더 타입 클래스는 Engine 모듈 | `RR/Public/MaterialShader.h:55`, `RR/Public/MeshMaterialShader.h:67`, `RE/Public/MaterialShaderType.h:89`, `RE/Public/MeshMaterialShaderType.h:25`, 등록 예 `RR/Private/BasePassRendering.cpp:180–192` |
| 템플릿 원문 | `SH/Private/MaterialTemplate.ush` | 4961줄, 생성 코드 자리 `:360–4960` |
| 에디터 화면·노드 미리보기 | MaterialEditor 모듈 | `ED/MaterialEditor/Private/MaterialEditor.h:57` |

UE는 번역기·에셋·실행 상태를 한 모듈에 두어 쿠커와 게임이 같은 코드를 링크한다. Rocks는 계획 14.6절대로 나눈다.

### 3.2 Rocks 배치(계획 14.6절 검토)

| 모듈 | 계층 | 책임 | 의존 | 금지 |
|---|---|---|---|---|
| **MaterialSchema** | Engine Runtime | 그래프 데이터 타입(`MaterialGraph`·노드·간선·핀 키), 노드 descriptor 표(데이터만), 파라미터 schema, 품질 수준·기본 속성 묶음, `MaterialShaderTraits`, semantic hash. 영역·블렌드 모드·shading model 번호표는 참조만 한다(Renderer 소유, 10.4절 D2) | Core·MetaSchema·Serialization·ShaderCore(셰이더 타입 id만)·RenderData(번호표, 10.4절 D2 제안) | RHI·Graphics·RenderBase·ObjectRuntime 객체 포인터·노드 lowering 코드 |
| **MaterialCompiler** | Engine Developer | 그래프 검증, 함수·레이어 펼치기, IR 생성·분석·최적화, HLSL 생성, 상수 식 생성, layout, 진단·원문 위치 지도 | MaterialSchema·ShaderCore·Core | 장치·프로세스 실행(셰이더 컴파일은 하지 않는다)·EditorFramework |
| **MaterialRuntime** | Engine Runtime | `MaterialRenderState`, 상수 식 실행, 상수 블록 채움, 텍스처 handle 해석, 셰이더 집합 열기, 기본 머티리얼 대체 | MaterialSchema·RenderBase·ShaderCore·GraphicsTypes | ObjectRuntime·AssetRuntime(렌더 층 "Object 없음", 계획 3.1절)·그래프·컴파일러 |
| **Material**(계획의 MaterialAssetBridge) | Engine Runtime | 에셋 클래스(`MaterialBase`·`Material`·`MaterialInstance`·`MaterialFunction`·`MaterialFunctionInstance`·`ParameterCollection`), `DynamicMaterialInstance`, 쿠킹 데이터 → `MaterialRenderState` 생성 | AssetRuntime·ObjectRuntime·MaterialSchema·MaterialRuntime | 컴파일러·셰이더 도구 |
| **MaterialCook** | Engine Developer | 번역 너겟·셰이더 job 목록·쿠킹 처리기·퍼뮤테이션 보고 | Material·MaterialCompiler·AssetCooker·AssetNugget·ShaderCompiler | Release 링크 금지 |
| **EditorMaterial** | Engine Editor | `CompileCoordinator`·`PreviewSession`·그래프 편집 명령(계획 14.6절) | 위 모듈·EditorBackend·Renderer | 화면 코드에서 shader 의미·검증 재구현 금지(계획 14.6절) |

계획과 다른 점:
- **에셋 모듈 이름.** 계획의 MaterialAssetBridge는 에셋 클래스가 사는 곳이다. 텍스처 에셋 모듈이 `Texture`(AssetSystem 3절)이므로 `Material`로 맞춘다(결정 MD8).
- **MaterialRuntime 의존.** 계획의 "Core·RenderData·RHI 계약"은 RenderBase(셰이더 집합·바인딩·상수 블록)로 바뀐다. RenderBase가 계획 뒤에 생겼기 때문이다.
- **MaterialSchema가 특성 타입을 가진다.** pass 셰이더 거르기 함수가 머티리얼 특성을 받아야 하는데, 이 거르기는 쿠커(Developer)와 게임(Runtime) 모두에서 실행된다. 그래서 장치 없는 MaterialSchema에 둔다.
- **MaterialCompiler는 셰이더를 컴파일하지 않는다.** 그래프 → 원문까지만 하고, job 제출은 MaterialCook·CompileCoordinator가 ShaderCompiler(셰이더 시스템 3절)로 한다. 그래서 MaterialCompiler는 프로세스·도구 없이 시험된다(계획 MT08 headless).
- **계획의 ShaderWorker**는 셰이더 시스템 이름 ShaderCompileWorker(셰이더 시스템 14절)다.

의존 방향(→는 의존):

```text
EditorMaterial → { MaterialCook, MaterialCompiler, Material, Renderer, EditorBackend }
MaterialCook   → { Material, MaterialCompiler, RendererShaders, 정점 입력 타입 선언(16절 MRB8), AssetCooker, AssetNugget, ShaderCompiler }
MaterialCompiler → { MaterialSchema, ShaderCore, Core }
Material(Runtime) → { AssetRuntime, ObjectRuntime, MaterialSchema, MaterialRuntime }
MaterialRuntime → { MaterialSchema, RenderBase, ShaderCore, GraphicsTypes }
MaterialSchema → { Core, MetaSchema, Serialization, ShaderCore, RenderData(번호표) }
Renderer → { RendererShaders, MaterialRuntime, RenderBase, RenderGraph, … }   (Material 에셋 모듈은 모른다)
RendererShaders(Renderer 3.2절) → { ShaderCore, RenderData, MaterialSchema }
```

**pass 셰이더 선언 모듈.** 쿠커는 "이 머티리얼에 어떤 pass 셰이더가 필요한가"를 알아야 한다. UE는 pass 셰이더 타입을 Renderer 모듈이 정적으로 등록하고(예 `RR/Private/BasePassRendering.cpp:180–192`), 셰이더 맵 layout을 만들 때 등록된 셰이더 타입 전체를 훑는다(`RE/Private/Materials/MaterialShader.cpp:3210–3310`). 그래서 Renderer 모듈이 로드되어야 표가 찬다. Rocks는 Renderer 스펙이 장치 없는 `RendererShaders` 모듈을 두었다(Renderer 3.2절, 결정 RN11). MaterialCook과 Renderer가 모두 이 모듈을 링크한다. 정점 입력 타입 선언도 같은 이유로 장치 없는 위치에 있어야 한다(16절 MRB8).

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

UE는 파라미터 기본값을 `FMaterialCachedExpressionData`의 타입별 병렬 배열로 캐시한다(`RE/Public/MaterialCachedData.h:209, 340–373`). 레이어 스택 노드의 일반화판(`UMaterialExpressionLayerStack`)은 Experimental이다(`RE/Public/Materials/MaterialExpressionLayerStack.h:30–31`). 이 문서는 따르지 않는다.

### 4.2 Rocks 에셋 클래스

모든 클래스는 `ROCKS_CLASS`이고 Outer는 Package다(AssetSystem 6.1절). "EditorOnly" 필드는 쿠킹 데이터에서 빠진다(AssetSystem 6.2·10절).

| 클래스 | 필드(EditorOnly 표시) | 쿠킹 데이터 |
|---|---|---|
| `MaterialBase`(Abstract) | — | — |
| `Material` | `graph : MaterialGraph`(EditorOnly), `traits : MaterialTraitsDesc`(도메인·혼합 방식·양면·마스크 기준값 등 기본 속성), `usage : VertexInputUsage`, `functionRefs`(EditorOnly, Build 의존)·`collectionRefs`(Hard + Build, 4.4절), 미리보기 설정(EditorOnly) | `MaterialArtifact` 목록(품질 수준별), 텍스처·컬렉션 Hard 참조 |
| `MaterialInstance` | `parent : ObjectRef<MaterialBase>`(Hard), 종류별 override 배열(4.3절), `staticOverrides`, `traitOverrides : Optional<MaterialTraitsDesc>`, `layerStackOverrides` | static 변형이 있으면 자기 `MaterialArtifact`, 없으면 평탄화한 override 값만 |
| `MaterialFunction` | `graph`(EditorOnly), `usage : {General, Layer, LayerBlend}`, `exposeToLibrary`(EditorOnly), 입출력 선언 | 없다(쿠킹 런타임에 함수는 필요 없다. 함수를 쓰는 머티리얼에 펼쳐져 들어간다) |
| `MaterialFunctionInstance` | `parent : ObjectRef<MaterialFunction>`, 파라미터 기본값 override | 없다 |
| `ParameterCollection` | `scalars`·`vectors : Array<CollectionParameter>`(`ParameterId`·이름·기본값) | layout과 기본값(World 상태가 쓴다) |

`DynamicMaterialInstance`는 에셋이 아니다. World 쪽 객체가 만들어 소유하며, 부모 머티리얼은 `SoftObjectRef`와 `AssetLease`로 붙잡는다(AssetSystem 6.3절. 계획 7.1절 예시의 `SoftObjectRef<MaterialAsset>`과 같은 규칙). dynamic 파라미터만 바꿀 수 있고 static 값·기본 속성은 바꿀 수 없다(`UnsupportedSetting`). UE도 static 변형 갱신 함수가 에디터 전용이다(`RE/Public/Materials/MaterialInstance.h:1042–1083`). 다만 UE에서 실행 중 인스턴스가 static override를 거부하는 경로 전체를 추적하지는 않았다(미확인).

**함수 인스턴스는 쿠킹 런타임에 남지 않는다.** UE도 함수 호출은 호출한 쪽 그래프에 인라인된다(`RE/Private/Materials/MaterialExpressions.cpp:15774–15824`). Rocks는 함수·함수 인스턴스를 Build 의존으로만 다룬다(4.4절).

### 4.3 리플렉션과 저장

- **그래프 저장.** `MaterialGraph`는 `ROCKS_STRUCT`다. 노드는 `Array<MaterialNodeRecord>`이고 노드별 속성은 노드 타입이 정한 `ROCKS_STRUCT`를 `InstancedStruct`(리플렉션 B9, ReflectionExtensions)로 담는다. 노드 위치·주석·접힘 같은 화면 데이터는 `MaterialGraphLayout` 구조체로 따로 둔다(semantic hash 밖, 5.5절).
- **모르는 노드.** 등록되지 않은 `NodeTypeId`(plugin 비활성 등)는 속성 바이트를 그대로 보존하는 자리 노드로 읽는다(계획 14.6절). 출력에 닿으면 쿠킹 오류다(5.4절 V12).
- **스키마 변경.** 노드 타입마다 `schemaVersion`을 두고 migration 함수를 등록한다(계획 14.6절). 핀 제거·이름 변경은 `PinKey`가 안정 키라 간선이 보존되며, 제거된 핀에 붙은 간선은 진단과 함께 끊는다.
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

## 5. 그래프 스키마

### 5.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 노드 | `UMaterialExpression` 파생 UCLASS 하나가 노드 하나. 기본 번역기는 노드의 `Compile`이 코드 조각을 낸다. IR 번역기는 `Build(MIR::FEmitter&)`를 쓴다(구현 237개) | `RE/Public/Materials/MaterialExpression.h:284`, `RE/Private/Materials/MaterialExpressionsToMIR.cpp:330–333` |
| 출력 속성 | `EMaterialProperty`: Emissive·Opacity·OpacityMask·BaseColor·Metallic·Specular·Roughness·Anisotropy·Normal·Tangent·WPO·Subsurface·AO·Refraction·CustomizedUVs0–7·PixelDepthOffset·ShadingModel·FrontMaterial(Substrate)·SurfaceThickness·Displacement·MaterialAttributes·CustomOutput. WorldDisplacement·TessellationMultiplier는 `_DEPRECATED` | `RE/Public/SceneTypes.h:149–190` |
| 함수 입력 타입 | Scalar부터 Substrate까지 13종 | `RE/Public/Materials/MaterialExpressionFunctionInput.h:18–35` |
| attribute 집합 | `FMaterialAttributeDefinitionMap`이 속성 Guid·기본값·타입·빈도를 정의하고 custom attribute를 등록받는다 | `RE/Public/Materials/MaterialAttributeDefinitionMap.h:83–160` |
| static 분기 노드 | Quality·ShadingPath·FeatureLevel·ShaderStage Switch | `RE/Public/Materials/MaterialExpressionQualitySwitch.h:14`, `…ShadingPathSwitch.h:17, 28`, `…FeatureLevelSwitch.h:17`, `…ShaderStageSwitch.h:14` |
| 임의 HLSL 노드 | `Code`·`Inputs`·`AdditionalOutputs`·`AdditionalDefines`·`IncludeFilePaths` | `RE/Public/Materials/MaterialExpressionCustom.h:74–104` |
| IR 전용 노드 | `meta=(NewMaterialTranslator)` 노드는 기본 번역기에서 "Unsupported expression" | `RE/Private/Materials/MaterialExpressions.cpp:855–861` |
| exec wire | 5.8 deprecated | `RE/Public/Materials/Material.h:1068–1070` |

### 5.2 노드·핀·간선

- **그래프** = 노드 목록 + 간선 목록 + 출력 노드 하나(`MaterialOutput`). 함수 그래프는 입력·출력 노드를 여럿 가진다.
- **노드** = `{ NodeId, NodeTypeId, schemaVersion, properties(InstancedStruct) }`. 핀은 노드 타입 descriptor가 정한다. 개수가 바뀌는 핀(임의 HLSL 노드의 입력, 함수 호출 노드의 입출력)은 노드 속성에서 핀 목록을 만드는 descriptor 함수가 정한다.
- **핀** = `{ PinKey, 방향, 타입 제약, 기본값(입력 핀), 단계 제약, 필수 여부 }`. `PinKey`는 노드 타입 descriptor가 선언한 안정 키다(계획 14.6절 PinId). 표시 이름·배열 순번과 독립이라 핀 이름을 바꿔도 키는 그대로다. 핀을 없애거나 합치면 노드 migration이 옛 키를 새 키로 옮긴다.
- **간선** = `(출력 NodeId, 출력 PinKey) → (입력 NodeId, 입력 PinKey)`. 입력 핀은 간선 하나, 출력 핀은 여럿을 가진다.
- **재경유(reroute)·주석**은 화면 노드다. 재경유는 IR을 만들 때 간선으로 접히며 semantic hash에서도 접힌 간선으로 센다. 그래서 재경유를 더해도 compile이 일어나지 않는다.
- 노드 타입 descriptor(계획 14.6절 `MaterialNodeDescriptor`)는 MaterialSchema에 데이터로 두고, lowering 함수(노드 → IR)는 MaterialCompiler에 둔다. plugin은 둘을 같은 module lease로 등록한다(계획 14.6절 `RegisterMaterialNode`).

### 5.3 타입 체계

| 타입 | 값 | 비고 |
|---|---|---|
| `Float1`–`Float4` | 32-bit float 벡터 | half는 없다(셰이더 시스템 7.2절 5항이 막는다) |
| `Int1`–`Int4`, `UInt1`–`UInt4` | 32-bit 정수 벡터 | 상수 블록에서는 float와 다른 16 byte slot에 둔다(7.2절 5항) |
| `Bool` | 실행 중 bool | 상수 블록에는 uint 비트로 담는다(UE도 32비트 단위 비트 패킹, `RE/Private/Materials/HLSLMaterialTranslator.cpp:3792–3822`) |
| `Float3x3`, `Float4x4` | 행렬 | 상수 블록에는 `Float4x4`만 둔다(7.2절 5항). `Float3x3`는 IR 안에서만 쓴다 |
| `WorldPosition` | 절대 world 위치(타일 + 오프셋, `Float3` 둘) | cm 단위. 셰이더 안 기본 위치는 카메라 상대 world(translated world)이고, 절대 위치가 필요한 노드만 이 타입을 쓴다. 표현은 Renderer 스펙의 view 상수(16절 MRD6)를 따른다. UE는 LWC 타입을 쓰고 기본으로 켠다(`RE/Private/Materials/HLSLMaterialTranslator.cpp:150–178` `r.MaterialEditor.LWCEnabled`) |
| `Texture2D`·`Texture2DArray`·`TextureCube`·`TextureCubeArray`·`Texture3D` | 텍스처 자원 | 가상 텍스처·외부(영상) 텍스처는 후속 |
| `StaticBool`·`StaticInt`·`StaticEnum` | 번역 때 정해지는 값 | static switch·component mask·preset 선택 |
| `Surface` | BSDF 트리(10절) | 출력 노드의 표면 입력 |
| `MaterialAttributes` | 속성 집합(표면 입력 + 등록된 custom attribute) | 레이어·혼합 함수의 입출력 |

**암묵 변환.**
- scalar → 벡터 복제는 허용한다.
- 벡터 → 짧은 벡터(앞 성분 사용)는 허용하고 그래프 진단에 정보 수준으로 남긴다. 텍스처 RGBA → 색 RGB 같은 연결이 흔하기 때문이다.
- 짧은 벡터 → 긴 벡터는 오류다. 명시 Append 노드를 쓴다.
- 정수 → float는 허용하고, float → 정수는 오류다(명시 변환 노드).
- `StaticBool` → `Bool`은 허용하고, 반대는 오류다.

### 5.4 검증 규칙

검증은 MaterialCompiler가 한다. 진단은 `(AssetId, NodeId, PinKey, 코드, 메시지)`다(계획 MT02 "원인 node/pin 위치 일치"). 화면은 이 진단을 그대로 표시한다.

| ID | 규칙 | 시점 |
|---|---|---|
| V1 | 핀 타입 제약과 암묵 변환(5.3절) | 연결·번역 |
| V2 | 데이터 흐름 순환 금지(재경유 포함) | 연결·번역 |
| V3 | 함수 호출 순환·인스턴스 부모 사슬 순환 금지 | 번역(Build 의존 SCC) |
| V4 | 단계 규칙. 픽셀 전용 노드(화면 좌표·미분·scene texture)를 정점 출력에 쓰면 오류다. 정점에서 암묵 mip 텍스처 샘플도 오류다(명시 mip 필요) | 번역(IR 분석, 6.4절) |
| V5 | 도메인·혼합 방식이 허용하지 않는 출력 핀 연결(예: Opaque의 Opacity) | 번역. 연결은 경고로 남기고 IR에서 버린다 |
| V6 | 렌더 경로·pass·형식이 제공하지 않는 입력·기능 사용(예: Forward+에서 GBuffer 읽기, 불투명에서 scene color 읽기, GLES Deferred에서 GBufferD가 필요한 shading model) | 번역(capability 검사, 계획 14.7절, Renderer 14.2·14.6절) |
| V7 | static 입력에 비static 값 연결 | 번역 |
| V8 | 파라미터 이름 충돌(같은 scope에서 같은 이름·다른 종류) | 번역 |
| V9 | 텍스처·샘플러 수, 보간 벡터 수, 상수 블록 크기, 컬렉션 수의 형식·pass 예산 초과(7.4절 4항·13절, pass별 텍스처 예산은 Renderer 7.4절) | 번역((pass, 퍼뮤테이션)별 예산)·job(형식별 상한) |
| V10 | 텍스처 샘플 종류와 텍스처 설정 불일치(예: sRGB 텍스처를 `Normal`로 샘플) | 번역. 텍스처 메타데이터는 진단용으로만 읽고 키에 넣지 않는다 |
| V11 | NaN·Inf 상수 금지 | 연결·번역 |
| V12 | 등록되지 않은 노드 타입이 출력에 닿음 | 번역(에디터는 열 수 있고, 쿠킹은 실패) |
| V13 | 임의 HLSL 노드 규칙(7.5절) | 번역·job |
| V14 | 레이어 스택: 레이어 함수는 `MaterialAttributes` 출력 하나, 혼합 함수는 두 입력과 알파, 레이어 수 상한(MAT3 착수 때 정함) | 번역 |

오류가 있는 그래프도 draft로 저장·재열기할 수 있다(계획 14.8절). 쿠킹은 오류가 하나라도 있으면 게시하지 않는다.

### 5.5 안정 id와 semantic hash

- `NodeId`: 노드를 만들 때 정하는 Guid다. 복사·붙여넣기는 새 Guid를 받는다. 저장 순서와 무관하다.
- `ParameterId`: 파라미터 노드를 만들 때 정하는 Guid다. 이름을 바꿔도 유지된다. 에셋 복제는 새 Guid를 받는다.
- **semantic hash**: 그래프의 의미 부분만 정규화해 SHA-256한 값이다.
  - 노드를 `NodeId`로 정렬하고, 노드마다 (`NodeTypeId`, schemaVersion, 속성 정규 바이트)를 쓴다. 속성 정규 바이트는 리플렉션 binary codec 출력이다(기본값 포함, float는 bit 패턴).
  - 간선은 (입력 NodeId, 입력 PinKey) 순으로 정렬해 쓴다. 재경유는 접은 뒤 센다.
  - 출력에 닿지 않는 노드도 넣는다. 닿지 않는 노드를 고쳐도 키가 바뀌지만 번역 결과는 같으므로, 셰이더 집합 id(8.6절)가 같아 셰이더는 다시 쌓이지 않는다.
  - 파라미터 노드의 기본값(scalar·vector·텍스처·dynamic bool)은 넣지 않는다(4.5절). static 파라미터 기본값은 넣는다.
  - 화면 데이터(위치·크기·주석 문구·색·접힘·선택)는 넣지 않는다. 그래서 노드를 옮겨도 compile이 일어나지 않는다(계획 14.6·14.8절, MT04).
- semantic hash는 에디터 상태 판정(현재 그래프와 마지막 성공 결과의 일치, 계획 14.8절)과 번역 너겟 키(12.2절)에 쓴다.

### 5.6 출력 노드(도메인별 typed output)

| 도메인 | 출력 핀 | 단계 | 비고 |
|---|---|---|---|
| Surface | `Surface`(BSDF), `Opacity`, `OpacityMask`, `AmbientOcclusion`, `PixelDepthOffset`, `WorldPositionOffset`, `CustomizedUv0`–`7`, 이름 있는 custom 출력 | 픽셀·정점 | 혼합 방식에 따라 `Opacity`·`OpacityMask`가 활성화된다(V5). `Refraction`·`Displacement`는 후속 |
| DeferredDecal | `Surface`(쓰는 속성 부분집합), `Opacity` | 픽셀 | 쓰는 속성(색·노멀·거칠기·발광)은 Renderer 스펙의 decal 계약이 정한다(16절 MRD12) |
| LightFunction | `LightColor`(Float3) | 픽셀 | |
| PostProcess | `Color`(Float4), scene texture 입력 노드 | 픽셀 | 삽입 위치는 계획 26절의 HDR·tone-map 위치 |
| UserInterface | `Color`(Float3), `Opacity`, `OpacityMask` | 픽셀 | UI 정점 입력 타입(UiToolkit) |
| Volume | `Albedo`·`Extinction`·`Emissive` | 픽셀 | 후속(Renderer volumetric fog 단계) |

UE의 RuntimeVirtualTexture 도메인은 deprecated이며 로드 때 Surface + RVT 출력 노드로 바뀐다(`RE/Public/MaterialDomain.h:26–27`, `RE/Private/Materials/Material.cpp:3259–3333`). Rocks는 이것을 도메인으로 두지 않는다. 가상 텍스처 단계에서 Surface의 custom 출력으로 정한다(후속).

도메인은 Renderer가 준비될 때까지 `Unsupported`로 표시한다(계획 14.7절). M3 기준은 Surface(Opaque·Masked)와 `DefaultLit`·`Unlit` preset이다. 영역 이름은 Renderer 14.2절을 따른다.

### 5.7 노드 묶음(1차 범위)

계획 14.5절 "기본 표현식"과 MAT0–MAT2 범위다. 아래 이름은 기능을 가리키는 가칭이다. 구현 때 Rocks 이름으로 정하고, UE 대응은 노드 descriptor 문서에 둔다.

| 묶음 | 노드 |
|---|---|
| 상수·파라미터 | 상수 1–4성분, Scalar·Vector·Texture 파라미터, Static Bool·Static Component Mask 파라미터, Dynamic Bool 파라미터, 컬렉션 값 |
| 텍스처 | Texture Sample(종류·샘플 종류·mip 방식: 암묵·bias·level·gradient), Texture Size, Texture Coordinate(채널·tiling) |
| 수학 | 사칙·Min·Max·Clamp·Saturate·Lerp·Abs·Floor·Frac·Sqrt·Power·Exp2·Log2·Sin·Cos·Dot·Cross·Normalize·Length·Distance·Append·Component Mask·Swizzle·If(비교 선택) |
| 기하·공간 | World Position·Object Position·Camera Position·Vertex Normal·Vertex Tangent·Vertex Color·Pixel Normal·Transform(공간 사이)·Screen Position·Pixel Depth |
| 시간·뷰 | Time·Delta Time·View Size |
| 분기 | Static Switch·Quality Switch·Render Path Switch·Capability Tier Switch·Stage Switch |
| 표면 | Surface Slab·shading preset 노드·Surface Mix 등(10절, 단계별) |
| 재사용 | Function Call·Function Input·Function Output·Make/Break Attributes·Layer Stack(MAT3) |
| 고급 | Vertex Interpolator(정점 → 픽셀 명시 보간), 임의 HLSL(선택, 7.5절) |

## 6. IR

### 6.1 UE 5.8 방식

| 경로 | 5.8 상태 | 근거 |
|---|---|---|
| 노드가 코드 조각을 직접 내는 번역기(`FHLSLMaterialTranslator`, 진입 함수 `Translate_Legacy`) | **기본값.** 클래스에 deprecation 표시가 없다 | `RE/Private/Materials/HLSLMaterialTranslator.h:233`, 분기 `RE/Private/Materials/MaterialShared.cpp:3683–3704` |
| Material IR + IR→HLSL(`FMaterialIRToHLSLTranslation`) | **실험.** `r.Material.Translator.EnableNew`(기본 false, 설명 "prototype (WIP)")와 머티리얼별 `bEnableNewHLSLGenerator`가 모두 켜져야 쓰인다. 사용 시 "new experimental material translator" 로그, 쿠킹 때 "still experimental" Display 로그 | `MaterialShared.cpp:108–112, 3616`, `RE/Private/Materials/MaterialInterface.cpp:233–237, 342–351`, `RE/Public/Materials/Material.h:1072–1074` |
| 5.1–5.3의 HLSLTree(`MaterialHLSLGenerator`) | **코드 제거.** Runtime·Editor 전체 grep 0건이다. 플래그 이름 `bEnableNewHLSLGenerator`만 IR 스위치로 재사용된다 | grep 결과(2026-11-17) |

UE IR 구조(참고):
- 값 종류는 순수 값(상수·builtin·파라미터 컬렉션·uniform 등)과 명령(연산·분기·텍스처 읽기·Substrate 연산·단계 전환·미분·보간 설정·호출·preshader 파라미터)이다(`RE/Public/Materials/MaterialIR.h:43–90`).
- 값은 POD이고, 빌더가 같은 계산을 해시로 찾아 한 인스턴스로 쓴다(`MaterialIR.h:97–118`).
- 모듈은 단계(정점·픽셀·compute)와 custom 출력마다 진입점을 가진다(`RE/Public/Materials/MaterialIRModule.h:20–63`).
- 빌드 단계는 초기화 → 노드 그래프를 IR로 → Substrate 출력 → 출력 설정 명령 → 의존 순 분석 → 모듈 전체 분석(한도·preshader fixup) → 명령을 가장 좁은 블록에 배치 → 산출 확정이다(`RE/Private/Materials/MaterialIRModuleBuilder.cpp:1532–1598`).
- 상수 접기(`RE/Private/Materials/MaterialIREmitter.cpp:2142, 2755, 2902, 3016`)와 uniform 연산의 preshader 승격(`MaterialIREmitter.cpp:2738`, 설계 설명 `RE/Public/Materials/MaterialIRPreshader.h:12–32`)이 있다. 별도 DCE·CSE 패스가 있는지는 미확인이다.

**Rocks 선택(결정 MD2).**
- UE 기본 번역기는 "이름이 legacy인 기본값"이고 IR 쪽은 실험이다.
- 계획은 처음부터 typed IR을 요구하고(14.1·14.7절), HLSL 말고도 CPU bake(`BakeMaterialProfile`, 계획 14.10절)와 ray profile(M8)이 IR을 소비한다.
- 그래서 Rocks는 IR 하나를 정본으로 두고 HLSL은 그 소비자 가운데 하나로 만든다. 노드가 HLSL 조각을 직접 내는 경로는 두지 않는다.

### 6.2 구조

- **모듈** = (머티리얼 static 변형, 품질 수준)마다 하나다. 진입점은 정점 결과·이전 프레임 정점 결과·표면·opacity·opacity mask·pixel depth offset·custom 출력마다 하나다(7.3절 함수와 1:1).
- **값** = `{ 타입, 연산, 피연산자 값들, 빈도, 단계 집합, 속성(공간·색공간·미분 요구) }`. 피연산자는 값 번호다. (타입, 연산, 피연산자, 속성)이 같으면 한 값으로 공유한다(hash-consing).
- **빈도** 격자: `Constant < Static < Uniform < Instance < Vertex < Pixel`.
  - `Uniform`: 머티리얼 파라미터·컬렉션 값과 그것만으로 된 식. 상수 식 프로그램(9.4절)으로 간다.
  - `Instance`: 그리기 단위 데이터(primitive·instance custom data). Renderer 스펙의 instance 데이터 계약을 따른다.
  - 연산의 빈도는 피연산자 빈도의 최대다. 예외로 미분과 암묵 mip 텍스처 읽기는 `Pixel`이고, 명시 mip 텍스처 읽기는 최소 `Vertex`다(uniform으로 끌어올리지 않는다).
- **조건 영역.** 번역 때 정해지지 않는 static 조건(렌더 경로·기능 등급)은 IR의 조건 값으로 남기고 생성 원문에서 `#if`로 낸다(6.3절). 품질 수준과 static 파라미터는 번역 때 정해진다.

### 6.3 생성(그래프 → IR)

1. **입력 고정.** 그래프 snapshot(계획 14.6절 immutable)과 Build 의존 함수·컬렉션의 snapshot을 받는다. Object 주소는 들고 가지 않는다.
2. **static 해석.** static 파라미터(인스턴스 override 반영), Quality Switch(번역 중인 품질 수준), Static Switch를 풀고, 선택되지 않은 입력은 IR로 만들지 않는다. UE도 Quality Switch는 유효 입력 하나만 컴파일한다(`RE/Private/Materials/MaterialExpressions.cpp:9082–9107`, 검증 때만 모든 입력 `:9088–9102`).
3. **렌더 경로·기능 등급 분기.** Render Path Switch는 Deferred·ForwardPlus·Default 세 입력을 가진다. 경로 전용 pass(GBuffer·ForwardOpaque)는 그 경로 입력을, 두 경로가 함께 쓰는 pass(Depth·ShadowDepth·Translucent·Velocity 등, Renderer 14.3절)는 Default 입력을 쓴다. Render Path Switch와 Capability Tier Switch는 모든 입력을 IR로 만들고 조건 값으로 잇는다. UE는 shading path를 플랫폼당 하나로 정해 한쪽만 컴파일한다(`RE/Public/Materials/MaterialExpressionShadingPathSwitch.h:50`). Rocks는 한 실행 안에서 view마다 Deferred·Forward+를 고르므로(계획 11.1절) pass job이 정한다.
4. **함수 펼치기.** 함수 호출은 scope를 쌓고 함수 그래프를 펼친다. 함수 입력은 호출한 쪽의 값이다. UE도 인라인이다(`RE/Private/Materials/MaterialExpressions.cpp:15774–15824`, 호출자 문맥 복귀 `:16564–16594`). 파라미터 scope 경로는 9.2절이다.
5. **레이어 스택 펼치기.** 레이어 i를 펼치고 혼합 함수로 결합한다. 레이어·혼합 scope는 레이어 Guid다. UE는 레이어 index에 연관을 단다(`MaterialExpressions.cpp:7238–7239, 7273–7274`).
6. **출력.** 도메인·혼합 방식이 쓰는 출력 핀만 진입점으로 만든다(V5).

### 6.4 분석

| 분석 | 내용 | 오류 |
|---|---|---|
| 타입 | 노드 lowering이 낸 타입과 암묵 변환을 확정한다 | V1 |
| 단계 | 진입점에서 거슬러 각 값이 필요한 단계 집합을 구한다. 정점 진입점이 픽셀 전용 값을 쓰면 오류다 | V4 |
| 미분 | 암묵 미분(암묵 mip 샘플·`ddx`)은 픽셀 단계에서만 허용한다. 미분 요구를 값에 기록한다. 후속 visibility 기반 shading이 analytic 미분을 요구할 때 쓰려는 것이다(UE는 analytic 미분 모드를 따로 둔다, `RE/Private/Materials/HLSLMaterialTranslator.cpp:150–178`) | V4 |
| 공간 | 벡터 값의 좌표 공간 표지(local·world·translated world·view·tangent 등)를 따라가 잘못된 공간 결합을 경고한다. 노멀 출력은 tangent 공간이 기본이고 표지로 확인한다 | 계획 14.7절 "잘못된 normal 공간" |
| 색공간 | 텍스처 샘플 종류가 sRGB 해제를 정하고 값에 선형 표지를 단다 | V10 |
| 보간 배정 | 정점 → 픽셀 값(customized UV·Vertex Interpolator)을 `Float4` 묶음에 채운다. 순서는 핀 번호·NodeId 순이다 | V9 |
| 자원 집계 | 텍스처·샘플러·상수 블록 크기·컬렉션·보간 벡터 수를 센다 | V9 |
| capability | 노드가 요구하는 기능(scene texture 읽기·GBuffer 읽기·dual source blend 등)을 진입점별로 모아 `MaterialShaderTraits`에 기록한다 | V6 |

### 6.5 최적화

| 최적화 | 규칙 |
|---|---|
| 값 공유(CSE) | hash-consing으로 생성 중에 한다 |
| 상수 접기 | **정확히 반올림되는 연산만** 호스트에서 접는다: 사칙·`sqrt`·`min`·`max`·`abs`·`floor`·`frac`·비교·선택·swizzle. 초월 함수(`sin`·`pow`·`exp2`·`log2`)는 Core 결정적 수학(A0-C)으로만 접고, 결정적 수학 함수가 없는 연산은 접지 않는다. 호스트 libm으로 접으면 호스트마다 바이트가 달라진다(셰이더 시스템 10절 "상수 접기") |
| 대수 단순화 | `x*1`, `x-0`, 이중 swizzle 합치기만 한다. `x*0 → 0`(NaN)과 `x+0 → x`(−0 부호)처럼 IEEE 결과를 바꾸는 단순화는 하지 않는다 |
| 불필요 값 제거(DCE) | 진입점에서 닿지 않는 값을 버린다. 함수·레이어의 쓰지 않는 출력도 사라진다 |
| uniform 끌어올림 | `Uniform` 빈도 부분식은 상수 식 프로그램의 출력 하나로 바꾸고, 셰이더는 상수 블록 값을 읽는다(9.4절). 텍스처 읽기는 끌어올리지 않는다 |
| static 가지 제거 | 6.3절 2단계 |

**IR hash.** 최적화를 마친 IR을 값 번호 순으로 직렬화한 바이트의 SHA-256이다. 번역 너겟 키는 번역 전에 계산해야 하므로 IR hash를 쓸 수 없다. IR hash는 결과 비교 진단과 시험(15절)에 쓴다. 셰이더 중복 제거는 셰이더 집합 id(8.6절)가 한다.

### 6.6 결정성

- 값 번호는 진입점 순서(7.3절 표 순서) → 핀 순서 → 피연산자 순서의 깊이 우선 첫 방문 순이다. 해시 표 순회 순서에 기대지 않는다.
- 노드를 훑는 순서는 NodeId 정렬이고, 함수 펼치기는 호출 위치 NodeId 정렬이다.
- 실수 상수는 bit 패턴으로 다루고, 원문에는 최단 왕복 표기(A0-C CRT 없는 숫자 변환)로 쓴다.
- 여러 머티리얼을 병렬로 번역하면 결과를 정렬한 뒤 기록한다(AssetSystem 10절).
- 시험: 노드 저장 순서만 섞은 두 파일의 번역 결과 byte 동일, Windows·macOS 사이 byte 동일(15.3절 MS-T1).

### 6.7 다른 IR 소비자

| 소비자 | 단계 | 계약 |
|---|---|---|
| HLSL 생성(7절) | MAT0 | 이 문서 |
| CPU bake(`BakeMaterialProfile`, 계획 14.10절) | M3–M4 | 같은 IR에서 지원 노드 부분집합을 CPU 평가기로 바꾼다. 지원하지 않는 값(미분·view 의존·임의 HLSL)은 노드 위치 오류다. 상세는 bake 스펙 |
| ray profile(계획 14.4절 RayScene 재질표) | M8 | 후속 |

IR은 소비자별 capability 검사를 받는다. 예를 들어 CPU bake는 임의 HLSL 노드를 지원하지 않는다.

## 7. 코드 생성

### 7.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 삽입 방식 | `%{name}` 이름 있는 자리 문자열 템플릿이다. 형식별로 템플릿을 캐시하고 파일 감시로 무효화한다. 자리 이름을 정렬해 해시한다 | `RE/Private/Materials/StringTemplate.h:13–29`, `MaterialSourceTemplate.h:12–52`, `MaterialSourceTemplate.cpp:93–146` |
| 템플릿 | `SH/Private/MaterialTemplate.ush`. 구조체 `FPixelMaterialInputs`(`:371`)·`FMaterialPixelParameters`(`:427`)·`FMaterialVertexParameters`(`:677`), 계산 `CalcPixelMaterialInputs`(`:4333`)·`CalcMaterialParametersEx`(`:4664`), 자리 `material_declarations`(`:360`)·`pixel_material_inputs`(`:373`)·`calc_pixel_material_inputs_*`(`:4338–4466`) | 같은 파일 |
| 생성 파일 경로 | `/Engine/Generated/Material.ush`를 공유 컴파일 환경의 가상 경로 표에 등록한다 | `RE/Private/Materials/MaterialShared.cpp:3573–3575` |
| pass 셰이더 연결 | pass 원문이 `/Engine/Generated/Material.ush`를 include한다(`SH/Private` 아래 63개 파일). 예: `SH/Private/BasePassPixelShader.usf:94`, `DepthOnlyPixelShader.usf:12` | grep 결과 |
| 두 번역기 공통 | 기본 번역기와 IR 번역기가 같은 템플릿·같은 가상 경로를 쓰므로 pass 원문은 번역기를 모른다 | `MaterialShared.cpp:3664–3678` |

### 7.2 Rocks 파일 구조

```text
/Engine/Private/Material/MaterialCommon.rsh     정적. 문맥 구조체·보조 함수, 생성 파일 include
SurfaceAttributes 정의(Renderer 소유)           Renderer 14.1절. MaterialCommon.rsh가 include
/Engine/Generated/Material.rsh                  job마다 연결되는 생성 파일(번역 결과)
/Engine/Generated/VertexInput.rsh               정점 입력 타입 원문(RenderBase 10.2절)
pass 셰이더(.rsf, Renderer 스펙)                 MaterialCommon.rsh와 VertexInput.rsh를 include
```

- **템플릿 치환을 하지 않는다.** 생성 파일은 정해진 이름의 정의·자원 선언·함수만 담고, 정적 원문이 그것을 부른다. 텍스트 치환이 없으므로 템플릿 버전 hash가 필요 없고, 정적 원문의 내용은 전처리 결과로 job 키에 들어간다(셰이더 시스템 10절 7항).
- **연결.** 메인 프로세스 전처리기가 job마다 `/Engine/Generated/Material.rsh`를 번역 결과의 메모리 텍스트로 잇는다. 정점 입력 타입의 `VertexInput.rsh` 연결(RenderBase 10.2절)과 같은 장치다. 머티리얼이 없는 셰이더 타입(엔진 전역 셰이더)이 이 경로를 include하면 전처리 오류다.
- **셰이더 맵 키.** 같은 pass 셰이더라도 머티리얼마다 원문이 다르다. 그래서 셰이더 맵 키와 런타임 조회 키에 머티리얼 셰이더 집합 id가 들어간다(8.6절, 16절 MSR2).

### 7.3 생성 파일 계약

생성 파일은 아래 정의와 함수를 **항상 모두** 낸다. 쓰지 않는 함수는 기본값을 돌려준다. 그래서 pass 원문이 머티리얼마다 `#if`로 함수가 있는지 묻지 않는다. 예외는 custom 출력 함수로, 머티리얼이 정의한 것만 낸다(pass가 요구한 custom 출력이 없는 머티리얼은 거르기에서 빠진다, 8.3절).

| 정의·함수 | 뜻 |
|---|---|
| `MATERIAL_DOMAIN_SURFACE` 등 도메인 하나 = 1 | 도메인 |
| `MATERIAL_BLEND_OPAQUE`·`_MASKED`·`_TRANSLUCENT`·`_ADDITIVE`·`_MODULATE`·`_ALPHA_COMPOSITE` 하나 = 1 | 블렌드 모드(Renderer 14.2절) |
| `MATERIAL_TWO_SIDED`, `MATERIAL_USES_WPO`, `MATERIAL_USES_PREVIOUS_WPO`, `MATERIAL_USES_PIXEL_DEPTH_OFFSET`, `MATERIAL_USES_OPACITY_MASK` | 기능 표지(0·1) |
| `MATERIAL_SHADING_MODELS`, `MATERIAL_SINGLE_SHADING_MODEL` | 쓰는 shading model 집합 비트(번호는 Renderer 14.2절)와, 하나뿐이면 그 번호. Renderer가 그 분기만 컴파일한다(Renderer 14.4절) |
| `MATERIAL_NUM_TEXCOORDS`, `MATERIAL_NUM_INTERPOLANT_VECTORS` | 정점 입력에서 읽을 UV 채널 수, 머티리얼이 쓰는 보간 `float4` 수 |
| `MATERIAL_OPACITY_MASK_CLIP` | 마스크 기준값(기본 속성 상수) |
| `void EvaluateMaterialVertex(in MaterialVertexInputs c, inout MaterialVertexResult r)` | WPO(cm, world 공간 오프셋)·customized UV·보간 값 |
| `void EvaluateMaterialVertexPrevious(in MaterialVertexInputs c, inout MaterialVertexResult r)` | 이전 프레임 WPO(계획 14.7절 current/previous 계약). 문맥의 이전 시간·이전 instance 데이터로 평가한다 |
| `SurfaceAttributes EvaluateMaterialSurface(in MaterialInputs c)` | 표면 속성(Renderer 14.1절 함수 이름) |
| `float EvaluateMaterialOpacity(in MaterialInputs c)` | 반투명 coverage(`SurfaceAttributes.opacity`와 같은 값, 표면 전체 없이) |
| `float EvaluateMaterialOpacityMask(in MaterialInputs c)` | 마스크 값. depth·shadow pass가 표면 전체를 계산하지 않게 따로 둔다(10.4절 D8) |
| `float EvaluateMaterialPixelDepthOffset(in MaterialInputs c)` | PDO(cm) |
| `float4 EvaluateMaterialCustomOutput_<이름>(in MaterialInputs c)` | 머티리얼이 정의한 custom 출력 |
| `float4 EvaluateMaterialPreview(in MaterialInputs c)` | 노드 미리보기 번역에서만 의미가 있다(11.4절) |

함수 사이에는 계산이 겹칠 수 있다(예: 마스크 계산과 표면 계산이 같은 텍스처를 읽는다). 진입점마다 필요한 값만 내므로, 한 pass가 쓰는 함수는 필요한 계산만 한다. 한 pass에서 두 함수를 부르면 겹치는 계산의 제거는 하위 컴파일러(DXC·FXC)의 CSE에 맡긴다.

**문맥 구조체**(`MaterialCommon.rsh`):
- `MaterialVertexInputs`: world·translated world 위치, local 위치, 법선·tangent 기저, UV 채널, 정점 색, instance 데이터 index, 이전 프레임 변환 접근.
- `MaterialInputs`(Renderer 14.1절 이름): 화면 위치, translated world 위치, 기저, 보간된 UV·정점 색·custom 보간 값, 앞면 여부, view 상수 접근.
- 채우는 쪽은 pass 원문과 정점 입력 타입이다. RenderBase 10.2절이 "첫 메시 셰이더 착수 전에 정한다"고 남긴 `.rsh` 함수 인터페이스를 이 구조체로 정한다(16절 MRB3). UE 템플릿의 픽셀·정점 파라미터 구조체(`SH/Private/MaterialTemplate.ush:427, 677`)와 역할이 같다.

### 7.4 바인딩 규약(셰이더 시스템 7.2절)과의 관계

| 7.2절 항 | 머티리얼 생성 원문 규칙 |
|---|---|
| 1항 선언 | register·`[[vk::*]]`를 쓰지 않는다. 이름 없는 전역 uniform 금지에 맞춰 상수는 이름 있는 `cbuffer MaterialConstants`로 낸다 |
| 2항 루트 상수 | 머티리얼은 루트 상수를 쓰지 않는다. 그리기별 상수는 Renderer 몫이다 |
| 3항 슬롯 정규화 | 생성 cbuffer의 정규화 layout 항목에 동적 layout 서명을 단다(RenderBase SX8의 "파라미터 구조체 identity" 자리, 16절 MRB2) |
| 4항 상한 | 텍스처·샘플러는 번역 때 (pass, 퍼뮤테이션)별 `16 − 렌더러 합계`(Renderer 7.4절)로, 상수 버퍼·보간 수는 (프로필 공통 상한 − pass 예약 수)로 검사하고(V9), job에서 형식 상한으로 다시 검사한다 |
| 5항 상수 버퍼 layout | `float4` 경계 정렬, 16 byte slot 안에 float·int·uint를 섞지 않음, 배열은 `float4` 배열, bool은 uint 비트. 머티리얼 컴파일러가 이 규칙으로 layout 표를 만들고, 워커가 리플렉션 오프셋과 대조한다 |
| 6항 행렬 | 변환 노드는 `mul(v, M)`으로 내고 행렬은 행 우선 상수다 |
| 7항 단계 사이 변수 | 머티리얼 보간 값은 pass가 준 기준 번호부터 `TEXCOORDn`을 이어 쓴다(n ≤ 15, GLES는 형식 상한). struct 멤버 순서는 n 오름차순이다 |
| 9항 텍스처·샘플러 짝 | 텍스처 `MaterialTexture<k>`마다 `MaterialTexture<k>Sampler`를 낸다(짝 규칙 그대로, 결정 MD4) |
| 결정 7(경고) | 생성 원문도 경고 0이 기준이다. 생성기가 낸 경고는 생성기 버그로 본다 |

**레이아웃 서명.** 머티리얼 상수 블록의 정규 layout 표(멤버 이름·타입·offset·크기, 텍스처·샘플러 순번)의 SHA-256이다. job 키 8항(셰이더 시스템 10절)에 들어가고, 쿠킹 셰이더 집합 항목에도 기록되어 실행 때 상수 블록 layout과 대조한다(RenderBase 9.3절·SX6 서명 검증과 같다).

**스트리밍 텍스처 규칙(TextureStreaming 5.3절).**
- Texture Size 노드는 텍스처 slot의 **논리 크기** 상수(상수 블록)를 읽는다. 논리 크기는 바뀌지 않으므로 스트리밍 교체 때 상수 블록을 다시 쓰지 않는다.
- 명시 LOD 샘플과 정수 Load는 TextureStreaming의 상주 mip helper를 거쳐 생성한다. helper를 쓸 수 없는 식이면 그 텍스처 slot에 "전체 상주 전용" 표지를 달아 번역 결과로 넘긴다(TextureStreaming이 비스트리밍으로 돌린다).
- 도함수 기반 샘플은 변환하지 않는다.

### 7.5 임의 HLSL 노드

계획 14.7절대로 선택 고급 노드다. 기본 에디터 Gate를 막지 않는다.
- **선언.** 입력(이름·타입), 출력(이름·타입), 본문 텍스트, include 목록(허용 목록의 가상 경로만), 요구 기능(HLSL 2021·SM6 기능 등).
- **생성.** 노드마다 `MaterialCustom_<NodeId 앞 16 hex>(입력…, out 출력…)` 함수로 낸다. 본문은 함수 몸체에만 들어간다.
- **금지(V13).** 자원 선언·register·전역 변수·`#include`(include는 선언 목록으로만)·`#pragma`·임의 `discard`/`clip`(선언한 경우만 허용). 검사는 두 겹이다: 토큰 수준 텍스트 검사, 그리고 job 리플렉션(머티리얼 layout 밖 자원이 생기면 오류).
- **형식 거르기.** SM6 기능처럼 형식 정의에 없는 기능을 요구하면 그 형식에서 쿠킹 오류다. HLSL 2021은 셰이더 결정 2대로 형식 정의 기본값과 셰이더 타입 선택을 따른다: `D3D11_SM5`는 SPIRV-Cross 경로로 컴파일되고, GLES 형식은 기본값이 끔이라 pass 셰이더 타입이 켜지 않는 한 HLSL 2021 문법 노드는 GLES에서 쿠킹 오류다.
- CPU bake·ray profile에서는 미지원이다(6.7절).
- UE 대응은 `RE/Public/Materials/MaterialExpressionCustom.h:74–104`이며, UE는 자원 접근과 include를 막지 않는다.

### 7.6 진단과 원문 위치

- 생성 원문의 `#line`은 가상 경로 `/Engine/Generated/Material.rsh`만 쓴다(셰이더 시스템 10절 경로 누출 금지).
- 번역 결과에 **원문 위치 지도**(생성 줄 범위 → NodeId·PinKey)를 둔다(계획 14.6절 SourceMap). job 오류의 줄 번호는 이 지도로 노드 위치 진단으로 바뀐다.
- 에디터의 생성 원문 보기(계획 14.5절 "generated HLSL")는 이 파일 텍스트를 그대로 보여 준다.

## 8. 퍼뮤테이션

### 8.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 머티리얼 요약 | `FMaterialShaderParametersBase`: 도메인·shading model 집합·혼합 방식·품질·feature level·customized UV 수와 64+10개 비트 플래그(`bIsMasked`, `bIsUsedWith{SkeletalMesh, MorphTargets, …}`, `bMaterialMayModifyMeshPosition` 등) | `RE/Public/MaterialShared.h:1152–1271` |
| 거르기 | 머티리얼 셰이더는 셰이더 타입의 거르기만, 메시 머티리얼 셰이더는 vertex factory 거르기 AND 셰이더 거르기. pipeline은 모든 단계가 통과해야 한다 | `RE/Private/Materials/MaterialShader.cpp:1999–2002`, `RE/Private/Materials/MeshMaterialShader.cpp:178–186`(pipeline `:187–215`) |
| 레이아웃 | 모든 머티리얼 셰이더 타입×퍼뮤테이션, pipeline, 정렬된 vertex factory마다 거른 결과로 셰이더 맵 layout을 만든다 | `RE/Private/Materials/MaterialShader.cpp:3210–3310` |
| 사용 플래그 | `EMaterialUsage`. 에디터에서 처음 쓰일 때 플래그를 자동으로 켜고 패키지를 dirty로 한다(`bAutomaticallySetUsageInEditor` 기본 true). 게임에서는 "Default Material will be used in game" 경고 | `RE/Public/Materials/MaterialInterface.h:76–120`, `RE/Private/Materials/Material.cpp:1873–1983`, `RE/Public/Materials/Material.h:934` |
| 사용 플래그 거르기 예 | GPU skin vertex factory는 skeletal·morph 사용이 없으면 제외 | `RE/Private/GPUSkinVertexFactory.cpp:850–863` |
| 품질 수준 | Low·High·Medium·Epic(열거 순서). Quality Switch가 실제로 다른 입력을 가질 때만 그 수준을 "사용"으로 표시하고, 사용하지 않는 수준은 기본 리소스 하나로 공유 | `RE/Public/SceneTypes.h:129–139`, `RE/Private/Materials/MaterialCachedData.cpp:625–637`, `RE/Private/Materials/Material.cpp:2684–2720` |
| 쿠킹 품질 | 플랫폼 품질 override와 `bDiscardQualityDuringCook` | `RE/Private/Materials/MaterialInterface.cpp:374–398` |
| 노드 미리보기 | 로컬 vertex factory의 NoLightMap BasePass만 컴파일 | `ED/MaterialEditor/Private/MaterialEditor.cpp:351–376` |

### 8.2 Rocks 축

| 축 | 정하는 곳 | 줄이는 규칙 |
|---|---|---|
| 머티리얼 static 변형 | 머티리얼·인스턴스의 static 값·기본 속성(9.5절) | 결과가 같은 변형 공유(8.5절) |
| pass 셰이더 타입 × pass 퍼뮤테이션 | Renderer 스펙 14.3절 표·퍼뮤테이션 차원(`RendererShaders`) | `MaterialShaderTraits` 거르기(8.3절). pass 퍼뮤테이션 차원 수는 Renderer가 선언하고 상한을 둔다 |
| 정점 입력 타입 | RenderBase 10.2절 | 사용 대상(8.4절) AND 정점 입력 타입 거르기 |
| 형식 | TargetProfile `shaderFormats`(셰이더 시스템 12.3절) | 형식 기능 거르기(요구 기능·기능 등급) |
| 품질 수준 | 프로필의 쿠킹 품질 목록 | 실제로 다른 수준만(8.5절) |
| 렌더 경로 | 축이 아니다 | pass 셰이더 타입이 경로를 정한다(GBuffer는 Deferred, ForwardOpaque는 Forward+, Depth·ShadowDepth·Translucent 등은 공통, Renderer 14.3절). Render Path Switch는 pass job의 `#if`로 풀린다(6.3절) |

머티리얼 하나의 job 수 = (static 변형, 품질 수준, pass 셰이더 타입, pass 퍼뮤테이션, 정점 입력 타입, 형식) 조합 가운데 거르기를 통과한 것마다 그 pass의 단계 수(VS·PS)를 더한 값이다.

### 8.3 머티리얼 셰이더 특성과 거르기

`MaterialShaderTraits`(MaterialSchema)는 번역 결과에서 나온다. UE에서 번역 결과로 만드는 머티리얼 셰이더 파라미터(`RE/Public/MaterialShared.h:1287–1290`)와 같은 성격이다.

| 필드 | 뜻 |
|---|---|
| domain, blendMode, twoSided | 영역·블렌드 모드(번호는 Renderer 14.2절)·양면 |
| shadingModels | 쓰는 shading model 집합(Renderer 14.2절 번호, 10.3절) |
| translucencyPass, writesVelocity, castsShadow | 투명 pass 종류·velocity 쓰기·그림자 caster(Renderer 14.5절 정적 속성) |
| usesWpo, usesPreviousWpo, usesPdo, usesOpacityMask | 정점 변형·깊이 변경·마스크 |
| requiredCapabilities | scene color·scene depth·GBuffer 읽기·dual source blend 등(6.4절 capability) |
| customOutputs | 이름 있는 custom 출력 목록 |
| isDefaultMaterial, isSpecialEngineMaterial | 엔진 기본 머티리얼 여부(항상 컴파일 대상) |
| quality | 번역한 품질 수준 |

pass 셰이더 타입의 거르기 함수 서명은 `bool ShouldCompile(const MaterialShaderTraits&, VertexInputTypeId, PermutationId, FormatId)`이다. 정점 입력 타입의 거르기는 `bool ShouldCompile(const MaterialShaderTraits&, FormatId)`이다. 두 거르기는 결정적이어야 한다(등록 순서·실행 순서와 무관한 순수 함수). 셰이더 시스템 P14의 "형식·퍼뮤테이션 거르기 함수"에 머티리얼 인자를 더한 것이다(16절 MSR5). pass 셰이더 타입이 custom 출력을 요구하면 그 출력이 없는 머티리얼은 거른다.
- 사용 대상(정점 입력 타입 집합)은 특성에 넣지 않는다. 원문과 무관한 쿠킹 입력이라 job 목록만 바꾼다(8.4절).
- 쿠커가 두 거르기를 장치 없이 실행해야 하므로 pass 셰이더 타입은 `RendererShaders`(Renderer 3.2절)에, 정점 입력 타입 선언은 장치 없는 모듈(16절 MRB8)에 있어야 한다.

### 8.4 사용 대상(정점 입력 타입)

UE는 에디터에서 사용 플래그를 자동으로 켜고 패키지를 dirty로 만든다. 게임에서 빠진 플래그는 기본 머티리얼로 대체된다(8.1절). Rocks는 다음과 같이 한다(결정 MD6).
- **선언.** `Material.usage`에 정점 입력 타입 집합을 저장한다. 새 머티리얼의 기본값은 엔진 기본 정점 입력과 static mesh 정점 입력이다(메시 정점 입력 타입은 메시 단계에서 정한다).
- **쿠킹 유도.** 쿠커는 AssetIndex의 역방향 Hard 의존으로 이 머티리얼(과 그 인스턴스)을 쓰는 메시 에셋 전체의 정점 입력 타입을 모아 선언과 합친다. 쿠킹 대상 집합이 아니라 프로젝트 전체에서 모으므로, 부분 쿠킹과 전체 쿠킹의 결과가 같다.
  - 유도한 집합은 머티리얼 cookKey의 지역 입력이다. 메시가 새로 생기거나 바뀌면 cookKey가 달라져 부분 쿠킹 대상에 든다(AssetSystem 10절 "cookKey가 달라진 항목", 16절 MAS5).
  - 번역 키·번역 결과에는 넣지 않는다. 원문은 사용 대상과 무관하고, 사용 대상은 job 목록만 바꾼다.
- **실행 중 할당.** 코드가 실행 중에 머티리얼을 다른 정점 입력 타입 메시에 붙이는 경우는 쿠커가 알 수 없으므로 선언이 필요하다. 빠졌으면 기본 머티리얼 + 진단이다(11.3절).
- **에디터 보조.** 에디터에서 처음 쓰는 조합이 생기면 머티리얼을 자동으로 바꾸지 않고, "사용 대상에 추가" 진단과 명령을 낸다. 에셋을 사용자 동의 없이 dirty로 만들지 않는다.

### 8.5 증가 억제와 보고

- **품질 수준.** 쿠킹 품질 목록은 프로필이 정한다(기본은 하나). 그래프에 Quality Switch가 없거나 그 수준의 입력이 기본 입력과 같으면 그 수준은 기본 번역을 공유한다(UE와 같은 판정, `RE/Private/Materials/MaterialCachedData.cpp:625–637`). 공유하는 수준은 번역 키의 품질 항목에 "기본" 표지를 써서 키와 결과가 하나가 되게 한다.
- **static 변형 공유.** (루트 머티리얼, 사슬 전체를 합쳐 정규화한 static 값, 합친 기본 속성 override)가 같으면 같은 변형이다. 중간 인스턴스가 달라도 이 셋이 같으면 번역 키가 같다(12.2절). 번역 결과가 같은 서로 다른 조합은 셰이더 집합 id가 같아(8.6절) 셰이더를 한 번만 담는다.
- **dynamic 파라미터는 퍼뮤테이션을 만들지 않는다**(계획 14.1절). Dynamic Bool 파라미터는 셰이더 분기로 낸다. UE도 static bool의 `DynamicBranch`를 uniform 분기로 컴파일한다(`RE/Public/Materials/MaterialExpressionStaticBoolParameter.h:22, 31–32`, `RE/Private/Materials/HLSLMaterialTranslator.cpp:8925–8945`).
- **보고.** 쿠커는 머티리얼마다 축별 수와 job 수, 프로필 전체 합계, 이전 쿠킹 대비 증감을 보고한다(계획 14.1절 "증가량을 cooker가 보고"). 머티리얼당 job 수가 프로젝트 설정 상한을 넘으면 경고, 프로필 상한을 넘으면 쿠킹 오류(설정으로 끌 수 있음)다.
- **편집 중.** 에디터 미리보기는 호스트 형식 하나·정점 입력 하나·미리보기 pass만 컴파일한다(11.4절).

### 8.6 조회 키

머티리얼 셰이더의 런타임 조회 키는 `(셰이더 타입, 정점 입력 타입, 머티리얼 셰이더 집합 id, 퍼뮤테이션 ID, 단계)`다.
- 셰이더 시스템 12.2절 키 + RenderBase SX4(정점 입력 타입) + SX5(묶음 id)에 해당한다.
- 머티리얼 셰이더 집합 id = SHA-256(셰이더 집합 형식 버전, 생성 원문, layout 서명, `MaterialShaderTraits` 정규 바이트)이다. 번역 결과 **내용**의 hash다. 번역 너겟 키에는 툴체인 지문이 들어가 호스트마다 다르므로(셰이더 시스템 10절) 키로 id를 만들면 쿠킹 패키지가 호스트마다 달라진다. 내용 hash는 호스트와 무관하고, 결과가 같은 서로 다른 번역 입력은 같은 id가 된다.
- 엔진 전역 셰이더는 묶음 id 0이다(RenderBase SX5).

## 9. 파라미터

### 9.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 종류 | Scalar·Vector·DoubleVector·Texture·TextureCollection·Font·RuntimeVirtualTexture·SparseVolumeTexture·StaticSwitch·ParameterCollection(실행 중 종류), StaticComponentMask(에디터 전용) | `RE/Public/Materials/MaterialParameters.h:187–207` |
| 식별 | 이름 + 연관(전역·레이어·혼합) + 레이어 index. 전역은 index 없음 | `MaterialParameters.h:25–46` |
| 메타데이터 | 값·primitive data index, 에디터 전용 그룹·설명·범위·정렬·Guid | `MaterialParameters.h:446–497` |
| 상수 layout | preshader 버퍼(`float4` 배열), VT·텍스처 컬렉션 묶음, 텍스처·샘플러 slot(최대 128), 공유 샘플러 `Wrap_`·`Clamp_WorldGroupSettings`. 이름 `"Material"`, 정점 전용 압축판 `"MaterialVS"` | `RE/Private/Materials/MaterialUniformExpressions.cpp:524–700, 960–1001` |
| 기본값 | `DefaultValues` 바이트 표에 오프셋으로 | `RE/Public/MaterialShared.h:729–737, 860`, `MaterialUniformExpressions.cpp:1105–1111` |
| preshader | opcode 집합(상수·파라미터·산술·TextureSize 등) | `RE/Public/Shader/Preshader.h:19–75` |
| 평가 | 렌더 스레드 `EvaluateUniformExpressions`가 버퍼를 채우고 uniform buffer를 만들거나 갱신, 결과를 캐시 | `RE/Private/Materials/MaterialRenderProxy.cpp:458–637`, 캐시 `RE/Public/Materials/MaterialRenderProxy.h:48–85` |
| 값 해석 순서 | proxy 값 → (에디터) 임시 override → 기본값 표 | `RE/Private/Shader/Preshader.cpp:390–465`, `MaterialUniformExpressions.cpp:1526–1543` |
| 인스턴스 | 타입별 해시 맵에 값, 없으면 부모 proxy로 재귀. 게임 스레드 변경은 렌더 명령으로 전달하고 캐시를 무효화 | `RE/Private/Materials/MaterialInstanceSupport.h:207, 259–265, 406–418`, `RE/Private/Materials/MaterialInstance.cpp:295–398, 608–625` |
| static | static switch·component mask·지형 레이어 가중치(랜드스케이프 바인딩)·레이어 구성이 셰이더 맵 키에 들어간다 | `RE/Public/StaticParameterSet.h:396–468`, `RE/Public/MaterialShared.h:1345–1348` |
| 컬렉션 | 머티리얼당 최대 2개(초과 시 번역 오류), 컬렉션 크기 `r.MPC.MaxVectorStorage` 1280(초과는 경고만), `float4` 배열 하나짜리 uniform buffer, 씬의 버퍼 → 없으면 기본 인스턴스 | `RE/Public/ParameterCollection.h:13–18`, `RE/Private/Materials/HLSLMaterialTranslator.cpp:5069–5072`, `RE/Private/Materials/ParameterCollection.cpp:25–30, 798–830`, `RR/Private/ShaderBaseClasses.cpp:119–141` |
| 함수 안 파라미터 | 일반 함수 호출은 연관을 바꾸지 않으므로 전역 연관이다. 그래서 같은 이름은 같은 파라미터가 된다(레이어·혼합 호출만 연관을 바꾼다, `RE/Private/Materials/MaterialExpressions.cpp:7238–7239, 7273–7274`) | 근거에서 추론. 번역기의 파라미터 소유자 스택(`RE/Private/Materials/HLSLMaterialTranslator.h:803–807`) 전체 경로는 추적하지 않았다(미확인) |

### 9.2 식별

`ParameterKey = { ownerAssetId, ParameterId, scope }`.
- **ownerAssetId** = 파라미터 노드가 있는 에셋(머티리얼 또는 함수)이다. 계획 14.7절의 "MaterialAssetId·ParameterId·layer instance 경로·function call-site NodeId 경로"와 같다.
- **scope** = 전역 | 함수 호출 경로(호출 위치 NodeId 목록) | 레이어(레이어 Guid, 그 안의 호출 경로) | 혼합(혼합 Guid, 그 안의 호출 경로).
- 계획 14.7절대로 함수 안 파라미터는 **호출 위치마다 독립**이다. 같은 함수를 두 번 부르면 두 파라미터다. 여러 호출이 공유해야 하는 파라미터는 파라미터 노드의 "공유" 표시로 전역 scope에 둔다. 이것은 UE와 다르다(9.1절: UE는 같은 이름이면 합쳐진다).
- **이름**은 표시·검색용이다. 인스턴스 에디터는 이름과 그룹으로 보여 주고, 저장은 키로 한다. 같은 scope의 같은 이름은 V8 오류다.
- 레이어 순서를 바꿔도 레이어 Guid가 그대로라 override가 남는다. UE는 index를 다시 매긴다(`RE/Public/Materials/MaterialParameters.h:78, 134` `RemapLayerIndex`).

### 9.3 종류와 단계

| 종류 | 값 | 재컴파일 | 단계 |
|---|---|---|---|
| Scalar | float | 없음 | MAT0 |
| Vector | float4(색·벡터) | 없음 | MAT0 |
| Texture | 텍스처 에셋 참조(종류는 그래프가 고정) | 없음(종류가 같으면) | MAT0 |
| Dynamic Bool | bool(셰이더 분기) | 없음 | MAT2 |
| Static Bool | bool | static 변형 | MAT0 |
| Static Component Mask | RGBA 4비트 | static 변형 | MAT2 |
| Static Enum(preset 선택 등) | 정수 | static 변형 | MAT2 |
| Collection 참조 | 컬렉션 에셋 | static 변형(layout이 원문에 들어간다) | MAT2 |
| Layer Stack | 레이어·혼합 함수 목록 | static 변형 | MAT3 |
| Double Vector(절대 위치 등) | double4 | 없음 | 후속(`WorldPosition` 파라미터가 필요할 때) |
| 가상 텍스처·텍스처 컬렉션·sparse volume·font | — | — | 후속(UE 종류는 대응표에만) |

### 9.4 상수 식 프로그램과 상수 블록

- **상수 식 프로그램**(`ConstantProgram`): IR의 `Uniform` 빈도 출력마다 명령열 하나다. 명령은 스택 기계 바이트코드이며 형식 버전을 가진다.
  - 명령: 상수, 파라미터 읽기(layout slot index), 컬렉션 값 읽기, 사칙·min·max·abs·floor·frac·sqrt·saturate·lerp·dot·cross·normalize·length·sin·cos·pow·exp2·log2, 비교·선택, swizzle·구성. 목록은 노드 묶음(5.7절)에 맞춰 MS2에서 확정한다.
  - UE preshader opcode(`RE/Public/Shader/Preshader.h:19–75`)와 대부분 겹친다. UE에는 Lerp·Pow 대신 Tan·Asin·Atan2·Fmod·Round·Sign·Log10 등이 있다.
  - 텍스처 크기는 명령이 아니라 텍스처 slot의 논리 크기 상수다(7.4절, TextureStreaming 5.3절).
  - 실행은 float32다. 실행 결과는 쿠킹 바이트가 아니므로 결정성 규칙(AssetSystem 10절) 대상이 아니다. 다만 같은 실행 파일에서는 같은 입력에 같은 결과를 낸다(멀티스레드 평가도 순서 무관).
- **상수 블록 layout**(`MaterialConstants`): 머티리얼 컴파일러가 7.4절 5항 규칙으로 정한다.
  - 순서: float 계열 slot(상수 식 출력을 크기 큰 것부터, 같은 크기는 값 번호 순으로 채움) → int·uint slot → bool 비트 slot.
  - 텍스처는 상수 블록 밖의 자원 slot `MaterialTexture<k>`(k는 첫 사용 값 번호 순)이다. Texture Size를 쓰는 slot은 논리 크기 상수를 float slot에 더한다.
  - layout 표 = 멤버 목록 + 텍스처 목록(종류·샘플 종류·샘플러 출처) + 상수 식 출력 → 멤버 대응.
  - UE는 preshader 버퍼를 `float4` 배열 하나로 두고 이름 없이 오프셋으로 접근한다(`MaterialUniformExpressions.cpp:642`). Rocks는 GLES 에뮬레이션의 타입 분리 규칙 때문에 float·int·uint를 나눈다.
- **평가 위치.** 렌더 lane이 `MaterialRenderState`마다 (파라미터 version, 컬렉션 version)이 바뀐 때만 상수 식을 실행하고 상수 블록을 채운다(UE와 같은 위치, 9.1절). 게임 lane은 바뀐 파라미터 값만 프레임 packet으로 보낸다(RenderBase 1절 "불변 값 묶음"). 평가가 많으면 CP2 병렬 실행을 쓴다(구현 때 측정).
- **상수 블록 수명.** 머티리얼 렌더 상태의 상수 블록은 `Persistent`(RenderBase 9.4절)다. 값이 바뀌면 새 블록을 할당하고 이전 블록은 `Retire`한다(진행 중 프레임이 읽고 있을 수 있다). GLES는 CPU 사본이고 업로드 판단은 RenderBase 9.4절대로다.

### 9.5 static 파라미터와 변형

- static 값(Static Bool·Component Mask·Enum·컬렉션 참조·레이어 구성)과 **기본 속성 override**(혼합 방식·양면·마스크 기준값·shading preset)를 인스턴스가 바꾸면 static 변형이 생긴다. UE도 기본 속성 override가 있으면 static 변형을 만든다(`RE/Private/Materials/MaterialInstance.cpp:2415–2424`, 키 `RE/Public/MaterialShared.h:1376` `BasePropertyOverridesHash`).
- 변형은 부모 머티리얼 그래프 + 인스턴스 사슬의 static 값으로 번역한다. 번역 너겟 키에 정규화한 static 값 목록이 들어간다(12.2절).
- static 변형이 없는 인스턴스는 부모 산출물(셰이더 집합)을 그대로 쓰고 자기 값 배열만 가진다.
- 에디터는 static 값을 바꿀 때 "새 변형 컴파일" 비용을 표시한다(계획 14.5절 "static variant"). dynamic 값만 바꾸면 compile job은 0이다(계획 MT04).
- `DynamicMaterialInstance`는 static 값을 바꿀 수 없다(`UnsupportedSetting`, 4.2절).

### 9.6 인스턴스 override 해석

- **평탄화.** 인스턴스를 로드하거나 편집할 때, 셰이더 집합을 가진 조상(머티리얼 또는 static 변형)의 layout 기준으로 값 배열을 만든다. slot마다 가장 가까운 override, 없으면 머티리얼 기본값이다.
  - 부모가 바뀌면(편집·hot reload) 자손 인스턴스의 배열을 다시 만든다. 자손 목록은 AssetIndex 역방향 의존으로 찾는다.
  - 그리기 경로에서 부모를 거슬러 찾지 않는다. UE는 렌더 스레드에서 부모 proxy로 재귀한다(9.1절).
- **`DynamicMaterialInstance`.** 부모의 평탄화 배열을 복사해 시작하고, `Set(ParameterKey, value)`로 slot 하나를 바꾸고 version을 올린다. slot index를 미리 얻는 API(`FindSlot`)를 두어 매 프레임 키 검색을 피한다. UE도 index 기반 설정을 둔다(`RE/Public/Materials/MaterialInstanceDynamic.h:41, 48`).
- **텍스처 값.** 텍스처 override는 텍스처 에셋의 논리 handle이다. 실제 장치 자원은 그래프 구성 때 TextureStreaming의 바인딩 snapshot으로 해석한다(TextureStreaming 결정 "교체 게시": descriptor를 제자리에서 덮지 않는다). 그래서 스트리밍이 mip을 바꿔도 머티리얼 상수 블록을 다시 만들지 않는다. UE도 간접 참조를 묶어 같은 효과를 낸다(`RE/Private/Materials/MaterialUniformExpressions.cpp:1712–1729`).
- **layout이 바뀔 때(hot reload).** 새 layout의 slot을 `ParameterKey`로 찾아 이전 값을 옮긴다. 없어진 키의 override는 에셋에는 남기고(복원 가능) 진단으로 표시한다. 타입이 바뀐 키는 기본값으로 두고 진단한다.

### 9.7 파라미터 컬렉션

- **에셋.** scalar·vector 파라미터 목록(`ParameterId`·이름·기본값). layout = scalar를 4개씩 묶은 `float4` + vector `float4` 배열이다(UE와 같은 배치, `RE/Private/Materials/ParameterCollection.cpp:720–731`).
- **머티리얼당 개수.** 처음에는 2개다. UE 값과 같고(`RE/Public/ParameterCollection.h:13–18`), 데스크톱 공통 상수 버퍼 상한 12(셰이더 시스템 7.2절 4항)에서 Renderer 상수 slot(Renderer 15.2절)과 머티리얼 블록을 뺀 여유 안이다. 늘리려면 그 slot 수를 본 뒤 정한다.
- **크기 상한.** 컬렉션 하나의 `float4` 수 상한을 형식 정의 상한(GLES uniform 벡터 수)에서 정한다. 넘으면 오류다(UE는 경고만 낸다).
- **바인딩.** 컬렉션은 공유 상수 slot(RenderBase 9.5절)이다. slot 등록은 정적 표가 아니라 컬렉션 에셋 identity로 하는 동적 등록이다(16절 MRB4). 생성 원문은 `cbuffer MaterialCollection<k>`로 내고, layout 서명은 컬렉션 layout의 SHA-256이다.
- **값.** World마다 `ParameterCollectionState`가 있고 게임 lane이 값을 바꾼다. 바뀐 값은 프레임 packet으로 넘어가고 렌더 lane이 컬렉션 상수 블록을 갱신한다.
- **키.** 컬렉션 **layout**(파라미터 목록·순서)이 번역 키에 들어간다. 기본값·실행 중 값은 들어가지 않는다. UE는 컬렉션 StateId를 셰이더 맵 키에 넣는다(`RE/Public/MaterialShared.h:1351–1354`).

## 10. shading model과 출력

### 10.1 렌더러와의 경계

Renderer 스펙 14.6절 경계표와 같은 나눔이다. 아래는 그 표에 머티리얼 쪽 항목을 더한 것이다.

| 항목 | 머티리얼 시스템(이 문서) | Renderer 스펙 |
|---|---|---|
| `SurfaceAttributes` 필드와 의미 | 그래프(BSDF 트리)를 이 필드로 낮춘다(10.3절) | 정한다(14.1절) |
| shading model·블렌드 모드·영역 번호표 | 참조한다. 노드·이름 표시·검증·UI | 소유한다(`RendererShaders`, 결정 RN5) |
| GBuffer 배치·인코딩, BRDF·직접광 평가 | — | 정한다(7.1·14.4절) |
| pass 목록·pass 셰이더·pass 퍼뮤테이션 차원 | 거르기 입력(`MaterialShaderTraits`)을 준다 | 정한다(14.3절) |
| 머티리얼 셰이더 집합 키·쿠킹 단위·static 변형 | 정한다(8.6·12절) | 조회 키만 요구(14.5절) |
| 그래프·IR·HLSL 생성·인스턴스·파라미터 갱신 | 정한다 | — |
| 문맥 구조체 `MaterialInputs`·`MaterialVertexInputs` | 정한다(7.3절) | pass 원문이 채운다(정점 입력 타입과 함께) |
| (pass, 퍼뮤테이션)별 머티리얼 텍스처 상한 | 계산해 진단·쿠킹 검사에 쓴다(8.3·14절) | `16 − 렌더러 합계` 표를 정한다(7.4절, 결정 RN13) |
| scene texture 읽기 노드 | 노드 검증(V6) | 경로별 가능 여부(14.6절) |
| WPO 현재·이전 | 정점 함수로 낸다(7.3절) | depth·velocity·그림자 pass가 쓴다 |
| 렌더 경로·기능 등급 정의 이름 | 생성 원문의 `#if`에 쓴다 | 정한다 |
| 기본 머티리얼 대체 | 대체를 결정한다(11.3절) | 표시 방식을 정한다 |

### 10.2 UE 5.8 조사: Substrate와 고전 shading model

**Substrate 기본 여부.**

| 항목 | 값 | 근거 |
|---|---|---|
| 엔진 CVar `r.Substrate` | 기본 0, ReadOnly | `RC/Private/RenderUtils.cpp:2068–2072` |
| 새 프로젝트 | `GameProjectUtils`가 DefaultEngine.ini에 `r.Substrate=True`와 `r.Substrate.ProjectGBufferFormat=0`(Blendable)을 쓴다(템플릿에 값이 있으면 유지). 정책 주석 "New project will using Substrate with Blendable GBuffer" | `ED/GameProjectGeneration/Private/GameProjectUtils.cpp:199–211`, `RC/Private/RenderUtils.cpp:2074–2083` |
| 고급 템플릿 | AEC·PhotoStudio 등은 Adaptive(=1) 명시 | `Templates/TP_AEC_ArchvisBP/Config/DefaultEngine.ini:43–44`, `Templates/TP_PhotoStudioBP/Config/DefaultEngine.ini:65–70`(리포지토리 루트 기준) |
| Blendable 판정 | 모바일 deferred이거나, 플랫폼이 Adaptive를 지원하지 않거나, 프로젝트가 Adaptive를 요청하지 않으면 Blendable | `RC/Private/RenderUtils.cpp:2226–2233` |
| Adaptive 지원 플랫폼 | PCD3D_SM6·METAL_SM6(Mac·iOS)·VULKAN_SM6만 | `CFG/Windows/DataDrivenPlatformInfo.ini:78, 135`, `CFG/Mac/DataDrivenPlatformInfo.ini:68, 113`, `CFG/IOS/DataDrivenPlatformInfo.ini:81, 125`, `CFG/VulkanPC/DataDrivenPlatformInfo.ini:141, 251` |
| Blendable 제약 | 픽셀당 20 byte, closure 1개 강제, glint 없음, rough refraction·확률적 조명 없음 | `RC/Private/RenderUtils.cpp:2255–2261, 2328–2373, 2441, 2237, 2546` |
| SM5 플랫폼 | PCD3D_SM5·VULKAN_SM5는 closure 최대 1 | `CFG/Windows/BaseWindowsEngine.ini:50–60` |
| Blendable 저장 | Substrate 데이터를 고전 GBuffer 데이터로 내보낸다 | `SH/Private/BasePassPixelShader.usf:1927–1948`, `SH/Private/Substrate/SubstrateExport.ush:1058` |
| Adaptive 저장 | 고전 GBufferA–C를 끄고 top layer(노멀 22·거칠기 8·종류 2비트)와 material 배열(`r.Substrate.BytesPerPixel` 기본 80) | `RC/Private/GBufferInfo.cpp:265–328`, `SH/Private/Substrate/Substrate.ush:922–940`, `RR/Private/Substrate/Substrate.cpp:646–656`, `RC/Private/RenderUtils.cpp:2092–2096` |

**고전 경로의 레거시 상태.**
- C++·열거 수준 deprecation은 없다. `EMaterialShadingModel` 값에 deprecated 표시가 없다(`RE/Classes/Engine/EngineTypes.h:706–728`).
- 셰이더 함수에는 `// UE_DEPRECATED 5.7 - Deprecated by Substrate` 주석이 있다: GBuffer 인코딩(`SH/Private/DeferredShadingCommon.ush:966`), 고전 동적 조명 누적(`SH/Private/DeferredLightingCommon.ush:315`), BxDF 적분(`SH/Private/ShadingModels.ush:1136`).
- Substrate를 켜도 고전 루트 노드 에셋은 자동으로 다시 쓰지 않는다(`IsHiddenMaterialAssetConversionEnabled`가 하드코딩 true, `RC/Private/RenderUtils.cpp:2564–2570`). Blendable에서 FrontMaterial이 연결되지 않은 머티리얼은 고전 머티리얼 셰이더로 컴파일된다(`RE/Private/Materials/SubstrateTranslatorCommon.cpp:1579–1593`).
- 정리: 5.8에서 고전 경로는 "지원되는 선택지이지만 셰이더 수준에서 Substrate로 대체 예고된 경로"다. 새 프로젝트 기본은 Substrate다.

**Substrate 구조(참고).**
- BSDF 노드: Slab(DiffuseAlbedo·F0·F90·Roughness·Anisotropy·Normal·Tangent·SSS MFP·Emissive·SecondRoughness·Fuzz·Glint·Profile 등, `RE/Public/Materials/MaterialExpressionSubstrate.h:273–436`, 입력 `:281–405`), Hair·Eye·Single Layer Water·Unlit·Volumetric Fog Cloud·Light Function·PostProcess·UI·Toon(Experimental)(`:508–980`, Toon `:860–861`).
- 연산: Horizontal Blend·Vertical Layer·Add·Coverage Weight·Select(`:1135–1338`), 셰이더 상수(`SH/Shared/SubstrateDefinitions.h:59–65`).
- 한도: closure 최대 8, 연산 15, 트리 깊이 48(`SubstrateDefinitions.h:16–22`, `RE/Classes/Engine/EngineTypes.h:840`).
- 예산 초과 시 연산을 깊이 순으로 parameter blending하고 slab을 단순화한다(`RE/Private/Materials/SubstrateTranslatorCommon.cpp:624–713, 1319–1433`).
- 고전 shading model → Substrate 변환 노드(`MaterialExpressionSubstrate.h:72–271`)와 셰이더 쪽 변환(`SH/Private/Substrate/SubstrateLegacyConversion.ush:153`). 대응: Subsurface → SubsurfaceWrap, PreintegratedSkin → SubsurfaceMFP, TwoSidedFoliage → SubsurfaceThinTwoSided(`RE/Private/Materials/Material.cpp:3425–3439`).

**도메인·혼합 방식(레거시 확인).**

| 열거 | 값 | 상태 | 근거 |
|---|---|---|---|
| 도메인 | Surface·DeferredDecal·LightFunction·Volume·PostProcess·UI | 활성 | `RE/Public/MaterialDomain.h:11–30` |
| 〃 | RuntimeVirtualTexture | **Deprecated**, Hidden | `MaterialDomain.h:26–27` |
| 혼합 방식 | Opaque·Masked·Translucent·Additive·Modulate·AlphaComposite·AlphaHoldout | 활성 | `RE/Classes/Engine/EngineTypes.h:244–259` |
| 〃 | TranslucentColoredTransmittance | Substrate 전용(dual source blending 요구) | `EngineTypes.h:254`, `RE/Private/Materials/HLSLMaterialTranslator.cpp:15000–15001` |
| 〃 | TranslucentGreyTransmittance·ColoredTransmittanceOnly | Hidden 별칭 | `EngineTypes.h:256–258` |
| shading model | Unlit·DefaultLit·Subsurface·PreintegratedSkin·ClearCoat·SubsurfaceProfile·TwoSidedFoliage·Hair·Cloth·Eye·SingleLayerWater·ThinTranslucent·FromMaterialExpression | 활성(열거 수준) | `EngineTypes.h:706–728` |
| 〃 | Strata(Substrate) | Hidden | `EngineTypes.h:721` |

### 10.3 Rocks 표면 모델(결정 MD1)

**그래프 모델은 Substrate형이고, 렌더러에는 `SurfaceAttributes`로 내보낸다.**
- 출력 노드의 `Surface` 입력은 BSDF 트리다. BSDF 노드는 `SurfaceSlab`·`HairBsdf`·`EyeBsdf`·`WaterBsdf`·`UnlitBsdf`이고, 연산 노드는 `SurfaceMix`(수평)·`SurfaceStack`(수직)·`SurfaceAdd`·`SurfaceCoverage`·`SurfaceSelect`다.
- **shading preset 노드**는 Renderer 스펙 14.2절의 shading model마다 하나다: `Unlit`·`DefaultLit`·`ClearCoat`·`Subsurface`·`SubsurfaceProfile`·`TwoSidedFoliage`·`Cloth`·`Eye`·`ThinTranslucent`·`SingleLayerWater`(`Hair`는 P3). 고전 입력(baseColor·metallic·specular·roughness 등)을 받아 BSDF 트리를 낸다. UE 변환 노드와 역할이 같다. 새 머티리얼의 기본 그래프는 `DefaultLit` preset 하나가 출력에 이어진 모양이라 고전 사용 경험과 같다.
- **내보내기.** 번역기의 마지막 단계가 IR의 `Surface` 값을 Renderer 스펙 14.1절 `SurfaceAttributes`(baseColor·metallic·specular·roughness·normal·emissive·ambientOcclusion·opacity·opacityMask·shadingModel·customData0·customData1·tangent·anisotropy)로 낮춘다.
  - preset 노드의 고전 입력은 변환 없이 그대로 지나간다. UE도 고전 변환 머티리얼은 고전 GBuffer 데이터를 그대로 넘기는 표지를 둔다(`RE/Public/Rendering/SubstrateMaterialShared.h:180–237`의 컴파일 출력).
  - `SurfaceSlab`의 직접 입력(DiffuseAlbedo·F0·F90)을 고전 값으로 바꾸는 변환은 UE Blendable의 내보내기(`SH/Private/Substrate/SubstrateExport.ush:1058`, 호출 `SH/Private/BasePassPixelShader.usf:1927–1948`)에 대응한다. 변환식은 SF3에서 정한다. 그 전에는 preset 노드만 연다.
  - `customData0`·`customData1`의 shading model별 의미는 Renderer 스펙이 정한다(14.1절 "clear coat 강도·roughness 등").
- **closure 하나로 접기.** 번역기는 BSDF 트리를 closure 하나로 접는다. 연산은 parameter blending(입력 값의 가중 혼합)으로 바꾼다. 결과 BSDF와 기능 조합에서 shading model 하나를 고른다(아래 표). 표로 나타낼 수 없는 조합(예: fuzz와 2차 반사를 함께 쓰는 slab, slab과 hair의 혼합)은 정해진 우선순위로 하나를 고르고 경고한다. UE Blendable이 closure 하나를 강제하고 예산을 넘으면 단순화하는 것과 같은 성격이다(10.2절).

| 접은 BSDF·기능 | shading model(Renderer 14.2절) |
|---|---|
| `UnlitBsdf` | `Unlit` |
| slab(추가 기능 없음) | `DefaultLit` |
| slab + 2차 반사 | `ClearCoat` |
| slab + SSS wrap | `Subsurface` |
| slab + SSS profile | `SubsurfaceProfile` |
| slab + 양면 wrap | `TwoSidedFoliage` |
| slab + fuzz | `Cloth` |
| slab + 얇은 투과 | `ThinTranslucent` |
| `EyeBsdf` | `Eye` |
| `WaterBsdf` | `SingleLayerWater` |
| `HairBsdf` | `Hair`(P3, 계획 11.1절) |

- **shading model이 하나인 머티리얼.** 트리가 static으로 shading model 하나로 정해지면 생성 원문에 `MATERIAL_SINGLE_SHADING_MODEL`을 내고, Renderer는 그 분기만 컴파일한다(Renderer 14.4절). 실행 중 선택(`SurfaceSelect`의 dynamic 입력)이면 픽셀마다 `shadingModel` 값을 쓴다.
- **여러 closure 저장은 계획 14.4절 연구 gate**다(Renderer 7.1절도 Blendable과 같은 고정 배치로 시작한다). IR의 `Surface` 타입은 처음부터 트리이므로, gate를 통과하면 접기·내보내기 단계만 기능 등급별로 바꾸면 된다. 그래프·에셋 migration은 필요 없다.

**블렌드 모드.** Renderer 스펙 14.2절 목록(`Opaque`·`Masked`·`Translucent`·`Additive`·`Modulate`·`AlphaComposite`)을 쓴다. UE 5.8의 `AlphaHoldout`(활성)과 Substrate 전용 색 투과 반투명은 그 목록 밖이라 `Unsupported`로 표시한다(10.4절 D4).

**GBufferD가 필요한 shading model.** `ClearCoat` 등은 GBufferD를 쓰므로 Deferred에서는 MRT 8 형식에서만 지원된다(Renderer 7.1절, 결정 RN3·RN4). GLES Deferred에서는 `Unsupported`이고 Forward+에서는 지원된다. 한 경로만 되는 조합은 그 경로의 쿠킹 오류다(Renderer 14.2절). 머티리얼 컴파일러는 이 조건을 V6 capability 검사로 진단한다.

**단계.**

| 단계 | 표면 범위 | 계획·Renderer 대응 |
|---|---|---|
| SF1 | `DefaultLit`·`Unlit` preset, `Opaque`·`Masked` | MAT0–MAT1(M3), Renderer RS1·RS2 |
| SF2 | `Translucent`·`Additive`·`Modulate`·`AlphaComposite` | MAT2–MAT3, Renderer 블렌드 모드 단계(RS0–RS5) |
| SF3 | `ClearCoat`·`Subsurface`·`SubsurfaceProfile`·`TwoSidedFoliage`·`Cloth`·`Eye`·`ThinTranslucent`·`SingleLayerWater` preset, `SurfaceSlab` 직접 입력, 표면 연산(parameter blending) | MAT4, 계획 11.1절 "M6–M8 고급 공통 재질", Renderer RS9 |
| Hair | `Hair` preset·`HairBsdf` | 계획 11.1절 P3 별도 gate, Renderer 14.2절 |
| SF4 | 여러 closure 저장(기능 등급 SM6 형식만), glint·rough refraction | 계획 14.4절 연구 gate |

**결정 전 대안 (가)(고전 enum).** 출력 노드가 baseColor·metallic·specular·roughness·normal·shadingModel 핀을 직접 가지는 방식이다. 지금 렌더러 계약(`SurfaceAttributes`)과는 바로 맞지만, 이후 Substrate형으로 옮기려면 UE처럼 에셋 변환이 필요하다(UE `RE/Private/Materials/Material.cpp:3522–3569`). 결정 MD1은 (나)이다.

### 10.4 Renderer 문서와 다른 점(Renderer 개정 2, 2dc9068 기준)

맞추는 작업은 두 문서의 리뷰 단계에서 함께 한다. Renderer 문서는 고치지 않았다.

| ID | Renderer 스펙 | 이 문서 | 제안 |
|---|---|---|---|
| D1 | 14.1절 표가 `worldPositionOffset`(VS)을 `SurfaceAttributes` 필드로 둔다 | WPO는 정점 단계 값이라 별도 정점 함수(현재·이전)로 낸다(7.3절). `SurfaceAttributes`는 픽셀 함수 결과다 | Renderer 표에서 WPO를 "정점 함수 결과"로 분리 |
| D2 | 번호표는 `RendererShaders`가 소유하고 머티리얼 스키마가 참조(결정 RN5). 의존은 `RendererShaders` → MaterialSchema(3.2절) | MaterialSchema가 번호표를 참조하려면 `RendererShaders`에 의존해야 해 **순환**이 생긴다 | 소유는 Renderer 그대로 두고, 번호표의 C++ 정의는 장치 없는 `RenderData`(MaterialSchema가 의존 가능)에, HLSL 상수는 `/Engine/Shared/`의 공용 `.h`에 둔다 |
| D3 | 결정 RN13 (가): 모든 형식 16, 머티리얼 텍스처 = `16 − 렌더러 합계`. 선택 자원을 많이 켠 조합은 1–3개 | 초안 권장은 (다)(공유 샘플러 + 데스크톱 SRV 상한을 높인 형식)였다. 사용자 확정(RN13)에 맞춰 (가)로 적었다(결정 MD4) | **위험:** 일반 PBR 머티리얼(색·노멀·ORM에 디테일 1–2장)은 3–5장이다. 무거운 퍼뮤테이션에서 쿠킹 오류가 흔할 수 있다. 측정 MM5로 실제 분포를 본 뒤 RN13 (나)·(다)를 다시 볼 조건을 둔다. 공유 샘플러만으로는 SRV 상한이 풀리지 않는다(샘플러 수만 준다) |
| D4 | 14.2절 블렌드 모드에서 `AlphaHoldout`과 Substrate 전용 값을 뺀다 | UE 5.8에서 `AlphaHoldout`은 활성 값이다(`RE/Classes/Engine/EngineTypes.h:244–259`). 이 문서는 Renderer를 따라 `Unsupported`로 둔다 | `AlphaHoldout`을 후속으로 둘지 Renderer가 명시 |
| D5 | 14.3절 거르기 키는 (블렌드 모드, 영역, shading model, 정점 입력 타입, 형식) | `MaterialShaderTraits`는 WPO·마스크·capability·custom 출력·투명 pass 종류·velocity·그림자 caster를 더 가진다(8.3절) | Renderer 거르기 함수가 `MaterialShaderTraits`를 받는 것으로 통일(Renderer 15.3절 "머티리얼 정적 속성"과 같은 뜻) |
| D6 | 14.6절 "머티리얼 static switch 차원과 곱한다" | static 값은 퍼뮤테이션 차원이 아니라 static 변형(따로 번역, 다른 셰이더 집합 id)이다(9.5절). 곱해지는 수는 같다 | 용어만 맞춘다 |
| D7 | 14.3절 pass 표에 경로 전용과 공통 구분이 없다 | Render Path Switch는 경로 전용 pass(GBuffer·ForwardOpaque)에서 그 경로 입력을, 공통 pass(Depth·ShadowDepth·Translucent·Velocity 등)에서 Default 입력을 쓴다(6.3절) | pass 표에 "경로" 열과 정의 이름(16절 MRD9) |
| D8 | 14.1절 함수 `EvaluateMaterialSurface(MaterialInputs) → SurfaceAttributes` | 그대로 쓴다. 더해 `EvaluateMaterialOpacityMask`(depth·shadow pass용)·정점 함수·custom 출력 함수를 낸다(7.3절) | Renderer 14.3절 Depth·ShadowDepth가 마스크 전용 함수를 부르도록 |
| D9 | 14.2절 영역 이름 `DeferredDecal`·`UserInterface` | 이 문서 초안의 `Decal`·`UI`를 Renderer 이름으로 바꿨다 | 없음 |

## 11. 비동기 컴파일·대체 머티리얼·미리보기·hot reload

### 11.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 캐시 조회 | 메모리 셰이더 맵 → 비동기 DDC 조회 → 없으면 컴파일 시작. 쿠킹 데이터이거나 컴파일할 수 없으면 셰이더 맵 없음(기본 머티리얼 사용). 기본 머티리얼 자신이 그렇게 되면 Fatal | `RE/Private/Materials/MaterialShared.cpp:2998–3219`(조회 `:3043, 3066`, 실패 `:3150–3176`) |
| 컴파일 | 번역 → 컴파일 환경 → 셰이더 맵 `Compile`. 비동기면 완성 전 복제본을 게임 스레드 셰이더 맵으로 둔다 | `MaterialShared.cpp:3714–3816` |
| 우선순위 | 동기 High, 비영속(미리보기) Normal, 그 밖 Low | `RE/Private/Materials/MaterialShader.cpp:2904–2915` |
| 동기 컴파일 | 기본 머티리얼은 항상 동기 | `MaterialShared.cpp:2351–2354` |
| 컴파일 모드 | None·Background·Synchronous, 기본 Background | `RE/Public/MaterialShaderPrecompileMode.h:6–13` |
| 그리는 동안 대체 | 셰이더 맵이 없거나 미완성이면 대체 사슬을 따라 완성된 머티리얼을 쓴다. 에디터는 이때 미뤄 둔 job을 제출한다 | `RE/Private/Materials/MaterialRenderProxy.cpp:870–893` |
| 대체 대상 | 도메인 기본 머티리얼(`UMaterial::GetDefaultMaterial(Domain)`). 기본 머티리얼은 대체가 없다 | `RE/Private/Materials/Material.cpp:441–450, 555–558, 1094–1109` |
| 기본 머티리얼 에셋 | `WorldGridMaterial`(Surface), `DefaultLightFunctionMaterial`, `DefaultDeferredDecalMaterial`, `DefaultPostProcessMaterial` | `CFG/BaseEngine.ini:155–156, 219–220` |
| 편집 반영 | `PostEditChange`가 렌더 명령을 비우고 진행 중 컴파일을 취소한 뒤 다시 캐시. 미리보기 머티리얼(`bIsPreviewMaterial`)은 전역 컴포넌트 재생성을 건너뛴다 | `RE/Private/Materials/Material.cpp:5356–5438`, `ED/MaterialEditor/Private/MaterialEditor.cpp:695` |
| 갱신 문맥 | `FMaterialUpdateContext`(컴포넌트 재등록·렌더 스레드 동기·render state 재생성) | `RE/Public/MaterialShared.h:3480–3535` |
| 노드 미리보기 | 노드를 출력으로 삼은 별도 `FMaterial`·proxy. 생성 때 `CacheShaders(None)`, 갱신 때 지연 삭제 | `ED/MaterialEditor/Private/MaterialEditor.h:57, 117`, `MaterialEditor.cpp:7634–7691` |
| `UMaterialInstance` proxy의 대체 구현 | — | 미확인(열람하지 않음) |

### 11.2 컴파일 흐름(에디터)

`CompileCoordinator`(EditorMaterial)가 맡는다. 계획 14.8절의 규칙(transaction revision·debounce·결과 수용 조건·취소 수명)을 구현하는 순서는 다음과 같다.
1. 편집 transaction이 끝나면 semantic hash를 다시 계산한다. 같으면(화면 편집만) 아무것도 하지 않는다.
2. debounce 뒤 번역 요청을 만든다. 입력은 그래프·Build 의존의 immutable snapshot과 미리보기 프로필이다.
3. **번역**은 메인 프로세스의 작업(WorkerScheduler)으로 한다. 번역 너겟을 먼저 찾는다. 번역은 도구·장치가 필요 없다. 걸리는 시간은 측정 MM1로 확인하고, 길면 별도 프로세스로 옮길지 정한다.
4. 번역 결과에서 job 목록(미리보기 프로필의 형식·정점 입력·pass)을 만들어 ShaderCompiler에 제출한다. 우선순위는 열린 머티리얼의 미리보기 > 선택 노드 미리보기 > 화면에 보이는 다른 머티리얼 > 백그라운드다(16절 MSR7).
5. 결과가 오면 수용 조건(세션 epoch·현재 semantic hash·프로필·compile generation)을 확인하고, 미리보기 프로필 범위의 완결성을 검사한다.
6. 통과하면 새 `MaterialShaderSet`(새 epoch)을 렌더 lane에 넘기고 프레임 경계에서 교체한다(RenderBase 9.8절, 11.5절).

### 11.3 대체 규칙

| 상황 | 그리는 것 | 표시 |
|---|---|---|
| 에디터, 처음 컴파일 중 | 도메인 기본 머티리얼 | 컴파일 중 표시 |
| 에디터, 성공한 적이 있고 다시 컴파일 중 | 마지막 성공 셰이더 집합(계획 14.8절) | "현재 그래프와 다른 마지막 성공 버전" |
| 에디터, 컴파일 실패 | 마지막 성공 셰이더 집합, 없으면 기본 머티리얼 | 오류 위치(노드·핀) |
| 쿠킹 실행, 필요한 셰이더가 라이브러리에 없음(사용 대상 누락 등) | 도메인 기본 머티리얼 | Development: 머티리얼·정점 입력 타입마다 한 번 오류 로그. Release: 오류 계수(진단 API)만 |
| 쿠킹 실행, 머티리얼 셰이더 집합 서명 불일치 | 도메인 기본 머티리얼 | `IncompatibleCook` 진단 |
| 엔진 기본 머티리얼이 없거나 불완전 | — | 렌더 lane 시작 실패(`IncompatibleCook`, RenderBase 9.2절 완결성 검사) |

**검은 표면 금지.** 계획 11.1절대로 실패를 검은색이나 투명으로 처리하지 않는다. Surface 기본 머티리얼은 눈에 띄는 격자 무늬다(UE와 같은 성격, `CFG/BaseEngine.ini:155`).

**엔진 기본 머티리얼.**
- 영역마다 하나(Surface·DeferredDecal·LightFunction·PostProcess·UserInterface, Volume은 후속)다. 엔진 에셋이며, 그 셰이더는 엔진 내장 셰이더 라이브러리(셰이더 시스템 12.3절)에 들어간다.
- 모든 엔진 정점 입력 타입과 모든 pass에 대해 컴파일한다(`isDefaultMaterial` 특성, 8.3절). 그래서 사용 대상 거르기를 받지 않는다.
- 렌더 lane 시작의 엔진 셰이더 완결성 검사(RenderBase 9.2절)에 포함한다. UE는 기본 머티리얼을 처음 쓸 때 동기 컴파일하지만, Rocks는 쿠킹 라이브러리에서 시작 때 검사한다.
- 엔진 내장 라이브러리를 만드는 `shader-compile --engine`(셰이더 시스템 12.3절)은 지금 `Engine/Shaders` 원문만 다룬다. 엔진 기본 머티리얼 에셋을 번역해 넣는 경로를 더해야 한다(16절 MSR11). 필요 시 컴파일 형식(`GLES31_PC`)은 RenderBase SX14 셰이더 공급자가 엔진 기본 머티리얼 번역도 맡는다(16절 MRB1).
- 셰이더 시스템 12.2·12.3절은 "필요한 셰이더가 없으면 `IncompatibleCook`"이다. 머티리얼 셰이더는 그 예외로 위 표의 조합 단위 대체를 쓴다(16절 MSR10). 엔진 셰이더와 엔진 기본 머티리얼은 그 규칙 그대로다.

### 11.4 미리보기

- **미리보기 프로필.** 에디터 호스트의 현재 backend 형식 하나, 미리보기 메시의 정점 입력 타입, 미리보기 view의 렌더 경로 pass만 컴파일한다. Deferred·Forward+ 전환(계획 14.5절 "실시간 확인")은 다른 경로 pass를 추가로 요청한다.
- **노드 미리보기.** 선택 노드의 출력을 `EvaluateMaterialPreview`로 내는 번역을 따로 한다. 정점 입력은 엔진 기본, pass는 미리보기 전용 unlit pass 하나다(UE도 BasePass 하나로 줄인다, `ED/MaterialEditor/Private/MaterialEditor.cpp:351–376`). 텍스처 객체·`Surface`·`MaterialAttributes`처럼 색으로 보일 수 없는 타입은 정해진 시각화(텍스처 샘플·BaseColor 등)를 쓴다. 동시에 컴파일하는 노드 미리보기 수에 상한을 둔다(계획 14.8절).
- **실시간 갱신.** 시간 의존 노드가 있으면 미리보기 viewport가 매 프레임 그린다. 끄기 선택(계획 14.5절 "realtime on/off")은 viewport 갱신만 멈춘다.
- **dynamic 파라미터 편집.** 인스턴스 에디터·머티리얼 에디터의 파라미터 값 변경은 상수 블록만 바꾼다(compile 0, 계획 MT04).

### 11.5 hot reload

| 원인 | 다시 하는 일 |
|---|---|
| 머티리얼 그래프 편집 | 그 머티리얼의 번역·job. 자손 인스턴스는 static 변형이 있으면 변형 번역, 없으면 값 배열만 다시 평탄화(9.6절) |
| 함수·함수 인스턴스 편집 | AssetIndex 역방향 Build 의존으로 쓰는 머티리얼만 다시 번역(계획 14.8절 "역의존 closure만") |
| 컬렉션 layout 편집 | 참조 머티리얼 다시 번역. 값만 바꾸면 컬렉션 상수 블록만 갱신 |
| pass 원문·`MaterialCommon.rsh` 편집(셰이더 시스템 SH4) | 번역은 그대로, job만 다시(전처리 결과가 바뀌어 job 키가 바뀐다) |
| 기본 텍스처 교체 | 셰이더 그대로, 텍스처 handle만 교체(4.5절) |

교체 규칙:
- 새 셰이더 집합은 새 epoch이고, 완결성·서명 검사를 통과해야 교체 후보가 된다(RenderBase 9.8절).
- 교체는 프레임 경계에서 셰이더 집합·상수 블록 layout·텍스처 목록을 한 묶음으로 한다(계획 14.8절). layout이 바뀌면 렌더 상태의 값을 `ParameterKey`로 옮긴다(9.6절).
- 이전 집합·상수 블록은 `Retire`하고, recording lease와 마지막 GPU fence가 끝난 뒤 회수한다(계획 11.2절, MT09).

## 12. 쿠킹과 에셋 너겟, 실행 시 로드

### 12.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 셰이더 맵 id | 머티리얼 Guid·경로 hash, 품질·feature level, 용도, static 값, 참조 함수·컬렉션, 셰이더·pipeline·vertex factory 의존(원문 hash 포함), 텍스처 참조·include·기본 속성 override·외부 코드 hash, 번역기 종류, 코드 형식, Substrate 설정, 플랫폼 layout | `RE/Public/MaterialShared.h:1304–1401`, 기록 `RE/Private/Materials/MaterialShader.cpp:1692–1834` |
| DDC 키 | `"MATSM_"` + 파생 데이터 버전 Guid + 셰이더 형식·버전 + 셰이더 맵 id + 셰이더 타입별 환경 hash + 속성 정의 + GBuffer 키 + 번역기 버전 | `MaterialShader.cpp:207–219, 256–317`, 버킷 `:320–324` |
| 번역 결과 캐시 | 별도 버킷 `"MaterialTranslation"` | `RE/Private/Materials/HLSLMaterialTranslator.cpp:209–212` |
| 쿠킹 | 플랫폼×품질별 리소스를 만들어 셰이더 맵을 패키지에 직렬화. 셰이더 라이브러리가 켜져 있으면 코드는 라이브러리로 보내고 hash만 기록 | `RE/Private/Materials/Material.cpp:2684–2720`, `RE/Private/Materials/MaterialShared.cpp:1514–1590`, `RC/Private/ShaderMap.cpp:309–331` |
| 실행 시 로드 | 라이브러리에서 hash로 코드를 읽거나 인라인 코드 | `RC/Private/ShaderMap.cpp:344–372` |
| PSO 미리 준비 | 셰이더 맵이 준비된 뒤 vertex factory마다 PSO 초기화 정보를 모은다 | `MaterialShared.cpp:3315–3348` |

### 12.2 번역 너겟

번역 처리기(MaterialCook)의 너겟이다. AssetSystem 9절 공통 규칙을 따른다.

| AssetSystem 9절 항목 | 번역 너겟 |
|---|---|
| 형식 버전, 처리기 Guid·버전 Guid | 번역 처리기. IR·생성 규칙이 바뀌면 처리기 버전 Guid를 바꾼다 |
| 외부 도구 | 없다(번역은 도구를 쓰지 않는다) |
| 결정적 수학 버전 | 상수 접기(6.5절)가 쓰므로 넣는다 |
| 툴체인 지문 | 공통 규칙대로 넣는다 |
| 에셋 TypeId | `Material` 또는 `MaterialInstance`(static 변형) |
| 입력 bulk | 그래프 semantic hash(5.5절, 파라미터 기본값 제외) |
| 설정 전체 | 기본 속성(`MaterialTraitsDesc`, 인스턴스 사슬 override를 합친 값), 정규화한 static 값 목록(사슬 합침), 품질 수준(기본과 같으면 "기본" 표지, 8.5절), 노드 타입별 (NodeTypeId, schemaVersion, lowering 버전) 정렬 목록(그래프에 쓰인 것만), `MaterialCommon.rsh` 계약 버전. 사용 대상은 넣지 않는다(8.4절) |
| 대상 프로필과 출력 설정 | 프로필 id·출력 설정 전체(공통 규칙). 여기서 번역이 실제로 쓰는 것은 프로필 형식들의 기능 등급 집합과 공통 상한이다 |
| 의존 입력 | (역할, AssetId, 의존 키) 정렬 목록. 의존 키는 의존 에셋 종류마다 정한다: 함수 = SHA-256(함수 그래프 semantic hash, 그 함수의 의존 키 목록), 함수 인스턴스 = SHA-256(override 정규 바이트, 부모 함수 키), 컬렉션 = layout 서명. static 변형은 루트 머티리얼(역할 Root)의 그래프 semantic hash와 의존을 넣고, 중간 인스턴스의 정체성은 넣지 않는다. AssetSystem 9절은 "같은 처리기 build key"를 쓰므로 이것은 그 일반화다(16절 MAS6) |

값(`.rnug` 부분):
1. 생성 원문(`/Engine/Generated/Material.rsh` 텍스트)
2. 상수 블록 layout 표와 서명
3. 상수 식 프로그램
4. 텍스처 slot 목록(종류·샘플 종류·샘플러 출처·논리 크기 사용·전체 상주 전용 표지)과 slot ↔ `ParameterKey` 대응. 기본값과 기본 텍스처 AssetId는 넣지 않는다(쿠킹 처리기가 그래프에서 읽는다, 4.5절)
5. `MaterialShaderTraits`
6. 텍스처 sampling scale 표(12.5절)
7. 원문 위치 지도(7.6절)
8. 진단·통계(IR 값 수·텍스처·보간·상수 블록 크기)
9. IR hash

### 12.3 셰이더 job

- MaterialCook이 8.2절 축으로 job 목록을 만든다: 통과한 (static 변형, 품질 수준, pass 셰이더 타입, 퍼뮤테이션, 정점 입력 타입, 형식)의 단계마다 셰이더 시스템 job 하나다. 정점 입력 타입은 선언 ∪ 유도 사용 대상(8.4절)이다.
- job 키는 셰이더 시스템 10절 그대로다. 생성 원문은 전처리 결과(7항)에 들어가고, 머티리얼 layout 서명은 8항에 들어간다.
- 같은 pass 원문·같은 생성 원문이면 머티리얼이 달라도 job 키가 같아 너겟을 공유한다.

### 12.4 쿠킹 데이터

| 위치 | 내용 |
|---|---|
| 머티리얼 패키지(쿠킹) | `MaterialArtifact` 목록(품질 수준별): 셰이더 집합 id, `MaterialShaderTraits`, layout 표·서명, 상수 식 프로그램, 평탄화 기본값 배열, 텍스처 slot 목록과 기본 텍스처 Hard 참조, 컬렉션 Hard 참조, sampling scale 표 |
| 인스턴스 패키지(쿠킹) | 부모 Hard 참조, 평탄화 값 배열(부모 layout 서명과 함께). static 변형이면 자기 `MaterialArtifact` |
| 프로젝트 셰이더 라이브러리(형식마다) | 머티리얼 셰이더 항목(8.6절 조회 키 → 코드 SHA). 셰이더 시스템 12.3절 프로젝트 라이브러리이며 묶음 id 열(RenderBase SX5)로 셰이더 집합을 묶는다 |
| 쿠킹 보고서 | 머티리얼별 퍼뮤테이션 수와 증감(8.5절) |

셰이더 코드는 패키지에 넣지 않는다. 패키지는 셰이더 집합 id만 가진다. UE의 "라이브러리가 켜졌을 때 hash만 기록"과 같은 구조를 항상 쓴다.

**쿠킹 품질 수준.** TargetProfile에 쿠킹할 머티리얼 품질 수준 목록을 둔다(기본 하나, 16절 AS4). 실행 중 품질 전환은 쿠킹한 수준 안에서만 된다.

### 12.5 텍스처 sampling scale(TextureStreaming T4)

TextureStreaming 결정 T4는 "머티리얼 컴파일러가 생기면 셰이더 빌드 결과로 공급"한다고 남겼다. Rocks는 IR 분석으로 정한다.
- 텍스처 샘플의 UV 식이 `TexCoord(i) × k + b`(k·b가 `Constant` 또는 `Static`)이면 (채널 i, scale |k|의 최대 성분)이다.
- k가 `Uniform`이면 기본값으로 계산한 scale과 "파라미터 의존" 표지를 둔다. 실행 중 값이 바뀌면 렌더 상태가 상수 식으로 다시 계산해 스트리밍에 알린다(측정 뒤 필요 여부 결정, 후속).
- 그 밖(world 좌표 UV·임의 식)은 "알 수 없음"이다. 텍스처 샘플 노드의 명시 scale 속성을 쓰고, 없으면 TextureStreaming의 보수적 상한 규칙(계획 9.5절 "동적 UV")을 따른다.
- 출력: 텍스처 slot마다 (UV 채널, scale, 출처: 정적·파라미터·명시·알 수 없음).
- UE는 에디터에서 debug view 셰이더로 그려 재서 `TextureStreamingData`에 저장한다(`RE/Public/Materials/MaterialInterface.h:200–211, 422`, `RE/Private/Materials/MaterialInterface.cpp:2453`, `RE/Private/DebugViewModeHelpers.cpp:337`). 측정 경로의 세부는 미확인이다. Rocks는 GPU 없이 결정적으로 계산할 수 있는 IR 분석을 먼저 쓴다.

### 12.6 실행 시 로드

1. AssetRuntime이 머티리얼 패키지를 로드한다(CPU ready). 텍스처·컬렉션·부모는 Hard 참조로 함께 로드된다.
2. `Material` 모듈이 `MaterialArtifact`로 `MaterialRenderState` 생성 요청을 렌더 lane에 보낸다(Render ready 단계, AssetSystem 3절 "Render ready 계약").
3. 렌더 lane의 MaterialRuntime이 셰이더 라이브러리에서 그 셰이더 집합 id 항목을 연다. 장치 셰이더는 처음 쓸 때 만든다(RenderBase 9.2절).
4. 열 때 layout 서명을 대조한다. 다르면 `IncompatibleCook`이고 기본 머티리얼로 대체한다(11.3절).
5. 그리기에서 (pass, 정점 입력 타입) 조합이 집합에 없으면 그 조합만 기본 머티리얼로 대체한다(11.3절).
6. 인스턴스는 부모 렌더 상태의 셰이더 집합을 공유하고 자기 상수 블록을 가진다.

**PSO 미리 준비**는 후속이다. RenderBase 9.7절 `PipelinePrewarm`에 (머티리얼 셰이더 집합, 정점 입력 타입, pass) 목록을 넘기는 형태로 둔다(16절 MRB6). UE는 셰이더 맵이 준비된 뒤에만 PSO를 모은다(12.1절).

### 12.7 결정성

- 번역 너겟 값과 쿠킹 머티리얼 패키지는 같은 도구 빌드에서 호스트와 무관하게 같은 바이트다(AssetSystem 10절). 6.6절 규칙과 상수 접기 규칙(6.5절)이 이것을 보장하고, 패키지에 들어가는 셰이더 집합 id는 번역 결과 내용 hash라 호스트별 값이 섞이지 않는다(8.6절).
- 셰이더 job 결과의 호스트 사이 동일성은 셰이더 시스템 10절 측정(S3)을 따른다.

## 13. backend 지원표와 제약

형식 id는 셰이더 시스템 2절이다. 숫자 상한은 형식 정의(셰이더 시스템 12.1절 `limits`)가 정본이고, 아래는 머티리얼 쪽 영향만 적는다.

| 항목 | `D3D12_SM6` | `D3D11_SM5` | `VULKAN_SM5` | `METAL_SM5` | `GLES31_ANDROID`·`GLES31_PC` |
|---|---|---|---|---|---|
| 기능 등급 | SM6 | SM5 | SM5 | SM5 | Mobile |
| 머티리얼 텍스처 수 | (pass, 퍼뮤테이션)별 `16 − 렌더러 합계`(Renderer 7.4절, 결정 RN13·MD4). 공통 상한이 GLES에 묶여 모든 형식이 같은 값 | 같음 | 같음 | 같음 | 같음 |
| 상수 블록 | cbuffer | cbuffer | uniform buffer | buffer | uniform `vec4` 배열 에뮬레이션(타입별 배열, 7.2절 4항). uniform 벡터 수 상한 안 |
| 머티리얼 보간 | `TEXCOORDn` n ≤ 15 − pass 예약 | 같음 | 같음 | 같음 | varying 상한 − pass 예약 |
| VS·PS storage buffer 읽기 | 가능(공통 상한 4) | 가능 | 가능 | 가능 | **불가**(ES 3.2 최소 보장 0, 7.2절 4항). 이런 노드는 GLES에서 쿠킹 오류 |
| VS·PS UAV 쓰기 | 공통 상한에서 금지 | 금지 | 금지 | 금지 | 금지 |
| half 정밀도 | 없음(모든 형식 float32, 7.2절 5항) | | | | 없음. 모바일 성능 영향은 측정 MM3 |
| HLSL 2021(임의 HLSL 노드) | 켬 | 끔(SPIRV-Cross 경로로 감) | 켬 | 켬 | 끔 |
| wave 연산 | 가능(형식 기능) | 불가 | 불가 | 불가 | 불가 |
| GBuffer MRT(GBufferD가 필요한 shading model) | 8, 지원 | 8, 지원 | 8(결정 RN3 장치 요구), 지원 | 8, 지원 | 4, Deferred `Unsupported`·Forward+ 지원(결정 RN4) |
| scene color·depth 읽기(반투명) | pass가 제공 | 같음 | 같음 | 같음 | pass가 제공(framebuffer fetch는 후속) |
| 여러 closure 저장(SF4) | 연구 gate 대상 | 불가 | 불가 | 불가 | 불가 |
| 가상 텍스처 | 후속 | 후속 | 후속 | 후속 | 후속 |

UE 대조:
- 셰이더당 샘플러 기본 16이고(`Engine/Source/Runtime/RHI/Private/DataDrivenShaderPlatformInfo.cpp:163`), PCD3D_SM6은 32다(`CFG/Windows/DataDrivenPlatformInfo.ini:136`). OpenGL은 16을 넘으면 컴파일 오류다(`Engine/Source/Developer/ShaderFormatOpenGL/Private/OpenGLShaderCompiler.cpp:868–877`). UE 머티리얼은 공유 샘플러로 슬롯을 아낀다(`RE/Classes/Engine/EngineTypes.h:282–293`, `RE/Private/Materials/HLSLMaterialTranslator.cpp:7364–7388`).
- UE 번역기의 ES3_1 shading model 금지 검사는 `#if 0`으로 꺼져 있다(`RE/Private/Materials/HLSLMaterialTranslator.cpp:2175–2199`). 모바일 forward에서 GBuffer scene texture·SceneColor 읽기는 오류다(`:8320–8326, 8372–8375`). Rocks는 V6 capability 검사로 같은 일을 한다.
- 모바일의 shading model 거르기는 `r.Mobile.ShadingModelsMask`로 한다(`Engine/Source/Runtime/Core/Private/HAL/ConsoleManager.cpp:4018–4023`). Rocks는 기능 등급별 지원 preset 표를 형식 정의 기능 목록에 두고 V6로 검사한다.
- UE 모바일은 Substrate도 Blendable이다(`RC/Private/RenderUtils.cpp:2231–2232`). Rocks의 closure 1개 저장과 같은 조건이다.

**D3D11(FXC) 주의.** 생성 원문이 FXC에서 실패하면 셰이더 시스템 규칙대로 SPIRV-Cross 경로로 다시 간다(셰이더 시스템 2절). 그래서 생성 원문은 HLSL 2021 문법을 쓰지 않는다(임의 HLSL 노드가 요구한 경우 제외).

## 14. 에디터 요구

그래프 편집 화면은 범위 밖이다. 아래는 에디터가 쓰도록 이 문서의 모듈이 제공해야 하는 기능이다. 화면 Element는 UiToolkit `GraphView`(UiToolkit 6절 에디터 프레임워크 표의 "그래프 편집" 행)를 쓴다.

| ID | 기능 | 제공 모듈 | 계획 대응 |
|---|---|---|---|
| E1 | 노드 타입 목록·검색 메타데이터(분류·키워드·핀 설명) | MaterialSchema | 14.5 "node 검색·palette" |
| E2 | 편집 명령 API(노드 추가·삭제·연결·속성 변경)와 expected revision transaction, undo/redo 단위 | EditorMaterial(EditorBackend transaction) | 14.8 |
| E3 | 연결 가능성 질의(핀 타입·단계·암묵 변환 결과) | MaterialCompiler(V1 부분 검사, 동기·빠름) | 14.5 "typed pin 연결" |
| E4 | 진단 목록(노드·핀 위치, 심각도) | MaterialCompiler | MT02 |
| E5 | 컴파일 상태 이벤트(대기·번역·job 진행·성공·실패·마지막 성공과 다름) | CompileCoordinator | 14.8 |
| E6 | 미리보기 세션(메시·조명·환경·카메라·렌더 경로 전환) offscreen 렌더 | PreviewSession + Renderer | 14.5 "실시간 확인" |
| E7 | 노드 미리보기 요청·해상도·동시 수 상한 | CompileCoordinator | 14.5, 14.8 |
| E8 | 생성 원문 보기와 원문 위치 지도 | MaterialCompiler | 14.5 "generated HLSL" |
| E9 | 통계: IR 기준(텍스처·샘플러·보간·상수 크기·값 수), (pass, 퍼뮤테이션)별 머티리얼 텍스처 상한과 사용 수(Renderer RQ18), 형식별 컴파일 결과(명령 수 등, 도구가 주는 값). 정적 지표와 GPU 측정을 구분 | MaterialCompiler·ShaderCompiler | 14.5 "진단·비용" |
| E10 | 퍼뮤테이션 수(프로필별)·static 변형 추가 비용 표시 | MaterialCook(조회 API) | 14.1, 14.5 |
| E11 | 인스턴스 편집 데이터(파라미터 그룹·정렬·범위·override 여부·reset) | Material(에셋 모듈) | 14.5 "인스턴스 편집" |
| E12 | 의존 추적(이 함수를 쓰는 머티리얼, 이 머티리얼의 인스턴스) | AssetIndex 역방향 의존 | 14.5 "재사용" |
| E13 | 모르는 노드 자리 표시·migration 결과 보고 | MaterialSchema | 14.6, MT10 |
| E14 | headless 번역·쿠킹 명령(`material-compile`·`asset-cook`) | MaterialCook·Programs/Commandlet | MT08 |

화면 코드는 shader 의미·검증을 다시 구현하지 않는다(계획 14.6절). E3·E4는 화면이 부르는 유일한 검증 경로다.

## 15. 단계와 완료 기준

### 15.1 선행 작업

| 선행 | 담당 문서 | 막는 단계 | 이유 |
|---|---|---|---|
| SH1(워커·전처리기·두 층 키·너겟) | 셰이더 시스템 | MS2 | 생성 원문을 컴파일해 검증 |
| SH2(리플렉션 정규화·워커 규칙 검사·P14 셰이더 타입) | 셰이더 시스템 | MS2·MS4 | 7.2절 규칙 검사, pass 셰이더 타입 |
| SH3·P7(파라미터 구조체·`ShaderParameterLayout`) | 셰이더 시스템 | MS2 | 동적 layout 형식(결정 MD3) |
| 셰이더 결정 4(파라미터 선언) | 셰이더 시스템 15절 | MS2 | MD3과 같은 형식이어야 한다 |
| P6b(TargetProfile·라이브러리 묶음 id) | 셰이더 시스템 | MS3 | 셰이더 집합 쿠킹 |
| 셰이더 시스템 개정 MSR1–MSR11(16절) | 셰이더 시스템 | 각 단계 | |
| A1–A4(AssetRuntime·ProjectLayout·에셋 너겟·Cooker) | 에셋 시스템 | MS3 | 에셋·너겟·쿠킹 |
| A5(텍스처 에셋) | 에셋 시스템 | MS3 | 텍스처 파라미터 |
| 리플렉션 B9(`InstancedStruct`·`OrderedMap`·`Optional`) | ReflectionExtensions | MS1 | 노드 속성 저장 |
| RB3–RB5(셰이더 집합·바인딩·상수 블록·정점 입력 타입) | 렌더 기반 | MS4 | 실행 시 바인딩 |
| Renderer 스펙 14절 계약(개정 2)과 16절 MRD1–MRD12 | Renderer | MS2(계약)·MS4(구현) | 생성 원문 계약 |
| TextureStreaming 바인딩 snapshot | 텍스처 스트리밍 | MS4 | 텍스처 handle 해석 |
| 정점 입력 타입 선언의 장치 없는 위치(16절 MRB8) | 렌더 기반 | MS3 | 쿠커의 거르기 |

### 15.2 단계

| 단계 | 내용 | 완료 기준 | 계획 대응 |
|---|---|---|---|
| MS1 | MaterialSchema: 그래프 데이터·노드 descriptor 등록·semantic hash·리플렉션 저장·migration 틀 | 저장/재열기 왕복, 화면 편집의 semantic hash 불변, 모르는 노드 보존, 노드 등록 해제 drain | MAT0 |
| MS2 | MaterialCompiler: 검증(V1–V12)·IR·최적화·HLSL 생성·상수 식·layout(SF1 표면 범위) | MS-T1·T2·T3, 생성 원문이 엔진 컴파일 프로필 모든 형식에서 컴파일·워커 규칙 검사 통과, V 규칙 음성 대조 | MAT0 |
| MS3 | Material 에셋 모듈·MaterialCook: 에셋 클래스·번역 너겟·job 목록·쿠킹 데이터·라이브러리 항목·퍼뮤테이션 보고 | MS-T4·T5, 원본 없이 쿠킹 데이터만으로 로드, 부분 쿠킹(함수 변경의 역의존만) | MAT0–MAT2 |
| MS4 | MaterialRuntime + Renderer 연결: 렌더 상태·상수 식 실행·상수 블록·텍스처 handle·기본 머티리얼·두 렌더 경로 | MS-T6·T7·T8, 계획 MT03 | MAT1 |
| MS5 | 에디터 지원(E1–E14): CompileCoordinator·PreviewSession·노드 미리보기·hot reload·통계 | 계획 MT04·MT05·MT07·MT09 | MAT1–MAT2 |
| MS6 | 함수·Attributes·컬렉션·인스턴스 static 변형·Dynamic Bool·Component Mask | 계획 MT06(함수·인스턴스), MS-T9 | MAT2 |
| MS7 | 레이어 스택·레이어 인스턴스·SF2(반투명 블렌드 모드)·DeferredDecal·PostProcess·UserInterface 영역 | 계획 MT06(레이어), 영역별 기준 영상 | MAT3 |
| MS8 | SF3 표면(고급 shading model preset·slab 직접 입력·표면 연산) | 기준 영상, GLES Deferred 거르기(결정 RN4) | MAT4 |
| MS9 | SF4 여러 closure 저장, Hair(P3) | 계획 14.4절 연구 gate·계획 11.1절 P3 gate 판정 | 연구 gate |

### 15.3 시험

| ID | 시험 | 판정 |
|---|---|---|
| MS-T1 | 같은 그래프 같은 바이트: (a) 같은 파일 두 번 번역 (b) 노드 저장 순서만 섞은 파일 (c) Windows·macOS (d) 작업 디렉터리·locale 변경 | 생성 원문·layout·상수 식·번역 너겟 값·쿠킹 패키지 byte 동일 |
| MS-T2 | 화면 편집만(노드 이동·주석·재경유 추가) | semantic hash 불변, 번역·job 0개 |
| MS-T3 | 골든 원문 | fixture 그래프의 생성 원문이 저장된 기대 원문과 같음(생성 규칙 변경은 처리기 버전과 함께 기대 원문 갱신) |
| MS-T4 | 퍼뮤테이션 수 | fixture 머티리얼 묶음 × 엔진 기본 프로필의 축별 수·job 수가 기대 표와 같음. 사용 대상 축소·품질 공유·static 변형 공유가 수를 줄이는지 확인 |
| MS-T5 | 너겟 무효화 | 그래프·함수·컬렉션 layout·static 값·노드 lowering 버전·프로필을 하나씩 바꾸면 번역 너겟 미적중. 파라미터 기본값(scalar·vector·텍스처)·컬렉션 값·화면 데이터·사용 대상만 바꾸면 번역 너겟 적중(기본값은 cookKey만, 사용 대상은 job 목록과 cookKey만 바뀐다) |
| MS-T6 | 기준 영상 | 같은 머티리얼 fixture를 Deferred·Forward+로 그린 결과가 Renderer가 정한 허용 오차 안. backend별 결과가 D3D12와 셰이더 시스템 self-test 허용 오차(채널 ±8·2%) 안 |
| MS-T7 | 상수 식 | 상수 식으로 계산한 상수 블록 값과, 같은 식을 셰이더 안에서 계산한 값의 차이가 허용 오차 안(GPU readback) |
| MS-T8 | 대체 | 사용 대상이 빠진 조합·서명 불일치·라이브러리 항목 누락에서 기본 머티리얼로 그리고 진단 1회 |
| MS-T9 | override 보존 | 파라미터 이름 변경·레이어 순서 변경·함수 반복 호출에서 override가 키로 보존되고 서로 덮지 않음 |
| MS-T10 | 텍스처 예산 | (pass, 퍼뮤테이션)별 예산(Renderer 7.4절)을 넘는 머티리얼이 그 조합에서만 쿠킹 오류이고, 진단에 상한·사용 수·pass·퍼뮤테이션이 나옴 |
| MS-T11 | 사용 대상 유도 | 메시를 추가한 뒤 부분 쿠킹하면 그 메시가 쓰는 머티리얼의 cookKey가 바뀌어 다시 쿠킹되고, 전체 쿠킹과 결과가 같음 |

계획 14.9절 MT01–MT10은 위 단계의 완료 기준으로 그대로 쓴다(MT01 → MS1, MT02 → MS2, MT03 → MS4, MT04·MT05·MT07·MT09 → MS5, MT06 → MS6·MS7, MT08 → MS2·MS3, MT10 → MS1·MS5).

### 15.4 측정

| ID | 측정 | 쓰는 곳 |
|---|---|---|
| MM1 | 번역 시간(p50·p95, 노드 수별) | 11.2절 번역을 메인 프로세스 작업으로 둘지 |
| MM2 | 상수 식 평가 비용(인스턴스 수·프레임당 변경 수) | 9.4절 병렬 평가 |
| MM3 | GLES highp 전용의 성능 비용 | 13절 half 미지원 영향 |
| MM4 | 머티리얼당 job 수·쿠킹 시간(대표 프로젝트) | 8.5절 상한 기본값 |
| MM5 | 대표 콘텐츠의 머티리얼 텍스처 수 분포 대 (pass, 퍼뮤테이션)별 예산(Renderer 7.4절 합계 13–15 조합) | 결정 MD4·RN13 재검토 조건(10.4절 D3) |

## 16. 다른 문서에 반영할 요구

위치만 적는다. 실제 수정은 해당 문서 담당이 한다. ID는 다른 문서의 단계 ID(RenderBase RB1–RB7, TextureStreaming TS1 등)와 겹치지 않게 `M`으로 시작한다.

**셰이더 시스템(ShaderSystem.ko.md)**

| ID | 위치 | 요구 |
|---|---|---|
| MSR1 | 7.2절 4항(상한) | 머티리얼 pass 셰이더의 머티리얼 텍스처 예산 = (pass, 퍼뮤테이션)별 `16 − 렌더러 합계`(Renderer 7.4절·RQ12, 결정 MD4). 워커 상한 검사 오류에 머티리얼·pass·퍼뮤테이션을 담는다 |
| MSR2 | 3절(셰이더 맵 키)·12.2절(조회 키) | 머티리얼 셰이더 집합 id를 키 축으로(8.6절). RenderBase SX5 묶음 id와 같은 값 |
| MSR3 | 3절(가상 경로)·결정 6(전처리기) | `/Engine/Generated/Material.rsh`를 job마다 메모리 텍스트로 잇는 규칙(7.2절) |
| MSR4 | 13절(P7)·15절 결정 4 | `ShaderParameterLayout`을 C++ 타입 없이도 만들 수 있는 데이터 형식으로(동적 layout, 결정 MD3) |
| MSR5 | 12.2절(P14) | 셰이더 타입의 "머티리얼 셰이더" 표지와 거르기 인자 `MaterialShaderTraits`(8.3절), pass가 요구하는 custom 출력 선언 |
| MSR6 | 3절 | 임의 HLSL 노드의 include 허용 목록(가상 경로 범위) |
| MSR7 | 11절(워커) | job 우선순위(미리보기 > 노드 미리보기 > 화면 > 백그라운드)와 취소 |
| MSR8 | 12.3절 | 프로젝트 셰이더 라이브러리에 머티리얼 셰이더 집합이 들어가는 규칙과 쿠킹 매니페스트 v2 항목 |
| MSR9 | 12.1절(형식 정의) | 기능 등급(Mobile·SM5·SM6) 속성과 기능 등급별 지원 shading model·노드 기능 표(V6·Capability Tier Switch 입력) |
| MSR10 | 12.2·12.3절("없으면 `IncompatibleCook`") | 머티리얼 셰이더는 예외로 (머티리얼, 정점 입력 타입, pass) 조합 단위 기본 머티리얼 대체 + 진단(11.3절). 엔진 셰이더와 엔진 기본 머티리얼은 지금 규칙 그대로 |
| MSR11 | 12.3절(`shader-compile --engine`) | 엔진 기본 머티리얼 에셋을 번역해 엔진 내장 라이브러리에 넣는 경로(11.3절) |

**렌더 기반(RenderBase.ko.md)**

| ID | 위치 | 요구 |
|---|---|---|
| MRB1 | 9.2절(셰이더 집합)·SX14(셰이더 공급자) | 머티리얼 셰이더 집합(묶음 id)을 열고, 엔진 기본 머티리얼을 시작 완결성 검사에 포함. 필요 시 컴파일 형식(`GLES31_PC`)의 공급자가 엔진 기본 머티리얼 번역도 맡는다 |
| MRB2 | 9.3절(바인딩 지도) | `TypeOf<T>()` 대신 동적 layout 표에서 바인딩 지도를 만드는 경로 |
| MRB3 | 10.2절(정점 입력 `.rsh` 인터페이스) | 정점 입력 타입이 `MaterialVertexInputs`·`MaterialInputs`를 채우는 계약(7.3절) |
| MRB4 | 9.5절(공유 상수 slot) | 컬렉션 에셋 identity로 하는 동적 slot 등록 |
| MRB5 | 9.8절(셰이더 교체) | 머티리얼 셰이더 집합·상수 블록 layout·텍스처 목록의 묶음 교체(11.5절) |
| MRB6 | 9.7절(파이프라인 캐시) | `PipelinePrewarm`에 (머티리얼 셰이더 집합, 정점 입력 타입, pass) 목록 입력(후속) |
| MRB7 | 9.4절(상수 블록) | 머티리얼 렌더 상태의 `Persistent` 블록 교체·`Retire` 규칙 |
| MRB8 | 10.2절(등록) | 정점 입력 타입 선언(이름·`.rsh` 경로·거르기·정적 배치)을 장치 없는 모듈(ShaderCore 또는 그와 같은 층)에 두어 쿠커가 RenderBase 없이 거르기를 실행할 수 있게 한다(8.3절) |

**Renderer 스펙(Renderer.ko.md, 개정 2)**

| ID | 요구 |
|---|---|
| MRD1 | 10.4절 D1: `worldPositionOffset`을 정점 함수 결과로 분리 |
| MRD2 | 10.4절 D2: 번호표의 C++·HLSL 정의 위치(순환 해소) |
| MRD3 | 10.4절 D3: 결정 RN13의 재검토 조건(측정 MM5) |
| MRD4 | 10.4절 D4: `AlphaHoldout` 처리 명시 |
| MRD5 | 10.4절 D5: pass 거르기 인자를 `MaterialShaderTraits`로 |
| MRD6 | view 상수의 translated world 기준과 절대 위치 표현(`WorldPosition` 타입, 5.3절) |
| MRD7 | instance 데이터 계약(머티리얼 `Instance` 빈도 입력, 이전 프레임 변환) |
| MRD8 | 기본 머티리얼 대체 표시 방식 |
| MRD9 | 10.4절 D7: pass별 경로 전용·공통 구분과 렌더 경로·기능 등급 정의 이름 |
| MRD10 | Deferred·Forward+ 기준 영상 허용 오차(MS-T6) |
| MRD11 | 10.4절 D8: Depth·ShadowDepth pass가 마스크 전용 함수를 부름 |
| MRD12 | DeferredDecal이 쓰는 속성, 반투명 scene color·depth 입력, PostProcess 위치(5.6절) |

**에셋 시스템(AssetSystem.ko.md)**

| ID | 위치 | 요구 |
|---|---|---|
| MAS1 | 3절(개념과 모듈)·1절 단계표 | `Material` 에셋 모듈과 머티리얼 쿠킹 처리기 |
| MAS2 | 6.2절(참조 종류) | 머티리얼 참조 사례(4.4절 표), Build 의존 순환의 `UnsupportedCycle` |
| MAS3 | 12절(텍스처) | 텍스처 샘플 종류 검사(V10)용 메타데이터 조회 API. 노멀맵 쿠킹 형식(채널 수)을 프로필마다 고정해 머티리얼의 노멀 해석이 static이 되게 함(UE의 노멀맵 Z 복원 규칙은 미확인) |
| MAS4 | 10절(TargetProfile) | 쿠킹 머티리얼 품질 수준 목록 필드 |
| MAS5 | 10절(cookKey 지역 입력·부분 쿠킹) | 처리기가 AssetIndex 역방향 Hard 의존에서 유도한 값(머티리얼 사용 대상, 8.4절)을 cookKey 지역 입력으로 넣는 규칙. 그러면 메시 추가가 머티리얼 cookKey를 바꿔 부분 쿠킹 대상에 든다 |
| MAS6 | 9절(의존 입력 튜플) | 의존 튜플의 세 번째 값이 "같은 처리기 build key"가 아닌 "의존 에셋 종류가 정한 의존 키"(함수 키·컬렉션 layout 서명, 12.2절)일 수 있다는 일반화 |

**엔진 구현 계획(EngineImplementationPlan.ko.md)**

| ID | 위치 | 요구 |
|---|---|---|
| MEP1 | 3.2절 모듈 트리, 2.2절 모듈 표(MaterialSchema/Compiler/Runtime 행) | MaterialAssetBridge → `Material`, MaterialCompiler를 Engine Developer로, ShaderWorker → ShaderCompileWorker, MaterialRuntime 의존 RenderBase |
| MEP2 | 14.4절 | 결정 MD1 반영(Substrate형 그래프, `SurfaceAttributes` 내보내기, 여러 closure는 연구 gate) |
| MEP3 | 14.5절 표 "Material Output" 행 | `Surface` 입력 + shading preset 노드 |
| MEP4 | 14.6절 표 | 3.2절 배치(이 문서) |
| MEP5 | 14.9절 | MAT0–MAT4 ↔ MS1–MS9 대응(15.2절) |

**그 밖**

| ID | 문서·위치 | 요구 |
|---|---|---|
| MTS1 | TextureStreaming 4.1절·결정 T4 | sampling scale 공급 형식(12.5절). 5.3절 helper·논리 크기 규칙은 이 문서 7.4절이 따른다 |
| MUT1 | UiToolkit `RetainedPanel` 행·UI 렌더 | `UserInterface` 영역 머티리얼과 UI 정점 입력 타입 |

## 17. 결정 사항

2026-11-18 사용자 지시("결정사항들은 권장안대로 하면됨")로 MD1–MD9를 확정했다. MD4는 같은 날 확정된 Renderer 결정 RN13과 이 문서 초안 권장이 달라 RN13에 맞췄다. 결정 대기로 남은 항목은 없다.

| ID | 결정 | 선택지 | 결정 | 근거 |
|---|---|---|---|---|
| MD1 | **Substrate 채택 형태** | (가) 고전 shading model enum만. (나) Substrate형 BSDF 그래프 + closure 하나로 접어 `SurfaceAttributes`로 내보냄, 여러 closure는 연구 gate. (다) Substrate 전체(여러 closure·Adaptive형 저장) | **결정됨(2026-11-18, 권장안 (나))** | UE 5.8 새 프로젝트 기본(Substrate + Blendable)과 같은 조합이다(10.2절). UE는 고전 셰이더 함수를 5.7부터 "Deprecated by Substrate"로 표시했다. Renderer도 Blendable과 같은 고정 배치로 시작한다(Renderer 7.1절). Rocks 형식 가운데 Adaptive 대상은 `D3D12_SM6`뿐이라 (다)는 한 형식만 이득을 본다. (나)는 그래프·에셋을 처음부터 Substrate형으로 두어 나중에 여러 closure로 갈 때 에셋 변환이 없다 |
| MD2 | **번역기 구조** | (가) typed IR → HLSL. (나) 노드가 HLSL 조각을 직접 내는 번역기(UE 5.8 기본) | **결정됨(2026-11-18, 권장안 (가))** | 계획이 typed IR을 요구하고(14.1·14.7절), CPU bake·ray profile이 같은 IR을 소비한다(6.7절). Rocks는 하위 호환할 노드 구현이 없다. 결정성(상수 접기·번호 매김)도 IR에서 한 번에 지킨다 |
| MD3 | **셰이더 결정 4와의 관계** | 셰이더 결정 4는 (A) MetaCompiler 파라미터 종류 또는 (B) UE 매크로 방식이다. 머티리얼 layout은 어느 쪽이든 실행 중 데이터로 만든다 | **결정됨(2026-11-18, 권장안): (A)를 고르고 그 layout 표를 `ShaderParameterLayout` 데이터 형식으로 정해 머티리얼 컴파일러도 같은 형식을 낸다** | 바인딩 지도·서명 검증·GLES 복사 표가 C++ 구조체와 머티리얼에 같은 경로 하나로 동작한다(RenderBase 9.3절). (B)는 매크로 메타데이터와 머티리얼 동적 layout이 두 형식이 되어 변환층이 필요하다. UE도 머티리얼 uniform buffer는 매크로가 아니라 실행 중 layout으로 만든다(`RE/Private/Materials/MaterialUniformExpressions.cpp:524`). 셰이더 결정 4 자체도 같은 지시로 권장안 (A)가 되는지는 셰이더 시스템 문서에서 확인한다 |
| MD4 | **텍스처·샘플러 한도** | (가) 짝 규칙 유지, 머티리얼 텍스처 = (pass, 퍼뮤테이션)별 `16 − 렌더러 합계`(= Renderer RN13 (가)). (나) 16-SRV 형식용 pass 셰이더 타입을 따로 두어 데스크톱은 높은 상한(RN13 (나)). (다) 공유 샘플러 + 데스크톱 SRV 상한을 높인 장치 요구 형식(이 문서 초안 권장) | **결정됨(2026-11-18): (가), Renderer RN13에 맞춤** | 사용자가 RN13 (가)를 확정했다. 초안 권장 (다)는 RN13과 충돌해 접었다. 재검토 조건: 측정 MM5에서 대표 콘텐츠의 머티리얼이 무거운 퍼뮤테이션 예산을 넘는 비율이 크면 RN13 (나)·(다)를 함께 다시 본다(10.4절 D3) |
| MD5 | **함수 안 파라미터 범위** | (가) 계획 14.7절대로 호출 위치마다 독립 + 명시 "공유" 표시. (나) UE처럼 같은 이름이면 공유 | **결정됨(2026-11-18, 권장안 (가))** | 계획 결정을 따른다. UE 에셋을 들일 때 차이는 변환 도구가 다룬다 |
| MD6 | **사용 대상 결정 방식** | (가) 선언 + 쿠킹 유도(8.4절). (나) UE처럼 에디터가 자동으로 켜고 저장 | **결정됨(2026-11-18, 권장안 (가))** | 에셋을 동의 없이 바꾸지 않고, 참조로 알 수 있는 조합은 쿠커가 빠뜨리지 않는다 |
| MD7 | **품질 수준** | (가) Low·Medium·High·Epic 4단계(UE와 같음). (나) 프로젝트 정의 | **결정됨(2026-11-18, 권장안 (가))** | 프로필이 쿠킹 수준을 고른다 |
| MD8 | **이름** | 2절 대응표의 Rocks 이름, 에셋 모듈 이름 `Material`(계획 MaterialAssetBridge 대신). 영역·블렌드 모드·shading model 이름은 Renderer 14.2절 | **결정됨(2026-11-18, 권장안)** | — |
| MD9 | **상수 식 평가 위치** | (가) 렌더 lane(UE와 같음). (나) 게임 lane이 최종 바이트를 만들어 packet으로 넘김 | **결정됨(2026-11-18, 권장안 (가))** | 텍스처·스트리밍 상태와 같은 lane에서 해석하고, 게임 lane은 바뀐 값만 보낸다. 병렬화는 측정 MM2 뒤 |

## 18. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1(초안) | 2026-11-17 – 11-18 | 첫 작성. UE 5.8.4 조사(에셋 계층·파라미터·uniform expression·static 변형·컬렉션, 번역기 기본값과 IR 실험 상태, 템플릿 연결, 셰이더 맵 키·퍼뮤테이션·사용 플래그·품질, 비동기 컴파일·대체·미리보기, DDC·쿠킹, Substrate 기본 여부·Blendable/Adaptive·고전 경로 상태, 도메인·혼합·shading model 레거시 확인, 모바일·SM5 제약). 모듈 배치·에셋 모델·그래프 스키마·IR·코드 생성·퍼뮤테이션·파라미터·표면 모델·컴파일·쿠킹·backend 표·에디터 요구·단계·다른 문서 요구. 2026-11-18: 결정 MD1–MD9 확정(사용자 "권장안대로", MD4는 Renderer RN13에 맞춤), Renderer 개정 2(2dc9068) 14절 계약에 경계를 맞추고 다른 점을 10.4절에 정리, 교차 검증 리뷰 반영(셰이더 집합 id를 내용 hash로, 파라미터 기본값을 번역 키에서 제외, 사용 대상 유도를 cookKey 입력으로, 정점 입력 거르기의 장치 없는 위치, PinKey 정의, 의존 키 규칙, TextureStreaming 5.3 규칙, 요구 ID 접두어 `M`) |
