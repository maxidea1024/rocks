# 셰이더 시스템 설계 — 4. 현재 코드베이스(2026-11-14)

[목차](README.ko.md)

## 4. 현재 코드베이스(2026-11-14)

HEAD `0be1e64` 기준이다. 셰이더 시스템이 쓸 기반과 바꿀 대상, 없는 것을 모은다.

| 영역 | 있는 것 | 셰이더 시스템에 부족한 것 → 선행 작업 |
|---|---|---|
| Graphics API | `Backend{D3D12, OpenGL, Vulkan, Metal}`(Vulkan·Metal은 Unsupported, `DeviceTypes.h:14–21`), `ShaderSource{hlsl, glsl}`(`:140–143`), 파이프라인 생성이 컴파일까지 한다. 바인딩은 고정(상수 b0, 텍스처 t0·s0, `:145–148`), `DrawConstants` 80 byte(`float transform[16]`·`float color[4]`, `:161–164`) | 컴파일된 코드를 받는 API·파이프라인 layout → SH2. D3D11·Vulkan·Metal backend와 OpenGL의 GLES 전환 → P9·P11·P12·SH2·SH5 |
| D3D12 | root signature 하나(32-bit 상수 20개 b0, SRV·샘플러 테이블 각 1개는 PS만, `Engine/Source/Runtime/D3D12Graphics/Private/Win64/D3D12Device.cpp:1083–1120`), 런타임 `D3DCompile` `vs_5_1`·`ps_5_1`(`:1122–1146`), 모든 구성에서 `d3dcompiler` 링크(`D3D12Graphics.Build.cs:21–22`), 입력 시맨틱 `TEXCOORD`+location(`:703–704`), 디버그 레이어(`:323–330`) | 파이프라인별 root signature(7.2절 4항) → SH2, `d3dcompiler` 제거 → P4·SH2 |
| OpenGL | Windows WGL 4.5 core(`Engine/Source/Runtime/OpenGLGraphics/Private/Win64/OpenGLDevice.cpp:394–422`), GL 함수 선언을 직접 두고 `wglGetProcAddress`로 얻음(`:144–146`), 런타임 GLSL 450 컴파일(`:921–935`), uniform 이름 조회(`:653–654`), DSA(`:574–585`·`:996`), `glClipControl`(`:888–893`) | GLES 3.2 공통 API 부분집합과 쿠킹된 ESSL → P11·SH2 |
| Renderer | 내장 셰이더를 C++ 문자열로 가진다(HLSL·GLSL 문자열, 2026-11-17부터 Sandbox 시험 샘플 `Programs/Sandbox/Private/SampleRenderer.cpp`), 파이프라인 하나(텍스처×정점색×항목색) | `.rsf` 원문과 쿠킹된 라이브러리 → SH1·SH2 |
| Sandbox | self-test가 D3D12 하드웨어·WARP·OpenGL을 대조(허용 오차 채널 ±8·2%, `Programs/Sandbox/Private/GraphicsSelfTest.cpp:409–415`, 대상 `:427–438`). `--graphics=` 인자는 없다 | Vulkan·GLES 미리보기 대조 → SH2 |
| GraphicsTypes | RGBA8 2종, 2D, 필터 4종(`PixelFormat.h:12–16`, `Sampler.h:11–20`) | A0-G(다른 문서, 5.2절 요구) |
| 수학 타입 | `Matrix4`·`Vector3`는 정렬 지정 없음(`Core/Source/Runtime/Core/Public/Core/Math/Matrix4.h:56–61`, `Vector3.h:19–24`) | 셰이더 전용 정렬 타입(7.2절 5항) → SH2 |
| 플랫폼 | `Platform/Process.h`(P1 구현), `Platform/DirectoryWatch.h:88`(A0-P 구현). `Platform/DynamicLibrary.h`(P10 구현, Windows 검증, XInput이 이 API로 연다). PlatformWindow는 Win32만이고 다른 플랫폼은 Unsupported(`PlatformWindow.Build.cs`) | P10, P12, P13 |
| Core | SHA-256(`Core/Encoding/Sha256.h:23,40`), JSON(`Core/Text/Json.h`), 결정적 수학 버전 `rocks::deterministic::Version`(`DeterministicMath/DeterministicMath.h:31`), 툴체인 지문·도구 빌드 ID(`Core/Build/BuildInfo.h:24,30`), Archive·Record·RecordList, `WorkerScheduler`, `CommandletRegistry` | 그대로 쓴다(5.2절) |
| 리플렉션 | A0-B B1–B8 구현(`ROCKS_STRUCT`·identity·배열·지정자·World 타입, `e405c37`) | 셰이더 파라미터 종류 → P7 |
| ThirdParty | 루트 `Core/Source/ThirdParty` 하나(`Programs/Rocks.Tooling.Common/ThirdPartyManifest.cs:76`), `kind`는 `code`·`data`(`:28`), `code`는 `.c` 번역 단위만(`:357–364`), 미리 빌드된 바이너리 필드 없음(`:132–136`, 모르는 필드는 오류 `:45`) | P3 |
| 빌드 | 모듈 종류는 StaticLibrary·Executable(`Programs/Rocks.Build/NativeBuildModels.cs:38`), 언어는 C·C++(`:57`), `PrebuiltLibraries`(`:147`), 런타임 파일 복사는 Asan DLL만(`ActionGraph.cs:329–337`), rpath 처리 없음, 생성 코드 `<out>/Generated/<Module>`·`<out>/BuildInfo` | P4·P12 |
| CI | `.github/workflows/foundation.yml` 하나(Windows·macOS), 캐시 단계 없음 | P3(저장소 캐시), P9(Vulkan 장치) |
| 에셋 | A0-O·A0-G·A1–A6 미착수(AssetRuntime·AssetNugget 모듈 없음), `ProjectLayout`은 이름만(AssetSystem 1절 A2) | A1–A4 → P5·P6b |
| 셰이더 원문·설정 | `Engine/Shaders`·`Engine/Config` 없음, 셰이더 파일 없음 | SH1·P6a·P14 |

다른 설계 문서 상태: A0-P·A0-C·A0-B는 구현되었고 A0-O·A0-G·A1–A6은 착수 전이다. "화면 마일스톤"의 범위는 2026-11-16 [렌더 기반](../RenderBase.ko.md) 18.1절에 정의했다(셰이더 구현과 렌더 기반을 함께 세우는 단계, 결정 11 (나)).
