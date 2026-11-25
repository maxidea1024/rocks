# 엔진 셰이더 — 7. 공통 라이브러리 1차 범위(SL0, SH2와 함께)

[목차](README.ko.md)

## 7. 공통 라이브러리 1차 범위(SL0)

### 7.1 원칙

- **순수 함수만.** SL0 파일은 자원·상수 버퍼·전역 변수를 선언하지 않는다(텍스처가 필요한 함수는 텍스처·샘플러를 인자로 받는다). 그래서 어떤 pass에 include해도 바인딩이 늘지 않고, PS probe 하나로 모두 시험할 수 있다. CS에서만 의미가 있는 API(`ComputeDispatch`·`WaveOperations`)는 SH2에 compute dispatch가 없으므로 SL0에 넣지 않고 SL1(GX3·GX4 뒤)로 둔다.
- **HLSL 2018 문법.** `D3D11_SM5`·`GLES31_*` 형식은 HLSL 2021이 꺼져 있다(SS 결정 2). 라이브러리는 템플릿·연산자 오버로드·`select` 내장 등 HLSL 2021 기능과 구조체 메서드를 쓰지 않는다. 벡터 폭별 함수는 손으로 따로 쓴다(`float`·`float2`·`float3`·`float4`). double·half·`min16*`도 쓰지 않는다(GLES·결정 SLD4).
- **실수 규칙.** 비정규수에 기대지 않는다(GPU가 0으로 내릴 수 있다). 0 나눗셈·음수 제곱근 가능성이 있는 곳은 `Safe*` 함수나 이름 있는 하한 상수를 쓴다. NaN·Inf 입력의 결과는 함수 주석에 적고, 정의하지 않은 입력은 시험하지 않는다.
- **결정성.** 정수·비트 함수(해시·패킹·Morton·저불일치 수열의 정수부)는 모든 형식에서 bit 단위로 같아야 한다. 실수 함수는 허용 오차 안에서 같아야 한다(8.3절).
- **근거 표기.** 함수 머리 주석에 공개 문헌(4장 각 영역)과 정의역·치역·오차 상한을 적는다.
- **이름(개정 3).** 라이브러리 함수·구조체·상수는 3.5절 이름 그대로 쓰고 `Rocks` 같은 접두를 붙이지 않는다(엔진 밖 셰이더가 include하는 공개 API — 이름 충돌은 플러그인 쪽이 피한다, UE 라이브러리 함수도 접두가 없다). 예외는 RB 18.5절에서 먼저 넣은 `Color/FilmicToneCurve.rsh`의 `Rocks*` 이름으로, `TonemapPass.rsf`·`TextureInspector.rsf`가 쓰므로 그대로 둔다. 그 파일의 원색·순응 행렬은 `Color/ColorSpaces.rsh`·`Color/ChromaticAdaptation.rsh` 상수로 바꿔 같은 행렬을 두 벌 두지 않는다(공용 유틸리티 중복 금지).
- **FXC 단일 출구(개정 4).** `D3D11_SM5`의 FXC는 조건 안 이른 `return`이 있는 함수를 다른 함수의 분기 안에 펼치면 쓰지 않은 반환값을 "초기화되지 않았을 수 있음"(X4000)으로 보고, 경고를 오류로 하는 정책(SS 결정 7)에서 컴파일이 실패한다(2026-11-25 확인). 그래서 라이브러리 함수는 출구가 하나다(조건은 결과 변수에 대입하거나 선택식으로 쓴다). `out` 인자로 받는 지역 변수는 선언 때 초기화한다. HLSL 키워드(`linear`·`sample`·`point` 등 보간 한정자)는 변수 이름으로 쓰지 않는다.
- **행렬 순서.** 색 행렬은 RGB 열 벡터 기준 행 나열로 두고 `mul(M, c)`로 쓴다(색 공간 문헌·SMPTE RP 177 표기와 같다 — 기하 행렬의 행 벡터 `mul(v, M)` 규약(3.4절)과 따로다). 기하 함수(`Math/MatrixMath.rsh`·`Math/Quaternion.rsh`)는 3.4절 행 벡터 규약이다.

### 7.2 A01·`Shared`

| 파일 | 1차 내용 |
|---|---|
| `Public/ShaderFeatures.rsh` | `ROCKS_FEATURE_COMPUTE`, `ROCKS_FEATURE_WAVE_OPERATIONS`, `ROCKS_FEATURE_TYPED_UAV_LOAD`, `ROCKS_FEATURE_DEBUG_PIXEL_UAV`을 늘 0·1로 정의한다. SC10이 형식 정의 `features`를 정의로 내보내기 전(SLR1)에는 형식 id 임시 표: 웨이브는 `D3D12_SM6`만 1, `VULKAN_SM5`·`METAL_SM5`·`D3D11_SM5`·`GLES31_*`는 0(지금 형식 정의 파일과 같다). typed UAV load·디버그 PS UAV는 어휘가 생기기 전까지 모든 형식 0. `vertexViewportLayerOutput`은 런타임 확인 기능이라 원문에서는 퍼뮤테이션 정의로만 쓴다 |
| `Common/ShaderPrelude.rsh` | 정확히 네 파일을 include: `Public/ShaderFeatures.rsh`, `Private/Common/ShaderAssert.rsh`, `Private/Math/MathConstants.rsh`, `Private/Math/ScalarMath.rsh`(4.1절) |
| `Common/ShaderAssert.rsh` | `ROCKS_SHADER_ASSERT(cond, code)` — SL0에서는 빈 매크로(GPU 메시지 버퍼는 SL2. SL2 뒤에도 VS·PS는 `ROCKS_FEATURE_DEBUG_PIXEL_UAV`가 없으면 빈 매크로 — 결정 SLD18) |
| `Common/StageOutputs.rsh` | MRT 1–8 출력 구조 선언 매크로, 깊이 출력 변형 |
| `Common/ScalarArrays.rsh` | `ScalarArrayElement(float4 arr[N], i)` 형 매크로(배열 길이를 인자로) |
| `Shared/ShaderTypeAliases.h`·`ShaderStaticAssert.h`·`ReservedBindingSpaces.h` | 3.3절 |

SL1로 옮긴 CS 전용 API(참고): `Common/ComputeDispatch.rsh`(`LinearThreadIndex`·`UnflattenGroupId2D`·`DivideAndRoundUp`·그룹 수 상한 65,535와 큰 dispatch 풀기), `Common/WaveOperations.rsh`(그룹 단위 `GroupSumUint`·`GroupMin*`·`GroupMax*`·`GroupPrefixSumUint`·`GroupBroadcastFirst`와 float 합. 정수 연산은 웨이브 경로·groupshared 경로가 bit 일치, float 합은 더하는 순서가 달라 허용 오차 안 일치 — 결정 SLD5).

### 7.3 A02 1차

| 파일 | 함수(요지) | 시험 |
|---|---|---|
| `Math/MathConstants.rsh` | π·2π·½π·1/π·1/2π, `DegreesToRadians`, float 최대·최소 정규수·ε, `SmallestSafeDivisor` 등 하한 상수 | 값 = CPU `double`을 float로 반올림한 값 |
| `Math/ScalarMath.rsh` | `Square`, `Pow2–Pow5`, `Max3`·`Min3`, `SafeRcp`·`SafeSqrt`·`SafeNormalize`(0 벡터 → 지정 기본값), `SaturateSigned`, `LinearStep`, `SmoothStep` 변형, `IsFiniteValue`·`ReplaceNonFinite` | 경계(0·음수·큰 값) 대조 |
| `Math/VectorMath.rsh` | `BuildOrthonormalBasis`(Duff 2017 — 부호 분기 포함), `GramSchmidtTangent`, `ReflectDirection`·`RefractDirection`(전반사 표지) | 직교성 ‖BᵀB−I‖ ≤ 1e-6, 전 방향 무작위 2¹⁶개(256×256 배치) |
| `Math/MatrixMath.rsh` | `TransformPosition`·`TransformDirection`(행 벡터), `InverseMatrix3x3`, `NormalMatrixFromWorld`(전치 역), `ExtractTranslation`·`ExtractScale`, reversed-Z 무한 원평면 투영에서 near·`tan(half FOV)` 읽기 | CPU Core `Matrix4` 연산과 같은 결과(Core 수학이 UE 규약 — EIP 2.2절) |
| `Math/Quaternion.rsh` | `QuatMultiply`, `QuatRotateVector`, `QuatToMatrix`, `MatrixToQuat`, `QuatSlerp`, tangent 기저 ↔ quaternion 압축 | Core `Quat`와 같은 회전(UE 회전 방향), 왕복 |
| `Math/FastApproximations.rsh` | `FastAcos`·`FastAsin`·`FastAtan`·`FastAtan2`·`FastRsqrt`·`FastExp2`·`FastLog2` | 정의역 전체 최대 절대·상대 오차 ≤ 함수별 상한(주석 값) |
| `Math/ExtendedPrecision.rsh` | `TwoSum`·`TwoProduct`(FMA 없는 Dekker 분할), 확장 정밀도 3벡터 더하기·빼기·스칼라 곱, `ToTranslatedWorld(extendedWorld, viewOrigin)` | 1e7 cm(100 km) 거리에서 카메라 상대 위치 오차 ≤ 0.01 cm(CPU double 대조). 컴파일러 재결합 금지(`precise`) 확인 |
| `Math/BitPacking.rsh` | unorm·snorm 8·10·16 bit 부호화·복호화, `RGB10A2Unorm`·`RG11B10Ufloat`·`RGB9E5Ufloat` 묶기·풀기(형식 이름은 PixelFormat 문서), `PackHalf2`·`UnpackHalf2`, `InsertBits`·`ExtractBits`, `PackSignedToUnsigned`(지그재그) | 8·10 bit 전 부호 왕복, 16 bit 전 부호 왕복(65,536), `RG11B10Ufloat`·`RGB9E5Ufloat` = CPU 참조 bit 단위 |
| `Math/UnitVectorEncoding.rsh` | `EncodeOctahedral`·`DecodeOctahedral`(float2 [-1,1]), 반구 팔면체, `QuantizeOctahedral(bits)`(가장 가까운 격자점 탐색 포함 정밀 버전), `ReconstructNormalZ` | 무작위 방향 2²⁰개(1024×1024 배치) 최대 각 오차 ≤ 격자별 상한(Cigolle 2014 표), 왕복 각 오차 |
| `Math/DitheredQuantization.rsh` | `QuantizeWithDither(color, bits, noise)`(선형·sRGB 영역) | 평균 오차 기대값 0(통계), 결정적 잡음 입력 golden |
| `Math/MortonOrder.rsh` | `MortonEncode2D/3D`·`MortonDecode2D/3D`(16·10 bit 성분) | 전 범위 왕복(2D는 2¹⁶×2¹⁶ 중 표본·경계 전부) |
| `Math/IntegerHash.rsh` | `HashPcg`·`HashPcg2D/3D/4D`, `HashMix32`(finalizer), `HashCombine` | golden 표(입력 0–255·경계값)를 CPU 구현과 bit 대조 |
| `Math/RandomNumbers.rsh` | `UintToUnitFloat`(상위 24 bit → [0,1)), 상태 있는 PCG 수열 | [0,1) 범위·1−ulp 상한, χ² 균일성(2²⁰ 표본을 1024×1024 2D 배치로, seed·유의 수준 고정) |
| `Math/ScreenNoise.rsh` | `InterleavedGradientNoise(pixel, frame)`, 프레임 회전, 파란 잡음 텍스처 좌표 | golden 값, 주파수 특성은 측정만 |
| `Math/LowDiscrepancy.rsh` | `RadicalInverseBase2`(bit 뒤집기), `Hammersley`, `Halton2`·`Halton3`, `SobolOwen`(Burley 2020), `R2Sequence` | 정수부 bit 대조, 2D 별 불일치(star discrepancy) 상한 — CPU에서 계산한 같은 수열과 동일 |
| `Math/SampleWarping.rsh` | `ConcentricSquareToDisk`, `UniformSphere`, `UniformHemisphere`, `CosineHemisphere`, `UniformCone`, `SampleGgxVisibleNormal`(Heitz 2018)과 각 pdf | pdf 적분 = 1(수치), 표본 히스토그램 χ²(pdf 대비), 사상 경계 연속성 |
| `Math/SphericalHarmonics.rsh` | 2·3차 기저 평가, 방향 투영, `ConvolveCosineLobe`(Ramamoorthi 계수), 조도 평가, 창 함수(ringing 억제) | 해석 입력(상수·한 방향 광원)의 조도 = 해석해, 직교성 |
| `Math/RayIntersection.rsh` | 광선–평면·구·AABB(slab)·원뿔·원기둥·삼각형(watertight) | CPU double 참조 대조, 경계 스침 광선 무작위 2¹⁶개(256×256 배치)에서 watertight 성질(공유 모서리 누락 0) |

### 7.4 A03 1차

| 파일 | 함수(요지) | 시험 |
|---|---|---|
| `Color/ColorSpaces.rsh` | 원색·백색점 상수(Rec.709·Rec.2020·Display P3·AP0·AP1·D65·D60), RGB↔XYZ 행렬(상수는 RP 177 절차로 CPU에서 계산해 소수 9자리), `Luminance(rgb)`(작업 색 공간 Y 행), `RgbToYCoCg`·역, `RgbToHsv`·역, `LinearRgbToOklab`·역 | 행렬 왕복 = 단위 행렬(1e-6), 백색 (1,1,1) → 백색점 xy, 표준 표의 원색 xy |
| `Color/ChromaticAdaptation.rsh` | `BradfordAdaptation(srcWhite, dstWhite)`, `Cat02Adaptation`, `ColorTemperatureToXy`(1,667–25,000 K: Planck 궤적 근사, 4,000 K 이상 선택 주광 궤적), `WhiteBalanceMatrix(temperature, tint)` | 6,500 K·D65 근처 xy = CIE 표 값(허용 0.0005), 같은 백색 순응 = 단위 행렬 |
| `Color/TransferFunctions.rsh` | `LinearToSrgb`·`SrgbToLinear`(구간 함수 정확), `LinearToGamma`, `LinearToRec709`, `Rec1886ToLinear`, `LinearToPq`·`PqToLinear`(nit, 10,000 기준), `LinearToHlg`·역, `LinearToLutLog`·역 | 8 bit sRGB 전 256 부호 왕복 = CPU, PQ 0·100·1,000·10,000 nit 표준 값, 단조성 |
| `Color/FilmicToneCurve.rsh` | 파라미터(기울기·toe·shoulder·black clip·white clip) → 곡선 구간 계수 계산 함수와 평가 함수, AP1 작업(입력 작업 색 공간 → AP1 → 곡선 → 출력 원색) | 파라미터 묶음 몇 개의 곡선 표본 = CPU 구현(같은 식을 double로), 단조성·연속성(구간 경계 C¹) |

### 7.5 A04 1차

| 파일 | 함수(요지) | 시험 |
|---|---|---|
| `Shading/MicrofacetTerms.rsh` | `DistributionGgx(α, NoH)`, `DistributionGgxAniso`, `VisibilitySmithJointGgx`·근사, `FresnelSchlick(F0, F90, VoH)`, `FresnelDielectric(ior, cos)`, `DistributionCharlie`·`DistributionInverseGgx`, `RoughnessToAlpha`(하한 상수) | ∫D(h)(n·h)dω = 1(수치 적분, α 범위), 상반성 f(l,v)=f(v,l), 백색 노 시험(Fresnel 1에서 ∫f cos ≤ 1, 다중 산란 보정 후 ≈ 1) |
| `Shading/DiffuseTerms.rsh` | `DiffuseLambert`, `DiffuseBurley`(재정규화), `DiffuseChan`, `WrapLighting` | Lambert ∫ = albedo, Burley 에너지 ≤ 1, Chan ≤ 1.05(수치 — 7.8절, 개정 4) |
| `Shading/EnvironmentBrdf.rsh` | `EnvironmentBrdfApprox(NoV, roughness)`(해석 근사)만. `EnvironmentBrdfFromLut`는 LUT가 내장 자원 팩에 들어오는 SL3 | 근사 = CPU 수치 적분(split-sum 두 항) 대비 오차 상한 측정·기록 |
| `Shading/SurfaceAttributes.rsh` | RN 14.1절 `SurfaceAttributes` HLSL 구조체와 `DefaultSurfaceAttributes()`(필드 기본값: specular 0.5 등) | 기본값 = RN 14.1절 표, 필드 순서·타입이 RN 정의와 같음(C++ 쪽 `RenderData` 정의와 `Shared` 대조는 SL2) |
| `Shading/SurfaceShading.rsh` | `SurfaceAttributes` → `ShadingParameters`(확산 색·F0·F90·α), shading model 분기 골격(`Unlit`·`DefaultLit`만 동작, 나머지는 `#error`가 아닌 기본값 + 진단 정의) | 금속 0·1, specular 0.5 → F0 0.04 등 표 값 |

### 7.6 시험 셰이더와 실행 위치(개정 3 — 결정 SLD19–SLD21)

- 시험 셰이더는 엔진 내장 라이브러리에 넣지 않는다. 시험 셰이더 폴더 `Programs/Sandbox/EngineShaderTests`를 시험 전용 플러그인 루트(`/Plugin/EngineShaderTests`)로 mount해 컴파일한다(SS SC2 mount 규칙, SLR2). 쿠킹 대상이 아니므로 Development 공급자(SS SSD16·SSD17)로 만든다.
- 시험 전용 셰이더는 이 루트 아래 `/Plugin/EngineShaderTests/<영역>/`에 둔다(엔진 셰이더 트리 `Engine/Shaders`에 두지 않는다 — 결정 SLD16): `Common/IncludeClosure.rsf`(7.2–7.5절 파일 전부를 include하는 VS·PS·CS 엔트리 — 완료 기준 1), `Common/ProbeCommon.rsh`(probe 상수·사례 번호·시험 입력 생성), `Math/MathProbes.rsf`, `Color/ColorProbes.rsf`, `Shading/ShadingTestKernels.rsf`.
- **실행 위치(SLD19).** 시험은 둘로 나눈다. (C) 컴파일 행렬은 EngineTests(`Engine/Source/Developer/ShaderCompiler/Tests/EngineShaderCompileMatrixTests.cpp`)가 실제 워커로 모든 시험 엔트리를 형식 정의 전부(`D3D12_SM6`·`D3D11_SM5`·`VULKAN_SM5`·`VULKAN_ANDROID`·`GLES31_PC`·`GLES31_ANDROID`·`METAL_SM5`·`METAL_IOS`)에 컴파일한다. 도구가 없는 형식(Windows의 Metal)은 SKIP과 사유다. (N)·(G)·(S)·(P) GPU probe는 실제 장치가 있는 Sandbox self-test(`EngineShaderSelfTest.cpp`)가 D3D12·D3D12 WARP·D3D11·D3D11 WARP·OpenGL ES(ES context·GL core context)·Vulkan(하드웨어·소프트웨어 ICD)에서 돌리고 CPU 기준 구현(`EngineShaderReference*.cpp`, 시험 쪽 `double` 구현 — 8.2절)과 대조한 뒤 장치 사이 결과도 대조한다(8.5절). EngineTests에는 실제 GPU 장치 시험이 없으므로(장치 계약 시험은 Sandbox self-test 몫 — SS 5절 P11·S4) GPU probe는 Sandbox에 둔다. `Sandbox --engine-shader-self-test`는 이 검사만 돈다(반복 검증용, `--self-test`에도 들어 있다).
- PS probe(SL0): 전체 화면 삼각형(A18 `FullscreenTriangle.rsf`, RB 11.4절 2항에서 이미 넣었다)으로 W×H `RGBA32Float`(또는 `R32Uint`) 타깃에 시험 사례 하나당 픽셀 하나를 쓴다. 사례가 많으면 2D로 배치하고 폭은 16,384 이하로 둔다. 장치 한도(`DeviceCaps.limits.textureSize2D` — 최소 보장은 Vulkan 4,096, GLES 3.2 2,048)가 더 작으면 그 폭으로 나눠 돈다. 7.3절의 큰 표본(2²⁰)은 1024×1024라 모든 backend에서 한 번에 돈다. 입력은 시험 사례 번호에서 시험 전용 정수 생성기(`ProbeCommon.rsh`·CPU 쪽 같은 식 — 라이브러리 해시와 독립, SLD20)로 만들거나 작은 상수 표로 준다. 생성기는 정수 연산과 24 bit 가수 변환만 써서 GPU·CPU 입력이 bit 단위로 같다.
- **readback(SLD21).** 결과는 Graphics `Device::ReadTexture`로 읽는다. `RGBA32Float`·`R32Uint` 렌더 타깃은 네 backend 모두 ReadTexture가 그대로 읽으므로(RB GX8, A0-G) 변환 셰이더가 필요 없다. `Utility/TextureReadback.rsf`(표본 불가·readback 불가 형식을 읽을 수 있는 버퍼로 바꾸는 셰이더)는 그런 형식이 필요한 SL1로 옮긴다(4.18절).
- compute probe는 GX3·GX4(dispatch) 뒤 SL1에서 같은 시험을 CS로도 돌리고, CS 전용 API(`ComputeDispatch`·`WaveOperations`)는 그때 시험한다.
- 실행 backend 결과가 CI에서 NOT RUN이면(Vulkan·GLES) 로컬 결과를 기록한다(7.8절).

### 7.7 SL0 완료 기준

1. 7.2–7.5절의 모든 파일이 엔진 컴파일 프로필의 모든 형식에서 컴파일되고 워커 규칙 검사를 통과한다(include 폐포 시험 셰이더 하나가 전부를 include).
2. 모든 공개 함수가 CPU 기준 구현과 대조된다(8장 표의 허용 오차, D3D12·Vulkan·GLES 미리보기).
3. 정수·비트 함수 golden이 모든 backend에서 bit 단위로 같다.
4. (SL1로 옮김) 웨이브 경로와 대체 경로 대조는 CS 전용 API와 함께 SL1에서 한다. 웨이브 경로가 있는 형식은 지금 `D3D12_SM6`뿐이다(형식 정의 `features`).
5. 엔진 셰이더 시험이 coverage 사유 목록을 낸다: 시험하지 않은 공개 함수가 있으면 이름과 사유.

### 7.8 SH2g(SL0) 구현 메모(개정 4, 2026-11-25)

SS 16.1절 "SH2g(SL0)"이다(같은 문서의 "SH2g 구현 메모(개정 46)"는 GLES cube 쓰기를 고친 다른 일이다).

- **파일.** 7.2–7.5절 표의 파일을 모두 만들었다(`Public/ShaderFeatures.rsh`, `Common` 넷, `Shared` 셋, `Math` 17, `Color` 넷 — `FilmicToneCurve.rsh`는 원색·순응 행렬을 `ColorSpaces`·`ChromaticAdaptation` 상수로 바꿨다, `Shading` 다섯). `ShaderFeatures.rsh`는 SC10이 이미 `ROCKS_FEATURE_*`를 내므로(SH1e) 빠진 정의를 막는 검사만 한다(임시 표 없음 — SLR1 충족). HLSL 2018·FXC에는 `static_assert`가 없어 `ROCKS_SHADER_STATIC_ASSERT`의 HLSL 쪽은 빈 매크로다(C++ 쪽이 같은 Shared 헤더를 검사한다 — 3.3절). conservative depth 출력은 워커 허용 시맨틱(SS 7.2절 7항)에 없어 `StageOutputs.rsh`에 두지 않았다.
- **구현 중 바꾼 정의.** (1) `GramSchmidtTangent`의 평행 판정은 남은 길이 제곱이 원래 길이 제곱의 1e-8 미만(상대 — 절대 하한이면 평행 입력의 반올림 잡음을 정규화한다). (2) `QuantizeOctahedral`은 후보를 cos 각이 아니라 거리 제곱으로 고른다(16 bit 격자에서 cos 차이가 1 근처 float ulp보다 작아 GPU 최대 오차가 3배였다). (3) `RayTriangleIntersection`의 모서리 함수는 두 꼭짓점을 사전순으로 정렬해 계산하고 부호를 되돌린다(`precise`를 무시하고 FMA로 축약하는 드라이버에서도 공유 모서리 값이 정확히 부호만 다르다). 처음 시험은 실루엣 모서리(두 면을 반대쪽에서 보는 광선 — 둘 다 빗나가는 것이 맞다)를 섞어 누락 약 5.8%를 냈고, 원점을 두 면의 같은 쪽에 두도록 고친 뒤 누락 0이다. (4) `NormalizeExtended`는 `FastTwoSum` 대신 `TwoSum`(크기 순서와 무관). (5) half·11·10 bit float는 정수 연산으로 부호화한다(GLSL `packHalf2x16`의 반올림은 명세가 정하지 않는다). NaN은 부호를 남긴 조용한 NaN이다.
- **시험(결정 SLD19).** EngineTests `EngineShaders_CompileMatrix`: 시험 엔트리 7개(include 폐포 VS·PS·CS, probe 4)와 엔진 셰이더 타입 10개를 형식 정의 전부에 컴파일 — `D3D12_SM6`·`D3D11_SM5`(FXC 직접)·`VULKAN_SM5`·`GLES31_PC`·`GLES31_ANDROID` 통과, `METAL_SM5`는 Windows에 도구가 없어 SKIP, `VULKAN_ANDROID`·`METAL_IOS`는 단계 없는 자리표시 형식이라 SKIP. Sandbox `--engine-shader-self-test`(그리고 `--self-test`): 장치마다 247 검사, 8 장치(D3D12 GTX 1060·WARP, D3D11 GTX 1060·WARP, OpenGL ES ES context·GL 4.3 core, Vulkan GTX 1060·lavapipe) 모두 통과(2026-11-25 로컬). 정수 probe 56개는 golden 지문(`EngineShaderGoldens.inl`)과 같고 장치 사이 bit 단위로 같다. 실수 probe의 장치 사이 최대 차이는 2.4e-4(`CosineHemisphere`의 sin·cos)다.
- **장치 사이 대조에서 뺀 것.** 팔면체 양자화 부호와 watertight 맞음 bit는 float 판정이라 장치마다 다를 수 있다(정수 golden이 아니다) — CPU 기준과의 성질(최대 각 오차·누락 0)로만 판정한다.
- **측정값(허용 오차 안).** 정규 직교 기저 ‖BᵀB − I‖ 3.2e-7, TwoSum·TwoProduct 오차 없음, 1e7 cm 월드의 카메라 상대 오차 6.1e-5 cm, 팔면체 양자화 최대 각 오차 8·12·16 bit 0.632°·0.0393°·0.00246°(double 같은 방법과 같음), PCG χ² 952(임계 1168), Hammersley·Owen Sobol 256점 별 불일치 0.016·0.012, SH 직교성 3e-5, SH3 클램프 코사인 근사 0.094, ∫D(h)(n·h)dω = 1 ± 3e-4(GGX·이방 GGX·Charlie·inverse GGX), GGX 백색 노 최대 0.991, VNDF χ² 통과·pdf 적분 1 ± 4e-4, PQ 표준 표 차이 3e-5.
- **판정을 넓히거나 기록만 한 것(8.3절 "넓힐 때는 사유").** (1) Chan 2018 확산의 백색 노 값은 1.045(roughness 1, 스치는 시선 — retro-reflection 항)다. 발표 식 그대로이고 UE처럼 엽을 1로 자르지 않으므로 판정을 ≤ 1.05로 넓혔다(Burley는 0.914). (2) 환경 BRDF 해석 근사(Karis 2014)는 높이 상관 Smith split-sum 수치 적분과 최대 0.364(NoV 0.04·roughness 0.98), NoV ≥ 0.25에서도 0.195 다르다(비상관 Schlick–Smith 적분과도 roughness 1·NoV 1에서 0.15). 근사 식 자체는 CPU 같은 식과 2e-5 안이다. 측정만 기록하고(0.5 넘으면 실패), SL3 LUT(RB 11.1절)가 대신한다. (3) 높이 상관 가시성의 선형 근사(Hammon 2017)는 정확한 식과 상대 0.29까지 다르다(측정 기록). (4) IGN은 안쪽 frac이 0·1 경계(±1e-4)인 픽셀을 뺀다(정의의 불연속 — 52.98배 폭으로 갈린다).
- **coverage 사유(7.7절 5항).** self-test가 찍는다: 빈 매크로(`ROCKS_SHADER_ASSERT`), 선언 매크로(`ROCKS_PIXEL_OUTPUT_*` — include 폐포 PS가 2 타깃 형을 컴파일), 상수 버퍼 배열 접근(`ROCKS_SCALAR_ARRAY_ELEMENT` — SL2 파라미터 구조체), `Shared` 헤더의 C++ 쪽(SL2 RenderData), 공개 함수를 거쳐 시험되는 보조 함수 목록.
