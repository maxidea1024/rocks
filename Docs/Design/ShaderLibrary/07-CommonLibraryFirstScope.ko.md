# 셰이더 라이브러리 — 7. 공통 라이브러리 1차 범위(SL0, SH2와 함께)

[목차](README.ko.md)

## 7. 공통 라이브러리 1차 범위(SL0)

### 7.1 원칙

- **순수 함수만.** SL0 파일은 자원·상수 버퍼·전역 변수를 선언하지 않는다(텍스처가 필요한 함수는 텍스처·샘플러를 인자로 받는다). 그래서 어떤 pass에 include해도 바인딩이 늘지 않고, PS probe 하나로 모두 시험할 수 있다.
- **HLSL 2018 문법.** `D3D11_SM5`·`GLES31_*` 형식은 HLSL 2021이 꺼져 있다(SS 결정 2). 라이브러리는 템플릿·연산자 오버로드·`select` 내장 등 HLSL 2021 기능과 구조체 메서드를 쓰지 않는다. 벡터 폭별 함수는 손으로 따로 쓴다(`float`·`float2`·`float3`·`float4`). double·half·`min16*`도 쓰지 않는다(GLES·결정 SLD4).
- **실수 규칙.** 비정규수에 기대지 않는다(GPU가 0으로 내릴 수 있다). 0 나눗셈·음수 제곱근 가능성이 있는 곳은 `Safe*` 함수나 이름 있는 하한 상수를 쓴다. NaN·Inf 입력의 결과는 함수 주석에 적고, 정의하지 않은 입력은 시험하지 않는다.
- **결정성.** 정수·비트 함수(해시·패킹·Morton·저불일치 수열의 정수부)는 모든 형식에서 bit 단위로 같아야 한다. 실수 함수는 허용 오차 안에서 같아야 한다(8.3절).
- **근거 표기.** 함수 머리 주석에 공개 문헌(4장 각 영역)과 정의역·치역·오차 상한을 적는다.

### 7.2 A01·`Shared`

| 파일 | 1차 내용 |
|---|---|
| `Public/ShaderFeatures.rsh` | `ROCKS_HAS_WAVE_OPS`(D3D12·Vulkan·Metal 1, D3D11·GLES 0 — SS SLR1 전에는 형식 id 표), `ROCKS_HAS_TYPED_UAV_LOAD`(형식 기능), `ROCKS_HAS_VIEWPORT_LAYER_OUTPUT`(런타임 확인 기능이라 원문에서는 퍼뮤테이션 정의로만) |
| `Common/ShaderPrelude.rsh` | 아래 셋을 include |
| `Common/ShaderAssert.rsh` | `ROCKS_SHADER_ASSERT(cond, code)` — SL0에서는 빈 매크로(GPU 메시지 버퍼는 SL2) |
| `Common/StageOutputs.rsh` | MRT 1–8 출력 구조 선언 매크로, 깊이 출력 변형 |
| `Common/ComputeDispatch.rsh` | `LinearThreadIndex`, `UnflattenGroupId2D`, `DivideAndRoundUp`(정수), 그룹 수 상한 상수(65,535)와 큰 dispatch 풀기 |
| `Common/WaveOperations.rsh` | `WaveSumFloat`·`WaveSumUint`·`WaveMin*`·`WaveMax*`·`WavePrefixSumUint`·`WaveBroadcastFirst` — 웨이브 없는 형식은 호출자가 넘긴 groupshared 배열로 같은 값을 낸다(그룹 단위 결과로 정의해 두 경로가 같은 결과를 갖게 한다, 결정 SLD5) |
| `Common/ScalarArrays.rsh` | `ScalarArrayElement(float4 arr[N], i)` 형 매크로(배열 길이를 인자로) |
| `Shared/ShaderTypeAliases.h`·`ShaderStaticAssert.h`·`ReservedBindingSpaces.h` | 3.3절 |

### 7.3 A02 1차

| 파일 | 함수(요지) | 시험 |
|---|---|---|
| `Math/MathConstants.rsh` | π·2π·½π·1/π·1/2π, `DegreesToRadians`, float 최대·최소 정규수·ε, `SmallestSafeDivisor` 등 하한 상수 | 값 = CPU `double`을 float로 반올림한 값 |
| `Math/ScalarMath.rsh` | `Square`, `Pow2–Pow5`, `Max3`·`Min3`, `SafeRcp`·`SafeSqrt`·`SafeNormalize`(0 벡터 → 지정 기본값), `SaturateSigned`, `LinearStep`, `SmoothStep` 변형, `IsFiniteValue`·`ReplaceNonFinite` | 경계(0·음수·큰 값) 대조 |
| `Math/VectorMath.rsh` | `BuildOrthonormalBasis`(Duff 2017 — 부호 분기 포함), `GramSchmidtTangent`, `ReflectDirection`·`RefractDirection`(전반사 표지) | 직교성 ‖BᵀB−I‖ ≤ 1e-6, 전 방향 무작위 1e5개 |
| `Math/MatrixMath.rsh` | `TransformPosition`·`TransformDirection`(행 벡터), `InverseMatrix3x3`, `NormalMatrixFromWorld`(전치 역), `ExtractTranslation`·`ExtractScale`, reversed-Z 무한 원평면 투영에서 near·`tan(half FOV)` 읽기 | CPU Core `Matrix4` 연산과 같은 결과(Core 수학이 UE 규약 — EIP 2.2절) |
| `Math/Quaternion.rsh` | `QuatMultiply`, `QuatRotateVector`, `QuatToMatrix`, `MatrixToQuat`, `QuatSlerp`, tangent 기저 ↔ quaternion 압축 | Core `Quat`와 같은 회전(UE 회전 방향), 왕복 |
| `Math/FastApproximations.rsh` | `FastAcos`·`FastAsin`·`FastAtan`·`FastAtan2`·`FastRsqrt`·`FastExp2`·`FastLog2` | 정의역 전체 최대 절대·상대 오차 ≤ 함수별 상한(주석 값) |
| `Math/ExtendedPrecision.rsh` | `TwoSum`·`TwoProduct`(FMA 없는 Dekker 분할), 확장 정밀도 3벡터 더하기·빼기·스칼라 곱, `ToTranslatedWorld(extendedWorld, viewOrigin)` | 1e7 cm(100 km) 거리에서 카메라 상대 위치 오차 ≤ 0.01 cm(CPU double 대조). 컴파일러 재결합 금지(`precise`) 확인 |
| `Math/BitPacking.rsh` | unorm·snorm 8·10·16 bit 부호화·복호화, RGB10A2·R11G11B10F·RGB9E5 묶기·풀기, `PackHalf2`·`UnpackHalf2`, `InsertBits`·`ExtractBits`, `PackSignedToUnsigned`(지그재그) | 8·10 bit 전 부호 왕복, 16 bit 전 부호 왕복(65,536), R11G11B10F·RGB9E5 = CPU 참조 bit 단위 |
| `Math/UnitVectorEncoding.rsh` | `EncodeOctahedral`·`DecodeOctahedral`(float2 [-1,1]), 반구 팔면체, `QuantizeOctahedral(bits)`(가장 가까운 격자점 탐색 포함 정밀 버전), `ReconstructNormalZ` | 무작위 방향 1e6개 최대 각 오차 ≤ 격자별 상한(Cigolle 2014 표), 왕복 각 오차 |
| `Math/DitheredQuantization.rsh` | `QuantizeWithDither(color, bits, noise)`(선형·sRGB 영역) | 평균 오차 기대값 0(통계), 결정적 잡음 입력 golden |
| `Math/MortonOrder.rsh` | `MortonEncode2D/3D`·`MortonDecode2D/3D`(16·10 bit 성분) | 전 범위 왕복(2D는 2¹⁶×2¹⁶ 중 표본·경계 전부) |
| `Math/IntegerHash.rsh` | `HashPcg`·`HashPcg2D/3D/4D`, `HashMix32`(finalizer), `HashCombine` | golden 표(입력 0–255·경계값)를 CPU 구현과 bit 대조 |
| `Math/RandomNumbers.rsh` | `UintToUnitFloat`(상위 24 bit → [0,1)), 상태 있는 PCG 수열 | [0,1) 범위·1−ulp 상한, χ² 균일성(1e6 표본, 유의 수준 고정) |
| `Math/ScreenNoise.rsh` | `InterleavedGradientNoise(pixel, frame)`, 프레임 회전, 파란 잡음 텍스처 좌표 | golden 값, 주파수 특성은 측정만 |
| `Math/LowDiscrepancy.rsh` | `RadicalInverseBase2`(bit 뒤집기), `Hammersley`, `Halton2`·`Halton3`, `SobolOwen`(Burley 2020), `R2Sequence` | 정수부 bit 대조, 2D 별 불일치(star discrepancy) 상한 — CPU에서 계산한 같은 수열과 동일 |
| `Math/SampleWarping.rsh` | `ConcentricSquareToDisk`, `UniformSphere`, `UniformHemisphere`, `CosineHemisphere`, `UniformCone`, `SampleGgxVisibleNormal`(Heitz 2018)과 각 pdf | pdf 적분 = 1(수치), 표본 히스토그램 χ²(pdf 대비), 사상 경계 연속성 |
| `Math/SphericalHarmonics.rsh` | 2·3차 기저 평가, 방향 투영, `ConvolveCosineLobe`(Ramamoorthi 계수), 조도 평가, 창 함수(ringing 억제) | 해석 입력(상수·한 방향 광원)의 조도 = 해석해, 직교성 |
| `Math/RayIntersection.rsh` | 광선–평면·구·AABB(slab)·원뿔·원기둥·삼각형(watertight) | CPU double 참조 대조, 경계 스침 광선 무작위 1e5개에서 watertight 성질(공유 모서리 누락 0) |

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
| `Shading/DiffuseTerms.rsh` | `DiffuseLambert`, `DiffuseBurley`(재정규화), `DiffuseChan`, `WrapLighting` | Lambert ∫ = albedo, Burley·Chan 에너지 ≤ 1(수치) |
| `Shading/EnvironmentBrdf.rsh` | `EnvironmentBrdfFromLut(lut, sampler, NoV, roughness)`, `EnvironmentBrdfApprox(NoV, roughness)`(해석 근사) | LUT 표본 = RB 11.1절 CPU 생성 LUT, 근사 오차 상한 측정·기록 |
| `Shading/EnergyConservation.rsh` | 방향 albedo E(μ)·평균 albedo 표 표본, `MultiScatterCompensation(F0, E)` | 백색 노 시험(보정 후 ∫ ≈ 1, 허용 1%) |
| `Shading/SurfaceShading.rsh` | `SurfaceAttributes` → `ShadingParameters`(확산 색·F0·F90·α), shading model 분기 골격(`Unlit`·`DefaultLit`만 동작, 나머지는 `#error`가 아닌 기본값 + 진단 정의) | 금속 0·1, specular 0.5 → F0 0.04 등 표 값 |

### 7.6 시험 셰이더와 실행 위치

- 시험 셰이더는 엔진 내장 라이브러리에 넣지 않는다. 시험 runner(EngineTests)의 셰이더 폴더를 시험 전용 플러그인 루트(`/Plugin/ShaderLibraryTests`)로 mount해 컴파일한다(SS SC2 mount 규칙). 쿠킹 대상이 아니므로 Development 필요 시 컴파일(SS SC12)로 만든다.
- PS probe(SL0): 전체 화면 삼각형(A18 `FullscreenTriangle.rsf` — SL0에 VS만 앞당겨 둔다)으로 N×1 `RGBA32Float`(또는 `R32Uint`) 타깃에 시험 사례 하나당 픽셀 하나를 쓴다. 입력은 시험 사례 번호로 결정적으로 만들거나 작은 상수 표로 준다. 결과는 `Utility/TextureReadback.rsf` 경로로 읽는다. compute probe는 GX4(dispatch) 뒤 같은 시험을 CS로도 돌린다.
- 실행 backend: D3D12 하드웨어·WARP, Vulkan, GLES 미리보기(Sandbox self-test와 같은 대상 — SS 16절 SH2). Vulkan·GLES가 CI에서 NOT RUN이면 로컬 결과를 기록한다.

### 7.7 SL0 완료 기준

1. 7.2–7.5절의 모든 파일이 엔진 컴파일 프로필의 모든 형식에서 컴파일되고 워커 규칙 검사를 통과한다(include 폐포 시험 셰이더 하나가 전부를 include).
2. 모든 공개 함수가 CPU 기준 구현과 대조된다(8장 표의 허용 오차, D3D12·Vulkan·GLES 미리보기).
3. 정수·비트 함수 golden이 모든 backend에서 bit 단위로 같다.
4. 웨이브 경로와 대체 경로가 같은 결과를 낸다(웨이브 있는 형식에서 두 경로를 다 돌린다).
5. 셰이더 라이브러리 시험이 coverage 사유 목록을 낸다: 시험하지 않은 공개 함수가 있으면 이름과 사유.
