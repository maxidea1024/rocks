# 셰이더 시스템 설계 — 16. 이후 단계(선행 작업 뒤)

[목차](README.ko.md)

## 16. 이후 단계(선행 작업 뒤)

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| SH0 | 도구 스파이크: 지금의 내장 샘플 셰이더(`Programs/Sandbox/Private/SampleRenderer.cpp`의 HLSL)를 DXIL·SPIR-V·DXBC·MSL·ESSL로 만들고 검증 도구를 돌린다. Windows·macOS 양쪽에서 실행 | P3·P4·P10 | 형식별 산출, `spirv-val` 통과, DXIL 서명 hash 검사(macOS DXC 포함, S1), FXC 고정본 로드, 리플렉션 덤프, `ROCKS_IMAGE_FORMAT`의 DXC 지원 확인, S2·S3·S6·S8·S9·S10·S12(데스크톱)·S13·S14 측정. 남은 것(2026-11-20): macOS 부분(S1·S3 호스트 사이·S9)과 기계 하나로 못 재는 S2·S3·S12 일부(5.4절 끝) |
| SH1 | 소스 트리·가상 경로·`Platform.rsh`·전처리기(결정 6)·ShaderCompileWorker(P1, 11절)·두 층 키(P5)·에셋 너겟·`shader-compile` 명령·Development 필요 시 컴파일. 설계·하위 단계 SH1a–SH1e: 11.1절(개정 23) | SH0·P1·P5(A2·A3·P6a)·결정 6 | 같은 입력 같은 바이트, 빈/찬 캐시 동일, 워커 강제 종료 보고, 부모 종료 시 워커와 그 자식 종료, 형식 버전 불일치 진단, 도구 조회, 전처리 정합성(측정 S7) |
| SH2 | 리플렉션 정규화와 워커 규칙(7.2절)·ShaderCore와 셰이더 타입·퍼뮤테이션(P14)·쿠킹 라이브러리와 매니페스트 v2(P6b)·Graphics API의 쿠킹 코드 수신·RB3 셰이더 집합·런타임 컴파일러 제거·GLES 미리보기 Release 제외·샘플 렌더러 전환. 설계와 하위 단계 SH2a–SH2g: 16.1절(개정 31) | SH1·P2·P4·P9·P11·A0-G·결정 2·3·4·5·9 | 16.1절 완료 기준 |
| SH3 | 파라미터 구조체(P7)·생성 HLSL·오프셋 시험 | SH2·P7 | 7.3절 (a)·(f)를 생성 구조체로 |
| SH4 | 감시 재컴파일(`DirectoryWatch`)·오류 위치·디버그 심볼·PSO 교체 | SH2 | 저장 후 10초 안 반영(계획 I5) |
| SH5 | MetalGraphics(D3D11의 쿠킹 셰이더 전환은 개정 32에서 SH2로 옮겼다) | SH2·P12·P15 | Metal이 D3D12와 허용 오차 안 일치, 7.3절 (c)·(e)·(g)의 Metal 부분 |
| SH6 | Android: `GLES31_ANDROID` 기기 실행, `VULKAN_ANDROID` 형식과 VulkanGraphics Android | SH2·P13·P11 | Android(실기기 또는 S11 에뮬레이터)에서 D3D12 기준 이미지와 허용 오차 안 일치, 7.3절 (c)·(i)의 Android 부분, S12(기기) |

### 16.1 SH2 설계(개정 31·32, 2026-11-24)

SH2는 쿠킹된 셰이더로 그리는 첫 단계다. 위 표의 SH2 행과 결정 9를 다시 적고, P14(12.2.1절)·P6b(12.3.1절)·RenderBase 결정 R8(가)·SX4–SX14·엔진 셰이더 SLR3·SLR12를 한 범위로 묶는다. 결정은 권장안으로 확정했다(SSD18–SSD23. 개정 32는 교차 검증 반영).

**범위(위 표의 SH2 행을 대신한다).**

- **워커 정규화(7.2절 3–9·12항).** 리플렉션 → `ShaderBindingLayout`(값 1 BindingLayout), Vulkan set·binding 재배치와 SPIR-V 입출력 location 재작성(`spirv-val` 재검증), Metal 인덱스 배정, D3D11 SPIRV-Cross 경로의 register 재배정과 `ATTRIBUTEn` 유지, GLES 상수 버퍼 에뮬레이션 텍스트 재작성·복사 표(값 2 GlesCopyTable)·binding 재매김·combined sampler 이름, 상수 버퍼 오프셋 검사(5항)·job 형식 상한 검사(4항)·시맨틱 규칙(7항)·짝 규칙과 공유 샘플러(9항)·PS 진단 버퍼 예외(12항). 처리기 버전을 모두 2로 올린다(출력 의미가 바뀐다 — 형식 정의 `processor.version`도 함께).
- **ShaderCore와 P14(12.2.1절).** 공용 타입 이동, 셰이더 타입 등록부(모듈 수명 객체 등록·소속)·퍼뮤테이션·형식 사실·라이브러리 v1 읽기·쓰기·`ShaderBindingLayout`·GLES 복사 표 형식·손으로 쓴 레이아웃 표, 엔진 컴파일 프로필 폐지(12.3.1절 SSD24).
- **P6b(12.3.1절).** TargetProfile 새 칸·매니페스트 v2(에셋 세션), `CookShaderLibraries`와 비에셋 산출물 훅·`--engine`·`cookDevelopmentShaders`, 런타임 열기·형식 선택.
- **Graphics API(결정 9·SSD18).** backend별 원문 `ShaderSource{hlsl, glsl}`와 개발용 컴파일 훅 `ShaderSourceCompiler`를 지운다. 새 API: `ShaderCodeKind`(Dxil·Dxbc·Spirv·Essl·Metallib), 장치 셰이더 생성 `Device::CreateShader(ShaderCodeDesc{kind, 단계, 코드 byte, BindingLayout byte, GlesCopyTable byte})`, 파이프라인은 셰이더 handle과 두 단계 layout을 합친 파이프라인 layout으로 만든다(D3D12 root signature를 파이프라인 layout마다 캐시 — 7.2절 4항, D3D11 단계별 슬롯, Vulkan pipeline layout set = 단계, GLES program과 바인딩 머리). **D3D12·D3D11·Vulkan·OpenGL ES 네 backend 모두** `CreateShader`로 바꾼다(D3D11은 SH1e의 FXC 경로로 쿠킹된 DXBC를 받는다). 장치는 자기 backend의 `ShaderCodeKind`만 받는다(다르면 `InvalidArgument`). Graphics는 ShaderCore에 의존해 `ShaderBindingLayout`·GLES 복사 표를 읽는다(12.2.1절 SSD1).
- **런타임 컴파일러 제거(결정 9).** D3D12Graphics·D3D11Graphics의 `D3DCompile`(Release 예외 `release-shader-compiler`의 D3D12 항과 D3D11 `until SH5` 항을 모두 지운다), OpenGLGraphics의 손으로 쓴 GLSL 원문, ShaderFormatVulkan의 Graphics 컴파일 훅(`VulkanShaderCompiler`).
- **GLES 미리보기 Release 제외.** Release Windows 실행 파일이 OpenGLGraphics를 링크하지 않게 한다(Release 예외 `release-windows-opengl until SH2` 삭제 — 9절 정책이 그대로 검사한다). 미리보기는 Development 공급자(SSD16)로 `GLES31_PC`를 받는다.
- **RenderBase RB3 셰이더 집합(R8 (가)·SX7·SX10·SX11·SX14).** 셰이더 집합(라이브러리 열기·완결성 검사(개발 전용 항목 제외)·서명 검사·타입 집합 검사·첫 사용 생성·해시로 찾는 `TypedShader<T>`·공급자 interface)과 GX3 가운데 속성 없는 파이프라인·MRT 블렌드·깊이(R8 (가)).
- **RenderBase RB4의 SH2분(R8 (가)).** 바인딩 지도(9.3절)·상수 블록(9.4절)·파이프라인 캐시를 손으로 쓴 레이아웃 표(7.2절 5항)로 만든다. 생성 layout은 SH3다. compute 파이프라인·스텐실·다중 stream은 GX3로 SH2 뒤다.
- **샘플 렌더러 전환(SSD19).** Sandbox 샘플 렌더러 셰이더를 `.rsf`로 옮겨 셰이더 타입으로 등록한다. 엔진 셰이더가 아니므로 `Engine/Shaders`에 두지 않고(엔진 계층 원칙) Sandbox의 셰이더 폴더를 `/Plugin/Sandbox`로 mount한다. Sandbox는 Development 공급자로 받는다.

**하위 단계(각각 첫 검증 1회 뒤 병합, 순서 SSD20).**

| 단계 | 범위 | 시험 |
|---|---|---|
| SH2a1 | ShaderCore 모듈: 공용 형식 타입 이동(12.2.1절 SSD1), 가상 경로 문법 이동(SC2 — 셰이더 타입 경로 검사와 ShaderCompiler가 같은 규칙을 쓴다), 셰이더 타입 기술·`nameHash`·소속 표지(SSD2), 등록부·`Freeze`(SSD3), 모듈 수명 객체 등록 도우미(호스트 서비스 `ShaderTypeRegistry`), 퍼뮤테이션 차원·ID(SSD4) | 12.2.1절 완료 기준의 등록·퍼뮤테이션 부분 |
| SH2a2 | ShaderCore 나머지: 형식 사실·라이브러리 v1·`ShaderBindingLayout`·GLES 복사 표 형식·손으로 쓴 레이아웃 표·셰이더 전용 정렬 타입. `ShaderCompiler`의 job 열거와 `CookShaderLibraries`(파일 쓰기까지), 엔진 컴파일 프로필·키 7행 제거 | 12.2.1절 완료 기준의 나머지 |
| SH2b | 워커 정규화 1: `ShaderBindingLayout`(D3D·SPIR-V 리플렉션), Vulkan 재배치·location 재작성, D3D11 SPIRV-Cross 재배정, Metal 인덱스, 오프셋·상한·시맨틱·짝·공유 샘플러·PS 진단 버퍼 규칙, 처리기 버전 2 | 7.3절 (a)·(b)·(e 컴파일)·(f)·(g)·(h 음성)·(j 컴파일)·(k) |
| SH2c | 워커 정규화 2(GLES): 상수 버퍼 에뮬레이션 텍스트 재작성·복사 표·binding 재매김·combined sampler 이름·모바일 상한 | 7.3절 (i 컴파일)·GLES 부분의 (a)·(b)·(h) |
| SH2e | 공급자와 메모리 라이브러리(SSD16), Graphics API 전환(4 backend `CreateShader`·파이프라인 layout), RB3 셰이더 집합, RB4 SH2분, 런타임 컴파일러 제거 | RenderBase SX10, 7.3절 (c)·(d)·(j 장치) |
| SH2f | 샘플 렌더러 전환·Sandbox self-test(D3D12·D3D11·Vulkan·GLES 미리보기)·Release 제외 정책·측정 S5 | 아래 완료 기준 1·4·5 |
| SH2d | P6b: `CookShaderLibraries`를 비에셋 산출물 훅에 붙임·`--engine`·`cookDevelopmentShaders`, 런타임 열기·형식 선택. 선행: 에셋 세션의 TargetProfile 칸·매니페스트 v2·훅 | 12.3.1절 완료 기준 |
| SH2g(선택) | 엔진 셰이더 SL0(SSD21) | SH2g 완료 기준(아래) |

**완료 기준(위 표 SH2 행을 대신한다).**

1. Sandbox self-test에서 D3D12·D3D11·Vulkan·GLES 미리보기가 공급자 셰이더로 허용 오차 안 일치(Vulkan·GL은 CI에서 NOT RUN일 수 있다 — 로컬 결과 기록).
2. EngineTests가 시험 등록부로 `CookShaderLibraries`를 직접 불러 라이브러리 파일을 만들고, 그 라이브러리로 D3D12(WARP 포함)·Vulkan 장치에서 그린다(쿠킹 경로 확인 — `asset-cook` 연결은 SH2d가 따로 시험한다). 같은 형식에서 공급자 메모리 라이브러리 byte = 그 파일 byte.
3. 7.3절의 SH2 시험((a)–(k)의 SH2 부분), 12.2.1·12.3.1절 완료 기준, RenderBase SX10(첫 사용 생성 경쟁의 게시 1회 — 명시 gate, 완결성·서명 음성 대조, 파이프라인 캐시 적중 계수 — RB4 SH2분).
4. 행렬 옵션·앞면 매핑 기록(측정 S5), A0-G 범위를 Vulkan이 받음(G3).
5. Release 구성에 셰이더 컴파일러·GLES 미리보기 없음(9절 정책, `release-shader-compiler`의 D3D12·D3D11 항과 `release-windows-opengl` 예외 삭제).

**SH2g 완료 기준(선택, SLR3).** 엔진 셰이더 7.7절 1–3·5(SL0 컴파일 행렬 — 형식은 호스트 플랫폼 TargetProfile의 형식(SSD24)·CPU 대조·정수 golden·coverage 사유). SH2 완료 기준에 들지 않는다.

**결정.**

- **SSD18 Graphics API 모양.** 장치 셰이더를 파이프라인과 따로 만든다(UE `RHICreateVertexShader`·`RHICreatePixelShader` 뒤 PSO 초기화자가 handle을 받는 구조, `Runtime/RHI/Public/RHIResources.h:4440`). 셰이더 집합의 첫 사용 생성(RenderBase 9.2절)이 이 단위를 요구하기 때문이다. 버린 대안: `PipelineDesc`가 코드 byte를 직접 받기(같은 셰이더를 쓰는 파이프라인마다 장치 셰이더를 다시 만든다).
- **SSD19 샘플 셰이더 위치.** Sandbox 원문은 `Programs/Sandbox/Shaders`를 `/Plugin/Sandbox`로 mount한다(SC2 `/Plugin/<이름>` 규칙 — 새 루트를 만들지 않는다). 소속은 `EnginePlugin`(루트 `/Plugin/Sandbox`)이지만 Sandbox 모듈만 등록하므로 쿠커의 모듈 집합에 들지 않고 쿠킹되지 않는다(Sandbox는 프로젝트가 없는 엔진 전용 실행이고 공급자로 받는다). 버린 대안: `Engine/Shaders`에 두기(엔진 계층에 샘플을 두지 않는다).
- **SSD20 하위 단계 순서.** SH2a1 → SH2a2 → SH2b → SH2c → SH2e → SH2f → SH2d → SH2g(선택). SH2b·SH2c는 런타임 변경 없이 쿠킹 산출물만 바꾸므로 먼저 하고, Graphics API 전환(SH2e)은 공급자·메모리 라이브러리가 준비된 뒤 한 번에 한다(네 backend self-test를 같은 변경에서 통과 — 결정 9). 쿠킹·매니페스트 연결(SH2d)은 에셋 세션의 AssetSystem 변경(TargetProfile·매니페스트 v2·훅)이 선행이라 마지막이다. 그 전에도 SH2e·SH2f는 공급자 경로와 완료 기준 2의 직접 호출로 쿠킹 라이브러리 형식을 확인한다.
- **SSD21 SL0와 SH2(SLR3).** 엔진 셰이더 SL0(순수 함수 공통 라이브러리 1차, 엔진 셰이더 7장)는 SH2의 **선택 하위 단계 SH2g**로 두고 완료 기준도 SH2g 자체 기준으로 둔다. SH2e(쿠킹 셰이더로 VS·PS 그리기)와 A0-G 렌더 타깃·readback 뒤에 할 수 있다. 하지 않으면 엔진 셰이더 SL2에서 같은 범위를 한다(엔진 셰이더 6장). SL0의 원문 작성 범위(A01–A04 1차 수십 개 파일)가 SH2 규모와 맞먹어 SH2 완료를 막지 않게 했다. 시험 셰이더는 `/Plugin/EngineShaderTests`(SSD17)로 컴파일한다.
- **SSD22 PS 진단 버퍼(SLR12).** 7.2절 12항대로 어휘·예약 위치·워커 예외를 SH2b에 넣는다. 런타임 버퍼 처리는 엔진 셰이더 SL2다.
- **SSD23 공급자 시기(SX14).** RenderBase SX14는 공급자 interface를 SH1 산출물로 적었으나 SH1은 컴파일 쪽(관리자·`CompileNow`)만 만들었다. interface와 Development 등록은 SH2e에서 셰이더 집합과 함께 만든다.

**범위 충돌(다른 세션·문서와 조정 — 14절 표).**

- **AssetSystem(에셋 세션).** TargetProfile `graphicsBackend` → `graphicsBackends`·`shaderFormats`(SSD10), 매니페스트 v2 머리 순서와 비에셋 산출물 표(SSD13), 비에셋 산출물 단계 훅·`--engine`·`cookDevelopmentShaders`(SSD11). 에셋 세션이 AssetSystem 문서를 먼저 고치고 구현한다. 셰이더 쪽은 `CookShaderLibraries`와 훅 구현만 한다(SH2d).
- **엔진 라이브러리 생산 시점.** 12.3절 원안(설치물 단계 `shader-compile --engine`)을 UE대로 프로젝트 쿠킹 산출물로 바꿨다(SSD11). RenderBase 9.2절 서술과 계획(EIP 9.6) 문구를 맞춰야 한다.
- **렌더 세션.** Graphics API 전환(SSD18, 네 backend)과 RB3·RB4 SH2분·GX3 일부(R8 (가))는 backend와 RenderBase 모듈을 고친다. `DeviceCaps`의 장치 요구 비트(SSD15)는 GX1 쪽 일이다. 구현 주체와 순서를 렌더 세션과 정해야 한다.
- **Commandlet 링크·모듈 적재.** 쿠커가 셰이더 타입을 정의하는 Runtime 모듈(렌더러)을 링크하고, 프로젝트·플러그인 타입은 BuildTargets BS4·모듈 로더 ML5b 뒤에 쿠킹한다(12.2.1절 SSD3). Commandlet 링크 규칙은 빌드 세션에 요청한다.

**SH2a1 구현 메모(개정 34, 2026-11-24).** 12.2.1절 SSD1–SSD4의 등록·퍼뮤테이션 부분을 구현했다.

- 모듈: `Engine/Source/Runtime/ShaderCore`(공개 의존 Core, 비공개 의존 ModuleLoader). `ShaderCompilerCommon`·`ShaderCompiler`가 공개 의존으로 쓴다.
- 공용 형식 어휘(`ShaderCore/ShaderFormatTypes.h`): `ShaderBackend`·`ShaderStageBits`·`ShaderFeatureBits`(`AllShaderFeatures`·런타임 확인 묶음 포함)·`ShaderDeviceRequirementBits`·`ShaderVersion`·`ShaderFormatLimits`·`NoShaderLimit`·`ShaderBackendName`을 `ShaderFormatDefinition.h`에서 옮겼다(사본 없음, 같은 namespace). JSON 어휘 철자 표(읽기용)는 `ShaderCompilerCommon`에 남는다.
- 가상 경로 문법(`ShaderCore/ShaderVirtualPath.h`): SC2 규칙과 상수(`MaxShaderVirtualPathBytes`·가상 루트 이름)를 옮겨 `CheckShaderVirtualPath`(루트·플러그인 이름·확장자·Shared 판정을 함께 돌려준다)·`CheckShaderVirtualDirectory`로 두었다. `ShaderCompiler`의 `ValidateShaderVirtualPath`·`ValidateShaderVirtualDirectory`는 같은 문구로 감싼다(규칙 하나).
- 셰이더 타입 기술(`ShaderCore/ShaderType.h`): 정의는 매크로 대신 `constexpr` 함수 `MakeShaderTypeDesc(이름 상수, 경로, 엔트리, 단계, 소속)`로 만든다(매크로가 할 일이 없다). 공개 헤더용 `ROCKS_SHADER_TYPE_NAME(변수, "이름")`이 이름·해시 상수(`ShaderTypeName`)를 만든다. 해시는 CP7대로 `HashStringBytesIgnoringAsciiCase`라 대소문자만 다른 이름은 충돌로 거부된다. 손으로 쓴 레이아웃 표 칸은 그 형식을 정하는 SH2a2에서 더한다. 거르기 함수 서명은 형식 사실(SH2a2)을 전방 선언으로 받는다.
- 등록부(`ShaderCore/ShaderTypeRegistry.h`): 호스트 서비스(`ROCKS_DECLARE_HOST_SERVICE`)다. 검사 위반·중복·해시 충돌은 `InvalidArgument`이고 목록 하나가 실패하면 그 목록 전체를 등록하지 않는다(`LastFailure`에 타입 이름과 영어 이유). `Freeze` 뒤 등록은 `ScopeClosed`(등록 창이 닫힘)다. `Find`는 `Freeze` 전 선형·뒤 이진 탐색이다. `Unregister`는 모듈 종료 단계에서 `Freeze` 뒤에도 된다. 타입 집합 SHA(`TypeSetDigest`)는 정렬된 해시를 little-endian 8 byte로 이은 SHA-256이다.
- 모듈 등록 도우미 `ShaderTypeModuleRegistration`: 수명 객체 `Startup`에서 호스트 서비스의 등록부에 등록하고 등록한 동안 모듈 lease를 잡는다(RenderBase GPU trace 등록과 같은 방식). 등록부가 없는 호스트에서는 아무 일도 하지 않는다. 시험할 수 있게 `Attach(HostServices&, ModuleLease)`를 공개했다(`ModuleStartContext`는 로더만 만든다). 호스트 쪽(엔진 부트가 등록부를 올리고 렌더 lane 전에 `Freeze`)은 셰이더 집합과 함께 SH2e에서 연결한다.
- 퍼뮤테이션(`ShaderCore/ShaderPermutation.h`): 차원 수 상한 16, 도메인 크기 상한 65,536. 차원 정의 이름은 식별자이고 `ROCKS_`·`__` 접두를 거부한다(SC10과 같다).
- 시험(EngineTests `ShaderCore_*` 7건, `--filter Shader` 93건 통과): 가상 경로 문법 양성·음성과 길이 초과, 형식 어휘 이름, 퍼뮤테이션 UE 순서 golden(희소 정수의 첫 값이 가장 큰 value id)·전체 ID 왕복·끝에 더한 차원의 ID 보존·빈 도메인, 도메인 검사 음성 대조와 상한 경계(256 × 256)·차원 수 상한·개발 전용 판정, 등록 음성 대조 14종·중복·대소문자 충돌·null·목록 원자성, `Freeze`의 정렬·등록 순서 무관 SHA·`ScopeClosed`·해제, 모듈 도우미(등록부 없음·있음·두 번 붙기·다른 모듈의 중복 실패·소멸자 해제).
- 남은 정합 문제(SH2a2에서 정한다): RenderBase CP7은 "저장·쿠킹 형식은 이름 문자열과 SHA-256을 쓰고 해시는 실행 중 조회용"이라 적었으나 SSD7 라이브러리 항목은 `nameHash`를 담는다. 라이브러리에 이름(이름 표)을 키로 두고 해시는 열 때 계산할지, 해시 함수를 형식의 일부로 고정할지 SH2a2 설계에서 정한다.

SH0 진척(2026-11-17, `feature/shader-tools`, Windows 부분 구현):
- [x] 스파이크 `Programs/ShaderToolSpike`(Program 타깃, `ShaderToolSpike --out <디렉터리> [--sm 6_0]`). 도구는 P4 `ShaderToolSet`으로 사용자 저장소에서 연다. 산출물은 `<out>` 아래에 두고 저장소에는 이 요약만 남긴다. `manifest.json`(산출물 이름·크기·SHA-256, 결정성 비교용)과 `report.json`(검사·측정값)을 쓴다.
- [x] 형식별 산출(내장 Renderer VS·PS): DXIL `vs_6_0`·`ps_6_0`(`-Qstrip_reflect`), SPIR-V(`-spirv -fspv-target-env=vulkan1.1 -fvk-use-dx-position-w`), DXBC(FXC 고정본 `vs_5_0`·`ps_5_0`), MSL 2.1 macOS, ESSL `#version 320 es`의 `GLES31_ANDROID`(`fixup_clipspace`·`flip_vert_y` 켬)와 `GLES31_PC`(끔), HLSL 5.0. HLSL 5.0 출력은 FXC로 다시 컴파일된다(D3D11 대체 경로 확인).
- [x] 검증: `spirv-val`(vulkan1.1) 통과, DXIL container digest 검사 통과(0·preview 우회 값이면 실패 처리), 리플렉션 덤프(DXIL `ID3D12ShaderReflection`, SPIR-V SPIRV-Reflect: 바인딩·상수 버퍼 오프셋·입출력 시맨틱과 location).
- [x] `ROCKS_IMAGE_FORMAT`: DXC 1.9.2609가 `[[vk::image_format]]`을 지원한다. `rgba8`·`rgba8snorm`·`rgba16f`·`r11g11b10f`·`r32f`·`r32ui`·`unknown`이 SPIR-V 형식 그대로 나오고, 지정이 없으면 `float4`에서 `Rgba32f`로 추론한다. DXIL은 attribute를 무시하며 `-Wignored-attributes` 경고를 낸다. ESSL 320은 `r11g11b10f`를 받지 않고(SPIRV-Cross 오류 "image format not supported in ES profile"), `unknown`은 형식 한정자 없는 image를 낸다.
- [x] SPIRV-Cross 실수 출력(10절): 극값 fixture 8개(최소·최대 비정규수, 최소 정규수, 최대 유한값, 0.1, 1/3, 1+ulp, 음의 비정규수)의 ESSL·MSL 철자가 C locale에서 독립 계산한 `%.32g`(정확한 반올림)와 같고 원래 bit로 다시 읽힌다. 쉼표 소수점 locale(`de-DE`)에서도 출력 byte가 같다(`fixup_radix_point`). Windows UCRT 기준이며 다른 CRT(macOS)는 미측정이다.
- [x] 측정 S2·S3(Windows 부분)·S6·S8·S12(데스크톱): 5.4절 결과 열.
- 규약 검토 자료(리플렉션 덤프에서 확인한 것, 결정 아님):
  - SPIR-V에서 원문의 `register(t0)`·`register(s0)`가 같은 set 0·binding 0이 된다(texture와 sampler가 한 binding을 나눠 Vulkan descriptor layout으로 쓸 수 없다). 7.2절 4항 워커 재배치가 필요하다는 근거다. DXIL은 종류별 register 공간이라 겹치지 않는다.
  - 상수 버퍼 `DrawConstants`는 DXIL·SPIR-V 모두 80 byte, `transform` 0·`color` 64다. SPIRV-Cross ESSL은 `layout(row_major) mat4`와 `vec * M`, HLSL 5.0은 `column_major`로 낸다(행렬 옵션은 S5, 7.2절 6항). 이 측정은 2026-11-17 행 벡터 전환 전의 열 우선 원문으로 했다.
  - SPIR-V 입출력은 location 0부터 선언 순서이고, DXC는 `-fspv-reflect` 없이는 시맨틱 decoration을 내지 않아 시맨틱은 변수 이름(`in.var.TEXCOORD0`)에만 남는다. SPIRV-Cross HLSL 5.0은 시맨틱을 location 기준 `TEXCOORDn`으로 다시 쓴다(`COLOR0` → `TEXCOORD1`).
  - ESSL의 결합 sampler는 이름이 `_29`처럼 id이고 binding이 없다. 7.2절 GLES binding 재매김과 이름 규칙을 워커가 정해야 한다.
- [x] SH0 남은 측정(2026-11-20, `feature/sh0-rest`): S10(문서 조사, 8절 2항), S13(`TypedBuffer.ps`·`.cs`, 7.2절 4항), S14(`LayerViewport.vs`, 7.2절 7항). 스파이크 산출물은 68개이고 반복 실행의 manifest가 같다. 측정을 위해 P4 SPIRV-Cross 함수 표에 `spvc_compiler_get_combined_image_samplers`·`spvc_compiler_get_name`을 더했고, 스파이크 MSL 출력은 `texture_buffer_native`를 켠다(7.2절 4항 Metal).
  - 규약 검토 자료(결정 아님): `SV_InstanceID`를 쓰는 VS의 ESSL에는 ES에 draw parameters가 없어 SPIRV-Cross가 `uniform int SPIRV_Cross_BaseInstance`(기본값 0)를 더한다. base instance가 늘 0이라(7.2절 7항) 값은 맞고, SH2에서 워커가 SPIRV-Cross `support_nonzero_base_instance`를 꺼 이 uniform을 없앨지 정한다.
- [ ] macOS: S1(macOS DXC 서명), S3 Windows–macOS 사이 byte 비교, S9, macOS CRT의 실수 출력. 개정 29: 절차는 `shader-compile --out`으로 옮겼고(11.1절 E10) 스파이크는 지운다. macOS에서 DXC를 여는 일(ShaderToolLoader `Private/Mac`, DXC는 `WinAdapter.h`)은 macOS 산출물(P3)과 함께 한다.
- [ ] S3의 FMA3 유무·OS 버전 차이와 S12의 다른 GPU 공급자는 기계가 하나라 미측정이다.
