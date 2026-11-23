# 셰이더 시스템 설계 — 14. P8 다른 문서 개정

[목차](README.ko.md)

## 14. P8 다른 문서 개정

이 문서 확정과 함께 아래를 고친다. AssetSystem 행은 해당 단계 착수 전에 먼저 고친다(6절 2항). 다른 세션이 문서를 계속 고치므로 위치는 절 번호와 인용 문구로 적는다(줄 번호는 2026-11-14 HEAD 기준 참고값).

| 문서 | 위치 | 현재 | 개정 |
|---|---|---|---|
| EngineImplementationPlan | 서문(:27), §2.2 그래픽 행(:122) | "D3D11/D3D12/Vulkan/Metal/OpenGL", 공통 material IR과 backend별 shader toolchain | OpenGL을 OpenGL ES(Android ES 3.2, Windows 개발 미리보기)로. HLSL 단일 원문(Material IR은 HLSL을 만드는 앞단). "5개 backend"·"5개 RHI"·"5종 RHI" 표현(:11·:185·:203·:208·:1101·:1121·:1128·:1376·:1381·:2521·:2532·:2533·:2770)은 개수가 같으므로 그대로 두고 서문에서 OpenGL이 OpenGL ES임을 한 번 밝힌다 |
| 〃 | §2.5 표(:187–193) | D3D11 "HLSL→FXC/D3DCompile→DXBC", Metal "typed IR→MSL"(:192), OpenGL "IR→GLSL"(:193), Vulkan Windows/Linux | 2절 표의 경로. OpenGL 행은 OpenGL ES(ESSL 320). Linux Vulkan 검증은 이 문서 범위 밖이다(Linux는 서버·headless 대상이고 지금 빌드 대상이 아니다, `NativeBuildModels.cs:4`) |
| 〃 | §2.5(:197) | Windows용 Metal 제작 도구가 있으므로 Metal cook을 macOS 전용으로 단정하지 않는다 | 유지한다(15절 결정 5와 같은 방향) |
| 〃 | §3.1(:234), §3.2(:262), §14.6(:1276), §14.9(:1341), §27.7(:2287) | ShaderWorker | ShaderCompileWorker(§14.6의 의존 대상은 `Developer/ShaderCompiler` 라이브러리) |
| 〃 | §3.2(:259) | `Engine/Source/ThirdParty/ Adapters` | 외부 라이브러리 원본·manifest·바이너리 manifest·adapter(8절) |
| 〃 | §11.5(:1107) | Vulkan "1.2급" | `VULKAN_SM5`는 target env 1.1(UE `SF_VULKAN_SM5`와 같다, 2절) |
| 〃 | §11.5(:1108·:1109·:1117) | Metal "typed IR→SPIR-V/MSL 변환→Apple offline library", OpenGL "core 4.5급; IR→SPIR-V→GLSL", GL clip 규약 | Metal은 HLSL→SPIR-V→MSL. OpenGL ES 3.2(Android)와 Windows 개발 미리보기(GL 4.3 core 이상 + clip control), HLSL→SPIR-V→ESSL 320, 상수 버퍼 에뮬레이션. clip은 7.2절 8항 |
| 〃 | §14.1(:1212), §14.7(:1319) | DXBC·DXIL·SPIR-V·MSL·GLSL 경로, OpenGL의 target GLSL compile/link와 driver pipeline compilation | GLSL을 ESSL로. 쿠킹된 ESSL의 program link, 프로그램 바이너리 캐시는 후속 |
| 〃 | §27.9(:2315) | `CapabilityUnavailable` | `UnsupportedCapability`(AssetSystem A1의 Core ErrorCode) |
| 〃 | S71(:2965) | SPIRV-Cross "MSL·GLSL" | "MSL·ESSL·HLSL 5.0" |
| 〃 | §2.1(:114) | ThirdParty의 예외·RTTI 옵션은 별도 빌드, 예외가 엔진 경계를 넘지 않게 변환 | 유지한다(8절 4항의 오프라인 도구 빌드가 이 규칙의 사례) |
| CoreObjectInputDetailedPlan | :1301 | 5개 graphics API, D3D11/D3D12/Vulkan/OpenGL/Metal | OpenGL을 OpenGL ES로 |
| EngineRuntime | :3·:14·:38·:39·:220·:222·:236·:238·:239·:243·:255·:266·:274 | 범위 "D3D12·OpenGL로 화면에", OpenGL 4.5 core, Vulkan·Metal "자리만", `Backend{D3D12, OpenGL, Vulkan, Metal}`, `CreateDevice`(코드에 없음), 런타임 `D3DCompile`·GLSL 4.50, "다른 backend 구현은 범위 밖", `--graphics=d3d12\|opengl`(코드에 없음), OpenGL self-test, "Vulkan은 미구현", 창 `GL_LOWER_LEFT`·RenderTarget `GL_UPPER_LEFT` 원점 규칙(:238), 디버그 레이어(:239) | OpenGLGraphics를 OpenGL ES backend(Windows 미리보기·Android, P11)로 다시 정의하고(원점은 7.2절 8항, 디버그 레이어 규칙은 Vulkan 검증 레이어에도 적용), 셰이더 원문 규칙을 이 문서 참조로 바꾼다. VulkanGraphics 절(P9, SH2 전)과 파이프라인 래스터(컬링·앞면)·깊이 상태(지금 `PipelineDesc`에 없다, `DeviceTypes.h:145–148`, SH2), D3D11·Metal 절(P15, SH5 전), `Backend`에 D3D11 추가(`DeviceTypes.h:14–21`). 코드에 없는 `CreateDevice`·`--graphics` 서술 정리 |
| AssetSystem | 1절 A0-G(:15), A1(:16), A2(:17) | OpenGL swizzle·샘플러 포함, ErrorCode 추가, `ProjectLayout`은 이름만 | G1–G3(5.2절), A1 ErrorCode를 셰이더에도 쓴다, A2 `ProjectLayout`에 셰이더 디렉터리·`ShaderWorkers` 작업 디렉터리·프로젝트 없는 엔진 위치 규칙(5.2절) |
| 〃 | 2절 사전 조사 "VFS(APK) → 후속" | APK 안 데이터 읽기 후속 | P13(SH6)의 선행 작업으로 단계를 정한다 |
| 〃 | 4.6절 진단 표(:115) | `IncompatibleCook`은 부분 쿠킹 병합 불가·런타임 프로파일 불일치 | 필요한 셰이더 형식·셰이더가 쿠킹 결과에 없음을 더한다(2절, 12절) |
| 〃 | 9절(:267–292) | 에셋 중심 키(TypeId·입력 bulk·의존 목록, SCC 식) | N1–N3(5.2절) |
| 〃 | 10절 TargetProfile(:296)·cookKey(:299)·매니페스트(:307)·결정성(:311) | 그래픽 backend 단일 항목, 기본 프로필 Windows·Android, 매니페스트 v1 | K1–K3(5.2절), 12절 `graphicsBackends`·`shaderFormats`·`MacOS` 프로필. 결정성 규칙은 그대로 셰이더에 적용한다(10절) |
| ReflectionFoundations | 3절 값 종류, 4절 struct | — | 셰이더 파라미터 종류가 `ROCKS_STRUCT` 경로를 쓰고 셰이더 자원 값 종류를 더한다(13절, 결정 4) |
| PlatformLayer | P1 표 "동적 모듈", 모바일 절 | 동적 모듈 구현(P10, "동적 라이브러리" 절), Android는 시험 실행만 | P13 Android 실행 기반, P12 macOS 창(PlatformWindow) |
| RepositoryLayout | 트리(:14·:29)·:52–56 | Engine에 Shaders·Config 없음, ThirdParty는 Core만 원본(:14 목록도 낡음) | 3절 구조와 8절 루트·`binary`·`headers` 종류 |
| CompilerDiagnostics | :9·:25·:29·:36 | 생성 파일도 경고 검사, 외부 원본도 같은 경고 프로필로 컴파일, Core 루트만, "이 절 밖의 경고 억제 예외는 없다" | 15절 결정 7의 답에 따라: 두 루트, Rocks가 컴파일하지 않는 미리 빌드된 바이너리(오프라인 도구 빌드 포함) 승인 절과 그 공개 헤더의 경고 0 조건, 셰이더 경고 절(작성자 원문과 SPIRV-Cross가 다시 쓴 중간 원문 모두 경고를 오류로) |
| Compression | 3.1·3.3·3.5 | Core 루트, `.c` 번역 단위만 | Engine 루트와 `kind: binary`·`headers` 언급(규칙 자체는 그대로) |
| CoreDataFoundations | 1.2절(:20) | ThirdParty `kind`는 `code`·`data` | `binary`·`headers`를 더한다(8절) |
| 정책 검사(코드) | `PolicyChecks.cs` `PythonFreeMainPath`(:313–349) | `Programs`·`Build`·`.github`의 Python 호출 금지 | 오프라인 도구 빌드 workflow를 명시 제외(8절 4항, P3 구현 때) |
| 코드(SH2 때) | `Engine/Source/Runtime/Graphics/Graphics.Build.cs:4–5`, `Programs/Sandbox/Sandbox.Build.cs:4·14`, `D3D12Graphics.Build.cs:21–22`, `DeviceTypes.h:14–15` | OpenGL·Vulkan·Metal 자리 설명, `d3dcompiler` 링크 | VulkanGraphics 의존 추가, OpenGL ES 설명, `d3dcompiler` 제거 |

**개정 31(SH2 설계)이 요구하는 다른 문서 개정.** 이 문서는 고치지 않았다 — 각 문서를 맡은 세션이 SH2 하위 단계 착수 전에 반영한다(16.1절 범위 충돌).

| 문서 | 고칠 곳 | 내용 |
|---|---|---|
| [에셋 시스템](../AssetSystem/10-Cooker.ko.md) | 10.2절 2·3·10·12항, 10.1절 | TargetProfile `graphicsBackend` → `graphicsBackends`·`shaderFormats`(12.3.1절 SD10), CookSettings `cookDevelopmentShaders`, `asset-cook` 셰이더 단계와 `--engine`(SD11), 매니페스트 v2 비에셋 산출물 표(SD13) |
| [에셋 시스템](../AssetSystem/11-RuntimeLoading.ko.md) | 11절 로더 시작 | 매니페스트 v2의 라이브러리 항목을 셰이더 집합에 넘김(SD14), 엔진 전용 쿠킹 루트 |
| [렌더 기반](../RenderBase.ko.md) | 9.2절, SX7·SX14, GX1·GX3, R8 | 엔진 라이브러리는 프로젝트 쿠킹 산출물(SD11), 공급자 interface는 SH2e(SD23), 메모리 라이브러리(SD16), `DeviceCaps` 장치 요구 비트(SD15), Graphics `CreateShader`·파이프라인 layout(SD18) |
| [엔진 셰이더](../EngineShaders/09-LimitsAndFollowups.ko.md) | SLR3·SLR12 | SLR3은 16.1절 SD21(선택 하위 단계 SH2g, 완료 기준 6), SLR12는 7.2절 12항(SD22)으로 반영됨 |
| [엔진 구현 계획](../EngineImplementationPlan.ko.md) | 9.6절 | 엔진 내장 셰이더 라이브러리를 만드는 단계(설치물 → 쿠킹, SD11) |
| [빌드 타깃](../BuildTargets/README.ko.md) | Commandlet 타깃 | 쿠커가 셰이더 타입 Runtime 모듈을 링크(SD3) |
