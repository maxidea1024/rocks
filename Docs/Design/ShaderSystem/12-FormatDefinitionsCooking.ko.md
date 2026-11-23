# 셰이더 시스템 설계 — 12. P6a·P14·P6b 형식 정의·셰이더 타입·TargetProfile·쿠킹 매니페스트

[목차](README.ko.md)

## 12. P6a·P14·P6b 형식 정의·셰이더 타입·TargetProfile·쿠킹 매니페스트

엔진 컴파일 프로필은 형식 정의와 함께 P6a(12.1절)가, TargetProfile 필드는 A4가 쓰는 프로필 파일에 P6b(12.3절)가 더한다.

### 12.1 P6a 형식 정의와 엔진 컴파일 프로필

- **형식 정의.** `Engine/Config/ShaderFormats/<id>.json`에 둔다: 이 형식이 쓰는 도구 목록(10절 키 3항의 정적 목록), 언어 버전 기본값, 컴파일러(DXC·FXC·SPIRV-Cross 경로), 셰이더 모델·target env·ESSL 버전, 바인딩 규약 버전, ShaderFormat 처리기 버전, 상한(7.2절 4항)·지원 기능, 생산 호스트 규칙. UE에서는 기능·형식 이름 부분이 `DataDrivenPlatformInfo.ini`의 `[ShaderPlatform]`(`Language`·`MaxFeatureLevel`·`ShaderFormat`·기능 플래그, `Engine/Config/Windows/DataDrivenPlatformInfo.ini:78–139`)에 있고, SM 선택은 ShaderFormat 코드에 있다(`ShaderFormatD3D.cpp:102–150`, FXC·DXC 분기 `D3DShaderCompiler.cpp:1520–1522` `DoesShaderModelRequireDXC`). Rocks는 둘 다 형식 정의에 둔다. 형식 정의 SHA-256이 job 키에 들어간다(10절). 형식 정의에는 결정 2(언어 버전 기본값)·3(셰이더 모델)·5(생산 호스트)가 들어가므로 P6a는 그 결정 뒤다(15절 기한).
- **엔진 컴파일 프로필.** 프로젝트 프로필 없이 하는 컴파일(엔진 내장 라이브러리, Development 필요 시 컴파일)이 쓰는 호스트별 프로필이다. 런타임 backend 선택이 없는 컴파일 전용이라 TargetProfile과 스키마를 나눈다: `Engine/Config/ShaderFormats/Engine-<호스트>.json` = `{ "id", "formats": [...] }`. `Engine-Windows`는 `D3D12_SM6`·`D3D11_SM5`·`VULKAN_SM5`·`GLES31_PC`·`GLES31_ANDROID`, `Engine-MacOS`는 `METAL_SM5`·`GLES31_ANDROID`다. 그 호스트에서 쓰거나 그 호스트가 빌드하는 대상(Android 포함)이 쓰는 형식 가운데 그 호스트가 만들 수 있는 것만 넣는다(10절, 15절 결정 5에 따라 바뀐다). job 키 9항이 이 프로필에서 나온다(프로필 공통 상한은 개정 17에서 폐기, 7.2절 4항). SH1이 쓰므로 P6a에서 만든다.
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

### 12.3 P6b TargetProfile·쿠킹 매니페스트·엔진 내장 라이브러리

- **TargetProfile**(AssetSystem 10절)은 backend와 형식 id 목록만 갖는다. 언어 버전·target env·생산 호스트·장치 요구를 프로필에 두면 출처가 둘이 되기 때문이다. `graphicsBackends`는 런타임이 시도하는 순서다. `shaderFormats`는 backend마다 선호 순서가 있는 형식 목록이며(형식의 backend는 형식 정의가 정한다), 런타임은 장치가 지원하는 첫 형식을 고른다(UE `D3D12TargetedShaderFormats` 같은 backend별 형식 목록, `Runtime/RHI/Private/Windows/WindowsDynamicRHI.cpp:624–626`). 그래서 장치 요구 수준이 다른 형식(2절)이나 결정 3의 D3D12 형식 두 개를 함께 둘 수 있다. 목록에 형식이 없는 backend, 목록에 없는 backend의 형식, 형식 정의의 backend가 키와 다른 형식은 프로필 검증 오류다. backend 이름은 구현 여부와 무관한 고정 어휘다(`D3D11`은 SH5에서 구현된다). 런타임은 구현되지 않았거나 장치가 없는 backend를 `UnsupportedCapability`로 건너뛰고 다음 backend를 시도한다.

```json
"graphicsBackends": ["D3D12", "D3D11", "Vulkan"],
"shaderFormats": { "D3D12": ["D3D12_SM6"], "D3D11": ["D3D11_SM5"], "Vulkan": ["VULKAN_SM5"] }
```

- 엔진 기본 프로필: `Windows`(위), `MacOS`(`Metal`: `METAL_SM5`, 새로 더함), `Android`(`OpenGL`: `GLES31_ANDROID`, SH6에서 `Vulkan`: `VULKAN_ANDROID`를 앞에 더함). 현재 엔진 기본은 `Windows`·`Android`뿐이다(AssetSystem 10절). backend 이름 `OpenGL`은 OpenGL ES backend다(UE `EWindowsRHI::OpenGL`과 같은 이름).
- 엔진 컴파일 프로필은 12.1절(P6a)이 정한다.
- `GLES31_PC`는 출시 프로필과 엔진 내장 라이브러리에 넣지 않는다. Development 실행 파일이 필요할 때 컴파일한다(SH1). UE도 쿠킹 데이터를 쓰는 실행 파일에는 이 플랫폼을 등록하지 않는다(2절). Release Windows 실행 파일은 OpenGLGraphics를 링크하지 않는다(9절 정책).
- **쿠킹 산출물.** (프로필, 형식)마다 셰이더 라이브러리 파일 하나를 만든다. 파일 이름은 패키지처럼 내용 주소(AssetSystem 4.2절 규칙, 형식 id와 내용 SHA-256)라 게시된 파일을 덮어쓰지 않는다. 게시와 정리는 `Packages/`와 같다: staging → no-replace `Move` → `SyncDirectory` → 매니페스트 `Replace`, 전체 쿠킹은 새 매니페스트가 가리키지 않는 라이브러리 파일을 지우고 부분 쿠킹은 지우지 않는다(AssetSystem 10절). UE는 라이브러리(Global·프로젝트·플러그인/청크)×형식마다 `ShaderArchive-<Library>-<Format>-<Platform>.ushaderbytecode`를 쿠킹하고(`Runtime/RenderCore/Private/ShaderLibrary/ShaderCodeLibraryUtilities.cpp:84–96`, `<Format>-<Platform>` 문자열 `ShaderCodeLibrary.cpp:1482–1488`), 배포는 IoStore나 네이티브 라이브러리로 연다(ushaderbytecode는 배포 형식이 아니라는 주석, `ShaderLibraryInstance.cpp:27–43`). Rocks도 라이브러리 파일은 쿠킹 단위이고, 배포 컨테이너로 옮기는 일은 패키징 단계가 맡는다. 라이브러리 분할(플러그인·청크)은 후속이다.
- **엔진 내장 셰이더 라이브러리.** 엔진 셰이더(`Engine/Shaders`)의 라이브러리는 엔진 설치물을 만드는 단계(설치 SDK 내보내기·패키징)에서 `shader-compile --engine` 명령으로 그 호스트의 엔진 프로필 형식(`GLES31_PC` 제외)마다 만든다. 빌드 그래프 action이 아니다: Rocks.Build에는 빌드한 프로그램을 실행하는 action이 없다(action 종류는 Compile·Archive·Link·Copy, `ActionGraph.cs`). 그 호스트가 만들 수 없는 형식은 다른 호스트의 설치물 단계가 만든다(macOS는 DXBC를 만들지 않는다. 계획 §27.9는 호스트가 지원하는 target의 로컬 빌드가 되어야 한다고 요구한다). 설치물에서는 프로젝트·너겟·네트워크 없이 연다(계획 9.6, 5.2절 K3). 개발 트리에서는 Development 필요 시 컴파일이 같은 역할을 한다. UE의 Global 라이브러리에 해당한다. 프로젝트 쿠킹은 프로젝트·플러그인 셰이더 라이브러리만 만든다.
- 시험 전용 플러그인 루트 `/Plugin/EngineShaderTests`(11.1절 SC2, 개정 29 — 엔진 셰이더 SLR2)의 원문은 엔진 내장 라이브러리에 넣지 않는다. 엔진 셰이더 시험 runner만 그 루트를 mount해 Development에서 컴파일한다.
- 쿠킹 매니페스트 v1은 AssetId 단위라서 v2에 "에셋이 아닌 산출물" 항목(이름, 형식 id, 파일 SHA-256·크기, cookKey)을 더한다. 라이브러리 cookKey 입력은 포함된 job 키 정렬 목록과 라이브러리 형식 버전이다. 게시는 기존 쿠킹 게시와 같다(staging → `SyncDirectory` → `Replace`).
- 런타임은 실행 장치의 backend에 맞는 형식의 라이브러리만 연다. 없거나 필요한 셰이더가 빠졌으면 `IncompatibleCook`이다.

완료 기준: 프로필 스키마 검증(모르는 속성 거부, 프로필에 형식 옵션을 적으면 오류, backend·형식 목록 검사), 장치 기능에 따른 형식 선택, 형식 상한 초과가 그 형식 job만의 쿠킹 오류인지, 매니페스트 v2 왕복, 형식이 빠진 쿠킹을 연 런타임의 `IncompatibleCook`, 프로젝트 없이 엔진 내장 라이브러리를 여는 시험.
