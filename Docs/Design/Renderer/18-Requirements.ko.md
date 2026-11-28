# 렌더러(Renderer) 설계 — 18. 선행 작업에 반영할 요구

[목차](README.ko.md)

## 18. 선행 작업에 반영할 요구

실제 수정은 하지 않았다. 각 문서의 확정 때 반영한다. "기한"은 그 요구가 막는 단계다.

### 18.1 요구 목록

| ID | 대상 | 요구 | 기한 |
|---|---|---|---|
| RQ1 | GX2 버퍼 | 용도: structured(SRV·UAV), typed buffer(SRV·UAV, `PixelFormat` 지정), 간접 인자. typed buffer 형식 `R32Uint`·`RGBA32Float`(GLES image 가능 형식, 8.1절) **구현(2026-11-18, GX2 버퍼, RenderBase 4.4절)**: 버퍼 용도 비트·structured·raw·typed·간접 인자, 버퍼 view, `ReadBuffer`. GLES typed 형식은 ES 3.2 표 대조(`R32Uint`·`RGBA32Float` 읽기·쓰기 가능) | RS0 |
| RQ2 | GX2 텍스처 | `D32FloatS8Uint`·`D32Float` 깊이, 깊이 읽기 전용 view와 스텐실 plane view(PixelFormat 10절), MSAA, cube array(반사 capture), 3D(LUT·후속 GI), UAV 텍스처, mip별 view(HZB) | RS0·RS3 |
| RQ3 | GX4 | MRT 최대 8(타깃별 load·store·clear), 깊이 attachment 읽기 전용 표시, 스텐실 ref, dispatch·간접 dispatch, indexed instanced 간접 draw, 텍스처 복사(history·distortion), UAV clear, viewport·scissor **구현(2026-11-18, GX4 5beb4540)**: `BeginPass`(색 8·깊이·load·store·clear·읽기 전용 깊이), `Dispatch`·`DispatchIndirect`·`DrawIndirect`·`DrawIndexedIndirect`, `CopyBuffer`·`CopyTexture`(mip 0), `ClearWritableBufferView`, viewport·scissor, 스텐실 ref. 남은 것: mip·slice attachment(GX2 텍스처), 구조화 view UAV clear는 `Unsupported`(D3D12 debug layer 거부), GLES indexed 간접은 index offset 0만 | RS0 |
| RQ4 | GX1 | `maxColorAttachments`, 형식별 MSAA 표본 수, typed UAV load 형식(HZB `R32Float`·`RG16Float`), texel buffer 형식 능력, 깊이 clamp, VS layer·viewport 출력과 viewport 수, 64 bit atomic, RT tier, HDR 출력 능력 **구현(2026-11-18, GX1)**: `DeviceCaps`(MRT·형식별 MSAA·typed UAV load·texel buffer 형식·깊이 clamp·layer·viewport 출력·atomic64·RT tier·HDR 출력·viewport 수). 측정값은 RenderBase 4.4절 | RS0·RS3·RS11 |
| RQ5 | GX3 | 깊이 `GreaterEqual`·`Equal`, slope-scaled 깊이 bias, 깊이 clamp, 타깃별 블렌드(decal), 스텐실 연산, 색 쓰기 mask, alpha-to-coverage **구현(2026-11-18, GX3 5beb4540)**: `Graphics/PipelineState.h`의 비교 함수 전부(깊이 기본 reverse-Z `GreaterEqual`), 깊이 bias·기울기·상한, 깊이 clamp, 타깃별 블렌드·색 쓰기 mask, alpha-to-coverage, 스텐실 연산, 기본도형 5종, compute 파이프라인. 능력 밖은 `CheckPipelineFeatures`가 `Unsupported` | RS0·RS3·RS8 |
| RQ6 | GX5 | 깊이 읽기 전용 상태(DSV 읽기 + SRV 동시, D3D·Vulkan·Metal. GLES는 feedback loop이므로 깊이 복사 경로, 8.2절), compute 사슬의 UAV→SRV, MSAA resolve 상태 | RS0 |
| RQ7 | RenderBase 9.5·9.6절 | 15.2절 상수 slot 6개 등록, 자원 부분을 펼쳐 넣기 구조체로 공유하는 방식(같은 자원 묶음을 여러 pass가 포함)을 9.6절 규칙에 적는다. **반영(RenderBase 개정 15, 9.5·9.6절)**: slot 여섯의 선언 위치(`RendererShaders`)·묶는 단위·상수 버퍼 예산, 자원 묶음 규칙(이름 `<묶음>_<멤버>`, 쓰는 멤버만 의존). 개정 8: `PrecomputedLighting`을 그리기별 바인딩으로 옮겨 공유 slot은 다섯, `Scene`은 `ViewSet` 단위, 상수 버퍼 예산 최대 10(RenderBase 개정 16) | RS0 |
| RQ8 | RenderBase 10절 | 정점 입력 타입이 GPU Scene typed buffer로 인스턴스 변환을 읽는 규약, 인스턴스 offset 루트 상수. **반영(RenderBase 개정 15, 10.5절)**: `InstanceIndices[instanceListOffset + SV_InstanceID]`로 인스턴스 번호를 얻고 GPU Scene typed buffer를 읽는다. 늘 목록을 거친다(RenderBase 결정 R13). 루트 상수 앞 16 byte `MeshDrawConstants`. 개정 8: typed buffer 원소 상한과 넘침(RenderBase 결정 R16) | RS0 |
| RQ9 | PixelFormat 8.6절 | GLES texel buffer 형식(ES 3.2 Table 8.18)에서 `R32Uint`·`RGBA32Float` 대조(지금 "미대조") | RS1 |
| RQ10 | 셰이더 시스템 7.2절 4항 | GLES에서 typed buffer SRV가 텍스처 유닛을 차지함을 상한 계산에 명시(이미 "SRV 텍스처(typed buffer 포함)"이나 GLES `samplerBuffer` 관계를 적는다), SPIRV-Cross가 HLSL `Buffer<T>`를 ESSL `samplerBuffer`·`texelFetch`로 내는지 SH0 측정에 추가 | RS1 |
| RQ11 | 셰이더 시스템 12.1절(형식 정의) | `VULKAN_SM5` `renderTargets`를 8로 올리고 `deviceRequirements`에 `maxColorAttachments ≥ 8`(결정 RN3 (가)) 또는 별도 형식 id. **반영(셰이더 시스템 개정 14)**: (가)로 `renderTargets` 8. 장치 요구는 형식 `limits`를 런타임 형식 선택이 `DeviceCaps`와 대조하는 규칙으로 둔다(별도 어휘 없음) | RS7 전(RS2는 기본 layout으로 진행, 결정 RN3과 같다) |
| RQ12 | 셰이더 시스템 7.2절 4항·12.1절 | 렌더러 자원(`DeferredLightingPass`·`ForwardOpaque`·`Translucent`)을 GLES 보장치 16 안에 두는 예산표 유지(7.4절) | RS2 |
| RQ13 | 셰이더 시스템 12.2절(P14) | pass 셰이더 타입의 거르기 함수가 `MaterialShaderTraits`·정점 입력 타입·퍼뮤테이션·형식을 받는 것(SX4와 함께, 머티리얼 MSR5와 같은 요구), 쿠커의 퍼뮤테이션 수 보고. **반영(셰이더 시스템 개정 16, 12.2절)**: 머티리얼 셰이더·정점 입력 축 표지, 머티리얼 시스템 8.3절과 같은 서명과 AND 결합, 형식 사실 조회, 퍼뮤테이션 수 보고 | RS0 |
| RQ14 | 셰이더 시스템 7.2절 7항 | `SV_Target0–7`과 형식별 MRT 상한 검사(이미 있음) 확인, `SV_RenderTargetArrayIndex`(VS)를 능력 형식으로 열 때의 형식 id 규칙. **반영(셰이더 시스템 개정 16, 7.2절 4·7항)**: MRT는 이미 있었으나 프로필 공통 상한(4)으로 검사하던 것을 job 형식 `renderTargets` 검사로 고쳤다. VS 층 출력은 런타임 확인 기능 `vertexLayerOutput`이고 형식 id를 나누지 않는다(셰이더 시스템 결정 12). 개정 8: 2D atlas 한 번 그리기는 layer가 아니라 viewport 출력이므로 어휘를 `vertexViewportLayerOutput`으로 넓혔다(셰이더 시스템 개정 17), 공통 상한 폐기 | RS3(한 번 그리기는 후속) |
| RQ15 | RenderGraph | `GraphSharedData`로 `SceneTextureSet` 전달, history export·import, mip view 그래프 자원, view 여럿의 한 그래프 구성. **반영(RenderBase 개정 15, 7.5절)**: 렌더 그래프 별도 문서가 없어 RenderBase 7절에 두었다. 개정 8: `ViewSet` 여럿의 `SceneTextureTable`, history 풀 반납(RenderBase 결정 R15) | RS0 |
| RQ16 | RenderBase 11·14절 | MSAA 색·깊이 resolve 셰이더(깊이·velocity 전경 선택, 계획 11.1절)를 그리기 유틸리티로. **반영(RenderBase 개정 15, 11.2·14절)**: 색 휘도 가중 평균, 깊이 최댓값(전경), 같은 표본의 velocity, coverage 선택 출력. 개정 8: 깊이 늘·velocity는 temporal 때·색 수단(RenderBase 결정 R17), 8.2절 문구 | RS8 |
| RQ17 | RenderBase 11.1절 내장 자원 팩 | 빈 light grid, 빈 그림자(깊이 1 = 원평면, 9.2절 보통 Z 규약에서 비교 결과 "그림자 없음"), 검은 cube, 기본 BRDF LUT(또는 시작 때 생성). **반영(RenderBase 개정 15, 11.1절)**: BRDF LUT는 시작 때 CPU 생성 128×32 `RG16Float`(RenderBase 결정 R14). 개정 8: 검은 cube array 알파 0(8.3절 capture 알파) | RS1·RS3·RS7 |
| RQ18 | 머티리얼 시스템 | 14절 계약 전체, 특히 `SurfaceAttributes`·생성 함수(14.1절)·번호표 참조(`RenderData`, 결정 RN5)·pass 셰이더 목록과 경로·`MaterialShaderTraits` 거르기, (pass, 퍼뮤테이션, 형식)별 머티리얼 텍스처 상한 표시(7.4절), Render Path Switch의 깊이 일치 검사(14.3절). 머티리얼 시스템 개정 2가 10.1·10.4절에 반영했다 | MS2(MAT0) |
| RQ19 | AssetSystem 메시 단계 | 정적 메시: 위치·패킹 법선·tangent·UV, section·머티리얼 slot, bounds, LOD. 이동 물체 velocity를 위한 이전 위치는 렌더러가 변환으로 계산(skinned는 이전 pose를 렌더러에 넘김). **명세됨(2026-11-20)**: [메시·스켈레톤·애니메이션 에셋](../MeshAnimationAssets/05-StaticMesh.ko.md) 5.3절 `StaticMeshRenderData`(stream 집합은 RenderBase 10.4절 끝 "메시 에셋 요구"), 6.3절 `SkinnedRenderLod`(RS12용 영향·palette·inverse-bind·morph 희소 목록). 구현은 MA2·MA5 | RS0 |
| RQ20 | WorldIntegration | `SceneChangeList`·`ViewDesc` 생성, teleport·cut 표시, `RenderNotices` 처리 | RS0 |
| RQ21 | TextureStreaming | 렌더러가 텍스처 사용 기록별 마지막 그린 프레임 번호(4.2절)와 view별 mip bias(11.2절)를 넘기는 형식. TextureStreaming 4.1절 요구를 렌더러 쪽에서 받은 것 | RS0(사용 기록)·RS6(mip bias) |
| RQ22 | 셰이더 시스템 12.1절(형식 정의 `limits`)·7.2절 4항 | 결정 RN13 개정(2026-11-18 사용자 결정). (1) `sampledTextures`를 형식별로: `D3D12_SM6`·`METAL_SM5` 128 유지, `VULKAN_SM5`는 넓히고 `deviceRequirements`에 `maxPerStageDescriptorSampledImages`·`maxPerStageDescriptorSamplers` 하한을 넣는다(값은 GX1 장치 분포 확인 뒤, RQ11과 같은 방식), `D3D11_SM5`는 16, GLES는 16 유지. (2) 단계당 SRV 텍스처 상한 검사는 프로필 공통 상한(형식들의 최소값)이 아니라 job 형식의 상한으로 하고, 오류에 머티리얼·pass·퍼뮤테이션·형식을 담는다. 원문은 형식마다 바꾸지 않는다. **반영(셰이더 시스템 개정 13)**: (2)와 (1)의 D3D11 16은 7.2절 4항·12.1절에 들어갔다. Vulkan은 개정 14에서 128(장치 요구는 `limits`와 `DeviceCaps` 대조, `maxDescriptorSetSampledImages`·`maxPerStageResources` 합산 포함) | MS2·RS1 |
| RQ23 | 셰이더 시스템 7.2절 9항 | 짝 규칙의 예외로 머티리얼 텍스처의 **공유 샘플러**(UE `SSM_Wrap_WorldGroupSettings`·`SSM_Clamp_WorldGroupSettings`와 같은 Wrap·Clamp 두 종, view 상수 쪽 샘플러, mip bias 포함)를 후속에서 앞당긴다. GLES는 공유 샘플러 객체를 각 텍스처 유닛에 묶어 combined sampler 규칙을 유지한다. 이것이 없으면 RQ22로 넓힌 형식도 샘플러 16에 묶인다(7.4절). **반영(셰이더 시스템 개정 13, 7.2절 9항)**: SH2와 함께 구현한다. 공유 샘플러 상태(필터·이방성·mip bias)는 렌더러가 view마다 정해 파이프라인 layout의 공유 slot에 준다 | 머티리얼 텍스처가 16장을 넘는 콘텐츠 전(측정 MM5로 시점 판단) |
| RQ24 | RenderBase GX13·GX2 텍스처 | 메모리 없는 attachment 용도 비트: Vulkan `TRANSIENT_ATTACHMENT` + `LAZILY_ALLOCATED`, Metal memoryless, D3D는 일반 텍스처(21.10절, UE `TexCreate_Memoryless`) | RS8 |

### 18.2 문서별 개정 위치

| 문서 | 위치 | 현재 | 개정 |
|---|---|---|---|
| EngineImplementationPlan | 3.1절 모듈 표 "RenderGraph / Renderer" 행 | 의존 RHI·Task·Core·RenderData·SpatialIndex | Renderer → RendererShaders·RenderGraph·RenderBase·RenderData·MaterialRuntime·TextureStreaming·SpatialIndex. `RendererShaders` 행 추가 |
| 〃 | 11.1절 표 "광원" 행 | "clustered/tiled light list를 deferred pass에서 소비" | 같은 light grid를 전체 화면 조명 pass 하나가 소비(결정 RN1), tiled 없음 |
| 〃 | 11.4절 공통 전반부 | "light cluster·표면 shading" | 그림자 depth를 base pass 전(6.2절 F7), 하늘·안개 전체 화면 pass(결정 RN8) |
| 〃 | 14.2절 | "cascaded/atlas shadow" | 두 경로 직접 표본, point 6면(결정 RN10) |
| 〃 | 14.3절 조합 규칙 | 동적 GI 결과 소비 | `IndirectLightingInputs` 두 제품(12.2절), 정적 배율 두 값 |
| RenderBase | 4.4절 GX1·GX2·GX3·GX4·GX5 항목 | 일반 범위 | RQ1–RQ6 항목 추가 |
| 〃 | 9.5·9.6절 공유 상수 slot·펼쳐 넣기 | 일반 규칙 | 렌더러 상수 slot 목록과 공유 자원 묶음(RQ7). **반영(RenderBase 개정 15)** |
| 〃 | 7절 렌더 그래프 | 일반 규칙 | RQ15. **반영(RenderBase 개정 15, 7.5절)** |
| 〃 | 10절 정점 입력 타입 | 인스턴스 데이터 규약 없음 | RQ8. **반영(RenderBase 개정 15, 10.5절)** |
| 〃 | 11.1절 내장 자원 팩 | 1차 내용 | RQ17. **반영(RenderBase 개정 15)** |
| 〃 | 14절 표 "GBuffer 메타"·"MSAA resolve"·"렌더러 모듈 인터페이스" 행 | "Deferred 경로", "Forward+ MSAA", "Renderer 설계" | 이 문서 3.2절 배치(GBuffer 메타 `RendererShaders`, resolve RenderBase 유틸리티 RQ16). MSAA resolve 행은 **반영(RenderBase 개정 15, 11.2절)**, GBuffer 메타·렌더러 인터페이스 행은 남았다 |
| 〃 | 18.1절 "범위 밖" 첫 항목 | "Forward+·Deferred 조명 경로와 GI(계획 11·14절)" | 이 문서 17절 RS0–RS11 참조 |
| ShaderSystem | 7.2절 4항 | GLES 모바일 상한, SRV 상한을 프로필 공통 상한으로 검사 | RQ10·RQ12, SRV 텍스처는 job 형식 상한으로 검사(RQ22) |
| 〃 | 7.2절 9항 | 공유·정적 샘플러는 후속 | 머티리얼 공유 샘플러(RQ23) |
| 〃 | 12.1절 형식 정의 | `VULKAN_SM5` `renderTargets` 4, `sampledTextures` D3D11 128·Vulkan 16 | RQ11(결정 RN3), RQ22(결정 RN13 개정) |
| 〃 | 12.2절 P14 | 셰이더 타입·거르기 | RQ13. **반영(셰이더 시스템 개정 16)** |
| 〃 | 7.2절 7항 | 시스템 값 목록 | RQ14. **반영(셰이더 시스템 개정 16, 4·7항)** |
| PixelFormat | 8.6절 texel buffer 열 | "미대조" | RQ9 |
| TextureStreaming | 4.1절 사용 기록·표본 해상도 입력 | 렌더러 쪽 형식 미정 | 렌더러가 넘기는 사용 기록·mip bias 형식(RQ21) |
| RenderingGuide | 9절 | 계획 기반 개요 | 이 문서 6.2절 순서와 13절 지원표 참조 |
| EngineRuntime | 10.3절 범위 밖 | "Deferred/Forward 경로" | 이 문서 참조 |
| 머티리얼 시스템 | 10.1절 경계·10.4절 정합 결과 | 개정 2에서 반영 | 14절 계약(RQ18). 이 문서 쪽 처리는 14.7절 |
