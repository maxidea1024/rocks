# 머티리얼 시스템(MaterialSystem) 설계

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
| **MaterialSchema** | Engine Runtime | 머티리얼 그래프 schema(`MaterialGraph` = NodeGraph `GraphDocument`, 도메인 `Material`)와 `GraphSchema` 구현, 노드 descriptor 표(데이터만), 파라미터 schema, 품질 수준·기본 속성 묶음, `MaterialShaderTraits`, semantic hash. 영역·블렌드 모드·shading model 번호표는 참조만 한다(Renderer 소유, 10.4절 D2) | Core·MetaSchema·Serialization·NodeGraph(ObjectRuntime 포함 — 리플렉션 codec)·ShaderCore(셰이더 타입 id만)·RenderData(번호표 C++ 정의, 10.4절 D2) | RHI·Graphics·RenderBase·ObjectRuntime 객체 포인터·노드 lowering 코드 |
| **MaterialCompiler** | Engine Developer | 그래프 검증, 함수·레이어 펼치기, IR 생성·분석·최적화, HLSL 생성, 상수 식 생성, layout, 진단·원문 위치 지도 | MaterialSchema·ShaderCore·Core | 장치·프로세스 실행(셰이더 컴파일은 하지 않는다)·EditorFramework |
| **MaterialRuntime** | Engine Runtime | `MaterialRenderState`, 상수 식 실행, 상수 블록 채움, 텍스처 handle 해석, 셰이더 집합 열기, 기본 머티리얼 대체 | MaterialSchema·RenderBase·ShaderCore·GraphicsTypes | ObjectRuntime·AssetRuntime(렌더 층 "Object 없음", 계획 3.1절)·그래프·컴파일러 |
| **Material**(계획의 MaterialAssetBridge) | Engine Runtime | 에셋 클래스(`MaterialBase`·`Material`·`MaterialInstance`·`MaterialFunction`·`MaterialFunctionInstance`·`ParameterCollection`), `DynamicMaterialInstance`, 쿠킹 데이터 → `MaterialRenderState` 생성 | AssetRuntime·ObjectRuntime·MaterialSchema·MaterialRuntime | 컴파일러·셰이더 도구 |
| **MaterialCook** | Engine Developer | 번역 너겟·셰이더 job 목록·쿠킹 처리기·퍼뮤테이션 보고 | Material·MaterialCompiler·RendererShaders(pass 셰이더 타입 목록)·AssetCooker·AssetNugget·ShaderCompiler | Release 링크 금지 |
| **EditorMaterial** | Engine Editor | `CompileCoordinator`·`PreviewSession`·그래프 편집 명령(계획 14.6절) | 위 모듈·EditorBackend·Renderer·GraphView | 화면 코드에서 shader 의미·검증 재구현 금지(계획 14.6절) |

계획과 다른 점:
- **에셋 모듈 이름.** 계획의 MaterialAssetBridge는 에셋 클래스가 사는 곳이다. 텍스처 에셋 모듈이 `Texture`(AssetSystem 3절)이므로 `Material`로 맞춘다(결정 MD8).
- **MaterialRuntime 의존.** 계획의 "Core·RenderData·RHI 계약"은 RenderBase(셰이더 집합·바인딩·상수 블록)로 바뀐다. RenderBase가 계획 뒤에 생겼기 때문이다.
- **MaterialSchema가 특성 타입을 가진다.** pass 셰이더 거르기 함수가 머티리얼 특성을 받아야 하는데, 이 거르기는 쿠커(Developer)와 게임(Runtime) 모두에서 실행된다. 그래서 장치 없는 MaterialSchema에 둔다.
- **MaterialCompiler는 셰이더를 컴파일하지 않는다.** 그래프 → 원문까지만 하고, job 제출은 MaterialCook·CompileCoordinator가 ShaderCompiler(셰이더 시스템 3절)로 한다. 그래서 MaterialCompiler는 프로세스·도구 없이 시험된다(계획 MT08 headless).
- **계획의 ShaderWorker**는 셰이더 시스템 이름 ShaderCompileWorker(셰이더 시스템 14절)다.
- **그래프 공통 층.** 그래프 데이터 모델·변경·undo·복사·노드 등록표는 공통 모듈 `NodeGraph`([노드 그래프](../NodeGraph/11-ModulesAndView.ko.md) 11절)에 있고, MaterialSchema는 그 위의 도메인 schema다. NodeGraph도 Runtime 계층이라 MaterialSchema의 Runtime 배치가 유지된다. 리플렉션 struct의 생성 codec이 ObjectRuntime 헤더를 쓰므로 둘 다 ObjectRuntime 모듈에 의존한다. 객체 포인터는 금지 그대로다(노드 그래프 개정 4). 그래프 편집 화면은 `GraphView`(Editor)다.

의존 방향(→는 의존):

```text
EditorMaterial → { MaterialCook, MaterialCompiler, Material, Renderer, EditorBackend, GraphView }
MaterialCook   → { Material, MaterialCompiler, RendererShaders, 정점 입력 타입 선언(16절 MRB8), AssetCooker, AssetNugget, ShaderCompiler }
MaterialCompiler → { MaterialSchema, ShaderCore, Core }
Material(Runtime) → { AssetRuntime, ObjectRuntime, MaterialSchema, MaterialRuntime }
MaterialRuntime → { MaterialSchema, RenderBase, ShaderCore, GraphicsTypes }
MaterialSchema → { Core, MetaSchema, Serialization, NodeGraph, ShaderCore, RenderData(번호표) }
Renderer → { RendererShaders, MaterialRuntime, RenderBase, RenderGraph, … }   (Material 에셋 모듈은 모른다)
RendererShaders(Renderer 3.2절) → { ShaderCore, RenderData, MaterialSchema }
```

**pass 셰이더 선언 모듈.** 쿠커는 "이 머티리얼에 어떤 pass 셰이더가 필요한가"를 알아야 한다. UE는 pass 셰이더 타입을 Renderer 모듈이 정적으로 등록하고(예 `RR/Private/BasePassRendering.cpp:180–192`), 셰이더 맵 layout을 만들 때 등록된 셰이더 타입 전체를 훑는다(`RE/Private/Materials/MaterialShader.cpp:3210–3310`). 그래서 Renderer 모듈이 로드되어야 표가 찬다. Rocks는 Renderer 스펙이 장치 없는 `RendererShaders` 모듈을 두었다(Renderer 3.2절, 결정 RN11). MaterialCook과 Renderer가 모두 이 모듈을 링크한다. 정점 입력 타입 선언도 같은 이유로 장치 없는 위치에 있어야 한다(16절 MRB8).

**번호표와 순환.** `RendererShaders`는 거르기 함수 인자 `MaterialShaderTraits` 때문에 MaterialSchema에 의존한다. 번호표를 `RendererShaders`에 두면 MaterialSchema → `RendererShaders` → MaterialSchema 순환이 생긴다. 그래서 번호표의 정의 권한은 Renderer 스펙에 두고(결정 RN5), C++ 열거는 장치 없는 `RenderData`에, HLSL 상수는 `/Engine/Shared/`의 C++·HLSL 공용 `.h`(셰이더 시스템 3절)에 둔다(Renderer 3.2절, 10.4절 D2). 의존은 `RendererShaders` → MaterialSchema → `RenderData`로 한 방향이다.
