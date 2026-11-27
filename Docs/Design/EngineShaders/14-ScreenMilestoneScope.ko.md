# 엔진 셰이더 — 14. SL1 나머지(RB 18.4절 SM13)

[목차](README.ko.md)

## 14. SL1 나머지(RB 18.4절 SM13, 개정 6)

화면 마일스톤(RB 18.1절)이 먼저 가져간 SL1 몫(전체 화면 VS·2D 복사·fallback 메시·tone map·텍스처 표시 — RB 11.4·18.5절)을 뺀 나머지다. RB 18.4절 SM13 행은 처음에 이것을 "후속"으로 두었으나(밉 생성은 런타임 생성 텍스처가 생길 때, LUT 좌표는 SL8, 자리표시는 MS MSR10과 함께) 렌더 세션 순서(2026-11-27)로 렌더러 RS0 전에 당긴다. 이유는 셋이다. (1) RS0 첫 pass들이 clear·깊이 복사·자리표시·compute 보조를 바로 쓴다. (2) CS 전용 API(`ComputeDispatch`·`WaveOperations`)는 SL3 scan·축소와 SL6 컬링의 바탕이라 먼저 형식 대조를 끝내 두어야 한다. (3) 모두 렌더러 기능이 없는 유틸리티라 고정 순서를 앞당기지 않는다(SL0과 같은 이유 — 6장).

### 14.1 범위

| 항목 | 지금 | 근거·남긴 이유 |
|---|---|---|
| A01 `Common/ComputeDispatch.rsh` | 구현(14.2절) | GX3·GX4 dispatch가 있다(RB 4.4절) |
| A01 `Common/WaveOperations.rsh` | 구현(14.2절) | 결정 SLD5 — 웨이브·groupshared 두 경로 대조 |
| A03 `Color/LutCoordinates.rsh` | 구현(14.3절) | 함수만이다. 첫 소비자는 SL8 LUT pass |
| A03 `Color/DisplayEncoding.rsh` | SDR sRGB만 구현(14.3절) | HDR10·scRGB는 후속(4.3절 그대로) |
| A03 `Color/ColorMaps.rsh` | 구현(14.3절) | 디버그 시각화(A20)가 쓴다 |
| A18 `Utility/TextureSamplingHelpers.rsh` | 구현(14.4절) | |
| A18 `Utility/PlaceholderShaders.rsf` | 구현(14.4절) | MS MSR10 대체는 "기본 머티리얼"이라 머티리얼 셰이더 쪽 일이다. 여기 셰이더는 엔진 pass의 단색·깊이 전용·눈에 띄는 무늬다 |
| A18 `Utility/CopyDepth.rsf` | 단일 표본 둘 구현(14.4절) | MSAA 깊이 복사는 RS8 `MsaaResolve.rsf`(RB 11.2절 깊이 resolve)가 맡는다 |
| A18 `Utility/ClearResources.rsf` + RB RR17 | 구현(14.4·14.5절) | 기본 텍스처 캐시는 RenderBase C++ |
| A18 `Utility/GenerateMips.rsf` | 구현(14.4·14.5절) | 한 dispatch 여러 mip은 후속(14.8절) |
| SL0 함수의 compute probe(6장 SL1 행) | 남김(14.9절) | CS 단계에서 같은 함수가 같은 결과인지 — 이번 묶음에 넣지 못했다 |
| A18 `CopyTexture.rsf` 나머지(배열 slice·cube 면·3D 단면·mip 복사·형식 변환·채널 고르기·CS) | 남김 | SM13 행 밖이다. 첫 소비자(스크린샷·썸네일·편집기 미리보기)가 생길 때. 같은 형식 mip 0 전체 복사는 장치 `CopyTexture`가 있다 |
| A18 `TextureReadback.rsf` | 남김 | SM13 행 밖이다. readback 불가 형식(블록 압축·GLES 비색 형식)을 읽는 소비자가 아직 없다(결정 SLD21 — 지금 self-test는 읽을 수 있는 형식만 쓴다) |
| A18 `FullscreenTriangle.rsf` 깊이 값·사각형 변형 | 남김 | 부분 clear는 viewport·scissor와 clear 전용 VS(14.4절 4항)로 한다. 다른 소비자가 생기면 더한다 |
| A06 `DepthConversion.rsh` 일부 | 남김(SL2) | view 상수(`Shared/ViewLayout.h`, SL2)가 있어야 의미가 있다 |

### 14.2 A01 CS 전용 API

**공통.** 두 파일은 `ROCKS_COMPUTE_SHADER`가 아니면 `#error`다(4.1절). 라이브러리 규칙(7.1절 — HLSL 2018, float만, 단일 출구, 이름 접두 없음)을 따른다. 결과 정의는 웨이브 크기·형식과 무관하다.

1. **`ComputeDispatch.rsh`(UE `ComputeShaderUtils.ush`의 그룹 번호 보조 역할).**
   - `MaxDispatchGroupsPerAxis = 65535` — D3D12·Vulkan·GLES가 보장하는 축당 그룹 수 하한(D3D12 `D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION`, Vulkan `maxComputeWorkGroupCount` 최소값, GLES 3.1 `MAX_COMPUTE_WORK_GROUP_COUNT` 최소값)이다.
   - `DivideAndRoundUp(value, divisor)`(uint·uint2·uint3), `LinearThreadIndex(groupThreadId, groupSize)`(= x + y·sx + z·sx·sy — `SV_GroupIndex`와 같은 값을 2D·3D 그룹 크기 상수로 셈), `UnflattenGroupId2D(linear, groupsPerRow)`(→ (l mod n, l / n)), `TileOrigin2D(linearTile, tilesPerRow, tileSize)`, `GroupThreadMortonCoordinate(groupIndex)`(A02 `MortonDecode2D` — 2의 거듭제곱 정사각 그룹 안 Z 순서 좌표).
   - **큰 1D dispatch 풀기.** 그룹 수 g가 65,535를 넘으면 C++가 (min(g, 65535), ⌈g / 65535⌉, 1)로 낸다(RenderBase `WrapGroupCount`, 14.5절 1항). 셰이더는 `UnwrapLinearGroupId(groupId)` = x + y·65535, `UnwrapDispatchThreadId(groupId, groupIndex, groupThreads)` = 그 값 × groupThreads + groupIndex로 되돌리고, 마지막 행의 남는 그룹은 호출자가 원소 수로 거른다. 두 축만 쓰므로 그룹 수 상한은 65,535²(약 4.29 × 10⁹, uint32 안)이다. UE도 x 축 상한을 넘으면 y 축으로 감싼다(`GetUnWrappedDispatchGroupId`) — 감싸는 폭을 상수 하나로 고정한 것이 Rocks 선택이다(셰이더에 그룹 수 상수를 넘기지 않아도 된다).
   - **간접 인자.** `DispatchArgumentsForThreads(threads, groupSize)` = 위 감싸기를 적용한 (x, y, 1)(스레드 0이면 (0, 1, 1) — 빈 dispatch), `WriteDispatchArguments(buffer, byteOffset, groups)` = `RWByteAddressBuffer.Store3`(UE `WriteDispatchIndirectArgs`). 인자 자리 규칙은 C++ `ValidateDispatchArguments`(RB 4.4절)다.
2. **`WaveOperations.rsh`(결정 SLD5, UE `WaveOpUtil.ush`·`WaveBroadcastIntrinsics.ush`의 그룹 단위 판).**
   - **선언.** include 전에 `GROUP_OPERATION_THREADS`(그룹 스레드 수 1–1024, 엔트리의 numthreads 곱)를 정의해야 한다(없으면 `#error`). 파일이 `groupshared uint GroupOperationScratch[2 × N]`(1024 스레드에서 8 KiB — GLES 3.1 보장 16 KiB 안)를 선언한다. CS 전용 API라 라이브러리 "자원 선언 없음" 규칙(3.5절)의 예외다(groupshared는 전역에만 선언할 수 있다).
   - **함수.** `GroupSumUint`·`GroupSumFloat`·`GroupMinUint`·`GroupMaxUint`·`GroupMinFloat`·`GroupMaxFloat`·`GroupPrefixSumUint`(배타 — 자기 앞 스레드 합)·`GroupBroadcastFirstUint`·`GroupBroadcastFirstFloat`(`SV_GroupIndex` 0의 값). 인자는 (값, `SV_GroupIndex`)이고 그룹의 **모든 스레드가 균일한 흐름에서** 부른다(안에 barrier가 있다). 함수 끝에도 barrier를 두어 이어 부르는 호출이 scratch를 덮어도 된다.
   - **float 최소·최대.** 값을 순서 보존 uint(부호 bit가 있으면 전체 반전, 없으면 부호 bit 켜기)로 바꿔 정수로 비교한다. 그래서 두 경로가 bit 일치하고 −0 < +0이다. NaN 입력은 정의하지 않는다.
   - **groupshared 경로**(형식 기능 `waveOperations`가 0인 형식 전부와, 시험용 `GROUP_OPERATION_FORCE_GROUPSHARED` = 1): 합·최소·최대는 N보다 작은 가장 큰 2의 거듭제곱 폭에서 반씩 접는 트리(N이 2의 거듭제곱이 아니어도 된다), 배타 prefix는 Hillis–Steele 포함 scan(scratch 두 벌을 번갈아 — log₂N 단계)에서 자기 값을 뺀다(uint 감산은 mod 2³²라 포함 합과 같은 정보).
   - **웨이브 경로**(`D3D12_SM6`): `WaveActiveSum`·`WaveActiveMin`·`WaveActiveMax`·`WavePrefixSum`으로 웨이브 안을 줄이고, 웨이브 첫 lane이 웨이브 결과를 `scratch[웨이브 번호]`에 쓴 뒤 모든 스레드가 웨이브 결과를 번호 순서로 합친다(웨이브 수는 ⌈N / `WaveGetLaneCount()`⌉ — 웨이브 크기를 가정하지 않는다). prefix는 자기 웨이브 앞 웨이브 합 + 웨이브 안 prefix다.
   - **웨이브 배치 가정.** 웨이브 번호 = `SV_GroupIndex` / `WaveGetLaneCount()`, lane 번호 = `SV_GroupIndex` mod 웨이브 크기로 본다(웨이브가 연속한 `SV_GroupIndex` 구간). HLSL 명세는 compute의 스레드 → 웨이브 배치를 정하지 않지만 D3D12 구현은 모두 이 배치이고 UE도 같은 가정을 쓴다(`WaveOpUtil.ush`의 그룹 단위 보조와 Nanite·Lumen 원문의 `GroupIndex / WaveGetLaneCount()`). Rocks는 가정을 숨기지 않고 장치마다 확인한다: 웨이브 경로가 있는 장치에서 probe가 스레드마다 (lane 번호, 웨이브 크기, 웨이브 첫 lane의 `SV_GroupIndex`)를 써서 CPU가 배치를 확인한다(14.6절 2항 — 어긋나면 시험 실패).
   - **결과 동일성.** 정수 함수(합·최소·최대·prefix·broadcast, float 최소·최대 포함)는 두 경로 bit 일치, float 합은 더하는 순서가 달라 허용 오차 안이다(SLD5). 순서와 무관한 결정성이 필요한 쪽(정렬 키·morph 누적)은 정수를 쓴다(SLD17).

### 14.3 A03 1차 나머지

1. **`LutCoordinates.rsh`(UE `TonemapCommon.ush`·`PostProcessCombineLUTs.usf`의 LUT 좌표 역할).**
   - `DefaultLutSize = 32`(UE `r.LUT.Size` 기본값). 크기는 인자로 받는다(UE도 퍼뮤테이션·상수로 32 밖 크기를 받는다).
   - `LutTexelCenterCoordinate(unit, size)` = unit · (size − 1)/size + 0.5/size — [0, 1] 값을 첫·끝 texel 중심에 맞춘다(반 texel 보정). `LinearColorToLutCoordinate(linear, size)` = log shaper(SL0 `LinearToLutLog`) + 반 texel 보정. `LutTexelToNeutralColor(texel, size)` = texel/(size − 1)(LUT 생성 pass가 texel마다 계산할 입력), `LutTexelToLinearColor(texel, size)` = 그 값의 log shaper 역(`LutLogToLinear`).
   - **2D 펼친 LUT**(LUT 생성 렌더 타깃 — 볼륨 렌더 타깃이 없는 경로, UE `PostProcessCombineLUTs`의 2D 판과 같은 배치): 폭 size², 높이 size, x = 파랑 slice × size + 빨강, y = 초록. `UnwrappedLutPixelToTexel(pixel, size)`와 `SampleUnwrappedLut2D(lut, sampler, unit, size)`(파랑 축의 이웃 두 slice를 bilinear로 각각 표본해 선형 보간 — 3D 표본과 같은 삼선형 결과).
   - `SampleLut3D(lut, sampler, unit, size)` = 반 texel 보정 좌표의 `SampleLevel`(mip 0).
2. **`DisplayEncoding.rsh`.** include 전에 `DISPLAY_OUTPUT_DEVICE`(출력 장치 번호 — pass 퍼뮤테이션)를 정의한다. 지금은 `DisplayOutputSdrSrgb`(0)만 받고 다른 값은 `#error`다(HDR10·scRGB는 후속 — 4.3절). `EncodeSdrSrgbDisplay(linear)` = sRGB 구간 함수(saturate 뒤 — SDR 범위 밖은 자른다), `EncodeDisplayColor(linear)` = 장치 번호로 고른 부호화. UE도 SDR sRGB 출력은 정확한 구간 함수다(`ACESDisplayEncoding.ush`·`GammaCorrectionCommon.ush`의 sRGB 경로). 스왑체인 view가 sRGB인 경로(RB 18.5절 1항)는 이 함수를 쓰지 않는다. UNORM 대상에 쓰는 경로(GLES 표면이 sRGB view를 주지 않을 때, 스크린샷 부호화)가 쓴다.
3. **`ColorMaps.rsh`.** 반환 값 공간을 이름에 적는다: `TurboColorMapSrgb(t)`·`ViridisColorMapSrgb(t)`는 색 지도 정의 그대로의 sRGB 부호 값(t는 saturate), `TurboColorMap(t)`·`ViridisColorMap(t)`는 그 선형 값(sRGB 대상에 쓰는 셰이더용). Turbo는 Mikhailov(Google, 2019) 발표의 다항 근사(5차, 성분별), viridis는 matplotlib viridis 표에 맞춘 Zucker의 6차 다항(CC0)이다. 표 자체(256 항목)는 넣지 않는다 — 디버그 색에 다항 근사 오차(측정값 14.9절)로 충분하다. `DistinctIndexColor(index)` = `HashPcg(index)` 상위 24 bit의 색상(hue) + 고정 채도 0.75·명도 0.9의 HSV(sRGB 부호 공간) → 선형이다. 이웃 번호가 비슷한 색이 되지 않고(해시) 결정적이다(정수 해시 + 같은 HSV 식).

### 14.4 A18 유틸리티

1. **`TextureSamplingHelpers.rsh`.**
   - `SnapUvToTexelCenter(uv, textureSize)` = (floor(uv · size) + 0.5)/size(UE `SnapToClosestPixelCenterUV`).
   - `SampleCatmullRom5Tap(texture, linearSampler, uv, textureSize)`: Catmull–Rom 4×4 커널에서 네 모서리 texel을 빼고 가중을 다시 정규화한 필터를 bilinear 표본 다섯 번으로 낸다(가운데 2×2 한 번, 네 변의 2 texel 쌍 한 번씩 — 축마다 가운데 두 가중을 합쳐 bilinear 위치를 정한다). Jimenez, *Filmic SMAA*(SIGGRAPH 2016 Advances)의 방법이고 UE `TextureSampling.ush`의 `GetBicubic2DCatmullRomSamples`(TAA 이력 표본)와 같은 역할이다. 샘플러는 bilinear·clamp여야 한다.
   - `LoadBilinearUint(texture, uv, textureSize)`·`LoadBilinearInt`: 정수 형식(필터 불가)의 수동 bilinear — 이웃 네 texel을 `Load`(가장자리 clamp)해 가중 합을 float4로 낸다.
2. **`PlaceholderShaders.rsf`.** 엔트리 셋(모두 `FullscreenTriangleVertex` 등 아무 VS와 짝): `SolidColorPixel`(그리기별 상수 `SolidColorConstants{color}` 그대로 — UE `OneColorShader.usf`·`SolidColorPixelShader.ush`), `DepthOnlyPixel`(출력 없음 — 깊이 전용 pass·깊이만 지우는 부분 clear, UE `NullPixelShader.usf`는 0을 내지만 Rocks는 색 타깃 없는 파이프라인에 쓰므로 출력이 없다), `MissingShaderPixel`(8 px 칸 자홍·검정 바둑판, 알파 1 — 셰이더가 없거나 아직 준비되지 않은 그리기를 화면에서 찾게 한다. UE `MissingShaderPixelShader.usf`는 디버그 view용 미정 값이라 Rocks는 눈에 띄는 무늬로 바꾼다). MS MSR10의 머티리얼 대체는 기본 머티리얼로 하고(머티리얼 시스템 11.3절), 이 셰이더는 엔진 pass의 자리표시다.
3. **`CopyDepth.rsf`.** `CopyDepthToColorPixel`(깊이 SRV를 `Load`해 (d, 0, 0, 1) — `R32Float` 등 색 대상, 시각화·readback), `CopyDepthPixel`(같은 값을 `SV_Depth`로 — 깊이 대상, 깊이 비교 `Always`·쓰기 켬). 텍스처 이름은 `SourceDepth`다. UE `CopyDepthTexture.usf`와 같은 일이고 MSAA 판은 RS8이다(14.1절).
4. **`ClearResources.rsf`(RB RR17, UE `ClearReplacementShaders.usf`·`ClearQuad`).**
   - `ClearWritableCompute`: 쓰기 view의 [minBounds, maxBounds) 범위를 값 하나로 채운다. 퍼뮤테이션은 차원(`CLEAR_DIMENSION`: 버퍼·2D·2D 배열·3D) × image 형식(`CLEAR_FORMAT`: rgba8·rgba8snorm·rgba16f·rgba32f·r32f·r32ui·r32i·rgba32ui·rg16f·r16f·rg32f·r11g11b10f)이다. 형식 축이 있는 이유는 GLES·SPIR-V storage image 선언이 형식을 요구하기 때문이다(SS 7.2절 1항, `ROCKS_IMAGE_FORMAT`). GLES 형식에서는 GLES 3.1 core image 형식(앞 여덟)만 컴파일한다(거르기 함수). 그룹은 버퍼 64 × 1 × 1(큰 버퍼는 `UnwrapDispatchThreadId`), 2D 8 × 8, 배열·3D 4 × 4 × 4다. 그리기별 상수 `ClearWritableConstants{value(uint4 bit), minBounds, maxBounds}`.
   - `ClearRegionVertex`·`ClearRegionPixel`: 활성 raster pass 안 사각형 부분 clear다. C++가 viewport·scissor를 사각형으로 두고 전체 화면 삼각형을 그린다(UE `DrawClearQuad`는 사각형 정점 범위를 상수로 받는다 — 결과는 같고 Rocks는 정점 계산이 없다). VS는 깊이 값(상수)을 z로 내고, PS는 값 계열 퍼뮤테이션(`CLEAR_VALUE_TYPE`: float·uint·sint)으로 색 타깃 0에 값을 쓴다. 깊이만 지우면 PS는 `DepthOnlyPixel`, 스텐실은 파이프라인 스텐실 `Always`·`Replace`와 스텐실 기준 값이다. 그리기별 상수 `ClearRegionConstants{value(uint4 bit), depth(float4 — x)}`. 색 타깃 여럿(MRT)은 후속(14.8절).
   - `InitializeDispatchArgumentsCompute`: 간접 인자 버퍼의 [byteOffset, byteOffset + 12 × count)에 같은 `DispatchIndirectArguments` 기록 count개를 쓴다(그리기별 상수 `DispatchArgumentsInitConstants{arguments, range}`). 32-bit word 하나로 채우는 것은 장치 `ClearWritableBufferView`로 충분하다.
   - 전체 clear는 지금처럼 attachment load 동작이다(RB 11.2절).
5. **`GenerateMips.rsf`(UE `ComputeGenerateMips.usf`·`FGenerateMips`).**
   - **두 경로(UE와 같다).** compute 경로 `GenerateMipsCompute`(형식에 `StorageWrite` 능력이 있고 텍스처가 `Storage` 용도일 때 — 퍼뮤테이션 차원 2D·2D 배열·3D × image 형식 float 계열 아홉)와 raster 경로 `GenerateMipsPixel`(`RenderTarget` 능력·용도 — 퍼뮤테이션 원본 차원 2D·2D 배열·cube·3D, `FullscreenTriangleVertex`와 짝). 고르는 규칙은 C++ `SelectMipGenerationPath`(14.5절 2항)다. cube는 storage 텍스처가 될 수 없어(Graphics `ValidateTextureDesc`) raster만이다. sRGB 형식은 storage 능력이 없으므로 raster로 간다: 원본을 sRGB view로 읽어 하드웨어가 선형으로 풀고, 렌더 타깃 쓰기가 다시 sRGB로 부호화한다. 그래서 **필터는 선형 공간**이다(UE compute 경로도 sRGB 원본을 선형으로 읽어 필터한 뒤 손으로 부호화한다).
   - **필터.** 이전 mip에서 다음 mip을 만든다(UE와 같다 — mip 0에서 매번 만들지 않는다). 축마다 정확한 면적 박스 필터다: 원본 크기 s가 짝수면 (2x, 2x+1)을 ½씩, 홀수(s = 2d + 1)면 (2x, 2x+1, 2x+2)를 ((d − x)/s, d/s, (x + 1)/s)로 — 대상 texel이 덮는 원본 구간의 겹친 길이 비율이다. s = 1이면 그 texel 하나다. 3D는 세 축의 곱 가중이다. UE는 대상 texel 중심 UV에서 bilinear 한 번을 표본해 짝수 크기에서는 박스와 같지만 홀수 크기에서는 박스가 아니다(경계 texel이 빠지거나 두 번 들어간다). Rocks는 홀수 크기도 박스로 낸다(UE 개선 — 결정 SLD23). 알파는 다른 채널과 같이 평균한다(premultiply 하지 않는다 — UE와 같다).
   - **원본 읽기.** 원본 mip 하나의 Read view를 `Load`로 읽는다(`TextureViewBaseMip` 기준 mip을 상수로 받는다 — 범위 view가 없는 GLES 대응, RB 7.5절). cube는 HLSL `TextureCube`에 `Load`가 없어 면 f·texel 중심 방향을 point 샘플러로 `SampleLevel`한다(방향이 texel 중심이라 point 표본이 그 texel 하나를 정확히 고른다). 그리기별 상수 `GenerateMipsConstants{source(폭·높이·깊이·기준 mip), destination(폭·높이·깊이, slice)}`.

### 14.5 RenderBase C++(RB 11.1·11.2절 RR17)

1. **`RenderBase/ComputeDispatch.h` 추가.** `MaxWrappedGroupsPerAxis = 65535`, `WrapGroupCount(groups)` → (min(g, 65535), ⌈g / 65535⌉, 1)(g = 0이면 빈 그룹 수). 셰이더 `UnwrapLinearGroupId`의 짝이다.
2. **`RenderBase/GenerateMips.h`.** `MipGenerationPath{Compute, Raster}`, `SelectMipGenerationPath(device, desc)`(compute 조건: `Storage` 용도·형식 `StorageWrite`·image 형식 대응이 있고 cube 계열이 아님. 아니면 raster 조건: `RenderTarget` 용도·형식 `RenderTarget`. 둘 다 아니면 `Unsupported`, mip이 하나면 `InvalidArgument`), `GenerateMips(device, set, cache, texture, desc)`: mip 1부터 끝까지 그 경로로 만든다. compute는 기록 하나(attachment 없는 pass)에 mip마다 dispatch(Read view 원본 mip·쓰기 view 대상 mip, 전이는 장치 암묵 전이), raster는 (mip, slice)마다 pass 하나(attachment = 대상 mip·slice/3D 깊이 면)다. 렌더 프레임을 기록하지 않을 때 부른다(장치 한 기록 규칙 — `Busy`). 만든 view는 함수 안에서 Retire한다. 그래프 pass 판(UE `FGenerateMips::Execute(GraphBuilder, …)`)은 첫 소비자(렌더러 RS0의 런타임 생성 텍스처 — scene capture 등)와 함께 RenderGraph에 둔다(14.8절).
3. **`RenderBase/ClearResources.h`.**
   - `ClearWritableTexture(device, commands, set, cache, view, value, region)`: view 정보(`GetTextureViewInfo` — 형식·차원·크기)로 퍼뮤테이션을 골라 dispatch한다. 범위는 늘 준다(빈 범위 `InvalidArgument` — 14.9절 정정 1). 형식이 image 형식 표에 없거나 view가 쓰기 view가 아니면 `Unsupported`·`InvalidArgument`다. `ClearWritableBuffer(…, bufferView, value, firstElement, count)`는 typed 버퍼 view용이다(R32 정수·raw는 장치 `ClearWritableBufferView`가 더 싸다 — 그쪽을 권한다).
   - `ClearRegion(commands, set, cache, desc)`: 활성 raster pass 안에서 사각형(viewport·scissor) 부분 clear. desc는 사각형, 색 대상 형식·값(없으면 색을 쓰지 않는다), 깊이 형식·값, 스텐실 값이다. 파이프라인은 pass attachment 형식과 같아야 한다(호출자가 넘긴다). 그린 뒤 viewport·scissor를 대상 전체로 돌린다.
   - `InitializeDispatchArguments(commands, set, cache, bufferView, byteOffset, count, arguments)`.
   - 값 `ClearValue`: float4 또는 uint4(정수 형식은 정수로 읽는다 — 형식 값 계열을 따른다).
4. **`RenderBase/DefaultTextureCache.h`(UE `FSystemTextures::GetDefaultTexture`, RB 11.1절 "(값, 형식, 차원) 키 캐시").** `Find(device, dimension, format, value)` → 1×1(3D 1×1×1, cube 면 6, 배열 층 1) `Sampled` 텍스처. 같은 키는 같은 handle이다. 값은 형식으로 부호화한다(unorm 반올림·sRGB는 Core `LinearColor::ToSRGB`·half는 Core `Float16`·정수는 값 그대로). 받는 형식은 부호화 표가 있는 것뿐이고(8·16·32 bit 색과 정수, 14.9절 표) 나머지·깊이·블록 압축은 `Unsupported`, cube 배열 능력이 없으면 `Unsupported`, 항목 상한(`MaxDefaultTextures` 64)을 넘으면 `CapacityExceeded`다. 소유 스레드 전용(장치 생성 규칙)이고 `Retire(device)`로 모두 놓는다(렌더 lane 종료 단계). 내장 자원 팩(RB 11.4절 5항)은 고정 목록이고 이 캐시는 pass가 요구하는 임의 (값, 형식, 차원)의 대체다(9.6절 typed variant와 같은 역할).

### 14.6 시험

1. **(C) 컴파일 행렬.** `EngineShaders_CompileMatrix`가 엔진 셰이더 타입의 **모든 퍼뮤테이션**(거르기 함수를 통과한 것 — 형식 사실 기준)을 형식 정의 전부에 컴파일한다(개정 6 전에는 퍼뮤테이션 0만 — 엔진 타입에 퍼뮤테이션이 없었다). 시험 엔트리에 include 폐포 CS(14.2·14.3·14.4절 새 `.rsh` 포함)·compute probe·유틸리티 probe를 더한다.
2. **(N)·(G) GPU probe(Sandbox `--engine-shader-self-test`, 여덟 장치).**
   - A03 함수(PS probe, 기존 `ColorProbes.rsf`에 함수 번호 추가): LUT 좌표 식(반 texel·log shaper·2D 펼침 texel) = CPU double, SDR 부호화 = CPU(8 bit 256 부호 + 무작위), 색 지도 = CPU 같은 다항(그리고 표 끝점·중간점과의 근사 오차 기록), 구분 색 = CPU 같은 해시·HSV.
   - compute probe(`Common/ComputeProbes.rsf`): `ComputeDispatch` 함수 bit 일치(그룹 크기 8 × 8·64 × 1, 감싼 dispatch 65,540그룹 — 모든 선형 그룹 번호가 정확히 한 번), 간접 인자 쓰기 → `DispatchIndirect`로 실행한 결과, `WaveOperations` 열 가지 함수를 그룹 크기 64·256·100(2의 거듭제곱 아님)에서 무작위 입력(SLD20 생성기)으로 — CPU 기준과 정수 bit 일치·float 합 허용 오차, 웨이브 경로가 있는 장치(D3D12 하드웨어·WARP)는 강제 groupshared 퍼뮤테이션도 돌려 두 경로 대조와 웨이브 배치 확인.
   - SL0 함수의 compute probe: 네 probe 원문의 PS 엔트리를 사례 평가 함수 + PS·CS 엔트리로 나누고, 같은 probe 목록을 CS(쓰기 텍스처 `RGBA32Float`·`R32Uint`)로 한 번 더 돌려 같은 CPU 기준·같은 판정으로 대조한다.
   - 유틸리티 probe(`Utility/UtilityProbes.rsf`): Catmull–Rom 5 tap(`RGBA16Float` 원본, bilinear 가중 정밀도 때문에 절대 1e-2)·정수 bilinear(정확)·texel 중심·2D 펼친 LUT 표본(항등 LUT `RGBA16Float`, 절대 2e-3) = CPU.
3. **(N) 유틸리티 pass(Sandbox `--self-test`의 `RunRenderUtilityChecks`, 여덟 장치).** 밉 생성: 형식(`RGBA8Unorm`·`RGBA8UnormSrgb`·`RGBA16Float`·`RGBA32Float`·`R32Float`) × 차원(2D 홀수 크기 13 × 7·2D 배열·cube·3D) × 경로(장치가 받는 것 모두)에서 mip k + 1 = CPU 박스(그 장치가 쓴 mip k에서, 8 bit은 ±1 부호·float은 상대 1e-3). UAV clear: 형식·차원별 값과 범위 밖 보존. 부분 clear: 사각형 안 = 값·밖 = 원래 값(색 float·uint, 깊이, 스텐실). 기본 텍스처 캐시: 형식·차원별 readback 값, 같은 키 같은 handle. 깊이 복사: 그린 깊이 → `R32Float`·깊이 → 깊이 = 원본 그대로. 자리표시: 단색 = 상수, 바둑판 무늬 = CPU 식, 깊이 전용 pass의 깊이 = 그린 깊이.
4. **EngineTests(기록 장치).** `WrapGroupCount` 경계(0·1·65,535·65,536·uint32 끝), 밉 경로 선택표(용도·능력·cube·mip 1·형식)와 기록 열(compute: dispatch 수 = mip − 1·view 생성·Retire, raster: pass 수 = (mip − 1) × slice), clear 퍼뮤테이션 선택·오류(`Unsupported` 형식·Read view·범위 밖 region), 기본 텍스처 캐시(초기 byte·키 적중·`Unsupported`·cube 배열 능력·상한·Retire).

### 14.7 결정과 버린 대안

| ID | 결정 | 근거 | 버린 대안 |
|---|---|---|---|
| SLD22 | SM13을 RS0 전에 한다. SM13 행 밖의 SL1 항목(`CopyTexture` 나머지·`TextureReadback`·전체 화면 VS 변형)은 소비자가 생길 때 | 14장 머리. 소비자 없는 변환 셰이더는 시험 기준(무엇과 같아야 하는가)이 없다 | SL1 전체를 한 번에(시험 기준 없는 코드가 생긴다) |
| SLD23 | 밉 생성은 UE처럼 compute·raster 두 경로를 능력으로 고르고, 필터는 이전 mip에서 정확한 면적 박스(홀수 크기 포함), 한 dispatch에 mip 하나 | UE `FGenerateMips`의 경로 선택. 홀수 크기 박스는 UE bilinear 한 번보다 정확하고 CPU 기준이 분명하다. 한 dispatch 여러 mip은 SL3 축소(A21)와 측정이 필요하다 | bilinear 한 번(UE — 홀수 크기에서 박스가 아님), mip 0에서 매번(비용 큼, UE와 다름), compute 하나만(sRGB·cube가 안 된다) |
| SLD24 | 부분 clear는 viewport·scissor + 전체 화면 삼각형 clear 그리기, UAV clear는 (차원 × image 형식) 퍼뮤테이션 CS | SPIR-V·GLES storage image는 형식이 필요하다. viewport·scissor면 사각형 정점 계산이 없다 | 형식 없는(`unknown`) storage image(Vulkan `shaderStorageImageWriteWithoutFormat`·GLES에 없음), UE처럼 정점 범위 상수(같은 결과에 상수만 늘어난다) |
| SLD25 | 그룹 연산은 그룹 단위 결과, 웨이브 경로는 웨이브 = 연속한 `SV_GroupIndex` 구간 가정을 장치마다 probe로 확인, float 최소·최대는 순서 보존 uint | SLD5. 가정은 D3D12 구현·UE 원문과 같고 숨기지 않는다. 순서 보존 uint면 두 경로가 bit 일치한다 | 웨이브 배치를 가정하지 않는 원자 연산 합성(정수 합·최소·최대는 되지만 float 합·prefix가 결정적이지 않다), `WaveActiveMin`(float — −0·+0 결과가 경로마다 다를 수 있다) |
| SLD26 | 큰 1D dispatch는 x 축 65,535 고정 폭으로 감싼다 | 셰이더에 그룹 수를 넘기지 않아도 되고 모든 형식 하한이다 | 장치 한도 폭(셰이더 상수가 늘고 형식마다 결과 배치가 달라진다) |
| SLD27 | 새 엔진 셰이더의 그리기별 상수는 생성 파라미터 구조체(`ROCKS_SHADER_PARAMETERS(…, RootConstants)`, SS 16.2절), 자원은 원문 선언 + 이름 상수(`EngineShaderTypes.h`)다 | 이 도우미들은 그래프 밖 기록이고 쓰기 텍스처를 묶는다. 쓰기 자원 멤버는 렌더 그래프 pass 문맥만 묶는다(RB 9.13절 `ParameterBatch`는 그래프 멤버·쓰기 자원을 건너뛴다). SM6·SM7 엔진 셰이더와 같은 방식이다 | 자원까지 생성 구조체(그래프 pass 판이 생길 때 — 14.8절) |
| SLD28 | 기본 텍스처 캐시는 RenderBase 클래스, 값 부호화는 표에 있는 형식만 | UE `FSystemTextures` 역할. 부호화가 틀린 대체는 대체가 아니다 | 모든 형식 부호화(블록 압축·깊이는 1×1 대체의 의미가 없다) |

### 14.8 한계·후속

- 한 dispatch 여러 mip(AMD FidelityFX SPD 방식 — groupshared, A21 축소 재사용)은 SL3 축소 뒤 측정하고 정한다. 지금은 mip마다 dispatch·pass 하나다.
- 밉 생성·clear의 렌더 그래프 pass 판(subresource 접근 선언·자원 생성 구조체)은 RS0 첫 소비자와 함께 RenderGraph에 둔다. 지금 도우미는 그래프 밖 기록이다.
- 부분 clear MRT(색 타깃 여럿에 서로 다른 값)는 후속이다(UE `DrawClearQuadMRT`). 지금은 색 타깃 0만 지운다.
- 웨이브 배치 가정이 깨진 장치가 나오면 그 장치의 웨이브 경로를 끄는 장치 능력 표시를 더한다(지금 웨이브 경로 형식은 `D3D12_SM6` 하나).
- SL0 함수의 compute probe(SL0 probe 원문을 평가 함수 + PS·CS 엔트리로 나누기)는 남았다(14.9절).
- Turbo 다항 근사는 표 끝에서 0.13 가까이 어긋난다(14.9절 측정 0.1255). 정확한 색이 필요한 시각화(색각 접근성 등)가 생기면 256 항목 표 텍스처로 바꾼다.
- `DisplayEncoding`의 HDR 분기·dither는 후속(SL8 grain·EIP 26.4절).

### 14.9 구현 메모(개정 7, 2026-11-27)

14.1–14.8절 결정대로 구현했다. 바탕은 origin/feature/engine-runtime `92d173424`다.

- **파일.** 셰이더: `Common/ComputeDispatch.rsh`·`Common/WaveOperations.rsh`, `Color/LutCoordinates.rsh`·`Color/DisplayEncoding.rsh`·`Color/ColorMaps.rsh`, `Utility/TextureSamplingHelpers.rsh`·`Utility/PlaceholderShaders.rsf`·`Utility/CopyDepth.rsf`·`Utility/ClearResources.rsf`·`Utility/GenerateMips.rsf`. RenderBase: `UtilityShaderTypes.h`(타입 이름·자원 이름·생성 상수 구조체 다섯·`StorageImageFormat`·퍼뮤테이션 ID 도우미, 등록은 `EngineShaderTypes()`), `GenerateMips.h`, `ClearResources.h`, `DefaultTextureCache.h`, `ComputeDispatch.h`의 `WrapGroupCount`·`MaxWrappedGroupsPerAxis`. 엔진 셰이더 타입은 11개(퍼뮤테이션 포함 D3D12 형식 조합 89개, GLES 형식 61개)가 늘었다.
- **스펙과 다르게 정한 것.**
  1. `ClearWritableTexture`의 범위는 늘 준다(빈 범위는 `InvalidArgument`). `TextureViewInfo`에 크기가 없어 "view 전체" 기본값을 만들 수 없다(14.5절 3항 정정).
  2. 밉 생성은 cube 배열을 받지 않는다(`Unsupported` — raster PS 퍼뮤테이션이 2D·2D 배열·cube·3D뿐이다). 첫 소비자(반사 capture 배열)가 생기면 퍼뮤테이션을 더한다.
  3. Turbo 다항은 5차다(14.3절 3항의 "6차"를 고쳤다 — viridis가 6차).
  4. 그룹 연산 probe의 −0 입력은 그리기별 상수 bit로 준다. FXC(`D3D11_SM5`)가 셰이더 리터럴 `asfloat(0x80000000)`을 +0으로 접어 float 최소의 −0 판정이 장치마다 달라졌다(라이브러리 결함이 아니라 시험 입력 문제 — 상수로 준 −0은 D3D11에서도 bit 일치).
  5. HLSL 키워드 `linear`는 변수 이름으로 쓸 수 없다(7.1절 규칙 그대로 — probe에서 한 번 걸렸다).
  6. D3D12·D3D11 장치는 렌더 타깃·storage 텍스처의 `UpdateTexture`를 `InvalidArgument`로 거부한다(Vulkan·GLES는 받는다 — backend 계약 차이, Graphics 쪽 기록 대상). 밉 생성 self-test는 mip 0을 시험 PS(`MipPatternPixel`)로 그려 채운다.
- **compile 행렬.** `EngineShaders_CompileMatrix`가 쿠킹과 같은 열거(`EnumerateShaderLibraryCombinations`)로 엔진 타입의 퍼뮤테이션 전부를 컴파일한다. 시험 엔트리는 compute probe 셋, 그룹 연산 probe 여섯(그룹 64·256·100 × 강제 groupshared), 유틸리티 probe 둘이 늘었다. `D3D12_SM6`·`D3D11_SM5`·`VULKAN_SM5` 123, `GLES31_PC`·`GLES31_ANDROID` 95 job 통과, `METAL_SM5`는 도구 없음 SKIP, `VULKAN_ANDROID`·`METAL_IOS`는 자리표시 형식 SKIP.
- **GPU 시험(2026-11-27 로컬, `Sandbox --engine-shader-self-test`).** 여덟 장치 모두 PASS, NOT RUN 없음. 엔진 셰이더 probe는 D3D12·WARP 280 검사(웨이브 경로 대조 셋 포함), 나머지 274 검사다(SL0 247 + SL1). 장치 사이 대조: 정수 probe bit 일치, 실수 최대 차이 0.0067(Catmull–Rom — bilinear 가중 정밀도).

| 장치 | 엔진 셰이더 probe | 웨이브 | 밉 생성(compute·raster) | UAV clear(텍스처 실행) | 유틸리티 pass |
|---|---|---|---|---|---|
| D3D12(GTX 1060) | 280 PASS | 32 lane, 두 경로 정수 bit 일치 | 32(12·20) | 33 | PASS |
| D3D12 WARP | 280 PASS | 4 lane, 두 경로 정수 bit 일치 | 32(12·20) | 33 | PASS |
| D3D11(GTX 1060) | 274 PASS | 없음(groupshared) | 32(12·20) | 33 | PASS |
| D3D11 WARP | 274 PASS | 없음 | 32(12·20) | 33 | PASS |
| OpenGL ES(ES context) | 274 PASS | 없음 | 32(12·20) | 21(core image 형식만) | PASS |
| GL core 위 ES | 274 PASS | 없음 | 32(12·20) | 21 | PASS |
| Vulkan(GTX 1060) | 274 PASS | 없음 | 32(12·20) | 33 | PASS |
| lavapipe | 274 PASS | 없음 | 32(12·20) | 33 | PASS |

- **측정값.** 웨이브 배치 가정(연속 `SV_GroupIndex` 구간)은 D3D12 하드웨어(32 lane)·WARP(4 lane) 모두 성립했다. float 합은 순서 무관 상한 (N − 1)·2⁻²⁴·Σ|f|의 최대 0.039배, 두 경로 차이 최대 4.9e-4(256 스레드, 값 범위 ±16). Catmull–Rom 5 tap은 CPU 정확 식과 최대 0.0052(허용 0.01 — 하드웨어 bilinear 가중 8 bit, lavapipe는 6e-7), 2D 펼친 LUT·3D LUT 표본은 항등 LUT에서 최대 6.4e-4(허용 2e-3 — half 원본). 색 지도 다항과 공개 표의 차이는 viridis 0.0126, Turbo 0.1255(끝점 t = 0의 파랑 — 판정 0.13, 14.8절 한계). 밉 생성은 형식 다섯 × 차원 넷 × 장치가 받는 경로 32 실행이 모두 "장치가 쓴 이전 mip의 CPU 면적 박스"와 형식 정밀도 안이다. GL core 위 ES(범위 view 없음 — 텍스처 전체가 묶인다)에서도 raster 경로가 맞았다: 원본 mip만 `texelFetch`하고 대상 mip만 attachment라 같은 texel을 읽고 쓰지 않는다.
- **EngineTests.** `RenderBase_WrapGroupCount`·`RenderBase_StorageImageFormatOf`·`RenderBase_UtilityShaderTypesRegistered`(도메인 크기·거르기 — 밉 D3D 27·GLES 15, clear GLES 32)·`RenderBase_MipGenerationPathSelection`·`RenderBase_ClearResourcesRecording`·`RenderBase_EncodeDefaultTexel`·`RenderBase_DefaultTextureCache`. 기록 장치 집합은 바인딩 layout이 비어 밉 생성은 원본 slot을 못 찾아 기록 전 `NotFound`까지만 본다(실제 기록 열은 Sandbox).
- **SL0 compute probe(14.1절 표).** 이번 묶음에 넣지 못했다. CS 전용 API의 compute probe와 SL1 유틸리티 probe가 CS·PS 양쪽 경로를 덮었고, SL0 probe 원문의 PS 엔트리를 평가 함수 + CS 엔트리로 나누는 일은 남은 항목이다(14.8절에 더한다).

