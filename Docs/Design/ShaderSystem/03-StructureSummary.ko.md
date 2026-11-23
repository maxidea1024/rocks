# 셰이더 시스템 설계 — 3. 구조 요약

[목차](README.ko.md)

## 3. 구조 요약

```text
Engine/Shaders/{Public,Private,Shared}             .rsf(엔트리)·.rsh(include)·.h(C++·HLSL 공용)
Engine/Config/ShaderFormats/<id>.json              형식 정의(12절)
Engine/Config/TargetProfiles/<id>.json             graphicsBackends·shaderFormats(12절, 파일은 AssetSystem 10절)
Engine/Source/ThirdParty/<Lib>/                    DXC·SPIRV-Cross·SPIRV-Tools·FXC(binary manifest·오프라인 빌드 스크립트), SPIRV-Reflect(C 원본), Vulkan-Headers(headers)(8절)
Engine/Source/Runtime/ShaderCore                   셰이더 타입·퍼뮤테이션 ID·파라미터 메타데이터·셰이더 전용 정렬 타입·라이브러리 로드
Engine/Source/Developer/ShaderCompilerCommon       형식 정의 읽기·워커 메시지 형식·형식 컴파일 인터페이스(두 프로세스 공용, 11.1절)
Engine/Source/Developer/ShaderCompiler             소스 트리·전처리기·job 키·에셋 너겟·컴파일 관리자·워커 풀(메인 프로세스)
Engine/Source/Developer/ShaderWorker               워커 메시지 처리·도구 조회·형식 분배(워커 프로세스 본체, 시험 probe도 부른다)
Engine/Source/Developer/ShaderToolTable            ThirdParty manifest에서 생성한 도구 표(이름·SHA-256, 메타데이터만, 9절)
Engine/Source/Developer/ShaderFormat{D3D,Vulkan,Metal,OpenGL}   워커 안의 형식별 컴파일·재배치·리플렉션·GLES 재작성
Programs/ShaderCompileWorker                       워커 프로세스(ShaderWorker를 부르는 진입점)
```

ShaderCore만 Runtime 계층에 두고(런타임이 쿠킹된 라이브러리를 연다), 컴파일러 쪽은 Developer·Programs에 둔다.

가상 경로는 `/Engine/{Public,Private,Shared,Generated}`·`/Plugin/<이름>`·`/Project`이고, 규칙은 UE `CheckVirtualShaderFilePath`(`Runtime/RenderCore/Private/ShaderCore.cpp:1620–1670`)를 따르되 확장자는 `.rsf`·`.rsh`, 공유 디렉터리만 `.h`다(UE의 `/ThirdParty/` 예외는 두지 않는다). 키는 두 층이다: 셰이더 맵 키(엔트리 파일과 include 폐포의 내용 SHA-256, 낡음 판정)와 job 키(10절, 너겟 키). UE는 원본 파일 해시에 SHA-1(`ShaderCore.cpp:3282–3409`), 셰이더 맵 ID와 코드 해시에 XxHash64(`ShaderCore.cpp:1536–1551`, `Runtime/Engine/Private/Materials/MaterialShader.cpp:1341–1344`), job 키에 BLAKE3(`ShaderCore.cpp:4612–4671`)를 쓴다. Rocks는 모두 Core SHA-256이다.
