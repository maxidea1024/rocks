# 머티리얼 시스템(MaterialSystem) 설계

## 8. 퍼뮤테이션

### 8.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 머티리얼 요약 | `FMaterialShaderParametersBase`: 영역·shading model 집합·블렌드 모드·품질·feature level·customized UV 수와 64+10개 비트 플래그(`bIsMasked`, `bIsUsedWith{SkeletalMesh, MorphTargets, …}`, `bMaterialMayModifyMeshPosition` 등) | `RE/Public/MaterialShared.h:1152–1271` |
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
| 렌더 경로 | 축이 아니다 | pass 셰이더 타입이 경로를 정한다(GBuffer는 Deferred, ForwardOpaque·DepthAux는 Forward+, Depth·ShadowDepth·Translucent 등은 공통, Renderer 14.3절). Render Path Switch는 pass job의 `#if`로 풀린다(6.3절) |

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
  - 메시 에셋 스펙의 요구(MeshAnimationAssets MR4, 2026-11-20): 메시 정점 입력 타입은 정적 메시용과 skinned 메시용 둘이다(stream 집합은 RenderBase 10.4절 끝 "메시 에셋 요구"). 이름은 RenderBase RB5가 정하고 이 집합에 등록한다. skinned 메시 정점 입력은 morph 적용 여부를 정점 입력 타입의 특성으로 갖는다(UE `bUsedWithMorphTargets`에 대응 — morph 변화량은 정점 stream이 아니라 렌더러가 적용하므로 머티리얼 원문은 바뀌지 않고 job 목록만 바뀐다). 쿠킹 유도는 스킨 메시의 morph target 유무로 한다.
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
