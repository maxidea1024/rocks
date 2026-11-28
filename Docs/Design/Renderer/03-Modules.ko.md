# 렌더러(Renderer) 설계 — 3. 모듈 배치

[목차](README.ko.md)

## 3. 모듈 배치

### 3.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 렌더러 선택 | feature level이 SM5 이상이면 deferred 렌더러(desktop forward 포함), 아니면 모바일 렌더러를 만든다 | `R/SceneRenderBuilder.cpp:499–522`, `EN/Public/SceneUtils.h:23–33` |
| desktop forward | 별도 렌더러가 아니다. `r.ForwardShading`(읽기 전용, 플랫폼별 mask)이 `FORWARD_SHADING` 셰이더 정의와 GBuffer 사용 여부를 가른다 | `RU:737–745, 1680–1696`, `EN/Private/ShaderCompiler/ShaderCompiler.cpp:3962–3969` |
| 모바일 | 별도 렌더러 클래스. 자체 forward·deferred 모드 | `R/SceneRendering.h:2957`, `R/MobileShadingRenderer.cpp:404, 1181–1909` |
| 셰이더 타입 | pass 셰이더(머티리얼 결합)가 Renderer 모듈 안에 등록된다 | `R/BasePassRendering.cpp:180–192` |
| GBuffer 메타 | RenderCore에 있다 | `RC/Public/GBufferInfo.h:282` |

### 3.2 Rocks 배치

| 모듈 | 위치 | 직접 의존 | 책임 | 금지 |
|---|---|---|---|---|
| `RenderData` | `Engine/Source/Runtime/RenderData` | Core·MetaSchema | `RenderInstanceDesc`·`RenderLightDesc`·`ViewDesc`·`SceneChangeList` 값 타입, `RenderInstanceId`·`RenderLightId`·`ViewId`, lane 전달 가능 등록(`ROCKS_LANE_TRANSFERABLE` — MetaSchema, RenderBase 5.2절), `ShadingModel`·`BlendMode`·`MaterialDomain` 번호표의 C++ 정의(결정 RN5, 14.2절) | 장치·Object·World |
| `RendererShaders` | `Engine/Source/Runtime/RendererShaders` | ShaderCore·RenderData·MaterialSchema | pass 셰이더 타입·퍼뮤테이션 도메인·거르기 함수(`MaterialShaderTraits`를 받는다, 14.3절), `GBufferLayout` 정의와 shading model의 GBuffer 의미, 파라미터 구조체 선언 | 장치·Renderer |
| `Renderer` | `Engine/Source/Runtime/Renderer` | RendererShaders·RenderGraph·RenderBase·RenderData·MaterialRuntime·TextureStreaming·SpatialIndex·Task·Core | `RenderScene`·`RenderView`·가시성·`GpuScene`·light grid·그림자·두 경로의 pass 조립·`PostChain`·GI 연결점 | World·ObjectRuntime·AssetRuntime |
| `WorldIntegration` | 계획 3.1 | World·RenderData·… | 컴포넌트를 `SceneChangeList`와 `ViewDesc`로 바꾼다. `RenderNotices`를 받아 게임 쪽 해제. 받는 컴포넌트 목록과 RS 단계별 도입 순서는 [컴포넌트 도입](../ComponentCatalog/03-RequiredComponents.ko.md) 3·4.6절(CR12) | Renderer 내부 주소 |

의존 방향(위가 아래를 쓴다):

```text
WorldIntegration ──▶ RenderData ◀──────────────────────┐
      │                  ▲                              │
      ▼ (프레임 packet)   │ (번호표)                      │
Renderer ──▶ RendererShaders ──▶ MaterialSchema ──┘      │
   │   │            └──────▶ ShaderCore                  │
   │   ├──▶ MaterialRuntime ──▶ RenderBase ──────────────┤
   │   ├──▶ TextureStreaming ──▶ RenderBase              │
   │   └──▶ RenderGraph ──▶ RenderBase ──▶ Graphics      │
   └─────────────────────────────────────────────────────┘
MaterialCook(Host) ──▶ ShaderCore 셰이더 타입 등록부(등록부 원천 — RendererShaders가 등록한 ShaderTypeMaterialShader 타입)
```

- **MaterialCook과 `RendererShaders`(개정 14, 머티리얼 시스템 MRD18·셰이더 시스템 16.5절 SSD92).** MaterialCook은 `RendererShaders`를 링크하지 않는다. 쿠킹할 pass 셰이더 타입은 ShaderCore 셰이더 타입 등록부에서 `ShaderTypeMaterialShader` 표지를 단 타입이고, 등록부는 셰이더 라이브러리 쿠킹 단계와 같은 원천(`ShaderCookRegistrySource`)에서 받는다. `RendererShaders`는 그 원천이 시작하는 엔진 모듈 집합에 들어가 자기 타입을 등록한다. pass 타입 거르기는 `MaterialShaderTraits`를 정규 바이트로 받아(`ShaderMaterialShouldCompileFunction`, 셰이더 시스템 SSD86) `RendererShaders`가 MaterialSchema로 해석한다.

- **번호표 위치(결정 RN5, 머티리얼 시스템 10.4절 D2).** 번호표의 정의 권한은 이 문서(14.2절)에 있다. 코드는 두 곳에 둔다: C++ 열거는 `RenderData`, HLSL 상수는 `/Engine/Shared/`의 C++·HLSL 공용 `.h`(셰이더 시스템 3절)다. C++ 열거는 공용 헤더의 값과 `static_assert`로 맞춘다. MaterialSchema는 `RenderData`만 참조하므로 `RendererShaders` → MaterialSchema 의존과 순환이 생기지 않는다. GBuffer 인코딩 의미(GBufferB.a 4 bit 등)는 `RendererShaders`에 남는다. UE도 C++·셰이더 공용 정의를 `Engine/Shaders/Shared/`에 둔다(예 `Engine/Shaders/Shared/SubstrateDefinitions.h`).

- `RendererShaders`를 따로 두는 이유: 머티리얼 쿠킹은 "이 머티리얼로 컴파일할 pass 셰이더 타입 전체"를 알아야 한다. UE는 그 목록이 Renderer 모듈의 정적 등록이고 에디터·쿠커가 Renderer를 링크한다(`R/BasePassRendering.cpp:180–192`). Rocks 쿠커(Host)는 장치 모듈을 링크하지 않으므로 장치 없는 등록부를 분리한다(결정 RN11). 쿠커 안에서도 MaterialCook이 직접 링크하지 않고 등록부 원천으로 본다(개정 14).
- RenderBase 14절이 "Renderer 설계"로 미룬 항목(`GBufferInfo`·`RendererInterface`·`SceneRenderingAllocator`·`ReadOnlyCVARCache`, MSAA resolve)은 이 배치를 따른다: GBuffer 메타는 `RendererShaders`, 장면 할당은 RenderBase CP3 프레임 arena, 설정은 ConfigSystem snapshot(RenderBase 12절), MSAA resolve는 RenderBase 그리기 유틸리티(18절 RQ16).
- MaterialRuntime은 Renderer에 의존하지 않는다. Renderer가 MaterialRuntime의 셰이더 집합·파라미터 묶음을 소비한다(14절). 계획 14.6절 MaterialRuntime 의존(Core·RenderData·RHI 계약)과 같은 방향이고, 머티리얼 시스템 3.2절이 그 의존을 MaterialSchema·RenderBase·ShaderCore로 구체화했다.
