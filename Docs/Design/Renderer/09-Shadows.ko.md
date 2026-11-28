# 렌더러(Renderer) 설계 — 9. 그림자

[목차](README.ko.md)

## 9. 그림자

### 9.1 UE 5.8 방식

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| CSM | directional, 기본 최대 10단(scalability 1–10), 해상도 2048, `PF_ShadowDepth` | `R/SceneRendering.cpp:470–475`, `Engine/Config/BaseScalability.ini:135,170,205,243,281`, `Core/Private/HAL/ConsoleManager.cpp:4367–4375`, `R/ShadowSetup.cpp:6154–6202` |
| 분할 | 광원 컴포넌트가 분할 거리 계산 | `EN/Private/Components/DirectionalLightComponent.cpp:786, 928` |
| atlas | 물체별·spot 그림자는 "ShadowDepthAtlas" | `R/ShadowSetup.cpp:5892–5931, 5971, 6056–6075` |
| point | 한 번에 그리는 cube. 정점 셰이더 layer 출력이 없으면 geometry shader로 대체 | `R/ShadowSetup.cpp:6215–6239`, `R/ShadowDepthRendering.cpp:1883`, `RU:1672–1678` |
| 깊이 규약 | 일반 그림자는 보통 Z(`CF_LessEqual`, clear 1), one-pass point만 reversed-Z. 형식 `PF_ShadowDepth`(D16, PixelFormat 10절) | `R/ShadowDepthRendering.cpp:856–865`, `R/ShadowSetup.cpp:6074–6075` |
| 그리기 순서 | VSM → atlas → cube | `R/ShadowDepthRendering.cpp:1836–1884` |
| 투영 | 광원마다 screen shadow mask에 투영(shadow map·VSM·capsule) | `R/ShadowRendering.cpp:2468–2513` |
| VSM | 엔진 기본 꺼짐(새 프로젝트 켬), Nanite·64 bit atomic·forward 아님 필요. 페이지 128², 가상 16k | `R/VirtualShadowMaps/VirtualShadowMapArray.cpp:160–170`, `Editor/GameProjectGeneration/Private/GameProjectUtils.cpp:136–143`, `RU:1274–1328, 1425–1434, 1498–1526`, `Engine/Shaders/Shared/VirtualShadowMapDefinitions.h:12–20` |
| 거리장 그림자 | 기본 켬, mesh distance field 필요(엔진 기본 꺼짐) | `R/DistanceFieldShadowing.cpp:36–40, 798, 1122`, `EN/Private/DistanceFieldAtlas.cpp:55–57` |
| contact shadow | 조명 셰이더 안 screen-space ray | `SH/DeferredLightingCommon.ush:14, 48`, `R/LightRendering.cpp:285, 669` |
| capsule·RT 그림자 | capsule 기본 켬, RT 그림자 기본 꺼짐 | `R/CapsuleShadowRendering.cpp:36–40`, `R/LightRendering.cpp:83–88`, `R/RayTracing/RayTracingShadows.cpp:383` |

### 9.2 Rocks 구조

| 그림자 | 방식 | 표본 | 단계 |
|---|---|---|---|
| directional | CSM, 1차 최대 4단(설정), shadow atlas 타일(단마다 하나), 직교 투영 | 조명 함수 안 직접 PCF(비교 샘플러, SX13) | RS3 |
| spot | atlas 타일, 원근 투영 | 직접 PCF | RS3 |
| point | atlas 타일 6개(면마다 그리기 1회) | 면 선택 후 직접 PCF | RS3 |
| rect | spot과 같은 반구 투영(근사) | 직접 PCF | RS9 |
| contact shadow | 조명 함수 안 screen-space ray(깊이 버퍼) | — | RS10 |
| 가상 그림자(VSM 유사) | 계획 14.2절 별도 모듈 | — | 후속(별도 스펙) |
| 거리장·RT 그림자 | SDF(DynamicGI G3)·`RayScene` 공유 | — | 후속 |

- **screen shadow mask를 두지 않는다.** 두 경로가 `EvaluateLight` 안에서 atlas·CSM을 직접 표본한다. 그래서 그림자가 F7(base pass 전)에 있어야 하고(6.2절), Forward+의 채널 상한이 없다. 대가는 픽셀당 그림자 표본 비용이고, 측정 MR1에 포함한다.
- **point 6면.** atlas 타일 6개에 면마다 그린다. 한 번 그리기는 정점 셰이더가 타일마다 viewport를 고르는 viewport index 출력(`SV_ViewportArrayIndex`)이 필요하다. layer 출력(`SV_RenderTargetArrayIndex`)은 cube·array 렌더 타깃용이라 2D atlas(결정 RN10 (가))에는 맞지 않는다(개정 8에서 바로잡음). 셰이더 시스템 개정 17은 두 출력을 런타임 확인 기능 `vertexViewportLayerOutput`으로 열었다(7.2절 7항, 결정 12). UE의 geometry shader 대체(`R/ShadowDepthRendering.cpp:1883`)는 Rocks 셰이더 단계(VS·PS·CS)에 없다. 그래서 1차는 면마다 그리기이고, viewport 출력이 되는 장치의 한 번 그리기는 능력 확장 후속이다(결정 RN10).
- **atlas 하나.** CSM 단과 local 광원 그림자를 같은 atlas에 둔다. 단계당 SRV 16 예산 때문이다(7.4절). 단마다 크기가 크므로 atlas 기본 크기는 CSM 단 수 × 단 해상도를 담을 수 있게 정한다.
- **깊이 규약과 형식.** UE를 따른다. 그림자 depth는 보통 Z(가까움 0, `LessEqual`, clear 1)이고 형식은 `D16Unorm`이다(PixelFormat 10절 그림자 기본). 화면 깊이(reversed-Z 무한 원평면)와 다른 이유는 그림자 투영은 유한 원평면이고 UE도 그렇게 쓰기 때문이다. 정밀도가 부족한 장면용 `D32Float` atlas는 설정으로 둔다. 비교 샘플러 함수는 이 규약에 맞춘다(SX13). slope-scaled bias·직교 투영의 깊이 clamp(pancaking)가 GX3에 필요하다(RQ5). 깊이 clamp 능력이 없는 backend는 그림자 depth 정점 셰이더가 깊이를 clamp하는 셰이더 대체를 쓰고 지원표에 그렇게 표시한다(Rocks 설계, UE 대응은 확인하지 않았다).
- **caster 판정.** 광원 view마다 별도 frustum 판정(V5)이고 caster 집합·revision이 같은 view 묶음끼리만 그림자를 공유한다(계획 11.4절).
- **예산.** atlas 크기·CSM 해상도·광원당 해상도는 화면 크기 기반 할당이다. atlas가 차면 낮은 우선 광원부터 해상도를 내리고, 그래도 넘치면 그 광원을 "그림자 없음"으로 그리되 overflow 계수를 올린다(조용히 빠뜨리지 않는다).
- **정적 그림자.** `StaticLighting::Baked`의 Mixed 광원은 정적 caster 그림자를 shadow mask(Deferred GBufferE, Forward+ base pass 직접 표본)로 받고 이동 caster만 동적 그림자로 그린다(계획 14.10절). 채널 4개 상한과 overflow 처리는 bake 스펙(14.10절)과 같이 정한다.
