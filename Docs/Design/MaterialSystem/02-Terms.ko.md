# 머티리얼 시스템(MaterialSystem) 설계

## 2. 용어 대응

결정 MD8(확정)대로다. 영역·블렌드 모드·shading model의 이름과 번호는 Renderer 스펙 14.2절(Renderer 소유, 코드는 C++ `RenderData`·HLSL `/Engine/Shared/`, 결정 RN5)을 따른다.

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
| 출력 노드 `MaterialOutput` | 머티리얼 루트 노드(`UMaterial`의 입력 핀들) | 영역별 typed 출력 |
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
| 표면 연산 노드 `SurfaceHorizontalMix`·`SurfaceVerticalLayer`·`SurfaceAdd`·`SurfaceWeight`·`SurfaceSelect` | Horizontal Mixing·Vertical Layering·Add·Weight·Select | BSDF 결합(19장 이름) |
| shading preset 노드 `SurfaceFromShadingModel` | Substrate Shading Models(legacy 변환 노드)·`EMaterialShadingModel` | 고전 shading model 입력을 BSDF로 바꾸는 노드 하나(shading model은 속성·입력으로 고른다) |
| 기본 머티리얼 `DefaultMaterial`(영역별) | `UMaterial::GetDefaultMaterial` | 대체 머티리얼 |
| 컴파일 조정자 `CompileCoordinator`(계획 14.6) | 머티리얼 에디터 갱신 + 셰이더 맵 컴파일 | 편집 → 컴파일 요청 관리 |
| 미리보기 세션 `PreviewSession`(계획 14.6) | 머티리얼 에디터 preview·`FMatExpressionPreview` | 미리보기 렌더 |
