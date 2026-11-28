# 렌더러(Renderer) 설계 — 8. Forward+

[목차](README.ko.md)

## 8. Forward+

### 8.1 light grid

**UE 5.8 방식.**

| 항목 | UE 5.8 | 근거 |
|---|---|---|
| 셀 | XY 64 px, Z 32단. 셀당 상한 설정(기본 32)이 있으나 연결 목록 방식(기본 켬)에서는 셀 상한이 없어지고 전체 상한(가시 local 광원 수)이 된다 | `R/LightGridInjection.cpp:71–85, 123–135, 1681, 1720–1722` |
| Z 분포 | `slice = log2(z·B + O)·S`, 분포 계수 4.05, 마지막 단은 먼 범위 전용 | `R/LightGridInjection.cpp:627–634`, `RC/Public/RenderUtils.h:721–740`, `SH/LightGridCommon.ush:40–45` |
| 먼 평면 | 가장 먼 광원·반사 capture까지 + 10 | `R/LightGridInjection.cpp:1726–1730` |
| 생성 | compute, 선택 HZB 판정, 선택 2단 grid, 선택 async | `R/LightGridInjection.cpp:47–52, 107–113, 435–610, 937–1047, 1818–1851` |
| 출력 | 셀별 개수 버퍼, 셀별 광원 index(16 bit typed 또는 32 bit structured), 광원 데이터(광원당 float4 6개) | `R/LightGridInjection.cpp:911–935`, `R/BasePassRendering.h:60–69`, `R/SceneRendering.h:728–763` |
| directional | grid 밖 별도 목록과 첫 directional 전용 상수 | `R/SceneRendering.h:728–763` |
| 소비자 | forward base pass, 투명, 볼륨 안개, 반투명 조명 볼륨, 모바일 | `R/BasePassRendering.cpp:677`, `R/TranslucentRendering.cpp:1181`, `R/VolumetricFog.cpp:1790`, `R/MobileShadingRenderer.cpp:1399–1400` |

**Rocks 방식.**
- 셀 64 px × 64 px, Z 32단, 같은 로그 분포 식이다. reversed-Z이므로 셀 Z는 깊이 버퍼 값이 아니라 **view 공간 Z(전방 거리)**로 계산한다(5.1절). 값은 콘솔 설정(ConfigSystem)으로 두고 프레임 packet snapshot으로 넘긴다.
- **생성.** CS 두 단계다. (1) 셀마다 view 공간 광원 bounds(구·원뿔, rect는 구로 보수 근사)를 셀 frustum과 판정해 개수를 센다. HZB(furthest)가 유효하면 셀 깊이 범위를 좁힌다. (2) 개수의 prefix sum으로 셀마다 목록 시작 위치를 정하고 index를 쓴다. 그래서 셀당 상한이 없다(UE 연결 목록 방식과 같은 효과, 전체 상한만 있다).
- **전체 상한 넘침.** 목록 버퍼는 전체 용량을 가진다. 이번 프레임 합계가 용량을 넘으면 넘친 셀에 "넘침" 표시를 하고, 셰이더는 그 셀에서 가시 local 광원 **전체**를 순회한다(느리지만 광원을 빠뜨리지 않는 보수 대체). 넘침 수를 계측에 남기고 다음 프레임에 용량을 늘린다. 계획 13.1절 "overflow 시 객체를 임의로 소실시키지 않고 … 보수적 fallback·다음 frame 재요청"을 따른다.
- **원소 상한(개정 8).** 목록 버퍼(셀 머리 + 광원 index)와 광원 데이터 버퍼의 용량은 typed buffer view 원소 상한 `DeviceCaps.limits.texelBufferElements` 이하로 둔다(GLES·Vulkan 최소 보장 65536, RenderBase 10.5절·결정 R16). 넘치면 위 넘침 보수 대체를 쓴다. 셀 조회는 셀 좌표를 grid 크기 − 1로 clamp한 뒤 읽는다. 범위 밖 texel fetch 결과에 기대지 않고, 빈 grid(1×1×1 내장 자원, RenderBase 11.1절)도 같은 경로로 셀 0을 읽는다.
- **형식.** grid 자원은 모든 backend에서 **typed buffer**다: 광원 데이터 `RGBA32Float`, 셀 머리(시작·개수)와 광원 index를 합친 `R32Uint` 하나(7.4절 SRV 예산). PS는 typed buffer SRV로 읽는다. ES 3.2 PS의 storage block 최소 보장이 0이기 때문이고(셰이더 시스템 7.2절 4항), 계획 11.1절의 GLES "typed buffer(texture buffer)로 읽는다"를 모든 backend로 넓혀 셰이더 경로를 하나로 둔다. CS는 같은 버퍼를 typed UAV로 쓴다. 쓰기 형식을 `R32Uint`·`RGBA32Float`로 고른 이유는 GLES image 형식이 이 둘을 받기 때문이다(PixelFormat 8.6절 "image" 표시, `R16Uint`에는 없다). 두 형식이 GLES texel buffer(읽기) 형식인지는 PixelFormat 8.6절에서 아직 대조하지 않았다(**미확인**, RQ9).
- **광원 데이터.** 광원당 float4 6–8개: 위치·역반경, 색·광량, 방향·spot 각, 종류·그림자 index·IES·광원 함수 index, source 크기. 단위는 계획 14.2절(directional lux, local lumen·candela)이며 렌더러는 GPU에 올리기 전에 내부 단위로 바꾼다.
- 로컬 광원이 없는 view는 grid 생성을 건너뛰고 빈 grid(내장 자원)를 묶는다(계획 11.1절).

### 8.2 Forward+ depth prepass와 MSAA

- 전체 prepass(F4), base pass 깊이 `Equal`·쓰기 없음. UE도 forward에서 전체 prepass를 강제하고 base pass를 깊이 읽기로 둔다(`RU:681–700`, `R/RendererScene.cpp:4408–4419`, `DSR:2105–2114`).
- base pass가 깊이를 읽기 전용 attachment로 붙인 채 같은 깊이를 SRV로 읽는 기능(Forward+ contact shadow)은 GLES에서 feedback loop이므로 깊이 복사본을 읽는다(7.4절). D3D·Vulkan의 읽기 전용 깊이 + SRV 동시 사용은 GX5 요구(RQ6)다.
- MSAA 1·2·4(계획 11.1절, 기본 4x는 품질 profile). UE도 MSAA는 forward 경로만 받는다(`EN/Private/SceneUtils.cpp:184–225`, `EN/Classes/Engine/RendererSettings.h:731–736`). MSAA view는 velocity를 depth pass에서 쓰지 않고 별도 pass로 쓴다(UE 조건 `R/VelocityRendering.cpp:821–826`).
- MSAA view의 resolve는 RenderBase 11.2절 유틸리티다(RQ16, 개정 8에서 범위를 맞춤). 깊이는 temporal 유무와 무관하게 늘 resolve한다(전경 표본, HZB·하늘·안개·후처리가 단일 표본 깊이를 읽는다). velocity는 temporal·motion blur가 있을 때 깊이와 같은 표본을 쓴다. 색은 temporal이 있으면 셰이더 휘도 가중이고, 없으면 데스크톱은 같은 셰이더·tiled GPU는 base pass attachment resolve다(RenderBase 결정 R17, 지원표에 표시). 계획 11.1절의 "검증된 resolve pass"가 이것이다.
- masked 머티리얼은 MSAA에서 alpha-to-coverage를 쓴다(GX3 요구, RQ5).

### 8.3 base pass 조명 평가

```text
ForwardOpaquePS
  surface = EvaluateMaterialSurface(...)          // 머티리얼(14절)
  color   = surface.emissive
  color  += SampleStaticIndirect(surface)          // 정적 축(12.1절), 없으면 0
  color  += SampleDynamicGiCache(surface)          // 동적 축 world-space 표본(12.2절), 없으면 0
  color  += EvaluateReflectionAndSky(surface)      // 반사 capture·sky light(RS7)
  for 각 directional: color += EvaluateLight(surface, light, CSM 표본)
  for 각 local in LightGridCell: color += EvaluateLight(surface, light, atlas 표본)
  출력 SceneColor (+ 보조 출력, 8.4절)
```

- `EvaluateLight` 이하는 7.3절과 같은 함수다. UE도 forward base pass가 forward 조명 공통 원문을 include해 같은 grid를 순회한다(`SH/BasePassPixelShader.usf:203–206, 1366, 1480`).
- UE forward는 그림자 광원을 4채널 동적 그림자 채널에 배정하고("This is used in forward only", `R/DynamicShadowMapChannelBindingHelper.h:15–21`) screen-space 그림자 mask 하나(`PF_B8G8R8A8`)에 투영한다(`R/LightGridInjection.cpp:2045–2080`). 채널을 받지 못한 광원의 동작은 확인하지 못했다(**미확인**). Rocks는 atlas 직접 표본이므로 채널 상한이 없다(9절).
- 퍼뮤테이션은 정적 조명 정책(없음·라이트맵·프로브)과 동적 GI 표본 유무로 제한하고, 광원 수·그림자 유무는 동적 분기다(14.3절).
- **반사 capture 알파(개정 8).** capture cube array의 알파는 그 capture가 덮는 비율이다. 겹친 capture를 앞에서부터 `덮음 *= 1 − capture 알파`로 쌓고 남은 비율에 sky light 반사를 더한다(UE `SH/ReflectionEnvironmentComposite.ush:158–163, 242`). 그래서 capture가 없을 때 묶는 검은 cube array는 알파 0이다(RenderBase 11.1절). 반사 합성 세부는 RS7에서 정한다.

### 8.4 Forward+ 보조 출력

| 출력 | 형식 | 쓰는 pass | 필요한 소비자 |
|---|---|---|---|
| 보조 법선·roughness | 법선 `RGB10A2Unorm`(GBufferA와 같은 인코딩). roughness 자리는 결정 RN12 | F4 depth prepass | AO·SSR·contact shadow·screen-space GI·denoiser |
| 간접광 합성용 색 | `RGBA8UnormSrgb`(diffuse color·specular 강도) | F9 base pass 추가 MRT | F12 screen-space 간접광 합성 |

- 소비자가 없는 view에서는 만들지 않는다(계획 14.2절 "full GBuffer를 의무화하지 않는다"). `RGB10A2Unorm`의 A는 2 bit라 roughness를 담을 수 없으므로 roughness 자리는 RS10에서 정한다(결정 RN12).
- 보조 출력이 있는 Forward+ view의 MRT 수는 2(SceneColor + 색)이고 prepass는 1–2개라 MRT 하한 4 안이다.

### 8.5 투명 처리

10절. Forward+와 Deferred가 같은 pass를 쓴다.
