# 머티리얼 시스템(MaterialSystem) 설계

## 16. 다른 문서에 반영할 요구

위치만 적는다. 실제 수정은 해당 문서 담당이 한다. ID는 다른 문서의 단계 ID(RenderBase RB1–RB7, TextureStreaming TS1 등)와 겹치지 않게 `M`으로 시작한다.

**셰이더 시스템(ShaderSystem/README.ko.md)**

| ID | 위치 | 요구 |
|---|---|---|
| MSR1 | 7.2절 4항(상한) | 머티리얼 pass 셰이더의 머티리얼 텍스처 예산 = (pass, 퍼뮤테이션, 형식)별 `형식 상한 − 렌더러 합계`(Renderer 7.4절, 결정 MD4). SRV 텍스처 상한은 프로필 공통 상한이 아니라 job 형식의 상한으로 검사하고, 워커 상한 검사 오류에 머티리얼·pass·퍼뮤테이션·형식을 담는다(Renderer RQ22 (2)). **반영(셰이더 시스템 개정 13, 7.2절 4항)** |
| MSR2 | 3절(셰이더 맵 키)·12.2절(조회 키) | 머티리얼 셰이더 집합 id를 키 축으로(8.6절). RenderBase SX5 묶음 id와 같은 값 |
| MSR3 | 3절(가상 경로)·결정 6(전처리기) | `/Engine/Generated/Material.rsh`를 job마다 메모리 텍스트로 잇는 규칙(7.2절) — 코드 구현(7.8절 MS2 완료: `ShaderCompileJob::materialSource`·`MaterialGeneratedIncludePath`), 셰이더 시스템 문서 반영은 담당 세션 |
| MSR4 | 13절(P7)·15절 결정 4 | `ShaderParameterLayout`을 C++ 타입 없이도 만들 수 있는 데이터 형식으로(동적 layout, 결정 MD3). 셰이더 시스템 개정 12가 결정 4를 MetaCompiler 방식으로 확정하고 13절에 이 요구를 적었다 |
| MSR5 | 12.2절(P14) | 셰이더 타입의 "머티리얼 셰이더" 표지와 거르기 인자 `MaterialShaderTraits`(8.3절), pass가 요구하는 custom 출력 선언. **반영(셰이더 시스템 개정 16, 12.2절)**: 머티리얼 셰이더 표지·custom 출력 선언, 이 문서 8.3절과 같은 두 거르기 서명과 AND 결합. 거르기는 `FormatId`로 형식 사실(`limits`·`features`)을 읽고 쿠커·런타임이 같은 값을 쓴다 |
| MSR6 | 3절 | 임의 HLSL 노드의 include 허용 목록(가상 경로 범위) |
| MSR7 | 11절(워커) | job 우선순위(미리보기 > 노드 미리보기 > 화면 > 백그라운드)와 취소 |
| MSR8 | 12.3절 | 프로젝트 셰이더 라이브러리에 머티리얼 셰이더 집합이 들어가는 규칙과 쿠킹 매니페스트 v2 항목 |
| MSR9 | 12.1절(형식 정의) | 기능 등급(Mobile·SM5·SM6) 속성과 기능 등급별 지원 shading model·노드 기능 표(V6·Capability Tier Switch 입력) |
| MSR10 | 12.2·12.3절("없으면 `IncompatibleCook`") | 머티리얼 셰이더는 예외로 (머티리얼, 정점 입력 타입, pass) 조합 단위 기본 머티리얼 대체 + 진단(11.3절). 엔진 셰이더와 엔진 기본 머티리얼은 지금 규칙 그대로 |
| MSR11 | 12.3절(`shader-compile --engine`) | 엔진 기본 머티리얼 에셋을 번역해 엔진 내장 라이브러리에 넣는 경로(11.3절) |
| MSR12 | 12.1절(형식 정의 `limits`) | 결정 MD4·RN13 개정. `sampledTextures`를 형식별로: `D3D12_SM6`·`METAL_SM5` 128 유지, `VULKAN_SM5`는 넓히고 장치 요구(`maxPerStageDescriptorSampledImages`·`maxPerStageDescriptorSamplers` 하한)를 더함, `D3D11_SM5`는 128에서 16으로, GLES는 16 유지(Renderer RQ22 (1)과 같은 요구). **반영(셰이더 시스템 개정 13)**: D3D11 16은 12.1절과 형식 정의에 들어갔고, Vulkan은 GX1 측정 뒤 |
| MSR13 | 7.2절 9항(텍스처·샘플러 짝) | 머티리얼 텍스처 slot의 샘플러 출처(자기 샘플러·공유 Wrap·공유 Clamp)와 공유 샘플러를 후속에서 앞당김. 공유 샘플러가 없으면 넓힌 형식도 샘플러 16에 묶인다(Metal 장치 상한 16). GLES는 공유 샘플러 객체를 텍스처 유닛마다 묶어 combined sampler 규칙을 유지한다(Renderer RQ23과 같은 요구). **반영(셰이더 시스템 개정 13, 7.2절 9항)**: 예약 이름 `SharedWrapSampler`·`SharedClampSampler`, 텍스처마다 출처 하나, SH2와 함께 구현 |
| MSR14 | 13절(`ShaderParameterLayout`) | (개정 10) 이름을 소유하는 동적 layout builder, 텍스처·샘플러 자원 slot 표(7.4절 서명의 자원 순번), 머티리얼별 layout을 job 키 8행·라이브러리 항목 레이아웃 서명에 넣는 경로. 지금 형식은 cbuffer 멤버·정적 수명뿐이다 |
| MSR15 | 12.2절(셰이더 타입·거르기) | (개정 10) 셰이더 타입의 문맥 공급 기능 선언과 `MaterialShaderTraits`를 받는 거르기 서명(MSR5의 실제 서명). 지금 `ShaderType.h:87–89`에는 traits가 없다 |
| MSR17 | 12.3절(프로젝트 셰이더 라이브러리) | 프로젝트 라이브러리 쿠킹 단계가 머티리얼 셰이더 묶음(묶음 id = 셰이더 집합 id 앞 8 byte, 생성 원문·layout·job 목록)을 받아 형식마다 항목으로 넣는 길(12.8절 MS3f). 필요한 것: (a) 처리기가 넘긴 묶음을 단계가 받는 통로(`CookArtifactRequest`에 칸, 또는 단계가 쿠킹 패키지의 셰이더 집합 id로 번역 너겟을 다시 조회 — 재사용된 대상은 Cook이 불리지 않는다), (b) 엔진 라이브러리 열거가 `ShaderTypeMaterialShader` 타입을 건너뛰기(`ShaderLibraryContents.cpp`), (c) `ShaderLibraryKindOf`가 소유자가 아니라 묶음 id로 머티리얼 항목을 프로젝트 라이브러리로 보내기, (d) 묶음 id 충돌 진단. 머티리얼 job은 `warningsAsErrors = false`(7.8절) |
| MSR16 | 7.2절 9항(공유 샘플러 선언) | 공유 샘플러 헤더 경로를 `/Engine/Private/Common/SharedSamplers.rsh`로(7.8절 — `/Engine/Shared`는 C++·HLSL 공용 `.h`만 받는다). 렌더 세션이 정본으로 받아 고치기로 함(2026-11-28) |
| MSR18 | 형식 정의(12.1절 JSON) | `platformClass`(Mobile·Pc·Console — UE DDSPI IsMobile·IsPC·IsConsole)를 형식 정의에 더한다(머티리얼 번역 대상의 형식 사실, 12.8절 MS3c). 그 전에는 MaterialCook이 형식 id로 임시 판정한다 |
| MSR19 | 12.2절(거르기 서명, MSR15 세부) | 머티리얼 pass 타입 거르기는 `MaterialShaderTraits`를 정규 바이트(`Span<const uint8>`)로 받는다(ShaderCore는 MaterialSchema를 모른다 — pass 타입이 MaterialSchema로 해석, 12.8절 MS3e) |

**렌더 기반(RenderBase/README.ko.md)**

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
| MRB9 | 10.2절(정점 입력 타입) | (개정 10) 정점 입력 타입이 채우는 문맥 공급 기능 집합(vertex·skin·particle·speedtree·instance)을 선언한다. 판정은 파이프라인(pass ∪ 정점 입력) 단위다(19.3.1절) |

**Renderer 스펙(Renderer.ko.md)** — 개정 3이 모두 처리했다. 처리 내용은 Renderer 14.7절이다.

| ID | 요구 | 처리(Renderer 개정 3) |
|---|---|---|
| MRD1 | 10.4절 D1: `worldPositionOffset`을 정점 함수 결과로 분리 | 반영(Renderer 14.1절) |
| MRD2 | 10.4절 D2: 번호표의 C++·HLSL 정의 위치(순환 해소) | 반영(Renderer 3.2·14.2절) |
| MRD3 | 10.4절 D3: 결정 RN13의 재검토 조건(측정 MM5) | 사용자 결정으로 RN13 개정(Renderer 7.4·19.1절) |
| MRD4 | 10.4절 D4: `AlphaHoldout` 처리 명시 | 반영(Renderer 14.2절, 후속 값) |
| MRD5 | 10.4절 D5: pass 거르기 인자를 `MaterialShaderTraits`로 | 반영(Renderer 14.3·15.3절) |
| MRD6 | view 상수의 translated world 기준과 절대 위치 표현(`Double3` 타입, 5.3절) | 반영(Renderer 5.1·14.7절, 상위·하위 두 부분) |
| MRD7 | instance 데이터 계약(머티리얼 `Instance` 빈도 입력, 이전 프레임 변환) | 이전 변환은 반영(Renderer 4.2·5.1절). custom data 슬롯은 Renderer RS0 GPU Scene 배치 때 |
| MRD8 | 기본 머티리얼 대체 표시 방식 | 반영(Renderer 14.7·16.2절, 덧칠 없음·계수) |
| MRD9 | 10.4절 D7: pass별 경로 전용·공통 구분과 렌더 경로·기능 등급 정의 이름 | 반영(Renderer 14.3절 `RENDER_PATH_*`). 기능 등급 이름은 MSR9 |
| MRD10 | Deferred·Forward+ 기준 영상 허용 오차(MS-T6) | 반영(Renderer RS2 (a), 채널 ±8·2%로 시작) |
| MRD11 | 10.4절 D8: Depth·ShadowDepth pass가 마스크 전용 함수를 부름 | 반영(Renderer 14.1·14.3절) |
| MRD12 | DeferredDecal이 쓰는 속성, 반투명 scene color·depth 입력, PostProcess 위치(5.6절) | 자리 반영(Renderer 14.7절): decal 속성은 RS7, 반투명 입력은 RS5, PostProcess 위치는 계획 26절 스펙 |
| MRD13 | (개정 10) pass 셰이더 타입이 채우는 문맥 공급 기능 집합(previous 부분 포함)과 기능별 렌더러 자원 수를 선언한다. pass는 자기가 부르는 진입점의 `MATERIAL_<진입점>_USES_*` 합집합으로 `MATERIAL_CONTEXT_<기능>`을 정의하고 그 필드를 채운다. 공급하지 않는 기능은 문맥 값 표의 UE 기본값을 쓴다(19.3.1절, 결정 MD15) | 미처리(Renderer 담당) |
| MRD14 | (개정 10) Toon shading model(UE `SubstrateToonBSDF`·`UToonProfile`)을 14.2절 목록에 둘지와 GBuffer 배치. 정해지면 `MPQ-ToonShading`을 푼다 | 미처리(Renderer 담당) |
| MRD15 | (개정 10) MS3b가 `RenderData` 모듈을 번호표 세 열거(`MaterialDomain`·`BlendMode`·`ShadingModel`)만으로 먼저 만든다(4.6절). 리플렉션 열거라 의존이 Core·MetaSchema에 ObjectRuntime이 더해진다. `BlendMode`에 `TranslucentColoredTransmittance`를 TODO 값으로 끝에 둔다(UE 값 전부 — 사용자 지시). Renderer 3.2절 의존과 14.2절 목록을 이에 맞춰 고친다 | 미처리(Renderer 담당) |
| MRD17 | (개정 25) `SurfaceAttributes`(14.1절)에 `subsurfaceColor`(float3, 기본 1 — UE `MP_SubsurfaceColor`)를 더하고, 칸을 UE 고전 픽셀 속성(`FPixelMaterialInputs`)과 같게 둔다(6.11절 — GBuffer 배치는 렌더러가 UE `SetGBufferForShadingModel` 규칙으로). customData0·1은 고전 MP_CustomData0·1 뜻(ClearCoat 강도·거칠기, Cloth fuzz 양, Eye IrisMask·IrisDistance, Hair Backlit), Subsurface·TwoSidedFoliage·SubsurfaceProfile은 opacity가 SSS 값이다. 대표 노멀 진입점 `EvaluateMaterialNormal`을 pass가 먼저 부른다(7.3절) | 미처리(Renderer 담당) |
| MRD16 | (개정 18) `SurfaceAttributes`(14.1절)에 굴절(`refraction` float3)·표면 두께(`surfaceThickness` float, cm) 칸을 더한다. 머티리얼 번역은 MS2a부터 두 값을 IR로 내지만(6.8절) 칸이 없어 생성 원문에 쓰지 않는다(7.7절). UE 기본값은 굴절 (1, 0, 0)·두께 0.01 cm다 | 미처리(Renderer 담당) |
| MRD18 | 3절 모듈 배치(61·106·122·127행 "MaterialCook → RendererShaders") | MaterialCook은 `RendererShaders`를 링크하지 않고 ShaderCore 셰이더 타입 등록부(등록부 원천)에서 `ShaderTypeMaterialShader` 타입을 본다(머티리얼 12.8절). 거르기는 traits 정규 바이트를 받는다(MSR19) |

**에셋 시스템(AssetSystem)**

| ID | 위치 | 요구 |
|---|---|---|
| MAS1 | 3절(개념과 모듈)·1절 단계표 | `Material` 에셋 모듈과 머티리얼 쿠킹 처리기 |
| MAS2 | 6.2절(참조 종류) | 머티리얼 참조 사례(4.4절 표), Build 의존 순환의 `UnsupportedCycle` |
| MAS3 | 12절(텍스처) | 텍스처 샘플 종류 검사(V10)용 메타데이터 조회 API. 노멀맵 쿠킹 형식(채널 수)을 프로필마다 고정해 머티리얼의 노멀 해석이 static이 되게 함(UE의 노멀맵 Z 복원 규칙은 미확인) |
| MAS4 | 10절(TargetProfile) | 쿠킹 머티리얼 품질 수준 목록 필드 |
| MAS5 | 10절(cookKey 지역 입력·부분 쿠킹) | 처리기가 AssetIndex 역방향 Hard 의존에서 유도한 값(머티리얼 사용 대상, 8.4절)을 cookKey 지역 입력으로 넣는 규칙. 그러면 메시 추가가 머티리얼 cookKey를 바꿔 부분 쿠킹 대상에 든다 |
| MAS7 | 10.2절(쿠킹 처리기 문맥) | 처리기가 쿠킹 대상의 Build 의존 패키지를 전이로 열어 읽는 길: `CookContext`에 좁은 인터페이스(예 `CookDependencyOpener* dependencies` — `Open(AssetId, AssetDiagnostic*)` 하나, CookRun이 `EditorPackageSet::Open`으로 구현, 닫기는 지금 Release·CloseOpened). 소유 스레드 전용, Open 뒤 이전 `EditorPackageInfo` 참조는 무효, 열기가 의존 패키지를 dirty로 만들어도 메모리 값을 쓴다(12.8절 MS3c). **구현**: 머티리얼 세션이 `CookDependencyOpener`(CookProcessor.h)·`CookContext::dependencies`를 더했다(2026-11-28) |
| MAS6 | 9절(의존 입력 튜플) | 의존 튜플의 세 번째 값이 "같은 처리기 build key"가 아닌 "의존 에셋 종류가 정한 의존 키"(함수 키·컬렉션 layout 서명, 12.2절)일 수 있다는 일반화. 머티리얼 번역 너겟은 AssetId 없는 평평한 키(셰이더 job 너겟과 같은 Record 키 — 에셋 처리기 SCC 식의 예외, static 변형 공유 8.5절)이고 bucket은 `material_translation`이다 |
| MAS8 | 쿠킹 진단 코드 | `AssetDiagnosticCode::MaterialTranslationFailed`(detail = MaterialDiagnosticCode, related = 진단 asset) — 노드·문구는 쿠킹 보고서(12.8절 MS3c) |

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
