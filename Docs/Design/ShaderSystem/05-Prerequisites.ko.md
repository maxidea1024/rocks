# 셰이더 시스템 설계 — 5. 선행 작업

[목차](README.ko.md)

## 5. 선행 작업

### 5.1 목록

| ID | 작업 | 담당 문서 | 상태 | 막는 단계 | 의존 | 상세 |
|---|---|---|---|---|---|---|
| A0-C | SHA-256·JSON·결정적 수학 버전 | CoreDataFoundations | **구현** | — | — | 5.2 |
| A0-P | 파일 감시·원자적 교체·no-replace 이동·시각 설정 | AssetSystem A0-P | **구현**(macOS 미검증) | — | — | 5.2 |
| B1–B8 | 리플렉션(`ROCKS_STRUCT`·identity·배열), B7 툴체인 지문 | ReflectionFoundations | **구현** | — | — | 5.2 |
| A0-G | 형식·차원·swizzle·샘플러 확장 | AssetSystem A0-G | 미착수 | P11, SH2 | — | 5.2 G1–G3 |
| A1 | AssetRuntime, `UnsupportedCapability`, 매니페스트 reader | AssetSystem A1 | **구현** | P6b | A0-O | 5.2 |
| A2 | `ProjectLayout` | AssetSystem A2 | **구현**(A2a `AssetDatabase/ProjectLayout.h`) | P5, SH1 | A1 | 5.2 |
| A3 | 에셋 너겟 | AssetSystem A3 | **구현**(2026-11-23, `Developer/AssetNugget`) | P5 | A2 | 5.2 N1–N3 |
| A4 | Cooker·TargetProfile·쿠킹 매니페스트 | AssetSystem A4 | 미착수 | P6b | A3 | 5.2 K1–K3 |
| P1 | 자식 프로세스·pipe | PlatformLayer | **구현** | SH1 | — | 5.3 |
| P2 | 바인딩 규약(GLES 포함) | 이 문서 | 설계 | SH2 | — | 7절 |
| P3 | ThirdParty Engine 루트·`binary`·`headers` 종류·저장소·오프라인 도구 빌드 | 이 문서 | **구현**(Windows, Release 게시 완료. macOS 산출물 남음) | P4, P9, P12, SH0 | — | 8절 |
| P4 | 도구 표 생성·binary 의존 정책·Release 배제 검사 | 이 문서 | **구현**(Windows. macOS 미검증, 알려진 Release 예외 2건은 SH2까지) | SH0, SH2 | P3, P10 | 9절 |
| P5 | 셰이더 job 키 | 이 문서 | 설계(개정 23, A3 API에 맞춤) — 구현은 SH1b | SH1 | A2, A3, P6a | 10절 |
| P6a | 형식 정의 스키마와 파일, 엔진 컴파일 프로필 | 이 문서 | **구현**(2026-11-17, Windows 시험) | P5, P14, P6b, SH1 | A0-C(JSON), 결정 2·3·5(결정됨) | 12.1절 |
| P6b | TargetProfile 필드·매니페스트 v2·엔진 내장 셰이더 라이브러리 | 이 문서 | 설계 | SH2 | A1, A4, P6a, P14 | 12.3절 |
| P7 | MetaCompiler 셰이더 파라미터 | 이 문서 | 설계 | SH3 | B1–B8 | 13절 |
| P8 | 다른 문서 개정 | 이 문서 | 목록 | 모든 단계의 기준 | 결정별 기한(15절) | 14절 |
| P9 | Vulkan 선행 | 이 문서·[VulkanGraphics](../VulkanGraphics.ko.md) | **구현**(Windows. VulkanGraphics 개정 1, Android는 교차 빌드만) | SH2 | P3, P10 | 5.3 |
| P10 | 동적 라이브러리 API | PlatformLayer | **구현** | P4, P9, P13, SH0, SH2 | — | 5.3 |
| P11 | OpenGL ES 선행(OpenGLGraphics를 UE `OpenGLDrv` 방식으로) | [OpenGL ES backend](../OpenGLESGraphics.ko.md)·이 문서 | **구현**(2026-11-17, Windows 미리보기. Android는 컴파일만, 실행은 P13) | P13, SH2, SH6 | A0-G | 5.3 |
| P12 | macOS 실행 기반(Objective-C++ 빌드·Cocoa 창·Metal layer) | PlatformLayer·Rocks.Build | 미설계 | P15, SH5 | P3 | 5.3 |
| P13 | Android 실행 기반(앱·창·EGL surface·수명·APK) | PlatformLayer | 미설계 | SH6 | P10, P11, AssetSystem APK 읽기(VFS) | 5.3 |
| P14 | 셰이더 타입·퍼뮤테이션·라이브러리 형식 | 이 문서 | 설계 | SH2, P6b | P6a | 12절 |
| P15 | D3D11Graphics·MetalGraphics 설계 | EngineRuntime | D3D11 **구현**(10.2절), Metal 미설계 | SH5 | P12(Metal) | 5.3 |

선행 작업 뒤 첫 단계인 SH0(16절)은 **Windows 부분 구현**(2026-11-17, `feature/shader-tools`, `Programs/ShaderToolSpike`)이다. 형식별 산출·검증·리플렉션 덤프·`ROCKS_IMAGE_FORMAT` 확인과 Windows에서 할 수 있는 측정(S2·S3·S6·S8·S12 데스크톱)을 마쳤고, macOS 측정(S1 macOS DXC 서명, S3 호스트 사이, S9)이 남았다. 결과는 5.4절, 진척은 16절이다.

### 5.2 다른 문서가 맡는 선행 작업과 셰이더 요구

**구현된 것.** 셰이더 시스템은 아래를 그대로 쓰고 같은 기능을 따로 만들지 않는다.

- A0-C: SHA-256(`Core/Source/Runtime/Core/Public/Core/Encoding/Sha256.h:23,40`), JSON(`Core/Text/Json.h`, 결정적 출력), 결정적 수학 버전 `rocks::deterministic::Version`(`DeterministicMath/DeterministicMath.h:31`, 결과 비트가 바뀌면 올리며 너겟 키·cookKey에 들어간다).
- B7: 툴체인 지문·도구 빌드 ID `rocks::build::ToolchainFingerprint()`·`ToolBuildId()`(`Core/Build/BuildInfo.h:24,30`). 정규 텍스트는 컴파일러 버전·컴파일러와 링커 SHA-256·대상 triple·FP 모델이다(ReflectionFoundations 8.1절).
- B4·B5: `ROCKS_STRUCT`(struct 스키마는 클래스와 같은 `TypeSchema`, ReflectionFoundations 4.2절)·identity 명세(`<Module>.rocksids.json`)·배열 필드. P7의 바탕이다.
- A0-P: `Platform/DirectoryWatch.h:88`(SH4), `File::Flush`·`FileSystem::Replace`·no-replace `Move`·`SetModifiedTime`(너겟 기록·GC, 워커 출력).
- Core 직렬화(Archive·Record·RecordList, 워커 메시지), `WorkerScheduler`(워커 묶음 배분), `CommandletRegistry`(`shader-compile` 명령).

**미착수 단계에 넣을 셰이더 요구.** 그 단계를 설계·구현할 때 함께 해야 하므로 AssetSystem에 먼저 반영한다(14절, 6절 2항).

- **A0-G**
  - G1. 새 형식·차원·swizzle·샘플러 타입은 backend 중립으로 정의한다. D3D12 `Shader4ComponentMapping`, Vulkan `VkComponentMapping`, Metal `MTLTextureSwizzleChannels`, GLES `TEXTURE_SWIZZLE_R/G/B/A`로 옮길 수 있어야 한다. backend별 형식·필터 지원 표를 두고, 지원하지 않는 조합은 `UnsupportedCapability`다.
  - G2. A0-G의 OpenGL 구현은 OpenGL ES 3.2 core와 공통인 API를 기본으로 하고, 데스크톱 4.5 전용 호출(DSA `glTextureParameter*`·`glBindTextureUnit` 등)을 새로 늘리지 않는다. OpenGLGraphics는 SH2에서 GLES 미리보기, SH6에서 Android GLES로 이어지기 때문이다(P11). ES 3.2 core에 없는 A0-G 항목은 확장을 조건으로 한다: R16·RG16·RGBA16 UNORM(`GL_EXT_texture_norm16`), BGRA8(`GL_EXT_texture_format_BGRA8888`), 이방성 필터(`GL_EXT_texture_filter_anisotropic`), 32-bit float 선형 필터(`GL_OES_texture_float_linear`). 확장이 없으면 `UnsupportedCapability`로 실패하거나 Android 프로필 형식 표에서 뺀다. swizzle은 채널별 `TEXTURE_SWIZZLE_R/G/B/A`로 설정한다(`TEXTURE_SWIZZLE_RGBA`는 ES에 없다).
  - G3. SH2의 VulkanGraphics는 A0-G 범위(형식·차원·swizzle·샘플러)를 받아야 완료다.
- **A1**: Core `ErrorCode::UnsupportedCapability`(AssetSystem 1절 A1에서 추가)를 만들 수 없는 셰이더 형식에도 쓴다. 쿠킹 매니페스트 reader는 v2의 "에셋이 아닌 산출물" 항목을 읽는다(P6b).
- **A2**: `ProjectLayout`이 셰이더가 쓰는 위치를 준다. 엔진·프로젝트·플러그인의 `Shaders` 디렉터리(3절 가상 경로), 너겟 위치, 워커 작업 디렉터리 `<Project>/Intermediate/ShaderWorkers/`다. 프로젝트 없이 엔진만 쓸 때(Sandbox·엔진 내장 라이브러리)는 엔진 루트 아래 같은 이름을 쓴다. `ProjectLayout`은 AssetSystem에 이름만 있고 정의가 없다(1절 A2, 4.2절).
- **A3** — **반영됨**(AssetSystem 9.1절 결정 2·5·15, A3 구현 2026-11-23). 아래는 요구 원문이고 셰이더 쪽 대응은 10절(개정 23)이다: N1은 `WriteNuggetKeyHeader` 공통 머리와 `WriteNuggetEncoder` 목록, 키를 Record 키로 그대로 쓰는 규칙(결정 2 끝)으로, N2는 Record 값 표(ValueId 행)로, N3은 세대 Guid 둘(결정 4)·`.rnug` v2(결정 6)·일괄·비동기 요청(결정 12)·bucket `shader`(`NuggetBucketShader`)로 들어갔다.
  - N1. 에셋이 아닌 처리기도 공통 키 항목을 쓰게 한다. AssetSystem 9절 키 항목(번호는 이 문서 10절 표가 붙인 것) 가운데 1–5·8–9항(형식 버전, 처리기 Guid·버전 Guid, 외부 도구, 결정적 수학 버전, 툴체인 지문, 설정, 프로파일)은 공통 머리로 쓰고, 에셋 전용인 6·7·10항(TypeId·입력 bulk·의존 목록)은 처리기가 자기 입력으로 바꿔 쓸 수 있게 한다. 에셋이 아닌 처리기는 AssetId·Build 의존이 없으므로 SCC 식(구성원 key = SHA-256(SCC key, AssetId))을 쓰지 않고 키 = SHA-256(`BuildKey` 바이트)로 한다. 셰이더의 대응은 10절 표다.
  - N2. 값은 여러 부분(`.rnug` 부분 구조)이다: 바이트코드·정규화 바인딩 layout·GLES 복사 표·진단·디버그 심볼.
  - N3. Rockpile이 요구한 AssetSystem 9절 개정(`Rockpile.ko.md` 17절: 키의 에셋 너겟 세대 Guid 11.4절, `.rnug` v2 5.2절, 일괄·비동기 API 12.2절)이 A3에 반영되면 셰이더 키·값도 그대로 따른다. 셰이더 bucket은 `shader`다(`Rockpile.ko.md` 5.1절).
- **A4**
  - K1. TargetProfile에 `graphicsBackends`(복수, 순서 = 런타임 선택 순서)·`shaderFormats`(backend마다 선호 순서가 있는 형식 목록) 필드와 엔진 기본 `MacOS` 프로필을 둔다(12절). 지금 AssetSystem 10절은 "그래픽 backend"를 이름 없는 단일 항목으로 적고 기본 프로필은 Windows·Android뿐이다.
  - K2. 쿠킹 매니페스트 v2에 에셋이 아닌 산출물 항목을 둔다(12절). v1 항목은 AssetId 단위다(AssetSystem 10절).
  - K3. 엔진 내장 셰이더 라이브러리를 프로젝트·너겟·네트워크 없이 제공하는 경로를 둔다. 계획 9.6절이 "에셋 너겟·네트워크·프로젝트 없는 설치에서도 최소 자원을 제공한다"고 요구한다. 라이브러리를 만들 때 쓰는 엔진 컴파일 프로필은 P6a가 정하고(12.1절), A4는 설치물에 라이브러리를 두고 여는 경로만 맡는다(12.3절).

- **텍스처 스트리밍**([텍스처 스트리밍](../TextureStreaming.ko.md) 결정 T6, 2026-11-19 확정): 스트리밍 텍스처의 명시 LOD 표본과 정수 `Load`는 셰이더 helper를 거친다. helper는 `textureSize(t, 0)`로 상주 base mip을 계산해 논리 mip을 물리 mip으로 바꾼다(그 문서 5.3절). helper 없이 명시 LOD·`Load`를 쓰는 셰이더가 스트리밍 텍스처 멤버를 읽으면 워커가 진단을 내고, 그 텍스처는 "전체 상주 전용"으로 표시해 비스트리밍으로 돌린다. helper는 셰이더 표준 include에 두며 TS2(mip 전송)와 함께 구현한다.

### 5.3 셰이더 전용 선행 작업

규칙이 긴 작업(P2–P7)은 해당 절에 상세를 두고 여기에는 요약만 둔다.

**P1 자식 프로세스·pipe — 구현(2026-11-13).**
- `Platform/Process.h`: 시작·대기·종료 코드·자손까지 강제 종료(Windows job object, macOS 프로세스 그룹)·시간 제한 pipe·`RunProcess`·`CurrentProcessId`·`IsProcessRunning`·입력 pipe EOF lifeline([플랫폼 계층](../PlatformLayer.ko.md) "자식 프로세스와 pipe").
- 검증: Windows Debug·Asan, macOS CI(`b1d316f`), Android 에뮬레이터(Unsupported 경로).
- 남은 제약: Android `Start`는 Unsupported다(워커는 개발 호스트에서만 돈다). 환경 변수 지정이 없어 부모 환경을 물려준다(필요해지면 PlatformLayer 후속).

**P2 바인딩 규약 — 설계(7절).**
- 산출물: 7.2절 규칙, `ShaderBindingLayout` 형식, GLES 상수 버퍼 에뮬레이션 규약.
- 완료 기준: 측정에 기대지 않는 규칙이 확정되고 7.3절 시험이 SH2·SH3·SH5·SH6 완료 기준에 들어 있다. 행렬 규약(7.2절 6항)의 형식별 확인과 backend별 앞면 매핑(8항)은 측정 S5로 확인하는 SH2 산출물이다.

**P3 ThirdParty 확장과 오프라인 도구 빌드 — 설계(8절).**
- 산출물: Engine 루트, `kind: binary`·`kind: headers`, 저장소에 커밋하는 도구 공개 헤더, 사용자 단위 내용 주소 저장소와 bootstrap 명령, 라이브러리별 오프라인 빌드 스크립트와 수동 CI workflow, FXC 고정본, Apple 도구 최소 버전, SPIRV-Reflect(C 원본)·Vulkan-Headers(headers).
- 막는 것: SH0.

**P4 도구 표·binary 의존 정책·Release 배제 — 구현(9절).**
- 산출물: manifest에서 생성한 도구 표 모듈(메인 프로세스 키 계산·워커 로드 공용), binary 로드 정책, Release 배제 정책 검사.
- 의존: P3, P10. 막는 것: SH0(도구 표), SH2(Release 배제 판정).
- 달성(2026-11-17, `feature/shader-tools`): 9절 진척.

**P5 셰이더 job 키 — 설계(10절, 개정 23에서 A3 API에 맞춤).** 의존: A2, A3(N1–N3), P6a, A0-C·B7(모두 구현). 막는 것: SH1. 구현은 SH1b다(11.1절).

**P6a 형식 정의와 엔진 컴파일 프로필 — 구현(2026-11-17, 12.1절 진척).** 형식 정의 스키마와 `Engine/Config/ShaderFormats/<id>.json` 파일, 호스트별 엔진 컴파일 프로필(`Engine-<호스트>.json`)이다. job 키 3·9항이 형식 정의 SHA-256과 프로필을 쓰고 워커가 target env·ESSL 버전·상한을 읽으므로 P5·SH1 앞이다. 의존: A0-C JSON(구현), 결정 2·3·5.

**P6b TargetProfile 필드·매니페스트 v2·엔진 내장 라이브러리 — 설계(12.3절).** 의존: A1, A4(K1–K3), P6a, P14. 막는 것: SH2(런타임이 쿠킹된 라이브러리를 연다).

**P7 MetaCompiler 셰이더 파라미터 — 설계(13절, 15절 결정 4 확정).** 의존: B1–B8(구현). 막는 것: SH3. P7 전에는 손으로 쓴 레이아웃 표를 쓴다(7.2절 5항).

**P8 다른 문서 개정 — 목록(14절).** AssetSystem 쪽(5.2절 요구)은 해당 단계(A0-G·A1·A2·A3·A4) 착수 전에, 나머지는 15절의 결정별 기한에 맞춰 한다. 측정 뒤에 정하는 결정(3·5·6)에 걸린 행은 그 결정 뒤에 고친다.

**P9 Vulkan 선행 — 구현(Windows, 2026-11-17).**
- Vulkan-Headers: `kind: headers`(P3)로 Engine 루트에 둔다.
- 로더: import library를 링크하지 않고 런타임에 연다(P10, UE와 같다, 1절). Windows `vulkan-1.dll`은 드라이버·Vulkan 런타임이 설치한다. 없으면 VulkanGraphics 생성이 `UnsupportedCapability`로 실패하고 다른 backend는 그대로 동작한다.
- 검증 레이어: D3D12 디버그 레이어에 대응하며 같은 조건(EngineRuntime 10.1, 장치 생성의 `enableValidation`)에서 켠다. 공급(Khronos Vulkan-ValidationLayers 릴리스를 binary 항목으로 받거나 오프라인 빌드)은 측정 S4로 정한다.
- surface: Windows `VK_KHR_win32_surface`에 PlatformWindow의 네이티브 창 핸들을 쓴다. Android surface는 P13이다.
- VulkanGraphics 설계: [VulkanGraphics](../VulkanGraphics.ko.md)(디바이스·큐·메모리·파이프라인 layout(7.2절 4항)·검증 오류를 `ValidationErrorCount`로 집계). 내용이 커서 EngineRuntime 절 대신 따로 둔다.
- CI: GPU 없는 runner의 소프트웨어 ICD(lavapipe·SwiftShader)는 측정 S4다. 안 되면 CI에서 **NOT RUN**으로 보고하고 로컬 결과를 기록한다.
- macOS는 Metal을 쓰므로 Vulkan(MoltenVK 등)은 범위 밖이다.
- 완료 기준: Vulkan-Headers가 Rocks 경고 규칙으로 include되고, 로더 동적 로드와 검증 레이어 켜기가 Windows에서 시험되며, VulkanGraphics 설계 절이 확정된다.
- 달성(2026-11-17, 사용자 지시 "Vulkan까지는 작업을 미리하자"): 설계를 넘어 backend 자체를 SH2 전에 구현했다(`VulkanGraphics`, 개발용 컴파일 훅 `ShaderFormatVulkan`). 로더 동적 로드·검증 레이어 켜기는 EngineTests `VulkanDevice_LoadsTheSystemLoaderAndCreatesADevice`가, Graphics 계약 전체는 Sandbox self-test가 GTX 1060과 lavapipe에서 확인한다(D3D12와 픽셀 차이 0). 측정 S4 결과는 5.4절, 상세는 VulkanGraphics 17절이다. SH2에 남은 Vulkan 일은 쿠킹된 `VULKAN_SM5` SPIR-V와 리플렉션 머리를 받는 경로, `ROCKS_ROOT_CONSTANTS` 전개(지금은 `cbuffer DrawConstants` 이름 규칙), 단계 사이 변수 location 재작성, 7.3절 시험의 Vulkan 몫이다. Android 실행은 SH6·P13이다.

**P10 동적 라이브러리 API — 구현(2026-11-16, PlatformLayer P1 표의 "동적 모듈" 항목, 상세: PlatformLayer "동적 라이브러리" 절).**
- 구현 전: 공개 로더가 없고 private 코드가 OS 함수를 직접 불렀다(XInput, `Win64Window.cpp`). 지금은 XInput도 `OpenSystem`으로 연다. 계획 §2.5의 "동적 loader를 Foundation 선행 조건으로 만들지 않는다"는 native plugin 모듈 이야기다. 이 API는 Foundation이 아니라 셰이더 워커와 VulkanGraphics의 선행 조건이고, 엔진 모듈을 동적으로 싣는 로더가 아니다.
- 산출물 `Platform/DynamicLibrary.h`:
  - `Open(absolutePath)`: 절대 경로만 받는다. Windows `LoadLibraryExW`(`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`, 현재 디렉터리·PATH를 찾지 않고 같은 디렉터리의 의존 DLL은 찾는다), macOS·Android `dlopen(RTLD_NOW | RTLD_LOCAL)`.
  - `OpenSystem(name)`: OS가 공급하는 라이브러리만 이름으로 연다(Windows System32의 `vulkan-1.dll`·XInput, Android `libvulkan.so`).
  - `Find(symbol)`, 수명, 오류 변환(파일 없음 `NotFound`, 심볼 없음, 잘못된 경로 `InvalidArgument`). 수명은 PlatformLayer "동적 모듈" 검증 기준("참조 중 unload 거부")을 따른다: 얻은 심볼 표를 lease로 세고, lease가 남아 있으면 `Close`가 `Busy`로 거부한다.
- 완료 기준: 파일 없음·심볼 없음 오류, 현재 디렉터리에 둔 같은 이름 DLL을 열지 않는 음성 대조, 같은 디렉터리 의존 DLL 해결, Windows Debug·Asan·macOS CI·Android 에뮬레이터 시험. XInput 직접 로드를 이 API로 옮긴다.
- 달성(2026-11-16): Windows Debug·Asan에서 위 항목과 PATH 음성 대조·긴 경로를 확인했고 XInput을 옮겼다. 시험 DLL은 시험 지원 코드가 실행 중에 쓰는 데이터 전용 PE다(8절대로 Rocks.Build에 공유 라이브러리 종류를 더하지 않았다). Android 에뮬레이터는 OS 공급 라이브러리 시험만 돌고 의존·검색 시험은 SKIP, macOS는 CI 미확인이다. 두 플랫폼의 시험 라이브러리 생성기(Mach-O 코드 서명, ELF)는 후속이다.
- 막는 것: SH0(워커 도구 로드), SH2(Vulkan 로더).

**P11 OpenGL ES 선행 — 설계.** OpenGLGraphics를 UE `OpenGLDrv`처럼 Windows 미리보기와 Android ES를 함께 맡는 backend로 바꾸는 설계다.
- 현재: 4절 OpenGL 행(WGL 4.5 core, 런타임 GLSL 450, uniform 이름 조회, DSA).
- 공통 코드는 OpenGL ES 3.2와 데스크톱 GL 4.3 core가 함께 가진 API를 쓴다(UE는 공통 기반 `Runtime/OpenGLDrv/Private/OpenGLBase.h` 위에 Windows `FOpenGL4 : FOpenGL3`(`OpenGL4.h:17`, `Windows/WindowsOpenGL.h:9–19`)와 Android `OpenGLES.h:21`을 둔다). 플랫폼별·확장 의존 항목은 따로 목록으로 관리한다.
  - 데스크톱 전용: `glClipControl`(`GL_ARB_clip_control`), `GL_FRAMEBUFFER_SRGB`와 WGL sRGB 픽셀 형식(현재 `OpenGLDevice.cpp:384`·`:943`), DSA(지금 코드가 쓰며 공통 경로에서 뺀다).
  - ES 확장 의존: 5.2절 G2의 형식·필터 확장, sRGB 기본 framebuffer(`EGL_KHR_gl_colorspace`), 이방성 필터(지금 코드 `:585`).
  - sRGB 표시: Windows 미리보기는 offscreen sRGB 형식 target에 그린 뒤 blit하고, Android는 `EGL_KHR_gl_colorspace`가 있으면 sRGB 창 surface, 없으면 offscreen sRGB target에서 변환해 표시한다.
- Windows 미리보기: GL 4.3 core 이상과 `GL_ARB_clip_control`·`GL_ARB_ES3_2_compatibility`를 요구하고 `GLES31_PC` 코드를 받는다. `#version 320 es` 원문을 데스크톱 GL이 받으려면 `GL_ARB_ES3_2_compatibility`가 있어야 한다(4.3 core는 `300 es`, 4.5 core는 `310 es`까지). 드라이버 지원은 측정 S12이며, 없으면 `GLES31_PC`만 `#version 310 es`로 내는 방안을 그때 정한다. 창 표시는 offscreen에 그린 뒤 Y 반전 blit이다(7.2절 8항). UE는 이 확장이 있으면 쓰고(`Runtime/OpenGLDrv/Private/OpenGLDrv.cpp:470–474`) 없을 때 셰이더 쪽 대체 경로가 없으므로(컬 방향만 뒤집는다, `OpenGLShaders.cpp:959–968`, `OpenGLState.cpp:50–51`) Rocks는 필수로 한다. 데스크톱 드라이버가 `#version 320 es` 원문을 받는지는 측정 S12다(UE 5.8도 데스크톱에서 버전 줄을 고치지 않는다, `Runtime/OpenGLDrv/Private/OpenGLShaders.cpp:785–846`).
- Android: EGL ES 3.2 context(UE `Runtime/OpenGLDrv/Private/Android/AndroidEGL.cpp:730–820`, 3.2가 안 되면 3.1로 다시 시도 `:803–808`), NDK 시스템 라이브러리 `EGL`·`GLESv3`, surface는 P13이다.
- 셰이더 생성: 쿠킹된 ESSL과 바인딩 머리(7.2절 4항)를 받아 `glCreateProgram`·`glAttachShader`·`glLinkProgram`으로 만든다. UE도 separate shader object를 쓰지 않는다(`Runtime/OpenGLDrv/Private/OpenGLBase.h:283–287`). 프로그램 바이너리 캐시는 후속이다(UE는 Android만 기본으로 켠다, `OpenGLProgramBinaryFileCache.cpp:39–48`).
- 완료 기준: EngineRuntime OpenGL 절 개정이 확정되고, Windows 미리보기 컨텍스트에서 기존 self-test가 GLES 공통 API만으로 통과한다(셰이더까지는 SH2).
- 의존: A0-G(G2). 막는 것: SH2(미리보기), SH6(Android).
- 달성(2026-11-17, `feature/gles-graphics`, 설계·상태는 [OpenGL ES backend](../OpenGLESGraphics.ko.md)): 장치 동작을 ES 3.2 core 함수 표로 하는 공통 층과 플랫폼 층(Windows WGL, Android EGL)으로 나눴다. 데스크톱 전용 호출(`glClipControl`·`GL_FRAMEBUFFER_SRGB` 켜기·WGL sRGB 픽셀 형식)은 Windows 층에만 있고 DSA는 없앴다. Windows 미리보기는 ES 3.2 profile context를 먼저 쓰고 데스크톱 GL 4.3 core로 내려갈 수 있다(위 "GL 4.3 core 이상"과 다른 점, 측정은 그 문서 2.2절). 클립 공간은 창·RenderTarget 모두 `GL_UPPER_LEFT`이고 스왑체인은 offscreen 텍스처에 그린 뒤 Y 반전 blit으로 표시한다(7.2절 8항을 SH2보다 앞당겼다). 그리기 상수는 루트 상수 에뮬레이션 배열(`vu_h`·`pu_h`, 7.2절 4항)로 올리고 Sandbox 원문은 쿠킹된 `GLES31_PC`와 같은 모양의 GLSL ES 3.20이다. Sandbox self-test가 두 context에서 통과하고 D3D12와 픽셀 차이 0이다. Android 교차 빌드에서 EGL 층이 컴파일·링크된다. A0-G 형식 확장 전이라 형식 표는 지금 형식만 두고 확장 조건 열을 미리 두었다.

**P12 macOS 실행 기반 — 미설계.**
- 현재: PlatformWindow는 Win32만 있다. Rocks.Build 언어는 C·C++뿐이다(`NativeBuildModels.cs:57`, `ActionGraph.cs:373–374`). framework 링크 필드는 있다(`NativeBuildModels.cs:145`). macOS는 CI에서만 빌드·시험한다.
- UE 5.8: Apple 대상 C++ 원본을 Objective-C++로 컴파일하고(`Programs/UnrealBuildTool/Platform/Apple/AppleToolChain.cs:461,474`), MetalRHI는 `.cpp`에서 metal-cpp(`ThirdParty/MetalCPP`)를 쓰며, 창은 Cocoa다(`Runtime/ApplicationCore/Private/Mac/CocoaWindow.cpp`).
- 산출물: Rocks.Build의 Apple Objective-C++ 컴파일(정책 검사와의 관계 포함), metal-cpp 공급(`kind: headers`), PlatformWindow Mac backend(Cocoa 창·`CAMetalLayer`·입력), macOS 실행 검증 경로(실기기 또는 CI의 Metal 장치).
- 완료 기준: macOS에서 창 생성·이벤트 펌프·크기 변경·닫기 시험, `CAMetalLayer`에 지운 색 한 장 표시, Objective-C++ 원본의 경고 0과 정책 검사 통과, macOS CI 실행.
- 막는 것: SH5의 MetalGraphics(P15).

**P13 Android 실행 기반 — 미설계(PlatformLayer 모바일 절).**
- 현재: Android는 시험 실행 파일을 에뮬레이터에 올려 실행만 한다(PlatformLayer 모바일 절). 앱 껍데기·창·surface·수명·APK 패키징이 없고, APK 안 데이터 읽기(VFS)는 AssetSystem 2절에서 후속이다.
- 산출물: 앱 진입(NativeActivity 계열), `ANativeWindow` surface와 생성·소실·재생성 수명, APK 패키징(manifest `glEsVersion` 0x00030002, UE와 같다), 에뮬레이터·실기기 실행 경로(측정 S11). APK 안 쿠킹 데이터 읽기(VFS)는 AssetSystem이 맡는 선행 작업이다(14절).
- 완료 기준: 앱 시작·종료, 백그라운드 전환 때 surface 소실·재생성 시험, EGL ES 3.2 context로 지운 색 한 장 표시, 에뮬레이터 또는 실기기 실행 기록.
- 막는 것: SH6.

**P14 셰이더 타입·퍼뮤테이션·라이브러리 형식 — 설계(12절).**
- 현재: 셰이더 타입 등록, 퍼뮤테이션, 셰이더 라이브러리 파일 형식, 런타임 조회 규칙이 없다(Renderer 내장 셰이더는 문자열 하나다).
- UE 5.8: 셰이더 타입은 `IMPLEMENT_GLOBAL_SHADER`로 정적 등록하고(`Runtime/RenderCore/Public/GlobalShader.h:433`), 퍼뮤테이션은 차원 타입(`SHADER_PERMUTATION_BOOL` 등, `Runtime/RenderCore/Public/ShaderPermutation.h:629`)을 묶은 `TShaderPermutationDomain`(`:272`)이 정하며, 타입마다 `ShouldCompilePermutation`으로 형식·퍼뮤테이션을 거른다(`GlobalShader.h:100–102`). 라이브러리는 셰이더 맵 hash·셰이더 hash 표와 항목 표를 가진다(`FSerializedShaderArchive`, `Runtime/RenderCore/Public/ShaderCodeArchive.h:206–230`, 항목 `:48`·`:70`).
- 산출물: 셰이더 타입 등록(이름·`.rsf` 경로·엔트리·단계·요구 기능, 2절), 퍼뮤테이션 차원 선언과 ID 계산, 형식·퍼뮤테이션 거르기 규칙, 라이브러리 항목 키(셰이더 타입·퍼뮤테이션 ID·단계 → 코드 SHA-256)와 파일 형식·버전, 런타임 조회와 빠졌을 때의 `IncompatibleCook`. 개정 16에서 정점 입력 타입 축, 머티리얼 셰이더 표지, 거르기 서명과 형식 사실 조회, 퍼뮤테이션 수 보고를 더했다(12.2절).
- 완료 기준: 등록·퍼뮤테이션 ID의 결정성 시험(등록 순서와 무관), 같은 코드의 중복 제거, 라이브러리 왕복·손상 검출, 빠진 항목 조회의 `IncompatibleCook`.
- 의존: P6a. 막는 것: P6b, SH2.

**P15 D3D11Graphics·MetalGraphics 설계 — D3D11 구현, Metal 미설계(EngineRuntime).**
- 산출물: VulkanGraphics 절(P9)과 같은 수준의 backend 설계 절(장치·스왑체인·자원·파이프라인 layout(7.2절 4항)·검증 레이어·self-test 대상)이다. D3D11은 Windows, Metal은 P12 위에 선다.
- D3D11(2026-11-17): 설계는 EngineRuntime 10.2절이다. 사용자 결정으로 SH5의 D3D11 부분 가운데 backend 자체를 앞당겨 구현했다(`D3D11Graphics`, 실행 중 `D3DCompile` SM 5.0). SH5에 남은 D3D11 일은 쿠킹된 `D3D11_SM5` 코드를 받는 경로(SH2 API 위), 그리기 상수 b13 배치(7.2절 4항), 7.3절 (c)·(g)의 D3D11 대조다.
- 막는 것: SH5.

### 5.4 측정

결과에 따라 설계가 갈리는 값은 짐작하지 않고 SH0(또는 해당 단계)에서 잰다.

| ID | 측정 | 결과가 정하는 것 | 결과(SH0, 2026-11-17) |
|---|---|---|---|
| S1 | DXC 1.8.2505 내장 validator 서명(Windows·macOS) | 10절 서명 검사, `D3D12_SM6` 생산 호스트 | Windows: DXC 1.9.2609(내장 validator)가 Renderer VS·PS와 fixture DXIL 4개를 모두 서명했다(container digest가 0도 preview 우회 값도 아님). macOS: macOS 필요, 미측정 |
| S2 | 개발 기계 GPU와 WARP의 D3D12 최대 셰이더 모델, Windows 10 Agility SDK 요건 | 결정 3 | GTX 1060 3GB(드라이버 32.0.15.6094)·WARP(10.0.26100.9818) 모두 최대 SM **6.8**, feature level 12.1, resource binding tier 3. Agility SDK 없이 OS 런타임(Windows 11 build 26200, `D3D12Core.dll` 10.0.26100.9818)에서 6.6 이상이 된다. Windows 10 요건은 Windows 10 기계가 없어 미측정 |
| S3 | 결정성: 같은 호스트 반복, Debug·Release 워커, 작업 디렉터리 변경, FMA3 유무, OS 버전, **Windows·macOS 사이**(DXIL·SPIR-V·ESSL·MSL 텍스트)에서 형식별 바이트 동일 | 10절 결정성 | Windows 부분 byte 동일: 같은 입력 반복(Debug 2회·Release 3회), 작업 디렉터리 변경(Debug·Release 각 1회), Debug·Release 스파이크 빌드 사이에서 manifest의 산출물 44개(DXIL·SPIR-V·DXBC·ESSL·MSL·HLSL 5.0·리플렉션 덤프)가 모두 같다. 상수 초월 함수 fixture는 DXC가 DXIL·SPIR-V 모두 상수로 접었고(최적화 전 SPIR-V에도 `OpExtInst` 0), 접은 8개 값은 이 호스트 UCRT double 함수 결과를 float로 반올림한 값과 같다. FMA3 유무·OS 버전·Windows–macOS는 미측정(기계 하나) |
| S4 | Vulkan 검증 레이어 공급, GPU 없는 CI의 소프트웨어 ICD | P9 | 로컬 결과(2026-11-17, VulkanGraphics 17절): 검증 레이어는 LunarG Vulkan SDK 1.4.363.0 설치로 공급했고 CI 공급 방식은 SH2 때 정한다. 소프트웨어 ICD는 lavapipe(Mesa 26.2.7 `mesa-dist-win`, `VK_ADD_DRIVER_FILES`)로 self-test 전체가 통과한다(calibrated timestamps의 QPC domain이 없어 calibration은 `Unsupported`). SwiftShader는 Windows 공식 바이너리가 없어 쓰지 않았다. CI 연결은 아직이다 |
| S5 | D3D12·Vulkan·GLES 미리보기 픽셀 대조(행렬 옵션·앞면·Y 반전·루트 상수 대응) | 7.2절 6·8항 | C7 런타임 경로(2026-11-17, Windows, GTX 1060·WARP·NVIDIA OpenGL 4.5, Sandbox self-test `RunCullingChecks`). 행렬: D3D12(`D3DCompile` + `D3DCOMPILE_PACK_MATRIX_ROW_MAJOR`)·WARP·OpenGL(`transpose = GL_TRUE`) 모두에서 비균등 scale 뒤 이동한 비대칭 행렬의 사각형이 기대 영역(64칸 격자의 열 32–63·행 8–23)만 덮는다. 앞면: D3D12 `FrontCounterClockwise = TRUE`, OpenGL은 창(`GL_LOWER_LEFT`)·RenderTarget(`GL_UPPER_LEFT`) 모두 `glFrontFace(GL_CCW)`에서 화면 반시계가 앞면이다. None·Back·Front 컬, 음수 scale·거울 view 반전, 둘 다 음수일 때 원래대로가 세 장치에서 기대대로이고, 창 경로(`CopyPassTarget`)와 offscreen 경로의 이미지가 byte 단위로 같다. 창을 `GL_CW`로 둔 음성 대조는 창 경로 6건 불일치로 실패한다. 기존 self-test 픽셀 검사와 backend 간 대조도 그대로 통과한다(차이 0). Vulkan·GLES 미리보기·쿠킹 경로(DXC `-Zpr`)와 루트 상수 대응은 SH2에서 잰다 P11(2026-11-17): GLES 미리보기 런타임 경로(ES 3.2 profile·GL 4.3 core 두 context, 손으로 쓴 GLSL ES 3.20)에서 같은 검사가 통과하고 D3D12와 픽셀 차이 0이다. 창·RenderTarget 모두 `GL_UPPER_LEFT` + `GL_CCW`, 스왑체인은 offscreen에 그린 뒤 Y 반전 blit. 음성 대조: Y 반전을 빼면 창 경로 7건, `GL_CW`면 컬 검사 6건 불일치 |
| S6 | 도구 빌드 컴파일러(clang-cl·MSVC)·빌드 시간·바이너리 크기 | 8절 4항 | P3 인용: Windows x64 도구는 Visual Studio C++ 도구(vcvarsall x64, MSVC, DLL 링커 버전 14.29)와 CMake·Ninja Release, 업스트림 빌드 설정으로 만들었다. 크기: `dxcompiler.dll` 21,943,296 B, `spirv-cross-c-shared.dll` 2,305,536 B, `SPIRV-Tools-c.dll` 3,644,416 B(PDB 별도). 빌드 시간은 P3에서 기록하지 않았고 clang-cl 비교와 macOS는 미측정 |
| S7 | 자체 전처리 결과로 컴파일한 출력 = DXC 직접 전처리 출력(`#line` 포함) | 결정 6 구현의 정합성(SH1 완료 기준) | SH1a(2026-11-23, Windows, DXC 1.9.2609, `ShaderPreprocessS7_*`): fixture(상대·절대 include, `#pragma once`·include guard, `##`·가변 인자·여러 줄 호출, `ROCKS_*` 조건·`defined`·`true`, `ROCKS_IMAGE_FORMAT`, `__LINE__`, 여러 줄 주석, job 정의)에서 (가) Rocks 전처리 결과 컴파일과 (나) DXC 직접 전처리 컴파일의 산출물이 byte 단위로 같다: `D3D12_SM6` `cs_6_6` DXIL 3,508 B, `VULKAN_SM5` `cs_6_0` SPIR-V 1,164 B. job 정의 값 하나만 바꾼 음성 대조는 다른 byte다. 일부러 넣은 오류의 첫 진단 위치는 두 방식 모두 `/Plugin/S7/Lib/Broken.rsh:4`다. `#if`의 내장 이름은 Rocks 쪽 오류다. macOS는 미측정(DXC 호출 구현이 Windows만 — 11.1절 SH1a 구현 메모) |
| S8 | 워커 기동·DLL 로드 비용, SHA-256 처리량 | 11절 묶음 크기 | 측정값만(Release 스파이크 3회, i5-13600KF): `ShaderToolSet::Open` 82.5 ms 가운데 runtime 파일 31.3 MiB의 SHA-256 대조가 약 74 ms(Core SHA-256 423–426 MiB/s)이고 나머지 로드는 약 9 ms. 첫 DXIL 컴파일 5.6–6.4 ms(다음 3.6–3.7 ms), 첫 SPIR-V 3.6–4.3 ms(다음 2.9–3.1 ms), SPIRV-Cross ESSL 첫 0.34–0.36 ms(다음 0.07–0.10 ms), 첫 `spirv-val` 0.43–0.46 ms. Debug 빌드는 SHA-256 26–27 MiB/s, 열기 1.2 s. 워커 프로세스 기동은 워커가 없어 미측정 |
| S9 | Apple `metal` 재배포 조건, Windows용 Metal Developer Tools(UE 5.8 기대 버전 5.3, `Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:724–725`) | 결정 5 | macOS 필요, 미측정 |
| S10 | `d3dcompiler_47.dll` 재배포 조건과 고정 버전 | 8절 2항 FXC | 문서 조사(2026-11-20): Windows SDK 사용 조건 2.a "Distributable Code"가 가리키는 REDIST.TXT 목록(`learn.microsoft.com/legal/windows-sdk/redist`, 2024-10-21 갱신)에 `Redist\D3D\x64\d3dcompiler_47.dll`이 있다(x86과 `d3dcsx_47.dll`·`dxcompiler.dll`·`dxil.dll`도 같은 묶음). 조건: 고치지 않은 파일만, **Classic Windows(데스크톱) 응용과 함께만** 재배포하고 UWP 앱에는 넣지 않는다. 사용 조건의 배포 요건이 붙는다(프로그램에 상당한 주 기능을 더할 것, 배포자·최종 사용자가 이 조건 이상으로 보호하는 조건에 동의할 것, 자기 저작권 표시, Microsoft 면책, Microsoft OS 밖 플랫폼용 배포 금지, 저작권·상표 고지 변경 금지). 고정 버전: 저장소 manifest(`Engine/Source/ThirdParty/D3DCompiler/ThirdParty.json`)의 SDK 10.0.22621.0 파일 버전 10.0.22621.2428(4,916,728 B, SHA-256 `af7b99be…`)이 이 기계 설치본과 크기·SHA가 같다. 같은 기계 System32 본은 10.0.26100.9974라 OS 빌드마다 다르다는 전제도 맞다. UE 5.8 동봉본은 10.0.19041.1(GitDependencies, `Engine/Build/Commit.gitdeps.xml:39300`)이다. 결론은 8절 2항 |
| S11 | Android 에뮬레이터·CI의 OpenGL ES 3.2 context | SH6 검증 경로 | — |
| S12 | (SH0) 데스크톱 GL 드라이버의 `#version 320 es` 수용(`GL_ARB_ES3_2_compatibility` 유무), (SH6) UE 수정 pass 없이 업스트림 SPIRV-Tools·SPIRV-Cross만으로 만든 ESSL이 Android 기기 드라이버에서 도는지 | P11, 7.2절 11항 | 데스크톱(GTX 1060 3GB, NVIDIA 560.94): GL 4.6 core context에 `GL_ARB_ES3_2_compatibility`·`GL_ARB_ES3_1_compatibility`·`GL_ARB_clip_control`이 있고 GLSL 목록에 `320 es`가 있다. `GLES31_PC` Renderer VS·PS 원문이 경고 없이 컴파일·링크된다. `WGL_EXT_create_context_es2_profile`로 만든 ES 3.2 context에서도 같다. AMD·Intel 드라이버는 미측정. 기기(SH6)는 미측정 P11(2026-11-17) 추가: ES 3.2 profile context는 `GL_EXT_clip_control`·`GL_EXT_disjoint_timer_query`·`GL_KHR_debug`·`GL_EXT_sRGB_write_control`과 ES 형식 확장을 공개하고, 4.3 core 요청은 4.3.0을 준다([OpenGL ES backend](../OpenGLESGraphics.ko.md) 2.2절) |
| S13 | SPIRV-Cross가 HLSL `Buffer<T>`·`RWBuffer<T>`(typed buffer)를 ESSL `samplerBuffer`·`isamplerBuffer`·`usamplerBuffer`와 `texelFetch`, `imageBuffer`와 `imageLoad`·`imageStore`로 내는지, 결합 sampler 목록에 typed buffer SRV가 들어가는지 | 7.2절 4항 GLES typed buffer 규칙(Renderer RQ10) | Windows(2026-11-20, SPIRV-Cross C API 0.68.0, `TypedBuffer.ps`·`TypedBuffer.cs` fixture): ESSL 320(`GLES31_PC`·`GLES31_ANDROID` 같음)에서 `Buffer<float4>`·`Buffer<int4>`·`Buffer<uint>`가 `samplerBuffer`·`isamplerBuffer`·`usamplerBuffer`와 `texelFetch`로, `RWBuffer<float4>`(rgba8 지정)·`RWBuffer<uint>`가 `layout(rgba8) writeonly imageBuffer`·`layout(r32ui) uimageBuffer`와 `imageLoad`·`imageStore`로 나온다(읽지 않는 UAV에 `writeonly`가 붙는다). 결합 sampler 목록(`spvc_compiler_get_combined_image_samplers`)에는 텍스처·샘플러 짝(`albedo`·`albedoSampler`)만 있고 typed buffer SRV는 **들어가지 않는다**. typed buffer는 SPIR-V binding을 그대로 단 `layout(binding = n)`으로, 결합 sampler는 binding 없이 id 이름(`_57`)으로 나온다. MSL 2.1은 기본값이면 `texture2d`와 좌표 변환 함수로 흉내 내고 `texture_buffer_native`를 켜면 `texture_buffer<T>`(UAV는 `access::write`·`access::read_write`)다. DXIL·SPIR-V(`spirv-val`) 모두 통과. 결론은 7.2절 4항 |
| S14 | VS 출력 `SV_RenderTargetArrayIndex`·`SV_ViewportArrayIndex`의 변환: DXC DXIL(SM 6.6), DXC SPIR-V(target env 1.1에서 `ShaderViewportIndexLayerEXT` capability·`SPV_EXT_shader_viewport_index_layer` 확장으로 나오는지), SPIRV-Cross MSL(`[[render_target_array_index]]`·`[[viewport_array_index]]`), FXC `vs_5_0`이 받는지 | 7.2절 7항 VS layer·viewport 출력 규칙의 형식별 `vertexViewportLayerOutput`(Renderer RQ14), `D3D11_SM5` 포함 여부 | Windows(2026-11-20, `LayerViewport.vs` fixture: layer·viewport를 함께 낸다): DXC DXIL은 SM 6.6과 6.0 모두 받고 서명되며 요구 플래그 `D3D_SHADER_REQUIRES_VIEWPORT_AND_RT_ARRAY_INDEX_FROM_ANY_SHADER_FEEDING_RASTERIZER`(0x2000)가 선다. DXC SPIR-V(vulkan1.1)는 capability `ShaderViewportIndexLayerEXT`(5254)와 확장 `SPV_EXT_shader_viewport_index_layer`를 내고 `spirv-val`을 통과한다. SPIRV-Cross MSL 2.1은 `[[render_target_array_index]]`·`[[viewport_array_index]]`, HLSL 5.0은 `SV_RenderTargetArrayIndex`·`SV_ViewportArrayIndex`다. ESSL은 만들어지지만 `#extension GL_NV_viewport_array2 : require`가 붙는다. FXC `vs_5_0`은 원문과 SPIRV-Cross HLSL 5.0 출력을 모두 받고 같은 요구 플래그를 세운다. 결론은 7.2절 7항(`D3D11_SM5`에도 넣는다, 개정 21) |

SH0 남은 측정(2026-11-20): Windows에서 할 수 있는 측정은 모두 끝났다. 남은 것은 macOS가 필요한 S1(macOS DXC 서명)·S3(Windows–macOS 사이 byte 비교)·S9, 기계가 하나라 못 잰 S2(Windows 10 요건)·S3(FMA3 유무·OS 버전)·S12(다른 GPU 공급자), 다른 단계 몫인 S5(Vulkan·GLES 미리보기 쿠킹 경로와 루트 상수 대응, SH2)·S7(SH1 완료 기준)·S11(SH6, P13 미설계)·S12(기기, SH6)다. 개정 29: macOS 측정(S1·S3·S9)의 절차는 스파이크 대신 `shader-compile`로 옮겼다(11.1절 E10). macOS에서 DXC를 여는 일(P3 macOS 산출물)이 먼저다.
