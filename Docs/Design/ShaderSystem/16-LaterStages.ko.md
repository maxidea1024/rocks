# 셰이더 시스템 설계 — 16. 이후 단계(선행 작업 뒤)

[목차](README.ko.md)

## 16. 이후 단계(선행 작업 뒤)

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| SH0 | 도구 스파이크: 지금의 내장 샘플 셰이더(`Programs/Sandbox/Private/SampleRenderer.cpp`의 HLSL)를 DXIL·SPIR-V·DXBC·MSL·ESSL로 만들고 검증 도구를 돌린다. Windows·macOS 양쪽에서 실행 | P3·P4·P10 | 형식별 산출, `spirv-val` 통과, DXIL 서명 hash 검사(macOS DXC 포함, S1), FXC 고정본 로드, 리플렉션 덤프, `ROCKS_IMAGE_FORMAT`의 DXC 지원 확인, S2·S3·S6·S8·S9·S10·S12(데스크톱)·S13·S14 측정. 남은 것(2026-11-20): macOS 부분(S1·S3 호스트 사이·S9)과 기계 하나로 못 재는 S2·S3·S12 일부(5.4절 끝) |
| SH1 | 소스 트리·가상 경로·`Platform.rsh`·전처리기(결정 6)·ShaderCompileWorker(P1, 11절)·두 층 키(P5)·에셋 너겟·`shader-compile` 명령·Development 필요 시 컴파일. 설계·하위 단계 SH1a–SH1e: 11.1절(개정 23) | SH0·P1·P5(A2·A3·P6a)·결정 6 | 같은 입력 같은 바이트, 빈/찬 캐시 동일, 워커 강제 종료 보고, 부모 종료 시 워커와 그 자식 종료, 형식 버전 불일치 진단, 도구 조회, 전처리 정합성(측정 S7) |
| SH2 | 리플렉션 정규화·워커 규칙 검사(7.2절)·Graphics API(`ShaderCodeKind`·`CreateShader`·파이프라인 layout·root signature·래스터(컬링·앞면)와 깊이 상태)·셰이더 타입과 퍼뮤테이션(P14)·쿠킹된 라이브러리 로드(P6b)·VulkanGraphics 최소 구현(offscreen·내장 셰이더, P9)·OpenGLGraphics GLES 미리보기 전환(P11)·샘플 렌더러 원문·런타임 `D3DCompile`·GLSL 원문 제거(결정 9) | SH1·P2·P4·P6b·P9·P11·P14·A0-G·결정 2·3·4·5 | Sandbox self-test에서 D3D12·Vulkan·GLES 미리보기가 허용 오차 안 일치(Vulkan·GL은 CI에서 NOT RUN일 수 있다), A0-G 범위를 Vulkan이 받음(G3), 행렬 옵션·앞면 매핑 기록(S5), Release에 컴파일러 없음(P4), 7.3절 SH2 시험 |
| SH3 | 파라미터 구조체(P7)·생성 HLSL·오프셋 시험 | SH2·P7 | 7.3절 (a)·(f)를 생성 구조체로 |
| SH4 | 감시 재컴파일(`DirectoryWatch`)·오류 위치·디버그 심볼·PSO 교체 | SH2 | 저장 후 10초 안 반영(계획 I5) |
| SH5 | D3D11Graphics → MetalGraphics | SH2·P12·P15 | 각 backend가 D3D12와 허용 오차 안 일치, 7.3절 (c)·(e)·(g)의 D3D11·Metal 부분 |
| SH6 | Android: `GLES31_ANDROID` 기기 실행, `VULKAN_ANDROID` 형식과 VulkanGraphics Android | SH2·P13·P11 | Android(실기기 또는 S11 에뮬레이터)에서 D3D12 기준 이미지와 허용 오차 안 일치, 7.3절 (c)·(i)의 Android 부분, S12(기기) |

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
- [ ] macOS: S1(macOS DXC 서명), S3 Windows–macOS 사이 byte 비교, S9, macOS CRT의 실수 출력. 스파이크 macOS 구현(`Private/Mac`, DXC는 `WinAdapter.h`)은 macOS 산출물(P3)과 함께 한다.
- [ ] S3의 FMA3 유무·OS 버전 차이와 S12의 다른 GPU 공급자는 기계가 하나라 미측정이다.
