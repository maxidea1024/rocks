# 셰이더 시스템 설계 — 12. P6a·P14·P6b 형식 정의·셰이더 타입·TargetProfile·쿠킹 매니페스트

[목차](README.ko.md)

## 12. P6a·P14·P6b 형식 정의·셰이더 타입·TargetProfile·쿠킹 매니페스트

엔진 컴파일 프로필은 형식 정의와 함께 P6a(12.1절)가, TargetProfile 필드는 A4가 쓰는 프로필 파일에 P6b(12.3절)가 더한다.

### 12.1 P6a 형식 정의와 엔진 컴파일 프로필

- **형식 정의.** `Engine/Config/ShaderFormats/<id>.json`에 둔다: 이 형식이 쓰는 도구 목록(10절 키 3항의 정적 목록), 언어 버전 기본값, 컴파일러(DXC·FXC·SPIRV-Cross 경로), 셰이더 모델·target env·ESSL 버전, 바인딩 규약 버전, ShaderFormat 처리기 버전, 상한(7.2절 4항)·지원 기능, 생산 호스트 규칙. UE에서는 기능·형식 이름 부분이 `DataDrivenPlatformInfo.ini`의 `[ShaderPlatform]`(`Language`·`MaxFeatureLevel`·`ShaderFormat`·기능 플래그, `Engine/Config/Windows/DataDrivenPlatformInfo.ini:78–139`)에 있고, SM 선택은 ShaderFormat 코드에 있다(`ShaderFormatD3D.cpp:102–150`, FXC·DXC 분기 `D3DShaderCompiler.cpp:1520–1522` `DoesShaderModelRequireDXC`). Rocks는 둘 다 형식 정의에 둔다. 형식 정의 SHA-256이 job 키에 들어간다(10절). 형식 정의에는 결정 2(언어 버전 기본값)·3(셰이더 모델)·5(생산 호스트)가 들어가므로 P6a는 그 결정 뒤다(15절 기한).
- **엔진 컴파일 프로필(개정 32에서 폐지 — 12.3.1절 SSD24. 아래는 SH1 기록).** 프로젝트 프로필 없이 하는 컴파일(엔진 내장 라이브러리, Development 필요 시 컴파일)이 쓰는 호스트별 프로필이다. 런타임 backend 선택이 없는 컴파일 전용이라 TargetProfile과 스키마를 나눈다: `Engine/Config/ShaderFormats/Engine-<호스트>.json` = `{ "id", "formats": [...] }`. `Engine-Windows`는 `D3D12_SM6`·`D3D11_SM5`·`VULKAN_SM5`·`GLES31_PC`·`GLES31_ANDROID`, `Engine-MacOS`는 `METAL_SM5`·`GLES31_ANDROID`다. 그 호스트에서 쓰거나 그 호스트가 빌드하는 대상(Android 포함)이 쓰는 형식 가운데 그 호스트가 만들 수 있는 것만 넣는다(10절, 15절 결정 5에 따라 바뀐다). job 키 9항이 이 프로필에서 나온다(프로필 공통 상한은 개정 17에서 폐기, 7.2절 4항). SH1이 쓰므로 P6a에서 만든다.
- 완료 기준: 스키마 검증(모르는 속성 거부, 도구 목록이 도구 표에 있는지, 엔진 컴파일 프로필의 형식이 형식 정의에 있는지), 형식 정의 변경의 키 무효화(10절).

진척(2026-11-17, `feature/shader-tools`):
- [x] 형식 정의 `Engine/Config/ShaderFormats/{D3D12_SM6,D3D11_SM5,VULKAN_SM5,METAL_SM5,GLES31_ANDROID,GLES31_PC}.json`과 자리만 둔 `VULKAN_ANDROID`·`METAL_IOS`(schema·id·status·backend만). 속성: `schema`(1)·`id`(파일 이름과 같음)·`status`·`backend`·`processor`(ShaderFormat 처리기 이름·버전)·`bindingConvention`(바인딩 규약 버전)·`tools`(job 키 3항 정적 목록)·`language.hlsl2021`(결정 2)·`compiler`(경로와 경로별 값)·`stages`·`limits`(7.2절 4항)·`features`·`deviceRequirements`·`producers`(생산 호스트 규칙, 결정 5).
  - `compiler`: `path`(`DxcDxil`·`Fxc`·`DxcSpirv`·`DxcSpirvMsl`·`DxcSpirvEssl`), `shaderModel`(D3D12 `6_6`, D3D11 `5_0`, SPIR-V 경로 `6_0`), `imageFormat`(ROCKS_IMAGE_FORMAT 전개, D3D 경로 `Empty`·SPIR-V 경로 `VkImageFormat`), `spirvTargetEnv`(`vulkan1.1`), `dxPositionW`, D3D11 `fallbackPath`(`DxcSpirvHlsl50Fxc`), Metal `mslVersion`(2.4)·`mslPlatform`, GLES `esslVersion`(320)·`fixupClipSpace`·`flipVertexY`·`unsupportedImageFormats`. 경로마다 쓰는 속성만 받는다.
  - `limits` 값: D3D12 상수 버퍼 14·SRV 텍스처 128·구조화 버퍼 128·storage image 8·샘플러 16·UAV 합 8(Tier 1 FL 11.0), D3D11 상수 버퍼 13(+루트 상수)·SRV 텍스처 16(개정 13에서 128을 16으로, 결정 RN13), Vulkan 12·128·4·4·16(SRV 텍스처는 개정 14에서 16 → 128, 나머지는 핵심 최소 보장, render target 8), Metal 12·128·4·4·16(7.2절 4항 buffer 예산), GLES 12·16·4·4·16, render target 4, 단계 사이 vec4 15(`GL_MAX_VARYING_VECTORS`), 에뮬레이션 vec4 VS 256·PS 224·CS 128(ES 3.2 최소 보장 `GL_MAX_VERTEX_UNIFORM_VECTORS`·`GL_MAX_FRAGMENT_UNIFORM_VECTORS`·`GL_MAX_COMPUTE_UNIFORM_COMPONENTS`/4). 모든 형식이 UAV는 CS만, 루트 상수 128 byte, 단계 사이 vec4 16 이하·render target 8 이하다.
  - SH0 측정 반영: D3D12 SM 6.6과 장치 요구 `D3D12_SHADER_MODEL_6_6`(S2), `GLES31_PC`가 ESSL 320 그대로이고 장치 요구 `OPENGL_4_3`·`GL_ARB_ES3_2_compatibility`·`GL_ARB_clip_control`(S12, 310 대안 불필요), ROCKS_IMAGE_FORMAT 전개(D3D 경로에 `VkImageFormat`을 두면 거부)와 GLES `unsupportedImageFormats: [r11g11b10f]`.
  - 생산 호스트: `D3D12_SM6`은 macOS DXC 서명(S1 macOS)을 재기 전까지 Windows만, `D3D11_SM5`는 Windows, `VULKAN_SM5`·GLES는 두 호스트, `METAL_SM5`는 두 호스트이고 둘 다 `MetalToolchain` system 도구가 필요하다.
- [x] 엔진 컴파일 프로필 `Engine-Windows.json`(`windows-x64`: `D3D12_SM6`·`D3D11_SM5`·`VULKAN_SM5`·`GLES31_PC`·`GLES31_ANDROID`)·`Engine-MacOS.json`(`macos-arm64`: `METAL_SM5`·`GLES31_ANDROID`). 스키마는 `{schema, id, host, formats}`로 `host`를 더했다(생산 호스트 검사에 쓴다).
- [x] 읽기 코드 `Engine/Source/Developer/ShaderCompiler`(`ShaderCompiler/ShaderFormatDefinition.h`, Core JSON): `ParseShaderFormatDefinition`·`LoadShaderFormatDefinition`·`ParseEngineCompileProfile`·`ResolveEngineCompileProfile`·`LoadEngineCompileProfile`·`CommonLimits`(프로필 공통 상한). 검증: 모르는 속성(최상위·중첩·그 경로에 쓰이지 않는 속성·자리만 둔 형식의 속성), 어휘 밖 값, 도구가 도구 표(P4)에 있는지와 경로가 쓰는 도구가 목록에 있는지, system 도구가 형식 도구 목록에 있는지, backend–처리기–경로 짝, 프로필의 형식이 형식 정의에 있는지·자리만 둔 형식인지·프로필 호스트가 생산 호스트인지. 오류는 JSON 경로와 줄을 돌려준다.
- [x] 형식 정의 SHA-256 = 파일 byte의 SHA-256(job 키 3항). 시험: 같은 byte는 같은 값, 상한·HLSL 2021·처리기 버전·공백 하나를 바꾸면 다른 값.
- [x] 시험(EngineTests `ShaderFormat_*` 8건): 저장소 디렉터리 전체 읽기(정의 8·프로필 2), 결정·측정 값, Windows 프로필 공통 상한(상수 버퍼 12·텍스처 16·storage 4·4·샘플러 16·UAV 합 8·render target 4·단계 사이 15·에뮬레이션 256/224/128), 음성 대조(모르는 속성 6·도구 6·값 불일치 21·프로필 해석 3·프로필 스키마 4).
- [x] 개정 13(2026-11-18, Renderer RQ22 (1)·머티리얼 MSR12): `D3D11_SM5`의 `sampledTextures`를 16으로 내렸다. `D3D12_SM6`·`METAL_SM5` 128과 GLES 16은 그대로다.
- [x] 개정 14(2026-11-18, Renderer RQ11·RQ22 (1)·결정 RN3 (가)): `VULKAN_SM5`의 `sampledTextures` 16 → 128, `renderTargets` 4 → 8. 근거는 GX1 측정(RenderBase 4.4절: GTX 1060 `maxColorAttachments` 8·단계당 sampled image 1048576·set 1048576·`maxPerStageResources` 4294967295, lavapipe 8·1015808·1015808·1015808)이다. 장치 요구는 별도 어휘 없이 `limits` 자체를 장치 능력과 대조한다(7.2절 4항 "상한은 장치 요구다"). 이 형식의 PS 합산 요구는 12 + 128 + 4 + 4 + 8 = 156으로 Vulkan 핵심 최소 보장 128을 넘으므로, 그런 장치는 `VULKAN_SM5`를 쓸 수 없다. 장치 표본이 둘뿐이라 다른 공급자(AMD·Intel)의 값은 미측정이다. 프로필 공통 상한은 GLES가 들어 있어 그대로다(텍스처 16·render target 4).
- [x] 개정 16·17(Renderer RQ14), 구현 개정 18(2026-11-18): `features` 어휘에 런타임 확인 기능 `vertexViewportLayerOutput`을 더해 `D3D12_SM6`·`VULKAN_SM5`·`METAL_SM5` 형식 정의에 넣었다. 읽기 코드(`ShaderFormatDefinition.h`)에 `ShaderFeatureVertexViewportLayerOutput`, 런타임 확인 기능 묶음 `RuntimeCheckedShaderFeatures`와 `IsRuntimeCheckedShaderFeature`를 두었다(7.2절 7항, 15절 결정 12). SH2 전에 앞당겼다.
- [ ] job 키 계산(P5)과 상한 검사(워커: 모든 상한을 job 형식 상한으로, 개정 17)는 이 값을 쓰는 다음 단계에서 한다.
- [x] 개정 21(2026-11-20, 측정 S14): `D3D11_SM5` 형식 정의 `features`에 `vertexViewportLayerOutput`을 더하고 형식 정의 시험의 D3D11 단언을 바꿨다.
- [ ] 개정 29(SH1e E8, [엔진 셰이더](../EngineShaders/09-LimitsAndFollowups.ko.md) SLR1): `features` 어휘에 장치 보장 기능 `typedUavLoad`(형식 확장 typed UAV load)를 더하고, `deviceRequirements` 어휘에 `D3D12_TYPED_UAV_LOAD_ADDITIONAL_FORMATS`를 더해 `D3D12_SM6`에 둘 다 넣는다. 다른 형식은 0(11.1절 E8). 어휘는 SC10 정의 `ROCKS_FEATURE_<이름>`으로 원문에 보인다(11.1절 SC10).
- [ ] 개정 31(7.2절 12항, 엔진 셰이더 SLR12): `features` 어휘에 런타임 확인 기능 `debugPixelUav`를 더해 `D3D12_SM6`·`VULKAN_SM5`·`METAL_SM5`에 넣는다(SH2b). SC10 정의 `ROCKS_FEATURE_DEBUG_PIXEL_UAV`가 함께 생긴다.
- [x] 개정 17(7.2절 4항), 구현 개정 18(2026-11-18): `CommonLimits`와 그 시험(Windows 프로필 공통 상한 단언)을 지우고, 시험을 형식별 상한 단언(D3D12·Vulkan 128, D3D11·GLES 16, Vulkan render target 8, GLES 에뮬레이션 vec4 등)으로 바꿨다.

### 12.2 P14 셰이더 타입·퍼뮤테이션·라이브러리 형식

- **셰이더 타입.** 이름, `.rsf` 가상 경로, 엔트리, 단계, 요구 기능(2절), 퍼뮤테이션 도메인, 거르기 함수를 가진다. 정적 등록 표로 모은다(UE `IMPLEMENT_GLOBAL_SHADER`, `Runtime/RenderCore/Public/GlobalShader.h:433`). 등록 표의 순서는 결과에 영향을 주지 않는다(이름으로 정렬해 쓴다). 개정 16(Renderer RQ13, RenderBase SX4, 머티리얼 시스템 MSR5)에서 다음 두 표지를 더했다.
  - **머티리얼 셰이더 표지.** 표지가 있는 타입은 머티리얼 쿠킹이 머티리얼 셰이더 집합마다 곱한다(Renderer 14.3절 pass 셰이더). 없는 타입은 엔진 셰이더다. 머티리얼 셰이더 타입은 pass가 요구하는 custom 출력 이름 목록을 선언하고, 그 출력이 없는 머티리얼은 거르기에서 빠진다(머티리얼 시스템 8.3절).
  - **정점 입력 축 표지.** 표지가 있는 타입은 정점 입력 타입(RenderBase 10절)마다 따로 컴파일한다. 메시 pass 셰이더와 엔진 기본 정점 입력 타입의 fallback 셰이더(RenderBase 11.1절)가 쓴다. 전체 화면·compute 셰이더는 표지가 없고 정점 입력 타입 자리가 "없음"이다.
  - UE는 이 조합을 Global·Material·MeshMaterial 타입 종류로 나눈다(`Runtime/RenderCore/Public/Shader.h:1219–1226`). Rocks는 표지 둘로 같은 구분을 한다.
- **거르기 함수.** 타입마다 하나다. 서명은 머티리얼 시스템 8.3절·10.4절 D5와 Renderer 14.3절이 정한 것으로 통일한다.
  - 머티리얼 셰이더: `bool ShouldCompile(const MaterialShaderTraits&, VertexInputTypeId, PermutationId, FormatId)`. 정점 입력 축이 없는 타입은 `VertexInputTypeId`에 "없음"을 받는다.
  - 엔진 셰이더: `bool ShouldCompile(VertexInputTypeId, PermutationId, FormatId)`. 정점 입력 축이 없으면 역시 "없음"이다.
  - 정점 입력 타입의 거르기: `bool ShouldCompile(const MaterialShaderTraits&, FormatId)`(머티리얼 시스템 8.3절). 엔진 셰이더에는 기본값 특성(머티리얼 없음)을 넘긴다.
  - **결합.** 컴파일 여부 = 요구 기능 ⊆ 형식 `features`(2절) AND 셰이더 타입 거르기 AND 정점 입력 타입 거르기(정점 입력 축이 있을 때). UE도 정점 입력 타입 거르기와 셰이더 타입 거르기를 AND로 쓴다(`Runtime/Engine/Private/Materials/MaterialShader.cpp:3274, 3287`, `Runtime/Engine/Private/Materials/MeshMaterialShader.cpp:178–186`).
  - **결정성.** 거르기는 순수 함수다. 인자와 아래 형식 사실만 읽고 설정·장치·등록 순서·실행 순서를 읽지 않는다. 쿠커(Developer)와 런타임 완결성 검사(RenderBase 9.2절)가 같은 결과를 내야 하기 때문이다.
  - **형식 사실.** 거르기는 `FormatId`로 형식 정의의 `limits`·`features`를 읽는다. 예: GBuffer +D·+E profile은 `renderTargets` ≥ 8 형식에서만(Renderer 7.1절, 결정 RN3·RN4), VS layer·viewport 출력 퍼뮤테이션은 `features`에 `vertexViewportLayerOutput`이 있는 형식에서만(7.2절 7항). 기능 등급(Mobile·SM5·SM6, 머티리얼 시스템 MSR9)이 형식 정의에 들어가면 형식 사실에 함께 넣는다(자리만 둔다). ShaderCore가 `FormatId` → 형식 사실 조회를 제공한다. 쿠커는 형식 정의 파일에서, 런타임은 라이브러리 머리에 담긴 같은 값에서 채운다. 장치 능력(`DeviceCaps`)은 거르기 입력이 아니다(런타임 확인 기능은 셰이더 집합이 확인한다, 7.2절 7항).
- **퍼뮤테이션.** 차원(bool·정수 범위·열거)을 선언하고 차원 값의 조합에서 퍼뮤테이션 ID를 계산한다. 각 차원은 전처리 정의 하나로 원문에 들어간다(UE `SHADER_PERMUTATION_BOOL`·`TShaderPermutationDomain`, `ShaderPermutation.h:272,629`). 형식과 퍼뮤테이션마다 위 거르기로 컴파일 여부를 정한다(UE `ShouldCompilePermutation`, `GlobalShader.h:100–102`). 머티리얼이 정하는 값(마스크·WPO·PDO)은 차원이 아니라 `MaterialShaderTraits`로 거른다(Renderer 14.3절).
- **퍼뮤테이션 수 보고(Renderer RQ13).** 거르기 결과를 열거하는 결정적 함수를 둔다: 셰이더 타입마다 (정점 입력 타입, 퍼뮤테이션, 형식) 조합 가운데 통과한 수와 그 단계 수. 엔진 셰이더는 `shader-compile --engine`(12.3절)이, 머티리얼 셰이더는 쿠커가 머티리얼마다 이 값으로 축별 수·job 수·이전 쿠킹 대비 증감을 보고한다(머티리얼 시스템 8.5절, 계획 14.1절 "증가량을 cooker가 보고"). 입력 표는 Renderer 14.3절 pass 셰이더·퍼뮤테이션 차원이다.
- **라이브러리 형식.** 머리(형식 버전, 형식 id, 형식 정의 SHA-256, 거르기가 읽는 형식 사실(`limits`·`features`), 항목 수), 항목 표(셰이더 타입 이름 hash·정점 입력 타입 이름 hash(없음은 0)·퍼뮤테이션 ID·단계 → 코드 SHA-256, 그 항목의 요구 기능 가운데 런타임 확인 기능 비트(개정 17, RenderBase 9.2절이 장치 셰이더를 만들기 전에 `DeviceCaps`와 대조한다)), 코드 표(코드 SHA-256 → 오프셋·크기, 같은 코드는 한 번만), 코드 부분(바이트코드·바인딩 layout·GLES 바인딩 머리). UE `FSerializedShaderArchive`(셰이더 맵 hash 표·셰이더 hash 표·항목 표, `Runtime/RenderCore/Public/ShaderCodeArchive.h:206–230`)에 해당한다. 머티리얼 셰이더 집합의 묶음 id 열(RenderBase SX5, 머티리얼 시스템 MSR2)과 항목별 레이아웃 서명(SX6)은 이 개정에서 넣지 않았고 라이브러리 형식 버전 1 확정 전에 따로 넣는다.
- **런타임 조회.** (셰이더 타입, 정점 입력 타입, 퍼뮤테이션 ID, 단계)로 찾는다(RenderBase SX4. UE 셰이더 키도 vertex factory를 별도 축으로 둔다, `Runtime/RenderCore/Public/Shader.h:644–671`). 등록된 타입이 요구하는 조합(위 결합 거르기를 통과한 것)이 라이브러리에 없으면 `IncompatibleCook`이다. 머티리얼 셰이더의 예외(기본 머티리얼 대체)는 머티리얼 시스템 MSR10이다.
- 완료 기준: 퍼뮤테이션 ID의 결정성(등록 순서와 무관), 같은 코드의 중복 제거, 라이브러리 왕복·손상 검출(`CorruptEntry`), 빠진 항목 조회의 `IncompatibleCook`. 개정 16에서 더한 것: 정점 입력 타입 축 왕복(같은 셰이더 타입·퍼뮤테이션의 정점 입력 타입별 항목), 결합 거르기 음성 대조(정점 입력 타입이 거른 조합·요구 기능이 없는 형식의 조합이 라이브러리에 없고 조회도 요구하지 않음), 쿠커와 런타임의 거르기 결과 일치(같은 형식 사실), 형식 사실(`renderTargets`)을 바꾼 형식의 열거 변화, 수 보고 값 = 열거 결과.

#### 12.2.1 P14 확정 설계(개정 31·32, 2026-11-24)

위 서술을 구현 단위로 확정한다(결정 SSD1–SSD9, 권장안. 애매한 곳은 UE 5.8. 개정 32는 교차 검증 반영). 위 서술과 다른 곳은 이 항이 정한다.

- **SSD1 모듈과 공용 타입의 자리.** `Engine/Source/Runtime/ShaderCore`(3절)를 SH2a에서 만든다. 의존은 Core뿐이다.
  - 내용: 셰이더 타입 기술과 등록부, 퍼뮤테이션 도메인, 형식 사실(`ShaderFormatFacts`), 셰이더 라이브러리 파일 형식의 읽기·쓰기, `ShaderBindingLayout`·GLES 복사 표의 형식과 읽기(7.2절 3·4항), 셰이더 전용 정렬 타입과 손으로 쓴 레이아웃 표 형식(7.2절 5항).
  - **공용 타입 이동(개정 32).** 지금 `ShaderCompilerCommon/ShaderFormatDefinition.h`에 있는 `ShaderStageBits`·`ShaderFeatureBits`·`ShaderDeviceRequirementBits`·`ShaderFormatLimits`·`ShaderBackend`(와 런타임 확인 기능 묶음)를 ShaderCore로 **옮긴다**(사본을 두지 않는다). 의존 방향은 `ShaderCompilerCommon` → ShaderCore, `Graphics` → ShaderCore다(Graphics 장치가 `ShaderBindingLayout`·GLES 복사 표를 읽는다 — 16.1절 SSD18). 형식 정의 읽기(JSON)·도구 표 대조는 Developer(`ShaderCompilerCommon`)에 남는다. Runtime은 Developer에 닿지 않는다(9절 정책 그대로).
- **SSD2 셰이더 타입 기술과 식별.**
  - 식별은 **`nameHash`**(64-bit — CP7 렌더 이름과 같은 해시 함수)다. 라이브러리 항목·조회 키·`TypedShader<T>` 조회가 모두 해시로 찾는다.
  - `ShaderTypeDesc`(읽기 전용 값)는 **정의 모듈의 cpp**에 둔다. 공개 헤더에는 이름과 해시 상수만 둔다(Modular 링크에서 데이터 심볼을 다른 공유 라이브러리로 내보내지 않는다 — [빌드 타깃](../BuildTargets/03-Design/03.08-LinkType.ko.md) BD29). 다른 모듈은 해시로 등록부에서 desc를 찾는다.
  - 칸: `name`(ASCII 식별자, 전역 유일)·`nameHash`, `sourcePath`(`.rsf` 가상 경로)·`entryPoint`·`stage`(`ShaderStageBits` 한 비트, SC8), `requiredFeatures`(형식 기능 비트), 표지 `flags`(`MaterialShader`·`VertexInputAxis`·`DevelopmentOnly` — 7.2절 12항), **소속**(`Engine`·`EnginePlugin`·`Project`·`ProjectPlugin`, SSD3), `permutation`(도메인, 없으면 퍼뮤테이션 하나), `shouldCompile`(거르기 함수, null이면 늘 참), `parameterLayout`(손으로 쓴 레이아웃 표, 없으면 null — 레이아웃 서명, 10절 8행·SX6), `hlsl2021`(형식 기본·켬·끔 — 결정 2의 셰이더 타입 선택).
  - 정의 매크로는 cpp용 `ROCKS_SHADER_TYPE(변수, "이름", "/Engine/Private/X.rsf", "MainPS", ShaderStagePixel, …)`(`constexpr` desc)와 헤더용 `ROCKS_SHADER_TYPE_NAME(Type, "이름")`(이름·해시 상수)이다. 쓰기 가능한 정적 저장소는 없다.
  - UE `ModifyCompilationEnvironment`(타입·퍼뮤테이션별 정의·컴파일 플래그 추가) 대응은 후속이다. SH2는 차원 정의와 정점 입력 타입 정의만 쓴다.
- **SSD3 등록·소속·쿠커의 모듈 집합.** UE는 정적 생성자가 전역 연결 목록에 넣는다(`IMPLEMENT_GLOBAL_SHADER` → `FShaderTypeRegistration`, `Runtime/RenderCore/Public/GlobalShader.h:433`). Rocks는 정적 초기화 순서에 기대지 않으므로 **모듈 수명 객체의 `Startup`에서 등록**한다([모듈 로더](../ModuleLoader/03-Design/03.12-HostServices.ko.md) 3.12절).
  - 셰이더 타입을 정의하는 모듈의 수명 객체가 `Startup(context)`에서 `ShaderTypeRegistry::Register(desc 목록)`를 부르고 `Shutdown`에서 거둔다. 등록부는 엔진 부트가 렌더 lane 시작 전에 `Freeze()`하며, 그 뒤 읽기 전용이고 이름 해시순으로 정렬해 둔다(등록 순서는 결과에 영향이 없다).
  - 등록 오류(`InvalidArgument`, 진단에 타입 이름): 같은 이름 두 번, 다른 이름의 같은 `nameHash`(해시 충돌), 가상 경로 규칙 위반(SC2), 단계가 한 비트가 아님, 도메인 크기 상한 초과(SSD4), 소속과 가상 경로 루트가 맞지 않음(`Engine` → `/Engine`, 플러그인 → `/Plugin/<이름>`, `Project` → `/Project`).
  - **소속과 라이브러리(UE와 같다).** `Engine`·`EnginePlugin` 타입은 `Engine` 라이브러리, `Project`·`ProjectPlugin` 타입은 `Project` 라이브러리로 간다(12.3.1절 SSD11).
  - **쿠커의 모듈 집합.** 쿠커가 등록부를 채우는 모듈 집합은 쿠킹 대상 Game 타깃의 활성 모듈 집합(엔진 모듈 + 활성 플러그인 모듈 + 프로젝트 모듈)과 같아야 한다. 라이브러리 머리에 그 집합의 셰이더 타입 해시 목록 SHA-256을 기록하고, 런타임 등록부가 만든 값과 다르면 셰이더 집합을 열 때 `IncompatibleCook`이다. 프로젝트·플러그인 셰이더 타입을 쿠킹하려면 쿠커가 그 모듈을 적재해야 하므로 BuildTargets BS4·모듈 로더 ML5b가 선행이다. 그 전에는 `Engine` 라이브러리(엔진 모듈 타입)만 쿠킹한다.
- **SSD4 퍼뮤테이션 도메인.**
  - 차원 종류: bool(정의 값 0·1, UE `SHADER_PERMUTATION_BOOL`), 정수 범위 [first, first + count)(UE `SHADER_PERMUTATION_RANGE_INT`), 희소 정수 목록(UE `SHADER_PERMUTATION_SPARSE_INT`), 열거(값 이름 목록 — 정의 값은 0부터 순번, UE `SHADER_PERMUTATION_ENUM_CLASS`).
  - 차원마다 정의 이름 하나다(`ROCKS_`·`__` 접두 금지 — SC10 그대로). 차원에 `DevelopmentOnly` 표지를 둘 수 있다(그 차원이 기본값이 아닌 퍼뮤테이션은 개발 전용).
  - 퍼뮤테이션 ID는 UE와 같은 혼합 기수다: 선언 첫 차원이 가장 낮은 자리(`Runtime/RenderCore/Public/ShaderPermutation.h`의 `ToDimensionValueId` = 첫 차원 값 + 첫 차원 수 × 나머지 ID). 희소 정수 차원의 value id는 UE 순서를 따른다: 목록의 **첫 값이 가장 큰 value id**다(`ShaderPermutation.h:208–214`의 재귀 정의). ID ↔ 차원 값은 서로 역함수다.
  - **차원 추가와 ID 보존.** 차원을 선언 끝에 더하면(새 차원의 기본값 = value id 0일 때) 기존 퍼뮤테이션의 ID가 그대로다(가장 높은 자리에 0이 붙는다). 앞이나 중간에 넣으면 ID가 바뀐다. 어느 쪽이든 job 키는 전처리 결과(정의 포함)로 계산하므로(10절 5행) 컴파일 결과의 재사용은 ID와 무관하다. ID가 바뀌면 바뀌는 것은 라이브러리 항목 키뿐이고, 그것은 쿠킹이 다시 만든다.
  - 도메인 크기 상한 `MaxShaderPermutations` = 65,536이다(넘으면 등록 오류 — 조합 폭발을 등록 때 잡는다. UE에는 상한이 없다).
- **SSD5 형식 사실과 거르기 결합.**
  - `ShaderFormatFacts`: 형식 id, backend, 셰이더 모델, `limits`(12.1절 칸 그대로), `features`, `deviceRequirements`(12.3.1절 SSD15). 쿠커·공급자는 형식 정의에서, 런타임은 라이브러리 머리에서 채운다(위 서술 그대로).
  - 결합 순서: (1) `requiredFeatures ⊆ features`, (2) 개발 전용 타입·퍼뮤테이션은 개발 셰이더를 만드는 경우에만(12.3.1절 SSD11, 7.2절 12항), (3) 셰이더 타입 거르기, (4) 정점 입력 축이면 정점 입력 타입 거르기. 결정성 규칙은 위 서술 그대로다.
  - 정점 입력 타입 등록부는 셰이더 타입 등록부(SSD3)가 함께 맡는다(개정 48, RenderBase 10.6절 1항): desc `ShaderVertexInputType`(이름·id·`.rsh` 원문 경로·소속·거르기)을 같은 `Freeze`로 닫고, 쿠커·공급자·셰이더 집합이 등록부 목록을 쓴다. 정점 입력 타입의 장치 쪽(정적 정점 배치)은 RenderBase 몫이다.
- **SSD6 job 만들기.** (셰이더 타입, 정점 입력 타입, 퍼뮤테이션 ID, 형식)마다 job 하나다.
  - 엔트리 경로·함수·단계는 타입에서, 정의는 퍼뮤테이션 차원 정의 + 개발 전용 표지에서 나오는 엔진 정의 `ROCKS_DEBUG_BUFFER`(7.2절 12항 — SC10이 직접 정의한다)에서 온다. SC10이 이름순으로 조립한다.
  - 정점 입력 축 job은 그 정점 입력 타입의 `.rsh` 경로(`ShaderCompileJob::vertexInputSource`)를 받고, 전처리기가 고정 가상 경로 `/Engine/Generated/VertexInput.rsh`의 include를 그 파일로 잇는다(생성 include 표 `ShaderGeneratedInclude`, 개정 48 — RenderBase 10.6절 2항). include 폐포(키 5행)에는 실제 경로가 들어가 정점 입력 타입마다 키가 다르다. 연결이 없는 job의 그 include는 `GeneratedSource`다. 정점 입력 타입이 주는 정의는 지금 없다(필요하면 desc에 더한다).
  - 옵션은 형식 기본값에 타입의 `hlsl2021` 선택을 덮는다. 레이아웃 서명은 타입 `parameterLayout`의 SHA-256이다.
  - 관리자 `Compile`에 일괄로 넘긴다(SH1d). 열거는 결정적 순서(타입 이름 해시 → 정점 입력 타입 id → 퍼뮤테이션 ID → 형식 id)다.
- **SSD7 라이브러리 파일 형식 v1(`.rshl`).** little-endian. 모든 표는 정렬되어 있고 표 원소는 **고정 stride**다. 섹션 오프셋은 8 byte 정렬이고 정렬·stride 사이 빈 byte는 0이다(같은 입력 같은 byte — 10절 결정성).
  - 머리: magic `RSHL`, 형식 버전 1, 라이브러리 이름(`Engine`·`Project`), 형식 id, 형식 정의 SHA-256, 형식 사실(SSD5), 셰이더 타입 집합 SHA-256(SSD3), 항목 수·코드 수, 섹션 오프셋·크기, **파일 SHA-256**. 파일 SHA-256은 이 칸을 0으로 둔 머리와 그 뒤 전체를 덮는다.
  - 항목 표(키 오름차순, 고정 stride): 묶음 id(u64 — 엔진 셰이더 0, 머티리얼은 셰이더 집합 id, SX5·MSR2), 셰이더 타입 `nameHash`(u64), 정점 입력 타입 id(u64, 없음 0), 퍼뮤테이션 ID(u32), 단계(u8), 개발 전용 표시(u8), 런타임 확인 기능 비트(u32, 개정 17), 레이아웃 서명(SHA-256, 없으면 0 — SX6), 코드 색인(u32).
  - 코드 표(코드 SHA-256 오름차순, 고정 stride, 같은 코드는 한 번): 코드 SHA-256(아래 코드 부분 byte의 SHA), 오프셋, 크기.
  - 코드 부분: 코드마다 셰이더 Record 값(10절 값 표)을 (값 id, 크기, byte)로 잇는다. 값 0 Code·1 BindingLayout·2 GlesCopyTable만 넣고 3 DebugSymbols는 넣지 않는다(SH4가 별도 파일로).
  - **이름 표(개정 35 — RenderBase CP7 정합).** 타입 이름 문자열이 정체성이고 해시는 실행 중 조회용이다(CP7). 그래서 이름 표는 필수다: 항목에 나오는 모든 `nameHash`마다 (nameHash, 이름 byte 수, 이름 byte) 한 줄이 해시 오름차순으로 있다. 항목 표의 `nameHash`는 조회용이다. 읽을 때 이름 표의 이름에서 `ShaderTypeNameHash`를 다시 계산해 저장된 해시와 다르거나, 항목의 해시가 이름 표에 없거나, 이름이 셰이더 타입 이름 규칙(식별자, 64 byte 이하)을 어기면 `CorruptPackage`다. 해시 함수를 바꾸면 라이브러리 byte가 바뀌지만 이름으로 다시 계산하므로 틀린 항목을 조용히 쓰지 않는다.
  - UE `FSerializedShaderArchive`(셰이더 맵 hash 표·셰이더 hash 표·항목 표, `Runtime/RenderCore/Public/ShaderCodeArchive.h:206–230`)에 해당한다. 셰이더 맵 hash 대신 묶음 id 열로 묶는다.
  - 읽기 검사: 길이·magic, 형식 버전(1이 아니면 `UnsupportedVersion`), 파일 SHA·표 정렬·stride·범위·코드 색인 범위·중복 키·0이 아닌 빈 byte는 손상 `CorruptPackage`다(AssetSystem 쿠킹 패키지 판정 `CheckCookedPackageFile`과 같은 코드 — 12.3.1절 SSD14).
- **SSD8 런타임 조회.** `ShaderLibrary::Find(키)`(이진 탐색)다. 열기·완결성·서명 검사와 엔진 → 프로젝트 조회 순서는 RenderBase 9.2·9.3절이다(SX6·SX7). 같은 키가 엔진·프로젝트 라이브러리에 함께 있으면 쿠킹 결과가 잘못된 것이므로 열 때 `IncompatibleCook`이다. 완결성 검사는 **개발 전용 항목을 빼고** 한다(12.3.1절 SSD16).
- **SSD25 형식 사실(개정 35).** `ShaderFormatFacts` = 형식 id(`MaxShaderFormatIdBytes` 고정 칸), backend, 셰이더 모델(major·minor), `ShaderFormatLimits` 전체, `features`, `deviceRequirements`. `ShaderCompilerCommon`의 `MakeShaderFormatFacts(형식 정의)`가 만든다. 라이브러리 머리에는 정해진 순서의 little-endian 칸으로 담는다(SSD7).
- **SSD26 바인딩 형식(개정 35, 7.2절 3·4항).** 워커가 값 1·2로 내고(SH2b·SH2c) 장치가 읽는(SH2e) byte 형식을 ShaderCore가 정한다. 모두 little-endian, 고정 순서, 버전 칸이 맨 앞이다.
  - **`ShaderBindingLayout` v1(값 1).** 형식 버전 1, 단계(한 비트), 루트 상수 byte 수(0 = 없음), 정점 입력 attribute 마스크(u32, `ATTRIBUTEn` — VS만), render target 마스크(u32, `SV_Target0–7` — PS만), 자원 수, 자원 표. **개정 39에서 v2**: render target 마스크 뒤에 단계 사이 마스크(u32, `TEXCOORDn` — VS 출력·PS 입력, CS는 0)를 더한다(16.1절 SSD37 연결 검사의 입력). v1 reader는 두지 않는다(아직 쿠킹 결과를 읽는 쪽이 없다 — 처리기 버전이 올라 너겟도 다시 만든다). 자원 하나: 종류(u8 — 상수 버퍼·SRV 텍스처·구조화·raw 버퍼 SRV·구조화·raw 버퍼 UAV·storage image·샘플러, 7.2절 3항), 단계 안 순번(u16), 이름(byte 수 + byte, 64 byte 이하 식별자), 상수 버퍼 byte 수(u32, 상수 버퍼만), 샘플러 출처(u8 — 없음·자기·공유 Wrap·공유 Clamp, 텍스처만, 9항), 공유 샘플러 표시(u8, 샘플러만), 상수 버퍼 멤버 수와 멤버 표(이름·오프셋·byte 수, 상수 버퍼만 — 5항 오프셋 검사의 입력). 자원 표는 (종류, 순번) 오름차순이고 같은 (종류, 순번)은 하나다. 읽기는 버전·범위·정렬·중복·이름 규칙을 검사하고 어기면 `CorruptPackage`다.
  - **GLES 바인딩 머리 v1(값 2, `ShaderGlesBindingHeader`).** 형식 버전 1, 단계, attribute 마스크, uniform 배열 표(배열마다 상수 버퍼 순번 — 루트 상수는 0xFFFF —, 기본 타입 h·i·u, vec4 수), 샘플러 표(유닛마다 combined sampler 이름과 샘플러 출처), 복사 표(항목마다 상수 버퍼 순번, 원본 byte 오프셋, byte 수, 대상 배열 색인, 대상 vec4 오프셋). 배열 이름은 규칙(`<단계>c<순번>_<타입>`·`<단계>u_<타입>`, 7.2절 4항)에서 나오므로 담지 않는다. 표는 정렬되어 있고 복사 구간은 겹치지 않는다(어기면 `CorruptPackage`).
- **SSD27 손으로 쓴 레이아웃 표와 서명(개정 35, 7.2절 5항).** `ShaderParameterLayout` = 구조체 이름, 그리기별 상수 표시(루트 상수), 전체 byte 수, 멤버 표(이름·타입(float·int·uint 스칼라와 2–4성분, `float4x4`)·오프셋·배열 원소 수(0 = 배열 아님)). `ValidateShaderParameterLayout`이 5항 규칙을 검사한다: 허용 타입, 정렬(스칼라 4·2성분 8·3·4성분과 행렬 16), 배열은 원소 16 byte 배수만, 16 byte 슬롯 하나에 기본 타입 섞기 금지, 멤버 겹침 금지, 전체 크기 16 byte 배수, 루트 상수면 128 byte 이하. 셰이더 타입은 레이아웃 표 목록(0개 이상)을 가진다. **레이아웃 서명** = 목록의 정규 byte(구조체마다 이름·표시·크기·멤버 (이름, 타입, 오프셋, 원소 수)를 순서대로 little-endian으로)의 SHA-256이고, 목록이 비면 0이다(10절 8행·SX6). C++ 쪽은 ShaderCore 셰이더 전용 정렬 타입(`ShaderFloat2` `alignas(8)`, `ShaderFloat3`·`ShaderFloat4`·`ShaderFloat4x4` `alignas(16)`, int·uint 판)으로 구조체를 쓰고 `static_assert`로 표의 오프셋과 맞춘다.
- **SSD28 `CookShaderLibraries`(개정 35, SH2a2).** 입력: 등록부(`Freeze`됨), 형식 snapshot 목록, 정점 입력 타입 목록(등록부 `VertexInputTypes()` — 개정 48. 그 전에는 호출자가 줬다), 라이브러리 이름(`Engine`·`Project`), 개발 전용 포함 여부, 관리자, 출력 디렉터리. 형식마다 소속이 라이브러리에 맞는 타입을 SSD6 순서로 열거하고(SSD5 결합 — 형식 사실은 `MakeShaderFormatFacts`), 관리자 `Compile`로 한 번에 컴파일한 뒤 SSD7 v1 byte를 만든다. job 실패는 모아 보고하고 그 형식의 파일을 쓰지 않는다. 파일은 `<출력>/<이름>-<형식 id>-<파일 SHA 앞 16 hex>.rshl`에 임시 파일 → no-replace 이동으로 쓴다(같은 이름이 있으면 크기·SHA가 같을 때 그대로 둔다, 다르면 `CorruptEntry`). 매니페스트·게시·정리는 SH2d(비에셋 산출물 훅)다. 한계: 정점 입력 타입이 주는 정의는 RenderBase 10절이 정하기 전까지 넣지 않는다(축은 열거하지만 원문이 같으면 코드가 같아 한 번만 담긴다).
- **SSD9 퍼뮤테이션 수 보고.** 위 서술의 열거 함수를 SSD6 열거로 쓴다. 보고는 `asset-cook` 쿠킹 보고서의 셰이더 절이다(타입별 통과 조합 수·job 수·이전 쿠킹 대비 증감). `shader-compile --engine`은 만들지 않는다(12.3.1절 SSD11).
- 완료 기준(위에 더한다): 등록 오류 음성 대조(중복 이름·해시 충돌·경로·단계·도메인 상한·소속과 루트 불일치), 퍼뮤테이션 ID ↔ 차원 값 왕복과 UE 순서 golden(희소 정수 포함), 끝에 더한 차원의 ID 보존, 라이브러리 v1 왕복(묶음 id·개발 전용·서명 열 포함)과 손상 음성 대조(magic·버전·파일 SHA·정렬·stride·범위·중복 키·빈 byte), 같은 입력 같은 라이브러리 byte, 타입 집합 SHA 불일치의 `IncompatibleCook`.

### 12.3 P6b TargetProfile·쿠킹 매니페스트·엔진 내장 라이브러리

- **TargetProfile**(AssetSystem 10절)은 backend와 형식 id 목록만 갖는다. 언어 버전·target env·생산 호스트·장치 요구를 프로필에 두면 출처가 둘이 되기 때문이다. `graphicsBackends`는 런타임이 시도하는 순서다. `shaderFormats`는 backend마다 선호 순서가 있는 형식 목록이며(형식의 backend는 형식 정의가 정한다), 런타임은 장치가 지원하는 첫 형식을 고른다(UE `D3D12TargetedShaderFormats` 같은 backend별 형식 목록, `Runtime/RHI/Private/Windows/WindowsDynamicRHI.cpp:624–626`). 그래서 장치 요구 수준이 다른 형식(2절)이나 결정 3의 D3D12 형식 두 개를 함께 둘 수 있다. 목록에 형식이 없는 backend, 목록에 없는 backend의 형식, 형식 정의의 backend가 키와 다른 형식은 프로필 검증 오류다. backend 이름은 구현 여부와 무관한 고정 어휘다(`D3D11`은 SH5에서 구현된다). 런타임은 구현되지 않았거나 장치가 없는 backend를 `UnsupportedCapability`로 건너뛰고 다음 backend를 시도한다.

```json
"graphicsBackends": ["D3D12", "D3D11", "Vulkan"],
"shaderFormats": { "D3D12": ["D3D12_SM6"], "D3D11": ["D3D11_SM5"], "Vulkan": ["VULKAN_SM5"] }
```

- 엔진 기본 프로필: `Windows`(위), `Mac`(`Metal`: `METAL_SM5`, 새로 더함), `Android`(`OpenGL`: `GLES31_ANDROID`, SH6에서 `Vulkan`: `VULKAN_ANDROID`를 앞에 더함). 현재 엔진 기본은 `Windows`·`Android`뿐이다(AssetSystem 10절). backend 이름 `OpenGL`은 OpenGL ES backend다(UE `EWindowsRHI::OpenGL`과 같은 이름).
- 엔진 컴파일 프로필은 개정 32에서 없앴다(12.3.1절 SSD24). 형식 목록과 순서의 출처는 TargetProfile 하나다.
- `GLES31_PC`는 출시 프로필과 엔진 내장 라이브러리에 넣지 않는다. Development 실행 파일이 필요할 때 컴파일한다(SH1). UE도 쿠킹 데이터를 쓰는 실행 파일에는 이 플랫폼을 등록하지 않는다(2절). Release Windows 실행 파일은 OpenGLGraphics를 링크하지 않는다(9절 정책).
- **쿠킹 산출물.** (프로필, 형식)마다 셰이더 라이브러리 파일 하나를 만든다. 파일 이름은 패키지처럼 내용 주소(AssetSystem 4.2절 규칙, 형식 id와 내용 SHA-256)라 게시된 파일을 덮어쓰지 않는다. 게시와 정리는 `Packages/`와 같다: staging → no-replace `Move` → `SyncDirectory` → 매니페스트 `Replace`, 전체 쿠킹은 새 매니페스트가 가리키지 않는 라이브러리 파일을 지우고 부분 쿠킹은 지우지 않는다(AssetSystem 10절). UE는 라이브러리(Global·프로젝트·플러그인/청크)×형식마다 `ShaderArchive-<Library>-<Format>-<Platform>.ushaderbytecode`를 쿠킹하고(`Runtime/RenderCore/Private/ShaderLibrary/ShaderCodeLibraryUtilities.cpp:84–96`, `<Format>-<Platform>` 문자열 `ShaderCodeLibrary.cpp:1482–1488`), 배포는 IoStore나 네이티브 라이브러리로 연다(ushaderbytecode는 배포 형식이 아니라는 주석, `ShaderLibraryInstance.cpp:27–43`). Rocks도 라이브러리 파일은 쿠킹 단위이고, 배포 컨테이너로 옮기는 일은 패키징 단계가 맡는다. 라이브러리 분할(플러그인·청크)은 후속이다.
- **엔진 내장 셰이더 라이브러리.** 엔진 셰이더(`Engine/Shaders`)의 라이브러리는 엔진 설치물을 만드는 단계(설치 SDK 내보내기·패키징)에서 `shader-compile --engine` 명령으로 그 호스트의 엔진 프로필 형식(`GLES31_PC` 제외)마다 만든다. 빌드 그래프 action이 아니다: Rocks.Build에는 빌드한 프로그램을 실행하는 action이 없다(action 종류는 Compile·Archive·Link·Copy, `ActionGraph.cs`). 그 호스트가 만들 수 없는 형식은 다른 호스트의 설치물 단계가 만든다(macOS는 DXBC를 만들지 않는다. 계획 §27.9는 호스트가 지원하는 target의 로컬 빌드가 되어야 한다고 요구한다). 설치물에서는 프로젝트·너겟·네트워크 없이 연다(계획 9.6, 5.2절 K3). 개발 트리에서는 Development 필요 시 컴파일이 같은 역할을 한다. UE의 Global 라이브러리에 해당한다. 프로젝트 쿠킹은 프로젝트·플러그인 셰이더 라이브러리만 만든다.
- 시험 전용 플러그인 루트 `/Plugin/EngineShaderTests`(11.1절 SC2, 개정 29 — 엔진 셰이더 SLR2)의 원문은 엔진 내장 라이브러리에 넣지 않는다. 엔진 셰이더 시험 runner만 그 루트를 mount해 Development에서 컴파일한다.
- 쿠킹 매니페스트 v1은 AssetId 단위라서 v2에 "에셋이 아닌 산출물" 항목(이름, 형식 id, 파일 SHA-256·크기, cookKey)을 더한다. 라이브러리 cookKey 입력은 포함된 job 키 정렬 목록과 라이브러리 형식 버전이다. 게시는 기존 쿠킹 게시와 같다(staging → `SyncDirectory` → `Replace`).
- 런타임은 실행 장치의 backend에 맞는 형식의 라이브러리만 연다. 없거나 필요한 셰이더가 빠졌으면 `IncompatibleCook`이다.

완료 기준: 프로필 스키마 검증(모르는 속성 거부, 프로필에 형식 옵션을 적으면 오류, backend·형식 목록 검사), 장치 기능에 따른 형식 선택, 형식 상한 초과가 그 형식 job만의 쿠킹 오류인지, 매니페스트 v2 왕복, 형식이 빠진 쿠킹을 연 런타임의 `IncompatibleCook`, 프로젝트 없이 엔진 내장 라이브러리를 여는 시험.

#### 12.3.1 P6b 확정 설계(개정 31·32, 2026-11-24)

위 서술을 구현 단위로 확정한다(결정 SSD10–SSD17, SSD24). **엔진 내장 셰이더 라이브러리를 만드는 시점은 위 서술과 다르다(SSD11).** AssetSystem 쪽(TargetProfile 칸·매니페스트 v2·비에셋 산출물 단계 훅)은 에셋 세션이 AssetSystem 문서를 먼저 고치고 구현한다. 셰이더 쪽은 `CookShaderLibraries`와 그 훅에 붙는 구현만 한다(16.1절 범위 충돌).

- **SSD10 TargetProfile 칸.** AssetSystem 10.2절 2항의 `graphicsBackend`(String, A4는 기록만)를 다음 둘로 바꾼다(하위호환 없음 — 다시 쿠킹한다):
  - `graphicsBackends`(`Array<String>`): 런타임이 시도하는 순서다.
  - `shaderFormats`(`Map<String, Array<String>>`): backend → 선호 순서 형식 id 목록이다.
  - 병합: `graphicsBackends`는 ReflectionText 기본(배열 통째 교체)이다. `shaderFormats`는 **Replace**(프로젝트 파일에 있으면 Map 전체를 통째로 바꾼다 — 키별 병합을 쓰지 않는다)다. backend 목록과 형식 목록이 서로 맞아야 하므로 반쪽만 덮어 엇갈린 조합이 생기지 않게 한다. 새 칸이 프로필 출력 설정 SHA-256에 들어가므로 기존 쿠킹은 다시 쿠킹된다.
  - 검증(위반은 `InvalidSetting`). 개정 39(AssetSystem 10.3절 A4f 정합)에서 둘로 나눴다: **프로필 구조 검증**(어휘 — backend 이름이 고정 어휘 `D3D12`·`D3D11`·`Vulkan`·`Metal`·`OpenGL` 밖, `graphicsBackends` 중복, 목록의 backend에 형식 목록이 없음, 목록에 없는 backend 키, 상한 8 backend × backend당 16 형식)은 에셋 쪽(프로필 읽기)이 한다. **형식 정의 디렉터리 대조**(없는 형식 id·자리만 둔 형식, 형식 정의의 backend가 키와 다름, `GLES31_PC` — 출시 프로필에 넣지 않는다, 위 서술)는 셰이더 쪽 비에셋 산출물 단계(`CookArtifactStage`, SSD41)가 시작할 때 하고 위반을 진단 sink에 `InvalidSetting`으로 낸다.
  - **셰이더 없는 프로필(개정 39).** `graphicsBackends`가 비면(전용 서버 등) 매니페스트 머리 backendCount가 0이고 셰이더 단계는 라이브러리를 만들지 않는다. 런타임은 그래픽 장치·렌더러를 만들지 않는다(UE 전용 서버의 NullDrv와 같은 자리). 그런 쿠킹에서 장치 생성을 요청하면 `Unsupported`와 진단 "this cook has no shader formats"다.
  - 엔진 파일: `Windows`(`graphicsBackends` [`D3D12`, `D3D11`, `Vulkan`] — 이 순서, 형식은 위 예시), `Android`(`OpenGL`: `GLES31_ANDROID`), `Mac`(`Metal`: `METAL_SM5` — SH2에서 파일을 더한다. MetalGraphics는 SH5지만 형식 쿠킹은 Metal 도구가 있는 호스트에서 된다).
- **SSD24 엔진 컴파일 프로필 폐지(개정 32).** 12.1절의 엔진 컴파일 프로필(`Engine-Windows`·`Engine-MacOS`)을 없앤다. 형식 목록과 순서의 출처를 TargetProfile 하나로 둔다.
  - job 키 7행(프로필 id·byte SHA)을 지운다(10절). 형식 정의 SHA-256이 이미 3행에 있고, 프로필의 형식 목록·순서는 job 출력 byte에 영향이 없다.
  - `shader-compile`의 기본 형식 목록(SC13)과 공급자(SSD16)는 **호스트 플랫폼 TargetProfile**(Windows 호스트는 `Windows`, macOS 호스트는 `Mac` — 프로젝트 재정의를 합친 결과)의 `shaderFormats`를 `graphicsBackends` 순서로 이은 목록이다. 엔진 개발 트리에서 프로젝트가 없으면 엔진 파일 그대로다. `GLES31_PC`는 공급자가 따로 더한다(OpenGL 미리보기 backend를 고른 경우).
  - SH1 코드(`LoadEngineCompileProfile`·`Engine-*.json`·키 7행·`--profile`)는 SH2a2가 지운다(개정 36에서 지웠다 — 16.1절 SH2a2 구현 메모). TargetProfile `shaderFormats`(SSD10 — 에셋 세션)가 생기기 전까지 `shader-compile`은 `--format`을 하나 이상 받아야 한다(없으면 사용법 오류). 호스트 플랫폼 TargetProfile 기본값은 SSD10 뒤에 붙인다.
- **SSD41 SH2d 계약(개정 39, AssetSystem 10.3절 A4f — 에셋 세션 rocks-16).**
  - 비에셋 산출물 단계는 `CookArtifactStage{종류, 정체성, Run(request, sink)}`이고, `ShaderCompiler` 모듈 시작 hook이 `CookArtifactStageTable`에 **명시 등록**한다(정적 초기화 순서에 기대지 않는다 — SSD3과 같은 원칙).
  - 단계는 산출물의 이름·형식 id·cookKey와 byte만 넘긴다. 크기·SHA-256·내용 주소 파일 이름은 AssetCooker가 계산한다(셰이더 쪽이 파일 이름 규칙을 따로 두지 않는다 — SSD12·SSD28의 파일 쓰기는 직접 호출(`CookShaderLibraries`)의 시험·도구용으로 남는다).
  - 재사용은 한 방향이다: AssetCooker가 이전 매니페스트의 같은 (종류, 이름, 형식 id) 항목 cookKey를 단계에 주고, 같으면 단계는 다시 만들지 않는다고 답한다(단계가 매니페스트를 읽지 않는다).
  - 경로는 `AppendCookedArtifactPath`로 얻는다(쿠킹 루트 아래 경로 규칙은 AssetCooker 하나).
  - TargetProfile 모델과 `ReadHostTargetProfile`은 새 Developer 모듈 `TargetProfiles`에 둔다. `asset-cook`·`shader-compile`(SC13 기본 형식 목록, SSD24)·공급자(SSD16)가 같은 것을 쓴다.
  - `asset-cook --engine` 잠금은 `Engine/Saved/Locks/Cooked-<프로필 id>.lock`이다.
- **SSD11 엔진 셰이더 라이브러리 = 프로젝트 쿠킹 산출물.** UE는 엔진 전역 셰이더 라이브러리("Global")를 프로젝트 쿠킹이 만들고 런타임이 프로젝트 콘텐츠 디렉터리에서 연다(`Runtime/RenderCore/Private/ShaderLibrary/ShaderCodeLibrary.cpp:2272`). Rocks도 같게 한다. 위 서술의 "설치물 단계의 `shader-compile --engine`"은 버린다(근거: 라이브러리를 매니페스트·게시·부분 쿠킹 재사용 규칙 하나로 다루고, 등록된 셰이더 타입 전체를 아는 곳이 쿠커다).
  - `asset-cook --profile=<id>`는 에셋 쿠킹 뒤 **비에셋 산출물 단계**(AssetCooker의 훅 — 에셋 세션이 만든다)에서 `CookShaderLibraries`를 부른다: 프로필 `shaderFormats`의 형식 전체(중복 제거) × 등록된 셰이더 타입 × 정점 입력 타입 × 퍼뮤테이션(12.2.1절 SSD5 결합). 소속이 `Engine`·`EnginePlugin`인 타입은 라이브러리 `Engine`, `Project`·`ProjectPlugin`인 타입은 라이브러리 `Project`로 간다(UE의 Global·프로젝트·플러그인 라이브러리 구분 가운데 플러그인 분할은 후속). 머티리얼 셰이더 항목은 머티리얼 쿠킹 단계가 `Project` 라이브러리에 더한다(머티리얼 시스템). 이 호스트가 만들 수 없는 형식(macOS의 DXBC 등)은 `UnsupportedCapability` 진단으로 쿠킹 실패다.
  - 엔진만 쓰는 실행(Sandbox·엔진 시험·설치 SDK의 엔진 전용 실행)은 `asset-cook --engine --profile=<id>`로 프로젝트 없이 `Engine` 라이브러리만 `Engine/Saved/Cooked/<Profile>/`에 쓴다(너겟 root는 엔진 폴더 — `ResolveShaderNuggetRoot`). 설치 SDK 패키징은 그 결과를 복사한다. 빌드 그래프 action이 아니라는 위 서술의 근거는 그대로다.
  - 구현 위치: `Developer/ShaderCompiler`의 `CookShaderLibraries(요청)`(등록부·형식 사실·관리자·출력 디렉터리 → 라이브러리 파일과 보고)다. job 실패(컴파일 오류·상한)는 에셋 오류처럼 모아 보고하고 게시하지 않는다(AssetSystem 10절).
  - 개발 전용 퍼뮤테이션(7.2절 12항)은 CookSettings `cookDevelopmentShaders`(bool, 기본 false)가 참일 때만 라이브러리에 넣는다.
- **SSD12 파일 이름과 게시.** `<쿠킹 루트>/Shaders/<라이브러리 이름>-<형식 id>-<파일 SHA-256 앞 16 hex>.rshl`(내용 주소 — 게시된 파일을 덮어쓰지 않는다). 게시는 패키지와 같다: staging → no-replace `Move`(같은 이름이 있으면 크기·SHA-256 대조, 다르면 `CorruptEntry` 경고 + `Replace` — AssetSystem 10.2절 10항과 같은 코드) → `SyncDirectory` → 매니페스트 `Replace`(전환점은 매니페스트 하나). 전체 쿠킹은 새 매니페스트가 가리키지 않는 `Shaders/` 파일을 지우고 부분 쿠킹은 지우지 않는다.
- **SSD13 매니페스트 v2.** AssetSystem 10.1절 매니페스트 v1을 형식 버전 2로 올린다(v1 reader는 두지 않는다 — 하위호환 없음).
  - 머리에 쿠킹한 프로필의 `graphicsBackends` 순서와 `shaderFormats`(backend별 형식 순서)를 더한다. 런타임 형식 선택(SSD15)이 실행 파일 쪽 설정이 아니라 쿠킹된 결과를 기준으로 고르게 하기 위해서다.
  - **비에셋 산출물 표**: 항목은 종류(u8, `ShaderLibrary` = 1), 이름(`Engine`·`Project`), 형식 id, 상대 경로, 크기, 파일 SHA-256, cookKey. 표는 (종류, 이름, 형식 id) 오름차순이고 중복·경로 규칙 위반은 매니페스트 손상(AssetSystem 10.1절 판정 방식)이다.
  - 라이브러리 cookKey = SHA-256(공통 머리 `{ShaderLibraryCooker Guid, 버전}` + 라이브러리 형식 버전 + 형식 정의 SHA-256 + 셰이더 타입 집합 SHA-256 + 항목 표(키·서명·기능 비트·개발 전용) + 항목 코드의 job 키 정렬 목록).
  - 부분 쿠킹 재사용은 AssetSystem 10.2절 8항 규칙 그대로다(cookKey가 같고 파일 크기·SHA-256이 매니페스트와 같을 때만 그 파일 byte를 쓴다). 셰이더 단계는 job 키 계산까지는 늘 한다(너겟 적중이면 컴파일하지 않는다).
- **SSD14 런타임 열기.** 로더 시작(AssetSystem 11절)이 매니페스트를 연 뒤 RenderBase 셰이더 집합(9.2절)이 실행 장치의 형식(SSD15)에 맞는 `Engine`·`Project` 라이브러리 항목을 매니페스트에서 찾아 연다. 파일 크기·SHA-256이 매니페스트와 다르면 `CorruptPackage`(AssetSystem `CheckCookedPackageFile`과 같은 코드), 항목이 없으면 `IncompatibleCook`이다. 엔진 전용 실행은 `asset.cooked_root`가 엔진 쿠킹 루트를 가리킨다.
- **SSD15 런타임 형식 선택.** 매니페스트 머리의 `graphicsBackends` 순서대로 backend를 시도하고, backend마다 매니페스트 머리의 `shaderFormats[backend]` 앞에서부터 처음으로 다음을 모두 만족하는 형식을 고른다: (1) 그 형식의 `Engine` 라이브러리 항목이 있다, (2) 형식 사실의 `limits`가 장치 한도 안이다(7.2절 4항 대조 규칙), (3) 형식 사실의 `deviceRequirements`를 장치가 채운다. 없으면 그 backend는 `Unsupported`이고 다음 backend를 시도한다. (3)을 위해 Graphics `DeviceCaps`가 장치 요구 비트를 알아야 한다(RenderBase GX1 — 14절). 매니페스트 없이 공급자로 도는 실행은 호스트 플랫폼 TargetProfile(SSD24)의 순서로 같은 규칙을 쓰고 (1)은 공급자가 그 형식을 만들 수 있는지로 대신한다.
- **SSD16 Development 필요 시 컴파일(SX14 공급자).** 쿠킹 라이브러리가 없는 형식(`GLES31_PC`, 개발 트리에서 쿠킹하지 않고 돌리는 실행)은 RenderBase 셰이더 공급자 interface를 Development 실행 파일이 `ShaderCompiler` 구현으로 채운다: 등록부 열거(12.2.1절 SSD6) → 관리자 `Compile`(일괄, 너겟 캐시) → **메모리 라이브러리**(12.2.1절 SSD7 v1 byte를 메모리에서 만든다 — 런타임 경로를 하나로 둔다) → 셰이더 집합 공급 완료. 같은 형식·같은 등록부·같은 `cookDevelopmentShaders`면 메모리 라이브러리 byte는 쿠킹 라이브러리 byte와 같다(완료 기준). 등록 뒤에 생긴 조합은 `CompileNow`(SC12). Release 실행 파일에는 공급자가 없다(9절). RenderBase SX14가 "SH1 산출물"로 적은 공급자 interface는 SH2e에서 만든다(14절).
  - **개발 전용 항목의 실패.** 완결성 검사는 개발 전용 항목을 빼고 한다(12.2.1절 SSD8). 개발 전용 항목을 공급자가 만들지 못하면(컴파일 실패·도구 없음·Release 구성) 그 항목 조회는 `Unsupported`이고 호출자는 진단 없는 퍼뮤테이션으로 대체한다(7.2절 12항). 시작은 실패하지 않는다.
- **SSD17 시험 루트.** `/Plugin/EngineShaderTests`(개정 29)는 쿠킹에 넣지 않으므로 그 셰이더 타입은 시험 runner가 자기 등록부에만 등록하고 공급자로 컴파일한다.
- 완료 기준(위에 더한다): TargetProfile 새 칸의 검증 음성 대조(`shaderFormats` Replace 병합 포함), 쿠킹 → 매니페스트 v2(머리 순서 포함) → 런타임 열기 왕복(D3D12·D3D11·Vulkan, 프로젝트 루트와 엔진 전용 루트), 부분 쿠킹의 라이브러리 재사용과 cookKey 변화 시 재생성, 형식 선택(상한·장치 요구·없는 라이브러리·매니페스트 순서), 공급자 경로(`GLES31_PC` 공급 완료, Release 구성에서 `UnsupportedCapability`), **같은 형식에서 공급자 메모리 라이브러리 byte = 쿠킹 라이브러리 byte**, 매니페스트 v1 거부, `cookDevelopmentShaders` 거짓이면 개발 전용 항목이 없음, 개발 전용 항목 공급 실패 시 시작 성공·조회 `Unsupported`.
